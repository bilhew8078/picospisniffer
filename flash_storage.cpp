#include "flash_storage.h"
#include "hardware/flash.h"
#include "pico/sync.h"  // Required for save_and_disable_interrupts()
#include <cstring>      // For memcpy/memset

// Store key at last flash sector to avoid program code
#define KEY_FLASH_ADDR (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

// In-memory key buffer (for quick retrieval)
static uint8_t cached_key[32] = {0};
static bool key_cached = false;

// Structure for flash storage
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t key[32];
    uint32_t crc;
} key_store_t;

static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

void flash_store_key(const uint8_t* key) {
    key_store_t store;
    store.magic = 0x424C4B59;  // "BLKY"
    store.version = 1;
    memcpy(store.key, key, 32);
    store.crc = crc32(store.key, 32);

    // Critical section: disable interrupts during flash write
    uint32_t ints = save_and_disable_interrupts();

    // Erase sector
    flash_range_erase(KEY_FLASH_ADDR, FLASH_SECTOR_SIZE);

    // Write key structure
    flash_range_program(KEY_FLASH_ADDR, (const uint8_t*)&store, sizeof(store));

    // Restore interrupts
    restore_interrupts(ints);

    // Update cache
    memcpy(cached_key, key, 32);
    key_cached = true;
}

bool flash_retrieve_key(uint8_t* key) {
    // Check cache first
    if (key_cached) {
        memcpy(key, cached_key, 32);
        return true;
    }

    // Read from flash
    const uint8_t* flash_ptr = (const uint8_t*)(XIP_BASE + KEY_FLASH_ADDR);
    const key_store_t* store = (const key_store_t*)flash_ptr;

    if (store->magic != 0x424C4B59) return false;

    // Verify CRC
    if (store->crc != crc32(store->key, 32)) return false;

    memcpy(key, store->key, 32);
    memcpy(cached_key, store->key, 32);
    key_cached = true;
    return true;
}

void flash_erase_key() {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(KEY_FLASH_ADDR, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    key_cached = false;
    memset(cached_key, 0, 32);
}
