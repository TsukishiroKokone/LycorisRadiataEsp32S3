# LycorisRadiata GUI 使用说明

这是一个给小白使用的图形化烧录工具，可以把同一个 `.bin` 固件一键并行烧录到多个 ESP32-S3。

## 文件

- `esp32s3_flash_gui.py`：图形界面主程序
- `launch_flasher_gui.command`：macOS 双击启动脚本
- `launch_flasher_gui_windows.bat`：Windows 双击启动脚本
- `esp32s3_multi_flash.py`：命令行批量烧录工具
- `build_standalone.sh`：Linux/macOS 构建单文件可执行版
- `build_standalone_windows.bat`：Windows 构建单文件 `.exe`
- `dist/LycorisRadiata-Flasher`：Linux 构建后的单文件可执行程序
- `dist/LycorisRadiata-Flasher.app`：macOS 构建后的 App bundle

## 单文件可执行版 / 内置环境

Linux 或 macOS 构建：

```bash
chmod +x build_standalone.sh
./build_standalone.sh
```

构建完成后运行：

```bash
./dist/LycorisRadiata-Flasher
```

这个可执行文件内置：

- Python 运行时
- Tkinter GUI 所需模块
- esptool
- pyserial / serial
- 其它 Python 依赖

用户机器不需要再手动安装 esptool。注意：Linux 上可执行文件仍需要系统有图形桌面环境以及基础 Tk/X11 运行库；串口权限仍需要 `dialout` 组。

Windows 构建：

```bat
build_standalone_windows.bat
```

输出：

```text
dist\LycorisRadiata-Flasher.exe
```

## macOS 版怎么构建？

结论：**不能在 Linux 本机直接可靠地产出 macOS 可执行文件**；PyInstaller、Briefcase 这类工具通常都要求在目标系统构建。可行方式是使用真实 macOS 环境，例如：

- macOS 实体机 / Mac mini / MacBook
- GitHub Actions 的 `macos-latest` 或 `macos-13` runner

本项目已经内置 GitHub Actions 工作流：

```text
.github/workflows/build-flasher.yml
```

上传到 GitHub 后，在 Actions 里手动运行 `Build LycorisRadiata Flasher`，会分别构建：

- Linux x64
- Windows x64
- macOS Apple Silicon / ARM64
- macOS Intel / x64

macOS 构建产物会打包为：

```text
LycorisRadiata-Flasher-macos-arm64.zip
LycorisRadiata-Flasher-macos-intel.zip
```

其中包含可双击的：

```text
LycorisRadiata-Flasher.app
```

## macOS 本机手动构建

在 macOS 上安装 Python 3 后，进入项目目录运行：

```bash
chmod +x build_standalone.sh
./build_standalone.sh
```

输出：

```text
dist/LycorisRadiata-Flasher.app
dist/LycorisRadiata-Flasher-macOS.zip
```

解压 zip 后可双击 `.app`。如果 macOS Gatekeeper 阻止打开，可以右键 App → 打开，或在终端执行：

```bash
xattr -dr com.apple.quarantine dist/LycorisRadiata-Flasher.app
```

## macOS 源码启动

如果不构建 `.app`，也可以双击 `launch_flasher_gui.command`。如果 macOS 提示无权限，打开终端执行：

```bash
chmod +x launch_flasher_gui.command
```

然后：

1. 选择 `.bin` 固件。
5. 插入一个或多个 ESP32-S3。
6. 点击“刷新端口”。
7. 勾选要烧录的设备。
8. 点击“💖 一键烧录选中设备”。

macOS 常见端口名：

- `/dev/cu.usbmodem*`
- `/dev/cu.usbserial*`
- `/dev/cu.wchusbserial*`

## Linux 使用方法

```bash
cd /home/chloelilymu/Desktop/ESP_Claw_http_V3.0
python3 esp32s3_flash_gui.py
```

或者：

```bash
chmod +x esp32s3_flash_gui.py
./esp32s3_flash_gui.py
```

Linux 常见端口名：

- `/dev/serial/by-id/*`
- `/dev/ttyACM*`
- `/dev/ttyUSB*`

## Windows 使用方法

1. 安装 Python 3。
2. 双击 `launch_flasher_gui_windows.bat`。
3. 选择 `.bin` 固件。
4. 点击“刷新端口”，程序会优先只显示疑似 ESP32-S3 的 COM 端口。
5. 勾选要烧录的 COM 端口。
6. 点击一键烧录。

Windows 自动识别依据包括 Espressif 原生 USB/JTAG Serial、CP210x、CH340/CH910x、FTDI 等常见 ESP32-S3 开发板 USB 串口芯片。这样不会一打开就显示全部 `COM1..COM256`。如果没有识别到但你确认设备已插入，可以勾选“显示全部 COM”手动选择。

## 常见问题

### 提示连接不上 ESP32-S3

可以尝试：

1. 确认 USB 线支持数据传输，不只是充电线。
2. 按住 ESP32-S3 的 `BOOT` 键。
3. 点一下 `RESET`。
4. 松开 `BOOT`。
5. 再点一键烧录。

### Linux 没权限打开串口

确认用户在 `dialout` 组：

```bash
sudo usermod -aG dialout $USER
```

然后注销重新登录。

### Python 没有 Tkinter

Ubuntu/Debian：

```bash
sudo apt install python3-tk
```

macOS 官方 Python 通常自带 Tkinter。若 Homebrew Python 没有 Tkinter，可安装官方 Python 或 `python-tk`。
