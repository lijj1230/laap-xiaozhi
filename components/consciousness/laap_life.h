#pragma once
// ============================================================
// LAAP-lite 意识心跳（M5，移植自 laap-esp32 主循环意识段）
//   独立 FreeRTOS 任务，10s 一拍：
//     PSI tick / 旁听记账消化 / 语义向量补齐 / 表达链 / 独白链 /
//     预期结算 / 4h 记忆压缩 / 每日夜间反思 / 黑匣子（重启原因）
//   线程模型（v3.6x String 撕裂血泪的架构解）：意识态只在心跳任务里
//   读写（单一写者）；旁听入口只入队（锁保护），由心跳统一落账。
//   全链堆门禁沿用 v3.76h 数值：表达 45KB（强制 30KB）、独白 45KB、
//   教技能提取 40KB——宿主音频管线与意识 TLS 并存，堆退化区禁火。
// ============================================================
#include <string>

namespace laap {

using HostBusyFn = bool (*)();   // true = 宿主占着音频/对话（意识让路，双声道互斥）

void life_start();                               // boot 成功路径末调用：黑匣子+SNTP+任务
void life_note_user(const std::string& text);    // 旁听入口（协议任务调用，只入队）
void life_note_assistant(const std::string& text);
void life_set_host_busy(HostBusyFn fn);          // main 注入（DeviceState 忙闲探测）

}  // namespace laap
