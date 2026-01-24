# BitLocker TPM Sniffer (RP2040) - PIO High-Speed Edition

Firmware for the Raspberry Pi Pico to sniff TPM SPI traffic using the Pico's Programmable I/O (PIO) engine. This ensures **high-speed, reliable capture** (up to ~20+ MHz) without dropping data, extracts BitLocker recovery keys, and streams live data to a serial console.

## Features
- **PIO-Based Capture:** Uses the RP2040's PIO state machine hardware to capture SPI traffic bit-by-bit. This eliminates the race conditions associated with CPU interrupts, ensuring reliable operation even at high TPM clock speeds.
- **Bit-Level Reliability:** Logic is ported from the standard BitLocker analyzer (Saleae Logic 2), ensuring compatibility with command structures and sliding-window key detection.
- **Key Extraction:** Automatically detects and saves BitLocker keys to internal flash.
- **Live Streaming:** Real-time hex dump of TPM data (labeled Read/Write) via USB.
- **Robust Flash Storage:** Retains keys even after power loss. Flash write routines execute from RAM to prevent bus locking.

## Hardware
- **Board:** Raspberry Pi Pico (RP2040).
- **Connections:** Connect to the TPM SPI bus (3.3V logic).
  - **GPIO 1** -> SCK
  - **GPIO 2** -> MOSI
  - **GPIO 3** -> MISO
  - **GPIO 0** -> CS

## Usage
Connect via USB Serial (115200 baud). Use the following commands:

| Command | Description |
| :--- | :--- |
| `status` | Show transaction count and keys found. |
| `getkey` | Print the stored BitLocker key (hex). |
| `erasekey` | Delete the key from flash memory. |
| `stream` | Toggle live hex stream of TPM data. |
| `stats` | Show current window length. |
| `clear` | Clear statistics (transactions, keys found, window). |
| `reset` | Reset state machine and clear buffers. |

## Output Example
```text
> stream
[*] Streaming TPM Data STARTED (RW)
DATA: [W]80 [W]01 [W]00 [W]00
DATA: [R]00 [R]00 [R]00 [R]00
DATA: [W]2C [W]00 [W]00 [W]04
...
[[[ !!! KEY FOUND AND STORED !!! ]]]\nStored to Flash. Use 'getkey' to view.
```

## Extraction Sequence

1. **Power on Pico** - The PIO engine begins monitoring immediately.
2. **Boot target system** - BitLocker unlocks.
3. **Key detected** - Automatically validated and written to flash.
4. **Connect USB** - run `screen /dev/ttyACM0 115200`.
5. **Type `getkey`** - retrieves the 64-character hex key.
6. **Type `erasekey`** - Secure erase after extraction.

**Architecture Note:** This firmware uses the RP2040's PIO to handle the high signal rates of modern TPMs. The sliding window logic handles split-packet data, and flash storage routines are RAM-resident to ensure critical write operations do not collide with XIP (Execute-In-Place) fetches.

## Build & Deployment


**Steps:**
```bash
# Install packages
sudo apt install cmake python3 build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib

# Clone SDK and setup
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init && cd ..

# Copy import file
cp pico-sdk/external/pico_sdk_import.cmake .

# Build
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=../pico-sdk
make -j4
```

**Flash to Pico (hold BOOTSEL, copy UF2):**
```bash
cp picoblky.uf2 /media/$USER/RPI-RP2/
```

## Pinout & Connection

| Pico Pin | Signal | Connect To |
|----------|--------|------------|
| GPIO 0   | CS     | TPM SPI CS# |
| GPIO 1   | SCK    | TPM SPI CLK |
| GPIO 2   | MOSI   | TPM SPI MOSI |
| GPIO 3   | MISO   | TPM SPI MISO |
| GND      | GND    | System GND |

**⚠️ Hardware Warning**: The Pico's GPIO inputs are 3.3V only. Use level shifters if TPM operates at 1.8V or 5V.


