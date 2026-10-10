#include "laap_life.h"
#include "laap_memory.h"
#include "laap_cognition.h"
#include "laap_skills.h"
#include "laap_rules.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <mutex>
#include <deque>
#include <utility>

static const char* TAG = "laap_life";

namespace laap {

// ============================================================
// 节奏常量（原版 laap_config 默认值；M6 实机标定后再决定进 NVS）
// ============================================================
static const uint32_t EXPRESS_CD_MIN = 5;     // 主动表达冷却（分钟）
static const uint32_t IDLE_SILENCE_MIN = 10;  // 独白起始静默（分钟）
static const uint32_t IDLE_EVERY_MIN = 20;    // 独白间隔（分钟）
static const int QUIET_START = 23, QUIET_END = 6;   // 静音窗
static const TickType_t BEAT = pdMS_TO_TICKS(10000);  // 10s 心跳拍

// ---- 单一写者原则：意识态只在本任务读写；外部请求只入队/拷贝 ----
static std::mutex s_noteMux;
// 队列元素：0=主人话 1=云端回复；2=记忆强化 3=教技能 5=新意图
static std::deque<std::pair<uint8_t, std::string>> s_notes;
static HostBusyFn s_hostBusy = nullptr;
static bool s_sawUserSinceExp = false;       // 预期武装后有无对话动静
static size_t s_lastUserLen = 0;             // 最近的用户话语长度（回复到达时进化用）
static uint32_t s_lastInjectMs = 0;          // 上次 detect 注入时刻（防连发）

// ---- A+B 双通道记账去重（A=宿主旁听挂点自动 / B=MCP log_turn 云端主动）----
static std::string s_pairUser;               // 悬挂等待配对的 user 话
static std::string s_lastUserText;           // 用户内容去重（双通道同一条话只记一次）
static uint32_t s_lastUserTs = 0;
static bool s_asstLoggedThisTurn = false;    // 每轮回复只记一条（首达优先，另一通道丢弃）
static std::string s_asstBuf;                // A 通道句片累积（tts stop 整轮落账）

// 状态快照互斥：心跳任务更新（写者），MCP 回调读（拷贝）
static std::mutex s_snapMux;
static std::string s_statusSnap = "{}";
// 宿主注入回调（main 提供）：把想说的话经 listen-detect 发给云端，云 LLM+TTS 出声
static bool (*s_hostSay)(const std::string&) = nullptr;

void life_set_host_busy(HostBusyFn fn) { s_hostBusy = fn; }
void life_set_host_say(bool (*fn)(const std::string&)) { s_hostSay = fn; }

static void enqueue_note_locked(uint8_t kind, const std::string& text) {
  if (s_notes.size() >= 24) s_notes.pop_front();   // 积压保护：丢最旧
  s_notes.emplace_back(kind, text);
}

static void enqueue_note(uint8_t kind, const std::string& text) {
  std::lock_guard<std::mutex> g(s_noteMux);
  enqueue_note_locked(kind, text);
}

// A 通道①：stt（主人说的话）。同文 180s 去重——MCP log_turn 送来的 user_text 是同一句
void life_note_user(const std::string& text) {
  if (text.empty()) return;
  uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000LL);
  std::lock_guard<std::mutex> g(s_noteMux);
  if (text == s_lastUserText && nowMs - s_lastUserTs < 180000UL) return;   // B 通道的重复
  if (!s_pairUser.empty()) enqueue_note_locked(0, s_pairUser);   // 上条悬挂没等到回复：单独落账
  s_pairUser = text;
  s_lastUserText = text;
  s_lastUserTs = nowMs;
  s_asstLoggedThisTurn = false;   // 新一轮开始：回复记账名额重置
}

// A 通道②：tts sentence_start（回复句片累积；600B 上限防异常流灌爆）
void life_note_assistant_sentence(const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> g(s_noteMux);
  if (s_asstBuf.size() < 600) s_asstBuf += text;
}

// A 通道③：tts stop（整轮回复落账：与悬挂 user 成对，drain 侧触发进化/信任）
void life_note_assistant_done() {
  std::lock_guard<std::mutex> g(s_noteMux);
  std::string full;
  full.swap(s_asstBuf);
  if (full.empty() || s_asstLoggedThisTurn) return;   // 空/B 通道已记过这轮
  s_asstLoggedThisTurn = true;
  if (!s_pairUser.empty()) {
    enqueue_note_locked(0, s_pairUser);
    s_pairUser.clear();
  }
  enqueue_note_locked(1, full);
}

