#!/usr/bin/env python3
"""
esp32s3_multi_flash.py - 一键把同一个 .bin 固件并行烧录到多个 ESP32-S3 串口。

默认行为：
  1. 自动发现 /dev/serial/by-id/*、/dev/ttyACM*、/dev/ttyUSB* 端口
  2. 自动准备本目录下 .esp32s3-flash-venv 并安装 esptool（如果当前 Python 没有）
  3. 对所有发现端口并行执行：python -m esptool --chip esp32s3 ... write-flash 0x0 firmware.bin

示例：
  ./esp32s3_multi_flash.py firmware/ESP_Claw_http_V3.0_merged.bin
  ./esp32s3_multi_flash.py app.bin --ports /dev/ttyACM0,/dev/ttyUSB0
  ./esp32s3_multi_flash.py app.bin --list
  ./esp32s3_multi_flash.py app.bin --dry-run
"""
from __future__ import annotations

import argparse
import concurrent.futures as futures
import glob
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import textwrap
import time
from typing import Iterable

ROOT = Path(__file__).resolve().parent
DEFAULT_VENV = ROOT / ".esp32s3-flash-venv"


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def split_ports(raw: str | None) -> list[str]:
    if not raw:
        return []
    ports: list[str] = []
    for item in raw.replace(";", ",").split(","):
        item = item.strip()
        if item:
            ports.append(item)
    return ports


def dedupe_ports(paths: Iterable[str]) -> list[str]:
    """去重端口；优先保留 /dev/serial/by-id 下的稳定名字。"""
    best_by_real: dict[str, str] = {}
    for p in paths:
        if not p:
            continue
        real = os.path.realpath(p)
        old = best_by_real.get(real)
        if old is None:
            best_by_real[real] = p
            continue
        # 稳定 by-id 名优先；否则较短路径优先。
        old_score = (0 if "/dev/serial/by-id/" in old else 1, len(old))
        new_score = (0 if "/dev/serial/by-id/" in p else 1, len(p))
        if new_score < old_score:
            best_by_real[real] = p
    return sorted(best_by_real.values())


