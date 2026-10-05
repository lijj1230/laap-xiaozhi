#include "laap_memory.h"
#include "laap_fs.h"
#include "laap_cognition.h"
#include "laap_skills.h"     // utf8_cut
#include "laap_util.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cmath>
#include <ctime>
#include <cstring>
#include <algorithm>

static const char* TAG = "laap_mem";

namespace laap {

MemorySystem memory;

static const char* EP_PATH = "/mem/episodes.jsonl";
static const int  EP_MAX   = 300;
static const char* REL_PATH = "/mem/relations.jsonl";
static const int  REL_MAX  = 40;
static const char* EMB_PATH = "/mem/emb.bin";
static const int   EMB_DIM  = 1024;

// M3 接 laap_llm 后由其实现（weak 存根：返回 false=走关键词通道）
__attribute__((weak)) bool laap_embed(const std::string& text, std::string& binOut) { return false; }

static bool call_embedding(const std::string& text, float* out) {
  std::string bin;
  if (!laap_embed(text, bin) || bin.size() < (size_t)(EMB_DIM * 4)) return false;
  memcpy(out, bin.data(), EMB_DIM * 4);
  return true;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 流式逐行读工具：File API 无 Arduino readStringUntil，自实现
static bool read_line(FILE* f, std::string& out) {
  out.clear();
  int c;
  while ((c = fgetc(f)) != EOF && c != '\n') out += (char)c;
  return c != EOF || !out.empty();
}

// ---- 行内工具：抽 w 与时间戳 → (w, 新鲜度 0..1) ----
static void parse_wt(const std::string& l, float& w, float& fresh) {
  w = 1.0f; fresh = 0.5f;
  size_t wp = l.find("\"w\":");
  if (wp != std::string::npos) w = strtof(l.c_str() + wp + 4, nullptr);
  time_t now = time(nullptr);
  if (now <= 1700000000) return;
  size_t tp = l.find("\"t\":");
  if (tp == std::string::npos) return;
  uint32_t t = (uint32_t)strtoul(l.c_str() + tp + 4, nullptr, 10);
  if (t == 0) return;
  float ageDay = ((uint32_t)now - t) / 86400.0f;
  if (ageDay < 0) ageDay = 0;
  fresh = 1.0f / (1.0f + ageDay);
}

static std::string parse_mood(const std::string& l) {
  size_t mp = l.find("\"m\":\"");
  if (mp == std::string::npos) return "";
  size_t vs = mp + 5, ve = l.find('"', vs);
  if (ve == std::string::npos) return "";
  return l.substr(vs, ve - vs);
}

static std::string extract_x(const std::string& line) {
  size_t xp = line.find("\"x\":\"");
  if (xp == std::string::npos) return "";
  std::string x = line.substr(xp + 5);
  size_t xe = x.rfind('"');
  if (xe == std::string::npos || xe == 0) return "";
  x = x.substr(0, xe);
  // 常见转义还原
  auto repl = [&](const std::string& a, const std::string& b) {
    size_t p;
    while ((p = x.find(a)) != std::string::npos) x.replace(p, a.size(), b);
  };
  repl("\\n", " "); repl("\\t", " "); repl("\\\"", "\"");
  return sanitize_utf8(x);
}

static float cosine_of(const float* a, const float* b, int n) {
  float dot = 0, na = 0, nb = 0;
  for (int i = 0; i < n; i++) { dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
  if (na <= 0 || nb <= 0) return 0;
  return dot / (sqrtf(na) * sqrtf(nb));
}

// ================= begin：残尾修复/计数/净化/对齐/重建工作环 =================
bool MemorySystem::begin() {
  if (!fs_begin()) return false;
  fs_append("/mem/.keep", "");   // 确保 /mem 目录存在（追加会创建路径）
  // 掉电残尾修复：无换行尾行截断（防与新记录拼成损坏行）
  {
    size_t sz = fs_size(EP_PATH);
    if (sz > 0) {
      std::string all;
      if (fs_read(EP_PATH, all)) {
        if (all.back() != '\n') {
          size_t cut = all.rfind('\n');
          std::string keep = (cut == std::string::npos) ? "" : all.substr(0, cut + 1);
          fs_write(EP_PATH, keep);
          ESP_LOGW(TAG, "修复掉电残尾：截断未写完的最后一行");
        }
      }
    }
  }
  // 计数 + 净化（毒字节→?）
  count_ = 0;
  {
    std::string all;
    if (fs_read(EP_PATH, all) && !all.empty()) {
      std::string clean = sanitize_utf8(all);
      if (clean.size() != all.size()) {
        fs_write(EP_PATH, clean);
        ESP_LOGW(TAG, "episodes 净化：%uB → %uB", (unsigned)all.size(), (unsigned)clean.size());
        all = clean;
      }
      for (char c : all) if (c == '\n') count_++;
    }
  }
  // 向量缓存对齐条数
  embCount_ = 0; embFail_ = 0;
  size_t esz = fs_size(EMB_PATH);
  if (esz > 0) {
    if (esz % (EMB_DIM * 4) != 0) { fs_remove(EMB_PATH); embCount_ = 0; }
    else embCount_ = esz / (EMB_DIM * 4);
  }
  if (embCount_ != count_) { fs_remove(EMB_PATH); embCount_ = 0; }
  reloadWork();
  return true;
}

// ================= append（残尾检查+清洗+写失败不计数，v3.76e 语义） =================
void MemorySystem::appendEpisodic(const char* role, const std::string& rawText) {
  // 残尾检查：上次断电的无换行半行会让本条拼接成损坏行
  // （fs_last_char 只读末 1 字节——原全文件读每条消息 70KB+，心跳任务高频路径）
  {
    if (fs_size(EP_PATH) > 0 && fs_last_char(EP_PATH) != '\n')
      fs_append(EP_PATH, "\n");
  }
  std::string text = sanitize_utf8(rawText);
  time_t now = time(nullptr);
  uint32_t t = (now > 1700000000) ? (uint32_t)now : 0;
  std::string rec = std::string("{\"t\":") + std::to_string(t) + ",\"r\":\"" + role +
                    "\",\"w\":1.0,\"m\":\"" + mind.moodKey() + "\",\"x\":\"" + json_escape(text) + "\"}\n";
  if (!fs_append(EP_PATH, rec)) {
    ESP_LOGE(TAG, "episode 写入失败（盘满？）");
    return;
  }
  count_++;
  if (count_ > (uint32_t)EP_MAX + 50) rewriteEpisodicByScore();
}

// ================= 权重淘汰（w×0.7 + 新鲜×0.3，丢最低分） =================
void MemorySystem::rewriteEpisodicByScore() {
  std::string all;
  if (!fs_read(EP_PATH, all)) return;
  struct Rec { std::string line; float score; };
  std::vector<Rec> lines;
  time_t nowT = time(nullptr);
  uint32_t now = (nowT > 1700000000) ? (uint32_t)nowT : 0;
  (void)now;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    std::string l = all.substr(pos, eol - pos);
    pos = eol + 1;
    if (!l.size()) continue;
    float w, fr;
    parse_wt(l, w, fr);
    lines.push_back({l, w * 0.7f + fr * 0.3f});
  }
  if (lines.size() <= (size_t)EP_MAX) { count_ = lines.size(); return; }   // 无条件校正（防虚高全文件重扫）
  size_t drop = lines.size() - EP_MAX;
  for (size_t d = 0; d < drop; d++) {
    size_t worst = 0;
    for (size_t i = 1; i < lines.size(); i++)
      if (lines[i].score < lines[worst].score) worst = i;
    lines[worst].score = 999;
    lines[worst].line.clear();
  }
  std::string out;
  for (auto& r : lines) if (!r.line.empty()) { out += r.line; out += '\n'; }
  fs_write(EP_PATH, out);
  count_ = EP_MAX;
  fs_remove(EMB_PATH);   // 行序变了 → 向量缓存作废重建
  embCount_ = 0;
}

// ================= logEvent / recentContext / recentTurns =================
void MemorySystem::logEvent(const char* role, const std::string& text) {
  std::string tmp = std::string(role) + ":" + sanitize_utf8(text);
  if (tmp.length() > 160) tmp = utf8_cut(tmp, 160);
  work_[workHead_] = tmp;
  workHead_ = (workHead_ + 1) % WORK_MAX;
  if (workLen_ < WORK_MAX) workLen_++;
  appendEpisodic(role, text);
}

std::string MemorySystem::recentContext(int maxChars) {
  std::string out;
  out.reserve(256);
  for (int i = 0; i < workLen_ && (int)out.length() < maxChars; i++) {
    int idx = (workHead_ - 1 - i + WORK_MAX * 2) % WORK_MAX;
    out = work_[idx] + "\n" + out;
  }
  return out;
}

std::string MemorySystem::recentContextExcluding(int maxChars, const std::string& exclude) {
  std::string out;
  for (int i = 0; i < workLen_ && (int)out.length() < maxChars; i++) {
    int idx = (workHead_ - 1 - i + WORK_MAX * 2) % WORK_MAX;
    if (!exclude.empty() && work_[idx].find(exclude) != std::string::npos) continue;
    out = work_[idx] + "\n" + out;
  }
  return out;
}

int MemorySystem::recentTurns(std::string* out, uint8_t* roles, int max) const {
  int n = 0;
  for (int i = workLen_ - 1; i >= 0 && n < max; i--) {
    int idx = (workHead_ - 1 - i + WORK_MAX * 2) % WORK_MAX;
    const std::string& e = work_[idx];
    bool isUser = e.rfind("user:", 0) == 0;
    bool isAris = e.rfind("aris:", 0) == 0;
    if (!isUser && !isAris) continue;
    out[n] = e.substr(e.find(':') + 1);
    if (out[n].length() > 160) out[n] = utf8_cut(out[n], 160);
    if (roles) roles[n] = isAris ? 1 : 0;
    n++;
  }
  for (int a = 0, b = n - 1; a < b; a++, b--) {   // 翻转为远→近（对话时序）
    std::swap(out[a], out[b]);
    if (roles) std::swap(roles[a], roles[b]);
  }
  return n;
}

// ================= embedTick（M3 后接通；堆护栏/熔断/限速语义保留） =================
void MemorySystem::embedTick() {
  static bool s_poison = false;
  if (s_poison) return;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 30000) return;
  if (embFail_ >= 3) {
    static uint32_t s_probeMs = 0;
    if (laap_millis() - s_probeMs < 300000) return;
    s_probeMs = laap_millis();
  }
  if (laap_millis() - embLastMs_ < 30000) return;
  std::string all;
  if (!fs_read(EP_PATH, all)) return;
  int total = 0; bool lineHas = false;
  for (char ch : all) {
    if (ch == '\n') { if (lineHas) total++; lineHas = false; }
    else if (ch != '\r' && ch != ' ' && ch != '\t') lineHas = true;
  }
  if (lineHas) total++;
  if (total <= (int)embCount_) return;

  // 取第 embCount_+1 行
  size_t pos = 0; int lineno = 0; std::string line;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    line = all.substr(pos, eol - pos);
    pos = eol + 1;
    if (!line.empty() && ++lineno == (int)embCount_ + 1) break;
    line.clear();
  }
  if (line.empty()) { fs_remove(EMB_PATH); embCount_ = 0; return; }
  if (line.find("\"x\":\"") == std::string::npos) {
    ESP_LOGE(TAG, "episodes 第 %u 行损坏，embedding 熔断", (unsigned)(embCount_ + 1));
    s_poison = true;
    return;
  }
  std::string text = extract_x(line);
  float* vec = (float*)malloc(EMB_DIM * 4);
  if (!vec) return;
  embLastMs_ = laap_millis();
  if (!call_embedding(text, vec)) { free(vec); embFail_++; return; }
  embFail_ = 0;
  std::string bin((const char*)vec, EMB_DIM * 4);
  if (fs_append(EMB_PATH, bin)) embCount_++;
  free(vec);
}

