#include "laap_config.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>

static const char* TAG = "laap_cfg";
static const char* NS = "laap";

namespace laap {

Config cfg;

// 读键（不存在保持默认）
static void rd_str(nvs_handle_t h, const char* key, std::string& dst) {
  size_t len = 0;
  if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 0) {
    std::string v(len - 1, '\0');
    if (nvs_get_str(h, key, v.data(), &len) == ESP_OK) dst = v;
  }
}
static uint32_t rd_u32(nvs_handle_t h, const char* key, uint32_t def) {
  uint32_t v = def;
  nvs_get_u32(h, key, &v);
  return v;
}

bool Config::load() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  rd_str(h, "llmbase", llmBase);
  rd_str(h, "llmkey", llmKey);
  rd_str(h, "llmmodel", llmModel);
  rd_str(h, "embbase", embBase);
  rd_str(h, "embkey", embKey);
  rd_str(h, "embmodel", embModel);
  rd_str(h, "srchkeys", searchKeys);
  rd_str(h, "srchapi", searchApi);
  maxTokens = (int)rd_u32(h, "llmtok", (uint32_t)maxTokens);
  llmContinue = (int)rd_u32(h, "llmcont", (uint32_t)llmContinue);
  nvs_close(h);
  return true;
}

// 写键（空串也写——清空语义；失败登记）
static bool wr_str(nvs_handle_t h, const char* key, const std::string& v) {
  esp_err_t e = nvs_set_str(h, key, v.c_str());
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "配置键 %s 写入失败: %s", key, esp_err_to_name(e));
    return false;
  }
  return true;
}

bool Config::save() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "NVS 命名空间打开失败");
    return false;
  }
  bool ok = true;
  ok = wr_str(h, "llmbase", llmBase) && ok;
  ok = wr_str(h, "llmkey", llmKey) && ok;
  ok = wr_str(h, "llmmodel", llmModel) && ok;
  ok = wr_str(h, "embbase", embBase) && ok;
  ok = wr_str(h, "embkey", embKey) && ok;
  ok = wr_str(h, "embmodel", embModel) && ok;
  ok = wr_str(h, "srchkeys", searchKeys) && ok;
  ok = wr_str(h, "srchapi", searchApi) && ok;
  ok = (nvs_set_u32(h, "llmtok", (uint32_t)maxTokens) == ESP_OK) && ok;
  ok = (nvs_set_u32(h, "llmcont", (uint32_t)llmContinue) == ESP_OK) && ok;
  if (nvs_commit(h) != ESP_OK) ok = false;
  nvs_close(h);
  return ok;
}

}  // namespace laap
