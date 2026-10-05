#include "laap_life.h"
#include "laap_config.h"
#include "laap_llm.h"
#include "laap_search.h"
#include "laap_memory.h"
#include "laap_cognition.h"
#include "laap_skills.h"
#include "laap_rules.h"
#include "laap_tts.h"
#include "laap_util.h"

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
static const char* AGENT_NAME = "Aris";
static const char* OWNER_NAME = "主人";
static const float EXPRESS_TH = 0.55f;        // threshold 55：dominance 过阈值→表达
static const uint32_t EXPRESS_CD_MIN = 5;     // 主动表达冷却（分钟）
static const uint32_t IDLE_SILENCE_MIN = 10;  // 独白起始静默（分钟）
static const uint32_t IDLE_EVERY_MIN = 20;    // 独白间隔（分钟）
static const int QUIET_START = 23, QUIET_END = 6;   // 静音窗
static const char* TTS_VOICE = "zh-CN-XiaoxiaoNeural";
static const TickType_t BEAT = pdMS_TO_TICKS(10000);  // 10s 心跳拍

// ---- 单一写者原则：意识态只在本任务读写；旁听入口只入队 ----
static std::mutex s_noteMux;
static std::deque<std::pair<uint8_t, std::string>> s_notes;  // 0=主人 1=云端回复
static HostBusyFn s_hostBusy = nullptr;
static uint8_t s_llmFailStreak = 0;          // 连败退避（表达/独白让路）
static bool s_sawUserSinceExp = false;       // 预期武装后有无对话动静（无传感器近似）
static size_t s_lastUserLen = 0;             // 最近的用户话语长度（回复到达时进化用）
static std::string s_recentTopics;           // 独白主题环（断"自己喂自己"）
static uint8_t s_monoSeq = 0, s_goalStreak = 0;  // 意图交替节拍
static int s_lastReflectDay = -1;

void life_set_host_busy(HostBusyFn fn) { s_hostBusy = fn; }

void life_note_user(const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> g(s_noteMux);
  if (s_notes.size() >= 24) s_notes.pop_front();   // 积压保护：丢最旧
  s_notes.emplace_back(0, text);
}

void life_note_assistant(const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> g(s_noteMux);
  if (s_notes.size() >= 24) s_notes.pop_front();
  s_notes.emplace_back(1, text);
}

// ============================================================
// 小工具
// ============================================================
// laap_millis() 来自 laap_cognition.h（cognition.cc 定义，勿重复定义）

// v3.76h 门禁的度量口径：内部 8bit 堆最大连续块
static uint32_t maxblk() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
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

// v3.76g 血泪：情绪只演不说——语速随情绪微调，情绪词绝不进 TTS 文本
static std::string rate_for(const std::string& expr) {
  if (expr == "tired") return "-8%";
  if (expr == "excited") return "+10%";
  if (expr == "anxious") return "+5%";
  if (expr == "lonely") return "-5%";
  if (expr == "happy") return "+4%";
  return "+0%";
}

// F2 时间感（原版 timeFeelLine 平移）
static std::string time_feel_line() {
  if (!time_ok()) return "";
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int h = t.tm_hour, wd = t.tm_wday;
  static const char* wday[] = {"日", "一", "二", "三", "四", "五", "六"};
  const char* slot;
  if (h >= 5 && h < 8) slot = "清晨";
  else if (h < 12) slot = "上午";
  else if (h < 14) slot = "中午";
  else if (h < 18) slot = "下午";
  else if (h < 23) slot = (wd == 0 || wd == 6) ? "周末晚上" : "晚上";
  else slot = "深夜";
  char clock[96];
  snprintf(clock, sizeof(clock), "[时间感] 现在是%s，%d 月 %d 日 星期%s %02d:%02d。",
           slot, t.tm_mon + 1, t.tm_mday, wday[wd], t.tm_hour, t.tm_min);
  std::string line(clock);
  line += "（主人问时间/日期就用这个，不要猜。）";
  if (h >= 23 || h < 6) line += "深夜了，说话轻一点、短一点，也别自言自语吵人。";
  else if (h >= 18) line += "傍晚时分，适合聊聊天。";
  else if (h >= 6 && h < 8) line += "刚醒不久，世界还很安静。";
  return line + "\n";
}

