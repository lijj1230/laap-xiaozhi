#pragma once
// ============================================================
// LAAP-lite 公共工具（移植自 laap-esp32 laap_llm/laap_memory 内的公共函数）
// ============================================================
#include <string>

namespace laap {

// 严格 UTF-8 清洗（GBK 伪 UTF-8 / 过长编码 / 非字符码点整类拒收——
// 毒字节会被大模型 API 整包 400，v3.67 语义 1:1）
std::string sanitize_utf8(const std::string& s);

// JSON 字符串转义（含 <0x20 → \uXXXX 控制字符缺口，v3.55 语义）
std::string json_escape(const std::string& s);

// 二字组覆盖率：小片段（提问/目标）的大二字组有多少出现在大文本中（0..1）
// —— 跑题检测与关系召回的相关性判定（v3.53 语义）
float bigram_coverage(const std::string& small, const std::string& big);

// 二字组多数命中：small 的二字组 ≥30% 出现在 big 里（召回加分判据）
bool bigram_mostly_in(const std::string& small, const std::string& big);

}  // namespace laap