// ================= recallSmart（资源适配度+意图/情绪加权+关键词退通道） =================
std::string MemorySystem::recallSmart(const std::string& query, int maxChars, bool allowNet) {
  struct Hit { std::string line; float score; };
  std::vector<Hit> hits;

  bool allowEmb = (embFail_ < 3) && allowNet;
  int effChars = maxChars;
  {
    uint32_t maxblk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (maxblk < 40000) allowEmb = false;
    float depth = 1.0f;
    if (maxblk < 45000) depth -= 0.3f;
    if (mind.bodyStrain >= 0.30f) depth -= 0.3f;
    if (depth < 0.4f) depth = 0.4f;
    effChars = (int)(maxChars * depth);
    if (effChars < 120) effChars = 120;
  }
  std::string it0 = mind.intent(0);
  std::string curMood = mind.moodKey();
  std::string rel = relationsFor(query, it0, 2);

  std::string all;
  bool haveFile = fs_read(EP_PATH, all);

  // —— 主通道：语义（M3 后接通；call_embedding weak 存根默认失败=走退通道） ——
  if (allowEmb && haveFile) {
    float* qv = (float*)malloc(EMB_DIM * 4);
    if (qv) {
      embLastMs_ = laap_millis();
      if (call_embedding(query, qv)) {
        embFail_ = 0;
        FILE* ef = fopen("/conscious/mem/emb.bin", "rb");
        if (ef) {
          float* rv = (float*)malloc(EMB_DIM * 4);
          int idx = 0;
          size_t lpos = 0;
          while (rv && fread(rv, 1, EMB_DIM * 4, ef) == (size_t)(EMB_DIM * 4)) {
            if (lpos >= all.size()) break;
            size_t eol = all.find('\n', lpos);
            if (eol == std::string::npos) eol = all.size();
            std::string l = all.substr(lpos, eol - lpos);
            lpos = eol + 1;
            if (!l.size()) continue;
            float fw, fr;
            parse_wt(l, fw, fr);
            float wn = (fw > 3 ? 3 : fw) / 3.0f;
            float sim = cosine_of(qv, rv, EMB_DIM);
            float ib = bigram_mostly_in(it0, l) ? 0.10f : 0.0f;
            float mb = (curMood != "calm" && parse_mood(l) == curMood) ? 0.06f : 0.0f;
            hits.push_back({l, sim * 0.75f + wn * 0.15f + fr * 0.10f + ib + mb});
            idx++;
            if (hits.size() > 40) {
              size_t worst = 0;
              for (size_t i = 1; i < hits.size(); i++) if (hits[i].score < hits[worst].score) worst = i;
              hits[worst] = hits.back(); hits.pop_back();
            }
          }
          if (rv) free(rv);
          fclose(ef);
        }
      } else {
        embFail_++;
      }
      free(qv);
    }
  }

  // —— 退通道：关键词 ——
  if (hits.empty() && haveFile && query.size() >= 2) {
    size_t pos = 0;
    while (pos < all.size()) {
      size_t eol = all.find('\n', pos);
      if (eol == std::string::npos) eol = all.size();
      std::string l = all.substr(pos, eol - pos);
      pos = eol + 1;
      if (!l.size() || l.find(query) == std::string::npos) continue;
      float fw, fr;
      parse_wt(l, fw, fr);
      float wn = (fw > 3 ? 3 : fw) / 3.0f;
      float ib = bigram_mostly_in(it0, l) ? 0.10f : 0.0f;
      float mb = (curMood != "calm" && parse_mood(l) == curMood) ? 0.06f : 0.0f;
      hits.push_back({l, wn * 0.5f + fr * 0.5f + ib + mb});
    }
  }
  if (hits.empty()) return rel;

  std::string out;
  for (int round = 0; round < 3 && !hits.empty(); round++) {
    size_t best = 0;
    for (size_t i = 1; i < hits.size(); i++) if (hits[i].score > hits[best].score) best = i;
    std::string x = extract_x(hits[best].line);
    if (!x.empty() && out.find(x) == std::string::npos &&
        !(x == query || (query.size() >= 8 && x.find(query) != std::string::npos)))
      out = out.length() ? out + "\n" + x : x;
    hits[best] = hits.back(); hits.pop_back();
    if ((int)out.length() >= effChars) break;
  }
  if (!rel.empty()) out = out.length() ? out + "\n" + rel : rel;
  return out;
}

