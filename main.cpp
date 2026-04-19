#include <stdio.h>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "spi_sniffer.pio.h"

// --- Configuration ---
#define PIN_SPI0_CS   0
#define PIN_SPI0_SCK  1
#define PIN_SPI0_MOSI 2
#define PIN_SPI0_MISO 3
#define LED_PIN 25

//#define MAX_TRANSACTION_SIZE 64

// --- Named Constants ---
#define READWRITE_MASK 0x8000
#define ADDR_MASK 0x7000
#define DATA_MASK 0x0FFF

int main() {
    stdio_init_all();
    sleep_ms(2000); //wait for terminal to open

    setbuf(stdout, NULL);
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);  // Start with LED on
    bool ledstat = true;
    printf("\n=== SPI Sniffer Starting ===\n");

    // PIO Setup
    PIO pio = pio0;
    uint sm_mosi = 0, sm_miso = 1;
    uint offset = pio_add_program(pio, &spi_sniffer_program);

    auto setup_pio_sm = [&](uint sm, uint data_pin, uint cs_pin, uint sck_pin) {
        pio_sm_config c = spi_sniffer_program_get_default_config(offset);
        sm_config_set_clkdiv_int_frac8(&c, 250, 0);
        sm_config_set_in_pins(&c, data_pin);
        //sm_config_set_in_shift(&c, true, false, 8);
        sm_config_set_in_shift(&c, true, false, 31);
        gpio_set_function(cs_pin, GPIO_FUNC_PIO0);
        gpio_set_input_enabled(cs_pin, true);
        gpio_set_pulls(cs_pin, false, false);
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
        // Check PIO FIFO for SPI sent (MOSI):
        if(!pio_sm_is_rx_fifo_empty(pio, sm_mosi))
        {
            //uint32_t tempshit = pio_sm_get(pio, sm_mosi);
            //printf("MOSI tempshit=0x%x\n", tempshit);

            uint32_t mosi_word = (pio_sm_get(pio, sm_mosi) >> 16); //read the FIFO
            printf("MOSI WORD = 0x%08x  \n", mosi_word);
            /*
            if(!(mosi_word & READWRITE_MASK)) // bit 15 is low - WRITE
            {
                printf("WRITE: ");
                uint8_t address = (uint8_t)((mosi_word & ADDR_MASK) >> 8);
                printf("address=%d  ", address);
                uint16_t mosidata = mosi_word & DATA_MASK;
                printf("data=0x%x\n", mosidata);
                if(!pio_sm_is_rx_fifo_empty(pio, sm_miso))
                {
                    // this is a WRITE - if there is MISO data, trash it
                    uint16_t trash = (uint16_t)(pio_sm_get(pio, sm_miso) >> 16);
                }
            }
            else 
            {
                // bit 15 is high - READ
                printf("READ: ");
                uint8_t address = (uint8_t)(mosi_word & ADDR_MASK);
                printf("address=%d  ", address);
                if(pio_sm_is_rx_fifo_empty(pio, sm_miso))
                {
                    printf("\nNOTHING IN MISO FIFO!!\n");
                }
                else 
                {
                    uint16_t miso_word = (uint16_t)(pio_sm_get(pio, sm_miso) >> 16);
                    uint16_t misodata = miso_word & DATA_MASK;
                    printf("rcvdata=0x%x\n", misodata);
                }
            }
            */
            ledstat = !ledstat;
            gpio_put(LED_PIN, ledstat);
        }
    }
}





