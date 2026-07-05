/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "driver/mcpwm_prelude.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "dht.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define SERVO_MAX_CHANNELS     4
#define SERVO_DEFAULT_FREQ_HZ  50
#define SERVO_MIN_US           500
#define SERVO_MAX_US           2500
#define SERVO_PERIOD_US        (1000000 / SERVO_DEFAULT_FREQ_HZ)  // 20000us

#define LED_DEFAULT_GPIO       38
#define DHT11_DEFAULT_GPIO     40

#define STEPPER_DEFAULT_IN1    46
#define STEPPER_DEFAULT_IN2    17
#define STEPPER_DEFAULT_IN3    18
#define STEPPER_DEFAULT_IN4    21
#define STEPPER_DEFAULT_RPM    8.0f
#define STEPPER_STEPS_PER_REV  4096
#define STEPPER_DEFAULT_STEPS  128
#define STEPPER_MAX_STEPS      16384
#define STEPPER_MIN_RPM        1.0f
#define STEPPER_MAX_RPM        15.0f

static const char *TAG = "http_devices";

static const int s_board_available_gpios[] = {
    1, 8, 10, 11, 12, 13, 14, 17, 18, 21,
    33, 34, 39, 40, 41, 42, 47, 48,
};

static const int s_board_reserved_gpios[] = {
    0,
    2, 3,
    4, 5, 6, 7, 15, 16,
    9,
    19, 20,
    26, 27, 28, 29, 30, 31, 32,
    35, 36, 37,
    LED_DEFAULT_GPIO,
    43, 44,
    45, 46,
};

static const int s_stepper_gpios[] = {
    1, 8, 10, 11, 12, 13, 14, 17, 18, 21,
    33, 34, 39, 40, 41, 42, 46, 47, 48,
};

static int s_dht11_gpio = DHT11_DEFAULT_GPIO;

static bool gpio_in_list(const int *list, size_t count, int gpio)
{
    for (size_t i = 0; i < count; ++i) {
        if (list[i] == gpio) {
            return true;
        }
    }
    return false;
}

static bool gpio_is_board_available(int gpio)
{
    return gpio_in_list(s_board_available_gpios,
                        sizeof(s_board_available_gpios) / sizeof(s_board_available_gpios[0]),
                        gpio);
}

static bool gpio_is_servo_available(int gpio)
{
    return gpio_is_board_available(gpio) && gpio != s_dht11_gpio;
}

static bool gpio_is_stepper_available(int gpio)
{
    return gpio_in_list(s_stepper_gpios,
                        sizeof(s_stepper_gpios) / sizeof(s_stepper_gpios[0]),
                        gpio);
}

static void json_add_gpio_array(cJSON *root, const char *name, const int *list, size_t count, int exclude_gpio)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (list[i] == exclude_gpio) {
            continue;
        }
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(list[i]));
    }
    cJSON_AddItemToObject(root, name, arr);
}

// ── Servo ──────────────────────────────────────────────────────────────

typedef struct {
    int gpio;
    int angle;              // 0-180
    bool enabled;
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t cmpr;
    mcpwm_gen_handle_t gen;
} servo_ch_t;

static servo_ch_t s_servo[SERVO_MAX_CHANNELS];

static void servo_deinit_channel(int ch)
{
    if (ch < 0 || ch >= SERVO_MAX_CHANNELS) {
        return;
    }

    servo_ch_t *s = &s_servo[ch];
    if (!s->timer) {
        return;
    }

    if (s->enabled) {
        mcpwm_timer_start_stop(s->timer, MCPWM_TIMER_STOP_EMPTY);
        mcpwm_timer_disable(s->timer);
    }
    if (s->gen) {
        mcpwm_del_generator(s->gen);
    }
    if (s->cmpr) {
        mcpwm_del_comparator(s->cmpr);
    }
    if (s->oper) {
        mcpwm_del_operator(s->oper);
    }
    if (s->timer) {
        mcpwm_del_timer(s->timer);
    }
    gpio_set_level(s->gpio, 0);
    memset(s, 0, sizeof(*s));
}

