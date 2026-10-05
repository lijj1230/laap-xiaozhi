#pragma once
// ============================================================
// LAAP 意识组件总入口（M2d 接线层）
//   laap_consciousness_init()：挂载 conscious 分区 + 认知/记忆/技能/规则初始化。
//   由 main.cc 的 app_main 在 Application 启动前调用（M5 的 laap_life 心跳任务
//   也从这里后续启动；一期先只做持久层自检初始化）。
// ============================================================
namespace laap {

// 返回 true = 全部初始化成功（失败不阻塞宿主启动，意识功能降级）
bool consciousness_init();

// 意识自检一行（启动横幅/诊断用）
const char* consciousness_status_line();

}  // namespace laap
