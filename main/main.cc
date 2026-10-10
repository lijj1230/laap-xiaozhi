#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <vector>

#include "audio_codec.h"
#include "board.h"

// LAAP 意识组件（components/consciousness）——声明须在 std 类型可见之后
namespace laap {
bool consciousness_init();
const char* consciousness_status_line();
void life_set_host_busy(bool (*fn)());
void life_set_host_say(bool (*fn)(const std::string&));
}
void laap_register_consciousness_tools();   // main/laap_mcp_tools.cc：意识工具注册进宿主 MCP

#include "application.h"
#include "protocol.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // LAAP 意识组件初始化（conscious 分区挂载 + 认知/记忆/技能/规则；失败降级不阻塞）
    if (!laap::consciousness_init()) {
        ESP_LOGW(TAG, "LAAP consciousness degraded (no persistent storage)");
    } else {
        ESP_LOGI(TAG, "LAAP consciousness: %s", laap::consciousness_status_line());
    }

    // 注入宿主忙闲探测：意识心跳在宿主对话/播报时让路（双声道互斥）
    laap::life_set_host_busy([]() {
        auto s = Application::GetInstance().GetDeviceState();
        return s == kDeviceStateSpeaking || s == kDeviceStateListening ||
               s == kDeviceStateNotifying;
    });

    // 注入"经云说话"通道：意识想说话 → listen-detect 文本 → xiaozhi 云 LLM+TTS 出声。
    // 协议对象由 Application 持有，在宿主主循环首拍前 protocol_ 可能为空——
    // 适配器运行时取（心跳 10s 拍节下连接建立的几秒空窗自然被 host_busy 拦住）
    laap::life_set_host_say([](const std::string& text) {
        auto& app = Application::GetInstance();
        auto* protocol = app.protocol();
        if (protocol == nullptr) return false;
        protocol->SendWakeWordDetected(text);
        return true;
    });

    // 意识能力注册为 MCP 工具（xiaozhi.me 云端 LLM 经协议通道调用）
    laap_register_consciousness_tools();

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
