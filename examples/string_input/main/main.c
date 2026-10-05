/**
 * @file main.c
 * @brief Example for reading a line of text with a timeout
 *
 * Asks for a name, waits up to 30 seconds for it to be typed and ENTER to be
 * pressed, then greets it. BACK deletes. If time runs out, whatever was typed
 * so far is shown.
 *
 * Product used is www.solde.red/333360
 *
 * Connections:
 *   KEYBOARD     ESP32-S3 (and others)   ESP32
 *   SDA          GPIO8                   GPIO21
 *   SCL          GPIO9                   GPIO22
 *   3V3          3V3                     3V3
 *   GND          GND                     GND
 *
 * Or plug it into a Qwiic connector.
 *
 * @author Soldered Electronics
 */

#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soldered_inputronic_keyboard.h"

static const char *TAG = "STRING_INPUT";

// Change these to match how the keyboard is wired. GPIO6..11 hold the flash
// on the classic ESP32, so it uses its usual I2C pins instead.
#if CONFIG_IDF_TARGET_ESP32
#define PIN_NUM_SDA GPIO_NUM_21
#define PIN_NUM_SCL GPIO_NUM_22
#else
#define PIN_NUM_SDA GPIO_NUM_8
#define PIN_NUM_SCL GPIO_NUM_9
#endif

#define INPUT_TIMEOUT_MS 30000

void app_main(void)
{
    // The I2C bus belongs to the application so that other devices can share it
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_NUM_SDA,
        .scl_io_num = PIN_NUM_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    inputronic_keyboard_t kbd;
    inputronic_keyboard_config_t config = INPUTRONIC_KEYBOARD_CONFIG_DEFAULT(bus);
    ESP_ERROR_CHECK(inputronic_keyboard_init(&kbd, &config));

    while (1) {
        char name[33];

        ESP_LOGI(TAG, "Type your name and press ENTER (%d s)", INPUT_TIMEOUT_MS / 1000);

        // NULL picks the default end and backspace keys, ENTER and BACK
        esp_err_t err = inputronic_keyboard_string_input(&kbd, name, sizeof(name), INPUT_TIMEOUT_MS, NULL, NULL);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Hello, %s!", name);
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Timed out, got \"%s\" so far", name);
        } else {
            ESP_LOGE(TAG, "Reading the keyboard failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
