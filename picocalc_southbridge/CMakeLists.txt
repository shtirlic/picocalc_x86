set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

project(picocalc_southbridge
        DESCRIPTION "PicoCalc southbridge i2c driver."
        )

add_library(picocalc_southbridge INTERFACE)
target_sources(picocalc_southbridge INTERFACE ${CMAKE_CURRENT_LIST_DIR}/picocalc_southbridge.c)
target_link_libraries(picocalc_southbridge INTERFACE pico_stdlib hardware_spi hardware_i2c)
target_include_directories(picocalc_southbridge INTERFACE ${CMAKE_CURRENT_LIST_DIR})
