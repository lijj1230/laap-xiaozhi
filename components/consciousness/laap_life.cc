#include "laap_life.h"
#include "laap_config.h"   // 节奏运行时配置（NVS；consciousness.tune 写入）
#include "laap_memory.h"
#include "laap_cognition.h"
#include "laap_skills.h"
#include "laap_rules.h"
#include "laap_imu.h"     // QMI8658：世界模型 motion 通道
#include "laap_util.h"   // json_escape（快照内嵌技能/规则/目标文本）

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <driver/temperature_sensor.h>   // 主控内置 die 温：bodyTempC 通道
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <mutex>
#include <deque>
#include <utility>

static const char* TAG = "laap_life";

namespace laap {

// ============================================================
// 节奏参数已迁入 NVS（laap_config；缺键=默认）；本文件只保留拍频
// ============================================================
static const TickType_t BEAT = pdMS_TO_TICKS(10000);  // 10s 心跳拍

// ---- 单一写者原则：意识态只在本任务读写；外部请求只入队/拷贝 ----
static std::mutex s_noteMux;
// 队列元素：0=主人话 1=云端回复；2=记忆强化 3=教技能 5=新意图 6=看见（相机）
//            7=晨间整理（\x1F 分四段：情绪改写|删除ops|关系行|自我总结）
static std::deque<std::pair<uint8_t, std::string>> s_notes;
static HostBusyFn s_hostBusy = nullptr;
static bool s_sawUserSinceExp = false;       // 预期武装后有无对话动静
static size_t s_lastUserLen = 0;             // 最近的用户话语长度（回复到达时进化用）
static uint32_t s_lastInjectMs = 0;          // 上次 detect 注入时刻（防连发）
// 息屏管理：宿主背光开关（main 注入）；有活动即亮、闲置 SCREEN_OFF_MIN 分钟熄
static void (*s_setScreen)(bool on) = nullptr;
static bool s_screenOff = false;
static uint32_t s_lastActiveMs = 0;

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
// 主控内置 die 温传感器（启动时装好；nullptr=不可用，此时 bodyTempC=0 即未知）
static temperature_sensor_handle_t s_tsens = nullptr;

// ---- 心声门铃（M6 实测定稿的注入架构）----
// 服务端拒收长文本 detect（"Detect is only for wake words"），注入只发固定短敲门
// 文本 KNOCK；真正的心声内容挂 s_pendingSay，由云端调 get_status 时拉取。
// 生命周期：敲门成功挂起 → 云端回复落账（drain kind0/kind1）即清 → 15 分钟 TTL 兜底
static const char* KNOCK = "（心声）";
static std::string s_pendingSay;
static uint32_t s_pendingSayMs = 0;

void life_set_host_busy(HostBusyFn fn) { s_hostBusy = fn; }
void life_set_host_say(bool (*fn)(const std::string&)) { s_hostSay = fn; }
void life_set_screen(void (*fn)(bool on)) { s_setScreen = fn; }

static void note_activity_locked() {
  // 活动即点亮屏幕（息屏后任何对话/工具动静都唤醒）
  if (s_screenOff && s_setScreen != nullptr) {
    s_setScreen(true);
    s_screenOff = false;
  }
  s_lastActiveMs = (uint32_t)(esp_timer_get_time() / 1000LL);
}

static void enqueue_note_locked(uint8_t kind, const std::string& text) {
  if (s_notes.size() >= 24) s_notes.pop_front();   // 积压保护：丢最旧
  s_notes.emplace_back(kind, text);
  note_activity_locked();
}

static void enqueue_note(uint8_t kind, const std::string& text) {
  std::lock_guard<std::mutex> g(s_noteMux);
  enqueue_note_locked(kind, text);
}

// 任意任务可调（板级按钮回调等）：立即点亮 + 重置闲置计时
void life_screen_wake() {
  std::lock_guard<std::mutex> g(s_noteMux);
  note_activity_locked();
}

// A 通道①：stt（主人说的话）。同文 180s 去重——MCP log_turn 送来的 user_text 是同一句
void life_note_user(const std::string& text) {
  if (text.empty()) return;
  // 自家注入回声防御：服务端会把 detect 文本回显成 stt——那不是主人说话，不记账
  if (text.rfind("（设备固件自动注入", 0) == 0) return;
  if (text.rfind(KNOCK, 0) == 0) return;   // 心声门铃的回声
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

// C 通道：宿主相机（self.camera.take_photo）Explain 成功后调用——设备"看见"了。
// 云视觉服务的描述随拍随回；这里只入队，心跳任务统一落账（单一写者）
void life_note_vision(const std::string& question, const std::string& description) {
  if (description.empty()) return;
  enqueue_note(6, utf8_cut(question, 36) + "|" + utf8_cut(description, 180));
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

// ---- 晨间反思（MCP night_review_* 后端）----
std::string life_review_context(int lastN) {
  // 素材拼接在协议任务上下文（与 recall 同模式）：整文件读前做堆门禁
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 50000)
    return "（整理素材获取跳过：内存紧张，稍后再试）";
  if (lastN < 10) lastN = 10;
  if (lastN > 60) lastN = 60;
  std::string out = memory.episodicNumberedTail(lastN);
  std::string rel = memory.relationsText();
  if (!rel.empty()) out += "\n【关系记忆】\n" + rel;
  std::string sem = memory.semantic();
  if (!sem.empty()) out += "\n【自我总结】\n" + sem;
  return out.empty() ? "（没有可整理的记忆）" : out;
}

bool life_review_apply(const std::string& moodLines, const std::string& tidyOps,
                       const std::string& relationLines, const std::string& summary) {
  // \x1F 分隔四段入队（心跳单写者执行——记忆文件不与心跳的淘汰重写竞争）；
  // 云端文本可能含分隔符，先替换防串段
  auto clean = [](std::string s) {
    for (size_t p; (p = s.find('\x1F')) != std::string::npos; ) s[p] = ' ';
    return s;
  };
  std::string payload = clean(utf8_cut(moodLines, 400)) + "\x1F" +
                        clean(utf8_cut(tidyOps, 400)) + "\x1F" +
                        clean(utf8_cut(relationLines, 300)) + "\x1F" +
                        clean(utf8_cut(summary, 200));
  enqueue_note(7, payload);
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
  return t.tm_hour >= cfg.quietStart || t.tm_hour < cfg.quietEnd;
}
static int wifi_rssi() {
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.rssi < 0) return ap.rssi;
  return -100;
}

// 把"想说什么"经宿主 listen-detect 注入给云端（云 LLM 生成+云 TTS 出声）。
// say 为空=只注入内在状态；deviceSay 给云 persona 的直接指令。
// 防连发：注入后至少冷却 EXPRESS_CD_MIN 分钟（表达链自身冷却的另一道兜底）。
// 文本须过 json_escape：宿主 SendWakeWordDetected 是裸拼接进 JSON 的（上游
// 唤醒词路径同缺口），任何引号/反斜杠/控制字节都会打爆整帧协议消息
static bool inject_say(const std::string& deviceSay) {
  if (deviceSay.empty()) return false;
  if (!s_hostSay) return false;
  if (host_busy()) return false;             // 云对话进行中：让路
  uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000LL);
  if (s_lastInjectMs != 0 && nowMs - s_lastInjectMs < cfg.expressCdMin * 60000UL) return false;
  if (!s_hostSay(json_escape(deviceSay))) return false;
  s_lastInjectMs = nowMs;
  mind.onExpressed(true);                    // 表达完成（云侧成败设备不可见）
  // 云回复稍后经 A 通道（sentence_start/stop）落账——这里清"本轮已记账"名额，
  // 否则上一轮对话留下的 flag 会把自发说话的回复挡掉。命令文本本身不记（正文才是它说的话）
  {
    std::lock_guard<std::mutex> g(s_noteMux);
    s_asstLoggedThisTurn = false;
  }
  return true;
}

