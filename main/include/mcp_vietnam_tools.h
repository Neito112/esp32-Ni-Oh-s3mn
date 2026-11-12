#pragma once

/**
 * @file mcp_vietnam_tools.h
 * @brief Vietnamese Native MCP Registry — 10 công cụ MCP 100% Tiếng Việt cho ZClaw
 */

#include <atomic>
#include <cstdint>
#include <string>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace zclaw {

class McpRegistry {
public:
    static McpRegistry& instance();

    esp_err_t init();
    void deinit();
    bool is_initialized() const { return initialized_.load(); }

    /**
     * @brief Tạo mảng JSON OpenAI-compatible `tools` cho 10 công cụ MCP Tiếng Việt.
     *        Người gọi tự giải phóng bằng cJSON_Delete().
     */
    cJSON* build_tools_schema_json() const;

    /**
     * @brief Thực thi công cụ MCP theo tên và JSON tham số (UTF-8 tiếng Việt).
     * @return Chuỗi JSON kết quả bằng tiếng Việt tự nhiên.
     */
    std::string execute_tool(const char* tool_name, const char* args_json);

    // Tiện ích tính Âm lịch / Can Chi / Giờ Hoàng Đạo dùng chung
    static std::string get_vietnamese_time_lunar_json();

private:
    McpRegistry() = default;
    ~McpRegistry() = default;
    McpRegistry(const McpRegistry&) = delete;
    McpRegistry& operator=(const McpRegistry&) = delete;

    // 10 Tool Handlers
    std::string tool_xem_thoi_tiet(cJSON* args);
    std::string tool_xem_thoi_gian_lich_am(cJSON* args);
    std::string tool_doc_tin_tuc(cJSON* args);
    std::string tool_tra_cuu_thong_tin(cJSON* args);
    std::string tool_phat_nhac_truc_tuyen(cJSON* args);
    std::string tool_xem_video_truc_tuyen(cJSON* args);
    std::string tool_suy_luan_cao(cJSON* args);
    std::string tool_dieu_khien_thiet_bi(cJSON* args);
    std::string tool_gui_lenh_esp_now(cJSON* args);
    std::string tool_hen_gio_pomodoro(cJSON* args);

    // Pomodoro background timer + worker (không block esp_timer task)
    struct PomodoroEvent {
        char reminder[160];
    };
    esp_timer_handle_t pomodoro_timer_     = nullptr;
    QueueHandle_t      pomodoro_queue_     = nullptr;
    TaskHandle_t       pomodoro_task_      = nullptr;
    char               pomodoro_msg_[160]  = {};
    std::atomic<bool>  pomodoro_active_{false};
    std::atomic<bool>  initialized_{false};
    std::atomic<bool>  worker_running_{false};

    static void pomodoro_timer_cb(void* arg);
    static void pomodoro_worker_task(void* arg);
};

} // namespace zclaw

