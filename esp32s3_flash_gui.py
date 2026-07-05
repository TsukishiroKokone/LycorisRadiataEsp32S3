#!/usr/bin/env python3
"""
ESP32-S3 多端口粉白 GUI 烧录器

跨平台：Linux / macOS / Windows
依赖：源码运行时使用 Python 标准库 Tkinter + esptool；打包后 esptool 内置在可执行文件内。

给小白的用法：
  1. 双击 launch_flasher_gui.command（macOS/Linux 可执行版也可直接双击）或运行 python3 esp32s3_flash_gui.py
  2. 选择 .bin 固件
  3. 点“刷新端口”
  4. 勾选要烧录的 ESP32-S3
  5. 点“一键烧录”
"""
from __future__ import annotations

import argparse
import concurrent.futures as futures
import glob
import hashlib
import io
import os
from pathlib import Path
import platform
import queue
import runpy
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from typing import Iterable

APP_TITLE = "LycorisRadiata"
APP_DIR = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent
ROOT = APP_DIR

def find_default_firmware() -> Path:
    for base in (APP_DIR, APP_DIR.parent, Path.cwd()):
        p = base / "firmware" / "ESP_Claw_http_V3.0_merged.bin"
        if p.exists():
            return p
    return APP_DIR / "firmware" / "ESP_Claw_http_V3.0_merged.bin"

DEFAULT_FIRMWARE = find_default_firmware()
DEFAULT_VENV = APP_DIR / ".esp32s3-flash-venv"

PINK = "#ff8fc7"
PINK_DARK = "#e85aa6"
PINK_LIGHT = "#fff0f8"
PINK_SOFT = "#ffd6ea"
WHITE = "#ffffff"
TEXT = "#5c3b4f"
MUTED = "#9b718a"
GREEN = "#54c68a"
RED = "#ff6b8a"
YELLOW = "#ffd166"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def natural_key(s: str) -> tuple:
    import re

    return tuple(int(x) if x.isdigit() else x.lower() for x in re.split(r"(\d+)", s))


def dedupe_ports(paths: Iterable[str]) -> list[str]:
    best_by_real: dict[str, str] = {}
    for p in paths:
        if not p:
            continue
        real = os.path.realpath(p)
        old = best_by_real.get(real)
        if old is None:
            best_by_real[real] = p
            continue
        old_score = (0 if "/dev/serial/by-id/" in old else 1, len(old))
        new_score = (0 if "/dev/serial/by-id/" in p else 1, len(p))
        if new_score < old_score:
            best_by_real[real] = p
    return sorted(best_by_real.values(), key=natural_key)


def discover_ports() -> list[str]:
    system = platform.system().lower()
    candidates: list[str] = []
    if system == "windows":
        # 不依赖 pyserial 的最低限度 COM 扫描。
        for i in range(1, 257):
            candidates.append(f"COM{i}")
        return candidates
    if system == "darwin":
        patterns = (
            "/dev/cu.usbmodem*",
            "/dev/cu.usbserial*",
            "/dev/cu.wchusbserial*",
            "/dev/tty.usbmodem*",
            "/dev/tty.usbserial*",
            "/dev/tty.wchusbserial*",
        )
    else:
        patterns = ("/dev/serial/by-id/*", "/dev/ttyACM*", "/dev/ttyUSB*")
    for pattern in patterns:
        candidates.extend(glob.glob(pattern))
    return dedupe_ports([p for p in candidates if os.path.exists(p)])


def venv_python(venv: Path) -> Path:
    if platform.system().lower() == "windows":
        return venv / "Scripts" / "python.exe"
    return venv / "bin" / "python"


def frozen_bundle() -> bool:
    return bool(getattr(sys, "frozen", False))


def run_embedded_esptool(args: list[str]) -> tuple[int, str]:
    """在当前进程里运行内置 esptool，避免冻结版再依赖外部 Python/venv。"""
    old_argv = sys.argv[:]
    old_stdout, old_stderr = sys.stdout, sys.stderr
    buf = io.StringIO()
    try:
        sys.argv = ["esptool", *args]
        sys.stdout = buf
        sys.stderr = buf
        try:
            runpy.run_module("esptool", run_name="__main__")
            rc = 0
        except SystemExit as exc:
            code = exc.code
            rc = int(code) if isinstance(code, int) else (0 if code is None else 1)
        return rc, buf.getvalue()
    except Exception as exc:
        return 99, buf.getvalue() + f"\n内置 esptool 运行异常：{exc!r}\n"
    finally:
        sys.argv = old_argv
        sys.stdout = old_stdout
        sys.stderr = old_stderr


