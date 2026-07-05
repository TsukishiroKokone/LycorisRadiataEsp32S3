#!/usr/bin/env bash
# 构建 Linux/macOS 单文件可执行版：LycorisRadiata
# Linux 输出：dist/LycorisRadiata-Flasher
# macOS 输出：dist/LycorisRadiata-Flasher.app 和 dist/LycorisRadiata-Flasher-macOS.zip
set -euo pipefail
cd "$(dirname "$0")"

APP_NAME="LycorisRadiata-Flasher"
PY=""

for c in .flash-venv/bin/python3 .esp32s3-flash-venv/bin/python python3.11 python3.12 python3.10 python3 python; do
  if [ -x "$c" ]; then
    PY="$c"
    break
  fi
  if command -v "$c" >/dev/null 2>&1; then
    PY="$(command -v "$c")"
    break
  fi
done

if [ -z "$PY" ]; then
  echo "没有找到 Python 3，无法构建。"
  exit 1
fi

echo "[build] Python: $PY"
"$PY" -m pip --version >/dev/null 2>&1 || {
  echo "[build] 当前 Python 没有 pip，尝试 ensurepip..."
  "$PY" -m ensurepip --upgrade
}

"$PY" -m pip install -U pip pyinstaller esptool pyserial

TCLTK_LIB_DIR=""
TCLTK_DATA_DIR=""
TCLTK_ROOT="$($PY - <<'PY'
import pathlib, sysconfig
lib = pathlib.Path(sysconfig.get_config_var('LIBDIR') or '').resolve()
print(lib if (lib/'libtcl9.0.so').exists() or (lib/'libtcl8.6.so').exists() else '')
PY
)"
if [ -n "$TCLTK_ROOT" ]; then
  TCLTK_LIB_DIR="$TCLTK_ROOT"
  for d in "$TCLTK_ROOT"/tcl* "$TCLTK_ROOT"/tk*; do
    [ -d "$d" ] && TCLTK_DATA_DIR="$TCLTK_DATA_DIR --add-data $d:$(basename "$d")"
  done
fi

rm -rf build "dist/$APP_NAME" "dist/$APP_NAME.app" "dist/$APP_NAME-macOS.zip" "$APP_NAME.spec"

COMMON_ARGS=(
  --name "$APP_NAME"
  --onefile
  --clean
  --noconfirm
  --collect-all esptool
  --collect-all serial
  --hidden-import esptool
  --hidden-import serial
  --hidden-import serial.tools.list_ports
  --hidden-import tkinter
)

if [ -n "$TCLTK_LIB_DIR" ]; then
  for lib in "$TCLTK_LIB_DIR"/libtcl*.so "$TCLTK_LIB_DIR"/libtk*.so "$TCLTK_LIB_DIR"/libtcl*tk*.so; do
    [ -f "$lib" ] && COMMON_ARGS+=(--add-binary "$lib:.")
  done
  # shellcheck disable=SC2206
  DATA_ARGS=($TCLTK_DATA_DIR)
  COMMON_ARGS+=("${DATA_ARGS[@]}")
fi

# GUI 模式：macOS/Windows 不弹控制台；Linux 下仍可从终端看到错误。
case "$(uname -s)" in
  Darwin*) COMMON_ARGS+=(--windowed --osx-bundle-identifier "moe.tsukishirokokone.lycorisradiata") ;;
esac

"$PY" -m PyInstaller "${COMMON_ARGS[@]}" esp32s3_flash_gui.py

if [ -d "dist/$APP_NAME.app" ]; then
  APP="dist/$APP_NAME.app"
  BIN="$APP/Contents/MacOS/$APP_NAME"
  chmod +x "$BIN" || true
  echo "[build] 自检：$BIN --self-test"
  "$BIN" --self-test
  (cd dist && zip -qry "$APP_NAME-macOS.zip" "$APP_NAME.app")
  echo "[build] 完成：$APP"
  echo "[build] 压缩包：dist/$APP_NAME-macOS.zip"
elif [ -f "dist/$APP_NAME" ]; then
  BIN="dist/$APP_NAME"
  chmod +x "$BIN"
  echo "[build] 自检：$BIN --self-test"
  "$BIN" --self-test
  echo "[build] 完成：$BIN"
else
  echo "[build] 未找到输出文件：dist/$APP_NAME 或 dist/$APP_NAME.app"
  exit 1
fi
