/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dht.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

static const char *TAG = "http_server_modules";

typedef struct {
    const char *id;
    const char *category;
    bool implemented;
} module_item_t;

static const module_item_t MODULES[] = {
    {"sensor_gpio_light_break", "sensors", true},
    {"sensor_gpio_mercury_tilt", "sensors", true},
    {"sensor_gpio_button", "sensors", true},
    {"sensor_gpio_reed_switch", "sensors", true},
    {"sensor_gpio_ir_tracking", "sensors", true},
    {"sensor_gpio_active_buzzer", "sensors", true},
    {"sensor_gpio_dual_led", "sensors", true},
    {"sensor_gpio_traffic_led", "sensors", true},
    {"sensor_adc_photoresistor", "sensors", true},
    {"sensor_adc_water_level", "sensors", true},
    {"sensor_joystick", "sensors", true},
    {"sensor_soil_moisture", "sensors", true},
    {"sensor_sound_level", "sensors", true},
    {"sensor_pwm_passive_buzzer", "sensors", true},
    {"sensor_onewire_ds18b20", "sensors", true},
    {"sensor_onewire_dht11", "sensors", true},
    {"sensor_onewire_rgb_led", "sensors", true},
    {"sensor_onewire_ws2812_ring", "sensors", true},
    {"sensor_i2c_mpu6050", "sensors", true},
    {"sensor_rotary_encoder", "sensors", true},
    {"sensor_hcsr04_ultrasonic", "sensors", true},
    {"actuator_pwm_servo", "actuators", true},
    {"actuator_multi_servo", "actuators", true},
    {"actuator_stepper_28byj48", "actuators", true},
    {"actuator_dc_motor", "actuators", false},
    {"actuator_motor_expansion", "actuators", false},
    {"media_usb_microphone", "media", false},
    {"media_usb_speaker", "media", false},
    {"media_usb_uvc_camera", "media", false},
    {"media_dvp_camera", "media", false},
    {"media_usb_composite_camera", "media", false},
};

static const int GPIO_PINS[] = {
    0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 17, 18, 21,
    26, 27, 28, 29, 30, 31, 32, 33, 34, 38, 39, 40, 41, 42, 45, 46, 47, 48,
};

static const int ADC_PINS[] = {
    1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 17, 18,
};

static adc_oneshot_unit_handle_t s_adc_units[SOC_ADC_PERIPH_NUM];
static TaskHandle_t s_buzzer_task;
static led_strip_handle_t s_module_strip;
static int s_module_strip_pin = -1;
static int s_module_strip_count = 0;

typedef struct {
    int clk_pin;
    int dt_pin;
    int button_pin;
    int last_state;
    int rest_state;
    int accumulator;
    int count;
    int last_delta;
    int clk_level;
    int dt_level;
    int button_level;
    int button_events;
    int last_button_level;
    bool initialized;
} rotary_encoder_state_t;

static rotary_encoder_state_t s_rotary_state = {
    .clk_pin = -1,
    .dt_pin = -1,
    .button_pin = -1,
    .button_level = 1,
    .last_button_level = 1,
};
static TaskHandle_t s_rotary_task;
static portMUX_TYPE s_rotary_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint16_t freq;
    uint16_t ms;
} tone_note_t;

typedef struct {
    const char *id;
    const char *name;
    const tone_note_t *notes;
    size_t count;
} melody_t;

static const tone_note_t MELODY_TWO_TIGERS[] = {
    {262, 260}, {294, 260}, {330, 260}, {262, 260},
    {262, 260}, {294, 260}, {330, 260}, {262, 260},
    {330, 260}, {349, 260}, {392, 520},
    {330, 260}, {349, 260}, {392, 520},
    {392, 180}, {440, 180}, {392, 180}, {349, 180}, {330, 260}, {262, 260},
    {392, 180}, {440, 180}, {392, 180}, {349, 180}, {330, 260}, {262, 260},
    {294, 260}, {196, 260}, {262, 520},
    {294, 260}, {196, 260}, {262, 520},
};

static const tone_note_t MELODY_TWINKLE[] = {
    {262, 360}, {262, 360}, {392, 360}, {392, 360}, {440, 360}, {440, 360}, {392, 720},
    {349, 360}, {349, 360}, {330, 360}, {330, 360}, {294, 360}, {294, 360}, {262, 720},
    {392, 360}, {392, 360}, {349, 360}, {349, 360}, {330, 360}, {330, 360}, {294, 720},
    {392, 360}, {392, 360}, {349, 360}, {349, 360}, {330, 360}, {330, 360}, {294, 720},
    {262, 360}, {262, 360}, {392, 360}, {392, 360}, {440, 360}, {440, 360}, {392, 720},
    {349, 360}, {349, 360}, {330, 360}, {330, 360}, {294, 360}, {294, 360}, {262, 720},
};

static const tone_note_t MELODY_RUYUAN_SHORT[] = {
    {392, 360}, {440, 360}, {523, 540}, {494, 240}, {440, 360},
    {392, 360}, {330, 360}, {392, 540}, {0, 120},
    {440, 360}, {494, 360}, {587, 540}, {523, 240}, {494, 360},
    {440, 360}, {392, 360}, {440, 720},
};

static const melody_t MELODIES[] = {
    {"two_tigers", "Two Tigers", MELODY_TWO_TIGERS, sizeof(MELODY_TWO_TIGERS) / sizeof(MELODY_TWO_TIGERS[0])},
    {"twinkle", "Twinkle Twinkle Little Star", MELODY_TWINKLE, sizeof(MELODY_TWINKLE) / sizeof(MELODY_TWINKLE[0])},
    {"ruyuan", "Ru Yuan short pattern", MELODY_RUYUAN_SHORT, sizeof(MELODY_RUYUAN_SHORT) / sizeof(MELODY_RUYUAN_SHORT[0])},
};

static const module_item_t *find_module(const char *id)
{
    for (size_t i = 0; i < sizeof(MODULES) / sizeof(MODULES[0]); ++i) {
        if (strcmp(MODULES[i].id, id) == 0) {
            return &MODULES[i];
        }
    }
    return NULL;
}

