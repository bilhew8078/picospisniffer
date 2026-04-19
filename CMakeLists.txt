# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(spi_sniffer C CXX ASM)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(spi_sniffer
    main.cpp
)

# Generate PIO source files
pico_generate_pio_header(spi_sniffer ${CMAKE_CURRENT_LIST_DIR}/spi_sniffer.pio)


target_link_libraries(spi_sniffer
    pico_stdlib
    hardware_gpio
    hardware_flash
    hardware_pio
)

pico_enable_stdio_usb(spi_sniffer 1)
pico_enable_stdio_uart(spi_sniffer 0)

pico_add_extra_outputs(spi_sniffer)
