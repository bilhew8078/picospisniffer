cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(bitlocker_sniffer C CXX ASM)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(bitlocker_sniffer
    main.cpp
    flash_storage.cpp
)

target_link_libraries(bitlocker_sniffer
    pico_stdlib
    hardware_gpio
    hardware_flash
)

pico_enable_stdio_usb(bitlocker_sniffer 1)
pico_enable_stdio_uart(bitlocker_sniffer 0)

pico_add_extra_outputs(bitlocker_sniffer)