static bool module_id_is_valid(const char *id)
{
    if (!id || !id[0]) {
        return false;
    }
    for (const char *p = id; *p; ++p) {
        const bool ok = (*p >= 'a' && *p <= 'z') ||
                        (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9') ||
                        *p == '_' ||
                        *p == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void add_module_json(cJSON *parent, const module_item_t *item)
{
    cJSON_AddStringToObject(parent, "id", item->id);
    cJSON_AddStringToObject(parent, "category", item->category);
    cJSON_AddBoolToObject(parent, "implemented", item->implemented);
}

static cJSON *get_pins_object(cJSON *body)
{
    cJSON *pins = cJSON_GetObjectItemCaseSensitive(body, "pins");
    return cJSON_IsObject(pins) ? pins : NULL;
}

static bool get_pin_value(cJSON *body, const char *key, int *pin)
{
    cJSON *pins = get_pins_object(body);
    if (!pins) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(pins, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *pin = item->valueint;
    return true;
}

static bool get_param_int(cJSON *body, const char *key, int *value)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(body, "params");
    if (!cJSON_IsObject(params)) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *value = item->valueint;
    return true;
}

static const char *get_param_string(cJSON *body, const char *key)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(body, "params");
    if (!cJSON_IsObject(params)) {
        return NULL;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool module_uses_adc(const char *id)
{
    return strcmp(id, "sensor_adc_photoresistor") == 0 ||
           strcmp(id, "sensor_adc_water_level") == 0 ||
           strcmp(id, "sensor_joystick") == 0 ||
           strcmp(id, "sensor_soil_moisture") == 0 ||
           strcmp(id, "sensor_sound_level") == 0;
}

static bool module_uses_gpio_input(const char *id)
{
    return strcmp(id, "sensor_gpio_light_break") == 0 ||
           strcmp(id, "sensor_gpio_mercury_tilt") == 0 ||
           strcmp(id, "sensor_gpio_button") == 0 ||
           strcmp(id, "sensor_gpio_reed_switch") == 0 ||
           strcmp(id, "sensor_gpio_ir_tracking") == 0;
}

static bool module_uses_gpio_output(const char *id)
{
    return strcmp(id, "sensor_gpio_active_buzzer") == 0 ||
           strcmp(id, "sensor_gpio_dual_led") == 0 ||
           strcmp(id, "sensor_gpio_traffic_led") == 0;
}

static bool module_uses_led_strip_output(const char *id)
{
    return strcmp(id, "sensor_onewire_rgb_led") == 0 ||
           strcmp(id, "sensor_onewire_ws2812_ring") == 0;
}

static int rotary_transition_delta(int previous, int current)
{
    switch ((previous << 2) | current) {
    case 0b0001:
    case 0b0111:
    case 0b1110:
    case 0b1000:
        return 1;
    case 0b0010:
    case 0b1011:
    case 0b1101:
    case 0b0100:
        return -1;
    default:
        return 0;
    }
}

static void rotary_encoder_task(void *arg)
{
    (void)arg;
    while (true) {
        int clk_pin;
        int dt_pin;
        int button_pin;
        portENTER_CRITICAL(&s_rotary_lock);
        clk_pin = s_rotary_state.clk_pin;
        dt_pin = s_rotary_state.dt_pin;
        button_pin = s_rotary_state.button_pin;
        portEXIT_CRITICAL(&s_rotary_lock);

        if (clk_pin >= 0 && dt_pin >= 0) {
            const int clk = gpio_get_level((gpio_num_t)clk_pin);
            const int dt = gpio_get_level((gpio_num_t)dt_pin);
            const int state = (clk << 1) | dt;
            const int button = button_pin >= 0 ? gpio_get_level((gpio_num_t)button_pin) : 1;

            portENTER_CRITICAL(&s_rotary_lock);
            s_rotary_state.clk_level = clk;
            s_rotary_state.dt_level = dt;
            s_rotary_state.button_level = button;
            if (button_pin >= 0 && s_rotary_state.last_button_level == 1 && button == 0) {
                s_rotary_state.button_events++;
            }
            s_rotary_state.last_button_level = button;

            if (state != s_rotary_state.last_state) {
                int delta = rotary_transition_delta(s_rotary_state.last_state, state);
                if (delta == 0) {
                    s_rotary_state.accumulator = 0;
                } else {
                    s_rotary_state.accumulator += delta;
                    if (state == s_rotary_state.rest_state && abs(s_rotary_state.accumulator) >= 4) {
                        int detent_delta = s_rotary_state.accumulator > 0 ? 1 : -1;
                        s_rotary_state.count += detent_delta;
                        s_rotary_state.last_delta = detent_delta;
                        s_rotary_state.accumulator = 0;
                    }
                }
                s_rotary_state.last_state = state;
            }
            portEXIT_CRITICAL(&s_rotary_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t ensure_rotary_encoder_sampler(int clk_pin, int dt_pin, int button_pin)
{
    const uint64_t main_mask = (1ULL << clk_pin) | (1ULL << dt_pin);
    gpio_config_t io_conf = {
        .pin_bit_mask = main_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    if (button_pin >= 0) {
        gpio_config_t button_conf = {
            .pin_bit_mask = 1ULL << button_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&button_conf);
    }

    const int clk = gpio_get_level((gpio_num_t)clk_pin);
    const int dt = gpio_get_level((gpio_num_t)dt_pin);
    const int state = (clk << 1) | dt;
    const int button = button_pin >= 0 ? gpio_get_level((gpio_num_t)button_pin) : 1;

    portENTER_CRITICAL(&s_rotary_lock);
    if (!s_rotary_state.initialized ||
        s_rotary_state.clk_pin != clk_pin ||
        s_rotary_state.dt_pin != dt_pin ||
        s_rotary_state.button_pin != button_pin) {
        s_rotary_state.clk_pin = clk_pin;
        s_rotary_state.dt_pin = dt_pin;
        s_rotary_state.button_pin = button_pin;
        s_rotary_state.last_state = state;
        s_rotary_state.rest_state = state;
        s_rotary_state.accumulator = 0;
        s_rotary_state.count = 0;
        s_rotary_state.last_delta = 0;
        s_rotary_state.clk_level = clk;
        s_rotary_state.dt_level = dt;
        s_rotary_state.button_level = button;
        s_rotary_state.last_button_level = button;
        s_rotary_state.button_events = 0;
        s_rotary_state.initialized = true;
    }
    portEXIT_CRITICAL(&s_rotary_lock);

    if (!s_rotary_task) {
        BaseType_t ok = xTaskCreate(rotary_encoder_task, "module_rotary", 3072, NULL, 6, &s_rotary_task);
        if (ok != pdPASS) {
            s_rotary_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static const char *active_state_hint(const char *id)
{
    (void)id;
    return "check module silkscreen";
}

static const melody_t *find_melody(const char *id)
{
    if (!id || !id[0]) {
        id = "twinkle";
    }
    for (size_t i = 0; i < sizeof(MELODIES) / sizeof(MELODIES[0]); ++i) {
        if (strcmp(MELODIES[i].id, id) == 0) {
            return &MELODIES[i];
        }
    }
    return &MELODIES[1];
}

typedef struct {
    int pin;
    const melody_t *melody;
} buzzer_task_arg_t;

static void stop_buzzer_task(void)
{
    if (s_buzzer_task) {
        TaskHandle_t task = s_buzzer_task;
        s_buzzer_task = NULL;
        vTaskDelete(task);
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

static void buzzer_melody_task(void *arg)
{
    buzzer_task_arg_t *task_arg = (buzzer_task_arg_t *)arg;
    const melody_t *melody = task_arg->melody;
    const int pin = task_arg->pin;
    free(task_arg);

    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channel_conf = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&channel_conf);

    for (size_t i = 0; i < melody->count; ++i) {
        const tone_note_t *note = &melody->notes[i];
        if (note->freq > 0) {
            ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, note->freq);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        } else {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        }
        vTaskDelay(pdMS_TO_TICKS(note->ms));
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        vTaskDelay(pdMS_TO_TICKS(35));
    }

    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    s_buzzer_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_buzzer_melody(int pin, const melody_t *melody)
{
    stop_buzzer_task();
    buzzer_task_arg_t *arg = calloc(1, sizeof(*arg));
    if (!arg) {
        return ESP_ERR_NO_MEM;
    }
    arg->pin = pin;
    arg->melody = melody;
    BaseType_t ok = xTaskCreate(buzzer_melody_task, "module_buzzer", 3072, arg, 5, &s_buzzer_task);
    if (ok != pdPASS) {
        free(arg);
        s_buzzer_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void onewire_drive_low(int pin)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level((gpio_num_t)pin, 0);
}

static void onewire_release(int pin)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
}

static bool onewire_reset(int pin)
{
    onewire_drive_low(pin);
    esp_rom_delay_us(480);
    onewire_release(pin);
    esp_rom_delay_us(70);
    bool present = gpio_get_level((gpio_num_t)pin) == 0;
    esp_rom_delay_us(410);
    return present;
}

static void onewire_write_bit(int pin, int bit)
{
    onewire_drive_low(pin);
    if (bit) {
        esp_rom_delay_us(6);
        onewire_release(pin);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        onewire_release(pin);
        esp_rom_delay_us(10);
    }
}

static int onewire_read_bit(int pin)
{
    onewire_drive_low(pin);
    esp_rom_delay_us(3);
    onewire_release(pin);
    esp_rom_delay_us(10);
    int bit = gpio_get_level((gpio_num_t)pin);
    esp_rom_delay_us(53);
    return bit;
}

static void onewire_write_byte(int pin, uint8_t value)
{
    for (int i = 0; i < 8; ++i) {
        onewire_write_bit(pin, (value >> i) & 0x01);
    }
}

static uint8_t onewire_read_byte(int pin)
{
    uint8_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= (uint8_t)(onewire_read_bit(pin) << i);
    }
    return value;
}

static esp_err_t read_ds18b20_temperature(int pin, float *temperature)
{
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    if (!onewire_reset(pin)) {
        return ESP_ERR_NOT_FOUND;
    }
    onewire_write_byte(pin, 0xCC); // Skip ROM
    onewire_write_byte(pin, 0x44); // Convert T
    onewire_release(pin);
    vTaskDelay(pdMS_TO_TICKS(760));

    if (!onewire_reset(pin)) {
        return ESP_ERR_NOT_FOUND;
    }
    onewire_write_byte(pin, 0xCC); // Skip ROM
    onewire_write_byte(pin, 0xBE); // Read scratchpad
    uint8_t scratch[9] = {0};
    for (size_t i = 0; i < sizeof(scratch); ++i) {
        scratch[i] = onewire_read_byte(pin);
    }
    int16_t raw = (int16_t)((scratch[1] << 8) | scratch[0]);
    *temperature = (float)raw / 16.0f;
    return ESP_OK;
}

static esp_err_t module_strip_ensure(int pin, int count)
{
    if (s_module_strip && s_module_strip_pin == pin && s_module_strip_count == count) {
        return ESP_OK;
    }
    if (s_module_strip) {
        led_strip_del(s_module_strip);
        s_module_strip = NULL;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = pin,
        .max_leds = count,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_module_strip);
    if (err == ESP_OK) {
        s_module_strip_pin = pin;
        s_module_strip_count = count;
    }
    return err;
}

static esp_err_t ensure_adc_unit(adc_unit_t unit)
{
    if (unit < ADC_UNIT_1 || unit >= SOC_ADC_PERIPH_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t index = (size_t)unit;
    if (s_adc_units[index]) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = unit,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    return adc_oneshot_new_unit(&init_config, &s_adc_units[index]);
}

static esp_err_t read_adc_pin(int pin, int *raw, adc_unit_t *unit, adc_channel_t *channel)
{
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(pin, unit, channel), TAG, "map ADC GPIO");
    ESP_RETURN_ON_ERROR(ensure_adc_unit(*unit), TAG, "init ADC unit");

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_units[(size_t)(*unit)], *channel, &channel_config),
                        TAG, "config ADC channel");
    return adc_oneshot_read(s_adc_units[(size_t)(*unit)], *channel, raw);
}

static void add_adc_reading(cJSON *parent, const char *name, int pin)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return;
    }
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddNumberToObject(item, "pin", pin);

    int raw = 0;
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    esp_err_t err = read_adc_pin(pin, &raw, &unit, &channel);
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddNumberToObject(item, "raw", raw);
        cJSON_AddNumberToObject(item, "percent", (raw * 100) / 4095);
        cJSON_AddNumberToObject(item, "voltage_mv", (raw * 3300) / 4095);
        cJSON_AddNumberToObject(item, "adc_unit", unit);
        cJSON_AddNumberToObject(item, "adc_channel", channel);
    } else {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "所选 GPIO 不能作为 ADC 输入，请重新选择 ADC 引脚。");
        cJSON_AddStringToObject(item, "driver_error", esp_err_to_name(err));
        cJSON_AddStringToObject(item, "message", "Selected GPIO cannot be used as an ADC input. Please choose an ADC-capable pin.");
    }
    cJSON_AddItemToArray(parent, item);
}

static void add_gpio_reading(cJSON *parent, const char *name, int pin, const char *hint)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int level = gpio_get_level((gpio_num_t)pin);
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return;
    }
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddNumberToObject(item, "pin", pin);
    cJSON_AddNumberToObject(item, "level", level);
    cJSON_AddStringToObject(item, "hint", hint);
    cJSON_AddItemToArray(parent, item);
}

static int16_t read_i16_be(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] << 8 | data[1]);
}

static esp_err_t mpu6050_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(dev, payload, sizeof(payload), pdMS_TO_TICKS(100));
}

static esp_err_t mpu6050_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, pdMS_TO_TICKS(100));
}

