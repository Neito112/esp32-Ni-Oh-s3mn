idf_component_register(
    SRCS "audio_hal.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_supermini driver nvs_flash esp_timer
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)