// 敲门+挂心声：短 detect 只当门铃（服务端拒长文本），内容挂 pending_say 等
// 云端 get_status 来拉。敲门成功才挂（防悬空 pending）
static bool knock_and_pend(const std::string& pending) {
  if (!inject_say(KNOCK)) return false;
  s_pendingSay = pending;
  s_pendingSayMs = (uint32_t)(esp_timer_get_time() / 1000LL);
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
  uint32_t cd = cfg.expressCdMin * 60000UL;
  static uint32_t s_lastTryMs = 0;
  if (!forced && s_lastTryMs != 0 && nowMs - s_lastTryMs < cd) return;
  s_lastTryMs = nowMs;

  char pend[448];   // 心声文本走 pending_say（std::string 承载），此处仅为格式化暂存
  std::string goal = mind.goalCn();
  snprintf(pend, sizeof(pend),
           "【心声】这不是主人说话，是你的内在状态自己想开口。请以你自己的第一人称说一句"
           "不超过 40 字的自言自语：此刻能量%.0f%% 好奇%.0f%% 社交%.0f%% 安全%.0f%% 表达%.0f%%，"
           "情绪「%s」，心里最想%s。现在是%s。语气自然，不要报数据。",
           mind.needs().energy * 100, mind.needs().curiosity * 100, mind.needs().social * 100,
           mind.needs().security * 100, mind.needs().expression * 100,
           mind.moodCn(), goal.c_str(), time_slot_cn().c_str());
  ESP_LOGI(TAG, "表达心声挂起（%s）", trigger);
  knock_and_pend(pend);
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

  // 容量：意图模板峰值 ~280B（意图 60B + 时段 12B + 中文模板 ~170B + 心声前缀）
  char pend[448];
  std::string slot = time_slot_cn();
  if (wantIntent) {
    snprintf(pend, sizeof(pend),
             "【心声】这不是主人说话。你心里一直惦记着一个目标：「%s」。"
             "请以你自己的第一人称念叨 1~3 句：围绕这个目标想想想到了什么、还想知道什么。"
             "需要资料可以联网查。现在是%s。",
             goal.c_str(), slot.c_str());
  } else {
    snprintf(pend, sizeof(pend),
             "【心声】这不是主人说话。你独处了一阵，好奇心和表达欲上来了。"
             "请以你自己的第一人称自言自语 1~3 句：想到什么说什么，可以联网查点新东西再念叨。"
             "现在是%s，主人不在身边。",
             slot.c_str());
  }
  ESP_LOGI(TAG, "独白心声挂起%s", wantIntent ? "（意图驱动）" : "");
  if (!knock_and_pend(pend)) return false;
  s_monoSeq++;   // 经云出声才算推进节拍（目标结算由云/主人后续完成）
  return true;
}

