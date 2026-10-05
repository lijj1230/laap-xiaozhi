#pragma once
// ============================================================
// LAAP 设备侧联网搜索（移植语义：主源模板→DDG→Bing 三级回落，总预算 25s）
// 一期简化：默认链=必应 RSS → DDG（与 LAAP 空配置路径一致）；模板主源支持。
// 输出一 sense 段可注入 prompt 的知识文本（≤500B）。
// ============================================================
#include <string>

namespace laap {

class LaapSearch {
public:
  void begin();                       // 记录联网能力
  bool available() const { return ok_; }
  // 搜索并拼接知识文本（≤maxHit 条，总长 ≤maxLen）；拿不到锁/全源失败返回 ""
  std::string search(const std::string& query, int maxHit = 3, int maxLen = 500);
  std::string lastError;

private:
  bool ok_ = false;
  bool locked_ = false;
  bool net_lock();                    // 跨任务互斥（同 LAAP laapNetLock 语义）
  void net_unlock();
  std::string search_locked(const std::string& q, int maxHit, int maxLen);
  std::string search_rss(const std::string& q, int maxHit, int maxLen, int32_t budgetMs);
  std::string search_ddg(const std::string& q, int maxHit, int maxLen, int32_t budgetMs);
};

extern LaapSearch laapSearch;

}  // namespace laap
