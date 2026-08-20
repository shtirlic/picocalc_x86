cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-riscv)
set(PICO_USE_STACK_GUARDS ON)
# set(PICO_DEOPTIMIZED_DEBUG ON)
set(PICO_TOOLCHAIN_PATH /opt/riscv/)
set(PICO_COMMON_LANG_FLAGS " -march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb  -mabi=ilp32")
include(/usr/share/pico-sdk/external/pico_sdk_import.cmake)
set(PICO_DEFAULT_BOOT_STAGE2_FILE "${CMAKE_CURRENT_LIST_DIR}/empty_boot2.S")


project(picocalc_x86
        LANGUAGES C CXX ASM
        VERSION 0.0.6
        DESCRIPTION "PicoCalc x86 Emulator on Raspberry Pi Pico 2"
)

pico_sdk_init()

include_directories(
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/rp2040-psram rp2040-psram)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/pico_fatfs pico_fatfs)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/picocalc_display picocalc_display)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/picocalc_southbridge picocalc_southbridge)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/pico_x86 pico_x86)

add_executable(picocalc_x86 picocalc_x86.c picocalc_x86_screenshot.c)

configure_file(config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

target_compile_definitions(picocalc_x86 PRIVATE
    PSRAM_MUTEX=1
    # PSRAM_SPINLOCK=0
    PSRAM_ASYNC=1
    PSRAM_PIN_CS=20
    PSRAM_PIN_SCK=21
    PSRAM_PIN_MOSI=2
    PSRAM_PIN_MISO=3
    AUDIO_PIN_L=26
    AUDIO_PIN_R=27
    BIOS_EMBED_ONLY
    PICO_SYS_CLOCK_MHZ=250
    PICO_XOSC_STARTUP_DELAY_MULTIPLIER=128
    # DEBUG_KBD
    # DEBUG_CONSOLE
)

target_compile_options(picocalc_x86 PRIVATE
	-fno-strict-aliasing
	-funroll-loops
	-fomit-frame-pointer
    --param max-inline-insns-auto=200
    -Wall
    -Wno-unused-value
    -Wno-unused-but-set-variable
    -Wno-unused-variable
)

target_link_options(picocalc_x86 PRIVATE
	"-Wl,--print-memory-usage"
)

add_custom_command(TARGET picocalc_x86 POST_BUILD
    COMMAND size -A -d --format=SysV $<TARGET_FILE:picocalc_x86>
    COMMENT "Section sizes:"
)

target_link_libraries(picocalc_x86
    pico_stdlib
    pico_multicore
    pico_aon_timer
    pico_rand
    hardware_flash
    hardware_irq
    hardware_pwm
    hardware_i2c
    hardware_dma
    hardware_exception
    hardware_pio
    hardware_hazard3
    rp2040-psram
    pico_fatfs
    pico_x86
    picocalc_display
	picocalc_southbridge
    pico_util
	)

pico_enable_stdio_usb(picocalc_x86 0)
pico_enable_stdio_uart(picocalc_x86 0)
pico_add_extra_outputs(picocalc_x86)

pico_add_linker_script_override_path(picocalc_x86 ${CMAKE_CURRENT_LIST_DIR}/linker_scripts FILES default_text_excludes.incl default_rodata_excludes.incl)