// ================= 关系记忆 =================
static bool rel_parse(const std::string& l, std::string& k, std::string& x) {
  size_t xp = l.find("\"x\":\"");
  if (xp == std::string::npos) return false;
  x = l.substr(xp + 5);
  size_t xe = x.rfind('"');
  if (xe == std::string::npos || xe == 0) return false;
  x = x.substr(0, xe);
  size_t kp = l.find("\"k\":\"");
  if (kp != std::string::npos) {
    size_t ke = l.find('"', kp + 5);
    if (ke != std::string::npos && ke > kp + 5) k = l.substr(kp + 5, ke - kp - 5);
  }
  return true;
}

std::string MemorySystem::relationsFor(const std::string& query, const std::string& goal, int maxLines) {
  if (maxLines < 1) maxLines = 1;
  if (maxLines > 4) maxLines = 4;
  std::string all;
  if (!fs_read(REL_PATH, all)) return "";
  std::string picked[4];
  int n = 0;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    std::string l = all.substr(pos, eol - pos);
    pos = eol + 1;
    std::string k, x;
    if (!rel_parse(l, k, x)) continue;
    if (k.empty()) k = "记着";
    bool hit = (query.size() >= 6 && bigram_mostly_in(query, x)) ||
               (goal.size() >= 6 && bigram_mostly_in(goal, x));
    if (!hit) continue;
    picked[n % maxLines] = "【" + k + "】" + x;
    n++;
  }
  std::string out;
  for (int i = (n > maxLines ? n - maxLines : 0); i < n; i++) {
    if (!out.empty()) out += "\n";
    out += picked[i % maxLines];
  }
  return out;
}

