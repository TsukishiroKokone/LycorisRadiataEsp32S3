
# ① 先构建前端（每次修改 .tsx/.ts 后都要做）
cd components/http_server/frontend_source
pnpm install --frozen-lockfile    # 首次需要，后续可跳过
pnpm run build                     # 产出 dist/index.html.gz

# ② 再构建固件
cd ../../..                        # 回到 edge_agent 根目录
idf.py build                       # CMake 校验 index.html.gz 存在 → 嵌入固件
idf.py flash                       # 烧录


# HTTP Server 摄像头图像显示界面 —— 实施方案分析文档

> 分析日期：2026-06-10  
> 目标板：`esp32_S3_DevKitC_1_breadboard`（USB UVC 摄像头）  
> 目标：在现有 `http_server` 组件中添加一个 Web 页面，实时显示摄像头图像。

---

## 一、现有架构分析

### 1.1 组件总览

```
components/http_server/
├── include/http_server.h          # 公共 API 头文件
├── http_server_priv.h             # 内部私有头文件（路由注册、上下文）
├── http_server_core.c             # 核心：server 初始化、启动、停止
├── http_server_assets.c           # 前端静态资源（index.html.gz, favicon）
├── http_server_capabilities_api.c # /api/capabilities
├── http_server_config_api.c       # /api/config  (GET/POST)
├── http_server_lua_modules_api.c  # /api/lua-modules
├── http_server_lua_app_api.c      # /lua/*, /api/lua/*  (条件编译)
├── http_server_status_api.c       # /api/status, /api/restart
├── http_server_files_api.c        # /api/files/*, /files/*
├── http_server_wechat_api.c       # /api/wechat/login/*
├── http_server_webim_api.c        # /api/webim/*, WebSocket /ws/webim
├── http_server_json.c             # JSON 序列化/反序列化工具
├── http_server_utils.c            # 路径安全、URL 解码、临时缓冲区
├── CMakeLists.txt                 # 编译配置
└── frontend_source/               # SolidJS + TypeScript 前端源码
    ├── index.html
    ├── src/
    │   ├── App.tsx                # 主应用（hash 路由）
    │   ├── api/client.ts          # REST API 封装
    │   ├── components/layout/     # Layout, Sidebar, StatusBar
    │   ├── pages/                 # 各个页面组件
    │   ├── i18n/en.ts, zh-cn.ts   # 多语言翻译
    │   ├── state/dirty.ts         # TabId 定义
    │   └── ...
    └── dist/index.html.gz         # 构建产物（嵌入固件）
```

### 1.2 核心设计模式

1. **C 后端** 通过 ESP-IDF 的 `esp_http_server` 注册 URI handler
2. **前端** 是 SolidJS 单页应用，编译为单个 gzip 文件嵌入固件
3. **路由模式** 固定且互不冲突：`/api/xxx` 为数据接口，`/` 为 Web 页面
4. **添加新页面的标准路径**：
   - C 端：新增 `http_server_xxx_api.c`，在 `http_server_priv.h` 声明注册函数
   - C 端：在 `http_server_core.c` 的 `http_server_start()` 中调用注册函数
   - 前端：新增 `pages/XxxPage.tsx`
   - 前端：在 `Sidebar.tsx` 添加 `TabId`、导航节点、图标
   - 前端：在 `dirty.ts` 添加 `TabId`
   - 前端：在 `App.tsx` 添加路由分支
   - 前端：在 `i18n` 添加翻译 key

### 1.3 现有 TabId 类型

```typescript
// state/dirty.ts
export type TabId =
  | 'status' | 'basic' | 'llm' | 'im' | 'webreq'
  | 'memory' | 'webim' | 'capabilities' | 'skills' | 'files';
```

---

## 二、本板的摄像头架构（USB UVC）

### 2.1 硬件链路

```
USB 摄像头 ──(USB 线)──> ESP32-S3 USB OTG ──> USB Host 栈
                                              └──> esp_video (UVC 驱动)
                                                   └──> V4L2 设备节点 /dev/video0
```

