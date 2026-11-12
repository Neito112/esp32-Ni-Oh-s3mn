idf_component_register(
    SRCS "zclaw_llm_client.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        board_supermini
        audio_hal
        ui_eyes_clock
        minimp3
        json
        esp_http_client
        esp_http_server
        esp_https_server
        esp-tls
        esp_wifi
        nvs_flash
        esp_websocket_client
    PRIV_REQUIRES
        esp_rom
        esp_timer
        esp_netif
        esp_hw_support
        lwip
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)

