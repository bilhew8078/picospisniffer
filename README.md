# BitLocker TPM Sniffer (RP2040)

Firmware for the Raspberry Pi Pico to sniff TPM SPI traffic, extract BitLocker recovery keys, and stream live data to a serial console.

## Features
- **Passive Sniffing:** Monitors SPI bus non-intrusively.
- **Key Extraction:** Automatically detects and saves BitLocker keys to internal flash.
- **Live Streaming:** Real-time hex dump of TPM data (labeled Read/Write) via USB.
- **Flash Storage:** Retains keys even after power loss.

## Hardware
- **Board:** Raspberry Pi Pico (RP2040).
- **Connections:** Connect to the TPM SPI bus (3.3V logic).
  - `GPIO 2` -> SCK
  - `GPIO 3` -> MOSI
  - `GPIO 4` -> MISO
  - `GPIO 5` -> CS
  
## Usage
Connect via USB Serial (115200 baud). Use the following commands:

| Command | Description |
| :--- | :--- |
| `status` | Show transaction count and key status. |
| `getkey` | Print the stored BitLocker key (hex). |
| `stream` | Toggle live hex stream of TPM data. Format: `[R]FF` (Read) or `[W]01` (Write). |
| `erasekey` | Delete the key from flash memory. |
| `reset` | Clear buffers and reset window. |

## Output Example
```text
> stream
[*] Streaming TPM Data STARTED (RW)
DATA: [W]80 [W]01 [W]00 [W]00
DATA: [R]00 [R]00 [R]00 [R]00
DATA: [W]2C [W]00 [W]00 [W]04
```

## Extraction Sequence

1. **Power on Pico** - begins monitoring immediately
2. **Boot target system** - normal BitLocker unlock
3. **Key detected** - automatically written to flash
4. **Connect USB** - run `screen /dev/ttyACM0 115200`
5. **Type `getkey`** - retrieves 64-character hex key
6. **Type `erasekey`** - secure erase after extraction

The key persists through power cycles and is stored in the **last flash sector** (safe from program code). The `flash_store_key()` function uses atomic erase-program with CRC32 verification to ensure data integrity.

## Build & Deployment

```bash
# Clone SDK and build
sudo apt install cmake python3 build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init && cd ..
cp pico-sdk/external/pico_sdk_import.cmake .
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=../pico-sdk
make -j4

# Flash to Pico (hold BOOTSEL, copy UF2)
cp bitlocker_sniffer.uf2 /media/$USER/RPI-RP2/

# Connect USB, then in terminal:
screen /dev/ttyACM0 115200
# Commands: status, getkey, erasekey, reset
```


## Pinout & Connection

| Pico Pin | Signal | Connect To |
|----------|--------|------------|
| GPIO2    | SCK    | TPM SPI CLK |
| GPIO3    | MOSI   | TPM SPI MOSI |
| GPIO4    | MISO   | TPM SPI MISO |
| GPIO5    | CS     | TPM SPI CS# |
| GND      | GND    | System GND |

**⚠️ Hardware Warning**: The Pico's GPIO inputs are 3.3V only. Use level shifters if TPM operates at 1.8V or 5V.
