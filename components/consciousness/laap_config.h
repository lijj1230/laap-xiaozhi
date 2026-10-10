#pragma once
// ============================================================
// LAAP 配置（NVS）。MCP-first 精简：AI 客户端（LLM/搜索/embed）全归
// xiaozhi 云，设备侧不再存任何 API Key；此结构保留作意识节奏等
// 本地键的落点（M6 实机标定后把节奏常量迁进来）。
// ============================================================
#include <cstdint>
#include <string>

namespace laap {

struct Config {
  // 预留：表达冷却/独白间隔/静音窗（当前为编译期常量，M6 标定后迁入）
  bool load();                       // NVS → 内存（缺省回退）
  bool save();                       // 内存 → NVS
};

extern Config cfg;

}  // namespace laap
