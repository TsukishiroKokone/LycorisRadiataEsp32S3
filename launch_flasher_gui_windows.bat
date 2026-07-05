@echo off
REM Windows 双击启动：粉爪爪 ESP32-S3 一键烧录器
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
  py esp32s3_flash_gui.py
  goto :eof
)
where python >nul 2>nul
if %errorlevel%==0 (
  python esp32s3_flash_gui.py
  goto :eof
)
echo 没有找到 Python，请先安装 Python 3。
pause
