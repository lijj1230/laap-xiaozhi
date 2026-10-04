#pragma once
// ============================================================
// LAAP-lite 认知核心（移植自 laap-esp32 laap_cognition，语义 1:1）
//   NeedsSystem  ← 需求层（PSI 内在需求，稳态化饱和增长）
//   EmotionEngine← PAD 情绪（mood 由需求+愉悦度合成）
//   WorldModel   ← internal_world（世界模型快照 JSON）
//   Evolution    ← hebbian_learner（性格/权重随经历进化）
// 移植映射：String→std::string、millis→esp_timer、LittleFS→laap_fs、
//           ESP.getFreeHeap→esp_get_free_heap_size、Serial→ESP_LOG
// ============================================================
#include <cstdint>
#include <string>

namespace laap {

struct Needs {
  float energy = 0.30f;     // 能量/休息需求
  float curiosity = 0.40f;  // 好奇/探索需求
  float social = 0.45f;     // 社交/归属需求
  float security = 0.20f;   // 安全/秩序需求
  float expression = 0.35f; // 表达/创造需求
};

enum class Mood : uint8_t { Calm, Happy, Curious, Excited, Lonely, Anxious, Tired };

class Cognition {
public:
  void begin();                       // 加载进化状态，世代+1
  void tick(float dtMin);             // PSI 心跳：需求随时间演化
  void onUserInteraction();           // 主人说话：社交/好奇释放
  void onExpressed(bool success);     // 完成一次表达
  void onMonologue();                 // 独白也算"说了话"（满足度低于对话）
  void onDiscovery(float gain01);     // 发现新知（active inference，gain=新颖度 0..1）

  // ---- R3 预测-误差回环 ----
  enum : uint8_t { EXP_NONE = 0, EXP_OWNER_COME, EXP_OWNER_AWAY, EXP_WORLD_ACTIVE, EXP_WORLD_QUIET };
  void setExpectation(uint8_t cat);
  void expectOutcome(bool fulfilled);
  uint8_t expectation() const { return expCat_; }
  uint32_t expectAtMs() const { return expAtMs_; }
  void expectEmaLoad(const uint8_t* blob);
  void expectEmaBlob(uint8_t* out);
  float expectPrecision(uint8_t cat) const;
  float expEma_[5] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
  uint16_t expN_[5] = {0, 0, 0, 0, 0};

  // ---- C1 全局门控广播 ----
  float broadcastSalience(const char* kind, const std::string& text, float salience, bool writeBeat = true);
  float lastBroadcastTh() const { return bcTh_; }
  std::string broadcastLine() const;
  uint32_t bcCount() const { return bcCount_; }
  std::string metaLine(bool calm = false) const;

  // ---- C3 dominance 负反馈 ----
  void onNegativeFeedback(const char* src);
  void socNudge(float d);
  float expressGain() const { return negGain_; }
  void onExpressSuppressed() { n_.expression *= 0.5f; }
  float negGain_ = 1.0f;
  uint32_t negGainMs_ = 0;

  float bcTh_ = 0;
  std::string bcText_;
  uint32_t lastBcMs_ = 0;
  uint32_t bcCount_ = 0;

  std::string expectLine() const;
  void noteSurprise(float s01);

  // ---- 意图栈（PIANO goals） ----
  bool addIntent(const std::string& text, uint32_t ts);
  void dropIntent(int i);
  void dropStaleIntents(uint32_t nowTs);
  void clearAllIntents();
  void resetEvolution();
  std::string intentsLine() const;
  const std::string& intent(int i) const { return intents_[i]; }
  int intentCount() const { return intentN_; }
  void onError();
  void sense(float motion, int rssi);
  void senseBody(float tempC, int rssi, uint32_t upMs, float dtMin);
  void onButtonPress();

  // ---- 信任 ----
  float trust = 0.60f;
  void trustUpdate(float dPos, float dNeg);
  void onLetdown(float expectation01);
  float letdown = 0;

  const Needs& needs() const { return n_; }
  Mood mood() const;
  float dominance() const;
  const char* goalCn() const;
  const char* moodKey() const;
  const char* moodCn() const;

  std::string worldJson() const;
  std::string traitsLine() const;

  void evolveAfterChat(int userBytes);
  void saveEvolution(bool force = false);
  void reloadEvolution() { loadEvolution(); }
  uint32_t generation() const { return gen_; }
  uint32_t cycles() const { return cycles_; }
  uint32_t chats() const { return chats_; }
  void incCycle() { cycles_++; }

  // 世界模型字段
  float motionLevel = 0;
  int rssiDb = 0;
  uint32_t lastUserMs = 0;
  float bodyTempC = 0;
  float bodyStrain = 0;

private:
  Needs n_;
  Needs savedN_;
  float savedPl_ = 0.5f;
  uint8_t expCat_ = 0;
  uint32_t expAtMs_ = 0;
  std::string expLastTxt_;
  float openness_ = 0.5f;
  float sociability_ = 0.5f;
  float sensitivity_ = 0.5f;
  uint32_t gen_ = 0, cycles_ = 0, chats_ = 0;
  float pleasure_ = 0.5f;
  bool evoDirty_ = false;
  uint32_t lastEvoSaveCycle_ = 0;
  std::string intents_[3];
  uint32_t intentBorn_[3] = {0, 0, 0};
  int intentN_ = 0;
  void loadIntents();
  void saveIntents();
  bool loadEvolution();
};

extern Cognition mind;

// 毫秒时钟（esp_timer）
uint32_t laap_millis();

}  // namespace laap
