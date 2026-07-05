/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "camera_hal.h"
#include "esp_check.h"
#include "esp_board_manager_includes.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#define CAMERA_DEV_PATH        ESP_VIDEO_USB_UVC_NAME(0)
#define CAMERA_FRAME_TIMEOUT_MS 2000
#define MJPEG_FRAME_DELAY_MS   33
#define MJPEG_CLOSE_WAIT_MS    3000
#define AUDIO_TEST_SAMPLE_RATE  16000
#define AUDIO_TEST_CHANNELS     1
#define AUDIO_TEST_BITS         16
#define AUDIO_TEST_DURATION_SEC 10
#define AUDIO_TEST_CHUNK_BYTES  4096
#define AUDIO_TEST_OUT_VOL      70
#define AUDIO_TEST_IN_GAIN_DB   24
#define AUDIO_TEST_TONE_AMPLITUDE 0.55f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "http_camera";
static bool s_camera_open_attempted = false;
static volatile bool s_mjpeg_stop_requested = false;
static volatile bool s_camera_closing = false;
static volatile bool s_audio_loopback_active = false;
static volatile int s_mjpeg_active_clients = 0;
static httpd_handle_t s_camera_stream_server = NULL;

static void camera_reset_open_attempt(void)
{
    s_camera_open_attempted = false;
}

static esp_err_t camera_wait_for_mjpeg_stop(int timeout_ms)
{
    int waited_ms = 0;

    s_mjpeg_stop_requested = true;
    while (s_mjpeg_active_clients > 0 && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }

    return s_mjpeg_active_clients == 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

// ── Full camera init ──────────────────────────────────────────────────

static bool camera_full_init(void)
{
    esp_err_t init_ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
    if (init_ret != ESP_OK) {
        ESP_LOGW(TAG, "Board camera init before video open failed: %s", esp_err_to_name(init_ret));
    }

    ESP_LOGI(TAG, "Attempting camera_open(%s)...", CAMERA_DEV_PATH);

    esp_err_t ret = camera_open(CAMERA_DEV_PATH, NULL);
    ESP_LOGI(TAG, "camera_open() = %d (%s)", ret, esp_err_to_name(ret));

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open camera. Is a USB camera connected to the OTG port?");
        camera_reset_open_attempt();
        return false;
    }

    camera_stream_info_t info = {0};
    if (camera_get_stream_info(&info) == ESP_OK) {
        ESP_LOGI(TAG, "Stream ready: %ux%u fmt=%s",
                 (unsigned)info.width, (unsigned)info.height, info.pixel_format_str);
    }

    return true;
}

// ── Ensure camera is opened (lazy init) ─────────────────────────────

static bool camera_ensure_open(void)
{
    if (camera_is_open()) {
        return true;
    }
    s_camera_open_attempted = true;

    ESP_LOGI(TAG, "Camera not open, performing full init...");
    bool ok = camera_full_init();
    if (ok) {
        ESP_LOGI(TAG, ">>> Camera ready <<<");
    } else {
        ESP_LOGE(TAG, ">>> Camera init FAILED <<<");
    }
    return ok;
}

// ── Snapshot handler ─────────────────────────────────────────────────

static esp_err_t camera_snapshot_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, ">>> snapshot handler ENTRY <<<");

    if (!camera_ensure_open()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Camera not available. Is a USB camera connected?");
    }

    uint8_t *frame_data = NULL;
    size_t frame_bytes = 0;

    esp_err_t ret = camera_capture_frame(CAMERA_FRAME_TIMEOUT_MS,
                                         &frame_data, &frame_bytes, NULL);
    ESP_LOGI(TAG, "capture_frame() = %d (%s), bytes=%u",
             ret, esp_err_to_name(ret), (unsigned)frame_bytes);

    if (ret != ESP_OK || !frame_data || frame_bytes == 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to capture camera frame");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    ret = httpd_resp_send(req, (const char *)frame_data, (ssize_t)frame_bytes);
    camera_release_frame(frame_data);
    return ret;
}

// ── MJPEG stream handler ─────────────────────────────────────────────

