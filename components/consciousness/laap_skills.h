#pragma once
// ============================================================
// LAAP-lite 口令技能库（移植自 laap-esp32 laap_skills，语义 1:1）
//   Voyager 式：用户亲手教的。对它说「以后每当我说 X 你就 Y」
//   → 后台 LLM 提取 触发词|指令 → 存 /mem/skills.txt（conscious 分区）。
//   生效：注入 system prompt，由 LLM 模糊匹配触发词。
//   hits 只做热度记录（满 12 条淘汰最冷）。截断全部走 utf8_cut。
// ============================================================
#include <string>

namespace laap {

class LaapSkills {
public:
  std::string promptLine();                 // 注入段（空表返回空串）
  std::string text();                       // 人读文本
  // 教学落库：触发词 2~10 字、指令 ≤30 字；同触发词=替换；满 12 条淘汰 hits 最低
  bool teach(const std::string& trigger, const std::string& instruction);
  void hit(const std::string& userText);    // userText 含某触发词 → hits+1（落盘）
  void clear();

private:
  struct Skill {
    std::string trig;
    std::string instr;
    uint16_t hits = 0;
  };
  Skill s_[12];
  int n_ = 0;
  bool loaded_ = false;
  void ensureLoaded();
  void save();
  int find(const std::string& trigger) const;
};

extern LaapSkills skills;

// UTF-8 字符边界截断（substring 半截汉字 = 请求体 400，LAAP 教训×2）——公共工具
std::string utf8_cut(const std::string& s, size_t maxBytes);

}  // namespace laap
