#include "laap_rules.h"
#include "laap_fs.h"
#include "laap_skills.h"   // utf8_cut
#include <esp_log.h>

static const char* TAG = "laap_rules";
static const char* RULES_PATH = "/mem/rules.txt";
static const int RULES_MAX = 6;
static const size_t RULE_BYTES = 90;   // ≈30 个汉字（UTF-8 3B/字）

namespace laap {

void LaapRules::ensureLoaded() {
  if (loaded_) return;
  loaded_ = true;
  fs_read(RULES_PATH, cache_);
  while (!cache_.empty() && (cache_.back() == '\n' || cache_.back() == '\r' || cache_.back() == ' ')) cache_.pop_back();
}

std::string LaapRules::text() {
  ensureLoaded();
  return cache_;
}

int LaapRules::count() {
  ensureLoaded();
  if (cache_.empty()) return 0;
  int n = 1;
  for (char c : cache_) if (c == '\n') n++;
  return n;
}

std::string LaapRules::promptLine() {
  ensureLoaded();
  if (cache_.empty()) return "";
  std::string s = cache_;
  std::string out;
  size_t start = 0;
  while (start < s.size()) {                       // 每行带列表符
    size_t nl = s.find('\n', start);
    if (nl == std::string::npos) nl = s.size();
    out += "- " + s.substr(start, nl - start) + "\n";
    start = nl + 1;
  }
  return "[行为规则（自我进化沉淀，必须遵守）]\n" + out;
}

bool LaapRules::apply(const std::string& llmOutput) {
  // 行式解析：剥常见前缀（- • * 1. ① 等），只留像规则的行（v3.51 审计语义）
  std::string picked[RULES_MAX];
  int n = 0;
  size_t start = 0;
  while (start < llmOutput.size() && n < RULES_MAX) {
    size_t nl = llmOutput.find('\n', start);
    std::string ln = (nl == std::string::npos) ? llmOutput.substr(start) : llmOutput.substr(start, nl - start);
    start = (nl == std::string::npos) ? llmOutput.size() : nl + 1;
    while (!ln.empty() && (ln.front() == ' ' || ln.front() == '\r' || ln.front() == '\t')) ln.erase(ln.begin());
    while (!ln.empty() && (ln.back() == ' ' || ln.back() == '\r' || ln.back() == '\t')) ln.pop_back();
    if (ln.empty()) continue;
    if (ln[0] == '-' || ln[0] == '*') { ln.erase(ln.begin()); }
    else if (ln.rfind("•", 0) == 0) { ln = ln.substr(strlen("•")); }
    else if (ln[0] >= '0' && ln[0] <= '9') {
      // 数字必须紧跟 ". / 、 )" 才算序号（"12点睡觉"不被啃成"点睡觉"）
      size_t d = 0;
      while (d < ln.size() && ln[d] >= '0' && ln[d] <= '9') d++;
      if (d > 0 && d < ln.size() && d <= 3 &&
          (ln[d] == '.' || ln[d] == (char)0xE3 /*、*/ || ln[d] == ')')) {
        ln = ln.substr(d + 1);
      }
    }
    else if ((unsigned char)ln[0] == 0xE2 && ln.size() > 3) { ln = ln.substr(3); }   // ① 等 U+2000-2FFF 3B 起
    while (!ln.empty() && (ln.front() == ' ' || ln.front() == '\r' || ln.front() == '\t')) ln.erase(ln.begin());
    while (!ln.empty() && (ln.back() == ' ' || ln.back() == '\r' || ln.back() == '\t')) ln.pop_back();
    if (ln.length() < 8 || ln.length() > 160) continue;
    if (ln.rfind("规则", 0) == 0 || ln.rfind("输出", 0) == 0 || ln.rfind("以下", 0) == 0 || ln.rfind("好的", 0) == 0) continue;
    picked[n++] = utf8_cut(ln, RULE_BYTES);
  }
  if (!n) return false;
  std::string merged;
  for (int i = 0; i < n; i++) { if (i) merged += '\n'; merged += picked[i]; }
  ensureLoaded();
  if (merged == cache_) return false;             // 无变化不写盘（省磨损）
  if (!fs_write(RULES_PATH, merged)) { ESP_LOGE(TAG, "规则落盘失败"); return false; }
  cache_ = merged;
  return true;
}

void LaapRules::clear() {
  fs_remove(RULES_PATH);
  cache_.clear();
}

}  // namespace laap
