#pragma once
// ============================================================
// LAAP-lite 意识心跳（纯本地意识 + 云注入，MCP-first 架构）
//   独立 FreeRTOS 任务，10s 一拍：PSI tick / 对话记账消化 / 预期结算 /
//   表达链 / 独白链 / 黑匣子（重启原因）。无设备侧 AI 客户端——
//   说话经宿主 listen-detect 注入，由 xiaozhi 云 LLM+TTS 出声；
//   云端经 MCP 工具（main/laap_mcp_tools.cc）读取状态/查记忆/写技能。
//   线程模型：意识态只在心跳任务读写（单一写者）；
//   MCP 回调与宿主转发只入队/读快照。
// ============================================================
#include <string>

namespace laap {

using HostBusyFn = bool (*)();          // true = 宿主占着音频/对话（意识让路）
using HostSayFn = bool (*)(const std::string&);  // 注入说话；false=宿主拒收（未连接等）

void life_start();                               // boot 成功路径末调用：黑匣子+SNTP+任务
void life_set_host_busy(HostBusyFn fn);          // main 注入（DeviceState 忙闲探测）
void life_set_host_say(HostSayFn fn);            // main 注入（SendWakeWordDetected 适配器）

// ---- MCP 工具接口（McpServer 回调在协议任务上下文调用）----
void life_note_conversation(const std::string& userText, const std::string& assistantText);
std::string life_status_snapshot();              // 意识状态 JSON（mutex 快照，无撕裂）
std::string life_recall(const std::string& query, int maxChars);  // 关键词召回
bool life_remember(const std::string& fragment); // 记忆强化（权重 +0.5）
bool life_teach_skill(const std::string& trigger, const std::string& instruction);
int life_apply_rules(const std::string& rulesText);
bool life_add_intent(const std::string& text);

}  // namespace laap