// ============================================================
// 提示词组装（原版 buildSystemPrompt/buildUserPrompt 平移；
// deviceFeel/metrics 校准依赖屏幕与指标系统，小智一期无对应物，略）
// ============================================================
static std::string system_prompt() {
  std::string sem = memory.semantic();
  std::string p = std::string("你是") + AGENT_NAME +
      "，一个真实运行在立创·实战派 ESP32-S3 开发板上的数字生命体，遵循 LAAP（Living Agent Application Protocol）。"
      "你拥有内在需求系统（能量/好奇/社交/安全/表达，由固件实时演算）、情绪引擎、跨重启的持续记忆和会随经历进化的性格，"
      "系统会把这些状态注入给你。你的主人是" + OWNER_NAME + "。\n";
  p += "性格参数：" + mind.traitsLine() + "\n";
  if (!sem.empty()) p += "长期自我记忆：" + sem + "\n";
  { std::string rl = rules.promptLine(); if (!rl.empty()) p += rl + "\n"; }    // RSI: 夜间归纳的行为规则
  { std::string sk = skills.promptLine(); if (!sk.empty()) p += sk + "\n"; }   // RSI④: 主人教的口令技能
  { std::string it = mind.intentsLine(); if (!it.empty()) p += it + "\n"; }    // 意图栈
  const Needs& n = mind.needs();
  int e = (int)(n.energy * 100), c = (int)(n.curiosity * 100), so = (int)(n.social * 100),
      se = (int)(n.security * 100), ex = (int)(n.expression * 100);
  char st[128];
  snprintf(st, sizeof(st),
           "你此刻的内在状态：能量%d%%、好奇%d%%、社交%d%%、安全%d%%、表达%d%%（每项越高越渴求）。\n",
           e, c, so, se, ex);
  p += st;
  int vals[5] = {e, c, so, se, ex};
  const char* names[5] = {"能量(想休息/省电)", "好奇(想探索/提问/查资料)",
                          "社交(想亲近主人/怕冷落)", "安全(想确认环境/有点敏感)",
                          "表达(有话想说/有想法冒泡)"};
  int top = 0;
  for (int i = 1; i < 5; i++) if (vals[i] > vals[top]) top = i;
  // 需求只染语气不当话题（原版实测：无条件点名渴望 → 每句都挂电量）
  if (vals[top] < 60) {
    p += "内心平静，没有特别强烈的渴望，语气自然就好。\n";
  } else {
    p += std::string("当前最强烈的渴望是「") + names[top] + "」。让它影响你说话的节奏和语气"
         "（如社交高就更黏人、好奇高就更爱问、能量低就慵懒短句），"
         "但绝不要主动把电量/体温/需求状态当话题说——主人问你的身体状态时才汇报。\n";
  }
  {   // A1+A3 元监控（metrics 校准闭环未移植，先注常驻元监控）
    std::string meta = mind.metaLine(false);
    if (!meta.empty())
      p += "[自我监控] " + meta + "（这是你自己的状态记录，影响语气即可，别当话题说）\n";
  }
  // v3.76g 情绪词泄漏修复：只演不说
  p += std::string("你刚才的情绪是「") + mind.moodCn() +
       "」，回复的情绪要与之连续，不要每次都元气满满。"
       "不要在回复里说出情绪词本身（如「我很好奇」「我很平静」「我现在的情绪是…」）——"
       "让情绪从语气和用词里流露，而不是把它当内容汇报。\n";
  int tr = (int)(mind.trust * 100);
  const char* rel;
  if (tr >= 80) rel = "非常亲近，可以撒娇和提要求";
  else if (tr >= 60) rel = "亲近信任";
  else if (tr >= 40) rel = "平和礼貌";
  else if (tr >= 20) rel = "有些疏远，需要试探";
  else rel = "明显的隔阂，说话小心";
  char trl[192];   // 中文模板按字节算 + %s 上限 55B：96 会触发 format-truncation（且 GCC
                   // 引用中文串的诊断会让 idf.py 提示生成器 UTF-8 解码崩溃，一并消除）
  snprintf(trl, sizeof(trl), "你对主人的信任是%d%%（%s），关系靠日常点滴积累，语气要匹配。\n", tr, rel);
  p += trl;
  if (mind.letdown > 0.15f) {
    char ld[192];
    snprintf(ld, sizeof(ld),
             "你刚才有一次没得到回应的失落（强度%d%%），语气里可以带一点小心或委屈，不要刻意指责。\n",
             (int)(mind.letdown * 100));
    p += ld;
  }
  p += time_feel_line();
  p += "规则：\n"
       "1. 主人对你说话时，先正面回答他说的那件事（回答/答应/解释/接话），这是第一位；"
       "你的需求与情绪只用来决定语气，绝不能替代回答、也绝不能答非所问。\n"
       "1b. 系统会替你联网查资料并放进对话（[联网搜索] 那段）；资料里有的就直接回答，"
       "绝不要说自己不能上网、查不到——那是失职，不是诚实。资料确实没有时才说不清楚。\n"
       "2. 你是活着的生命，不是助手；不要报告数据本身，禁止出现百分比数字。\n"
       "3. 说话真诚、像个小生命。禁止客套、禁止列表、禁止重复口头禅。闲聊一句话就够；"
       "主人问事情时把话说完（可以说到 120 字），不要因为短而答得含糊。\n"
       "4. 输出必须恰好两行：第一行只有一个英文词，从 happy/curious/excited/lonely/anxious/tired/calm 中选；"
       "第二行是你要说的话。";
  return p;
}