typedef struct {
    int sda_pin;
    int scl_pin;
    uint8_t address;
    uint8_t who_am_i;
    bool swapped;
    uint8_t raw[14];
} mpu6050_sample_t;

static esp_err_t mpu6050_try_read(int sda_pin, int scl_pin, uint8_t address, bool swapped, mpu6050_sample_t *sample)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err == ESP_OK) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(bus, &dev_config, &dev);
    }

    uint8_t who_am_i = 0;
    if (err == ESP_OK) {
        err = mpu6050_read_regs(dev, 0x75, &who_am_i, 1);
    }
    if (err == ESP_OK && who_am_i != 0x68) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        err = mpu6050_write_reg(dev, 0x6B, 0x00); // Wake from sleep
    }
    if (err == ESP_OK) {
        (void)mpu6050_write_reg(dev, 0x1C, 0x00); // Accel +/-2g
        (void)mpu6050_write_reg(dev, 0x1B, 0x00); // Gyro +/-250 dps
        vTaskDelay(pdMS_TO_TICKS(5));
        err = mpu6050_read_regs(dev, 0x3B, sample->raw, sizeof(sample->raw));
    }

    if (err == ESP_OK) {
        sample->sda_pin = sda_pin;
        sample->scl_pin = scl_pin;
        sample->address = address;
        sample->who_am_i = who_am_i;
        sample->swapped = swapped;
    }
    if (dev) {
        i2c_master_bus_rm_device(dev);
    }
    if (bus) {
        i2c_del_master_bus(bus);
    }
    return err;
}

