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
void life_set_screen(void (*fn)(bool on));
}
void laap_register_consciousness_tools();   // main/laap_mcp_tools.cc：意识工具注册进宿主 MCP
void laap_web_start();                      // main/laap_web.cc：局域网状态/配置面板（:8080）

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

    // 注入"经云说话"通道：意识想说话 → 走完整上游唤醒流程 WakeWordInvoke
// （OpenAudioChannel + SetListeningMode + EnableVoiceProcessing 一次到位）。
// 裸发 SendWakeWordDetected 只通控制面：音频面未使能 → 有字幕无声音（M6 实测）；
// WakeWordInvoke 内部自行 Schedule 到主循环，跨任务安全
    laap::life_set_host_say([](const std::string& text) {
        auto& app = Application::GetInstance();
        if (app.protocol() == nullptr) return false;
        app.WakeWordInvoke(text);
        return true;   // 已受理（成败细节心跳不可见，冷却照常计）
    });

    // 注入息屏开关：闲置自动熄屏/活动恢复（立创板上游无板级息屏，心跳代管）
    laap::life_set_screen([](bool on) {
        auto* backlight = Board::GetInstance().GetBacklight();
        if (backlight == nullptr) return;
        if (on) backlight->RestoreBrightness();
        else backlight->SetBrightness(0);
    });

    // 意识能力注册为 MCP 工具（xiaozhi.me 云端 LLM 经协议通道调用）
    laap_register_consciousness_tools();
    // 局域网 Web 面板（状态/配置/记忆；WiFi 拿到 IP 后自动开服 :8080）
    laap_web_start();

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
