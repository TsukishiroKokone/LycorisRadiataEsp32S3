# ESP_Claw_http_V3.0 使用说明

这是 ESP-Claw Web 控制固件，当前版本新增了传感器、执行器、音视频模块页面。

## 文件夹结构

```text
ESP_Claw_http_V3.0/
  source/
    ESP_Claw_http_V3.0/      ESP-IDF 源码工程
  firmware/
    ESP_Claw_http_V3.0_merged.bin
  instructions/
    README.md
```

`firmware` 文件夹只保留合并固件。交付源码中不包含 `build`、`managed_components`、前端 `node_modules` 等构建临时文件。

## 直接刷机

合并固件从 `0x0` 地址烧录：

```powershell
cd ESP_Claw_http_V3.0
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 firmware\ESP_Claw_http_V3.0_merged.bin
```

这个合并固件已经包含 bootloader、分区表、应用程序、网页资源、系统 FATFS 和存储 FATFS。

## 从源码编译

建议使用 ESP-IDF 5.5.x。Windows 下建议复制到短路径编译，避免路径太长导致编译失败。

```powershell
cd ESP_Claw_http_V3.0
robocopy source\ESP_Claw_http_V3.0 C:\r\a\e /MIR /XD build managed_components node_modules
cd C:\r\a\e
$env:SDKCONFIG_DEFAULTS='sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults'
idf.py build
idf.py merge-bin -o build\ESP_Claw_http_V3.0_merged.bin
```

如果需要更新交付固件，把生成的文件复制到：

```text
ESP_Claw_http_V3.0\firmware\ESP_Claw_http_V3.0_merged.bin
```

如果修改了网页前端，需要先重新构建前端：

```powershell
cd C:\r\a\e\components\http_server\frontend_source
corepack pnpm install --frozen-lockfile
corepack pnpm build
cd C:\r\a\e
idf.py build
```

## 网页界面

网页左侧列表保持原 ESP-Claw Web 的布局和原有入口，同时新增：

- 传感器
- 执行器
- 音视频

页面支持中英文切换。同一个页面只显示当前语言，不同时显示中文名和英文名。

## 引脚规则

所有模块都没有默认引脚。进入模块页面后，需要先选择引脚，再保存配置和测试。

不可选择的引脚：

```text
4, 5, 6, 7, 15, 16, 19, 20, 35, 36, 37, 43, 44
```

配置不会自动保存。不点击“保存配置”时刷新页面会恢复未选择状态。换线或换模块测试时，可以点击“清除配置”重新选择。

## 当前已实现模块

传感器：

- GPIO 输入：光遮断模块、水银倾斜开关、按键模块、磁簧开关、TCRT5000 红外循迹模块
- GPIO 输出：有源蜂鸣器、单引脚双色 LED、三色灯模块，三色灯使用红、蓝、绿三个 GPIO 输出
- ADC：光敏电阻模块、水位检测模块
- GPIO & ADC：双轴摇杆模块、土壤湿度模块、声音强度模块
- PWM：无源蜂鸣器，支持网页选择旋律播放
- 单总线 / 单数据引脚：DS18B20、DHT11、全彩 RGB 模块、WS2812 灯环
- I2C：MPU6050 姿态传感器
- 其它：旋转编码器、HC-SR04 超声波测距模块

执行器：

- PWM 舵机
- 多路舵机测试
- 28BYJ-48 步进电机

音视频：

- USB 麦克风页面
- USB 喇叭页面
- USB UVC 摄像头页面
- DVP 摄像头占位页面
- USB 三合一摄像头页面

## 预留模块

下面这些页面已经保留入口，但第一版不作为完整硬件验收项：

- 直流电机驱动
- 电机驱动扩展
- USB 麦克风实时采集测试
- USB 喇叭实时播放测试
- USB UVC 摄像头实时预览测试
- DVP 摄像头驱动
- USB 三合一摄像头完整 UVC/UAC 联合测试

## 验收说明

- GPIO 输入模块：页面直接显示数字电平状态。
- ADC 模块：页面显示模拟电压和 ADC 原始值。
- GPIO & ADC 模块：页面同时显示数字电平和模拟电压。
- 输出模块：通过网页按钮控制 GPIO 或 PWM 输出。
- MPU6050：显示加速度、角速度、温度、估算俯仰角和横滚角。
- 每个模块测试前先选择引脚并保存配置，再查看实时状态或控制输出。
