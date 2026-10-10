#pragma once
// ============================================================
// LAAP-lite 意识心跳（M5，移植自 laap-esp32 主循环意识段）
//   独立 FreeRTOS 任务，10s 一拍：
//     PSI tick / 对话记账消化 / 语义向量补齐 / 表达链 / 独白链 /
//     预期结算 / 4h 记忆压缩 / 每日夜间反思 / 黑匣子（重启原因）
//   线程模型：意识态只在心跳任务里读写（单一写者）。
//   对话记账与外部查询全部走队列/快照：
//     - MCP 回调（协议任务）只入队（锁保护）或读字串快照（mutex 拷贝）
//     - 主对话记账走 laap_note_conversation（云端宿主转发，MCP-first 方案）
// ============================================================
#include <string>

namespace laap {

using HostBusyFn = bool (*)();   // true = 宿主占着音频/对话（意识让路，双声道互斥）

void life_start();                               // boot 成功路径末调用：黑匣子+SNTP+任务
void life_set_host_busy(HostBusyFn fn);          // main 注入（DeviceState 忙闲探测）

// ---- MCP 工具接口（McpServer 回调在协议任务上下文调用）----
// 宿主把每轮对话转发进来（主人说的话+它的回复，两条各调一次或合并一条均可）
void life_note_conversation(const std::string& userText, const std::string& assistantText);
// 意识状态快照（JSON：需求/情绪/目标/信任/世代/记忆条数）——mutex 下拷贝，无撕裂
std::string life_status_snapshot();
// 记忆召回（可带网络 embedding；在协议任务里跑，网络锁与心跳互斥）
std::string life_recall(const std::string& query, int maxChars);
// 记忆强化（"记住XX"类指令：把片段相关记忆权重 +0.5）
bool life_remember(const std::string& fragment);
// 口令技能教学（宿主 LLM 已解析好 触发词|指令，直接落库）
bool life_teach_skill(const std::string& trigger, const std::string& instruction);
// 行为规则落盘（宿主 LLM 归纳产出，规则文本逐行）
int life_apply_rules(const std::string& rulesText);
// 新目标入栈（宿主 LLM 提炼的"心里惦记的事"）
bool life_add_intent(const std::string& text);

}  // namespace laap