int MemorySystem::relationsApply(const std::string& llmText) {
  std::string existing;
  int lines = 0;
  fs_read(REL_PATH, existing);
  for (char c : existing) if (c == '\n') lines++;
  // 残尾修复
  if (!existing.empty() && existing.back() != '\n') fs_append(REL_PATH, "\n");
  int added = 0;
  size_t start = 0;
  while (start < llmText.size()) {
    size_t e = llmText.find('\n', start);
    std::string ln = (e == std::string::npos) ? llmText.substr(start) : llmText.substr(start, e - start);
    start = (e == std::string::npos) ? llmText.size() : e + 1;
    while (!ln.empty() && (ln.front() == ' ' || ln.front() == '\r')) ln.erase(ln.begin());
    while (!ln.empty() && (ln.back() == ' ' || ln.back() == '\r')) ln.pop_back();
    if (ln.size() > 1 && ln[0] == '-') { ln = ln.substr(1); }
    size_t bar = ln.find('|');
    if (bar == std::string::npos || bar == 0) continue;
    std::string k = sanitize_utf8(ln.substr(0, bar));
    k = utf8_cut(k, 12);
    std::string x = sanitize_utf8(ln.substr(bar + 1));
    x = utf8_cut(x, 80);
    if (k.find("偏好") == std::string::npos && k.find("承诺") == std::string::npos && k.find("边界") == std::string::npos) continue;
    std::string xesc = json_escape(x);
    if (x.size() < 6 || existing.find(xesc) != std::string::npos) continue;
    time_t now = time(nullptr);
    std::string rec = std::string("{\"t\":") + std::to_string(now > 1700000000 ? (uint32_t)now : 0) +
                      ",\"k\":\"" + json_escape(k) + "\",\"x\":\"" + xesc + "\"}\n";
    fs_append(REL_PATH, rec);
    existing += xesc + "\n";
    lines++; added++;
  }
  if (lines > REL_MAX) {   // 封顶：保最近 REL_MAX 条
    std::string all;
    if (!fs_read(REL_PATH, all)) return added;
    std::string keep;
    int n2 = 0;
    size_t pos = 0;
    while (pos < all.size()) {
      size_t eol = all.find('\n', pos);
      if (eol == std::string::npos) eol = all.size();
      std::string l = all.substr(pos, eol - pos);
      pos = eol + 1;
      std::string k2, x2;
      if (l.empty() || !rel_parse(l, k2, x2)) continue;
      keep += l + "\n";
      if (++n2 > REL_MAX) {
        size_t first = keep.find('\n');
        keep = keep.substr(first + 1);
        n2--;
      }
    }
    fs_write(REL_PATH, keep);
  }
  return added;
}