static esp_err_t send_mpu6050_read_response(httpd_req_t *req, cJSON *body, const char *action)
{
    int sda_pin = -1;
    int scl_pin = -1;
    if (!get_pin_value(body, "sda", &sda_pin) || !get_pin_value(body, "scl", &scl_pin)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SDA/SCL pins");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(body);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", "read");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", "MPU6050 read complete.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");

    mpu6050_sample_t sample = {0};
    esp_err_t err = ESP_FAIL;
    const uint8_t addresses[] = {0x68, 0x69};
    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        err = mpu6050_try_read(sda_pin, scl_pin, addresses[i], false, &sample);
        if (err == ESP_OK) {
            break;
        }
    }
    if (err != ESP_OK) {
        for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
            err = mpu6050_try_read(scl_pin, sda_pin, addresses[i], true, &sample);
            if (err == ESP_OK) {
                break;
            }
        }
    }

    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddStringToObject(item, "name", "mpu6050");
        cJSON_AddNumberToObject(item, "sda", err == ESP_OK ? sample.sda_pin : sda_pin);
        cJSON_AddNumberToObject(item, "scl", err == ESP_OK ? sample.scl_pin : scl_pin);
        cJSON_AddNumberToObject(item, "address", err == ESP_OK ? sample.address : 0x68);
        cJSON_AddNumberToObject(item, "who_am_i", sample.who_am_i);
        cJSON_AddBoolToObject(item, "swapped", err == ESP_OK ? sample.swapped : false);
        if (err == ESP_OK) {
            const int16_t ax_raw = read_i16_be(&sample.raw[0]);
            const int16_t ay_raw = read_i16_be(&sample.raw[2]);
            const int16_t az_raw = read_i16_be(&sample.raw[4]);
            const int16_t temp_raw = read_i16_be(&sample.raw[6]);
            const int16_t gx_raw = read_i16_be(&sample.raw[8]);
            const int16_t gy_raw = read_i16_be(&sample.raw[10]);
            const int16_t gz_raw = read_i16_be(&sample.raw[12]);
            const float ax = (float)ax_raw / 16384.0f;
            const float ay = (float)ay_raw / 16384.0f;
            const float az = (float)az_raw / 16384.0f;
            const float gx = (float)gx_raw / 131.0f;
            const float gy = (float)gy_raw / 131.0f;
            const float gz = (float)gz_raw / 131.0f;
            const float temp_c = (float)temp_raw / 340.0f + 36.53f;
            const float roll = atan2f(ay, az) * 57.2957795f;
            const float pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2957795f;

            cJSON_AddBoolToObject(item, "ok", true);
            cJSON_AddNumberToObject(item, "accel_x_g", ax);
            cJSON_AddNumberToObject(item, "accel_y_g", ay);
            cJSON_AddNumberToObject(item, "accel_z_g", az);
            cJSON_AddNumberToObject(item, "gyro_x_dps", gx);
            cJSON_AddNumberToObject(item, "gyro_y_dps", gy);
            cJSON_AddNumberToObject(item, "gyro_z_dps", gz);
            cJSON_AddNumberToObject(item, "roll_deg", roll);
            cJSON_AddNumberToObject(item, "pitch_deg", pitch);
            cJSON_AddNumberToObject(item, "celsius", temp_c);
        } else {
            cJSON_AddBoolToObject(item, "ok", false);
            cJSON_AddStringToObject(item, "error", esp_err_to_name(err));
            cJSON_AddStringToObject(item, "message", "MPU6050 did not respond on 0x68/0x69, including swapped SDA/SCL. Check VCC/GND and wiring.");
        }
        cJSON_AddItemToArray(data, item);
    }

    cJSON_AddItemToObject(root, "request", body);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_gpio_read_response(httpd_req_t *req, const module_item_t *module, cJSON *body, const char *action)
{
    int pin = -1;
    if (!get_pin_value(body, "signal", &pin)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing signal pin");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", "read");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", "GPIO level read complete.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");
    add_gpio_reading(data, "signal", pin, active_state_hint(module->id));
    cJSON_AddItemToObject(root, "request", body);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_gpio_output_response(httpd_req_t *req, const module_item_t *module, cJSON *body, const char *action)
{
    if (strcmp(module->id, "sensor_gpio_traffic_led") == 0) {
        int red_pin = -1;
        int blue_pin = -1;
        int green_pin = -1;
        if (!get_pin_value(body, "red", &red_pin) ||
            !get_pin_value(body, "blue", &blue_pin) ||
            !get_pin_value(body, "green", &green_pin)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing traffic light pins");
        }

        int red = 0;
        int blue = 0;
        int green = 0;
        if (strcmp(action, "stop") != 0) {
            get_param_int(body, "red", &red);
            get_param_int(body, "blue", &blue);
            get_param_int(body, "green", &green);
        }
        red = red ? 1 : 0;
        blue = blue ? 1 : 0;
        green = green ? 1 : 0;

        const struct {
            const char *name;
            int pin;
            int level;
        } outputs[] = {
            {"red", red_pin, red},
            {"blue", blue_pin, blue},
            {"green", green_pin, green},
        };

        uint64_t pin_mask = 0;
        for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i) {
            pin_mask |= 1ULL << outputs[i].pin;
        }
        gpio_config_t io_conf = {
            .pin_bit_mask = pin_mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i) {
            gpio_set_level((gpio_num_t)outputs[i].pin, outputs[i].level);
        }

        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "implemented", true);
        cJSON_AddStringToObject(root, "status", "output");
        cJSON_AddStringToObject(root, "action", action);
        cJSON_AddStringToObject(root, "message", "Traffic light GPIO outputs updated.");
        cJSON *data = cJSON_AddArrayToObject(root, "data");
        for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i) {
            cJSON *item = cJSON_CreateObject();
            if (item) {
                cJSON_AddStringToObject(item, "name", outputs[i].name);
                cJSON_AddNumberToObject(item, "pin", outputs[i].pin);
                cJSON_AddNumberToObject(item, "level", outputs[i].level);
                cJSON_AddItemToArray(data, item);
            }
        }
        cJSON_AddItemToObject(root, "request", body);
        return http_server_send_json_response(req, root);
    }

    int pin = -1;
    if (!get_pin_value(body, "signal", &pin)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing signal pin");
    }

    int level = 0;
    if (strcmp(action, "start") == 0) {
        level = 1;
    } else if (strcmp(action, "stop") == 0) {
        level = 0;
    } else if (!get_param_int(body, "level", &level)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing output level");
    }
    level = level ? 1 : 0;

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)pin, level);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", "output");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", level ? "GPIO output high." : "GPIO output low.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddStringToObject(item, "name", "output");
        cJSON_AddNumberToObject(item, "pin", pin);
        cJSON_AddNumberToObject(item, "level", level);
        cJSON_AddItemToArray(data, item);
    }
    cJSON_AddItemToObject(root, "request", body);
    (void)module;
    return http_server_send_json_response(req, root);
}

