cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD pico2)
set(PICO_PLATFORM rp2350-riscv)
set(PICO_USE_STACK_GUARDS ON)
# set(PICO_DEOPTIMIZED_DEBUG ON)
set(PICO_TOOLCHAIN_PATH /opt/riscv/)
set(PICO_COMMON_LANG_FLAGS " -march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp  -mabi=ilp32")

include(/usr/share/pico-sdk/external/pico_sdk_import.cmake)

project(picocalc_x86
        LANGUAGES C CXX ASM
        VERSION 0.0.1
        DESCRIPTION "PicoCalc x86 Emulator for PicoCalc on Pico 2."
)

pico_sdk_init()

include_directories(
  ${CMAKE_CURRENT_LIST_DIR}
)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/rp2040-psram rp2040-psram)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/pico_fatfs pico_fatfs)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/pico_x86 pico_x86)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/picocalc_display picocalc_display)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/picocalc_southbridge picocalc_southbridge)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/picocalc_sound picocalc_sound)


add_executable(picocalc_x86 picocalc_x86.c)


target_compile_definitions(picocalc_x86 PRIVATE
    PSRAM_MUTEX=1
    # PSRAM_SPINLOCK=0
    PSRAM_ASYNC=1
    PSRAM_PIN_CS=20
    PSRAM_PIN_SCK=21
    PSRAM_PIN_MOSI=2
    PSRAM_PIN_MISO=3
    TFT_CS_PIN=13 TFT_RST_PIN=15 TFT_LED_PIN=9 TFT_DC_PIN=14 TFT_DATA_PIN=11 TFT_CLK_PIN=10
    BIOS_EMBED
    # DEBUG_PERF
    # DEBUG_KBD
    DEBUG_CONSOLE
    DEBUG_EMS
)

target_compile_options(picocalc_x86 PRIVATE
	-fno-strict-aliasing
	-funroll-loops
	-fomit-frame-pointer
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
    picocalc_sound
	)

pico_enable_stdio_usb(picocalc_x86 1)
pico_enable_stdio_uart(picocalc_x86 0)
pico_add_extra_outputs(picocalc_x86)

pico_add_linker_script_override_path(picocalc_x86 ${CMAKE_CURRENT_LIST_DIR}/linker_scripts FILES default_text_excludes.incl default_rodata_excludes.incl)