static esp_err_t camera_mjpeg_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, ">>> MJPEG handler ENTRY <<<");

    if (s_camera_closing) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Camera is closing; retry after close completes.");
    }

    s_mjpeg_stop_requested = false;
    s_mjpeg_active_clients++;

    if (!camera_ensure_open()) {
        s_mjpeg_active_clients--;
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Camera not available. Is a USB camera connected?");
    }

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=FRAME");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    int frame_count = 0;
    while (!s_mjpeg_stop_requested) {
        uint8_t *frame_data = NULL;
        size_t frame_bytes = 0;

        esp_err_t ret = camera_capture_frame(CAMERA_FRAME_TIMEOUT_MS,
                                             &frame_data, &frame_bytes, NULL);
        if (ret != ESP_OK || !frame_data || frame_bytes == 0) {
            if (frame_count < 3) {
                ESP_LOGW(TAG, "mjpeg: frame #%d FAILED: %s", frame_count, esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        frame_count++;
        if (frame_count <= 3) {
            ESP_LOGI(TAG, "mjpeg: frame #%d %u bytes", frame_count, (unsigned)frame_bytes);
        }

        char hdr[128];
        int hdr_len = snprintf(hdr, sizeof(hdr),
            "\r\n--FRAME\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n\r\n",
            (unsigned)frame_bytes);

        if (httpd_resp_send_chunk(req, hdr, hdr_len) != ESP_OK) {
            camera_release_frame(frame_data);
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)frame_data,
                                  (ssize_t)frame_bytes) != ESP_OK) {
            camera_release_frame(frame_data);
            break;
        }

        camera_release_frame(frame_data);
        vTaskDelay(pdMS_TO_TICKS(MJPEG_FRAME_DELAY_MS));
    }

    s_mjpeg_active_clients--;
    if (s_mjpeg_active_clients == 0) {
        s_mjpeg_stop_requested = false;
    }
    ESP_LOGI(TAG, "MJPEG disconnected, %d frames sent", frame_count);
    return ESP_OK;
}

static esp_err_t camera_mjpeg_redirect_handler(httpd_req_t *req)
{
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || host[0] == '\0') {
        strlcpy(host, "192.168.4.1", sizeof(host));
    }

    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
    }

    char location[96];
    snprintf(location, sizeof(location), "http://%s:%u/camera/mjpeg", host, HTTP_CAMERA_STREAM_PORT);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

// ── Status API ───────────────────────────────────────────────────────

static esp_err_t camera_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }

    bool available = camera_is_open();
    cJSON_AddBoolToObject(root, "available", available);
    cJSON_AddBoolToObject(root, "streaming", camera_is_streaming());
    cJSON_AddNumberToObject(root, "mjpeg_clients", s_mjpeg_active_clients);

    uint32_t borrowed_count = 0;
    if (camera_get_borrowed_count(&borrowed_count) == ESP_OK) {
        cJSON_AddNumberToObject(root, "borrowed_frames", borrowed_count);
    }

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

// ── Close handler ────────────────────────────────────────────────────

static esp_err_t camera_close_handler(httpd_req_t *req)
{
    s_camera_closing = true;
    esp_err_t err = camera_wait_for_mjpeg_stop(MJPEG_CLOSE_WAIT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Timed out waiting for MJPEG stream to stop");
    }

    if (camera_is_open()) {
        err = camera_close();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Camera close failed once: %s; waiting and retrying", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(250));
            err = camera_close();
        }
        if (err != ESP_OK) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            s_camera_closing = false;
            return httpd_resp_sendstr(req, "Camera is still busy; retry close.");
        }
        ESP_LOGI(TAG, "Camera closed by user request");
    }
    camera_reset_open_attempt();
    s_camera_closing = false;
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t camera_audio_open_codec(const char *device_name,
                                         bool is_output,
                                         dev_audio_codec_handles_t **out_handle)
{
    esp_err_t ret = esp_board_manager_init_device_by_name(device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio %s init failed: %s", device_name, esp_err_to_name(ret));
        return ret;
    }

    dev_audio_codec_handles_t *handle = NULL;
    ret = esp_board_manager_get_device_handle(device_name, (void **)&handle);
    if (ret != ESP_OK || handle == NULL || handle->codec_dev == NULL) {
        ESP_LOGE(TAG, "Audio %s handle missing", device_name);
        (void)esp_board_manager_deinit_device_by_name(device_name);
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_TEST_SAMPLE_RATE,
        .channel = AUDIO_TEST_CHANNELS,
        .bits_per_sample = AUDIO_TEST_BITS,
    };

    (void)esp_codec_dev_close(handle->codec_dev);
    ret = esp_codec_dev_open(handle->codec_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Audio %s open failed: %s", device_name, esp_err_to_name(ret));
        (void)esp_board_manager_deinit_device_by_name(device_name);
        return ESP_FAIL;
    }

    if (is_output) {
        ret = esp_codec_dev_set_out_vol(handle->codec_dev, AUDIO_TEST_OUT_VOL);
    } else {
        ret = esp_codec_dev_set_in_gain(handle->codec_dev, AUDIO_TEST_IN_GAIN_DB);
    }
    if (ret != ESP_CODEC_DEV_OK && ret != ESP_CODEC_DEV_NOT_SUPPORT) {
        ESP_LOGW(TAG, "Audio %s level setup failed: %s", device_name, esp_err_to_name(ret));
    }

    *out_handle = handle;
    return ESP_OK;
}

