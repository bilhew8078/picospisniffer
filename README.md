# Pico SPI Sniffer for TI Stepper Controller DRV8711

# Code modified from "BitLocker TPM Sniffer - PIO High-Speed Edition"

# This program "simply" monitors the MOSI and MISO lines and prints out
#   WRITE/READ, the 3 bit Address, and the 12 bit DATA sent or received




## Pinout & Connection

| Pico Pin | Signal | Connect To |
|----------|--------|------------|
| GPIO 0   | CS     | TPM SPI CS# |
| GPIO 1   | SCK    | TPM SPI CLK |
| GPIO 2   | MOSI   | TPM SPI MOSI |
| GPIO 3   | MISO   | TPM SPI MISO |
| GND      | GND    | System GND |

**⚠️ Hardware Warning**: The Pico's GPIO inputs are 3.3V only. Use level shifters if TPM operates at 1.8V or 5V.


