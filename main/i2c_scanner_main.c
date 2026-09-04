#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_SDA_GPIO GPIO_NUM_8
#define I2C_SCL_GPIO GPIO_NUM_9
static const char *TAG = "i2c_scanner";

void app_main(void)
{
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

    ESP_LOGI(TAG, "I2C scanner started: SDA=GPIO8, SCL=GPIO9");
    ESP_LOGI(TAG, "OLED is normally at 0x3C or 0x3D");

    while (true) {
        int device_count = 0;

        for (uint8_t address = 0x08; address <= 0x77; address++) {
            esp_err_t result = i2c_master_probe(bus, address, 50);
            if (result == ESP_OK) {
                ESP_LOGI(TAG, "I2C device found at address 0x%02X", address);
                device_count++;
            }
        }

        if (device_count == 0) {
            ESP_LOGW(TAG, "No I2C device found; check GND/VCC/SCL/SDA wiring");
        } else {
            ESP_LOGI(TAG, "Scan complete: %d device(s) found", device_count);
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
