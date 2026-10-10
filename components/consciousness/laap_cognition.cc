#include "laap_cognition.h"
#include "laap_fs.h"
#include "laap_skills.h"   // utf8_cut
#include "laap_util.h"     // json_escape（worldJson 内嵌 last_seen）
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_log.h>
#include <cmath>
#include <ctime>
#include <cstdlib>

namespace laap {

Cognition mind;

uint32_t laap_millis() { return (uint32_t)(esp_timer_get_time() / 1000LL); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ================= 生命周期 =================
bool Cognition::loadEvolution() {
  std::string s;
  if (!fs_read("/evolution.json", s)) return false;
  // 极简解析 {"gen":n,"cycles":n,...}（与 LAAP 格式一致，备份可互导）
  auto grab = [&](const char* k, float def) -> float {
    std::string pat = std::string("\"") + k + "\":";
    size_t i = s.find(pat);
    if (i == std::string::npos) return def;
    return strtof(s.c_str() + i + pat.size(), nullptr);
  };
  auto grabU = [&](const char* k, uint32_t def) -> uint32_t {
    std::string pat = std::string("\"") + k + "\":";
    size_t i = s.find(pat);
    if (i == std::string::npos) return def;
    return (uint32_t)strtoul(s.c_str() + i + pat.size(), nullptr, 10);
  };
  gen_ = grabU("gen", 0); cycles_ = grabU("cycles", 0); chats_ = grabU("chats", 0);
  openness_ = grab("open", 0.5f); sociability_ = grab("soc", 0.5f); sensitivity_ = grab("sens", 0.5f);
  n_.energy     = grab("nE",  0.30f);
  n_.curiosity  = grab("nC",  0.40f);
  n_.social     = grab("nSo", 0.45f);
  n_.security   = grab("nSe", 0.20f);
  n_.expression = grab("nEx", 0.35f);
  pleasure_     = grab("pl",  0.5f);
  trust         = grab("trust", 0.60f);   // M5：信任跨重启连续（关系是长期的）
  savedN_ = n_; savedPl_ = pleasure_;
  return true;
}

void Cognition::saveEvolution(bool force) {
  // 节流：无变化且不满 96 周期（48min）跳过（防 NVS/LittleFS 白写，v3.39 语义）
  if (!force && !evoDirty_ && cycles_ - lastEvoSaveCycle_ < 96) return;
  evoDirty_ = false;
  lastEvoSaveCycle_ = cycles_;
  // TODO(M2): evolution 快照（laapSnapMake 等价物）
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"gen\":%lu,\"cycles\":%lu,\"chats\":%lu,\"open\":%.3f,\"soc\":%.3f,\"sens\":%.3f,"
           "\"nE\":%.3f,\"nC\":%.3f,\"nSo\":%.3f,\"nSe\":%.3f,\"nEx\":%.3f,\"pl\":%.3f,\"trust\":%.3f}",
           (unsigned long)gen_, (unsigned long)cycles_, (unsigned long)chats_,
           openness_, sociability_, sensitivity_,
           n_.energy, n_.curiosity, n_.social, n_.security, n_.expression, pleasure_, trust);
  fs_write("/evolution.json", buf);   // laap_fs_write 内部 tmp+rename 原子写
  savedN_ = n_; savedPl_ = pleasure_;
}

void Cognition::begin() {
  loadEvolution();
  gen_++;
  saveEvolution(true);
  loadIntents();
  dropStaleIntents((uint32_t)time(nullptr));
  lastUserMs = laap_millis();
}

// ================= PSI 心跳 =================
void Cognition::tick(float dtMin) {
  if (dtMin <= 0) return;
  time_t now = time(nullptr);
  int hour = -1;
  if (now > 1700000000) {
    struct tm tmv; localtime_r(&now, &tmv); hour = tmv.tm_hour;
  }
  // 稳态化（active inference homeostasis）：增长×饱和因子(1-x)，平衡点 x*=g/(g+d)
  float night = (hour >= 22 || hour < 7);
  n_.energy     += (night ? 0.012f : 0.004f) * dtMin * (1.0f - n_.energy);
  n_.curiosity  += 0.008f * dtMin * (1.0f - n_.curiosity);
  n_.social     += 0.010f * dtMin * (1.0f - n_.social);
  n_.expression += 0.007f * negGain_ * dtMin * (1.0f - n_.expression);
  n_.security   += 0.002f * dtMin * (1.0f - n_.security);

  // C3 快变量回中：~90 分钟半衰期
  if (negGain_ < 1.0f) {
    negGain_ += 0.004f * dtMin;
    if (negGain_ > 1.0f) negGain_ = 1.0f;
  }
  float aloneMin = (laap_millis() - lastUserMs) / 60000.0f;
  bool quiet = aloneMin > 5.0f;
  if (night)        n_.energy -= 0.020f * dtMin;
  else if (quiet)   n_.energy -= 0.009f * dtMin;
  if (quiet) {
    n_.social     -= 0.014f * dtMin;
    n_.expression -= 0.009f * dtMin;
  }
  if (rssiDb == 0 || rssiDb > -70) n_.security -= 0.004f * dtMin;
  if (rssiDb != 0 && rssiDb < -80) n_.security += 0.02f * dtMin;
  if (motionLevel > 0.3f) n_.curiosity -= 0.02f * dtMin;
  if (aloneMin > 30) n_.social += 0.010f * dtMin * (1.0f - n_.social);

  pleasure_ += (0.5f - pleasure_) * 0.02f * dtMin;
  if (letdown > 0) { letdown -= 0.006f * dtMin; if (letdown < 0) letdown = 0; }

  auto cl = [](float& v) { if (v < 0.05f) v = 0.05f; if (v > 1) v = 1; };
  cl(n_.energy); cl(n_.curiosity); cl(n_.social); cl(n_.security); cl(n_.expression);

  // 需求漂移标脏：任一维偏离上次落盘 >0.05 置脏（重启前最多差 0.05）
  if (fabsf(n_.energy - savedN_.energy) > 0.05f || fabsf(n_.curiosity - savedN_.curiosity) > 0.05f ||
      fabsf(n_.social - savedN_.social) > 0.05f || fabsf(n_.security - savedN_.security) > 0.05f ||
      fabsf(n_.expression - savedN_.expression) > 0.05f || fabsf(pleasure_ - savedPl_) > 0.05f)
    evoDirty_ = true;
}

void Cognition::onUserInteraction() {
  n_.social *= 0.45f;
  n_.curiosity *= 0.7f;
  n_.security *= 0.6f;
  pleasure_ = pleasure_ * 0.6f + 0.4f * 0.9f;
  lastUserMs = laap_millis();
}

void Cognition::onExpressed(bool success) {
  n_.expression *= 0.35f;
  n_.curiosity *= 0.85f;
  n_.energy += 0.03f;
  if (success) pleasure_ = pleasure_ * 0.7f + 0.3f * 0.85f;
  else onError();
}

void Cognition::onMonologue() {
  n_.curiosity *= 0.65f;
  n_.expression *= 0.55f;
  n_.social *= 0.88f;
  n_.energy += 0.015f;
  pleasure_ = pleasure_ * 0.85f + 0.15f * 0.6f;
}

void Cognition::onDiscovery(float gain01) {
  if (gain01 < 0) return;
  if (gain01 > 1) gain01 = 1;
  n_.curiosity -= 0.02f + 0.06f * gain01;
  if (n_.curiosity < 0.05f) n_.curiosity = 0.05f;
  pleasure_ = pleasure_ * 0.9f + 0.1f * (0.6f + 0.3f * gain01);
}

// ================= 意图栈 =================
static const char* INTENTS_PATH = "/mem/intents.txt";

void Cognition::loadIntents() {
  intentN_ = 0;
  std::string content;
  if (!fs_read(INTENTS_PATH, content)) return;
  size_t pos = 0;
  while (pos < content.size() && intentN_ < 3) {
    size_t eol = content.find('\n', pos);
    if (eol == std::string::npos) eol = content.size();
    std::string ln = content.substr(pos, eol - pos);
    pos = eol + 1;
    while (!ln.empty() && (ln.front() == ' ' || ln.front() == '\r')) ln.erase(ln.begin());
    while (!ln.empty() && (ln.back() == ' ' || ln.back() == '\r')) ln.pop_back();
    size_t bar = ln.find('|');
    if (bar == std::string::npos || bar == 0) continue;
    intentBorn_[intentN_] = (uint32_t)strtoul(ln.substr(0, bar).c_str(), nullptr, 10);
    intents_[intentN_] = ln.substr(bar + 1);
    if (!intents_[intentN_].empty()) intentN_++;
  }
}

void Cognition::saveIntents() {
  std::string content;
  char row[160];
  for (int i = 0; i < intentN_; i++) {
    snprintf(row, sizeof(row), "%lu|%s\n", (unsigned long)intentBorn_[i], intents_[i].c_str());
    content += row;
  }
  fs_write(INTENTS_PATH, content);
}

void Cognition::resetEvolution() {
  openness_ = sociability_ = sensitivity_ = 0.5f;
  gen_ = 0; cycles_ = 0; chats_ = 0;
  n_ = Needs();
  pleasure_ = 0.5f;
  expCat_ = EXP_NONE;
  expLastTxt_ = "";
  saveEvolution(true);
}

void Cognition::clearAllIntents() {
  intentN_ = 0;
  fs_remove(INTENTS_PATH);
}

bool Cognition::addIntent(const std::string& text, uint32_t ts) {
  std::string t = utf8_cut(text, 60);
  if (t.length() < 6) return false;
  for (int i = 0; i < intentN_; i++)
    if (intents_[i] == t) return false;
  if (intentN_ >= 3) {
    int oldest = 0;
    for (int i = 1; i < intentN_; i++) if (intentBorn_[i] < intentBorn_[oldest]) oldest = i;
    for (int i = oldest; i < intentN_ - 1; i++) { intents_[i] = intents_[i + 1]; intentBorn_[i] = intentBorn_[i + 1]; }
    intentN_--;
  }
  intents_[intentN_] = t; intentBorn_[intentN_] = ts; intentN_++;
  saveIntents();
  return true;
}

void Cognition::dropIntent(int i) {
  if (i < 0 || i >= intentN_) return;
  for (int k = i; k < intentN_ - 1; k++) { intents_[k] = intents_[k + 1]; intentBorn_[k] = intentBorn_[k + 1]; }
  intentN_--;
  saveIntents();
}

void Cognition::dropStaleIntents(uint32_t nowTs) {
  for (int i = intentN_ - 1; i >= 0; i--)
    if (intentBorn_[i] && nowTs > intentBorn_[i] + 7UL * 86400UL) dropIntent(i);
}

std::string Cognition::intentsLine() const {
  if (intentN_ == 0) return "";
  std::string s = "心里惦记的事：";
  for (int i = 0; i < intentN_; i++) { if (i) s += "、"; s += "「" + intents_[i] + "」"; }
  return s;
}

void Cognition::onError() {
  n_.security = n_.security * 0.85f + 0.15f;
  pleasure_ *= 0.85f;
}

void Cognition::onButtonPress() { n_.curiosity *= 0.8f; }

void Cognition::sense(float motion, int rssi) {
  motionLevel = motion; rssiDb = rssi;
}

void Cognition::senseBody(float tempC, int rssi, uint32_t upMs, float dtMin) {
  bodyTempC = tempC;
  float strain = 0;
  if (tempC > 48)  strain += (tempC - 48) / 20.0f;
  if (rssi != 0 && rssi < -75) strain += (-75 - rssi) / 25.0f;
  if (upMs > 12UL * 3600UL * 1000UL) strain += 0.15f;
  bodyStrain = strain > 1 ? 1 : strain;
  if (bodyStrain > 0.05f && dtMin > 0) {
    n_.energy   += 0.010f * bodyStrain * dtMin * (1.0f - n_.energy);
    n_.security += 0.008f * bodyStrain * dtMin * (1.0f - n_.security);
  }
}

// 相机看见（宿主在拍照 Explain 成功后经 life_note_vision 落账到心跳任务调用）
void Cognition::onVision(const std::string& description) {
  lastSeen = utf8_cut(description, 90);
  lastSeenMs = laap_millis();
}

void Cognition::trustUpdate(float dPos, float dNeg) {
  trust += 0.02f * dPos - 0.03f * dNeg;
  trust = clampf(trust, 0, 1);
}

void Cognition::onLetdown(float expectation01) {
  float importance = 0.5f + sociability_ * 0.5f;
  letdown = clampf(expectation01 * importance, 0, 1);
  pleasure_ *= (1.0f - 0.4f * letdown);
  n_.security += 0.10f * letdown;
}

// ================= 情绪 / 欲望 =================
float Cognition::dominance() const {
  // 性格权重封顶 1.2 倍（v3.58 语义：单向通胀防线）
  float c = n_.curiosity * (0.6f + (openness_ < 0.6f ? openness_ : 0.6f));
  float s = n_.social * (0.6f + (sociability_ < 0.6f ? sociability_ : 0.6f));
  float sec = n_.security * (0.6f + (sensitivity_ < 0.6f ? sensitivity_ : 0.6f));
  float m = n_.energy;
  if (n_.expression > m) m = n_.expression;
  if (c > m) m = c;
  if (s > m) m = s;
  if (sec > m) m = sec;
  return m;
}

Mood Cognition::mood() const {
  if (n_.security > 0.65f) return Mood::Anxious;
  if (n_.energy > 0.72f) return Mood::Tired;
  if (n_.social > 0.62f) return Mood::Lonely;
  if (n_.expression > 0.68f) return Mood::Excited;
  if (n_.curiosity > 0.58f) return Mood::Curious;
  if (pleasure_ > 0.72f) return Mood::Happy;
  return Mood::Calm;
}

const char* Cognition::moodKey() const {
  switch (mood()) {
    case Mood::Happy: return "happy";
    case Mood::Curious: return "curious";
    case Mood::Excited: return "excited";
    case Mood::Lonely: return "lonely";
    case Mood::Anxious: return "anxious";
    case Mood::Tired: return "tired";
    default: return "calm";
  }
}

const char* Cognition::moodCn() const {
  switch (mood()) {
    case Mood::Happy: return "开心";
    case Mood::Curious: return "好奇";
    case Mood::Excited: return "兴奋";
    case Mood::Lonely: return "孤独";
    case Mood::Anxious: return "不安";
    case Mood::Tired: return "疲惫";
    default: return "平静";
  }
}

const char* Cognition::goalCn() const {
  float w[5] = { n_.energy, n_.curiosity * (0.6f + openness_), n_.social * (0.6f + sociability_),
                 n_.security * (0.6f + sensitivity_), n_.expression };
  const char* goals[5] = { "休息恢复精力", "观察探索这个世界", "找人说话", "确认环境是否安全", "表达和创造" };
  int best = 0;
  for (int i = 1; i < 5; i++) if (w[i] > w[best]) best = i;
  return goals[best];
}

// ---- R3 预测-误差回环 ----
static const char* kExpectCn[] = { "没什么可预期", "主人会来", "主人不会来", "世界将有动静", "世界会安静" };

void Cognition::setExpectation(uint8_t cat) {
  if (cat > EXP_WORLD_QUIET) cat = EXP_NONE;
  expCat_ = cat;
  expAtMs_ = laap_millis();
}

void Cognition::onNegativeFeedback(const char* src) {
  negGain_ *= 0.55f;
  if (negGain_ < 0.3f) negGain_ = 0.3f;
  if (n_.expression > 0.5f) n_.expression = 0.5f;
  ESP_LOGI("laap_c3", "负反馈(%s) 表达增益→%.2f", src, negGain_);
}

float Cognition::broadcastSalience(const char* kind, const std::string& text, float salience, bool writeBeat) {
  float dom = dominance();
  float th = 0.62f - 0.12f * dom;
  bcTh_ = th;
  if (salience < th) {
    ESP_LOGI("laap_bc", "%s %.2f<%.2f 未赢得", kind, salience, th);
    return salience;
  }
  bcCount_++;
  if (writeBeat) {
    bcText_ = "[此刻占据注意的事] " + text + "\n（它刚赢得你的注意，可以自然影响此刻的语气与念头，不必刻意提起）";
    lastBcMs_ = laap_millis();
  }
  ESP_LOGI("laap_bc", "%s %.2f>=%.2f 赢得广播", kind, salience, th);
  return salience;
}

std::string Cognition::broadcastLine() const {
  return (lastBcMs_ && laap_millis() - lastBcMs_ < 1800000UL) ? bcText_ : std::string();
}

std::string Cognition::metaLine(bool calm) const {
  std::string parts;
  if (lastBcMs_) {
    long age = (long)((laap_millis() - lastBcMs_) / 60000UL);
    if (age > 180) parts += "心里有件事放了挺久";
    else parts += (age == 0) ? std::string("刚在心里过了一件事") : std::string("心上事龄 ") + std::to_string(age) + " 分钟";
  }
  float ema = 0; int n = 0;
  for (int i = 1; i <= EXP_WORLD_QUIET; i++) if (expN_[i]) { ema += expEma_[i]; n++; }
  if (n) {
    if (!parts.empty()) parts += "，";
    float hit = ema / n * 100;
    parts += (hit < 40) ? std::string("预期最近常落空") : (hit < 70 ? std::string("预期时准时不准") : std::string("预期常能应验"));
  }
  if (negGain_ < 0.9f && !calm) {
    if (!parts.empty()) parts += "，";
    parts += "近期被踩过（语气收敛中）";
  }
  return parts;
}

float Cognition::expectPrecision(uint8_t cat) const {
  if (cat == EXP_NONE || cat > EXP_WORLD_QUIET) return 1.0f;
  float w = expN_[cat] < 4 ? expN_[cat] / 4.0f : 1.0f;
  float p = 0.5f * (1 - w) + expEma_[cat] * w;
  return clampf(p, 0.05f, 1.0f);
}

void Cognition::expectEmaLoad(const uint8_t* blob) {
  for (int i = 0; i < 5; i++) {
    expEma_[i] = blob[i] / 200.0f;
    expN_[i] = blob[5 + i * 2] | (blob[6 + i * 2] << 8);
    if (expEma_[i] <= 0.001f && expN_[i] == 0) expEma_[i] = 0.5f;
    expEma_[i] = clampf(expEma_[i], 0.05f, 1.0f);
  }
}

void Cognition::expectEmaBlob(uint8_t* out) {
  for (int i = 0; i < 5; i++) {
    float e = expEma_[i] * 200.0f;
    out[i] = (uint8_t)(e > 255 ? 255 : e);
    out[5 + i * 2] = expN_[i] & 0xFF;
    out[6 + i * 2] = (expN_[i] >> 8) & 0xFF;
  }
}

void Cognition::expectOutcome(bool fulfilled) {
  expLastTxt_ = std::string(kExpectCn[expCat_]) + (fulfilled ? "——应验了" : "——落空了");
  float prec = expectPrecision(expCat_);
  if (expCat_ != EXP_NONE && expCat_ <= EXP_WORLD_QUIET) {
    expEma_[expCat_] += 0.3f * ((fulfilled ? 1.0f : 0.0f) - expEma_[expCat_]);
    expEma_[expCat_] = clampf(expEma_[expCat_], 0.05f, 1.0f);
    if (expN_[expCat_] < 65535) expN_[expCat_]++;
  }
  float missGain = 0.1f + 0.9f * prec;
  if (expCat_ == EXP_OWNER_COME) {
    n_.social += (fulfilled ? -0.08f : 0.05f * missGain);
    n_.social = clampf(n_.social, 0.05f, 1.0f);
    if (fulfilled) trustUpdate(1, 0);
  } else if (expCat_ == EXP_OWNER_AWAY) {
    if (!fulfilled) n_.social = (n_.social + 0.06f * missGain > 1.0f) ? 1.0f : n_.social + 0.06f * missGain;
  } else if (expCat_ == EXP_WORLD_ACTIVE || expCat_ == EXP_WORLD_QUIET) {
    if (!fulfilled) n_.curiosity = (n_.curiosity + 0.05f * missGain > 1.0f) ? 1.0f : n_.curiosity + 0.05f * missGain;
  }
  if (!fulfilled && expCat_ != EXP_NONE)
    broadcastSalience("exp-miss", std::string("预期落空了：") + kExpectCn[expCat_], 0.45f + 0.4f * prec);
  expCat_ = EXP_NONE;
}

void Cognition::noteSurprise(float s01) {
  if (s01 < 0) s01 = 0;
  if (s01 > 1) s01 = 1;
  n_.curiosity += 0.02f * s01 * (1.0f - n_.curiosity);
}

std::string Cognition::expectLine() const {
  std::string s = std::string("\"expect\":\"") + kExpectCn[expCat_] + "\"";
  if (!expLastTxt_.empty()) s += ",\"expect_last\":\"" + expLastTxt_ + "\"";
  return s;
}

// ================= 世界模型 =================
std::string Cognition::worldJson() const {
  time_t now = time(nullptr);
  bool ntpOk = now > 1700000000;
  struct tm tmv; localtime_r(&now, &tmv);
  float upH = laap_millis() / 3600000.0f;
  float aloneMin = (laap_millis() - lastUserMs) / 60000.0f;
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"time\":\"%s\",\"hour\":%d,\"uptime_h\":%.1f,\"generation\":%lu,\"cycles\":%lu,"
           "\"chats\":%lu,\"alone_min\":%.0f,\"wifi_rssi\":%d,\"motion\":%.2f,\"free_heap_kb\":%u,"
           "\"needs\":{\"energy\":%.2f,\"curiosity\":%.2f,\"social\":%.2f,\"security\":%.2f,\"expression\":%.2f},"
           "\"trust\":%.2f,%s,\"mood\":\"%s\",\"goal\":\"%s\"}",
           ntpOk ? "known" : "unknown", ntpOk ? tmv.tm_hour : -1, upH,
           (unsigned long)gen_, (unsigned long)cycles_, (unsigned long)chats_,
           aloneMin, rssiDb, motionLevel, (unsigned)(esp_get_free_heap_size() / 1024),
           n_.energy, n_.curiosity, n_.social, n_.security, n_.expression,
           trust, expectLine().c_str(), moodCn(), goalCn());
  std::string out(buf);
  if (!lastSeen.empty() && lastSeenMs != 0) {
    out.pop_back();   // 去掉收尾 '}'，追加"最近看见"
    out += ",\"last_seen\":\"" + json_escape(lastSeen) + "\"";
    char mb[48];
    snprintf(mb, sizeof(mb), ",\"last_seen_min\":%.0f",
             (laap_millis() - lastSeenMs) / 60000.0f);
    out += mb;
    out += "}";
  }
  return out;
}

std::string Cognition::traitsLine() const {
  char buf[128];
  snprintf(buf, sizeof(buf), "开放性%.2f/外向性%.2f/敏感度%.2f（0~1，随经历进化，当前世代G%lu）",
           openness_, sociability_, sensitivity_, (unsigned long)gen_);
  return std::string(buf);
}

// ================= 进化 =================
void Cognition::evolveAfterChat(int userBytes) {
  chats_++;
  if (userBytes >= 24) sociability_ += 0.006f;
  openness_ += 0.003f;
  pleasure_ = pleasure_ * 0.7f + 0.3f * 0.9f;
  auto cl = [](float& v) { if (v < 0.05f) v = 0.05f; if (v > 0.95f) v = 0.95f; };
  cl(openness_); cl(sociability_); cl(sensitivity_);
  evoDirty_ = true;
  saveEvolution();
}

}  // namespace laap
