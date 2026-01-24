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
  - `GPIO 2` -> SCK
  - `GPIO 3` -> MOSI
  - `GPIO 4` -> MISO
  - `GPIO 5` -> CS
  
## Usage
Connect via USB Serial (115200 baud). Use the following commands:

| Command | Description |
| :--- | :--- |
| `status` | Show transaction count, key status, and buffer levels. |
| `getkey` | Print the stored BitLocker key (hex). |
| `stream` | Toggle live hex stream of TPM data. Format: `[R]FF` (Read) or `[W]01` (Write). |
| `erasekey` | Delete the key from flash memory. |
| `reset` | Clear buffers, reset window, and zero out stats. |

## Output Example
```text
> stream
[*] Streaming TPM Data STARTED (RW)
DATA: [W]80 [W]01 [W]00 [W]00
DATA: [R]00 [R]00 [R]00 [R]00
DATA: [W]2C [W]00 [W]00 [W]04
...
[[[ !!! KEY FOUND !!! ]]]
Stored to Flash. Use 'getkey' to view.
```

## Extraction Sequence

1. **Power on Pico** - The PIO engine begins monitoring immediately.
2. **Boot target system** - BitLocker解锁.
3. **Key detected** - Automatically validated via CRC32 and written to flash.
4. **Connect USB** - run `screen /dev/ttyACM0 115200`.
5. **Type `getkey`** - retrieves the 64-character hex key.
6. **Type `erasekey`** - Secure erase after extraction.

**Architecture Note:** This firmware uses the RP2040's PIO to handle the high signal rates of modern TPMs. The sliding window logic handles split-packet data, and flash storage routines are RAM-resident to ensure critical write operations do not collide with XIP (Execute-In-Place) fetches.

## Build & Deployment

**Prerequisites:**
- Pico SDK
- CMake
- ARM GCC Toolchain

**Steps:**
```bash
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

**Important:** Ensure your `spi_sniffer.pio` file is included in your `CMakeLists.txt` to generate the assembly header:
```cmake
pico_generate_pio_header(picoblky ${CMAKE_CURRENT_LIST_DIR}/spi_sniffer.pio)
```

**Flash to Pico (hold BOOTSEL, copy UF2):**
```bash
cp picoblky.uf2 /media/$USER/RPI-RP2/
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