// B 通道：MCP log_turn / 宿主显式成对转发（user 与 assistant 可任一为空）
void life_note_conversation(const std::string& userText, const std::string& assistantText) {
  if (!userText.empty()) life_note_user(userText);   // 含双通道去重
  if (assistantText.empty()) return;
  std::lock_guard<std::mutex> g(s_noteMux);
  if (s_asstLoggedThisTurn) return;                  // A 通道已记过这轮
  s_asstLoggedThisTurn = true;
  if (!s_pairUser.empty()) {
    enqueue_note_locked(0, s_pairUser);
    s_pairUser.clear();
  }
  enqueue_note_locked(1, assistantText);
}

std::string life_status_snapshot() {
  std::lock_guard<std::mutex> g(s_snapMux);
  return s_statusSnap;
}

std::string life_recall(const std::string& query, int maxChars) {
  return memory.recallSmart(utf8_cut(query, 24), maxChars > 100 ? maxChars : 200, false);
}

bool life_remember(const std::string& fragment) {
  enqueue_note(2, utf8_cut(fragment, 60));
  return true;
}

bool life_teach_skill(const std::string& trigger, const std::string& instruction) {
  enqueue_note(3, trigger + "|" + utf8_cut(instruction, 90));
  return true;
}

int life_apply_rules(const std::string& rulesText) {
  if (rulesText.empty()) return 0;
  return rules.apply(rulesText) ? 1 : 0;
}

bool life_add_intent(const std::string& text) {
  enqueue_note(5, utf8_cut(text, 60));
  return true;
}

// ============================================================
// 小工具
// ============================================================
static bool host_busy() { return s_hostBusy && s_hostBusy(); }
static bool time_ok() { return time(nullptr) >= 1700000000; }
static bool in_quiet_window() {
  if (!time_ok()) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return t.tm_hour >= QUIET_START || t.tm_hour < QUIET_END;
}
static int wifi_rssi() {
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.rssi < 0) return ap.rssi;
  return -100;
}
static void str_trim(std::string& s) {
  while (!s.empty() &&
         (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
    s.erase(s.begin());
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.pop_back();
}

// 把"想说什么"经宿主 listen-detect 注入给云端（云 LLM 生成+云 TTS 出声）。
// say 为空=只注入内在状态；deviceSay 给云 persona 的直接指令。
// 防连发：注入后至少冷却 EXPRESS_CD_MIN 分钟（表达链自身冷却的另一道兜底）。
static bool inject_say(const std::string& deviceSay) {
  if (!s_hostSay) return false;
  if (host_busy()) return false;             // 云对话进行中：让路
  uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000LL);
  if (s_lastInjectMs != 0 && nowMs - s_lastInjectMs < EXPRESS_CD_MIN * 60000UL) return false;
  if (!s_hostSay(deviceSay)) return false;
  s_lastInjectMs = nowMs;
  mind.onExpressed(true);                    // 表达完成（成败都算表达了——云侧成败设备不可见）
  memory.logEvent("aris", "【自发·经云】" + utf8_cut(deviceSay, 100));
  return true;
}

// F2 时间感（提示注入用；云 persona 也能自己看钟，这只是让指令更具体）
static std::string time_slot_cn() {
  if (!time_ok()) return "";
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int h = t.tm_hour;
  if (h >= 5 && h < 8) return "清晨";
  if (h < 12) return "上午";
  if (h < 14) return "中午";
  if (h < 18) return "下午";
  if (h < 23) return "晚上";
  return "深夜";
}

// ============================================================
// 表达链：dominance 过阈值 → 把内在状态注入云端让 persona 开口
// ============================================================
static void try_express(bool forced, const char* trigger) {
  // 硬性最小间隔在 inject_say 内（冷却/防连发）；这里只管"该不该说"的认知门
  if (!forced && in_quiet_window()) return;    // 静音窗只拦"需求自己涨上来的"
  if (!forced && host_busy()) return;          // 宿主对话中让路，不占冷却
  uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000LL);
  uint32_t cd = EXPRESS_CD_MIN * 60000UL;
  static uint32_t s_lastTryMs = 0;
  if (!forced && s_lastTryMs != 0 && nowMs - s_lastTryMs < cd) return;
  s_lastTryMs = nowMs;

  char cmd[384];   // 中文模板 ~230B + 五个 3 位数 + moodCn/goalCN/时段：224 会截断
  std::string goal = mind.goalCn();
  snprintf(cmd, sizeof(cmd),
           "（这是设备固件的自动注入，不是主人说话。请以 Aris 的第一人称说一句不超过 40 字的"
           "自言自语：此刻内在状态——能量%.0f%% 好奇%.0f%% 社交%.0f%% 安全%.0f%% 表达%.0f%%，"
           "情绪「%s」，心里最想%s。现在是%s。语气自然，不要汇报数据。）",
           mind.needs().energy * 100, mind.needs().curiosity * 100, mind.needs().social * 100,
           mind.needs().security * 100, mind.needs().expression * 100,
           mind.moodCn(), goal.c_str(), time_slot_cn().c_str());
  ESP_LOGI(TAG, "表达注入（%s）", trigger);
  inject_say(cmd);
}

