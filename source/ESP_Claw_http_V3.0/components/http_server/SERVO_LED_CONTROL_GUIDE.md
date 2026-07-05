# HTTP Server 舵机 & 灯环控制页面 —— 实施规划文档

> 规划日期：2026-06-11
> 目标板：`esp32_S3_DevKitC_1_breadboard`
> 目标：在现有 `http_server` 组件中添加舵机控制和 WS2812 灯环控制页面。

---

## 一、现有基础设施分析

### 1.1 已可用的硬件模块

| 模块 | 组件 | 底层驱动 | 说明 |
|------|------|---------|------|
| **MCPWM** | `lua_driver_mcpwm` | `driver/mcpwm_prelude.h` | PWM 输出，可驱动舵机。支持 2 通道，频率/占空比可调 |
| **LED Strip** | `lua_module_led_strip` | `espressif/led_strip` | WS2812 灯环，使用 RMT TX（GPIO 38） |
| **GPIO** | `lua_driver_gpio` | `driver/gpio.h` | GPIO 输入/输出 |
| **LEDC** | - | `driver/ledc.h` | 硬件 PWM（可替代 MCPWM 用于舵机） |

### 1.2 板子已占用的 GPIO（esp32_S3_DevKitC_1_breadboard）

| GPIO | 用途 | 说明 |
|------|------|------|
| 38 | RMT TX | WS2812 灯环数据线 |
| 16 | LEDC 背光 | LCD 背光 PWM |
| 4, 5, 6, 7, 15 | SPI LCD | ST7789 显示屏 |
| 2, 3 | I2C | I2C 总线 |
| 9 | I2S DOUT | 音频输出 |
| 43, 44 | UART0 | 调试串口 |

**舵机可用 GPIO**（建议）：10, 11, 12, 13, 14, 17, 18, 21, 33, 34, 35, 36, 37, 39, 40, 41, 42, 45, 46, 47, 48

> 注：GPIO 34-48 仅支持输入或受限功能，舵机 PWM 应使用 GPIO 0-33 中的空闲引脚。

### 1.3 http_server 已有的添加页面模式（参考 Camera 实现）

| 步骤 | 文件 | 操作 |
|------|------|------|
| C 后端 | `http_server_xxx_api.c` | 新建：HTTP handler 实现 |
| C 后端 | `http_server_priv.h` | 声明注册函数 |
| C 后端 | `http_server_core.c` | `http_server_start()` 中注册路由 |
| C 后端 | `CMakeLists.txt` | 添加源文件和依赖 |
| C 后端 | `idf_component.yml` | 添加组件依赖 |
| 前端 | `src/pages/XxxPage.tsx` | 新建页面组件 |
| 前端 | `src/state/dirty.ts` | 追加 TabId |
| 前端 | `src/components/layout/Sidebar.tsx` | 添加导航项 |
| 前端 | `src/App.tsx` | 添加路由分支 |
| 前端 | `src/i18n/en.ts`, `zh-cn.ts` | 添加翻译 |

---

## 二、功能设计

### 2.1 舵机控制

```
┌─────────────────────────────────────────────┐
│  舵机控制                                   │
│                                             │
│  GPIO 引脚: [下拉选择: 10,11,12,13,14...]   │
│  频率:      [50 Hz] (舵机标准)              │
│  角度:      [0° ════════●═══════ 180°]      │
│             [滑块 + 数字输入]               │
│  使能:      [ON/OFF 开关]                   │
│                                             │
│  [+ 添加舵机通道]  (最多 4 个)              │
└─────────────────────────────────────────────┘
```