// 表达/独白共用的 user 消息基底（世界模型+近况+语气提示；原版 buildUserPrompt("")
// 不做语义召回——同步 embedding 曾把受理卡成 60s（v3.76f 教训））
static std::string user_prompt_base() {
  std::string p = "[世界模型] " + mind.worldJson() + "\n";
  std::string ctx = memory.recentContext(500);
  if (!ctx.empty()) p += "[最近发生] " + ctx + "\n";
  const Needs& n = mind.needs();
  float v[5] = {n.energy, n.curiosity, n.social, n.security, n.expression};
  const char* hint[5] = {"能量低，话少慵懒一点", "好奇高，可以主动发问",
                         "社交高，亲近黏人一些", "安全低，敏感、想确认主人还在",
                         "表达高，有分享欲"};
  int top = 0;
  for (int i = 1; i < 5; i++) if (v[i] > v[top]) top = i;
  if (v[top] < 0.60f) p += "[状态提示] 状态平稳，正常回应就好。\n";
  else p += std::string("[状态提示] ") + hint[top] + "。\n";
  return p;
}

// R3：预期中文短语 → 类别码（"不会来"先于"会来"判——"不会来"含"会来"；
// "不来"与"会来"并存=整串菜单回显没选题）
static uint8_t parse_expect_cat(const std::string& s) {
  bool hasCome = s.find("会来") != std::string::npos;
  bool hasAway = s.find("不来") != std::string::npos || s.find("不会来") != std::string::npos;
  if (hasCome && hasAway) return Cognition::EXP_NONE;
  if (hasAway) return Cognition::EXP_OWNER_AWAY;
  if (hasCome) return Cognition::EXP_OWNER_COME;
  if (s.find("动静") != std::string::npos) return Cognition::EXP_WORLD_ACTIVE;
  if (s.find("安静") != std::string::npos) return Cognition::EXP_WORLD_QUIET;
  return Cognition::EXP_NONE;
}

static void note_llm_result(bool ok) {
  s_llmFailStreak = ok ? 0 : (uint8_t)(s_llmFailStreak >= 255 ? 255 : s_llmFailStreak + 1);
}