static esp_err_t servo_init_channel(int ch, int gpio)
{
    if (ch < 0 || ch >= SERVO_MAX_CHANNELS) return ESP_ERR_INVALID_ARG;
    servo_ch_t *s = &s_servo[ch];

    if (s->timer) {
        if (s->gpio == gpio) {
            return ESP_OK;
        }
        servo_deinit_channel(ch);
    }

    s->gpio = gpio;
    s->angle = 90;
    s->enabled = false;

    // Create timer
    mcpwm_timer_config_t timer_cfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,  // 1MHz → 1us per tick
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = SERVO_PERIOD_US,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_timer(&timer_cfg, &s->timer), TAG, "new timer ch%d", ch);

    // Create operator
    mcpwm_operator_config_t oper_cfg = { .group_id = 0 };
    ESP_RETURN_ON_ERROR(mcpwm_new_operator(&oper_cfg, &s->oper), TAG, "new oper ch%d", ch);
    ESP_RETURN_ON_ERROR(mcpwm_operator_connect_timer(s->oper, s->timer), TAG, "connect ch%d", ch);

    // Create comparator & generator
    mcpwm_comparator_config_t cmpr_cfg = { .flags.update_cmp_on_tez = true };
    ESP_RETURN_ON_ERROR(mcpwm_new_comparator(s->oper, &cmpr_cfg, &s->cmpr), TAG, "new cmpr ch%d", ch);

    mcpwm_generator_config_t gen_cfg = { .gen_gpio_num = gpio };
    ESP_RETURN_ON_ERROR(mcpwm_new_generator(s->oper, &gen_cfg, &s->gen), TAG, "new gen ch%d", ch);

    // Set action: go high on timer empty, go low on compare match
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_timer_event(s->gen,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)), TAG, "action ch%d", ch);
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_compare_event(s->gen,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s->cmpr, MCPWM_GEN_ACTION_LOW)), TAG, "cmp ch%d", ch);

    ESP_LOGI(TAG, "Servo ch%d initialized on GPIO %d", ch, gpio);
    return ESP_OK;
}

static void servo_set_angle_raw(int ch, int angle)
{
    if (ch < 0 || ch >= SERVO_MAX_CHANNELS) return;
    servo_ch_t *s = &s_servo[ch];
    if (!s->timer) return;

    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    s->angle = angle;

    // 0°→500us, 180°→2500us
    uint32_t pulse_us = SERVO_MIN_US + (uint32_t)(angle * (SERVO_MAX_US - SERVO_MIN_US) / 180);
    mcpwm_comparator_set_compare_value(s->cmpr, pulse_us);
}

static void servo_enable_raw(int ch, bool en)
{
    if (ch < 0 || ch >= SERVO_MAX_CHANNELS) return;
    servo_ch_t *s = &s_servo[ch];
    if (!s->timer) return;
    s->enabled = en;
    if (en) {
        mcpwm_timer_enable(s->timer);
        mcpwm_timer_start_stop(s->timer, MCPWM_TIMER_START_NO_STOP);
    } else {
        mcpwm_timer_start_stop(s->timer, MCPWM_TIMER_STOP_EMPTY);
        mcpwm_timer_disable(s->timer);
        gpio_set_level(s->gpio, 0);
    }
}

// ── LED Strip ──────────────────────────────────────────────────────────

static led_strip_handle_t s_led_strip;
static int s_led_count = 8;
static int s_led_brightness = 80;

static void led_apply_brightness(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)((*r * s_led_brightness) / 100);
    *g = (uint8_t)((*g * s_led_brightness) / 100);
    *b = (uint8_t)((*b * s_led_brightness) / 100);
}

static esp_err_t led_ensure_init(void)
{
    if (s_led_strip) return ESP_OK;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_DEFAULT_GPIO,
        .max_leds = s_led_count,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip),
                        TAG, "led_strip init");
    ESP_LOGI(TAG, "LED strip initialized: %d LEDs on GPIO %d", s_led_count, LED_DEFAULT_GPIO);
    return ESP_OK;
}

static void led_set_solid(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led_strip) return;
    led_apply_brightness(&r, &g, &b);
    for (int i = 0; i < s_led_count; i++) {
        led_strip_set_pixel(s_led_strip, i, r, g, b);
    }
    led_strip_refresh(s_led_strip);
}