std::string MemorySystem::relationsText() const {
  std::string all;
  if (!fs_read(REL_PATH, all)) return "";
  std::string out;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    std::string l = all.substr(pos, eol - pos);
    pos = eol + 1;
    std::string k, x;
    if (!rel_parse(l, k, x)) continue;
    if (k.empty()) k = "记着";
    if (!out.empty()) out += "\n";
    out += "【" + k + "】" + x;
  }
  return out;
}

// ================= 情绪精标注（流式重写，行数不变） =================
static bool set_line_mood(std::string& l, const std::string& mood) {
  size_t mp = l.find("\"m\":\"");
  if (mp != std::string::npos) {
    size_t vs = mp + 5, ve = l.find('"', vs);
    if (ve == std::string::npos) return false;
    l = l.substr(0, vs) + mood + l.substr(ve);
    return true;
  }
  size_t wp = l.find("\"w\":");
  if (wp == std::string::npos) return false;
  size_t comma = l.find(',', wp);
  if (comma == std::string::npos) return false;
  l = l.substr(0, comma + 1) + "\"m\":\"" + mood + "\"," + l.substr(comma + 1);
  return true;
}

int MemorySystem::moodApply(const std::string& llmText) {
  static const char* kVocab[] = { "calm", "happy", "curious", "excited", "lonely", "anxious", "tired" };
  int ln[80];
  std::string mo[80];
  int nMap = 0;
  size_t start = 0;
  while (start < llmText.size() && nMap < 80) {
    size_t e = llmText.find('\n', start);
    std::string line = (e == std::string::npos) ? llmText.substr(start) : llmText.substr(start, e - start);
    start = (e == std::string::npos) ? llmText.size() : e + 1;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\r')) line.erase(line.begin());
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
    if (line.size() > 1 && line[0] == '-') line = line.substr(1);
    size_t bar = line.find('|');
    if (bar == std::string::npos || bar == 0) continue;
    long no = strtol(line.substr(0, bar).c_str(), nullptr, 10);
    std::string mood = line.substr(bar + 1);
    bool okMood = false;
    for (auto k : kVocab) if (mood == k) { okMood = true; break; }
    if (no < 1 || !okMood) continue;
    ln[nMap] = (int)no; mo[nMap] = mood; nMap++;
  }
  if (!nMap) return 0;
  std::string all;
  if (!fs_read(EP_PATH, all)) return 0;
  std::string out;
  int lineno = 0, applied = 0;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    std::string l = all.substr(pos, eol - pos);
    pos = eol + 1;
    while (!l.empty() && (l.front() == ' ' || l.front() == '\r')) l.erase(l.begin());
    while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
    if (l.empty()) continue;
    lineno++;
    for (int i = 0; i < nMap; i++)
      if (ln[i] == lineno && set_line_mood(l, mo[i])) applied++;
    out += l + "\n";
  }
  if (!applied) return 0;
  fs_write(EP_PATH, out);
  return applied;
}