### 2.2 已有代码基础设施

| 文件 | 作用 |
|------|------|
| `boards/.../setup_device.c` | 通过 `esp_video_init()` 初始化 USB UVC，设备路径为 `ESP_VIDEO_USB_UVC_NAME(0)`（即 `/dev/video0`） |
| `components/lua_modules/lua_module_camera/src/camera_hal.c` | **V4L2 HAL 层**，封装了 `open/ioctl/mmap/VIDIOC_DQBUF` 等底层操作 |
| `components/lua_modules/lua_module_camera/src/camera_hal.h` | 公开 API：`camera_open()`, `camera_capture_frame()`, `camera_release_frame()`, `camera_close()` |
| `components/lua_modules/lua_module_camera/src/lua_module_camera.c` | Lua 绑定层，调用 camera_hal.c |

### 2.3 camera_hal.h 关键 API

```c
// 打开摄像头设备
esp_err_t camera_open(const char *dev_path, const camera_open_opts_t *opts);

// 捕获一帧（阻塞），返回 JPEG 数据指针 + 大小
esp_err_t camera_capture_frame(int timeout_ms, uint8_t **frame_data,
                                size_t *frame_bytes, camera_frame_info_t *out_info);

// 释放帧（归还 V4L2 buffer）
esp_err_t camera_release_frame(void *frame_data);

// 获取流信息（宽/高/像素格式）
esp_err_t camera_get_stream_info(camera_stream_info_t *out_info);

// 关闭摄像头
esp_err_t camera_close(void);
```

> **关键差异**：USB UVC 摄像头输出的是 **已压缩的 JPEG 帧**（V4L2_PIX_FMT_MJPEG），不需要软件编码。camera_hal.c 通过 VIDIOC_DQBUF 出队一帧时得到的就是可直接发送给浏览器的 JPEG 数据。

---

## 三、方案选择

### 方案 A：MJPEG 流（推荐主方案）

**原理**：C 端创建 `/camera/mjpeg` HTTP endpoint，循环调用 `camera_capture_frame()` → 用 `multipart/x-mixed-replace` 格式发送 → 浏览器 `<img>` 标签原生支持。

**优点**：
- 一帧一帧来，浏览器 `<img src="/camera/mjpeg">` 直接播放
- 延迟低（~50-100ms），帧率取决于摄像头（通常 15-30fps）
- 项目已有 `camera_hal.c`，代码实现不超过 100 行

**缺点**：
- 一个长连接持续占用一个 HTTP socket

### 方案 B：快照 API（辅助方案）

**原理**：C 端提供 `/api/camera/snapshot` 返回单张 JPEG。前端按钮手动刷新。

**优点**：
- 无长连接，完全 RESTful
- 适合"拍一张照片"的交互场景

**缺点**：
- 无实时视频流

### 推荐：A + B 组合

- 主界面用 MJPEG `<img>` 标签做实时预览
- 加一个"抓取快照"按钮调用 `/api/camera/snapshot`

---

## 四、具体实现步骤

### 4.1 C 端改动

#### Step 1：新建 `http_server_camera_api.c`

