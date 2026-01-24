#ifndef FLASH_STORAGE_H
#define FLASH_STORAGE_H

#include <stdint.h>
#include <stdbool.h>

#define KEY_DATA_LEN 32

void flash_store_key(const uint8_t* key);
bool flash_retrieve_key(uint8_t* key);
void flash_erase_key(void);

#endif // FLASH_STORAGE_H
