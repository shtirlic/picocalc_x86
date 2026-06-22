project(picocalc_display
        VERSION 0.0.1
        DESCRIPTION "PicoCalc display PIO/SPI driver."
        )

add_library(picocalc_display INTERFACE)
target_sources(picocalc_display INTERFACE ${CMAKE_CURRENT_LIST_DIR}/picocalc_display.c)
target_link_libraries(picocalc_display INTERFACE hardware_pio hardware_clocks hardware_dma)
target_include_directories(picocalc_display INTERFACE ${CMAKE_CURRENT_LIST_DIR})
pico_generate_pio_header(picocalc_display ${CMAKE_CURRENT_LIST_DIR}/picocalc_display.pio)