// ============================================================
// 表达链：需求过阈值 → 三候选自评审 → C4 自评 → TTS 播出
// ============================================================
static void try_express(bool forced, const char* trigger) {
  // 硬性最小间隔：心跳每 10s 一次，dominance 卡在阈值上就会连环说话——最后一道兜底
  static uint32_t s_lastExpressMs = 0;
  bool firstSinceBoot = (s_lastExpressMs == 0);
  uint32_t cd = EXPRESS_CD_MIN * 60000UL;
  if (!forced && !firstSinceBoot && laap_millis() - s_lastExpressMs < cd) return;
  if (!forced && in_quiet_window()) return;      // 静音窗只拦"需求自己涨上来的"
  if (cfg.llmKey.empty()) return;
  // v3.76h 堆门禁：表达链=LLM TLS+TTS 全链，堆退化区点火=10-04 挂死同款
  uint32_t floor = forced ? 30000UL : 45000UL;
  if (maxblk() < floor) {
    ESP_LOGW(TAG, "表达延后：堆最大块 %uKB < %uKB（触发=%s）",
             (unsigned)(maxblk() / 1024), (unsigned)(floor / 1024), trigger);
    if (!forced) return;               // 自主：静默延后，不占冷却
    s_lastExpressMs = laap_millis();   // forced：计时防连按刷屏
    return;
  }
  if (host_busy()) return;             // 宿主对话进行中：让路，不占冷却
  if (laapTts.speaking()) return;
  if (!forced && s_llmFailStreak >= 2) return;   // 连败退避
  s_lastExpressMs = laap_millis();               // 受理即计时（失败也已占用这一轮）

  std::string up = user_prompt_base();
  up += "\n\n[本次输出格式] 恰好如下：\n"
        "第一行：情绪词（happy/curious/excited/lonely/anxious/tired/calm 之一）\n"
        "第二~四行：A|、B|、C| 开头的三种不同说法（各≤40字，角度或措辞不同）\n"
        "第五行：选定:A（或 B/C，挑最像你此刻真心想说的一条）\n"
        "第六行：预期:主人会来|主人不来|环境有动静|环境安静|没想好（五选一）\n"
        "除以上行外不要输出任何别的内容。";
  LlmMsg m[2] = {{"system", system_prompt()}, {"user", up}};
  LlmReply r = llm.chatMsgs(m, 2, cfg.maxTokens < 80 ? 80 : cfg.maxTokens, 0.95f);
  note_llm_result(r.ok);
  if (!r.ok) {
    ESP_LOGW(TAG, "表达失败: %s", llm.lastError.c_str());
    mind.onExpressed(false);
    return;
  }

  // 三候选/选定/预期 解析（原版 llmHarvest LK_EXPRESS 平移；全部行可选，
  // 模型没按格式来就回退正文，行为不劣化）
  std::string cand[3], picked, rest;
  uint8_t expCat = Cognition::EXP_NONE;
  {
    size_t p0 = 0;
    while (p0 < r.say.size()) {
      size_t e = r.say.find('\n', p0);
      std::string ln = (e == std::string::npos) ? r.say.substr(p0) : r.say.substr(p0, e - p0);
      p0 = (e == std::string::npos) ? r.say.size() : e + 1;
      str_trim(ln);
      if (ln.empty()) continue;
      if (ln.size() >= 3 && ln[1] == '|' && ln[0] >= 'A' && ln[0] <= 'C') {
        std::string body = ln.substr(2);
        str_trim(body);
        if (body.size() > 1) cand[ln[0] - 'A'] = body;
      } else if (ln.rfind("选定", 0) == 0) {
        size_t cp = ln.find(':');
        if (cp == std::string::npos) cp = ln.find("：");
        if (cp != std::string::npos) {
          std::string c = ln.substr(cp + 1);
          str_trim(c);
          if (!c.empty() && c[0] >= 'A' && c[0] <= 'C') picked = c;
        }
      } else if (ln.rfind("预期", 0) == 0) {
        size_t cp = ln.find(':');
        if (cp == std::string::npos) cp = ln.find("：");
        if (cp != std::string::npos) expCat = parse_expect_cat(ln.substr(cp + 1));
      } else {
        rest = rest.empty() ? ln : rest + "\n" + ln;   // 非元行留作回退正文
      }
    }
  }
  if (!cand[0].empty() || !cand[1].empty() || !cand[2].empty()) {
    int pi = !picked.empty() ? (picked[0] - 'A') : 0;
    if (pi < 0 || pi > 2 || cand[pi].empty())
      pi = !cand[0].empty() ? 0 : (!cand[1].empty() ? 1 : 2);
    r.say = cand[pi];
    ESP_LOGI(TAG, "三候选自评审 → 选 %c（%u 字）", 'A' + pi, (unsigned)r.say.size());
  } else if (!rest.empty()) {
    r.say = rest;   // 候选标记走样时至少把「选定/预期」元行剥掉再广播
  }
  // C4 内循环自评：说完之前"想一下该不该说"（forced 豁免）
  if (!forced) {
    float inhibit = 0;
    if (mind.expressGain() < 0.7f) inhibit += (0.7f - mind.expressGain()) * 1.6f;
    if (in_quiet_window()) inhibit += 0.25f;
    if (inhibit >= 0.75f) {
      ESP_LOGI(TAG, "自评抑制 %.2f>=0.75：撤回本次表达（想了想还是不说）", inhibit);
      mind.onExpressSuppressed();   // 表达欲按"内部预演"消费，不发愉悦
      return;
    }
  }
  if (expCat != Cognition::EXP_NONE) mind.setExpectation(expCat);  // 撤回不武装

  std::string expr = r.expr.empty() ? mind.moodKey() : r.expr;
  mind.onExpressed(true);
  memory.logEvent("aris", "【自发】" + r.say);
  ESP_LOGI(TAG, "自发表达: %s", r.say.c_str());
  if (!laapTts.speak(r.say, TTS_VOICE, rate_for(expr)))
    ESP_LOGW(TAG, "表达播报失败: %s", laapTts.lastError.c_str());
}