static esp_err_t camera_audio_play_tone(dev_audio_codec_handles_t *dac_handle,
                                        uint32_t freq_hz,
                                        uint32_t duration_ms)
{
    const uint32_t bytes_per_frame = AUDIO_TEST_CHANNELS * (AUDIO_TEST_BITS / 8);
    const uint32_t frames_per_chunk = AUDIO_TEST_CHUNK_BYTES / bytes_per_frame;
    const uint32_t total_frames = (AUDIO_TEST_SAMPLE_RATE * duration_ms) / 1000;
    uint8_t *tone_buf = (uint8_t *)malloc(frames_per_chunk * bytes_per_frame);
    if (!tone_buf) {
        return ESP_ERR_NO_MEM;
    }

    float phase = 0.0f;
    const float phase_step = 2.0f * (float)M_PI * (float)freq_hz / (float)AUDIO_TEST_SAMPLE_RATE;
    const float amplitude = 32767.0f * AUDIO_TEST_TONE_AMPLITUDE;
    uint32_t frames_written = 0;
    esp_err_t ret = ESP_OK;

    while (frames_written < total_frames) {
        uint32_t frames_this = total_frames - frames_written;
        if (frames_this > frames_per_chunk) {
            frames_this = frames_per_chunk;
        }
        int16_t *samples = (int16_t *)tone_buf;
        for (uint32_t i = 0; i < frames_this; ++i) {
            samples[i] = (int16_t)(sinf(phase) * amplitude);
            phase += phase_step;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }

        const int bytes = (int)(frames_this * bytes_per_frame);
        ret = esp_codec_dev_write(dac_handle->codec_dev, tone_buf, bytes);
        if (ret != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
            break;
        }
        frames_written += frames_this;
    }

    free(tone_buf);
    return ret;
}

static esp_err_t camera_audio_record_then_play(size_t *out_recorded_bytes)
{
    const size_t total_bytes = AUDIO_TEST_SAMPLE_RATE *
                               AUDIO_TEST_CHANNELS *
                               (AUDIO_TEST_BITS / 8) *
                               AUDIO_TEST_DURATION_SEC;
    uint8_t *recording = NULL;
    dev_audio_codec_handles_t *adc_handle = NULL;
    dev_audio_codec_handles_t *dac_handle = NULL;
    bool adc_inited = false;
    bool dac_inited = false;
    bool adc_opened = false;
    bool dac_opened = false;
    esp_err_t ret = ESP_OK;

    recording = (uint8_t *)heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recording) {
        recording = (uint8_t *)malloc(total_bytes);
    }
    if (!recording) {
        ESP_LOGE(TAG, "Audio recording buffer alloc failed: %u bytes", (unsigned)total_bytes);
        return ESP_ERR_NO_MEM;
    }

    ret = camera_audio_open_codec(ESP_BOARD_DEVICE_NAME_AUDIO_ADC, false, &adc_handle);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    adc_inited = true;
    adc_opened = true;

    ret = camera_audio_open_codec(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, true, &dac_handle);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    dac_inited = true;
    dac_opened = true;

    ESP_LOGI(TAG, "Audio start prompt tone");
    ret = camera_audio_play_tone(dac_handle, 523, 180);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    ret = camera_audio_play_tone(dac_handle, 659, 180);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    size_t recorded = 0;
    ESP_LOGI(TAG, "Audio recording %u seconds", AUDIO_TEST_DURATION_SEC);
    while (recorded < total_bytes) {
        size_t bytes = total_bytes - recorded;
        if (bytes > AUDIO_TEST_CHUNK_BYTES) {
            bytes = AUDIO_TEST_CHUNK_BYTES;
        }
        ret = esp_codec_dev_read(adc_handle->codec_dev, recording + recorded, (int)bytes);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Audio read failed after %u bytes", (unsigned)recorded);
            ret = ESP_FAIL;
            goto cleanup;
        }
        recorded += bytes;
    }
    ESP_LOGI(TAG, "Audio recorded %u bytes", (unsigned)recorded);

    ret = camera_audio_play_tone(dac_handle, 784, 500);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Audio playing recorded data");
    size_t played = 0;
    while (played < recorded) {
        size_t bytes = recorded - played;
        if (bytes > AUDIO_TEST_CHUNK_BYTES) {
            bytes = AUDIO_TEST_CHUNK_BYTES;
        }
        ret = esp_codec_dev_write(dac_handle->codec_dev, recording + played, (int)bytes);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Audio write failed after %u bytes", (unsigned)played);
            ret = ESP_FAIL;
            goto cleanup;
        }
        played += bytes;
    }
    ESP_LOGI(TAG, "Audio played %u bytes", (unsigned)played);
    if (out_recorded_bytes) {
        *out_recorded_bytes = recorded;
    }
    ret = ESP_OK;