static esp_err_t send_adc_read_response(httpd_req_t *req, const module_item_t *module, cJSON *body, const char *action)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", "read");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", "ADC read complete. Raw range is normally 0-4095.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");

    if (strcmp(module->id, "sensor_joystick") == 0) {
        int pin = -1;
        if (get_pin_value(body, "x", &pin)) {
            add_adc_reading(data, "x", pin);
        }
        if (get_pin_value(body, "y", &pin)) {
            add_adc_reading(data, "y", pin);
        }
        if (get_pin_value(body, "button", &pin)) {
            add_gpio_reading(data, "button", pin, "check module silkscreen");
        }
    } else if (strcmp(module->id, "sensor_soil_moisture") == 0 ||
               strcmp(module->id, "sensor_sound_level") == 0) {
        int ao_pin = -1;
        int do_pin = -1;
        if (get_pin_value(body, "ao", &ao_pin) || get_pin_value(body, "signal", &ao_pin)) {
            add_adc_reading(data, "ao", ao_pin);
        } else {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing AO ADC pin");
        }
        if (get_pin_value(body, "do", &do_pin)) {
            add_gpio_reading(data, "do", do_pin, "comparator digital output");
        }
    } else {
        int pin = -1;
        if (!get_pin_value(body, "signal", &pin)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing signal pin");
        }
        add_adc_reading(data, "signal", pin);
    }

    cJSON_AddItemToObject(root, "request", body);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_rotary_encoder_read_response(httpd_req_t *req, cJSON *body, const char *action)
{
    int clk_pin = -1;
    int dt_pin = -1;
    if (!get_pin_value(body, "clk", &clk_pin) || !get_pin_value(body, "dt", &dt_pin)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing CLK/DT pins");
    }

    int button_pin = -1;
    bool has_button = get_pin_value(body, "button", &button_pin);
    esp_err_t err = ensure_rotary_encoder_sampler(clk_pin, dt_pin, has_button ? button_pin : -1);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start rotary sampler");
    }

    int count;
    int delta;
    int clk;
    int dt;
    int button;
    int button_events;
    portENTER_CRITICAL(&s_rotary_lock);
    count = s_rotary_state.count;
    delta = s_rotary_state.last_delta;
    clk = s_rotary_state.clk_level;
    dt = s_rotary_state.dt_level;
    button = s_rotary_state.button_level;
    button_events = s_rotary_state.button_events;
    portEXIT_CRITICAL(&s_rotary_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", "read");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", "Rotary encoder sampler is running.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");

    cJSON *encoder = cJSON_CreateObject();
    if (encoder) {
        cJSON_AddStringToObject(encoder, "name", "rotary");
        cJSON_AddNumberToObject(encoder, "count", count);
        cJSON_AddNumberToObject(encoder, "delta", delta);
        cJSON_AddNumberToObject(encoder, "clk", clk);
        cJSON_AddNumberToObject(encoder, "dt", dt);
        cJSON_AddNumberToObject(encoder, "pin", clk_pin);
        if (has_button) {
            cJSON_AddNumberToObject(encoder, "button", button);
            cJSON_AddBoolToObject(encoder, "button_pressed", button == 0);
            cJSON_AddNumberToObject(encoder, "button_events", button_events);
            cJSON_AddNumberToObject(encoder, "button_pin", button_pin);
        }
        cJSON_AddItemToArray(data, encoder);
    }

    cJSON_AddItemToObject(root, "request", body);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_dht11_read_response(httpd_req_t *req, cJSON *body, const char *action)
{
    int pin = -1;
    if (!get_pin_value(body, "signal", &pin)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing DATA pin");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", "read");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", "DHT11 read complete.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");

    float temperature = 0.0f;
    float humidity = 0.0f;
    esp_err_t err = dht_read_float_data(DHT_TYPE_DHT11, (gpio_num_t)pin, &humidity, &temperature);
    if (err == ESP_OK) {
        cJSON *temp = cJSON_CreateObject();
        if (temp) {
            cJSON_AddStringToObject(temp, "name", "temperature");
            cJSON_AddNumberToObject(temp, "pin", pin);
            cJSON_AddNumberToObject(temp, "celsius", temperature);
            cJSON_AddItemToArray(data, temp);
        }
        cJSON *humi = cJSON_CreateObject();
        if (humi) {
            cJSON_AddStringToObject(humi, "name", "humidity");
            cJSON_AddNumberToObject(humi, "pin", pin);
            cJSON_AddNumberToObject(humi, "percent", humidity);
            cJSON_AddItemToArray(data, humi);
        }
    } else {
        cJSON *item = cJSON_CreateObject();
        if (item) {
            cJSON_AddStringToObject(item, "name", "DHT11");
            cJSON_AddNumberToObject(item, "pin", pin);
            cJSON_AddStringToObject(item, "error", esp_err_to_name(err));
            cJSON_AddStringToObject(item, "message", "DHT11 读取失败，请检查 DATA 引脚、供电和上拉电阻。");
            cJSON_AddItemToArray(data, item);
        }
    }

    cJSON_AddItemToObject(root, "request", body);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_passive_buzzer_response(httpd_req_t *req, cJSON *body, const char *action)
{
    int pin = -1;
    if (!get_pin_value(body, "signal", &pin)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing buzzer PWM pin");
    }

    const char *melody_id = get_param_string(body, "melody");
    const melody_t *melody = find_melody(melody_id);
    esp_err_t err = ESP_OK;
    const bool stopping = strcmp(action, "stop") == 0;
    if (stopping) {
        stop_buzzer_task();
    } else {
        err = start_buzzer_melody(pin, melody);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", stopping ? "stopped" : "playing");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", stopping ? "Buzzer stopped." : "Buzzer melody started.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddStringToObject(item, "name", "melody");
        cJSON_AddNumberToObject(item, "pin", pin);
        cJSON_AddStringToObject(item, "melody", melody->id);
        cJSON_AddStringToObject(item, "title", melody->name);
        cJSON_AddStringToObject(item, "state", stopping ? "stopped" : "playing");
        if (err != ESP_OK) {
            cJSON_AddStringToObject(item, "error", esp_err_to_name(err));
            cJSON_AddStringToObject(item, "message", "蜂鸣器播放任务启动失败。");
        }
        cJSON_AddItemToArray(data, item);
    }
    cJSON_AddItemToObject(root, "request", body);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_ds18b20_read_response(httpd_req_t *req, cJSON *body, const char *action)
{
    int pin = -1;
    if (!get_pin_value(body, "signal", &pin)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing DQ pin");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", "read");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", "DS18B20 read complete.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");

    float temperature = 0.0f;
    esp_err_t err = read_ds18b20_temperature(pin, &temperature);
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddStringToObject(item, "name", "temperature");
        cJSON_AddNumberToObject(item, "pin", pin);
        if (err == ESP_OK) {
            cJSON_AddNumberToObject(item, "celsius", temperature);
        } else {
            cJSON_AddStringToObject(item, "error", esp_err_to_name(err));
            cJSON_AddStringToObject(item, "message", "DS18B20 读取失败，请检查 DQ 引脚、供电和 4.7k 上拉电阻。");
        }
        cJSON_AddItemToArray(data, item);
    }
    cJSON_AddItemToObject(root, "request", body);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_hcsr04_read_response(httpd_req_t *req, cJSON *body, const char *action)
{
    int trig = -1;
    int echo = -1;
    if (!get_pin_value(body, "trig", &trig) || !get_pin_value(body, "echo", &echo)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Trig/Echo pins");
    }

    gpio_config_t trig_conf = {
        .pin_bit_mask = 1ULL << trig,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&trig_conf);
    gpio_config_t echo_conf = {
        .pin_bit_mask = 1ULL << echo,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&echo_conf);

    gpio_set_level((gpio_num_t)trig, 0);
    esp_rom_delay_us(2);
    gpio_set_level((gpio_num_t)trig, 1);
    esp_rom_delay_us(10);
    gpio_set_level((gpio_num_t)trig, 0);

    int64_t deadline = esp_timer_get_time() + 30000;
    while (gpio_get_level((gpio_num_t)echo) == 0 && esp_timer_get_time() < deadline) {
    }
    int64_t start = esp_timer_get_time();
    deadline = start + 30000;
    while (gpio_get_level((gpio_num_t)echo) == 1 && esp_timer_get_time() < deadline) {
    }
    int64_t end = esp_timer_get_time();
    bool ok = end < deadline && end > start;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", "read");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", "HC-SR04 read complete.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddStringToObject(item, "name", "distance");
        cJSON_AddNumberToObject(item, "pin", echo);
        cJSON_AddNumberToObject(item, "trig_pin", trig);
        cJSON_AddNumberToObject(item, "echo_pin", echo);
        if (ok) {
            int64_t duration_us = end - start;
            cJSON_AddNumberToObject(item, "duration_us", duration_us);
            cJSON_AddNumberToObject(item, "distance_cm", (double)duration_us / 58.0);
        } else {
            cJSON_AddStringToObject(item, "error", "timeout");
            cJSON_AddStringToObject(item, "message", "HC-SR04 测距超时，请检查 Trig/Echo 接线和 Echo 电平匹配。");
        }
        cJSON_AddItemToArray(data, item);
    }
    cJSON_AddItemToObject(root, "request", body);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_led_strip_response(httpd_req_t *req, const module_item_t *module, cJSON *body, const char *action)
{
    int pin = -1;
    if (!get_pin_value(body, "signal", &pin)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing DIN pin");
    }

    int count = strcmp(module->id, "sensor_onewire_rgb_led") == 0 ? 1 : 8;
    get_param_int(body, "count", &count);
    if (count < 1) {
        count = 1;
    } else if (count > 64) {
        count = 64;
    }

    int red = 0;
    int blue = 0;
    int green = 0;
    if (strcmp(action, "stop") != 0) {
        get_param_int(body, "red", &red);
        get_param_int(body, "blue", &blue);
        get_param_int(body, "green", &green);
    }
    red = red ? 80 : 0;
    green = green ? 80 : 0;
    blue = blue ? 80 : 0;

    esp_err_t err = module_strip_ensure(pin, count);
    if (err == ESP_OK) {
        for (int i = 0; i < count; ++i) {
            led_strip_set_pixel(s_module_strip, i, red, green, blue);
        }
        led_strip_refresh(s_module_strip);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "implemented", true);
    cJSON_AddStringToObject(root, "status", "output");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", err == ESP_OK ? "LED color updated." : "LED strip update failed.");
    cJSON *data = cJSON_AddArrayToObject(root, "data");
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddStringToObject(item, "name", "rgb");
        cJSON_AddNumberToObject(item, "pin", pin);
        cJSON_AddNumberToObject(item, "red", red);
        cJSON_AddNumberToObject(item, "green", green);
        cJSON_AddNumberToObject(item, "blue", blue);
        cJSON_AddNumberToObject(item, "count", count);
        if (err != ESP_OK) {
            cJSON_AddStringToObject(item, "error", esp_err_to_name(err));
            cJSON_AddStringToObject(item, "message", "全彩 RGB/WS2812 控制失败，请检查 DIN 引脚和供电。");
        }
        cJSON_AddItemToArray(data, item);
    }
    cJSON_AddItemToObject(root, "request", body);
    return http_server_send_json_response(req, root);
}

static esp_err_t modules_list_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    if (!root || !items) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < sizeof(MODULES) / sizeof(MODULES[0]); ++i) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
        add_module_json(item, &MODULES[i]);
        cJSON_AddItemToArray(items, item);
    }
    return http_server_send_json_response(req, root);
}

