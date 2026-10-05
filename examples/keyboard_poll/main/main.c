/**
 * @file main.c
 * @brief Example for checking the keymap: logs every key press
 *
 * Polls the keyboard and logs the matrix position, label and typed character
 * of every key press. Releases are skipped to keep the log readable. Useful
 * for checking a custom keymap, SHIFT and CAPS.
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

static const char *TAG = "KEYBOARD_POLL";

// Change these to match how the keyboard is wired. GPIO6..11 hold the flash
// on the classic ESP32, so it uses its usual I2C pins instead.
#if CONFIG_IDF_TARGET_ESP32
#define PIN_NUM_SDA GPIO_NUM_21
#define PIN_NUM_SCL GPIO_NUM_22
#else
#define PIN_NUM_SDA GPIO_NUM_8
#define PIN_NUM_SCL GPIO_NUM_9
#endif

// The keyboard queues up to 10 events, reading every 20 ms keeps up with
// fast typing
#define POLL_INTERVAL_MS 20

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
    ESP_LOGI(TAG, "Keyboard ready, press any key");

    while (1) {
        inputronic_keyboard_event_t event;
        esp_err_t err;

        // Drain everything that queued up since the last poll
        while ((err = inputronic_keyboard_read_event(&kbd, &event)) != ESP_ERR_NOT_FOUND) {
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Reading the keyboard failed: %s", esp_err_to_name(err));
                break;
            }
            if (!event.pressed) {
                continue;
            }

            if (event.character != 0) {
                ESP_LOGI(TAG, "Row %u, col %u, label %s, types '%c'", event.row, event.col, event.label,
                         event.character);
            } else {
                ESP_LOGI(TAG, "Row %u, col %u, label %s", event.row, event.col,
                         event.label ? event.label : "(unmapped)");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