```c
// components/http_server/http_server_camera_api.c

#include "http_server_priv.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_hal.h"                         // <-- 项目已有的 HAL

#define CAMERA_DEV_PATH    "/dev/video0"        // USB UVC 设备
#define CAMERA_FRAME_TIMEOUT_MS   2000
#define MJPEG_FRAME_DELAY_MS       33           // ~30fps throttle

static const char *TAG = "http_camera";

// ── 快照 handler ─────────────────────────────────────────────────

static esp_err_t camera_snapshot_handler(httpd_req_t *req)
{
    uint8_t *frame_data = NULL;
    size_t frame_bytes = 0;

    esp_err_t ret = camera_capture_frame(CAMERA_FRAME_TIMEOUT_MS,
                                         &frame_data, &frame_bytes, NULL);
    if (ret != ESP_OK || !frame_data || frame_bytes == 0) {
        ESP_LOGW(TAG, "snapshot: capture failed: %s", esp_err_to_name(ret));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to capture camera frame");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    ret = httpd_resp_send(req, (const char *)frame_data, (ssize_t)frame_bytes);
    camera_release_frame(frame_data);
    return ret;
}

// ── MJPEG 流 handler ─────────────────────────────────────────────

static esp_err_t camera_mjpeg_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=FRAME");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    while (true) {
        uint8_t *frame_data = NULL;
        size_t frame_bytes = 0;

        esp_err_t ret = camera_capture_frame(CAMERA_FRAME_TIMEOUT_MS,
                                             &frame_data, &frame_bytes, NULL);
        if (ret != ESP_OK || !frame_data || frame_bytes == 0) {
            // 超时或出错：等一下再试
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 构造 multipart 边界头
        char hdr[128];
        int hdr_len = snprintf(hdr, sizeof(hdr),
            "\r\n--FRAME\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n\r\n",
            (unsigned)frame_bytes);

        // 发送
        if (httpd_resp_send_chunk(req, hdr, hdr_len) != ESP_OK) break;
        if (httpd_resp_send_chunk(req, (const char *)frame_data,
                                  (ssize_t)frame_bytes) != ESP_OK) {
            camera_release_frame(frame_data);
            break;
        }

        camera_release_frame(frame_data);
        vTaskDelay(pdMS_TO_TICKS(MJPEG_FRAME_DELAY_MS));
    }

    ESP_LOGI(TAG, "MJPEG client disconnected");
    return ESP_OK;
}

// ── 状态 API ─────────────────────────────────────────────────────

static esp_err_t camera_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    // camera_hal 是全局单例；camera_is_open() 告诉我们是否已初始化
    bool available = camera_is_open();
    cJSON_AddBoolToObject(root, "available", available);

    if (available) {
        camera_stream_info_t info = {0};
        if (camera_get_stream_info(&info) == ESP_OK) {
            cJSON_AddNumberToObject(root, "width",  (double)info.width);
            cJSON_AddNumberToObject(root, "height", (double)info.height);
            http_server_json_add_string(root, "pixel_format", info.pixel_format_str);
        }
    }

    return http_server_send_json_response(req, root);
}

// ── 注册入口 ─────────────────────────────────────────────────────

esp_err_t http_server_register_camera_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/camera/snapshot", .method = HTTP_GET,
          .handler = camera_snapshot_handler },
        { .uri = "/api/camera/status",   .method = HTTP_GET,
          .handler = camera_status_handler },
        { .uri = "/camera/mjpeg",        .method = HTTP_GET,
          .handler = camera_mjpeg_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
```

#### Step 2：在 `http_server_priv.h` 中声明

```c
// 在文件末尾添加：
esp_err_t http_server_register_camera_routes(httpd_handle_t server);
```

#### Step 3：在 `http_server_core.c` 中注册

```c
// 在 http_server_start() 中，webim 路由注册之后添加：
ESP_RETURN_ON_ERROR(
    http_server_register_camera_routes(s_ctx.server),
    TAG, "Failed to register camera routes");

// 可选：如果摄像头尚未初始化，在此处做一次 open，确保 mjpeg/snapshot 可用
```

#### Step 4：更新 `CMakeLists.txt`

```cmake
# 在 http_server_srcs 列表末尾添加：
"http_server_camera_api.c"

# 在 http_server_requires 中添加（camera_hal 来自 lua_module_camera）：
list(APPEND http_server_requires lua_module_camera)
```