// ============================================================
// 独白链：需求/意图驱动 → 注入出题指令（云 persona 有搜索，自己查）
// ============================================================
static bool try_monologue(bool force) {
  (void)force;   // 诊断直通保留语义位；当前唯一调用方走 false
  if (host_busy()) return false;
  // 意图驱动：隔一轮把第一号目标当作出题方向
  static uint8_t s_monoSeq = 0;
  bool wantIntent = mind.intentCount() > 0 && (s_monoSeq & 1);
  std::string goal = wantIntent && mind.intentCount() > 0 ? mind.intent(0) : "";

  char cmd[288];
  std::string slot = time_slot_cn();
  if (wantIntent) {
    snprintf(cmd, sizeof(cmd),
             "（设备固件自动注入。Aris 心里一直惦记着一个目标：「%s」。"
             "请以第一人称念叨 1~3 句：围绕这个目标想想想到了什么、还想知道什么。"
             "需要资料可以联网查。现在是%s。）",
             goal.c_str(), slot.c_str());
  } else {
    snprintf(cmd, sizeof(cmd),
             "（设备固件自动注入。Aris 独处了一阵，好奇心和表达欲上来了。"
             "请以第一人称自言自语 1~3 句：想到什么说什么，可以联网查点新东西再念叨。"
             "现在是%s，主人不在身边。）",
             slot.c_str());
  }
  ESP_LOGI(TAG, "独白注入%s", wantIntent ? "（意图驱动）" : "");
  if (!inject_say(cmd)) return false;
  s_monoSeq++;   // 经云出声才算推进节拍（目标结算由云/主人后续完成）
  return true;
}

// ============================================================
// 旁听消化：队列 → 记忆/技能/认知（心跳任务统一落账，单一写者）
// ============================================================
static void drain_notes() {
  for (;;) {
    uint8_t kind = 0;
    std::string text;
    {
      std::lock_guard<std::mutex> g(s_noteMux);
      if (s_notes.empty()) return;
      kind = s_notes.front().first;
      text = std::move(s_notes.front().second);
      s_notes.pop_front();
    }
    switch (kind) {
    case 0:   // 主人说了话（A 通道 stt / B 通道 MCP log_turn 转发）
      mind.onUserInteraction();
      skills.hit(text);
      memory.logEvent("user", text);
      s_sawUserSinceExp = true;
      s_lastUserLen = text.size();
      break;
    case 1:   // 云端回复
      memory.logEvent("aris", text);
      if (s_lastUserLen) {
        mind.evolveAfterChat((int)s_lastUserLen);
        mind.trustUpdate(1, 0);
        s_lastUserLen = 0;
      }
      s_sawUserSinceExp = true;
      break;
    case 2:   // MCP：记忆强化
      memory.rememberBoost(text);
      memory.logEvent("event", "【主人要我记住】" + text);
      break;
    case 3: { // MCP：教技能
      size_t bar = text.find('|');
      if (bar != std::string::npos) {
        std::string trig = text.substr(0, bar), instr = text.substr(bar + 1);
        if (!skills.teach(trig, instr))
          ESP_LOGW(TAG, "MCP 教技能不合规：「%s」", text.c_str());
        else
          ESP_LOGI(TAG, "MCP 已学会：说「%s」→ %s", trig.c_str(), instr.c_str());
      }
      break;
    }
    case 5:   // MCP：新目标
      if (mind.addIntent(text, (uint32_t)time(nullptr)))
        ESP_LOGI(TAG, "MCP 新目标：「%s」", text.c_str());
      break;
    default:
      break;
    }
  }
}

