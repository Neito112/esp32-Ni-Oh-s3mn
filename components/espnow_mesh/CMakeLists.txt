idf_component_register(
    SRCS "espnow_comm.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_supermini audio_hal ui_eyes_clock esp_wifi esp_event nvs_flash
    PRIV_REQUIRES esp_hw_support
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)