// ================= rememberBoost（两遍流式语义，w+0.5 上限 5） =================
void MemorySystem::rememberBoost(const std::string& fragment) {
  if (fragment.length() < 2) return;
  std::string all;
  if (!fs_read(EP_PATH, all)) return;
  bool changed = false;
  if (all.find(fragment) != std::string::npos && all.find("\"w\":") != std::string::npos) changed = true;
  if (!changed) return;
  std::string out;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    std::string l = all.substr(pos, eol - pos);
    pos = eol + 1;
    while (!l.empty() && (l.front() == ' ' || l.front() == '\r')) l.erase(l.begin());
    while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
    if (l.empty()) continue;
    if (l.find(fragment) != std::string::npos) {
      size_t wp = l.find("\"w\":");
      if (wp != std::string::npos) {
        float w = strtof(l.c_str() + wp + 4, nullptr) + 0.5f;
        if (w > 5) w = 5;
        size_t comma = l.find(',', wp);
        if (comma != std::string::npos)
          l = l.substr(0, wp + 4) + std::to_string(w).substr(0, 4) + l.substr(comma);
      }
    }
    out += l + "\n";
  }
  fs_write(EP_PATH, out);
}

// ================= semantic / episodicTail =================
std::string MemorySystem::semantic() const {
  std::string s;
  fs_read("/mem/semantic.txt", s);
  s = sanitize_utf8(s);
  if (s.length() > 400) s = utf8_cut(s, 400);
  return s;
}