**API 设计**：

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/servo/status` | 获取所有通道状态 `{channels:[{gpio, angle, enabled}]}` |
| `POST` | `/api/servo/config` | 配置通道 `{channel:0, gpio:10, frequency:50}` |
| `POST` | `/api/servo/angle` | 设置角度 `{channel:0, angle:90}` |
| `POST` | `/api/servo/enable` | 使能/禁用 `{channel:0, enabled:true}` |

### 2.2 灯环控制（WS2812）

```
┌─────────────────────────────────────────────┐
│  WS2812 灯环控制                            │
│                                             │
│  LED 数量:  [8 ▼]  (8/12/16/24)            │
│                                            │
│  模式:  ○ 静态颜色  ○ 彩虹  ○ 呼吸  ○ 流水  │
│                                            │
│  静态颜色:                                  │
│  [颜色选择器]  亮度: [███●███] 80%          │
│                                            │
│  全部 LED 同色:  [ON/OFF]                   │
│  [填充]  [熄灭]                             │
└─────────────────────────────────────────────┘
```

**API 设计**：

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/led/status` | 获取状态 `{led_count, mode, color, brightness, enabled}` |
| `POST` | `/api/led/config` | 配置 `{led_count:8, gpio:38}` |
| `POST` | `/api/led/solid` | 静态颜色 `{color:"#FF0000", brightness:80}` |
| `POST` | `/api/led/mode` | 切换模式 `{mode:"rainbow"|"breathe"|"flow"|"solid"}` |
| `POST` | `/api/led/off` | 关闭所有 LED |

### 2.3 TabId 扩展

```typescript
export type TabId =
  | ... existing ...
  | 'camera'
  | 'devices';    // 统合舵机 + 灯环 + GPIO（一个页面切换子 tab）
```

---

## 三、架构设计

### 3.1 整体架构

```
浏览器                         ESP32-S3
┌──────────┐                  ┌──────────────────────────┐
│ Devices  │  HTTP GET/POST   │ http_server_devices_api.c│
│  Page    │ ◄──────────────► │                          │
│          │                  │  ├─ /api/servo/*         │
│  - 舵机  │                  │  ├─ /api/led/*           │
│  - 灯环  │                  │  └─ /api/gpio/*          │
│  - GPIO  │                  └────┬──────────┬──────────┘
└──────────┘                       │          │
                            ┌──────▼──┐  ┌───▼──────────┐
                            │  MCPWM  │  │  LED Strip   │
                            │  driver │  │  (RMT+WS2812)│
                            │         │  │              │
                            │ GPIO 10 │  │  GPIO 38     │
                            │  →舵机  │  │  →灯环数据线 │
                            └─────────┘  └──────────────┘
```

### 3.2 C 后端组件依赖

```
http_server
  ├─ 已有: camera_hal, esp_video, ...
  ├─ 新增: esp_driver_mcpwm    (舵机 PWM)
  ├─ 新增: espressif/led_strip (WS2812 灯环)
  └─ 新增: esp_driver_gpio     (GPIO 控制)
```

> 这些 ESP-IDF 驱动在 `idf.py` 的默认组件路径中，无需额外 `idf_component.yml` 声明（MCPWM 和 GPIO 是 IDF 内置组件）。LED strip 用的 `espressif/led_strip` 已在 `board_devices.yaml` 中引入。

---

## 四、C 端实现步骤

### Step 1：新建 `http_server_devices_api.c`

包含三个模块：

#### 4.1 舵机（MCPWM）

```c
// 数据结构
typedef struct {
    int gpio;           // GPIO 引脚号
    uint32_t freq_hz;   // 频率 (舵机标准 50Hz)
    int angle;          // 当前角度 0-180
    bool enabled;       // 是否使能
    // MCPWM 句柄（内部）
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t cmpr;
    mcpwm_gen_handle_t gen;
} servo_channel_t;

// API
servo_init(channel, gpio)       → 初始化 MCPWM 定时器和通道
servo_set_angle(channel, 0-180) → 将角度转换为占空比并设置
servo_enable(channel, bool)     → 使能/禁用 PWM 输出
servo_deinit(channel)           → 释放 MCPWM 资源
```

**角度→PWM 转换**（标准舵机）：
- 0°   → 500μs  (占空比 2.5% @ 50Hz)
- 90°  → 1500μs (占空比 7.5% @ 50Hz)
- 180° → 2500μs (占空比 12.5% @ 50Hz)

#### 4.2 灯环（WS2812 via RMT）

