#include "laap_config.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>

static const char* TAG = "laap_cfg";
static const char* NS = "laap";

namespace laap {

Config cfg;

bool Config::load() {
  // 当前无持久键（节奏常量编译期内置）；保留 NVS 打开探测作健康检查
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  nvs_close(h);
  return true;
}

bool Config::save() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "NVS 命名空间打开失败");
    return false;
  }
  bool ok = nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

}  // namespace laap