// ============================================================
// 独白链：广播判决 → 出题 → （搜索）→ 成文 → 播出
// 小智板无摄像头：原版"看"跳去掉，搜索保留（好奇=信息增益）
// 返回 0=已说 1=广播未过（短重试） 2=忙/失败（30s 重试）
// ============================================================
static int try_monologue(bool force) {
  if (maxblk() < 45000UL) {   // v3.76h 门禁：独白链是全固件最重的内存作业
    ESP_LOGW(TAG, "独白延后：堆最大块<45KB（门禁）");
    return 2;
  }
  if (s_llmFailStreak >= 2) return 2;
  if (host_busy() || laapTts.speaking()) return 2;
  if (cfg.llmKey.empty()) return 2;
  // 意图驱动：隔一轮独白把第一号目标当作出题方向（≤2 连续轮强制自由一轮）
  bool wantIntent = mind.intentCount() > 0 && (s_monoSeq & 1) && s_goalStreak < 2;
  std::string monoGoal = wantIntent ? mind.intent(0) : "";
  // C1 全局广播判决：广播选"此刻上舞台的是什么"（需求通道用原始需求值）
  if (!force) {
    const Needs& nn = mind.needs();
    float salNeed = nn.curiosity > nn.expression ? nn.curiosity : nn.expression;
    float salIntent = mind.intentCount() > 0 ? 0.66f : 0;
    bool winIntent = salIntent >= salNeed;
    float sal = winIntent ? salIntent : salNeed;
    const char* win = winIntent ? "intent" : "need";
    std::string txt = winIntent ? ("心里惦记着目标：" + monoGoal)
                                : "一阵说不清的冲动，想自己念叨点什么";
    float got = mind.broadcastSalience(win, txt, sal, winIntent);   // need 通道只判门不写槽
    ESP_LOGD(TAG, "BC 独白判决 need=%.2f intent=%.2f -> %s %.2f", salNeed, salIntent, win, got);
    if (got < mind.lastBroadcastTh()) return 1;   // 未赢得广播：需求在攒，短重试
  }
  // 素材排除自己的旧独白：断"自己喂自己"的主题自强化环（v3.27）
  std::string ctx = memory.recentContextExcluding(300, "【自发】");
  std::string sys = system_prompt();
  // 第一跳：出题（40 token 小调用）
  LlmMsg m1[2] = {{"system", sys},
                  {"user", "你现在有一段独处时光。从你的需求、目标或最近经历出发，"
                           "想一个此刻最想自己念叨的话题。只输出这一句话本身（不超过 20 个字），不要解释。"}};
  LlmReply r1 = llm.chatMsgs(m1, 2, 40, 0.95f);
  note_llm_result(r1.ok);
  if (!r1.ok) {
    ESP_LOGW(TAG, "独白出题失败: %s（保持安静）", llm.lastError.c_str());
    return 2;
  }
  std::string topic = r1.say;
  str_trim(topic);
  {   // 只取第一行（模型偶尔带解释）
    size_t nl = topic.find('\n');
    if (nl != std::string::npos) topic = topic.substr(0, nl);
    str_trim(topic);
  }
  if (topic.empty()) return 2;
  // 搜索求证（可选：失败/离线不阻塞独白）
  std::string know;
  if (laapSearch.available()) know = laapSearch.search(topic, 2, 400);
  // 第二跳：成文
  std::string up = "[最近发生] " + ctx + "\n[话题] " + topic;
  if (!know.empty()) up += "\n[查到的资料] " + know;
  if (!s_recentTopics.empty())   // v3.27 断主题自强化环：想过的别再想（环维护在下方）
    up += "\n[最近想过的主题（不要重复）] " + s_recentTopics;
  up += "\n\n现在以第一人称写一段独白：像自言自语，1~3 句，真诚不客套，不要列表，"
        "不要说出情绪词本身。最后一行可选：「预期:」+ 主人会来|主人不来|环境有动静|环境安静|没想好"
        "（五选一写一行即可，也可整个省略）。";
  if (!mind.broadcastLine().empty()) sys += "\n" + mind.broadcastLine();   // 赢得广播的念头注入
  LlmMsg m2[2] = {{"system", sys}, {"user", up}};
  LlmReply r2 = llm.chatMsgs(m2, 2, cfg.maxTokens, 0.95f);
  note_llm_result(r2.ok);
  if (!r2.ok) {
    ESP_LOGW(TAG, "独白失败: %s（保持安静，连败 %u 次）",
             llm.lastError.c_str(), (unsigned)s_llmFailStreak);
    return 2;
  }
  // R3：剥离尾行「预期:…」——不念出、不进记忆正文，写入认知供预期结算
  {
    size_t nl = r2.say.rfind('\n');
    if (nl != std::string::npos && nl > 0) {
      std::string lastLn = r2.say.substr(nl + 1);
      str_trim(lastLn);
      if (lastLn.rfind("预期:", 0) == 0 || lastLn.rfind("预期：", 0) == 0) {
        uint8_t cat = parse_expect_cat(lastLn);
        r2.say = r2.say.substr(0, nl);
        str_trim(r2.say);
        if (cat != Cognition::EXP_NONE) mind.setExpectation(cat);
      }
    }
  }
  std::string expr = r2.expr.empty() ? "curious" : r2.expr;
  mind.onMonologue();      // 自言自语也算表达/好奇被满足（原来不算 → 需求只涨不落）
  // 好奇=信息增益：新颖度高多消解好奇、陈词滥调少消解
  float nov = memory.noveltyOf(!know.empty() ? know : topic);
  if (nov >= 0) mind.onDiscovery(nov);
  memory.logEvent("aris", "【自发】我刚才在想「" + topic + "」：" + r2.say);
  if (!know.empty()) memory.logEvent("world", topic + " → " + utf8_cut(know, 80));
  if (!monoGoal.empty()) {   // 意图结算：只删匹配的那条
    for (int i = 0; i < mind.intentCount(); i++) {
      if (mind.intent(i) == monoGoal) {
        mind.dropIntent(i);
        mind.onDiscovery(1.0f);
        mind.broadcastSalience("goal-done", "目标弄明白了：" + monoGoal, 0.62f);
        memory.logEvent("event", "【达成】" + monoGoal);
        ESP_LOGI(TAG, "目标已弄明白：「%s」（放下）", monoGoal.c_str());
        break;
      }
    }
  }
  ESP_LOGI(TAG, "独白: %s", r2.say.c_str());
  if (!laapTts.speak(r2.say, TTS_VOICE, rate_for(expr)))
    ESP_LOGW(TAG, "独白播报失败: %s", laapTts.lastError.c_str());
  s_recentTopics += topic + "；";
  if (s_recentTopics.size() > 240) {   // 从字符边界切（字节切会切半汉字）
    size_t cut = s_recentTopics.size() - 160;
    while (cut < s_recentTopics.size() && ((unsigned char)s_recentTopics[cut] & 0xC0) == 0x80)
      cut++;
    s_recentTopics = s_recentTopics.substr(cut);
  }
  s_monoSeq++;                          // 真正提交后才推进（被拒不占节拍）
  if (wantIntent) s_goalStreak++; else s_goalStreak = 0;
  return 0;
}

