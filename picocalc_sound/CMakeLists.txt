
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

project(picocalc_sound
        DESCRIPTION "picocalc_sound for rp2040."
        )

add_library(picocalc_sound INTERFACE)
target_sources(picocalc_sound INTERFACE picocalc_sound.c)
target_link_libraries(picocalc_sound INTERFACE  pico_stdlib hardware_pwm)
target_include_directories(picocalc_sound INTERFACE ${CMAKE_CURRENT_LIST_DIR})