static void led_clear(void)
{
    if (!s_led_strip) return;
    led_strip_clear(s_led_strip);
}

// Stepper

typedef struct {
    int in1;
    int in2;
    int in3;
    int in4;
    int steps_per_rev;
    float rpm;
    bool release_after_move;
} stepper_config_t;

static stepper_config_t s_stepper = {
    .in1 = STEPPER_DEFAULT_IN1,
    .in2 = STEPPER_DEFAULT_IN2,
    .in3 = STEPPER_DEFAULT_IN3,
    .in4 = STEPPER_DEFAULT_IN4,
    .steps_per_rev = STEPPER_STEPS_PER_REV,
    .rpm = STEPPER_DEFAULT_RPM,
    .release_after_move = true,
};

static const uint8_t s_stepper_half_steps[8][4] = {
    { 1, 0, 0, 0 },
    { 1, 1, 0, 0 },
    { 0, 1, 0, 0 },
    { 0, 1, 1, 0 },
    { 0, 0, 1, 0 },
    { 0, 0, 1, 1 },
    { 0, 0, 0, 1 },
    { 1, 0, 0, 1 },
};

static int json_int_or_default(cJSON *body, const char *name, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(body, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static float json_float_or_default(cJSON *body, const char *name, float fallback)
{
    cJSON *item = cJSON_GetObjectItem(body, name);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : fallback;
}

static bool json_bool_or_default(cJSON *body, const char *name, bool fallback)
{
    cJSON *item = cJSON_GetObjectItem(body, name);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static bool stepper_pins_are_valid(const stepper_config_t *cfg)
{
    const int pins[] = { cfg->in1, cfg->in2, cfg->in3, cfg->in4 };

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (!gpio_is_stepper_available(pins[i])) {
            return false;
        }
        for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j) {
            if (pins[i] == pins[j]) {
                return false;
            }
        }
    }
    return true;
}

static esp_err_t stepper_apply_config(const stepper_config_t *cfg)
{
    const int pins[] = { cfg->in1, cfg->in2, cfg->in3, cfg->in4 };

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "stepper gpio config");
        gpio_set_level(pins[i], 0);
    }
    return ESP_OK;
}

static void stepper_write_phase(const stepper_config_t *cfg, const uint8_t phase[4])
{
    gpio_set_level(cfg->in1, phase[0]);
    gpio_set_level(cfg->in2, phase[1]);
    gpio_set_level(cfg->in3, phase[2]);
    gpio_set_level(cfg->in4, phase[3]);
}

static void stepper_release(const stepper_config_t *cfg)
{
    const uint8_t off[4] = { 0, 0, 0, 0 };
    stepper_write_phase(cfg, off);
}

static bool stepper_direction_is_ccw(const char *direction)
{
    if (!direction) {
        return false;
    }
    return strcmp(direction, "ccw") == 0 ||
           strcmp(direction, "counterclockwise") == 0 ||
           strcmp(direction, "counter-clockwise") == 0 ||
           strcmp(direction, "anticlockwise") == 0 ||
           strcmp(direction, "reverse") == 0 ||
           strcmp(direction, "backward") == 0;
}

static esp_err_t stepper_run_move(const stepper_config_t *cfg, int steps, const char *direction)
{
    bool ccw = stepper_direction_is_ccw(direction);
    uint32_t delay_us = (uint32_t)((60000000.0f / ((float)cfg->steps_per_rev * cfg->rpm)) + 0.5f);

    ESP_RETURN_ON_ERROR(stepper_apply_config(cfg), TAG, "stepper apply config");

    for (int i = 0; i < steps; ++i) {
        int offset = i % 8;
        int index = ccw ? (7 - offset) : offset;
        stepper_write_phase(cfg, s_stepper_half_steps[index]);
        esp_rom_delay_us(delay_us);
        if ((i + 1) % 64 == 0) {
            vTaskDelay(1);
        }
    }

    if (cfg->release_after_move) {
        stepper_release(cfg);
    }
    return ESP_OK;
}

// ── Handlers ───────────────────────────────────────────────────────────