def run_quiet(cmd: list[str], timeout: int = 20) -> int:
    try:
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
        return r.returncode
    except Exception:
        return 999


def have_esptool(py: str | Path) -> bool:
    return run_quiet([str(py), "-m", "esptool", "version"]) == 0


def python_has_pip(py: str | Path) -> bool:
    return run_quiet([str(py), "-m", "pip", "--version"]) == 0


def candidate_pythons() -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    names = [
        sys.executable,
        "python3.11",
        "python3.12",
        "python3.10",
        "python3",
        "python",
    ]
    if platform.system().lower() == "windows":
        names.extend(["py", "python.exe"])
    # 兼容命令行版已经装好的 venv。
    local = [ROOT / ".flash-venv" / ("Scripts/python.exe" if platform.system().lower() == "windows" else "bin/python3")]
    for path in local:
        if path.exists():
            names.insert(0, str(path))
    for name in names:
        path = shutil.which(str(name)) if not os.path.isabs(str(name)) else str(name)
        if path and path not in seen:
            seen.add(path)
            out.append(path)
    return out


def create_venv_with_pip(base_py: str, venv: Path, log) -> Path | None:
    try:
        py = venv_python(venv)
        if venv.exists() and (not py.exists() or not python_has_pip(py)):
            log(f"旧虚拟环境不完整，正在重建：{venv}")
            shutil.rmtree(venv)
        if not py.exists():
            log(f"创建烧录环境：{venv}")
            subprocess.check_call([base_py, "-m", "venv", "--clear", str(venv)])
        py = venv_python(venv)
        if not python_has_pip(py):
            subprocess.run([str(py), "-m", "ensurepip", "--upgrade"], check=False)
        if python_has_pip(py):
            return py
    except Exception as exc:
        log(f"使用 {base_py} 创建环境失败：{exc}")
    return None


