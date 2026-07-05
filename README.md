# LycorisRadiata ESP32-S3 Flasher

白粉色、小白友好的 ESP32-S3 图形化固件烧录工具。支持选择 `.bin` 固件、自动发现并筛选疑似 ESP32-S3 的串口、勾选多个设备并行烧录，适合把同一个固件批量写入多个 ESP32-S3。

## 下载三平台可执行文件

本仓库使用 GitHub Actions 自动构建三平台产物：

| 平台 | Actions 产物 |
|---|---|
| Linux x64 | `LycorisRadiata-Flasher-linux-x64` |
| Windows x64 | `LycorisRadiata-Flasher-windows-x64.exe` |
| macOS Apple Silicon | `LycorisRadiata-Flasher-macos-arm64.zip` |
| macOS Intel | `LycorisRadiata-Flasher-macos-intel.zip` |

进入 GitHub 仓库的 **Actions** 页面，打开 `Build LycorisRadiata Flasher` 的最新运行记录，在 Artifacts 区域下载对应系统版本即可。

## 使用方法

1. 下载对应系统的构建产物。
2. 解压后运行 `LycorisRadiata-Flasher` / `LycorisRadiata-Flasher.exe` / `LycorisRadiata-Flasher.app`。
3. 点击“浏览...”选择要烧录的 `.bin` 固件。
4. 插入 ESP32-S3 开发板。
5. 点击“刷新端口”。
6. 勾选要烧录的设备。
7. 点击“一键烧录选中设备”。

> 程序启动时不会默认选择固件，避免误烧录；每次请手动选择需要写入的 `.bin`。

## 本仓库包含

- `esp32s3_flash_gui.py`：Tkinter GUI 主程序。
- `esp32s3_multi_flash.py`：命令行多端口批量烧录工具。
- `firmware/ESP_Claw_http_V3.0_merged.bin`：示例/目标固件。
- `build_standalone.sh`：Linux/macOS PyInstaller 构建脚本。
- `build_standalone_windows.bat`：Windows PyInstaller 构建脚本。
- `.github/workflows/build-flasher.yml`：GitHub Actions 三平台构建工作流。
- `GUI_FLASHER_README.md`：更详细的小白使用说明。

## Windows 端口自动识别

Windows 下程序不会默认显示全部 `COM1..COM256`。它会先读取串口 USB 描述、硬件 ID、VID/PID，只显示疑似 ESP32-S3 的设备，例如：

- Espressif 原生 USB/JTAG Serial，VID `303A`
- CP210x，VID `10C4`
- CH340/CH910x，VID `1A86`
- FTDI，VID `0403`

如果确实识别不到，但你确认设备已经插上，可以勾选界面里的“显示全部 COM”再手动选择。

## 本地构建

### Linux / macOS

```bash
chmod +x build_standalone.sh
./build_standalone.sh
```

Linux 输出：

```text
dist/LycorisRadiata-Flasher
```

macOS 输出：

```text
dist/LycorisRadiata-Flasher.app
dist/LycorisRadiata-Flasher-macOS.zip
```

### Windows

```bat
build_standalone_windows.bat
```

输出：

```text
dist\LycorisRadiata-Flasher.exe
```

## GitHub Actions 构建

工作流位于：

```text
.github/workflows/build-flasher.yml
```

触发方式：

- push 到 `main` 分支自动构建。
- push `v*` tag 自动构建。
- Actions 页面手动运行 `workflow_dispatch`。

## 注意事项

- Linux 用户可能需要串口权限：`sudo usermod -aG dialout $USER` 后注销重新登录。
- Windows 用户可能需要安装对应 USB 串口驱动，例如 CH340/CP210x。
- macOS 首次打开未签名 `.app` 时，可能需要右键 App → 打开，或执行：

```bash
xattr -dr com.apple.quarantine LycorisRadiata-Flasher.app
```
