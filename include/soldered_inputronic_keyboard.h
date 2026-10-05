/**
 * @file soldered_inputronic_keyboard.h
 * @brief Public API for the soldered-inputronic-keyboard component
 *
 * ESP-IDF driver for the Soldered Inputronic KEYBOARD, an 80 key 8x10 matrix
 * scanned by a TI TCA8418. The TCA8418 scans and debounces the matrix on its
 * own and queues up to 10 press/release events in a FIFO, which this driver
 * reads over I2C. The INT pin is not used, the FIFO is polled.
 *
 * Every event is decoded into a matrix position, a key label from the keymap
 * and, for keys that type something, a character with SHIFT and caps lock
 * applied. CAPS toggles caps lock, which switches between the upper and lower
 * keymaps. Behaviour follows the Soldered Arduino library.
 *
 * A keyboard handle is not thread safe, use it from one task or guard it with
 * a mutex.
 *
 * @author Soldered Electronics
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/** I2C address the keyboard answers on */
#define INPUTRONIC_KEYBOARD_DEFAULT_ADDRESS 0x34

/** Default I2C clock, the TCA8418 handles up to 1 MHz */
#define INPUTRONIC_KEYBOARD_DEFAULT_SCL_SPEED_HZ 400000

/** Rows in the key matrix */
#define INPUTRONIC_KEYBOARD_ROWS 8

/** Columns in the key matrix */
#define INPUTRONIC_KEYBOARD_COLS 10

/**
 * @brief Key labels for every matrix position, indexed [row][col]
 *
 * Labels are strings such as "a", "7", "SHIFT", "ENTER" or "SPACE", NULL where
 * no key is fitted. A single character label types that character. The driver
 * recognises "CAPS" (toggles caps lock) and "SHIFT" (held modifier) by label,
 * and "SPACE" types a space.
 */
typedef const char *const inputronic_keyboard_keymap_t[INPUTRONIC_KEYBOARD_ROWS][INPUTRONIC_KEYBOARD_COLS];

/** What a character turns into while SHIFT is held */
typedef struct {
    char base;    /**< Character without SHIFT */
    char shifted; /**< Character with SHIFT */
} inputronic_keyboard_shift_pair_t;

/** Layout printed on the board with caps lock on, the one the keyboard starts in */
extern const inputronic_keyboard_keymap_t INPUTRONIC_KEYBOARD_KEYMAP_UPPER;

/** Layout printed on the board with caps lock off */
extern const inputronic_keyboard_keymap_t INPUTRONIC_KEYBOARD_KEYMAP_LOWER;

/** Default SHIFT table: numbers to the symbols above them, ';' ',' '.' to ':' ';' ':' */
extern const inputronic_keyboard_shift_pair_t INPUTRONIC_KEYBOARD_SHIFT_MAP[];

/** Number of entries in ::INPUTRONIC_KEYBOARD_SHIFT_MAP */
extern const size_t INPUTRONIC_KEYBOARD_SHIFT_MAP_LEN;

/**
 * @brief How the keyboard is wired up and which layout it uses
 *
 * The I2C bus belongs to the application, so that other devices can share it.
 * Create it with i2c_new_master_bus() first and hand it over here.
 *
 * Leave the keymap and shift map fields NULL for the layout printed on the
 * board. Custom tables must stay valid for as long as the keyboard is used.
 */
typedef struct {
    i2c_master_bus_handle_t bus;                       /**< Bus the keyboard is wired to */
    uint8_t address;                                   /**< ::INPUTRONIC_KEYBOARD_DEFAULT_ADDRESS */
    uint32_t scl_speed_hz;                             /**< I2C clock for this device */
    const inputronic_keyboard_keymap_t *keymap_upper;  /**< Labels with caps lock on, NULL for the default */
    const inputronic_keyboard_keymap_t *keymap_lower;  /**< Labels with caps lock off, NULL for the default */
    const inputronic_keyboard_shift_pair_t *shift_map; /**< SHIFT table, NULL for the default */
    size_t shift_map_len;                              /**< Entries in shift_map, ignored when it is NULL */
} inputronic_keyboard_config_t;

/**
 * @brief Configuration for a keyboard on the given bus, default address and layout
 * @param b I2C bus handle from i2c_new_master_bus()
 */