// ── GET /api/servo/status
static esp_err_t servo_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    cJSON *channels = cJSON_CreateArray();
    for (int i = 0; i < SERVO_MAX_CHANNELS; i++) {
        servo_ch_t *s = &s_servo[i];
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddNumberToObject(ch, "channel", i);
        cJSON_AddNumberToObject(ch, "gpio", s->gpio);
        cJSON_AddNumberToObject(ch, "angle", s->angle);
        cJSON_AddBoolToObject(ch, "enabled", s->enabled);
        cJSON_AddBoolToObject(ch, "active", s->timer != NULL);
        cJSON_AddItemToArray(channels, ch);
    }
    cJSON_AddItemToObject(root, "channels", channels);
    cJSON_AddNumberToObject(root, "frequency", SERVO_DEFAULT_FREQ_HZ);
    return http_server_send_json_response(req, root);
}

// ── POST /api/servo/config
static esp_err_t servo_config_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    cJSON *j_ch = cJSON_GetObjectItem(body, "channel");
    cJSON *j_gpio = cJSON_GetObjectItem(body, "gpio");
    int ch = j_ch ? j_ch->valueint : -1;
    int gpio = j_gpio ? j_gpio->valueint : -1;
    cJSON_Delete(body);

    if (ch < 0 || ch >= SERVO_MAX_CHANNELS || gpio <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel or gpio");
    }
    if (!gpio_is_servo_available(gpio)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "GPIO is reserved by this board");
    }

    err = servo_init_channel(ch, gpio);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Servo init failed");
    }
    return httpd_resp_sendstr(req, "OK");
}

// ── POST /api/servo/angle
static esp_err_t servo_angle_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    cJSON *j_ch = cJSON_GetObjectItem(body, "channel");
    cJSON *j_angle = cJSON_GetObjectItem(body, "angle");
    int ch = j_ch ? j_ch->valueint : -1;
    int angle = j_angle ? j_angle->valueint : 90;
    cJSON_Delete(body);

    if (ch < 0 || ch >= SERVO_MAX_CHANNELS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel");
    }
    servo_set_angle_raw(ch, angle);
    return httpd_resp_sendstr(req, "OK");
}

// ── POST /api/servo/enable
static esp_err_t servo_enable_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    cJSON *j_ch = cJSON_GetObjectItem(body, "channel");
    cJSON *j_en = cJSON_GetObjectItem(body, "enabled");
    int ch = j_ch ? j_ch->valueint : -1;
    bool en = j_en ? cJSON_IsTrue(j_en) : false;
    cJSON_Delete(body);

    if (ch < 0 || ch >= SERVO_MAX_CHANNELS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel");
    }
    servo_enable_raw(ch, en);
    ESP_LOGI(TAG, "Servo ch%d %s", ch, en ? "ON" : "OFF");
    return httpd_resp_sendstr(req, "OK");
}

// GET /api/stepper/status
static esp_err_t stepper_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    cJSON_AddNumberToObject(root, "in1", s_stepper.in1);
    cJSON_AddNumberToObject(root, "in2", s_stepper.in2);
    cJSON_AddNumberToObject(root, "in3", s_stepper.in3);
    cJSON_AddNumberToObject(root, "in4", s_stepper.in4);
    cJSON_AddNumberToObject(root, "steps_per_rev", s_stepper.steps_per_rev);
    cJSON_AddNumberToObject(root, "rpm", s_stepper.rpm);
    cJSON_AddBoolToObject(root, "release_after_move", s_stepper.release_after_move);
    json_add_gpio_array(root, "gpio_options", s_stepper_gpios,
                        sizeof(s_stepper_gpios) / sizeof(s_stepper_gpios[0]), -1);
    return http_server_send_json_response(req, root);
}