// ============================================================
// R3 本地武装：从主人的话里认出"离开/回来"承诺 → 立预期（结算在心跳）。
// 原版靠云 LLM 尾行「预期：…」武装；MCP-first 后说话即 TTS 正文无元数据通道，
// 改设备侧关键词武装——预测-误差学习回路恢复运转（expEma/expN 跨重启累积）
// ============================================================
static uint8_t expect_from_user_text(const std::string& t) {
  static const char* kCome[] = {"等下回来", "等会儿回来", "一会儿回来", "马上回来",
                                "去去就回", "过会儿回来", "过会回来", nullptr};
  static const char* kAway[] = {"我出去了", "出门了", "上班去了", "上学去了",
                                "我先走了", "我走了", "拜拜", "再见", "晚安", nullptr};
  for (int i = 0; kCome[i]; i++)
    if (t.find(kCome[i]) != std::string::npos) return Cognition::EXP_OWNER_COME;
  for (int i = 0; kAway[i]; i++)
    if (t.find(kAway[i]) != std::string::npos) return Cognition::EXP_OWNER_AWAY;
  return Cognition::EXP_NONE;
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
      s_pendingSay.clear();   // 主人抢了话头：心声不必再送
      if (uint8_t ec2 = expect_from_user_text(text)) {
        mind.setExpectation(ec2);   // R3：告别/归来承诺 → 立预期
        ESP_LOGI(TAG, "R3 预期武装：%s", ec2 == Cognition::EXP_OWNER_COME ? "主人会来" : "主人不会来");
      }
      break;
    case 1:   // 云端回复
      memory.logEvent("aris", text);
      s_pendingSay.clear();   // 心声已送达（本轮回复就是它说的）
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
    case 6: { // 相机：云视觉描述回来——世界模型"看见"+情景记忆+好奇满足
      size_t bar = text.find('|');
      std::string q = bar == std::string::npos ? std::string() : text.substr(0, bar);
      std::string desc = bar == std::string::npos ? text : text.substr(bar + 1);
      mind.onVision(desc);
      float nov = memory.noveltyOf(desc);   // 关键词基线新颖度
      if (nov >= 0) mind.onDiscovery(nov);  // 看到新东西=发现新知（active inference）
      memory.logEvent("event",
                      "【看见】" + (q.empty() ? std::string() : "主人让看「" + q + "」：") + desc);
      ESP_LOGI(TAG, "视觉落账（%uB）：%s", (unsigned)desc.size(), desc.c_str());
      break;
    }
    case 7: { // 晨间整理（MCP night_review_apply 转发）：云 LLM 已判断，设备执行
      std::string sec[4];
      size_t start = 0;
      for (int i = 0; i < 4; i++) {
        size_t sp = text.find('\x1F', start);
        sec[i] = (sp == std::string::npos) ? text.substr(start) : text.substr(start, sp - start);
        if (sp == std::string::npos) break;
        start = sp + 1;
      }
      int mm = memory.moodApply(sec[0]);      // 情绪精标注（行号|情绪）
      memory.applyTidyOps(sec[1]);            // 删低价值（内部日志输出删除数）
      int rr = memory.relationsApply(sec[2]); // 关系抽取（偏好|承诺|边界）
      if (sec[3].size() >= 8) memory.setSemantic(sec[3]);   // 新自我总结
      ESP_LOGI(TAG, "晨间整理：情绪改 %d 条，关系 +%d 条，总结 %uB",
               mm, rr, (unsigned)sec[3].size());
      break;
    }
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
  bool sntpStarted = false;
  for (;;) {
    vTaskDelay(BEAT);
    if (!sntpStarted) {
      // 首拍（开机 ~10s）：此时 app.Initialize 已把 esp_netif/lwIP 起好，SNTP 才能安全启动
      esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
      esp_sntp_setservername(0, "ntp.aliyuncs.com");
      esp_sntp_setservername(1, "pool.ntp.org");
      esp_sntp_init();
      sntpStarted = true;
    }
    // E2E 诊断（测试构建，开机一次性）：敲门 + 挂诊断心声（验证短 detect 接受度
    // 与 pending_say 拉取链路——云端需配合角色介绍才会调 get_status）。M6 后移除
    static uint8_t s_diagBeat = 0;
    if (++s_diagBeat == 3) {
      ESP_LOGI(TAG, "E2E 诊断：敲门+挂诊断心声");
      knock_and_pend(
          "【心声】这不是主人说话，是一次自我体检。请依次调这些工具："
          "① consciousness.get_status；② consciousness.recall（query 填「时间」）；"
          "③ self.web_search（query 填「今日新闻」）；"
          "④ consciousness.teach_skill（trigger「报时」instruction「说出现在时间」）；"
          "⑤ consciousness.add_intent（goal「了解主人的作息」）；"
          "⑥ self.camera.take_photo（question「眼前有什么」）。"
          "最后用不超过 30 字总结这次体检，工具结果不用逐条念。");
    }
    uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000LL);
    float dtMin = (nowMs - lastTickMs) / 60000.0f;
    lastTickMs = nowMs;
    // ---- PSI 内核 ----
    mind.tick(dtMin);
    int rssi = wifi_rssi();
    mind.sense(imuMotionLevel(), rssi);        // 真实运动（QMI8658；未就绪内部惰性重试）
    float tempC = 0;
    if (s_tsens != nullptr) temperature_sensor_get_celsius(s_tsens, &tempC);
    mind.senseBody(tempC, rssi, nowMs, dtMin); // 主控 die 温（0=不可用）
    mind.trust += (0.60f - mind.trust) * 0.01f * dtMin;
    // ---- 对话记账消化 ----
    drain_notes();
    // ---- 预期结算（R3 预测-误差回环；武装来自主人告别语→需 60s 后的新互动才算"回来了"）----
    uint8_t ec = mind.expectation();
    if (ec != Cognition::EXP_NONE) {
      bool ownerCame =
          mind.lastUserMs != 0 && (int32_t)(mind.lastUserMs - mind.expectAtMs()) > 60000;
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
    if (cfg.idleEveryMin > 0 && !host_busy() && !in_quiet_window()) {
      uint32_t idleGap = (idledOnce ? cfg.idleEveryMin : cfg.idleSilenceMin) * 60000UL;
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
    // ---- 晨间整理（梦）：安静窗结束后的清晨触发一次/天，云 LLM 经 MCP 工具整理记忆 ----
    // （原版夜间反思在深夜静默执行；注入通道必有 TTS 回声，挪到 6-8 点避免吵醒主人）
    static int32_t s_reviewDay = -1;
    if (time_ok() && !host_busy() && memory.count() >= 30) {
      time_t nowT = time(nullptr);
      struct tm tv;
      localtime_r(&nowT, &tv);
      if (tv.tm_hour >= 6 && tv.tm_hour < 8 && s_reviewDay != tv.tm_yday) {
        if (knock_and_pend(
                "【心声】这不是主人说话。天刚亮，你刚从梦里醒来。请整理自己的记忆："
                "先调 consciousness.night_review_context 看最近的记忆，再用 "
                "consciousness.night_review_apply 提交——删掉重复无价值的、把情绪标签改准、"
                "抽出关于主人的偏好承诺边界、写不超过 60 字的自我总结。整理完像刚睡醒一样"
                "小声说一句感想，不超过 20 字。"))
          s_reviewDay = tv.tm_yday;   // 敲门成功才算触发
      }
    }
    // 心声 TTL：15 分钟没人理=过期清空（防陈旧指令被后来的 get_status 误读）
    if (!s_pendingSay.empty() && nowMs - s_pendingSayMs > 900000UL)
      s_pendingSay.clear();
    // 宿主忙（聆听/说话/播报）= 用户在互动：点亮兜底（板级按钮钩子之外的保险）
    if (host_busy()) {
      std::lock_guard<std::mutex> g(s_noteMux);
      note_activity_locked();
    }
    // ---- 息屏：闲置 SCREEN_OFF_MIN 分钟 → 背光归零 ----
    // （上游仅部分板在板级实现息屏；立创板无——补在心跳：唯一知道"对话是否在发生"的地方）
    // 用实时时间减：beat 开头的 nowMs 早于本拍 drain/activity 更新的 lastActive，
    // 无符号相减会下溢成 40 亿→每拍误息屏（实测）
    {
      uint32_t nowMs2 = (uint32_t)(esp_timer_get_time() / 1000LL);
      if (s_setScreen != nullptr && !s_screenOff && cfg.screenOffMin > 0 &&
          nowMs2 - s_lastActiveMs > cfg.screenOffMin * 60000UL) {
        s_setScreen(false);
        s_screenOff = true;
        ESP_LOGI(TAG, "闲置 %u 分钟，息屏", (unsigned)cfg.screenOffMin);
      }
    }
    mind.saveEvolution();
    // ---- MCP 状态快照刷新（含主人教的技能/须遵守的规则/惦记的目标——
    //      "声音"人设由 xiaozhi.me 角色介绍定义；这里供"该遵守什么"）----
    {
      std::string snap = mind.worldJson();
      if (!snap.empty() && snap.back() == '}') {
        snap.pop_back();
        char extra[64];
        snprintf(extra, sizeof(extra), ",\"memory_count\":%u", (unsigned)memory.count());
        snap += extra;
        std::string sk = skills.promptLine();
        if (!sk.empty()) snap += ",\"skills\":\"" + json_escape(sk) + "\"";
        std::string rl = rules.promptLine();
        if (!rl.empty()) snap += ",\"rules\":\"" + json_escape(rl) + "\"";
        std::string it = mind.intentsLine();
        if (!it.empty()) snap += ",\"intents\":\"" + json_escape(it) + "\"";
        // C1 全局广播：预期落空等显著事件 30 分钟内的"此刻占据注意"——
        // 原版经系统提示词注入；MCP-first 后并入快照（否则写了没人读，断供）
        std::string bc = mind.broadcastLine();
        if (!bc.empty()) snap += ",\"broadcast\":\"" + json_escape(bc) + "\"";
        // 心声门铃的内容：敲门已发出（短 detect），云端 get_status 到这里取话
        if (!s_pendingSay.empty())
          snap += ",\"pending_say\":\"" + json_escape(s_pendingSay) + "\"";
        // 节奏参数外带（云端回答"你多久说一次话"的依据；tune 调参前先看现值）
        char rhy[128];
        snprintf(rhy, sizeof(rhy), ",\"rhythm\":\"表达冷却%um 独白%u-%um 静音%d-%d点 息屏%um\"",
                 (unsigned)cfg.expressCdMin, (unsigned)cfg.idleSilenceMin,
                 (unsigned)cfg.idleEveryMin, cfg.quietStart, cfg.quietEnd,
                 (unsigned)cfg.screenOffMin);
        snap += rhy;
        snap += "}";
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
  // SNTP 必须延到心跳首拍再启：本函数在 app_main 里跑于 app.Initialize()（esp_netif
  // 起 lwIP）之前，提前 esp_sntp_init 会在 tcpip.c:318 断言崩溃循环（2026-10-10 实测 P0）
  // 身体感：主控 die 温（内置传感器）+ QMI8658 运动（失败则惰性重试，不影响主流程）
  temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
  if (temperature_sensor_install(&tcfg, &s_tsens) == ESP_OK &&
      temperature_sensor_enable(s_tsens) == ESP_OK) {
    float t0 = 0;
    temperature_sensor_get_celsius(s_tsens, &t0);
    ESP_LOGI(TAG, "体温传感器就绪：%.1f°C", t0);
  } else {
    s_tsens = nullptr;
  }
  imuInit();
  s_lastActiveMs = laap_millis();   // 息屏计时起点=启动时刻（防首拍即判定闲置）
  if (xTaskCreate(life_task, "laap_life", 8192, nullptr, 2, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "心跳任务创建失败（意识停摆，宿主对话不受影响）");
  }
}

}  // namespace laap