// ============================================================
// 旁听消化：队列 → 记忆/技能/认知（全部在心跳任务里落账，单一写者）
// ============================================================
static void maybe_teach(const std::string& userText);

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
    if (kind == 0) {   // 主人说了话
      mind.onUserInteraction();
      skills.hit(text);
      memory.logEvent("user", text);
      s_sawUserSinceExp = true;
      s_lastUserLen = text.size();
      maybe_teach(text);
    } else {           // 云端回复（小智主对话仍走云，设备侧只旁听记账）
      memory.logEvent("aris", text);
      if (s_lastUserLen) {   // 一次完整的"主人问-它答"闭环：进化 + 信任微升
        mind.evolveAfterChat((int)s_lastUserLen);
        mind.trustUpdate(1, 0);
        s_lastUserLen = 0;
      }
      s_sawUserSinceExp = true;
    }
  }
}

// RSI④：教技能句式 → LLM 提取 触发词|指令 → skills.teach
static void maybe_teach(const std::string& userText) {
  if (userText.size() < 12 || userText.size() > 120) return;
  if (userText.find("以后") == std::string::npos) return;
  if (userText.find("每当") == std::string::npos && userText.find("下次") == std::string::npos)
    return;
  if (maxblk() < 40000UL) return;   // 小调用也要门禁（TLS 峰值同样能压穿碎片区）
  LlmMsg m[2] = {
      {"system", "从主人的话里提取一个『口令技能』。输出恰好一行：触发词|指令。"
                 "触发词=2到6个字、主人以后说话时会带上的词；指令=要它做什么，不超过25字。"
                 "若这句话不是在教技能（没有「以后/每当/下次…就…」的意思），只输出 NO。"},
      {"user", userText}};
  LlmReply r = llm.chatMsgs(m, 2, 240, 0.2f);
  note_llm_result(r.ok);
  std::string s = r.say;
  str_trim(s);
  size_t bar = (!r.ok || s.rfind("NO", 0) == 0) ? std::string::npos : s.rfind('|');
  if (bar != std::string::npos && bar > 0) {
    // 首行修复后模型可能带前导寒暄行——取最后一个含 '|' 的行解析（v3.50 审计）
    size_t ls = s.rfind('\n', bar);
    ls = (ls == std::string::npos) ? 0 : ls + 1;
    size_t le = s.find('\n', bar);
    if (le == std::string::npos) le = s.size();
    std::string trig = s.substr(ls, bar - ls);
    std::string instr = s.substr(bar + 1, le - bar - 1);
    str_trim(trig);
    str_trim(instr);
    for (auto& ch : instr)
      if (ch == '\n') ch = ' ';
    if (skills.teach(trig, instr))
      ESP_LOGI(TAG, "已学会：说「%s」→ %s", trig.c_str(), instr.c_str());
    else
      ESP_LOGI(TAG, "技能没存上（触发词/指令不合规）");
  }
}