// POST /api/stepper/config
static esp_err_t stepper_config_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    stepper_config_t next = s_stepper;
    next.in1 = json_int_or_default(body, "in1", next.in1);
    next.in2 = json_int_or_default(body, "in2", next.in2);
    next.in3 = json_int_or_default(body, "in3", next.in3);
    next.in4 = json_int_or_default(body, "in4", next.in4);
    next.steps_per_rev = json_int_or_default(body, "steps_per_rev", next.steps_per_rev);
    next.rpm = json_float_or_default(body, "rpm", next.rpm);
    next.release_after_move = json_bool_or_default(body, "release_after_move", next.release_after_move);
    cJSON_Delete(body);

    if (!stepper_pins_are_valid(&next)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or duplicate stepper GPIO");
    }
    if (next.steps_per_rev <= 0 || next.steps_per_rev > STEPPER_MAX_STEPS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid steps_per_rev");
    }
    if (next.rpm < STEPPER_MIN_RPM || next.rpm > STEPPER_MAX_RPM) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rpm");
    }

    s_stepper = next;
    err = stepper_apply_config(&s_stepper);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stepper GPIO init failed");
    }
    ESP_LOGI(TAG, "Stepper config IN1=%d IN2=%d IN3=%d IN4=%d rpm=%.1f",
             s_stepper.in1, s_stepper.in2, s_stepper.in3, s_stepper.in4, s_stepper.rpm);
    return httpd_resp_sendstr(req, "OK");
}

// POST /api/stepper/move
static esp_err_t stepper_move_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    stepper_config_t cfg = s_stepper;
    cfg.in1 = json_int_or_default(body, "in1", cfg.in1);
    cfg.in2 = json_int_or_default(body, "in2", cfg.in2);
    cfg.in3 = json_int_or_default(body, "in3", cfg.in3);
    cfg.in4 = json_int_or_default(body, "in4", cfg.in4);
    cfg.steps_per_rev = json_int_or_default(body, "steps_per_rev", cfg.steps_per_rev);
    cfg.rpm = json_float_or_default(body, "rpm", cfg.rpm);
    cfg.release_after_move = json_bool_or_default(body, "release_after_move", cfg.release_after_move);

    char direction_buf[24] = "cw";
    const char *direction = direction_buf;
    cJSON *j_direction = cJSON_GetObjectItem(body, "direction");
    if (cJSON_IsString(j_direction) && j_direction->valuestring) {
        strlcpy(direction_buf, j_direction->valuestring, sizeof(direction_buf));
    }

    int steps = json_int_or_default(body, "steps", 0);
    cJSON *j_degrees = cJSON_GetObjectItem(body, "degrees");
    cJSON *j_revolutions = cJSON_GetObjectItem(body, "revolutions");
    if (steps == 0 && cJSON_IsNumber(j_degrees)) {
        double raw_steps = (j_degrees->valuedouble / 360.0) * cfg.steps_per_rev;
        steps = (int)(raw_steps >= 0 ? raw_steps + 0.5 : raw_steps - 0.5);
    }
    if (steps == 0 && cJSON_IsNumber(j_revolutions)) {
        double raw_steps = j_revolutions->valuedouble * cfg.steps_per_rev;
        steps = (int)(raw_steps >= 0 ? raw_steps + 0.5 : raw_steps - 0.5);
    }
    if (steps == 0) {
        steps = STEPPER_DEFAULT_STEPS;
    }
    cJSON_Delete(body);

    if (steps < 0) {
        strlcpy(direction_buf, stepper_direction_is_ccw(direction) ? "cw" : "ccw", sizeof(direction_buf));
        steps = -steps;
    }
    if (steps > STEPPER_MAX_STEPS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too many steps");
    }
    if (!stepper_pins_are_valid(&cfg)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or duplicate stepper GPIO");
    }
    if (cfg.steps_per_rev <= 0 || cfg.steps_per_rev > STEPPER_MAX_STEPS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid steps_per_rev");
    }
    if (cfg.rpm < STEPPER_MIN_RPM || cfg.rpm > STEPPER_MAX_RPM) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rpm");
    }

    s_stepper = cfg;
    err = stepper_run_move(&s_stepper, steps, direction);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stepper move failed");
    }

    ESP_LOGI(TAG, "Stepper moved %d half-steps direction=%s IN=%d,%d,%d,%d rpm=%.1f",
             steps, direction, s_stepper.in1, s_stepper.in2, s_stepper.in3, s_stepper.in4, s_stepper.rpm);
    return httpd_resp_sendstr(req, "OK");
}