def ensure_esptool(log) -> str:
    if frozen_bundle():
        try:
            r = subprocess.run(
                [sys.executable, "--embedded-esptool", "version"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=30,
            )
        except Exception as exc:
            raise RuntimeError(f"内置 esptool 启动失败：{exc}") from exc
        if r.returncode == 0:
            first = r.stdout.strip().splitlines()[0] if r.stdout.strip() else "ok"
            log(f"使用内置 esptool：{first}")
            return "__embedded__"
        raise RuntimeError("内置 esptool 不可用：" + r.stdout[-1000:])

    for py in candidate_pythons():
        if have_esptool(py):
            log(f"找到 esptool：{py}")
            return str(py)

    py = venv_python(DEFAULT_VENV)
    if have_esptool(py):
        log(f"找到 esptool：{py}")
        return str(py)

    log("没有找到 esptool，准备自动安装到本地环境……")
    last_error = None
    for base_py in candidate_pythons():
        vpy = create_venv_with_pip(base_py, DEFAULT_VENV, log)
        if not vpy:
            continue
        try:
            log("安装 esptool / pyserial 中，请稍等……")
            subprocess.check_call([str(vpy), "-m", "pip", "install", "-U", "pip", "esptool", "pyserial"])
            if have_esptool(vpy):
                log(f"esptool 安装完成：{vpy}")
                return str(vpy)
        except Exception as exc:
            last_error = exc
            log(f"安装失败：{exc}")

    raise RuntimeError(f"esptool 自动安装失败：{last_error}")


def flash_one(py: str, port: str, firmware: Path, baud: int, address: str, erase_first: bool) -> tuple[str, int, float, str]:
    esptool_args = [
        "--chip",
        "esp32s3",
        "-p",
        port,
        "-b",
        str(baud),
        "--before",
        "default-reset",
        "--after",
        "hard-reset",
        "write-flash",
        address,
        str(firmware),
    ]
    if erase_first:
        esptool_args.append("--erase-all")
    started = time.time()
    try:
        if py == "__embedded__":
            cmd = [sys.executable, "--embedded-esptool", *esptool_args]
            r = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            return port, r.returncode, time.time() - started, r.stdout
        cmd = [py, "-m", "esptool", *esptool_args]
        r = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return port, r.returncode, time.time() - started, r.stdout
    except Exception as exc:
        return port, 99, time.time() - started, f"启动烧录失败：{exc!r}"


class CuteButton(tk.Button):
    def __init__(self, master, **kw):
        opts = dict(
            bg=PINK,
            fg=WHITE,
            activebackground=PINK_DARK,
            activeforeground=WHITE,
            relief="flat",
            bd=0,
            padx=16,
            pady=9,
            cursor="hand2",
            font=("Arial", 11, "bold"),
        )
        opts.update(kw)
        super().__init__(master, **opts)


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("940x680")
        self.minsize(860, 600)
        self.configure(bg=PINK_LIGHT)

        self.firmware_var = tk.StringVar(value="")
        self.baud_var = tk.StringVar(value="460800")
        self.address_var = tk.StringVar(value="0x0")
        self.erase_var = tk.BooleanVar(value=False)
        self.select_all_var = tk.BooleanVar(value=True)
        self.status_var = tk.StringVar(value="准备好了喵～请选择固件并刷新端口")
        self.port_vars: dict[str, tk.BooleanVar] = {}
        self.ports: list[str] = []
        self.log_q: queue.Queue[tuple[str, str]] = queue.Queue()
        self.busy = False

        self._build_style()
        self._build_ui()
        self.after(100, self._drain_log)
        self.refresh_ports()
        self._update_firmware_info()

    def _build_style(self) -> None:
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except Exception:
            pass
        style.configure("Cute.TFrame", background=PINK_LIGHT)
        style.configure("Card.TFrame", background=WHITE, relief="flat")
        style.configure("Cute.TLabel", background=PINK_LIGHT, foreground=TEXT, font=("Arial", 11))
        style.configure("Card.TLabel", background=WHITE, foreground=TEXT, font=("Arial", 11))
        style.configure("Muted.TLabel", background=WHITE, foreground=MUTED, font=("Arial", 9))
        style.configure("Cute.TCheckbutton", background=WHITE, foreground=TEXT, font=("Arial", 10))
        style.map("Cute.TCheckbutton", background=[("active", WHITE)])
        style.configure("Horizontal.TProgressbar", troughcolor=PINK_SOFT, background=PINK_DARK)

    def _build_ui(self) -> None:
        outer = tk.Frame(self, bg=PINK_LIGHT)
        outer.pack(fill="both", expand=True, padx=18, pady=16)

        header = tk.Frame(outer, bg=PINK_LIGHT)
        header.pack(fill="x", pady=(0, 12))
        tk.Label(
            header,
            text="LycorisRadiata",
            bg=PINK_LIGHT,
            fg=TEXT,
            font=("Arial", 22, "bold"),
        ).pack(anchor="w")
        tk.Label(
            header,
            text="Version 1.0.0 -- Develop by:TsukishiroKokone",
            bg=PINK_LIGHT,
            fg=MUTED,
            font=("Arial", 11),
        ).pack(anchor="w", pady=(4, 0))

        main = tk.Frame(outer, bg=PINK_LIGHT)
        main.pack(fill="both", expand=True)
        main.columnconfigure(0, weight=1)
        main.columnconfigure(1, weight=1)
        main.rowconfigure(0, weight=1)

        left = self._card(main)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        right = self._card(main)
        right.grid(row=0, column=1, sticky="nsew", padx=(8, 0))

        self._build_left(left)
        self._build_right(right)

        bottom = tk.Frame(outer, bg=PINK_LIGHT)
        bottom.pack(fill="x", pady=(12, 0))
        tk.Label(bottom, textvariable=self.status_var, bg=PINK_LIGHT, fg=TEXT, font=("Arial", 10, "bold")).pack(side="left")
        self.progress = ttk.Progressbar(bottom, mode="indeterminate", style="Horizontal.TProgressbar", length=220)
        self.progress.pack(side="right")

    def _card(self, master) -> tk.Frame:
        card = tk.Frame(master, bg=WHITE, highlightthickness=1, highlightbackground=PINK_SOFT)
        card.pack_propagate(False)
        return card

    def _build_left(self, parent: tk.Frame) -> None:
        wrap = tk.Frame(parent, bg=WHITE)
        wrap.pack(fill="both", expand=True, padx=16, pady=16)

        tk.Label(wrap, text="① 选择固件", bg=WHITE, fg=TEXT, font=("Arial", 15, "bold")).pack(anchor="w")
        row = tk.Frame(wrap, bg=WHITE)
        row.pack(fill="x", pady=(10, 6))
        self.firmware_entry = tk.Entry(row, textvariable=self.firmware_var, bg="#fffafd", fg=TEXT, relief="flat", font=("Arial", 10))
        self.firmware_entry.pack(side="left", fill="x", expand=True, ipady=8)
        CuteButton(row, text="浏览…", command=self.choose_firmware).pack(side="left", padx=(8, 0))

        self.firmware_info = tk.Label(wrap, text="", bg=WHITE, fg=MUTED, justify="left", anchor="w", font=("Arial", 9))
        self.firmware_info.pack(fill="x", pady=(0, 16))

        tk.Label(wrap, text="② 选择端口", bg=WHITE, fg=TEXT, font=("Arial", 15, "bold")).pack(anchor="w")
        port_bar = tk.Frame(wrap, bg=WHITE)
        port_bar.pack(fill="x", pady=(10, 4))
        CuteButton(port_bar, text="刷新端口 ✨", command=self.refresh_ports).pack(side="left")
        ttk.Checkbutton(
            port_bar,
            text="全选",
            variable=self.select_all_var,
            command=self.toggle_all_ports,
            style="Cute.TCheckbutton",
        ).pack(side="left", padx=12)

        self.port_canvas = tk.Canvas(wrap, bg=WHITE, bd=0, highlightthickness=0)
        scrollbar = ttk.Scrollbar(wrap, orient="vertical", command=self.port_canvas.yview)
        self.port_frame = tk.Frame(self.port_canvas, bg=WHITE)
        self.port_frame.bind("<Configure>", lambda e: self.port_canvas.configure(scrollregion=self.port_canvas.bbox("all")))
        self.port_canvas.create_window((0, 0), window=self.port_frame, anchor="nw")
        self.port_canvas.configure(yscrollcommand=scrollbar.set)
        self.port_canvas.pack(side="left", fill="both", expand=True, pady=(6, 0))
        scrollbar.pack(side="right", fill="y", pady=(6, 0))

    def _build_right(self, parent: tk.Frame) -> None:
        wrap = tk.Frame(parent, bg=WHITE)
        wrap.pack(fill="both", expand=True, padx=16, pady=16)

        tk.Label(wrap, text="③ 烧录设置", bg=WHITE, fg=TEXT, font=("Arial", 15, "bold")).pack(anchor="w")
        form = tk.Frame(wrap, bg=WHITE)
        form.pack(fill="x", pady=(10, 14))
        tk.Label(form, text="波特率", bg=WHITE, fg=TEXT).grid(row=0, column=0, sticky="w", pady=5)
        ttk.Combobox(form, textvariable=self.baud_var, values=["115200", "230400", "460800", "921600"], width=12).grid(row=0, column=1, sticky="w", padx=8)
        tk.Label(form, text="写入地址", bg=WHITE, fg=TEXT).grid(row=1, column=0, sticky="w", pady=5)
        tk.Entry(form, textvariable=self.address_var, bg="#fffafd", fg=TEXT, relief="flat", width=14).grid(row=1, column=1, sticky="w", padx=8, ipady=5)
        ttk.Checkbutton(form, text="写入前整片擦除（更慢，但更干净）", variable=self.erase_var, style="Cute.TCheckbutton").grid(row=2, column=0, columnspan=2, sticky="w", pady=5)

        btn_row = tk.Frame(wrap, bg=WHITE)
        btn_row.pack(fill="x", pady=(0, 12))
        self.flash_btn = CuteButton(btn_row, text="💖 一键烧录选中设备", command=self.start_flash)
        self.flash_btn.pack(side="left", fill="x", expand=True)
        CuteButton(btn_row, text="清空日志", command=self.clear_log, bg=PINK_SOFT, fg=TEXT, activebackground=PINK).pack(side="left", padx=(8, 0))

        tk.Label(wrap, text="④ 运行日志", bg=WHITE, fg=TEXT, font=("Arial", 15, "bold")).pack(anchor="w")
        self.log_text = tk.Text(
            wrap,
            height=18,
            bg="#fffafd",
            fg=TEXT,
            insertbackground=TEXT,
            relief="flat",
            wrap="word",
            font=("Menlo", 10) if platform.system().lower() == "darwin" else ("Consolas", 10),
        )
        self.log_text.pack(fill="both", expand=True, pady=(10, 0))
        self.log_text.tag_configure("ok", foreground=GREEN)
        self.log_text.tag_configure("err", foreground=RED)
        self.log_text.tag_configure("info", foreground=TEXT)
        self.log_text.tag_configure("cute", foreground=PINK_DARK)

    def choose_firmware(self) -> None:
        path = filedialog.askopenfilename(
            title="选择 ESP32-S3 .bin 固件",
            initialdir=str(ROOT),
            filetypes=[("BIN firmware", "*.bin"), ("All files", "*.*")],
        )
        if path:
            self.firmware_var.set(path)
            self._update_firmware_info()

    def _update_firmware_info(self) -> None:
        p = Path(self.firmware_var.get()).expanduser()
        if p.exists() and p.is_file():
            try:
                self.firmware_info.config(text=f"大小：{p.stat().st_size:,} bytes\nSHA256：{sha256_file(p)[:16]}…{sha256_file(p)[-16:]}")
            except Exception as exc:
                self.firmware_info.config(text=f"读取固件信息失败：{exc}")
        else:
            self.firmware_info.config(text="还没有选择有效固件喵～")

    def refresh_ports(self) -> None:
        if self.busy:
            return
        self.ports = discover_ports()
        for child in self.port_frame.winfo_children():
            child.destroy()
        self.port_vars.clear()
        if not self.ports:
            tk.Label(
                self.port_frame,
                text="没有发现端口。请插入 ESP32-S3，必要时按住 BOOT 再点 RESET。",
                bg=WHITE,
                fg=MUTED,
                justify="left",
                wraplength=360,
            ).pack(anchor="w", pady=10)
            self.status_var.set("未发现设备喵～")
            return
        for p in self.ports:
            var = tk.BooleanVar(value=True)
            self.port_vars[p] = var
            real = os.path.realpath(p)
            label = p if real == p else f"{p}\n  → {real}"
            cb = ttk.Checkbutton(self.port_frame, text=label, variable=var, style="Cute.TCheckbutton")
            cb.pack(anchor="w", fill="x", pady=5)
        self.status_var.set(f"发现 {len(self.ports)} 个端口，默认已全选 ✨")
        self.log(f"刷新端口：发现 {len(self.ports)} 个设备", "cute")

    def toggle_all_ports(self) -> None:
        value = self.select_all_var.get()
        for var in self.port_vars.values():
            var.set(value)

    def selected_ports(self) -> list[str]:
        return [p for p, var in self.port_vars.items() if var.get()]

    def validate_inputs(self) -> tuple[Path, int, str, list[str]] | None:
        firmware = Path(self.firmware_var.get()).expanduser()
        if not firmware.exists() or not firmware.is_file():
            messagebox.showerror("固件不对喵", "请先选择一个存在的 .bin 固件文件。")
            return None
        try:
            baud = int(self.baud_var.get())
            if baud <= 0:
                raise ValueError
        except ValueError:
            messagebox.showerror("波特率不对喵", "波特率需要是正整数，比如 460800。")
            return None
        address = self.address_var.get().strip() or "0x0"
        ports = self.selected_ports()
        if not ports:
            messagebox.showerror("没有选设备喵", "请至少勾选一个端口。")
            return None
        return firmware.resolve(), baud, address, ports

    def start_flash(self) -> None:
        if self.busy:
            return
        checked = self.validate_inputs()
        if not checked:
            return
        firmware, baud, address, ports = checked
        self.busy = True
        self.flash_btn.config(state="disabled", text="正在烧录中…")
        self.progress.start(10)
        self.status_var.set(f"正在烧录 {len(ports)} 个设备，请不要拔线～")
        self.log("\n🌸 开始新的烧录任务", "cute")
        self.log(f"固件：{firmware}")
        self.log(f"大小：{firmware.stat().st_size:,} bytes")
        self.log(f"SHA256：{sha256_file(firmware)}")
        self.log(f"端口：{', '.join(ports)}")
        t = threading.Thread(target=self._flash_worker, args=(firmware, baud, address, ports), daemon=True)
        t.start()

    def _flash_worker(self, firmware: Path, baud: int, address: str, ports: list[str]) -> None:
        try:
            py = ensure_esptool(lambda s: self.log_threadsafe(s))
            self.log_threadsafe(f"使用 Python/esptool：{py}")
            ok = 0
            failed: list[str] = []
            with futures.ThreadPoolExecutor(max_workers=len(ports)) as ex:
                futs = [ex.submit(flash_one, py, port, firmware, baud, address, self.erase_var.get()) for port in ports]
                for fut in futures.as_completed(futs):
                    port, rc, elapsed, out = fut.result()
                    if rc == 0:
                        ok += 1
                        tag = "ok"
                        title = f"✅ {port} 烧录成功，用时 {elapsed:.1f}s"
                    else:
                        failed.append(port)
                        tag = "err"
                        title = f"❌ {port} 烧录失败，退出码 {rc}，用时 {elapsed:.1f}s"
                    self.log_threadsafe("\n" + title, tag)
                    self.log_threadsafe(out.rstrip()[-6000:] if out else "", tag)
            if failed:
                self.finish_threadsafe(False, f"完成：成功 {ok}/{len(ports)}，有 {len(failed)} 个失败")
            else:
                self.finish_threadsafe(True, f"全部完成：成功 {ok}/{len(ports)} 🎉")
        except Exception as exc:
            self.log_threadsafe(f"\n❌ 任务失败：{exc}", "err")
            self.finish_threadsafe(False, "任务失败，请看日志")

    def log(self, msg: str, tag: str = "info") -> None:
        self.log_text.insert("end", msg + "\n", tag)
        self.log_text.see("end")

    def log_threadsafe(self, msg: str, tag: str = "info") -> None:
        self.log_q.put((tag, msg))

    def finish_threadsafe(self, ok: bool, status: str) -> None:
        self.log_q.put(("__finish_ok__" if ok else "__finish_fail__", status))

    def _drain_log(self) -> None:
        try:
            while True:
                tag, msg = self.log_q.get_nowait()
                if tag in {"__finish_ok__", "__finish_fail__"}:
                    self.busy = False
                    self.progress.stop()
                    self.flash_btn.config(state="normal", text="💖 一键烧录选中设备")
                    self.status_var.set(msg)
                    self.log(msg, "ok" if tag == "__finish_ok__" else "err")
                    if tag == "__finish_ok__":
                        messagebox.showinfo("烧录完成喵", msg)
                    else:
                        messagebox.showwarning("烧录结束但有问题", msg)
                else:
                    self.log(msg, tag)
        except queue.Empty:
            pass
        self.after(100, self._drain_log)

    def clear_log(self) -> None:
        self.log_text.delete("1.0", "end")


def main() -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--embedded-esptool", nargs=argparse.REMAINDER)
    parser.add_argument("--self-test", action="store_true")
    known, _ = parser.parse_known_args()
    if known.embedded_esptool is not None:
        rc, out = run_embedded_esptool(known.embedded_esptool)
        if out:
            print(out, end="")
        return rc
    if known.self_test:
        print(f"app={APP_TITLE}")
        print(f"frozen={frozen_bundle()}")
        print(f"default_firmware_exists={DEFAULT_FIRMWARE.exists()}")
        rc, out = run_embedded_esptool(["version"])
        print(f"embedded_esptool_rc={rc}")
        print(out.strip())
        return rc

    try:
        app = App()
        app.mainloop()
        return 0
    except tk.TclError as exc:
        print(f"无法启动图形界面：{exc}", file=sys.stderr)
        if "DISPLAY" in str(exc) or "display" in str(exc).lower():
            print("当前终端没有图形显示环境。请在桌面会话里双击启动脚本，或在有 DISPLAY 的终端运行。", file=sys.stderr)
        else:
            print("请确认系统安装了 Tkinter。Ubuntu 可安装：sudo apt install python3-tk", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