def discover_ports() -> list[str]:
    candidates: list[str] = []
    # by-id 放前面，避免 /dev/ttyACM0 这种热插拔变化名字优先。
    for pattern in ("/dev/serial/by-id/*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        candidates.extend(glob.glob(pattern))
    candidates = [p for p in candidates if os.path.exists(p)]
    return dedupe_ports(candidates)


def venv_python(venv: Path) -> Path:
    return venv / "bin" / "python"


def have_esptool(py: str | Path) -> bool:
    try:
        r = subprocess.run(
            [str(py), "-m", "esptool", "version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=20,
        )
        return r.returncode == 0
    except Exception:
        return False


def candidate_pythons() -> list[str]:
    """优先选择带 pip/venv 支持的 Python；避免某些系统 python3.14 venv 不带 pip。"""
    seen: set[str] = set()
    out: list[str] = []
    for name in (
        sys.executable,
        "python3.11",
        "python3.12",
        "python3.10",
        "python3",
        "python",
    ):
        path = shutil.which(str(name)) if not os.path.isabs(str(name)) else str(name)
        if path and path not in seen:
            seen.add(path)
            out.append(path)
    # 兼容我之前手动创建过的本地 venv。
    for path in (ROOT / ".flash-venv" / "bin" / "python3", ROOT / ".flash-venv" / "bin" / "python"):
        if path.exists():
            s = str(path)
            if s not in seen:
                seen.add(s)
                out.insert(0, s)
    return out


def python_has_pip(py: str | Path) -> bool:
    try:
        r = subprocess.run(
            [str(py), "-m", "pip", "--version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=20,
        )
        return r.returncode == 0
    except Exception:
        return False


def create_venv_with_pip(base_py: str, venv: Path) -> Path | None:
    """用指定 Python 创建 venv，并尽力让 venv 内有 pip。失败返回 None。"""
    try:
        py = venv_python(venv)
        # 如果上一次用不带 ensurepip 的 Python 创建出了“半残”venv，先清掉重建。
        if venv.exists() and (not py.exists() or not python_has_pip(py)):
            shutil.rmtree(venv)
        if not py.exists():
            subprocess.check_call([base_py, "-m", "venv", "--clear", str(venv)])
        py = venv_python(venv)
        if not python_has_pip(py):
            subprocess.run([str(py), "-m", "ensurepip", "--upgrade"], check=False)
        if python_has_pip(py):
            return py
    except Exception as exc:
        eprint(f"[setup] {base_py} 创建 venv 失败：{exc}")
    return None


def ensure_esptool(auto_install: bool, venv: Path) -> str:
    """返回可运行 esptool 的 Python 解释器路径。"""
    # 先找已经可用的 esptool，避免不必要安装。
    for py0 in candidate_pythons():
        if have_esptool(py0):
            return str(py0)

    py = venv_python(venv)
    if have_esptool(py):
        return str(py)

    if not auto_install:
        raise SystemExit(
            "当前 Python 没有 esptool。请先安装：python3 -m pip install esptool，"
            "或不要使用 --no-auto-install。"
        )

    eprint(f"[setup] 创建/更新烧录虚拟环境：{venv}")
    last_error = None
    for base_py in candidate_pythons():
        vpy = create_venv_with_pip(base_py, venv)
        if not vpy:
            continue
        try:
            subprocess.check_call([str(vpy), "-m", "pip", "install", "-U", "pip", "esptool", "pyserial"])
            if have_esptool(vpy):
                return str(vpy)
        except subprocess.CalledProcessError as exc:
            last_error = exc
            eprint(f"[setup] 使用 {base_py} 安装 esptool 失败：{exc}")

    # 如果系统有 uv，最后尝试用 uv pip 往 venv 安装。
    uv = shutil.which("uv")
    if uv:
        for base_py in candidate_pythons():
            vpy = create_venv_with_pip(base_py, venv) or venv_python(venv)
            try:
                subprocess.check_call([uv, "pip", "install", "--python", str(vpy), "esptool", "pyserial"])
                if have_esptool(vpy):
                    return str(vpy)
            except Exception as exc:
                last_error = exc
                eprint(f"[setup] uv 安装 esptool 失败：{exc}")

    raise SystemExit(f"esptool 自动安装失败，请检查 Python venv/pip 环境。最后错误：{last_error}")


def flash_one(
    py: str,
    port: str,
    firmware: Path,
    baud: int,
    address: str,
    before: str,
    after: str,
    extra: list[str],
    dry_run: bool,
) -> tuple[str, int, float, str]:
    cmd = [
        py,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "-p",
        port,
        "-b",
        str(baud),
        "--before",
        before,
        "--after",
        after,
        "write-flash",
        address,
        str(firmware),
        *extra,
    ]
    started = time.time()
    if dry_run:
        return port, 0, 0.0, "+ " + " ".join(shlex_quote(x) for x in cmd)

    try:
        r = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return port, r.returncode, time.time() - started, r.stdout
    except Exception as exc:
        return port, 99, time.time() - started, f"启动烧录失败：{exc!r}\n命令：{' '.join(cmd)}"


def shlex_quote(s: str) -> str:
    import shlex

    return shlex.quote(s)


def print_ports(ports: list[str]) -> None:
    if not ports:
        print("未发现串口设备。请插入 ESP32-S3，必要时按住 BOOT 再点 RESET 进入下载模式。")
        return
    print("发现端口：")
    for i, p in enumerate(ports, 1):
        real = os.path.realpath(p)
        suffix = f" -> {real}" if real != p else ""
        print(f"  {i}. {p}{suffix}")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="一键并行烧录同一个 bin 固件到多个 ESP32-S3。",
        epilog=textwrap.dedent(
            """
            常用：
              ./esp32s3_multi_flash.py firmware/ESP_Claw_http_V3.0_merged.bin
              ./esp32s3_multi_flash.py firmware.bin --ports /dev/ttyACM0,/dev/ttyUSB0
              ./esp32s3_multi_flash.py firmware.bin --list
              ./esp32s3_multi_flash.py firmware.bin --dry-run

            如果烧录失败提示连接不上：
              - 确认 USB 线支持数据传输
              - 按住 BOOT，点一下 RESET，再松开 BOOT，然后重试
              - Linux 权限需在 dialout 组；当前用户通常重新登录后生效
            """
        ),
    )
    p.add_argument("firmware", type=Path, help="要烧录的合并固件 .bin 文件")
    p.add_argument("--ports", help="逗号分隔端口列表；不填则自动发现所有 ttyACM/ttyUSB/by-id")
    p.add_argument("--list", action="store_true", help="只列出将使用的端口，不烧录")
    p.add_argument("--dry-run", action="store_true", help="只打印将执行的 esptool 命令，不烧录")
    p.add_argument("--baud", type=int, default=460800, help="烧录波特率，默认 460800")
    p.add_argument("--address", default="0x0", help="写入地址，合并固件通常是 0x0")
    p.add_argument("--jobs", type=int, default=0, help="并发数，默认=端口数量")
    p.add_argument("--before", default="default-reset", help="esptool --before，默认 default-reset")
    p.add_argument("--after", default="hard-reset", help="esptool --after，默认 hard-reset")
    p.add_argument("--erase-first", action="store_true", help="写入前先擦除 flash（更慢，但更干净）")
    p.add_argument("--no-auto-install", action="store_true", help="不要自动创建 venv/安装 esptool")
    p.add_argument("--venv", type=Path, default=DEFAULT_VENV, help=f"自动安装 esptool 的 venv 路径，默认 {DEFAULT_VENV}")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    firmware = args.firmware.expanduser().resolve()
    if not firmware.exists() or not firmware.is_file():
        eprint(f"固件不存在：{firmware}")
        return 2
    if firmware.suffix.lower() != ".bin":
        eprint(f"警告：文件后缀不是 .bin：{firmware}")

    ports = split_ports(args.ports) or discover_ports()
    ports = dedupe_ports(ports)

    print(f"固件：{firmware}")
    print(f"大小：{firmware.stat().st_size:,} bytes")
    print(f"SHA256：{sha256_file(firmware)}")
    print_ports(ports)

    if args.list:
        return 0
    if not ports:
        return 3

    if args.dry_run:
        py = sys.executable
        print(f"esptool Python：{py}（dry-run，不检查/安装 esptool）")
    else:
        py = ensure_esptool(auto_install=not args.no_auto_install, venv=args.venv.expanduser().resolve())
        print(f"esptool Python：{py}")

    extra: list[str] = []
    if args.erase_first:
        extra.append("--erase-all")

    jobs = args.jobs if args.jobs and args.jobs > 0 else len(ports)
    jobs = max(1, min(jobs, len(ports)))
    print(f"开始烧录：{len(ports)} 个端口，并发 {jobs}，地址 {args.address}，波特率 {args.baud}")

    ok = 0
    failed: list[tuple[str, int]] = []
    with futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [
            ex.submit(
                flash_one,
                py,
                port,
                firmware,
                args.baud,
                args.address,
                args.before,
                args.after,
                extra,
                args.dry_run,
            )
            for port in ports
        ]
        for fut in futures.as_completed(futs):
            port, rc, elapsed, out = fut.result()
            banner = f"===== {port} | {'OK' if rc == 0 else 'FAIL'} | {elapsed:.1f}s ====="
            print("\n" + banner)
            print(out.rstrip())
            print("=" * len(banner))
            if rc == 0:
                ok += 1
            else:
                failed.append((port, rc))

    print(f"\n完成：成功 {ok}/{len(ports)}")
    if failed:
        print("失败端口：")
        for port, rc in failed:
            print(f"  - {port}: exit {rc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
