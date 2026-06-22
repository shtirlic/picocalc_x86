cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(pico_x86
    LANGUAGES C CXX ASM
    VERSION 0.0.1
    DESCRIPTION "Raspberry Pi Pico 2 x86 emulator"
)
add_library(pico_x86 INTERFACE)

target_sources(pico_x86 INTERFACE
	pico_x86.c
    bios_embed.S
    pico_x86_video.c
)

target_include_directories(pico_x86 INTERFACE ${CMAKE_CURRENT_LIST_DIR} )


find_program(NASM_CMD nasm)
if(NOT NASM_CMD)
    message(FATAL_ERROR "NASM compiler not found! Please install NASM and add it to your PATH.")
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bios.bin
    COMMAND ${NASM_CMD} -i ${CMAKE_CURRENT_SOURCE_DIR}  -f bin ${CMAKE_CURRENT_SOURCE_DIR}/bios.asm -o ${CMAKE_CURRENT_BINARY_DIR}/bios.bin -l ${CMAKE_CURRENT_BINARY_DIR}/bios.lst
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bios.asm
    COMMENT "Assembling PicoCalc x86 BIOS"
    VERBATIM
)
add_custom_target(
    build_bios ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/bios.bin")

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/PICOEMS.SYS
    COMMAND ${NASM_CMD}  -f bin ${CMAKE_CURRENT_SOURCE_DIR}/pico-ems.asm -o ${CMAKE_CURRENT_BINARY_DIR}/PICOEMS.SYS
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pico-ems.asm
    COMMENT "Assembling PicoCalc x86 PICOEMS.SYS EMS driver for DOS"
    VERBATIM
)
add_custom_target(
    build_ems_driver ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/PICOEMS.SYS
)
