#pragma once
// ============================================================
// LAAP-lite 行为规则集（移植自 laap-esp32 laap_rules，语义 1:1）
//   /mem/rules.txt 一行一条规则（LLM 用"反馈+失败记录"整理产出）。
//   注入：system prompt 追加规则段（懒加载缓存，落盘即刷新）。
//   约束：最多 6 条、单条 ≤90 字节；无产出/无变化不写盘（省磨损）。
// ============================================================
#include <string>

namespace laap {

class LaapRules {
public:
  std::string promptLine();               // 注入用整段（无规则返回空串）
  std::string text();                     // 原始规则文本
  int count();
  bool apply(const std::string& llmOutput);  // 解析落盘，返回 true=确实更新
  void clear();

private:
  std::string cache_;
  bool loaded_ = false;
  void ensureLoaded();
};

extern LaapRules rules;

}  // namespace laap
