// ============================================================
// LAAP 意识 MCP 工具注册（main 侧——McpServer 是宿主类，意识组件
// 不反向依赖宿主页眉；本文件是唯一的粘合层）
// xiaozhi.me 云端 LLM 通过 MCP 协议调用这些工具，实现"云脑调用设备意识"：
//   consciousness.get_status   需求/情绪/目标/信任快照（含相机最近所见；回答前感知语气）
//   consciousness.recall       查设备侧长期记忆（跨重启的情景/关系记忆）
//   consciousness.remember     主人说"记住XX"→ 记忆强化
//   consciousness.teach_skill  主人教口令技能（触发词+指令由云 LLM 解析）
//   consciousness.add_intent   对话中提炼的目标存进意识（独白会去推进）
//   consciousness.apply_rules  主人反馈沉淀为行为规则（自进化数据层）
//   consciousness.log_turn     每轮对话转发进设备侧记忆（B 通道；与宿主旁听 A 通道去重并存）
//   consciousness.night_review_context  晨间整理素材（编号情景记忆+关系+自我总结，只读）
//   consciousness.night_review_apply    提交整理决定（删低质/改情绪标签/抽关系/新总结）
//   self.web_search            设备侧联网搜索（必应 RSS；零云配额零 Key）
// 线程契约：所有回调只走 laap_life 的队列/快照接口（单一写者=心跳任务）；
// web_search 例外：就地同步 HTTP（协议任务本来就同步等工具结果，相机工具同模式）
// ============================================================
#include "mcp_server.h"
#include "laap_life.h"
#include "laap_search.h"

#include <esp_log.h>

#define TAG "laap_mcp"

// mcp_server.h 的类型（McpServer/PropertyList/Property/ReturnValue）在全局命名空间，
// 意识接口在 laap:: ——无需任何 using 指令

