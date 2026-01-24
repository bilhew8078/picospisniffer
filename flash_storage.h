#pragma once
#include <stdint.h>
#include <stdbool.h>

// Store a 32-byte BitLocker key to flash
void flash_store_key(const uint8_t* key);

// Retrieve a stored key (returns false if none found)
bool flash_retrieve_key(uint8_t* key);

// Erase any stored key
void flash_erase_key();
