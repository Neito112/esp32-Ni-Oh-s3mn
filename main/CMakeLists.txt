idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        board_supermini
        audio_hal
        ui_eyes_clock
        chronos_ble
        espnow_mesh
        nioh_agent
        mcp_vietnam
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        littlefs
        mdns
        lwip
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)