static void add_pin_array(cJSON *root, const char *capability, const int *pins, size_t count)
{
    cJSON_AddStringToObject(root, "capability", capability);
    cJSON *array = cJSON_AddArrayToObject(root, "pins");
    if (!array) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(pins[i]));
    }
}

static esp_err_t pins_get_handler(httpd_req_t *req)
{
    char capability[24] = {0};
    if (http_server_query_get(req, "capability", capability, sizeof(capability)) != ESP_OK) {
        strlcpy(capability, "gpio", sizeof(capability));
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    if (strcmp(capability, "adc") == 0) {
        add_pin_array(root, "adc", ADC_PINS, sizeof(ADC_PINS) / sizeof(ADC_PINS[0]));
    } else if (strcmp(capability, "gpio") == 0 ||
               strcmp(capability, "pwm") == 0 ||
               strcmp(capability, "i2c") == 0 ||
               strcmp(capability, "onewire") == 0) {
        add_pin_array(root, capability, GPIO_PINS, sizeof(GPIO_PINS) / sizeof(GPIO_PINS[0]));
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported pin capability");
    }

    return http_server_send_json_response(req, root);
}

static esp_err_t get_modules_tail(httpd_req_t *req, char *tail, size_t tail_size)
{
    const char *prefix = "/api/modules/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(tail, req->uri + strlen(prefix), tail_size);
    char *query = strchr(tail, '?');
    if (query) {
        *query = '\0';
    }
    http_server_url_decode_inplace(tail);
    return tail[0] ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static bool split_tail(char *tail, char **id, char **suffix)
{
    *id = tail;
    char *slash = strchr(tail, '/');
    if (slash) {
        *slash = '\0';
        *suffix = slash + 1;
    } else {
        *suffix = "";
    }
    return module_id_is_valid(*id);
}

static esp_err_t module_config_path(const char *id, char *path, size_t path_size)
{
    if (!module_id_is_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    http_server_ctx_t *ctx = http_server_ctx();
    char dir[HTTP_SERVER_PATH_MAX];
    int dir_len = snprintf(dir, sizeof(dir), "%s/module_configs", ctx->storage_base_path);
    if (dir_len < 0 || dir_len >= (int)sizeof(dir)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "Failed to create module config dir: errno=%d", errno);
        return ESP_FAIL;
    }

    int written = snprintf(path, path_size, "%s/%s.json", dir, id);
    return (written > 0 && written < (int)path_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t send_unsaved_config(httpd_req_t *req, const char *id)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "module_id", id);
    cJSON_AddObjectToObject(root, "pins");
    cJSON_AddObjectToObject(root, "params");
    cJSON_AddBoolToObject(root, "saved", false);
    return http_server_send_json_response(req, root);
}

static esp_err_t module_config_get_handler(httpd_req_t *req, const char *id)
{
    char path[HTTP_SERVER_PATH_MAX];
    ESP_RETURN_ON_ERROR(module_config_path(id, path, sizeof(path)), TAG, "Failed to build config path");

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return send_unsaved_config(req, id);
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return send_unsaved_config(req, id);
    }
    long size = ftell(fp);
    rewind(fp);
    if (size <= 0 || size > 8192) {
        fclose(fp);
        return send_unsaved_config(req, id);
    }

    char *payload = calloc(1, (size_t)size + 1);
    if (!payload) {
        fclose(fp);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    size_t read_len = fread(payload, 1, (size_t)size, fp);
    fclose(fp);

    if (read_len != (size_t)size) {
        free(payload);
        return send_unsaved_config(req, id);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    return err;
}

static esp_err_t module_config_post_handler(httpd_req_t *req, const char *id)
{
    cJSON *root = NULL;
    esp_err_t err = http_server_parse_json_body(req, &root);
    if (err != ESP_OK || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, "module_id");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "saved");
    cJSON_AddStringToObject(root, "module_id", id);
    cJSON_AddBoolToObject(root, "saved", true);

    char *payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    char path[HTTP_SERVER_PATH_MAX];
    err = module_config_path(id, path, sizeof(path));
    if (err == ESP_OK) {
        FILE *fp = fopen(path, "wb");
        if (!fp) {
            err = ESP_FAIL;
        } else {
            size_t len = strlen(payload);
            err = (fwrite(payload, 1, len, fp) == len) ? ESP_OK : ESP_FAIL;
            fclose(fp);
        }
    }

    if (err != ESP_OK) {
        free(payload);
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save module config");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    err = httpd_resp_sendstr(req, payload);
    free(payload);
    cJSON_Delete(root);
    return err;
}

static esp_err_t module_config_delete_handler(httpd_req_t *req, const char *id)
{
    char path[HTTP_SERVER_PATH_MAX];
    ESP_RETURN_ON_ERROR(module_config_path(id, path, sizeof(path)), TAG, "Failed to build config path");
    if (unlink(path) != 0 && errno != ENOENT) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete module config");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return http_server_send_json_response(req, root);
}

static esp_err_t module_action_handler(httpd_req_t *req, const module_item_t *module, const char *action)
{
    cJSON *body = NULL;
    if (req->content_len > 0) {
        esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    }

    if (module->implemented &&
        (strcmp(action, "control") == 0 || strcmp(action, "start") == 0 || strcmp(action, "stop") == 0) &&
        module_uses_led_strip_output(module->id)) {
        return send_led_strip_response(req, module, body, action);
    }

    if (module->implemented &&
        (strcmp(action, "control") == 0 || strcmp(action, "start") == 0 || strcmp(action, "stop") == 0) &&
        module_uses_gpio_output(module->id)) {
        return send_gpio_output_response(req, module, body, action);
    }

    if (module->implemented && strcmp(module->id, "sensor_pwm_passive_buzzer") == 0 &&
        (strcmp(action, "control") == 0 || strcmp(action, "start") == 0 || strcmp(action, "stop") == 0)) {
        return send_passive_buzzer_response(req, body, action);
    }

    if (module->implemented && (strcmp(action, "read") == 0 || strcmp(action, "start") == 0)) {
        if (strcmp(module->id, "sensor_onewire_ds18b20") == 0) {
            return send_ds18b20_read_response(req, body, action);
        }
        if (strcmp(module->id, "sensor_onewire_dht11") == 0) {
            return send_dht11_read_response(req, body, action);
        }
        if (strcmp(module->id, "sensor_hcsr04_ultrasonic") == 0) {
            return send_hcsr04_read_response(req, body, action);
        }
        if (strcmp(module->id, "sensor_rotary_encoder") == 0) {
            return send_rotary_encoder_read_response(req, body, action);
        }
        if (strcmp(module->id, "sensor_i2c_mpu6050") == 0) {
            return send_mpu6050_read_response(req, body, action);
        }
        if (module_uses_gpio_input(module->id)) {
            return send_gpio_read_response(req, module, body, action);
        }
        if (module_uses_adc(module->id)) {
            return send_adc_read_response(req, module, body, action);
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(body);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "implemented", module->implemented);
    cJSON_AddStringToObject(root, "status", module->implemented ? "ready" : "reserved");
    if (module->implemented) {
        cJSON_AddStringToObject(root, "message", "Module API is ready. Hardware behavior is verified in each module acceptance page.");
    } else {
        cJSON_AddStringToObject(root, "message", "Reserved module. Driver is not implemented in this version.");
    }
    cJSON_AddStringToObject(root, "action", action);
    if (body) {
        cJSON_AddItemToObject(root, "request", body);
    }
    return http_server_send_json_response(req, root);
}

static esp_err_t modules_wildcard_handler(httpd_req_t *req)
{
    char tail[HTTP_SERVER_PATH_MAX];
    if (get_modules_tail(req, tail, sizeof(tail)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid module path");
    }

    char *id = NULL;
    char *suffix = NULL;
    if (!split_tail(tail, &id, &suffix)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid module id");
    }

    const module_item_t *module = find_module(id);
    if (!module) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Module not found");
    }

    if (suffix[0] == '\0' && req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
        add_module_json(root, module);
        return http_server_send_json_response(req, root);
    }

    if (strcmp(suffix, "config") == 0) {
        if (req->method == HTTP_GET) {
            return module_config_get_handler(req, id);
        }
        if (req->method == HTTP_POST) {
            return module_config_post_handler(req, id);
        }
        if (req->method == HTTP_DELETE) {
            return module_config_delete_handler(req, id);
        }
    }

    if (req->method == HTTP_POST &&
        (strcmp(suffix, "read") == 0 ||
         strcmp(suffix, "start") == 0 ||
         strcmp(suffix, "stop") == 0 ||
         strcmp(suffix, "control") == 0)) {
        return module_action_handler(req, module, suffix);
    }

    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Module route not found");
}

esp_err_t http_server_register_module_routes(httpd_handle_t server)
{
    const httpd_uri_t pins_get = {
        .uri = "/api/pins",
        .method = HTTP_GET,
        .handler = pins_get_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &pins_get), TAG, "register /api/pins");

    const httpd_uri_t modules_get = {
        .uri = "/api/modules",
        .method = HTTP_GET,
        .handler = modules_list_get_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &modules_get), TAG, "register /api/modules");

    const httpd_uri_t module_get = {
        .uri = "/api/modules/*",
        .method = HTTP_GET,
        .handler = modules_wildcard_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &module_get), TAG, "register GET /api/modules/*");

    const httpd_uri_t module_post = {
        .uri = "/api/modules/*",
        .method = HTTP_POST,
        .handler = modules_wildcard_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &module_post), TAG, "register POST /api/modules/*");

    const httpd_uri_t module_delete = {
        .uri = "/api/modules/*",
        .method = HTTP_DELETE,
        .handler = modules_wildcard_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &module_delete), TAG, "register DELETE /api/modules/*");

    return ESP_OK;
}
