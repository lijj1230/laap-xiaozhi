#include "laap_skills.h"
#include "laap_fs.h"
#include <esp_log.h>

static const char* TAG = "laap_skills";
static const char* SKILLS_PATH = "/mem/skills.txt";
static const int SKILLS_MAX = 12;

namespace laap {

LaapSkills skills;

// ---- 公共工具：UTF-8 字符边界截断（逐字节判断 UTF-8 首字节形态） ----
std::string utf8_cut(const std::string& s, size_t maxBytes) {
  if (s.length() <= maxBytes) return s;
  size_t cut = maxBytes;
  // 往回退到 UTF-8 首字节（0xxxxxxx / 11xxxxxx 是首字节，10xxxxxx 是续字节）
  while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
  return s.substr(0, cut);
}

void LaapSkills::ensureLoaded() {
  if (loaded_) return;
  loaded_ = true;
  std::string content;
  if (!fs_read(SKILLS_PATH, content)) return;
  size_t pos = 0;
  while (pos < content.size()) {
    size_t eol = content.find('\n', pos);
    if (eol == std::string::npos) eol = content.size();
    std::string ln = content.substr(pos, eol - pos);
    pos = eol + 1;
    // trim
    while (!ln.empty() && (ln.front() == ' ' || ln.front() == '\r')) ln.erase(ln.begin());
    while (!ln.empty() && (ln.back() == ' ' || ln.back() == '\r')) ln.pop_back();
    size_t b1 = ln.find('|');
    if (b1 == std::string::npos || b1 == 0) continue;
    size_t b2 = ln.find('|', b1 + 1);
    std::string trig = ln.substr(0, b1);
    std::string instr = (b2 != std::string::npos) ? ln.substr(b1 + 1, b2 - b1 - 1) : ln.substr(b1 + 1);
    uint16_t hits = 0;
    if (b2 != std::string::npos) hits = (uint16_t)atoi(ln.substr(b2 + 1).c_str());
    if (trig.empty() || instr.empty()) continue;
    if (n_ < SKILLS_MAX) {
      s_[n_] = {trig, instr, hits}; n_++;
    } else {
      // 满了：顶掉当前 hits 最低者，而不是静默丢行（v3.51 语义保留）
      int coldest = 0;
      for (int k = 1; k < n_; k++) if (s_[k].hits < s_[coldest].hits) coldest = k;
      if (hits > s_[coldest].hits) s_[coldest] = {trig, instr, hits};
    }
  }
}

void LaapSkills::save() {
  // 原子写（laap_fs_write 内部 tmp+rename）：掉电不丢技能库
  std::string content;
  char row[256];
  for (int i = 0; i < n_; i++) {
    snprintf(row, sizeof(row), "%s|%s|%u\n", s_[i].trig.c_str(), s_[i].instr.c_str(), (unsigned)s_[i].hits);
    content += row;
  }
  if (!fs_write(SKILLS_PATH, content)) ESP_LOGE(TAG, "技能落盘失败");
}

int LaapSkills::find(const std::string& trigger) const {
  for (int i = 0; i < n_; i++) if (s_[i].trig == trigger) return i;
  return -1;
}

bool LaapSkills::teach(const std::string& trigger, const std::string& instruction) {
  ensureLoaded();
  std::string trig = utf8_cut(trigger, 30);       // ≤10 字
  std::string instr = utf8_cut(instruction, 90);  // ≤30 字
  auto sanitize = [](std::string& v) {
    size_t p;
    while ((p = v.find('|')) != std::string::npos) v[p] = ' ';
    while ((p = v.find('\n')) != std::string::npos) v[p] = ' ';
  };
  sanitize(trig); sanitize(instr);
  if (trig.length() < 6 || instr.length() < 8) return false;   // 太短不成技能
  int i = find(trig);
  if (i >= 0) {
    s_[i].instr = instr;                        // 同触发词 = 重教 → 覆盖指令
  } else {
    if (n_ >= SKILLS_MAX) {                     // 满：淘汰 hits 最低的
      int coldest = 0;
      for (int k = 1; k < n_; k++) if (s_[k].hits < s_[coldest].hits) coldest = k;
      s_[coldest] = s_[n_ - 1]; n_--;
    }
    s_[n_++] = {trig, instr, 0};
  }
  save();
  return true;
}

void LaapSkills::hit(const std::string& userText) {
  ensureLoaded();
  bool changed = false;
  for (int i = 0; i < n_; i++)
    if (!s_[i].trig.empty() && userText.find(s_[i].trig) != std::string::npos) {
      if (s_[i].hits < 60000) s_[i].hits++;     // 饱和加法防 uint16 回绕（v3.51）
      changed = true;
    }
  if (changed) save();
}

void LaapSkills::clear() {
  fs_remove(SKILLS_PATH);
  n_ = 0;
  loaded_ = true;
}

std::string LaapSkills::promptLine() {
  ensureLoaded();
  if (!n_) return "";
  std::string out = "[口令技能] 主人亲手教过你这些，触发词出现时必须照做：\n";
  for (int i = 0; i < n_; i++) out += "说「" + s_[i].trig + "」→ " + s_[i].instr + "\n";
  return out;
}

std::string LaapSkills::text() {
  ensureLoaded();
  std::string out;
  for (int i = 0; i < n_; i++) {
    out += "说「" + s_[i].trig + "」→ " + s_[i].instr + "（命中" + std::to_string(s_[i].hits) + "次）\n";
  }
  return out;
}

}  // namespace laap