// POST /api/stepper/release
static esp_err_t stepper_release_handler(httpd_req_t *req)
{
    (void)req;
    esp_err_t err = stepper_apply_config(&s_stepper);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stepper GPIO init failed");
    }
    stepper_release(&s_stepper);
    ESP_LOGI(TAG, "Stepper released IN=%d,%d,%d,%d", s_stepper.in1, s_stepper.in2, s_stepper.in3, s_stepper.in4);
    return httpd_resp_sendstr(req, "OK");
}

// ── GET /api/led/status
static esp_err_t led_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    cJSON_AddBoolToObject(root, "active", s_led_strip != NULL);
    cJSON_AddNumberToObject(root, "led_count", s_led_count);
    cJSON_AddNumberToObject(root, "brightness", s_led_brightness);
    cJSON_AddNumberToObject(root, "gpio", LED_DEFAULT_GPIO);
    return http_server_send_json_response(req, root);
}

// ── POST /api/led/config
static esp_err_t led_config_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    cJSON *j_count = cJSON_GetObjectItem(body, "led_count");
    int count = j_count ? j_count->valueint : 0;
    cJSON_Delete(body);

    if (count > 0 && count <= 64) {
        s_led_count = count;
    }
    // Re-init with new count
    if (s_led_strip) {
        led_strip_del(s_led_strip);
        s_led_strip = NULL;
    }
    err = led_ensure_init();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LED init failed");
    }
    return httpd_resp_sendstr(req, "OK");
}

// ── POST /api/led/solid
static esp_err_t led_solid_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    cJSON *j_color = cJSON_GetObjectItem(body, "color");
    cJSON *j_brightness = cJSON_GetObjectItem(body, "brightness");
    char color[16] = "#FFFFFF";
    int brightness = s_led_brightness;
    if (j_color && cJSON_IsString(j_color)) {
        strlcpy(color, j_color->valuestring, sizeof(color));
    }
    if (j_brightness) brightness = j_brightness->valueint;
    cJSON_Delete(body);

    if (brightness >= 0 && brightness <= 100) s_led_brightness = brightness;

    // Parse hex color #RRGGBB
    uint32_t hex = 0xFFFFFF;
    if (color[0] == '#' && strlen(color) >= 7) {
        hex = (uint32_t)strtol(color + 1, NULL, 16);
    }
    uint8_t r = (hex >> 16) & 0xFF;
    uint8_t g = (hex >> 8) & 0xFF;
    uint8_t b = hex & 0xFF;

    led_ensure_init();
    led_set_solid(r, g, b);
    ESP_LOGI(TAG, "LED solid: #%02X%02X%02X brightness=%d%%", r, g, b, s_led_brightness);
    return httpd_resp_sendstr(req, "OK");
}

// ── POST /api/led/off
static esp_err_t led_off_handler(httpd_req_t *req)
{
    (void)req;
    led_clear();
    ESP_LOGI(TAG, "LED off");
    return httpd_resp_sendstr(req, "OK");
}

// ── DHT11 ────────────────────────────────────────────────────────────

static esp_err_t devices_gpios_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    json_add_gpio_array(root, "available", s_board_available_gpios,
                        sizeof(s_board_available_gpios) / sizeof(s_board_available_gpios[0]), -1);
    json_add_gpio_array(root, "servo", s_board_available_gpios,
                        sizeof(s_board_available_gpios) / sizeof(s_board_available_gpios[0]), s_dht11_gpio);
    json_add_gpio_array(root, "stepper", s_stepper_gpios,
                        sizeof(s_stepper_gpios) / sizeof(s_stepper_gpios[0]), -1);
    json_add_gpio_array(root, "dht11", s_board_available_gpios,
                        sizeof(s_board_available_gpios) / sizeof(s_board_available_gpios[0]), -1);
    json_add_gpio_array(root, "reserved", s_board_reserved_gpios,
                        sizeof(s_board_reserved_gpios) / sizeof(s_board_reserved_gpios[0]), -1);

    cJSON *defaults = cJSON_CreateObject();
    if (defaults) {
        cJSON_AddNumberToObject(defaults, "led", LED_DEFAULT_GPIO);
        cJSON_AddNumberToObject(defaults, "dht11", DHT11_DEFAULT_GPIO);
        cJSON *stepper = cJSON_CreateObject();
        if (stepper) {
            cJSON_AddNumberToObject(stepper, "in1", STEPPER_DEFAULT_IN1);
            cJSON_AddNumberToObject(stepper, "in2", STEPPER_DEFAULT_IN2);
            cJSON_AddNumberToObject(stepper, "in3", STEPPER_DEFAULT_IN3);
            cJSON_AddNumberToObject(stepper, "in4", STEPPER_DEFAULT_IN4);
            cJSON_AddItemToObject(defaults, "stepper", stepper);
        }
        cJSON_AddItemToObject(root, "defaults", defaults);
    }

    return http_server_send_json_response(req, root);
}