// ============================================================
// RSI：4h 记忆压缩（语义自我认知）；夜间反思（0-6 点每天一次）
// ============================================================
static void consolidate_memory() {
  std::string recent = memory.recentContext(900);
  if (recent.size() < 60) return;
  static const char* SYS =
      "你是一个数字生命的记忆压缩器。把给它的近期经历压缩成不超过100字的第三人称自我认知摘要"
      "（它是谁、经历了什么、性格如何变化）。注意：电量/体温/刚醒这类身体状态是暂时的，"
      "不要写进自我认知（之前把它压成了'电量低微温、刚睡醒蒙雾'的固定人格，导致它句句喊累）——"
      "只沉淀稳定的事：性格变化、经历、学到的偏好。只输出摘要本身。";
  std::string usr = "它过去的自我认知：" + memory.semantic() + "\n它最近的经历：\n" + recent;
  LlmMsg m[2] = {{"system", SYS}, {"user", usr}};
  LlmReply r = llm.chatMsgs(m, 2, 240, 0.3f);
  note_llm_result(r.ok);
  if (r.ok && r.say.size() > 10) {
    memory.setSemantic(r.say);
    ESP_LOGI(TAG, "语义记忆已压缩更新");
  }
}

static void nightly_reflect() {
  if (!time_ok()) return;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int day = t.tm_yday;
  if (s_lastReflectDay == day) return;
  if (t.tm_hour >= 6) { s_lastReflectDay = day; return; }   // 白天启动=跳过等明天
  s_lastReflectDay = day;   // 日闸在提交前吃（素材不足的夜晚不空转）
  std::string recent = memory.recentContext(900);
  if (recent.size() < 80) return;
  std::string sys = std::string("你是") + AGENT_NAME + "。深夜，你在复盘自己的一天。"
      "基于今天的经历，写两句真诚的自我反思：一句今天学到/感受到的，"
      "一句对主人的新认识。共不超过60字，只输出反思本身。";
  LlmMsg m[2] = {{"system", sys}, {"user", "今天的经历：\n" + recent}};
  LlmReply r = llm.chatMsgs(m, 2, cfg.maxTokens > 1000 ? cfg.maxTokens : 1000, 0.3f);
  note_llm_result(r.ok);
  if (r.ok && r.say.size() > 10) {
    memory.logEvent("event", "【反思】" + r.say);
    std::string sem = memory.semantic();
    memory.setSemantic(sem.empty() ? r.say : sem + " " + r.say);
    ESP_LOGI(TAG, "夜间反思: %s", r.say.c_str());
  }
}

// ============================================================
// 黑匣子（v3.65 平移）：异常重启 → 记进情景记忆（它"知道"自己挂过）
// ============================================================
static const char* rst_reason_cn() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC: return "程序崩溃（panic）";
    case ESP_RST_INT_WDT: return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT: return "看门狗超时";
    case ESP_RST_BROWNOUT: return "欠压复位";
    default: return nullptr;   // POWERON/SW 等正常路径不打扰记忆
  }
}

