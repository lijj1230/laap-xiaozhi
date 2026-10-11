#include "laap_config.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>

static const char* TAG = "laap_cfg";
static const char* NS = "laap";

namespace laap {

Config cfg;

// 载入后夹取合法域（防 NVS 残留/手改越界）
static void clamp_cfg(Config& c) {
  if (c.expressCdMin > 240) c.expressCdMin = 240;
  if (c.idleSilenceMin > 1440) c.idleSilenceMin = 1440;
  if (c.idleEveryMin > 1440) c.idleEveryMin = 1440;
  if (c.quietStart < 0 || c.quietStart > 23) c.quietStart = 23;
  if (c.quietEnd < 0 || c.quietEnd > 23) c.quietEnd = 6;
  if (c.screenOffMin > 240) c.screenOffMin = 240;
}

bool Config::load() {
  // 上限为有意保留：命名空间“打不开”= 从未写过配置（健康检查语义沿用 M2）
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  uint32_t u = 0;
  int32_t i = 0;
  if (nvs_get_u32(h, "exprCd", &u) == ESP_OK) expressCdMin = u;
  if (nvs_get_u32(h, "idleSil", &u) == ESP_OK) idleSilenceMin = u;
  if (nvs_get_u32(h, "idleEvr", &u) == ESP_OK) idleEveryMin = u;
  if (nvs_get_i32(h, "quietSt", &i) == ESP_OK) quietStart = (int)i;
  if (nvs_get_i32(h, "quietEn", &i) == ESP_OK) quietEnd = (int)i;
  if (nvs_get_u32(h, "scrOff", &u) == ESP_OK) screenOffMin = u;
  nvs_close(h);
  clamp_cfg(*this);
  return true;
}

bool Config::save() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "NVS 命名空间打开失败");
    return false;
  }
  nvs_set_u32(h, "exprCd", expressCdMin);
  nvs_set_u32(h, "idleSil", idleSilenceMin);
  nvs_set_u32(h, "idleEvr", idleEveryMin);
  nvs_set_i32(h, "quietSt", (int32_t)quietStart);
  nvs_set_i32(h, "quietEn", (int32_t)quietEnd);
  nvs_set_u32(h, "scrOff", screenOffMin);
  bool ok = nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

}  // namespace laap