static esp_err_t dht11_status_handler(httpd_req_t *req)
{
    if (!gpio_is_board_available(s_dht11_gpio)) {
        s_dht11_gpio = DHT11_DEFAULT_GPIO;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    cJSON_AddNumberToObject(root, "gpio", s_dht11_gpio);
    cJSON_AddNumberToObject(root, "default_gpio", DHT11_DEFAULT_GPIO);
    json_add_gpio_array(root, "gpio_options", s_board_available_gpios,
                        sizeof(s_board_available_gpios) / sizeof(s_board_available_gpios[0]), -1);
    return http_server_send_json_response(req, root);
}

static esp_err_t dht11_config_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    cJSON *j_gpio = cJSON_GetObjectItem(body, "gpio");
    int gpio = j_gpio ? j_gpio->valueint : -1;
    cJSON_Delete(body);

    if (!gpio_is_board_available(gpio)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "GPIO is reserved by this board");
    }

    s_dht11_gpio = gpio;
    ESP_LOGI(TAG, "DHT11 GPIO set to %d", s_dht11_gpio);
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t dht11_read_handler(httpd_req_t *req)
{
    (void)req;
    float temperature = 0, humidity = 0;
    esp_err_t ret = dht_read_float_data(DHT_TYPE_DHT11, s_dht11_gpio,
                                        &humidity, &temperature);
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    if (ret == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "temperature", temperature);
        cJSON_AddNumberToObject(root, "humidity", humidity);
        ESP_LOGI(TAG, "DHT11: T=%.1f°C H=%.1f%%", temperature, humidity);
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "DHT11 read failed. Check wiring and power.");
        ESP_LOGW(TAG, "DHT11 read failed: %s", esp_err_to_name(ret));
    }
    cJSON_AddNumberToObject(root, "gpio", s_dht11_gpio);
    return http_server_send_json_response(req, root);
}

// ── Route registration ─────────────────────────────────────────────────

esp_err_t http_server_register_devices_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/devices/gpios", .method = HTTP_GET,  .handler = devices_gpios_handler },
        // Servo
        { .uri = "/api/servo/status",  .method = HTTP_GET,  .handler = servo_status_handler },
        { .uri = "/api/servo/config",  .method = HTTP_POST, .handler = servo_config_handler },
        { .uri = "/api/servo/angle",   .method = HTTP_POST, .handler = servo_angle_handler },
        { .uri = "/api/servo/enable",  .method = HTTP_POST, .handler = servo_enable_handler },
        // Stepper
        { .uri = "/api/stepper/status",  .method = HTTP_GET,  .handler = stepper_status_handler },
        { .uri = "/api/stepper/config",  .method = HTTP_POST, .handler = stepper_config_handler },
        { .uri = "/api/stepper/move",    .method = HTTP_POST, .handler = stepper_move_handler },
        { .uri = "/api/stepper/release", .method = HTTP_POST, .handler = stepper_release_handler },
        // LED
        { .uri = "/api/led/status",    .method = HTTP_GET,  .handler = led_status_handler },
        { .uri = "/api/led/config",    .method = HTTP_POST, .handler = led_config_handler },
        { .uri = "/api/led/solid",     .method = HTTP_POST, .handler = led_solid_handler },
        { .uri = "/api/led/off",       .method = HTTP_POST, .handler = led_off_handler },
        // DHT11
        { .uri = "/api/dht11/status",   .method = HTTP_GET,  .handler = dht11_status_handler },
        { .uri = "/api/dht11/config",   .method = HTTP_POST, .handler = dht11_config_handler },
        { .uri = "/api/dht11/read",     .method = HTTP_GET,  .handler = dht11_read_handler },
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
