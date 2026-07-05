@echo off
REM 构建 Windows 单文件可执行版：LycorisRadiata
cd /d "%~dp0"
set APP_NAME=LycorisRadiata-Flasher

where py >nul 2>nul
if %errorlevel%==0 (
  set PY=py
) else (
  where python >nul 2>nul
  if %errorlevel%==0 (
    set PY=python
  ) else (
    echo 没有找到 Python 3，无法构建。
    pause
    exit /b 1
  )
)

%PY% -m pip install -U pip pyinstaller esptool pyserial
rmdir /s /q build 2>nul
rmdir /s /q dist\%APP_NAME% 2>nul
del /q %APP_NAME%.spec 2>nul

%PY% -m PyInstaller ^
  --name %APP_NAME% ^
  --onefile ^
  --windowed ^
  --clean ^
  --noconfirm ^
  --collect-all esptool ^
  --collect-all serial ^
  --hidden-import esptool ^
  --hidden-import serial ^
  --hidden-import serial.tools.list_ports ^
  --hidden-import tkinter ^
  esp32s3_flash_gui.py

dist\%APP_NAME%.exe --self-test
echo 完成：dist\%APP_NAME%.exe
if not defined CI pause
