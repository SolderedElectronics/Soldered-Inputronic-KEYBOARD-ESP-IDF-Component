/**
 * @file main.c
 * @brief Example for typing into the serial monitor
 *
 * Echoes what is typed on the keyboard to the console like a terminal: SHIFT
 * and CAPS work, BACK deletes, ENTER starts a new line and the arrow keys are
 * shown as [UP], [DOWN], [LEFT], [RIGHT]. Use a terminal that understands
 * backspace, such as `idf.py monitor`.
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

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soldered_inputronic_keyboard.h"

static const char *TAG = "SERIAL_TYPE";

// Change these to match how the keyboard is wired. GPIO6..11 hold the flash
// on the classic ESP32, so it uses its usual I2C pins instead.
#if CONFIG_IDF_TARGET_ESP32
#define PIN_NUM_SDA GPIO_NUM_21
#define PIN_NUM_SCL GPIO_NUM_22
#else
#define PIN_NUM_SDA GPIO_NUM_8
#define PIN_NUM_SCL GPIO_NUM_9
#endif

#define POLL_INTERVAL_MS 20

static void handle_press(const inputronic_keyboard_event_t *event)
{
    if (event->character != 0) {
        putchar(event->character);
    } else if (strcmp(event->label, "BACK") == 0) {
        fputs("\b \b", stdout);
    } else if (strcmp(event->label, "ENTER") == 0) {
        putchar('\n');
    } else if (strcmp(event->label, "UP") == 0 || strcmp(event->label, "DOWN") == 0 ||
               strcmp(event->label, "LEFT") == 0 || strcmp(event->label, "RIGHT") == 0) {
        printf("[%s]", event->label);
    }
    // Modifiers, CAPS and function keys type nothing
}

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
    ESP_LOGI(TAG, "Keyboard ready, start typing");

    while (1) {
        inputronic_keyboard_event_t event;
        esp_err_t err;
        bool typed = false;

        while ((err = inputronic_keyboard_read_event(&kbd, &event)) != ESP_ERR_NOT_FOUND) {
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Reading the keyboard failed: %s", esp_err_to_name(err));
                break;
            }
            if (event.pressed && event.label != NULL) {
                handle_press(&event);
                typed = true;
            }
        }

        // stdout is line buffered, flush so characters show up as they are typed
        if (typed) {
            fflush(stdout);
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
