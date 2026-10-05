#include "laap_boot.h"
#include "laap_config.h"
#include "laap_llm.h"
#include "laap_search.h"
#include "laap_fs.h"
#include "laap_cognition.h"
#include "laap_memory.h"
#include "laap_skills.h"
#include "laap_rules.h"
#include <esp_log.h>
#include <cstdio>

static const char* TAG = "laap_boot";

namespace laap {

bool consciousness_init() {
  ESP_LOGI(TAG, "LAAP 意识组件初始化…");
  cfg.load();            // NVS → 配置（llm/搜索/embed 键）
  laapSearch.begin();
  if (!memory.begin()) {
    ESP_LOGE(TAG, "conscious 分区挂载失败——记忆/技能/规则本次降级为不可持久");
    return false;
  }
  mind.begin();          // 加载进化状态，世代+1
  // C5 预期 EMA（NVS blob 恢复与 LAAP 固件同规格；此处先读默认，M5 接 nvs 键）
  ESP_LOGI(TAG, "认知核心就绪：世代 G%lu，记忆 %u 条，LLM[%s] 搜索[%s]",
           (unsigned long)mind.generation(), (unsigned)memory.count(),
           cfg.llmKey.empty() ? "无Key" : cfg.llmModel.c_str(),
           laapSearch.available() ? "就绪" : "离线");
  return true;
}

const char* consciousness_status_line() {
  static char buf[96];
  snprintf(buf, sizeof(buf), "G%lu mem=%u mood=%s goal=%s",
           (unsigned long)mind.generation(), (unsigned)memory.count(),
           mind.moodKey(), mind.goalCn());
  return buf;
}

}  // namespace laap