```c
// 使用 espressif/led_strip 组件
// board_devices.yaml 已配置: gpio=38, max_leds=1（需改为实际数量）

// API
led_strip_init(led_count, gpio)       → 创建 led_strip 句柄
led_set_all(r, g, b)                  → 设置全部 LED
led_set_pixel(index, r, g, b)         → 设置单个 LED
led_refresh()                         → 刷新（发送数据到灯环）
led_clear()                           → 全部熄灭
```

#### 4.3 GPIO（预留，可选）

```c
// 简单的 GPIO 读/写 API
gpio_set_output(gpio, level)
gpio_get_input(gpio) → level
```

### Step 2-4：注册路由、声明、构建

与 camera 模式一致，不再重复。

---

## 五、前端实现步骤

### 5.1 新建 `pages/DevicesPage.tsx`

单页面包含两个子 Tab：

```
┌──────────────────────────────────────┐
│  [舵机控制]  [灯环控制]  [GPIO]      │  ← 子 Tab 切换
├──────────────────────────────────────┤
│                                      │
│   (当前子 Tab 的内容)                │
│                                      │
└──────────────────────────────────────┘
```

### 5.2 前端改动清单

| 文件 | 操作 |
|------|------|
| `pages/DevicesPage.tsx` | 新建：设备控制页面（含舵机/灯环/GPIO 子 Tab） |
| `state/dirty.ts` | 追加 `\| 'devices'` |
| `layout/Sidebar.tsx` | 添加 `IconDevices` + 导航项 |
| `App.tsx` | lazy import + Show 分支 |
| `i18n/en.ts` | ~10 条翻译 |
| `i18n/zh-cn.ts` | ~10 条翻译 |

---

## 六、修改清单总结

| 层级 | 文件 | 操作 | 说明 |
|------|------|------|------|
| **C** | `http_server_devices_api.c` | **新建** | 舵机 + 灯环 + GPIO 三个模块的 HTTP handler |
| **C** | `http_server_priv.h` | 修改 | 声明 `http_server_register_devices_routes` |
| **C** | `http_server_core.c` | 修改 | `http_server_start()` 中注册 devices 路由 |
| **C** | `CMakeLists.txt` | 修改 | 添加源文件（不需要新增 REQUIRES，MCPWM/GPIO/LED 已在系统中） |
| **前端** | `pages/DevicesPage.tsx` | **新建** | 含舵机/灯环/GPIO 三个子 Tab 的控制页面 |
| **前端** | `state/dirty.ts` | 修改 | 追加 `'devices'` TabId |
| **前端** | `layout/Sidebar.tsx` | 修改 | 添加导航项 |
| **前端** | `App.tsx` | 修改 | 添加路由 |
| **前端** | `i18n/*.ts` | 修改 | 翻译 |

---

## 七、实施顺序

1. **C 后端**：创建 `http_server_devices_api.c`，按 §四 实现三个硬件 API
2. **构建配置**：修改 CMakeLists.txt、http_server_priv.h、http_server_core.c
3. **前端页面**：按 §五 创建 `DevicesPage.tsx` + 集成到路由
4. **编译测试**：`idf.py build && flash`

---

## 八、安全注意事项

1. **舵机最大功耗**：单个 SG90 舵机堵转电流 ~750mA @ 5V。勿从 ESP32 3.3V 引脚取电，需外接 5V 电源。
2. **灯环功耗**：WS2812 每颗 LED 全白 ~60mA。8 颗 ~480mA。同样需外接电源。
3. **GPIO 电平**：ESP32-S3 GPIO 为 3.3V。某些舵机需要 5V PWM 信号，可能需要电平转换。
4. **GPIO 冲突检测**：后端应检查用户选择的 GPIO 是否已被板子占用（LCD、I2C、调试串口等）。

---

## 九、后续扩展

- WebSocket 实时角度回传（当前角度传感器）
- 预设动作组（多舵机协同：摆手、点头等）
- 灯环动画编辑器（时间轴编辑 LED 序列）
- GPIO 中断/事件监控
