idf_component_register(
    SRCS
        "ui_eyes_clock.cpp"
        "vn_font_16.c"
    INCLUDE_DIRS "include"
    REQUIRES board_supermini audio_hal esp_lcd lvgl esp_lvgl_port nvs_flash esp_timer
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)

