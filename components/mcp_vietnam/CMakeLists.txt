idf_component_register(
    SRCS "mcp_vietnam_tools.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        board_supermini
        audio_hal
        ui_eyes_clock
        chronos_ble
        espnow_mesh
        nioh_agent
        json
        esp_http_client
        esp-tls
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)

