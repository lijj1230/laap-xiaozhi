#pragma once
// ============================================================
// LAAP 设备侧 LLM（移植语义：OpenAI 兼容 chat/completions，SSE 流式）
//   一期用途：自主表达/独白/规则归纳/技能提取（主对话仍走小智云）。
//   简化项（相对 LAAP 固件）：keep-alive 槽位不做（esp_http_client 自带
//   连接管理）；续写 continue 一期不做（M5 按需补）。
// ============================================================
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace laap {

struct LlmMsg {
  const char* role;      // "system" | "user" | "assistant"
  std::string content;
};

struct LlmReply {
  bool ok = false;
  std::string say;
  std::string expr;
};

class LlmClient {
public:
  // 整段请求（maxTokens/temperature 直传）
  LlmReply chatMsgs(const LlmMsg* msgs, int count, int maxTokens, float temperature);
  // 流式：每凑出一个句子回调一次（返回 false 可中断）；返回完整文本
  LlmReply chatMsgsStream(const LlmMsg* msgs, int count, int maxTokens, float temperature,
                          std::function<bool(const std::string& sentence)> onSentence);
  // embeddings（bge-m3；返回二进制 float 数组文本形态，与 LAAP laapEmbed 语义一致）
  bool embed(const std::string& text, std::string& binOut);
  std::string lastError;

private:
  LlmReply do_chat(const LlmMsg* msgs, int count, int maxTokens, float temperature,
                   std::function<bool(const std::string&)> onSentence);
};

extern LlmClient llm;

// SSE 增量文本中切句（。！？\n，引号内不切——与 LAAP 流式播报同语义）
std::vector<std::string> split_sentences(const std::string& delta, std::string& carry);

}  // namespace laap
