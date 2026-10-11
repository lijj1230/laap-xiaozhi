#pragma once
// ============================================================
// LAAP 配置（NVS 持久化；缺键=默认值）。MCP-first：AI 客户端全归
// xiaozhi 云，设备侧零 API Key；本结构=意识节奏运行时调参落点
// （consciousness.tune MCP 工具写入，心跳每拍读取）。
// ============================================================
#include <cstdint>
#include <string>

namespace laap {

struct Config {
  // 节奏（M6 实机标定后由 tune 工具运行时可调）
  uint32_t expressCdMin = 5;      // 主动表达最小冷却（分钟；0=无冷却）
  uint32_t idleSilenceMin = 10;   // 独白起始静默（分钟）
  uint32_t idleEveryMin = 20;     // 独白间隔（分钟；0=关闭独白）
  int quietStart = 23;            // 静音窗起（含；0-23）
  int quietEnd = 6;               // 静音窗止（不含；0-23）
  uint32_t screenOffMin = 1;      // 闲置息屏（分钟；0=不息屏）
  bool load();                    // NVS → 内存（缺键回退默认；命名空间缺失=false）
  bool save();                    // 内存 → NVS
};

extern Config cfg;

}  // namespace laap