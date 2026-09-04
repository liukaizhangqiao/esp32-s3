#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_SDA_GPIO GPIO_NUM_8
#define I2C_SCL_GPIO GPIO_NUM_9
#define OLED_I2C_ADDRESS 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_RAM_WIDTH 128

static const char *TAG = "oled_test";
static uint8_t framebuffer[OLED_WIDTH * OLED_PAGES];

static const char font_chars[] = " -0123EHLOPS";
static const uint8_t font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // space
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
};

static esp_err_t oled_send_commands(i2c_master_dev_handle_t oled,
                                    const uint8_t *commands, size_t count)
{
    // Send one command per transaction for maximum controller compatibility.
    uint8_t packet[2] = {0x00, 0x00};
    for (size_t i = 0; i < count; i++) {
        packet[1] = commands[i];
        ESP_RETURN_ON_ERROR(i2c_master_transmit(oled, packet, sizeof(packet), 1000),
                            TAG, "sending command 0x%02X failed", commands[i]);
    }
    return ESP_OK;
}

static esp_err_t oled_init(i2c_master_dev_handle_t oled)
{
    const uint8_t init_commands[] = {
        0xAE,             // Display off
        0x00, 0x10,       // Column zero
        0x40,             // Display start line
        0xB0,             // Page zero
        0x81, 0xCF,       // Contrast
        0xA1,             // Segment remap
        0xA6,             // Normal display (not inverted)
        0xA8, 0x3F,       // Multiplex ratio: 64 rows
        0x8D, 0x14,       // SSD1306/SSD1315 internal charge pump on
        0x20, 0x02,       // Page addressing mode
        0xC8,             // COM scan direction
        0xD3, 0x00,       // Display offset
        0xD5, 0x80,       // Clock divide ratio
        0xD9, 0xF1,       // Pre-charge period
        0xDA, 0x12,       // COM pin configuration
        0xDB, 0x40,       // VCOMH deselect level
        0xA4,             // Use display RAM contents
    };
    ESP_RETURN_ON_ERROR(oled_send_commands(oled, init_commands,
                                            sizeof(init_commands)), TAG,
                        "sending initialization commands failed");

    // Let the internal high-voltage charge pump stabilize before display-on.
    vTaskDelay(pdMS_TO_TICKS(150));
    const uint8_t display_on = 0xAF;
    ESP_RETURN_ON_ERROR(oled_send_commands(oled, &display_on, 1), TAG,
                        "turning display on failed");
    vTaskDelay(pdMS_TO_TICKS(150));
    return ESP_OK;
}

static void set_pixel(int x, int y)
{
    if (x >= 0 && x < OLED_WIDTH && y >= 0 && y < OLED_HEIGHT) {
        framebuffer[x + (y / 8) * OLED_WIDTH] |= 1U << (y % 8);
    }
}

static const uint8_t *find_glyph(char character)
{
    for (size_t i = 0; i < sizeof(font_chars) - 1; i++) {
        if (font_chars[i] == character) {
            return font_5x7[i];
        }
    }
    return font_5x7[0];
}

static void draw_text(int x, int y, const char *text, int scale)
{
    while (*text != '\0') {
        const uint8_t *glyph = find_glyph(*text++);
        for (int column = 0; column < 5; column++) {
            for (int row = 0; row < 7; row++) {
                if (glyph[column] & (1U << row)) {
                    for (int dx = 0; dx < scale; dx++) {
                        for (int dy = 0; dy < scale; dy++) {
                            set_pixel(x + column * scale + dx,
                                      y + row * scale + dy);
                        }
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

static void draw_test_pattern(void)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    draw_text(34, 12, "HELLO", 2);
    draw_text(40, 38, "ESP32-S3", 1);
}

static esp_err_t oled_refresh(i2c_master_dev_handle_t oled)
{
    uint8_t packet[OLED_WIDTH + 1];
    packet[0] = 0x40;

    for (uint8_t page = 0; page < OLED_PAGES; page++) {
        const uint8_t page_commands[] = {
            (uint8_t)(0xB0 | page), 0x00, 0x10,
        };
        ESP_RETURN_ON_ERROR(oled_send_commands(oled, page_commands,
                                                sizeof(page_commands)), TAG,
                            "setting page %u failed", page);
        memcpy(&packet[1], &framebuffer[page * OLED_WIDTH], OLED_WIDTH);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(oled, packet, sizeof(packet), 1000),
                            TAG, "writing page %u failed", page);
    }
    return ESP_OK;
}

static esp_err_t oled_clear_all_ram(i2c_master_dev_handle_t oled)
{
    uint8_t packet[OLED_RAM_WIDTH + 1] = {0};
    packet[0] = 0x40;

    for (uint8_t page = 0; page < OLED_PAGES; page++) {
        const uint8_t page_commands[] = {
            (uint8_t)(0xB0 | page), 0x00, 0x10,
        };
        ESP_RETURN_ON_ERROR(oled_send_commands(oled, page_commands,
                                                sizeof(page_commands)), TAG,
                            "setting clear page %u failed", page);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(oled, packet, sizeof(packet), 1000),
                            TAG, "clearing page %u failed", page);
    }
    return ESP_OK;
}

void app_main(void)
{
    // On a cold power-up the OLED module's power/reset circuit becomes ready
    // later than the ESP32-S3. Wait before sending the first I2C command.
    ESP_LOGI(TAG, "Waiting for OLED power to stabilize");
    vTaskDelay(pdMS_TO_TICKS(1000));

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    ESP_LOGI(TAG, "Probing OLED at 0x%02X", OLED_I2C_ADDRESS);
    ESP_ERROR_CHECK(i2c_master_probe(bus, OLED_I2C_ADDRESS, 100));

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t oled = NULL;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &device_config, &oled));

    ESP_ERROR_CHECK(oled_init(oled));
    ESP_ERROR_CHECK(oled_clear_all_ram(oled));
    draw_test_pattern();
    ESP_ERROR_CHECK(oled_refresh(oled));
    ESP_LOGI(TAG, "Displaying HELLO and ESP32-S3");
}