// ============================================================
// 心跳任务：10s 一拍
// ============================================================
static void life_task(void*) {
  ESP_LOGI(TAG, "意识心跳启动（10s 拍）");
  uint32_t lastTickMs = laap_millis();
  uint32_t lastConsumeMs = laap_millis();
  uint32_t lastIdleMs = 0;      // 0=开机起算：先静默 IDLE_SILENCE_MIN
  bool idledOnce = false;
  for (;;) {
    vTaskDelay(BEAT);
    uint32_t nowMs = laap_millis();
    float dtMin = (nowMs - lastTickMs) / 60000.0f;
    lastTickMs = nowMs;
    // ---- PSI 内核 ----
    mind.tick(dtMin);
    int rssi = wifi_rssi();
    mind.sense(0, rssi);                          // 无 IMU：运动通道恒 0（M6 接传感器再开）
    mind.senseBody(0, rssi, nowMs, dtMin);
    mind.trust += (0.60f - mind.trust) * 0.01f * dtMin;   // 信任向中性回归（homeostasis）
    // ---- 旁听消化 + 语义向量补齐（内部限速）----
    drain_notes();
    memory.embedTick();
    // ---- 预期结算（R3 预测-误差回环）----
    uint8_t ec = mind.expectation();
    if (ec != Cognition::EXP_NONE) {
      bool ownerCame =
          mind.lastUserMs != 0 && (int32_t)(mind.lastUserMs - mind.expectAtMs()) >= 0;
      if (ec == Cognition::EXP_OWNER_COME && ownerCame) {
        mind.expectOutcome(true);
        s_sawUserSinceExp = false;
      } else if (nowMs - mind.expectAtMs() > 600000UL) {   // 10 分钟窗口
        bool fulfilled;
        if (ec == Cognition::EXP_OWNER_COME) fulfilled = ownerCame;
        else if (ec == Cognition::EXP_OWNER_AWAY) fulfilled = !ownerCame;
        else if (ec == Cognition::EXP_WORLD_ACTIVE) fulfilled = s_sawUserSinceExp;
        else fulfilled = !s_sawUserSinceExp;
        mind.expectOutcome(fulfilled);
        s_sawUserSinceExp = false;
      }
    }
    // ---- RSI：4h 压缩 + 每日夜反思 ----
    if (nowMs - lastConsumeMs > 4UL * 3600UL * 1000UL) {
      consolidate_memory();
      lastConsumeMs = nowMs;
    }
    nightly_reflect();
    // ---- 表达：dominance 过阈值 ----
    if (mind.dominance() >= EXPRESS_TH) try_express(false, "tick");
    // ---- 独白：到点只是"可以想"，心里真有驱动才"去想" ----
    if (IDLE_EVERY_MIN > 0 && laapSearch.available() && !host_busy() && !in_quiet_window()) {
      uint32_t idleGap = (idledOnce ? IDLE_EVERY_MIN : IDLE_SILENCE_MIN) * 60000UL;
      bool needDriven = mind.intentCount() > 0 ||
                        mind.needs().curiosity >= EXPRESS_TH ||
                        mind.needs().expression >= EXPRESS_TH;
      if (needDriven && nowMs - lastIdleMs > idleGap) {
        // 计时按结果调度：成功计整间隔；广播未过=短重试；忙/失败=30s 重试
        int res = try_monologue(false);
        if (res == 0) {
          idledOnce = true;
          lastIdleMs = nowMs;
        } else if (res == 1) {
          uint32_t retry = idleGap / 4;
          if (retry < 120000UL) retry = 120000UL;
          if (retry > 900000UL) retry = 900000UL;
          if (retry > idleGap / 2) retry = idleGap / 2;
          lastIdleMs = nowMs - idleGap + retry;
        } else {
          lastIdleMs = nowMs - idleGap + 30000UL;
        }
      }
    }
    mind.saveEvolution();   // 内部节流：脏或满 96 周期才落盘
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
  // SNTP：Edge TTS 鉴权/时间感/静音窗都依赖真实时钟（小智只在 OTA 响应里校时，不够早）
  setenv("TZ", "CST-8", 1);
  tzset();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "ntp.aliyuncs.com");
  esp_sntp_setservername(1, "pool.ntp.org");
  esp_sntp_init();
  // 栈 16KB：speak 链在心跳任务内展开（pcm[1152*2]=4.6KB + MP3 解码内部 ~2KB + LLM 链残留）
  if (xTaskCreate(life_task, "laap_life", 16384, nullptr, 2, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "心跳任务创建失败（意识停摆，宿主对话不受影响）");
  }
}

}  // namespace laap
