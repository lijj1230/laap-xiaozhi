#include "laap_boot.h"
#include "laap_config.h"
#include "laap_fs.h"
#include "laap_cognition.h"
#include "laap_memory.h"
#include "laap_skills.h"
#include "laap_rules.h"
#include "laap_life.h"
#include <esp_log.h>
#include <cstdio>

static const char* TAG = "laap_boot";

namespace laap {

bool consciousness_init() {
  ESP_LOGI(TAG, "LAAP 意识组件初始化（纯本地+云注入）…");
  cfg.load();            // NVS → 配置（意识节奏等本地键）
  if (!memory.begin()) {
    ESP_LOGE(TAG, "conscious 分区挂载失败——记忆/技能/规则本次降级为不可持久");
    return false;
  }
  mind.begin();          // 加载进化状态，世代+1
  ESP_LOGI(TAG, "认知核心就绪：世代 G%lu，记忆 %u 条，规则 %d 条",
           (unsigned long)mind.generation(), (unsigned)memory.count(), rules.count());
  life_start();          // 黑匣子 + SNTP + 意识心跳任务（挂载失败不启动，避免半态写盘）
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