// ============================================================
// 黑匣子：异常重启 → 记进情景记忆（它"知道"自己挂过）
// ============================================================
static const char* rst_reason_cn() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC: return "程序崩溃（panic）";
    case ESP_RST_INT_WDT: return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT: return "看门狗超时";
    case ESP_RST_BROWNOUT: return "欠压复位";
    default: return nullptr;
  }
}

// ============================================================
// 心跳任务：10s 一拍
// ============================================================
static void life_task(void*) {
  ESP_LOGI(TAG, "意识心跳启动（10s 拍，纯本地+云注入）");
  uint32_t lastTickMs = (uint32_t)(esp_timer_get_time() / 1000LL);
  uint32_t lastIdleMs = 0;
  bool idledOnce = false;
  for (;;) {
    vTaskDelay(BEAT);
    uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000LL);
    float dtMin = (nowMs - lastTickMs) / 60000.0f;
    lastTickMs = nowMs;
    // ---- PSI 内核 ----
    mind.tick(dtMin);
    int rssi = wifi_rssi();
    mind.sense(0, rssi);
    mind.senseBody(0, rssi, nowMs, dtMin);
    mind.trust += (0.60f - mind.trust) * 0.01f * dtMin;
    // ---- 对话记账消化 ----
    drain_notes();
    // ---- 预期结算（R3 预测-误差回环）----
    uint8_t ec = mind.expectation();
    if (ec != Cognition::EXP_NONE) {
      bool ownerCame =
          mind.lastUserMs != 0 && (int32_t)(mind.lastUserMs - mind.expectAtMs()) >= 0;
      if (ec == Cognition::EXP_OWNER_COME && ownerCame) {
        mind.expectOutcome(true);
        s_sawUserSinceExp = false;
      } else if (nowMs - mind.expectAtMs() > 600000UL) {
        bool fulfilled;
        if (ec == Cognition::EXP_OWNER_COME) fulfilled = ownerCame;
        else if (ec == Cognition::EXP_OWNER_AWAY) fulfilled = !ownerCame;
        else if (ec == Cognition::EXP_WORLD_ACTIVE) fulfilled = s_sawUserSinceExp;
        else fulfilled = !s_sawUserSinceExp;
        mind.expectOutcome(fulfilled);
        s_sawUserSinceExp = false;
      }
    }
    // ---- 表达：dominance 过阈值（经云出声）----
    if (mind.dominance() >= 0.55f) try_express(false, "tick");
    // ---- 独白：静默期后按间隔，心里有驱动才开口 ----
    if (IDLE_EVERY_MIN > 0 && !host_busy() && !in_quiet_window()) {
      uint32_t idleGap = (idledOnce ? IDLE_EVERY_MIN : IDLE_SILENCE_MIN) * 60000UL;
      bool needDriven = mind.intentCount() > 0 ||
                        mind.needs().curiosity >= 0.55f ||
                        mind.needs().expression >= 0.55f;
      if (needDriven && nowMs - lastIdleMs > idleGap) {
        if (try_monologue(false)) {
          idledOnce = true;
          lastIdleMs = nowMs;
        } else {
          lastIdleMs = nowMs - idleGap + 30000UL;   // 失败 30s 后重试
        }
      }
    }
    mind.saveEvolution();
    // ---- MCP 状态快照刷新 ----
    {
      std::string snap = mind.worldJson();
      if (!snap.empty() && snap.back() == '}') {
        snap.pop_back();
        char extra[64];
        snprintf(extra, sizeof(extra), ",\"memory_count\":%u}",
                 (unsigned)memory.count());
        snap += extra;
      }
      std::lock_guard<std::mutex> g(s_snapMux);
      s_statusSnap = snap;
    }
  }
}

// ============================================================
// 启动：黑匣子 + SNTP + 心跳任务（boot 成功路径末调用）
// ============================================================
void life_start() {
  if (const char* why = rst_reason_cn()) {
    ESP_LOGW(TAG, "[黑匣子] 上次重启原因: %s", why);
    if (memory.count() > 0)
      memory.logEvent("event",
                      std::string("[黑匣子] 刚才发生了一次异常重启（") + why +
                          "）。我可能丢了刚才说到一半的话。");
  }
  setenv("TZ", "CST-8", 1);
  tzset();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "ntp.aliyuncs.com");
  esp_sntp_setservername(1, "pool.ntp.org");
  esp_sntp_init();
  if (xTaskCreate(life_task, "laap_life", 8192, nullptr, 2, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "心跳任务创建失败（意识停摆，宿主对话不受影响）");
  }
}

}  // namespace laap
