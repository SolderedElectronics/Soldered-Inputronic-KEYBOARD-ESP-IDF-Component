/**
 * @file soldered_inputronic_keyboard.c
 * @brief Implementation for the soldered-inputronic-keyboard component
 *
 * Register setup, event decoding, the CAPS toggle and SHIFT handling follow
 * the Soldered Inputronic KEYBOARD Arduino library.
 *
 * @author Soldered Electronics
 */

#include "soldered_inputronic_keyboard.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "inputronic_keyboard";

// TCA8418 registers, the subset the Arduino library uses
#define REG_INT_STAT        0x02
#define REG_KEY_LCK_EC      0x03
#define REG_KEY_EVENT_A     0x04
#define REG_GPIO_INT_STAT_1 0x11
#define REG_GPIO_INT_STAT_2 0x12
#define REG_GPIO_INT_STAT_3 0x13
#define REG_GPIO_INT_EN_1   0x1A
#define REG_GPIO_INT_EN_2   0x1B
#define REG_GPIO_INT_EN_3   0x1C
#define REG_KP_GPIO_1       0x1D
#define REG_KP_GPIO_2       0x1E
#define REG_KP_GPIO_3       0x1F
#define REG_GPI_EM_1        0x20
#define REG_GPI_EM_2        0x21
#define REG_GPI_EM_3        0x22
#define REG_GPIO_DIR_1      0x23
#define REG_GPIO_DIR_2      0x24
#define REG_GPIO_DIR_3      0x25
#define REG_GPIO_INT_LVL_1  0x26
#define REG_GPIO_INT_LVL_2  0x27
#define REG_GPIO_INT_LVL_3  0x28
#define REG_DEBOUNCE_DIS_1  0x29
#define REG_DEBOUNCE_DIS_2  0x2A
#define REG_DEBOUNCE_DIS_3  0x2B

#define INT_STAT_K_INT   0x01
#define INT_STAT_GPI_INT 0x02

// KEY_EVENT_A: bit 7 set on press, low 7 bits are the key code, 1..80 row
// major over 10 columns. Reading an empty FIFO returns 0.
#define EVENT_PRESS     0x80
#define EVENT_CODE_MASK 0x7F
#define EVENT_CODE_MAX  (INPUTRONIC_KEYBOARD_ROWS * INPUTRONIC_KEYBOARD_COLS)

// The FIFO holds 10 events, anything past that while draining means the bus
// is returning garbage
#define FIFO_DRAIN_LIMIT 32

#define I2C_TIMEOUT_MS 100