void MemorySystem::setSemantic(const std::string& s) {
  fs_write("/mem/semantic.txt", s);
}

std::string MemorySystem::episodicTail(int n) {
  std::string all;
  if (!fs_read(EP_PATH, all)) return "[]";
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    std::string l = all.substr(pos, eol - pos);
    pos = eol + 1;
    if (!l.empty()) lines.push_back(sanitize_utf8(l));
  }
  std::string out = "[";
  int start = (int)lines.size() - n; if (start < 0) start = 0;
  for (int i = start; i < (int)lines.size(); i++) {
    out += lines[i];
    if (i != (int)lines.size() - 1) out += ",";
  }
  return out + "]";
}

std::string MemorySystem::episodicNumberedTail(int n) {
  std::string all;
  if (!fs_read(EP_PATH, all)) return "";
  int total = 0;
  for (char c : all) if (c == '\n') total++;
  int startNo = total - n + 1;
  if (startNo < 1) startNo = 1;
  std::string out;
  int lineno = 0;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    std::string l = all.substr(pos, eol - pos);
    pos = eol + 1;
    while (!l.empty() && (l.front() == ' ' || l.front() == '\r')) l.erase(l.begin());
    while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
    if (l.empty()) continue;
    lineno++;
    if (lineno >= startNo) out += std::to_string(lineno) + ". " + l + "\n";
  }
  return out;
}

// ================= applyTidyOps（非空行口径；emb 同步压缩或作废） =================
void MemorySystem::applyTidyOps(const std::string& opsJson) {
  std::string all;
  if (!fs_read(EP_PATH, all)) return;
  int total = 0;
  {
    size_t pos = 0;
    while (pos < all.size()) {
      size_t eol = all.find('\n', pos);
      if (eol == std::string::npos) eol = all.size();
      std::string l = all.substr(pos, eol - pos);
      pos = eol + 1;
      while (!l.empty() && (l.front() == ' ' || l.front() == '\r')) l.erase(l.begin());
      while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
      if (!l.empty()) total++;
    }
  }
  if (!total) return;
  std::vector<char> del(total, 0);
  int cnt = 0;
  size_t pos = 0;
  while (cnt < 12) {
    size_t np = opsJson.find("\"n\":", pos);
    if (np == std::string::npos) break;
    size_t ob = opsJson.find('}', np);
    std::string chunk = opsJson.substr(np, (ob == std::string::npos ? opsJson.size() : ob) - np);
    pos = (ob == std::string::npos) ? opsJson.size() : ob + 1;
    std::string numStr;
    for (size_t k = 4; k < chunk.size(); k++) {
      char c = chunk[k];
      if (c == ',' || c == '}') break;
      if (c >= '0' && c <= '9') numStr += c;
    }
    if (numStr.empty()) continue;
    long n = strtol(numStr.c_str(), nullptr, 10);
    if (n < 1 || n > total || chunk.find("\"del\"") == std::string::npos) continue;
    if (!del[n - 1]) { del[n - 1] = 1; cnt++; }
  }
  if (!cnt) { ESP_LOGI(TAG, "整理：模型无可整理项"); return; }

  std::string out;
  int lineno = 0, kept = 0;
  pos = 0;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    std::string l = all.substr(pos, eol - pos);
    pos = eol + 1;
    if (l.empty()) continue;
    lineno++;
    if (lineno <= total && del[lineno - 1]) continue;
    out += l + "\n";
    kept++;
  }
  fs_write(EP_PATH, out);
  count_ = kept;

  // 向量缓存同步压缩；对不上整份作废
  // （emb.bin 满容 1.2MB：全量读/写进内部堆必失败——FILE* 流式过滤重写）
  bool aligned = false;
  if ((int)embCount_ == total) {
    FILE* src = fopen("/conscious/mem/emb.bin", "rb");
    FILE* dst = fopen("/conscious/mem/emb.bin.tmp", "wb");
    if (src && dst) {
      std::vector<char> vec(EMB_DIM * 4);
      int idx = 0, keptv = 0;
      bool ok = true;
      while (idx < total) {
        if (fread(vec.data(), 1, EMB_DIM * 4, src) != (size_t)(EMB_DIM * 4)) { ok = false; break; }
        if (!del[idx] &&
            fwrite(vec.data(), 1, EMB_DIM * 4, dst) != (size_t)(EMB_DIM * 4)) { ok = false; break; }
        if (!del[idx]) keptv++;
        idx++;
      }
      fclose(src);
      fclose(dst);
      if (ok && idx == total && keptv == kept) {
        remove("/conscious/mem/emb.bin");
        if (rename("/conscious/mem/emb.bin.tmp", "/conscious/mem/emb.bin") == 0) {
          embCount_ = keptv;
          aligned = true;
        }
      } else {
        remove("/conscious/mem/emb.bin.tmp");
      }
    } else {
      if (src) fclose(src);
      if (dst) fclose(dst);
      remove("/conscious/mem/emb.bin.tmp");
    }
  }
  if (!aligned) { fs_remove(EMB_PATH); embCount_ = 0; }
  ESP_LOGI(TAG, "整理完成：删 %d 条，剩 %d 条（向量%s）", cnt, kept, aligned ? "同步压缩" : "作废重建");
}

