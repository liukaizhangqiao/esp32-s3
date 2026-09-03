#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define RGB_LED_GPIO GPIO_NUM_48
#define RMT_RESOLUTION_HZ 10000000
#define DEBOUNCE_TIME_MS 30
#define POLL_INTERVAL_MS 10

static const char *TAG = "button_rgb";

static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 3,
    .level1 = 0,
    .duration1 = 9,
};

static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 9,
    .level1 = 0,
    .duration1 = 3,
};

static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = 250,
    .level1 = 0,
    .duration1 = 250,
};

static size_t ws2812_encode(const void *data, size_t data_size,
                            size_t symbols_written, size_t symbols_free,
                            rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;

    if (symbols_free < 8) {
        return 0;
    }

    const uint8_t *bytes = data;
    size_t data_pos = symbols_written / 8;

    if (data_pos < data_size) {
        size_t symbol_pos = 0;
        for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
            symbols[symbol_pos++] = (bytes[data_pos] & mask) ? ws2812_one : ws2812_zero;
        }
        return symbol_pos;
    }

    symbols[0] = ws2812_reset;
    *done = true;
    return 1;
}

static void set_rgb(rmt_channel_handle_t channel,
                    rmt_encoder_handle_t encoder,
                    uint8_t red, uint8_t green, uint8_t blue)
{
    // WS2812B transmits color bytes in green, red, blue (GRB) order.
    uint8_t pixel[] = {green, red, blue};
    const rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    ESP_ERROR_CHECK(rmt_transmit(channel, encoder, pixel, sizeof(pixel), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(channel, portMAX_DELAY));
}

typedef enum {
    LED_MODE_OFF,
    LED_MODE_RAINBOW,
    LED_MODE_BREATHE,
    LED_MODE_POLICE,
    LED_MODE_COUNT,
} led_mode_t;

static const char *const mode_names[] = {
    "off",
    "rainbow",
    "blue breathe",
    "red/blue flash",
};

static void color_wheel(uint8_t position, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (position < 85) {
        *red = 255 - position * 3;
        *green = position * 3;
        *blue = 0;
    } else if (position < 170) {
        position -= 85;
        *red = 0;
        *green = 255 - position * 3;
        *blue = position * 3;
    } else {
        position -= 170;
        *red = position * 3;
        *green = 0;
        *blue = 255 - position * 3;
    }

    // Limit every channel to about 12.5% brightness.
    *red /= 8;
    *green /= 8;
    *blue /= 8;
}

static void render_effect(rmt_channel_handle_t channel,
                          rmt_encoder_handle_t encoder,
                          led_mode_t mode, uint8_t phase)
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    switch (mode) {
    case LED_MODE_RAINBOW:
        color_wheel(phase, &red, &green, &blue);
        break;
    case LED_MODE_BREATHE: {
        uint8_t triangle = phase < 128 ? phase : 255 - phase;
        blue = (uint16_t)triangle * 32 / 127;
        break;
    }
    case LED_MODE_POLICE:
        if ((phase / 12) % 2 == 0) {
            red = 32;
        } else {
            blue = 32;
        }
        break;
    case LED_MODE_OFF:
    default:
        break;
    }

    set_rgb(channel, encoder, red, green, blue);
}

void app_main(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));

    rmt_channel_handle_t led_channel = NULL;
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = RGB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&channel_config, &led_channel));

    rmt_encoder_handle_t led_encoder = NULL;
    const rmt_simple_encoder_config_t encoder_config = {
        .callback = ws2812_encode,
    };
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&encoder_config, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_channel));

    led_mode_t mode = LED_MODE_OFF;
    uint8_t phase = 0;
    TickType_t last_frame = xTaskGetTickCount();
    render_effect(led_channel, led_encoder, mode, phase);
    ESP_LOGI(TAG, "Ready: short-press BOOT (GPIO0) to change RGB effects (GPIO48)");
    ESP_LOGI(TAG, "Mode: %s", mode_names[mode]);

    int stable_state = gpio_get_level(BOOT_BUTTON_GPIO);
    int candidate_state = stable_state;
    TickType_t candidate_since = xTaskGetTickCount();

    while (1) {
        int raw_state = gpio_get_level(BOOT_BUTTON_GPIO);
        TickType_t now = xTaskGetTickCount();

        if (raw_state != candidate_state) {
            candidate_state = raw_state;
            candidate_since = now;
        } else if (candidate_state != stable_state &&
                   now - candidate_since >= pdMS_TO_TICKS(DEBOUNCE_TIME_MS)) {
            stable_state = candidate_state;

            if (stable_state == 0) {
                mode = (led_mode_t)((mode + 1) % LED_MODE_COUNT);
                phase = 0;
                last_frame = now;
                render_effect(led_channel, led_encoder, mode, phase);
                ESP_LOGI(TAG, "Mode: %s", mode_names[mode]);
            }
        }

        if (now - last_frame >= pdMS_TO_TICKS(20)) {
            last_frame = now;
            phase++;
            render_effect(led_channel, led_encoder, mode, phase);
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
