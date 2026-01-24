#include "flash_storage.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <cstring>

#define KEY_FLASH_ADDR (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

static uint8_t cached_key[KEY_DATA_LEN] = {0};
static bool key_cached = false;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t key[KEY_DATA_LEN];
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

// CRITICAL: Must run from RAM during flash operations
void __attribute__((section(".ramcode"))) flash_store_key(const uint8_t* key) {
    key_store_t store;
    store.magic = 0x424C4B59; // "BLKY"
    store.version = 1;
    memcpy(store.key, key, KEY_DATA_LEN);
    store.crc = crc32((const uint8_t*)&store, sizeof(key_store_t) - sizeof(uint32_t));

    uint8_t write_buffer[256];
    memset(write_buffer, 0xFF, 256);
    memcpy(write_buffer, &store, sizeof(store));

    // CRITICAL: Update RAM cache BEFORE flash erase
    memcpy(cached_key, key, KEY_DATA_LEN);
    key_cached = true;

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(KEY_FLASH_ADDR, FLASH_SECTOR_SIZE);
    flash_range_program(KEY_FLASH_ADDR, write_buffer, 256);
    restore_interrupts(ints);
}

bool flash_retrieve_key(uint8_t* key) {
    if (key_cached) {
        memcpy(key, cached_key, KEY_DATA_LEN);
        return true;
    }

    const uint8_t* flash_ptr = (const uint8_t*)(XIP_BASE + KEY_FLASH_ADDR);
    const key_store_t* store = (const key_store_t*)flash_ptr;

    if (store->magic != 0x424C4B59) return false;
    if (store->crc != crc32((const uint8_t*)store, sizeof(key_store_t) - sizeof(uint32_t))) return false;

    memcpy(key, store->key, KEY_DATA_LEN);
    memcpy(cached_key, store->key, KEY_DATA_LEN);
    key_cached = true;
    return true;
}

void __attribute__((section(".ramcode"))) flash_erase_key() {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(KEY_FLASH_ADDR, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    key_cached = false;
    memset(cached_key, 0, KEY_DATA_LEN);
}