#define INPUTRONIC_KEYBOARD_CONFIG_DEFAULT(b)                                                                          \
    {                                                                                                                  \
        .bus = (b), .address = INPUTRONIC_KEYBOARD_DEFAULT_ADDRESS,                                                    \
        .scl_speed_hz = INPUTRONIC_KEYBOARD_DEFAULT_SCL_SPEED_HZ, .keymap_upper = NULL, .keymap_lower = NULL,          \
        .shift_map = NULL, .shift_map_len = 0                                                                          \
    }

/** One key press or release */
typedef struct {
    bool pressed;      /**< true for a press, false for a release */
    uint8_t row;       /**< Matrix row, 0..7 */
    uint8_t col;       /**< Matrix column, 0..9 */
    const char *label; /**< Label from the active keymap, NULL for an unmapped position */
    char character;    /**< What a press types with SHIFT and caps lock applied, ' ' for SPACE, 0 if nothing */
} inputronic_keyboard_event_t;

/**
 * @brief Driver state, allocated by the application and filled in by inputronic_keyboard_init()
 *
 * The fields are internal, use the functions below instead of reading them.
 */
typedef struct {
    i2c_master_dev_handle_t dev;
    const inputronic_keyboard_keymap_t *keymap_upper;
    const inputronic_keyboard_keymap_t *keymap_lower;
    const inputronic_keyboard_shift_pair_t *shift_map;
    size_t shift_map_len;
    bool key_state[INPUTRONIC_KEYBOARD_ROWS][INPUTRONIC_KEYBOARD_COLS];
    bool caps_lock;
    bool has_last;
    uint8_t last_row;
    uint8_t last_col;
} inputronic_keyboard_t;

/**
 * @brief Add the keyboard to the bus and configure the TCA8418
 *
 * Probes the address first, so a keyboard that is not plugged in is reported
 * as ESP_ERR_NOT_FOUND. Sets up the full 8x10 matrix with debounce on and
 * interrupts off, then empties the event FIFO. Caps lock starts on.
 *
 * @param[out] kbd    Keyboard handle to initialize
 * @param[in]  config Bus, address and layout
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for a NULL argument or bus,
 *         ESP_ERR_NOT_FOUND if nothing answers at the address, or the I2C error
 */
esp_err_t inputronic_keyboard_init(inputronic_keyboard_t *kbd, const inputronic_keyboard_config_t *config);

/**
 * @brief Remove the keyboard from the bus
 *
 * @param[in] kbd Keyboard handle
 *
 * @return ESP_OK on success, or the error from i2c_master_bus_rm_device()
 */
esp_err_t inputronic_keyboard_deinit(inputronic_keyboard_t *kbd);

/**
 * @brief Number of events waiting in the FIFO
 *
 * @param[in]  kbd   Keyboard handle
 * @param[out] count Events waiting, 0..10
 *
 * @return ESP_OK on success, or the I2C error
 */
esp_err_t inputronic_keyboard_events_available(inputronic_keyboard_t *kbd, uint8_t *count);

/**
 * @brief Read and decode the oldest event in the FIFO
 *
 * Updates which keys are held, and toggles caps lock when CAPS is pressed
 * (the event's label already comes from the new keymap). Call it until it
 * returns ESP_ERR_NOT_FOUND to drain the FIFO; the TCA8418 holds 10 events and
 * drops anything past that.
 *
 * @param[in]  kbd   Keyboard handle
 * @param[out] event Decoded event
 *
 * @return ESP_OK with an event, ESP_ERR_NOT_FOUND if the FIFO is empty,
 *         ESP_ERR_INVALID_RESPONSE for a key code outside the matrix, or the I2C error
 */
esp_err_t inputronic_keyboard_read_event(inputronic_keyboard_t *kbd, inputronic_keyboard_event_t *event);

/**
 * @brief Throw away every event in the FIFO and clear the interrupt flags
 *
 * Held key state is left as it is.
 *
 * @param[in]  kbd     Keyboard handle
 * @param[out] drained Number of events thrown away, may be NULL
 *
 * @return ESP_OK on success, or the I2C error
 */
esp_err_t inputronic_keyboard_clear_events(inputronic_keyboard_t *kbd, uint8_t *drained);