const inputronic_keyboard_keymap_t INPUTRONIC_KEYBOARD_KEYMAP_UPPER = {
    {NULL, NULL, NULL, NULL, NULL, NULL, "UP", "LEFT", "DOWN", "RIGHT"},
    {NULL, NULL, NULL, "BACK", "ENTER", "DEL", "FN3", "FN4", "FN5", "FN6"},
    {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10"},
    {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"},
    {"P", "Q", "W", "E", "R", "T", "Y", "U", "I", "O"},
    {";", "A", "S", "D", "F", "G", "H", "J", "K", "L"},
    {NULL, "Z", "X", "C", "V", "B", "N", "M", ",", "."},
    {NULL, "ESC", "TAB", "CAPS", "SHIFT", "CTRL", "ALT", "FN1", "FN2", "SPACE"},
};

const inputronic_keyboard_keymap_t INPUTRONIC_KEYBOARD_KEYMAP_LOWER = {
    {NULL, NULL, NULL, NULL, NULL, NULL, "UP", "LEFT", "DOWN", "RIGHT"},
    {NULL, NULL, NULL, "BACK", "ENTER", "DEL", "FN3", "FN4", "FN5", "FN6"},
    {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10"},
    {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"},
    {"p", "q", "w", "e", "r", "t", "y", "u", "i", "o"},
    {";", "a", "s", "d", "f", "g", "h", "j", "k", "l"},
    {NULL, "z", "x", "c", "v", "b", "n", "m", ",", "."},
    {NULL, "ESC", "TAB", "CAPS", "SHIFT", "CTRL", "ALT", "FN1", "FN2", "SPACE"},
};

const inputronic_keyboard_shift_pair_t INPUTRONIC_KEYBOARD_SHIFT_MAP[] = {
    {'1', '!'}, {'2', '"'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '&'}, {'7', '/'},
    {'8', '('}, {'9', ')'}, {'0', '='}, {';', ':'}, {',', ';'}, {'.', ':'},
};

const size_t INPUTRONIC_KEYBOARD_SHIFT_MAP_LEN =
    sizeof(INPUTRONIC_KEYBOARD_SHIFT_MAP) / sizeof(INPUTRONIC_KEYBOARD_SHIFT_MAP[0]);

static esp_err_t write_reg(inputronic_keyboard_t *kbd, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(kbd->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

// Repeated start between the register address and the read, as in the Arduino library
static esp_err_t read_reg(inputronic_keyboard_t *kbd, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(kbd->dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static const inputronic_keyboard_keymap_t *active_keymap(const inputronic_keyboard_t *kbd)
{
    return kbd->caps_lock ? kbd->keymap_upper : kbd->keymap_lower;
}

static bool is_single_char(const char *s)
{
    return s != NULL && s[0] != '\0' && s[1] == '\0';
}

static esp_err_t configure(inputronic_keyboard_t *kbd)
{
    // Register and value pairs, same defaults as the Arduino library's begin()
    static const uint8_t setup[][2] = {
        // Every GPIO an input
        {REG_GPIO_DIR_1, 0x00},
        {REG_GPIO_DIR_2, 0x00},
        {REG_GPIO_DIR_3, 0x00},
        // GPI events on, harmless with every pin in the matrix
        {REG_GPI_EM_1, 0xFF},
        {REG_GPI_EM_2, 0xFF},
        {REG_GPI_EM_3, 0xFF},
        {REG_GPIO_INT_LVL_1, 0x00},
        {REG_GPIO_INT_LVL_2, 0x00},
        {REG_GPIO_INT_LVL_3, 0x00},
        // Interrupts off, the FIFO is polled
        {REG_GPIO_INT_EN_1, 0x00},
        {REG_GPIO_INT_EN_2, 0x00},
        {REG_GPIO_INT_EN_3, 0x00},
        // Debounce on
        {REG_DEBOUNCE_DIS_1, 0x00},
        {REG_DEBOUNCE_DIS_2, 0x00},
        {REG_DEBOUNCE_DIS_3, 0x00},
        // Full 8x10 matrix: rows 0..7, columns 0..7, columns 8..9
        {REG_KP_GPIO_1, 0xFF},
        {REG_KP_GPIO_2, 0xFF},
        {REG_KP_GPIO_3, 0x03},
    };

    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        ESP_RETURN_ON_ERROR(write_reg(kbd, setup[i][0], setup[i][1]), TAG, "Writing register 0x%02X failed",
                            setup[i][0]);
    }
    return ESP_OK;
}

esp_err_t inputronic_keyboard_init(inputronic_keyboard_t *kbd, const inputronic_keyboard_config_t *config)
{
    ESP_RETURN_ON_FALSE(kbd != NULL && config != NULL && config->bus != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Invalid argument");

    memset(kbd, 0, sizeof(*kbd));
    kbd->keymap_upper = config->keymap_upper ? config->keymap_upper : &INPUTRONIC_KEYBOARD_KEYMAP_UPPER;
    kbd->keymap_lower = config->keymap_lower ? config->keymap_lower : &INPUTRONIC_KEYBOARD_KEYMAP_LOWER;
    if (config->shift_map != NULL) {
        kbd->shift_map = config->shift_map;
        kbd->shift_map_len = config->shift_map_len;
    } else {
        kbd->shift_map = INPUTRONIC_KEYBOARD_SHIFT_MAP;
        kbd->shift_map_len = INPUTRONIC_KEYBOARD_SHIFT_MAP_LEN;
    }
    kbd->caps_lock = true;

    if (i2c_master_probe(config->bus, config->address, I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "No keyboard at address 0x%02X", config->address);
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->address,
        .scl_speed_hz = config->scl_speed_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(config->bus, &dev_cfg, &kbd->dev), TAG, "Adding device failed");

    esp_err_t err = configure(kbd);
    if (err == ESP_OK) {
        err = inputronic_keyboard_clear_events(kbd, NULL);
    }
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(kbd->dev);
        kbd->dev = NULL;
    }
    return err;
}

esp_err_t inputronic_keyboard_deinit(inputronic_keyboard_t *kbd)
{
    ESP_RETURN_ON_FALSE(kbd != NULL && kbd->dev != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");
    esp_err_t err = i2c_master_bus_rm_device(kbd->dev);
    if (err == ESP_OK) {
        kbd->dev = NULL;
    }
    return err;
}

esp_err_t inputronic_keyboard_events_available(inputronic_keyboard_t *kbd, uint8_t *count)
{
    ESP_RETURN_ON_FALSE(kbd != NULL && count != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");
    uint8_t lck_ec;
    ESP_RETURN_ON_ERROR(read_reg(kbd, REG_KEY_LCK_EC, &lck_ec), TAG, "Reading event count failed");
    // Low nibble is the event counter
    *count = lck_ec & 0x0F;
    return ESP_OK;
}

esp_err_t inputronic_keyboard_read_event(inputronic_keyboard_t *kbd, inputronic_keyboard_event_t *event)
{
    ESP_RETURN_ON_FALSE(kbd != NULL && event != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");

    uint8_t raw;
    ESP_RETURN_ON_ERROR(read_reg(kbd, REG_KEY_EVENT_A, &raw), TAG, "Reading event failed");
    if (raw == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t code = raw & EVENT_CODE_MASK;
    if (code == 0 || code > EVENT_CODE_MAX) {
        ESP_LOGW(TAG, "Ignoring event 0x%02X", raw);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(event, 0, sizeof(*event));
    event->pressed = (raw & EVENT_PRESS) != 0;
    event->row = (code - 1) / INPUTRONIC_KEYBOARD_COLS;
    event->col = (code - 1) % INPUTRONIC_KEYBOARD_COLS;
    event->label = inputronic_keyboard_get_label(kbd, event->row, event->col);

    kbd->key_state[event->row][event->col] = event->pressed;
    if (!event->pressed) {
        return ESP_OK;
    }

    kbd->has_last = true;
    kbd->last_row = event->row;
    kbd->last_col = event->col;

    if (event->label == NULL) {
        return ESP_OK;
    }

    // CAPS toggles on press, the label then comes from the new keymap
    if (strcmp(event->label, "CAPS") == 0) {
        kbd->caps_lock = !kbd->caps_lock;
        event->label = inputronic_keyboard_get_label(kbd, event->row, event->col);
        return ESP_OK;
    }

    if (strcmp(event->label, "SPACE") == 0) {
        event->character = ' ';
    } else {
        char ch;
        if (inputronic_keyboard_label_to_char(kbd, event->label, &ch)) {
            event->character = ch;
        }
    }
    return ESP_OK;
}

esp_err_t inputronic_keyboard_clear_events(inputronic_keyboard_t *kbd, uint8_t *drained)
{
    ESP_RETURN_ON_FALSE(kbd != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");

    uint8_t count = 0;
    for (; count < FIFO_DRAIN_LIMIT; count++) {
        uint8_t raw;
        ESP_RETURN_ON_ERROR(read_reg(kbd, REG_KEY_EVENT_A, &raw), TAG, "Reading event failed");
        if (raw == 0) {
            break;
        }
    }

    // GPIO interrupt status clears on read
    uint8_t dummy;
    ESP_RETURN_ON_ERROR(read_reg(kbd, REG_GPIO_INT_STAT_1, &dummy), TAG, "Reading GPIO status failed");
    ESP_RETURN_ON_ERROR(read_reg(kbd, REG_GPIO_INT_STAT_2, &dummy), TAG, "Reading GPIO status failed");
    ESP_RETURN_ON_ERROR(read_reg(kbd, REG_GPIO_INT_STAT_3, &dummy), TAG, "Reading GPIO status failed");

    ESP_RETURN_ON_ERROR(write_reg(kbd, REG_INT_STAT, INT_STAT_GPI_INT | INT_STAT_K_INT), TAG,
                        "Clearing interrupt flags failed");

    if (drained != NULL) {
        *drained = count;
    }
    return ESP_OK;
}

const char *inputronic_keyboard_get_label(const inputronic_keyboard_t *kbd, uint8_t row, uint8_t col)
{
    if (kbd == NULL || row >= INPUTRONIC_KEYBOARD_ROWS || col >= INPUTRONIC_KEYBOARD_COLS) {
        return NULL;
    }
    return (*active_keymap(kbd))[row][col];
}

bool inputronic_keyboard_find_key(const inputronic_keyboard_t *kbd, const char *label, uint8_t *row, uint8_t *col)
{
    if (kbd == NULL || label == NULL || label[0] == '\0') {
        return false;
    }
    const inputronic_keyboard_keymap_t *map = active_keymap(kbd);
    for (uint8_t r = 0; r < INPUTRONIC_KEYBOARD_ROWS; r++) {
        for (uint8_t c = 0; c < INPUTRONIC_KEYBOARD_COLS; c++) {
            const char *l = (*map)[r][c];
            if (l != NULL && strcmp(l, label) == 0) {
                if (row != NULL) {
                    *row = r;
                }
                if (col != NULL) {
                    *col = c;
                }
                return true;
            }
        }
    }
    return false;
}

bool inputronic_keyboard_label_to_char(const inputronic_keyboard_t *kbd, const char *label, char *out)
{
    if (kbd == NULL || out == NULL || !is_single_char(label)) {
        return false;
    }

    char ch = label[0];
    if (inputronic_keyboard_is_label_pressed(kbd, "SHIFT")) {
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        } else {
            for (size_t i = 0; i < kbd->shift_map_len; i++) {
                if (kbd->shift_map[i].base == ch) {
                    ch = kbd->shift_map[i].shifted;
                    break;
                }
            }
        }
    }
    *out = ch;
    return true;
}

bool inputronic_keyboard_is_key_pressed(const inputronic_keyboard_t *kbd, uint8_t row, uint8_t col)
{
    if (kbd == NULL || row >= INPUTRONIC_KEYBOARD_ROWS || col >= INPUTRONIC_KEYBOARD_COLS) {
        return false;
    }
    return kbd->key_state[row][col];
}

bool inputronic_keyboard_is_label_pressed(const inputronic_keyboard_t *kbd, const char *label)
{
    uint8_t row, col;
    if (!inputronic_keyboard_find_key(kbd, label, &row, &col)) {
        return false;
    }
    return kbd->key_state[row][col];
}

bool inputronic_keyboard_any_key_pressed(const inputronic_keyboard_t *kbd)
{
    return inputronic_keyboard_held_count(kbd) > 0;
}

uint8_t inputronic_keyboard_held_count(const inputronic_keyboard_t *kbd)
{
    if (kbd == NULL) {
        return 0;
    }
    uint8_t n = 0;
    for (uint8_t r = 0; r < INPUTRONIC_KEYBOARD_ROWS; r++) {
        for (uint8_t c = 0; c < INPUTRONIC_KEYBOARD_COLS; c++) {
            if (kbd->key_state[r][c]) {
                n++;
            }
        }
    }
    return n;
}

bool inputronic_keyboard_get_last_key(const inputronic_keyboard_t *kbd, uint8_t *row, uint8_t *col,
                                      const char **label)
{
    if (kbd == NULL || !kbd->has_last) {
        return false;
    }
    if (row != NULL) {
        *row = kbd->last_row;
    }
    if (col != NULL) {
        *col = kbd->last_col;
    }
    if (label != NULL) {
        *label = inputronic_keyboard_get_label(kbd, kbd->last_row, kbd->last_col);
    }
    return true;
}

bool inputronic_keyboard_get_caps_lock(const inputronic_keyboard_t *kbd)
{
    return kbd != NULL && kbd->caps_lock;
}

void inputronic_keyboard_set_caps_lock(inputronic_keyboard_t *kbd, bool caps_lock)
{
    if (kbd != NULL) {
        kbd->caps_lock = caps_lock;
    }
}

esp_err_t inputronic_keyboard_string_input(inputronic_keyboard_t *kbd, char *buf, size_t buf_size, uint32_t timeout_ms,
                                           const char *end_label, const char *backspace_label)
{
    ESP_RETURN_ON_FALSE(kbd != NULL && buf != NULL && buf_size > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");

    if (end_label == NULL) {
        end_label = "ENTER";
    }
    if (backspace_label == NULL) {
        backspace_label = "BACK";
    }

    size_t len = 0;
    buf[0] = '\0';
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while (true) {
        inputronic_keyboard_event_t event;
        esp_err_t err = inputronic_keyboard_read_event(kbd, &event);

        if (err == ESP_ERR_NOT_FOUND) {
            if (timeout_ms > 0 && (xTaskGetTickCount() - start) >= timeout) {
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(INPUTRONIC_KEYBOARD_STRING_INPUT_POLL_MS) ?
                       pdMS_TO_TICKS(INPUTRONIC_KEYBOARD_STRING_INPUT_POLL_MS) :
                       1);
            continue;
        }
        if (err == ESP_ERR_INVALID_RESPONSE) {
            continue;
        }
        if (err != ESP_OK) {
            return err;
        }

        if (!event.pressed || event.label == NULL) {
            continue;
        }
        if (strcmp(event.label, end_label) == 0) {
            return ESP_OK;
        }
        if (strcmp(event.label, backspace_label) == 0) {
            if (len > 0) {
                buf[--len] = '\0';
            }
            continue;
        }
        if (event.character != 0 && len + 1 < buf_size) {
            buf[len++] = event.character;
            buf[len] = '\0';
        }
    }
}