> **注意**：`lua_module_camera` 组件内部依赖 `cap_lua` 和 `lua_module_image`，需要确认链接是否引入不必要的 Lua 符号。替代方案是将 `camera_hal.c` 抽成独立组件（见 [附录 A](#附录-a将-camera_hal-抽为独立组件可选)）。

### 4.2 前端改动

#### Step 1：新建 `pages/CameraPage.tsx`

```tsx
// frontend_source/src/pages/CameraPage.tsx
import { createSignal, type Component } from 'solid-js';
import { t } from '../i18n';
import { PageHeader } from '../components/ui/PageHeader';
import { Section } from '../components/ui/Section';

export const CameraPage: Component = () => {
  const [snapshotUrl, setSnapshotUrl] = createSignal('');

  // MJPEG 流 URL：直接用当前页面 host，避免跨域
  const mjpegUrl = () => `/camera/mjpeg`;

  const takeSnapshot = () => {
    // 加时间戳参数避免浏览器缓存
    setSnapshotUrl(`/api/camera/snapshot?t=${Date.now()}`);
  };

  return (
    <div>
      <PageHeader
        title={t('navCamera') as string}
        description={t('cameraDescription') as string}
      />
      <Section>
        <div class="flex flex-col items-center gap-4">
          {/* MJPEG 实时视频流 */}
          <div class="relative bg-black rounded-lg overflow-hidden max-w-full">
            <img
              src={mjpegUrl()}
              alt="Camera stream"
              class="max-w-full h-auto"
              onError={(e) => {
                // 如果加载失败，显示占位提示
                (e.target as HTMLImageElement).style.display = 'none';
              }}
            />
          </div>

          <p class="text-xs text-[var(--color-text-muted)]">
            {t('cameraStreamHint')}
          </p>

          {/* 快照按钮 */}
          <button
            onClick={takeSnapshot}
            class="px-4 py-2 bg-blue-600 text-white rounded
                   hover:bg-blue-700 transition"
          >
            {t('cameraSnapshot')}
          </button>

          {/* 快照结果 */}
          {snapshotUrl() && (
            <div class="mt-4">
              <p class="text-sm text-[var(--color-text-muted)] mb-2">
                {t('cameraSnapshotResult')}
              </p>
              <img
                src={snapshotUrl()}
                alt="Snapshot"
                class="max-w-xs rounded border border-[var(--color-border-subtle)]"
              />
            </div>
          )}
        </div>
      </Section>
    </div>
  );
};
```

#### Step 2：在 `state/dirty.ts` 中追加 TabId

```typescript
export type TabId =
  | ... existing ...
  | 'camera';
```

并初始化 `camera: false`。

#### Step 3：在 `Sidebar.tsx` 中添加导航

```tsx
import { Camera } from 'lucide-solid';
const IconCamera: Component = () => <Camera class={iconClass} />;

// NAV_TREE 末尾追加：
{ kind: 'leaf', id: 'camera', labelKey: 'navCamera', icon: IconCamera },
```

#### Step 4：在 `App.tsx` 中添加路由

```tsx
const CameraPage = lazy(() =>
  import('./pages/CameraPage').then((mod) => ({ default: mod.CameraPage })),
);
// Suspense 内添加：
<Show when={currentTab() === 'camera'}>
  <CameraPage />
</Show>
```

#### Step 5：i18n 翻译

```typescript
// en.ts 追加:
navCamera: 'Camera',
cameraDescription: 'Live USB camera preview via MJPEG streaming.',
cameraStreamHint: 'Image refreshes automatically via MJPEG stream.',
cameraSnapshot: 'Take Snapshot',
cameraSnapshotResult: 'Snapshot captured:',
cameraLoadError: 'Failed to open camera. Check USB connection.',

// zh-cn.ts 追加:
navCamera: '摄像头',
cameraDescription: '通过 MJPEG 流实时预览 USB 摄像头画面。',
cameraStreamHint: '画面通过 MJPEG 流自动刷新。',
cameraSnapshot: '抓取快照',
cameraSnapshotResult: '已捕获快照：',
cameraLoadError: '无法打开摄像头，请检查 USB 连接。',
```

---

## 五、注意事项（USB UVC 特有问题）

### 5.1 camera_hal 是全局单例，MJPEG 和快照共享

`camera_hal.c` 的 `s_camera` 是静态全局变量，同一时刻只有一个进程可以持有。这意味着：

- `camera_open()` 只需调用一次（通常在 `setup_device.c` 或 `app_main()` 中完成）
- MJPEG handler 和 snapshot handler 都是在同一进程内的不同 HTTP worker 线程，**共享同一个 V4L2 设备**
- `camera_capture_frame()` 是线程安全的（内部有 `SemaphoreHandle_t lock`）
- **Q**: MJPEG handler 持续 consume buffer，snapshot handler 能否同时拿到帧？  
  **A**: 可以，因为 V4L2 有 3 个 MMAP buffer（`CAMERA_BUFFER_COUNT = 3`），只要 MJPEG 每帧都 release，snapshot 就能 DQBUF 到其他 buffer。

### 5.2 带宽和帧率

USB UVC 输出的 JPEG 大小取决于传感器分辨率和压缩率：

| 分辨率 | 典型 JPEG 大小 | 30fps 带宽 | 
|--------|---------------|-----------|
| 320x240 | 8-15 KB | ~2-4 Mbps |
| 640x480 | 25-60 KB | ~6-15 Mbps |
| 1280x720 | 60-150 KB | ~15-36 Mbps |

Wi-Fi（ESP32-S3 802.11n 实测 ~10-20 Mbps TCP）在 VGA 30fps 下可能成为瓶颈。建议通过 `camera_open_opts_t` 限制分辨率。

### 5.3 Socket 资源

- `max_open_sockets = 12`（`http_server_core.c`）
- 每个 MJPEG 流占用 1 个长连接
- 建议最多同时 2-3 个 MJPEG 客户端，剩余 socket 留其他请求

### 5.4 摄像头初始化时机

`setup_device.c` 中通过 `CUSTOM_DEVICE_IMPLEMENT(camera, ...)` 声明了 `usb_camera_init()`。该函数调用 `esp_video_init()` → 创建 `/dev/video0`。此初始化在 `esp_board_manager_init()` 阶段执行。

http_server 启动时可能摄像头尚未完全就绪（等待 USB 枚举），建议在 `camera_` handler 内检查 `camera_is_open()` 并优雅返回错误。

### 5.5 与其他板子的兼容性

| 板子 | 摄像头类型 | 设备路径 | 帧获取 API |
|------|-----------|---------|-----------|
| **esp32_S3_DevKitC_1_breadboard** | USB UVC | `/dev/video0` | `camera_capture_frame()` |
| esp32_p4_eye | DVP (OV5640等) | N/A | `esp_camera_fb_get()` |
| 无摄像头的板子 | N/A | N/A | `camera_is_open()` → false |

建议在 Kconfig 中增加开关，且 snapshot handler 和 mjpeg handler 内部先检查 `camera_is_open()`。

---

## 六、修改清单总结

| 层级 | 文件 | 操作 | 说明 |
|------|------|------|------|
| **C** | `http_server_camera_api.c` | **新建** | MJPEG + snapshot + status 三个 handler |
| **C** | `http_server_priv.h` | **修改** | 声明 `http_server_register_camera_routes` |
| **C** | `http_server_core.c` | **修改** | `http_server_start()` 中注册 camera 路由 |
| **C** | `CMakeLists.txt` | **修改** | 添加 `http_server_camera_api.c` + 依赖 `lua_module_camera` |
| **前端** | `pages/CameraPage.tsx` | **新建** | MJPEG `<img>` + 快照按钮 |
| **前端** | `state/dirty.ts` | **修改** | 追加 `'camera'` TabId |
| **前端** | `layout/Sidebar.tsx` | **修改** | 导入 Camera 图标，追加导航项 |
| **前端** | `App.tsx` | **修改** | lazy import + Show 分支 |
| **前端** | `i18n/en.ts` | **修改** | 5 条英文翻译 |
| **前端** | `i18n/zh-cn.ts` | **修改** | 5 条中文翻译 |

---

## 七、附录 A：将 camera_hal 抽为独立组件（可选）

当前 `camera_hal.c` 位于 `lua_module_camera` 组件内，它的 CMakeLists.txt 依赖了 `cap_lua` 和 `lua_module_image`。如果不想让 http_server 间接依赖 Lua 栈，可以将 `camera_hal.c` + `camera_hal.h` 抽取为一个独立的 IDF 组件，比如：

```
components/camera_hal/
├── CMakeLists.txt       # 只依赖 esp_timer + freertos（无 Lua）
├── camera_hal.c
└── include/
    └── camera_hal.h
```

然后：
- `lua_module_camera` 的 CMakeLists.txt 添加 `REQUIRES camera_hal`
- `http_server` 的 CMakeLists.txt 也添加 `REQUIRES camera_hal`

这样 http_server 和 lua_module_camera 都复用同一套 HAL，无冗余依赖。

---

## 八、附录 B：快速验证方法

1. **检查摄像头是否就绪**：浏览器打开 `http://<设备IP>/api/camera/status`，应返回 `{"available":true,"width":640,...}`

2. **测试快照**：浏览器打开 `http://<设备IP>/api/camera/snapshot`，应看到一张 JPEG 图片

3. **测试 MJPEG 流**：浏览器打开 `http://<设备IP>/camera/mjpeg`，应看到连续的视频流

4. **完整前端测试**：`cd frontend_source && pnpm run dev`（连到设备），导航到 Camera 页面

---

## 九、附录 C：前端构建环境搭建与完整打包流程

修改前端代码后，必须重新打包才能嵌入固件。以下是完整操作步骤。

### C.1 一次性环境准备（仅首次）

**Windows 上需要安装 Node.js 和 pnpm：**

```powershell
# 1. 下载安装 Node.js LTS 版
#    官网: https://nodejs.org
#    下载 Windows Installer (.msi)，双击安装，一路默认即可。
#
#    装完后重新打开 PowerShell，验证：
node --version     # 应输出 v22.x.x 或 v20.x.x
npm --version      # 应输出 10.x.x

# 2. 用 npm 全局安装 pnpm
npm install -g pnpm

#    验证：
pnpm --version     # 应输出 10.x.x
```

> **注意**：如果在 PowerShell 中遇到 `无法将"pnpm"项识别为 cmdlet` 错误，
> 说明还没有安装 Node.js 或 pnpm。请先完成上面两步。

### C.2 每次修改前端后的打包流程

```powershell
# ① 进入前端源码目录
cd D:\ESP32-12-3\6-10\esp-claw\application\edge_agent\components\http_server\frontend_source

# ② 安装依赖（首次需要，后续如果 package.json 没变可跳过）
pnpm install --frozen-lockfile

# ③ 构建前端 → 产出 dist/index.html.gz
pnpm run build
# 看到 "✓ built in xxx ms" 表示成功

# ④ 回到项目根目录，构建固件
cd D:\ESP32-12-3\6-10\esp-claw\application\edge_agent
idf.py build
# 看到 "Project build complete" 表示成功

# ⑤ 烧录到设备
idf.py flash
```

### C.3 完整链路图示

```
修改 .tsx/.ts 源文件
       │
       ▼
  pnpm run build          ← Vite 打包、压缩
       │
       ▼
  dist/index.html.gz       ← 产物（所有 JS/CSS/HTML 合一的 gzip 文件）
       │
       ▼
  idf.py build            ← CMake 检测 .gz 存在
       │                    生成 .incbin 汇编 → 嵌入 .rodata 段
       │                    编译 + 链接 → esp-claw.bin
       ▼
  esp-claw.bin            ← 固件镜像（前端已嵌入 app 二进制）
       │
       ▼
  idf.py flash            ← esptool 烧入 Flash 的 ota_0 分区
       │
       ▼
  设备重启 → 浏览器访问 → HTTP 返回 .gz → 自动解压 → 渲染页面
```

### C.4 开发调试技巧

如果只是想快速调试前端 UI（不烧固件），可以用 Vite 开发服务器：

```powershell
cd components/http_server/frontend_source
pnpm run dev
# 启动 localhost:3000，自动代理 /api 请求到设备
# vite.config.ts 中配置了 proxy: '/api': 'http://esp-claw.local/'
```

这会启动一个本地开发服务器，修改 `.tsx` 文件后浏览器**热更新**，无需每次 `idf.py build && flash`。开发完成后再走 C.2 的打包流程。

---

## 十、实施记录（2026-06-10 ~ 2026-06-11）✅ 已完成

### 10.1 最终改动文件清单

| 层级 | 文件 | 操作 | 说明 |
|------|------|------|------|
| **新建组件** | `components/camera_hal/CMakeLists.txt` | 新建 | 独立 HAL 组件，依赖 `esp_timer` + `freertos` + `esp_video` |
| **新建组件** | `components/camera_hal/idf_component.yml` | 新建 | 依赖 `espressif/esp_video: ^2.0.1` |
| **新建组件** | `components/camera_hal/camera_hal.c` | 复制+修改 | 从 lua_module_camera 复制，**S_FMT 失败改为 fallback 到 G_FMT** |
| **新建组件** | `components/camera_hal/include/camera_hal.h` | 复制 | 公开 API（无修改） |
| **C** | `http_server/http_server_camera_api.c` | **新建** | MJPEG `/camera/mjpeg` + snapshot `/api/camera/snapshot` + status `/api/camera/status` |
| **C** | `http_server/http_server_priv.h` | 修改 | 声明 `http_server_register_camera_routes()` |
| **C** | `http_server/http_server_core.c` | 修改 | 在 `http_server_start()` 注册 camera 路由 |
| **C** | `http_server/CMakeLists.txt` | 修改 | 添加 `http_server_camera_api.c` 源文件 + `camera_hal` + `esp_video` 依赖 |
| **C** | `http_server/idf_component.yml` | 修改 | 添加 `camera_hal`（路径 `../camera_hal`）和 `espressif/esp_video` 依赖 |
| **C** | `main/main.c` | 修改 | 启动前 `unlink()` FATFS 脆弱文件，防止 `app_claw_start` 崩溃 |
| **C** | `camera_hal/camera_hal.c` | 修改 | `VIDIOC_S_FMT` 失败 → fallback `VIDIOC_G_FMT`，不再 abort |

> 前端已完整实现（`CameraPage.tsx`、`dirty.ts`、`Sidebar.tsx`、`App.tsx`、`i18n`），无需额外修改。

---

### 10.2 调试过程：5 个关键问题

#### 问题 1：前端页面存在，摄像头不显示

**现象**：`<img src="/camera/mjpeg">` 返回 404。

**原因**：前端已完成，C 后端 `http_server_camera_api.c` 不存在，HTTP server 未注册 camera 路由。

**解决**：按 §4.1 创建 `http_server_camera_api.c` 并注册路由。

---

#### 问题 2：CMake 找不到 `lua_module_camera`

```
Failed to resolve component 'lua_module_camera': unknown name
```

**原因**：`lua_module_camera` 有 Kconfig 条件 `CONFIG{APP_CLAW_LUA_MODULE_CAMERA} == True`，当前 `=n`。

**解决**：按 §7 附录 A 方案，将 `camera_hal` 抽为独立组件放在项目 `components/` 下，无 Kconfig 门控。

---

#### 问题 3：FATFS 损坏导致无限重启

```
E app_claw: app_claw_start(333): Failed to init event router → abort()
```

**原因**：`idf.py flash` 不烧 FATFS 分区，残留数据损坏被 `ESP_ERROR_CHECK` 放大为 abort。

**解决**：`main.c` 中 `app_claw_start()` 前 `unlink()` 脆弱文件，首次烧录用 `idf.py erase-flash`。

---

#### 问题 4：设备路径 `/dev/video0` vs `/dev/video40`

```
ESP_VIDEO_USB_UVC_NAME(0) = "/dev/video4" "0" = "/dev/video40"
```

代码写死 `/dev/video0`，实际 USB UVC 设备编号从 40 开始。

**解决**：`#define CAMERA_DEV_PATH ESP_VIDEO_USB_UVC_NAME(0)`

---

#### 问题 5：`VIDIOC_S_FMT` 被 UVC 驱动拒绝（最终关键问题）

**现象**：
```
VIDIOC_S_FMT failed for size=1280x720 format=JPEG (errno=22 EINVAL)
camera_open() → ESP_ERR_NOT_SUPPORTED
```

**数据流分析**：

```
camera_open() 执行的 V4L2 ioctl 序列:

 ① open("/dev/video40")           ✅
 ② VIDIOC_G_FMT                   ✅ → 1280x720, JPEG, sizeimage=0
 ③ camera_apply_internal_sizeimage  → 计算 buffer 大小 ≈ 307KB
 ④ VIDIOC_S_FMT                   ❌ EINVAL —— 驱动拒绝！
```

**根因**：该 USB 摄像头 UVC 驱动不接受外部指定的 `sizeimage`，内部自己管理 buffer 大小。`camera_hal.c` 原逻辑是：`G_FMT` → 算 `sizeimage` → `S_FMT` 写回。驱动收到非零 `sizeimage` 即拒绝。**这不是分辨率或格式问题**（640x480、MJPEG 均试过，同样失败）。

**解决**：修改 `camera_hal.c` 的 `camera_open_locked()`——`S_FMT` 失败不再 abort，fallback 到 `G_FMT` 获取的值继续：

```c
// 修改后：S_FMT 失败 → fallback 到 G_FMT
if (ioctl(s_camera.fd, VIDIOC_S_FMT, &format) != 0) {
    ESP_LOGW(TAG, "S_FMT failed; falling back to G_FMT");
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    memset(&format.fmt, 0, sizeof(format.fmt));
    if (ioctl(s_camera.fd, VIDIOC_G_FMT, &format) != 0) {
        // 彻底失败
        camera_close_locked();
        return ESP_ERR_NOT_SUPPORTED;
    }
}
// 用 G_FMT 的值继续 REQBUFS → mmap → QBUF → STREAMON
```

---

### 10.3 正常工作的完整数据流

```
浏览器 <img src="/camera/mjpeg">
       │
       ▼ HTTP GET /camera/mjpeg
http_server_camera_api.c
  └─ camera_ensure_open()         // 懒加载，首次调用时初始化
       └─ camera_open("/dev/video40", NULL)
            ├─ open()             ✅ 打开 V4L2 设备
            ├─ G_FMT              ✅ 获取原生格式
            ├─ S_FMT              ⚠️ fallback → G_FMT
            ├─ REQBUFS            ✅ 分配 3 个 MMAP buffer
            ├─ mmap × 3           ✅ 映射到用户空间
            ├─ QBUF × 3           ✅ 入队
            └─ STREAMON           ✅ 开启采集
       │
       ▼ 循环每帧
  camera_capture_frame(timeout=2000ms)
       ├─ DQBUF                  ← 出队一帧已压缩 JPEG
       ├─ httpd_resp_send_chunk("--FRAME\r\nContent-Type: image/jpeg\r\n...")
       ├─ httpd_resp_send_chunk(JPEG data)
       ├─ release_frame → QBUF   ← 归还 buffer
       └─ vTaskDelay(33ms)       ≈ 30fps
```

> **关键**：USB UVC 输出就是已压缩 JPEG，无需软件编解码，直接 HTTP chunk 发送。

---

### 10.4 兼容性

| 场景 | 行为 |
|------|------|
| USB 摄像头已插入 | `camera_open()` 成功，MJPEG 流正常 |
| USB 摄像头未插入 | G_FMT 失败，返回 503 + 英文提示 |
| 摄像头已打开，再次请求 | 跳过 init，直接复用 |
| 无摄像头的板子 | 页面正常显示，status API 返回 `available:false` |

---

### 10.5 验证命令

```bash
# 状态
curl http://192.168.0.155/api/camera/status
# → {"available":true,"width":1280,"height":720,"pixel_format":"JPEG"}

# 单帧
curl http://192.168.0.155/api/camera/snapshot -o snapshot.jpg

# MJPEG 流（浏览器或 VLC 打开）
# http://192.168.0.155/camera/mjpeg
```
