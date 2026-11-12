# Component "main" trong ESP-IDF v5 tự động link các component của IDF,
# components/ (minimp3) và managed_components/ (lvgl, esp_lvgl_port, littlefs, websocket).
# Lưu ý: Các file thuộc Giai đoạn 2 và 3 đã được khai báo sẵn để sẵn sàng build hoàn chỉnh sau Giai đoạn 3.

idf_component_register(
    SRCS
        # --- Giai đoạn 1 ---
        "src/audio_hal.cpp"
        # --- Giai đoạn 2 ---
        "src/ui_eyes_clock.cpp"
        "src/vn_font_16.c"
        "src/chronos_ble.cpp"
        "src/espnow_comm.cpp"
        # --- Giai đoạn 3 ---
        "src/zclaw_llm_client.cpp"
        "src/mcp_vietnam_tools.cpp"
        "src/main.cpp"
    INCLUDE_DIRS
        "include"
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)

