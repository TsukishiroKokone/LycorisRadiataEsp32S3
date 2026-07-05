#!/usr/bin/env bash
# macOS 双击启动：粉爪爪 ESP32-S3 一键烧录器
set -euo pipefail
cd "$(dirname "$0")"

PYTHON_BIN=""
for candidate in python3.11 python3.12 python3.10 python3 python; do
  if command -v "$candidate" >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v "$candidate")"
    break
  fi
done

if [ -z "$PYTHON_BIN" ]; then
  osascript -e 'display dialog "没有找到 Python。请先安装 Python 3，然后再双击启动。" buttons {"好"} default button 1 with icon caution' || true
  exit 1
fi

"$PYTHON_BIN" esp32s3_flash_gui.py
