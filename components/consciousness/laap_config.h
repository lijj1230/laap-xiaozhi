#pragma once
// ============================================================
// LAAP 配置（移植自 laap-esp32 laap_config；NVS 键名 1:1 对齐，将来可从
// 旧设备导配置迁移）。一期只取意识链需要的键（llm/搜索/embed）；语音链
// 配置归小智自己的 Settings 体系，不重复。
// ============================================================
#include <cstdint>
#include <string>

namespace laap {

struct Config {
  // LLM（OpenAI 兼容）
  std::string llmBase = "https://api.deepseek.com";
  std::string llmKey = "";
  std::string llmModel = "deepseek-chat";
  int maxTokens = 500;
  int llmContinue = 2;
  // 语义向量（空=复用 ASR 通道 base/key；bge-m3）
  std::string embBase = "";
  std::string embKey = "";
  std::string embModel = "BAAI/bge-m3";
  // 搜索
  std::string searchKeys = "什么,怎么,如何,为什么,为啥,多少,几,哪,新闻,今天,最新,查,搜索";
  std::string searchApi = "";        // URL 模板（{q}=查询词），空=必应 RSS

  bool load();                       // NVS → 内存（缺省回退）
  bool save();                       // 内存 → NVS（逐键登记失败）
};

extern Config cfg;

}  // namespace laap