/**
 * @brief Label at a matrix position in the active keymap
 *
 * @param[in] kbd Keyboard handle
 * @param[in] row Matrix row
 * @param[in] col Matrix column
 *
 * @return The label, or NULL for an unmapped or out of range position
 */
const char *inputronic_keyboard_get_label(const inputronic_keyboard_t *kbd, uint8_t row, uint8_t col);

/**
 * @brief Find the matrix position of a label in the active keymap
 *
 * @param[in]  kbd   Keyboard handle
 * @param[in]  label Label to look for, such as "SHIFT" or "a"
 * @param[out] row   Matrix row
 * @param[out] col   Matrix column
 *
 * @return true if the label was found
 */
bool inputronic_keyboard_find_key(const inputronic_keyboard_t *kbd, const char *label, uint8_t *row, uint8_t *col);

/**
 * @brief Character a single character label types right now
 *
 * While SHIFT is held, letters invert their case and anything in the SHIFT
 * table is replaced.
 *
 * @param[in]  kbd   Keyboard handle
 * @param[in]  label Label from the keymap
 * @param[out] out   Character
 *
 * @return true if the label is a single character, false for names such as "ENTER"
 */
bool inputronic_keyboard_label_to_char(const inputronic_keyboard_t *kbd, const char *label, char *out);

/**
 * @brief Whether the key at a matrix position is held
 *
 * Held state comes from the events read so far, so it is only current after
 * the FIFO has been drained.
 */
bool inputronic_keyboard_is_key_pressed(const inputronic_keyboard_t *kbd, uint8_t row, uint8_t col);

/** @brief Whether the key with this label in the active keymap is held */
bool inputronic_keyboard_is_label_pressed(const inputronic_keyboard_t *kbd, const char *label);

/** @brief Whether any key is held */
bool inputronic_keyboard_any_key_pressed(const inputronic_keyboard_t *kbd);

/** @brief Number of keys held */
uint8_t inputronic_keyboard_held_count(const inputronic_keyboard_t *kbd);

/**
 * @brief The last key pressed
 *
 * @param[in]  kbd   Keyboard handle
 * @param[out] row   Matrix row, may be NULL
 * @param[out] col   Matrix column, may be NULL
 * @param[out] label Its label in the active keymap, may be NULL
 *
 * @return true if a key has been pressed since inputronic_keyboard_init()
 */
bool inputronic_keyboard_get_last_key(const inputronic_keyboard_t *kbd, uint8_t *row, uint8_t *col,
                                      const char **label);

/** @brief Whether caps lock is on, the upper keymap is active */
bool inputronic_keyboard_get_caps_lock(const inputronic_keyboard_t *kbd);

/** @brief Turn caps lock on or off, switching between the upper and lower keymap */
void inputronic_keyboard_set_caps_lock(inputronic_keyboard_t *kbd, bool caps_lock);

/**
 * @brief Block until a line has been typed
 *
 * Collects what the keys type until end_label is pressed or the timeout runs
 * out, polling the FIFO every ::INPUTRONIC_KEYBOARD_STRING_INPUT_POLL_MS.
 * backspace_label deletes the last character. Characters past the end of buf
 * are dropped. buf is always NUL terminated and holds what was typed, also on
 * timeout.
 *
 * @param[in]  kbd             Keyboard handle
 * @param[out] buf             Typed text
 * @param[in]  buf_size        Size of buf including the NUL, at least 1
 * @param[in]  timeout_ms      Give up after this long, 0 to wait for ever
 * @param[in]  end_label       Key that ends input, NULL for "ENTER"
 * @param[in]  backspace_label Key that deletes, NULL for "BACK"
 *
 * @return ESP_OK when end_label was pressed, ESP_ERR_TIMEOUT when the timeout
 *         ran out, ESP_ERR_INVALID_ARG for a NULL or empty buffer, or the I2C error
 */
esp_err_t inputronic_keyboard_string_input(inputronic_keyboard_t *kbd, char *buf, size_t buf_size, uint32_t timeout_ms,
                                           const char *end_label, const char *backspace_label);

/** How often inputronic_keyboard_string_input() reads the FIFO */
#define INPUTRONIC_KEYBOARD_STRING_INPUT_POLL_MS 10

#ifdef __cplusplus
}
#endif