cleanup:
    if (dac_opened && dac_handle && dac_handle->codec_dev) {
        (void)esp_codec_dev_close(dac_handle->codec_dev);
    }
    if (adc_opened && adc_handle && adc_handle->codec_dev) {
        (void)esp_codec_dev_close(adc_handle->codec_dev);
    }
    if (dac_inited) {
        (void)esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_AUDIO_DAC);
    }
    if (adc_inited) {
        (void)esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_AUDIO_ADC);
    }
    free(recording);
    return ret;
}

static esp_err_t camera_audio_loopback_handler(httpd_req_t *req)
{
    if (s_audio_loopback_active) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Audio test is already running.");
    }
    if (s_mjpeg_active_clients > 0 || camera_is_streaming()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Stop video before running audio test.");
    }
    if (camera_is_open()) {
        (void)camera_close();
        camera_reset_open_attempt();
    }
    esp_err_t cam_deinit = esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
    if (cam_deinit != ESP_OK) {
        ESP_LOGW(TAG, "Board camera deinit before audio test returned: %s", esp_err_to_name(cam_deinit));
    } else {
        ESP_LOGI(TAG, "Board camera deinitialized before audio test");
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    s_audio_loopback_active = true;
    size_t recorded_bytes = 0;
    esp_err_t ret = camera_audio_record_then_play(&recorded_bytes);
    s_audio_loopback_active = false;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Audio Class loopback failed: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "USB Audio Class loopback failed. Check the USB camera microphone/speaker, stop video streaming, then replug the USB 3-in-1 camera if needed.");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "recorded_bytes", (double)recorded_bytes);
    cJSON_AddNumberToObject(root, "sample_rate", AUDIO_TEST_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "channels", AUDIO_TEST_CHANNELS);
    cJSON_AddNumberToObject(root, "bits_per_sample", AUDIO_TEST_BITS);
    cJSON_AddNumberToObject(root, "duration_seconds", AUDIO_TEST_DURATION_SEC);
    http_server_json_add_string(root, "message", "Recorded audio has been played back.");
    return http_server_send_json_response(req, root);
}

static esp_err_t http_server_start_camera_stream_server(void)
{
    if (s_camera_stream_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_CAMERA_STREAM_PORT;
    config.ctrl_port = HTTP_CAMERA_STREAM_CTRL_PORT;
    config.max_uri_handlers = 2;
    config.stack_size = 8192;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_camera_stream_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start camera stream server on port %u: %s",
                 HTTP_CAMERA_STREAM_PORT, esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t stream_handler = {
        .uri = "/camera/mjpeg",
        .method = HTTP_GET,
        .handler = camera_mjpeg_handler,
    };
    err = httpd_register_uri_handler(s_camera_stream_server, &stream_handler);
    if (err != ESP_OK) {
        (void)httpd_stop(s_camera_stream_server);
        s_camera_stream_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Camera stream server listening on port %u", HTTP_CAMERA_STREAM_PORT);
    return ESP_OK;
}

esp_err_t http_server_stop_camera_stream_server(void)
{
    if (!s_camera_stream_server) {
        return ESP_OK;
    }

    s_camera_closing = true;
    (void)camera_wait_for_mjpeg_stop(MJPEG_CLOSE_WAIT_MS);
    esp_err_t err = httpd_stop(s_camera_stream_server);
    if (err == ESP_OK) {
        s_camera_stream_server = NULL;
    }
    if (camera_is_open()) {
        (void)camera_close();
    }
    camera_reset_open_attempt();
    s_camera_closing = false;
    return err;
}

// ── Route registration ───────────────────────────────────────────────

esp_err_t http_server_register_camera_routes(httpd_handle_t server)
{
    ESP_LOGI(TAG, "=== Camera routes registering ===");

    ESP_RETURN_ON_ERROR(http_server_start_camera_stream_server(), TAG,
                        "Failed to start camera stream server");

    const httpd_uri_t handlers[] = {
        { .uri = "/api/camera/snapshot", .method = HTTP_GET,
          .handler = camera_snapshot_handler },
        { .uri = "/api/camera/status",   .method = HTTP_GET,
          .handler = camera_status_handler },
        { .uri = "/api/camera/close",    .method = HTTP_POST,
          .handler = camera_close_handler },
        { .uri = "/api/camera/audio/loopback", .method = HTTP_POST,
          .handler = camera_audio_loopback_handler },
        { .uri = "/camera/mjpeg",        .method = HTTP_GET,
          .handler = camera_mjpeg_redirect_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", handlers[i].uri, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Registered: %s", handlers[i].uri);
    }
    return ESP_OK;
}
