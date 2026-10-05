#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// LAAP 意识组件（components/consciousness）
namespace laap { bool consciousness_init(); const char* consciousness_status_line(); }

#include "application.h"

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

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