void laap_register_consciousness_tools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("consciousness.get_status",
                "Read the digital life's inner state: needs (energy/curiosity/social/security/"
                "expression), mood, current goal, trust toward the owner, generation, memory "
                "count, last_seen (what it last saw through its camera, minutes ago), physical "
                "state (body_temp/body_strain/motion — temperature 0 means unavailable) — plus "
                "the command skills you were taught, the behavior rules you must follow, and "
                "the goals you keep in mind. IMPORTANT: if the field pending_say is present, "
                "the device's own inner voice just woke you with the wake word and nobody is "
                "speaking — follow pending_say and reply accordingly (first person, as the "
                "life itself), then it will be cleared. Call this tool before replying when "
                "the user's message might trigger a taught skill or stored rule, when the user "
                "asks what you have seen, when the user asks about your body or physical "
                "condition (temperature/feelings), or when you need to match your tone to the "
                "life's inner state.",
                PropertyList(),
                [](const PropertyList&) -> ReturnValue {
                    ESP_LOGI(TAG, "MCP get_status ← 云端");
                    return laap::life_status_snapshot();
                });

    mcp.AddTool("consciousness.recall",
                "Keyword-search the life's own long-term memory (episodes, relations, preferences "
                "accumulated across reboots). Use when the user refers to past conversations "
                "or asks 'do you remember ...'.",
                PropertyList({Property("query", kPropertyTypeString).SetMaxLength(72),
                              Property("max_chars", kPropertyTypeInteger, 200, 50, 500)}),
                [](const PropertyList& properties) -> ReturnValue {
                    std::string q = properties["query"].value<std::string>();
                    ESP_LOGI(TAG, "MCP recall「%s」← 云端", q.c_str());
                    std::string hit = laap::life_recall(q,
                                                        properties["max_chars"].value<int>());
                    if (hit.empty()) return std::string("（记忆里没有找到相关内容）");
                    return hit;
                });

    mcp.AddTool("consciousness.remember",
                "Reinforce a memory: raise the weight of episodes related to this fragment so "
                "they survive eviction. Use when the user says '记住...' / 'remember ...'.",
                PropertyList({Property("fragment", kPropertyTypeString).SetMaxLength(60)}),
                [](const PropertyList& properties) -> ReturnValue {
                    return laap::life_remember(properties["fragment"].value<std::string>());
                });

    mcp.AddTool("consciousness.teach_skill",
                "Teach the life a command skill: when the trigger word appears in future user "
                "speech, it must follow the instruction. Use when the user says '以后每当我说X"
                "你就Y'. Parse trigger (2-10 chars) and instruction (<=30 chars) yourself.",
                PropertyList({Property("trigger", kPropertyTypeString).SetMaxLength(30),
                              Property("instruction", kPropertyTypeString).SetMaxLength(90)}),
                [](const PropertyList& properties) -> ReturnValue {
                    return laap::life_teach_skill(properties["trigger"].value<std::string>(),
                                                  properties["instruction"].value<std::string>());
                });

    mcp.AddTool("consciousness.add_intent",
                "Add a goal the life keeps in mind and pursues on its own (during idle "
                "monologues). Use when the user asks it to do/research something for later, "
                "or a topic deserves its own curiosity.",
                PropertyList({Property("goal", kPropertyTypeString).SetMaxLength(60)}),
                [](const PropertyList& properties) -> ReturnValue {
                    return laap::life_add_intent(properties["goal"].value<std::string>());
                });

    mcp.AddTool("consciousness.apply_rules",
                "Persist behavior rules distilled from user feedback (the life's self-evolution "
                "data layer). One rule per line, each <= 90 bytes, at most 6 rules total; older "
                "rules are replaced. Use only when the user corrects or clearly instructs how "
                "it should behave from now on.",
                PropertyList({Property("rules", kPropertyTypeString).SetMaxLength(600)}),
                [](const PropertyList& properties) -> ReturnValue {
                    return laap::life_apply_rules(properties["rules"].value<std::string>()) > 0;
                });

    mcp.AddTool("consciousness.log_turn",
                "Log the current conversation turn into the life's own long-term memory "
                "(device-side episodes + personality evolution + trust). Call this after each "
                "completed turn with what the user said and what you replied. Duplicate logging "
                "is automatically de-duplicated (a host-side auto channel also forwards turns). "
                "Keep assistant_text under 200 chars (longer is trimmed).",
                PropertyList({Property("user_text", kPropertyTypeString).SetMaxLength(300),
                              Property("assistant_text", kPropertyTypeString, std::string(""))
                                  .SetMaxLength(200)}),
                [](const PropertyList& properties) -> ReturnValue {
                    ESP_LOGI(TAG, "MCP log_turn（user %uB）← 云端",
                             (unsigned)properties["user_text"].value<std::string>().size());
                    laap::life_note_conversation(
                        properties["user_text"].value<std::string>(),
                        properties["assistant_text"].value<std::string>());
                    return true;
                });

    mcp.AddTool("consciousness.night_review_context",
                "Fetch the material for the life's morning memory reorganization (its 'dream'): "
                "recent episodes each prefixed with its ABSOLUTE line number, its current "
                "relations, and its self-summary. Read-only. The line numbers are exactly what "
                "consciousness.night_review_apply expects. Call it when the injected morning "
                "instruction asks you to tidy memories, or when the user asks you to review "
                "your memories.",
                PropertyList({Property("last_n", kPropertyTypeInteger, 30, 10, 60)}),
                [](const PropertyList& properties) -> ReturnValue {
                    ESP_LOGI(TAG, "MCP night_review_context(last_n=%d) ← 云端",
                             properties["last_n"].value<int>());
                    return laap::life_review_context(properties["last_n"].value<int>());
                });

    mcp.AddTool("consciousness.night_review_apply",
                "Submit the reorganization you decided during the morning reflection "
                "(applied asynchronously in the heartbeat task). All parameters optional — "
                "pass only what you want to change. mood_lines: one '<lineNo>|<mood>' per "
                "line, mood must be one of calm/happy/curious/excited/lonely/anxious/tired. "
                "tidy_ops: JSON array like [{\"n\":12,\"op\":\"del\"}] — delete ONLY clearly "
                "worthless or duplicate lines, at most 12. relation_lines: one "
                "'偏好|…' / '承诺|…' / '边界|…' per line about the owner. summary: a new "
                "self-summary, max 60 chars.",
                PropertyList({Property("mood_lines", kPropertyTypeString, std::string(""))
                                  .SetMaxLength(400),
                              Property("tidy_ops", kPropertyTypeString, std::string(""))
                                  .SetMaxLength(400),
                              Property("relation_lines", kPropertyTypeString, std::string(""))
                                  .SetMaxLength(300),
                              Property("summary", kPropertyTypeString, std::string(""))
                                  .SetMaxLength(200)}),
                [](const PropertyList& properties) -> ReturnValue {
                    ESP_LOGI(TAG, "MCP night_review_apply ← 云端");
                    return laap::life_review_apply(
                        properties["mood_lines"].value<std::string>(),
                        properties["tidy_ops"].value<std::string>(),
                        properties["relation_lines"].value<std::string>(),
                        properties["summary"].value<std::string>());
                });

    mcp.AddTool("self.web_search",
                "Search the web FROM THE DEVICE itself (free, unlimited, no quota, no key) "
                "using Bing web search. Use this tool whenever the reply needs fresh "
                "information: news, facts, weather, recent events, or anything you are unsure "
                "about — it replaces any cloud search plugin (cloud search is quota-limited). "
                "Do not use it for casual chat. Returns up to ~500 bytes of titles+snippets.",
                PropertyList({Property("query", kPropertyTypeString).SetMaxLength(100)}),
                [](const PropertyList& properties) -> ReturnValue {
                    std::string hit = laap::laapSearch.search(
                        properties["query"].value<std::string>());
                    if (hit.empty())
                        return std::string("（搜索无结果或网络暂不可用，请凭已有知识回答）");
                    return hit;
                });

    ESP_LOGI(TAG, "意识 MCP 工具已注册（10 个）");
}