// ================= noveltyOf（失败不熔断，独白后堆紧张会假失败） =================
float MemorySystem::noveltyOf(const std::string& text) {
  if (embFail_ >= 3) return -1;
  float* qv = (float*)malloc(EMB_DIM * 4);
  if (!qv) return -1;
  embLastMs_ = laap_millis();
  float novelty = -1;
  if (call_embedding(text, qv)) {
    embFail_ = 0;
    float maxCos = 0;
    // emb.bin 满容 300 条 × 4KB = 1.2MB：fs_read 全量进内部堆必分配失败——
    // 与 recallSmart 同款 FILE* 流式逐向量读（审计 2026-10-05）
    FILE* ef = fopen("/conscious/mem/emb.bin", "rb");
    if (ef) {
      float* rv = (float*)malloc(EMB_DIM * 4);
      if (rv) {
        while (fread(rv, 1, EMB_DIM * 4, ef) == (size_t)(EMB_DIM * 4)) {
          float c = cosine_of(qv, rv, EMB_DIM);
          if (c > maxCos) maxCos = c;
        }
        free(rv);
      }
      fclose(ef);
    }
    novelty = 1.0f - maxCos;
    if (novelty < 0) novelty = 0;
  }
  free(qv);
  return novelty;
}

void MemorySystem::clearAll() {
  fs_remove(EP_PATH);
  fs_remove("/mem/semantic.txt");
  fs_remove(EMB_PATH);
  fs_remove(REL_PATH);
  fs_remove("/mem/intents.txt");
  fs_remove("/mem/feedback.jsonl");
  count_ = 0; workLen_ = 0; workHead_ = 0;
  embCount_ = 0; embFail_ = 0;
}

void MemorySystem::onRestored() {
  embCount_ = 0; embFail_ = 0;
  count_ = 0;
  std::string all;
  if (fs_read(EP_PATH, all)) for (char c : all) if (c == '\n') count_++;
  reloadWork();
}

void MemorySystem::reloadWork() {
  workLen_ = 0; workHead_ = 0;
  std::string all;
  if (!fs_read(EP_PATH, all)) return;
  std::vector<std::string> tail;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos) eol = all.size();
    std::string l = all.substr(pos, eol - pos);
    pos = eol + 1;
    while (!l.empty() && (l.front() == ' ' || l.front() == '\r')) l.erase(l.begin());
    while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
    if (l.empty()) continue;
    if (l.find("\"r\":\"") == std::string::npos || l.find("\"x\":\"") == std::string::npos) continue;
    tail.push_back(l);
    if ((int)tail.size() > WORK_MAX) tail.erase(tail.begin());
  }
  for (auto& l : tail) {
    size_t rp = l.find("\"r\":\"");
    size_t xp = l.find("\"x\":\"");
    std::string role = l.substr(rp + 5, l.find('"', rp + 5) - (rp + 5));
    std::string x = extract_x(l);
    std::string entry = role + ":" + x;
    if (entry.length() > 160) entry = utf8_cut(entry, 160);
    work_[workHead_] = entry;
    workHead_ = (workHead_ + 1) % WORK_MAX;
    if (workLen_ < WORK_MAX) workLen_++;
  }
}

}  // namespace laap
