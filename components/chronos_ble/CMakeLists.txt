idf_component_register(
    SRCS "chronos_ble.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_supermini ui_eyes_clock bt nvs_flash
    PRIV_REQUIRES esp_timer
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)

