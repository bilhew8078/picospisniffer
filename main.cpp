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
#define PIN_CS   0
#define PIN_SCK  1
#define PIN_MOSI 2
#define PIN_MISO 3
#define LED_PIN 25
#define PIO_INPUT_PIN_BASE PIN_MOSI

//#define MAX_TRANSACTION_SIZE 64

// --- Named Constants ---
#define READWRITE_MASK 0x8000
#define ADDR_MASK 0x7000
#define DATA_MASK 0x0FFF

void get_reg_string(uint8_t reg, char * regstring)
{
		switch(reg)
		{
			case 0:
			regstring[0] = 'C';
			regstring[1] = 'T';
			regstring[2] = 'L';
			regstring[3] = 0;
			break;
			case 1:
			regstring[0] = 'T';
			regstring[1] = 'R';
			regstring[2] = 'Q';
			regstring[3] = 0;
			break;
			case 2:
			regstring[0] = 'O';
			regstring[1] = 'F';
			regstring[2] = 'F';
			regstring[3] = 0;
			break;
            case 3:
			regstring[0] = 'B';
			regstring[1] = 'L';
			regstring[2] = 'K';
			regstring[3] = 0;
			break;
			case 4:
			regstring[0] = 'D';
			regstring[1] = 'K';
			regstring[2] = 'Y';
			regstring[3] = 0;
			break;
			case 5:
			regstring[0] = 'S';
			regstring[1] = 'T';
			regstring[2] = 'L';
			regstring[3] = 0;
			break;
			case 6:
			regstring[0] = 'D';
			regstring[1] = 'R';
			regstring[2] = 'V';
			regstring[3] = 0;
			break;
			case 7:
			regstring[0] = 'S';
			regstring[1] = 'T';
			regstring[2] = 'S';
			regstring[3] = 0;
			break;
			default:
			regstring[0] = 'O';
			regstring[1] = 'F';
			regstring[2] = 'F';
			regstring[3] = 0;						
		}
}


int main() {
    stdio_init_all();
    sleep_ms(2000); //wait for terminal to open

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);  // Start with LED on
    bool ledstat = true;
    printf("\n=== SPI Sniffer Starting ===\n");

    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_IN);
    gpio_init(PIN_SCK);
    gpio_set_dir(PIN_SCK, GPIO_IN);

    gpio_init(PIN_MOSI);
    gpio_set_dir(PIN_MOSI, GPIO_IN);

    // PIO Setup
    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &spi_sniffer_program);
    spi_sniffer_program_init(pio, sm, offset, PIO_INPUT_PIN_BASE);
    printf("PIO SETUP: sm=%d  offset=%d  pin=%d\n", sm, offset, PIO_INPUT_PIN_BASE);



    // --- Main Loop ---
    while (true) {
        // Check PIO FIFO for SPI sent (MOSI):
        if(!pio_sm_is_rx_fifo_empty(pio, sm))
        {
            uint16_t mosi_word = (uint16_t)(pio_sm_get(pio, sm)); //read the FIFO
            printf("MOSI WORD = 0x%04x  \n", mosi_word);
            if(!(mosi_word & READWRITE_MASK)) // bit 15 is low - WRITE
            {
                printf("WRITE: ");
                uint8_t address = (uint8_t)((mosi_word & ADDR_MASK) >> 12);
                char regstr[5];
                get_reg_string(address, regstr);
                printf("reg=%s  ", regstr);
                uint16_t mosidata = mosi_word & DATA_MASK;
                printf("data=0x%03x\n", mosidata);
                //if(!pio_sm_is_rx_fifo_empty(pio, sm_miso))
                //{
                    // this is a WRITE - if there is MISO data, trash it
                    //uint16_t trash = (uint16_t)(pio_sm_get(pio, sm_miso) >> 16);
                //}
            }
            else 
            {
                // bit 15 is high - READ
                printf("READ: ");
                uint8_t rdaddr = (uint8_t)((mosi_word & ADDR_MASK) >> 12);
                char regstr2[5];
                get_reg_string(rdaddr, regstr2);
                printf("reg=%s  ", regstr2);
                printf("data would be here...\n");
                /*
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
                */
            }
            ledstat = !ledstat;
            gpio_put(LED_PIN, ledstat);
        }
    }
}





