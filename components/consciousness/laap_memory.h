#pragma once
// ============================================================
// LAAP-lite 记忆系统（移植自 laap-esp32 laap_memory，语义 1:1）
//   工作记忆环（40 条常驻）+ 情景记忆（episodes.jsonl 权重淘汰）
//   + 关系记忆（偏好/承诺/边界）；召回=关键词通道（语义向量已删，MCP-first）
// 存储：conscious 分区 /mem/（laap_fs 垫片）
// ============================================================
#include <string>
#include <cstdint>
#include <vector>

namespace laap {

class MemorySystem {
public:
  bool begin();                                          // 挂载+残尾修复+计数+净化+重建工作环
  void logEvent(const char* role, const std::string& text);   // 工作环+情景记忆
  std::string recentContext(int maxChars);
  std::string recentContextExcluding(int maxChars, const std::string& exclude);
  // 近→远工作记忆里的 user/aris 对话轮（roles 并行输出：0=主人 1=它自己），远→近返回
  int recentTurns(std::string* out, uint8_t* roles, int max) const;

  // 召回：关键词通道+关系事实，资源适配度调制深度（语义向量已删，MCP-first 简化）
  std::string recallSmart(const std::string& query, int maxChars, bool allowNet = true);

  // 关系记忆
  std::string relationsFor(const std::string& query, const std::string& goal, int maxLines);
  int relationsApply(const std::string& llmText);        // 「类型|内容」行解析落盘，返回新增数
  std::string relationsText() const;

  // 情绪精标注（夜间批量改写记忆的 m 字段；行号口径与 tidy 一致）
  int moodApply(const std::string& llmText);

  void rememberBoost(const std::string& fragment);       // 记忆强化（权重 +0.5，上限 5）
  std::string semantic() const;
  void setSemantic(const std::string& s);
  std::string episodicTail(int n);
  std::string episodicNumberedTail(int n);               // "12. {json}"（绝对行号）
  void applyTidyOps(const std::string& opsJson);         // [{"n":行号,"op":"del"}] ≤12 条
  float noveltyOf(const std::string& text);              // 新颖度 0..1（-1=无法评估；关键词基线）
  void clearAll();
  void onRestored();                                     // 快照恢复后对齐 RAM 计数
  void reloadWork();                                     // 从盘上末段重建工作环

  uint32_t count() const { return count_; }

private:
  static const int WORK_MAX = 40;
  std::string work_[WORK_MAX];
  int workHead_ = 0, workLen_ = 0;
  uint32_t count_ = 0;
  void appendEpisodic(const char* role, const std::string& rawText);
  void rewriteEpisodicByScore();
};

extern MemorySystem memory;

}  // namespace laap
