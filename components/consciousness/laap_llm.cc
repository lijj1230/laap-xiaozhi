#include "laap_llm.h"
#include "laap_config.h"
#include "laap_util.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <cJSON.h>
#include <cstring>
#include <vector>

static const char* TAG = "laap_llm";

namespace laap {

LlmClient llm;

static const int HTTP_TIMEOUT_MS = 30000;

// ---- 请求体构造（messages 数组；role 常量字符串直传） ----
static std::string build_body(const LlmMsg* msgs, int count, int maxTokens, float temperature, bool stream) {
  std::string body = "{\"model\":\"" + json_escape(cfg.llmModel) +
                     "\",\"messages\":[";
  for (int i = 0; i < count; i++) {
    if (i) body += ",";
    body += std::string("{\"role\":\"") + msgs[i].role + "\",\"content\":\"" + json_escape(msgs[i].content) + "\"}";
  }
  char tail[96];
  snprintf(tail, sizeof(tail), "],\"max_tokens\":%d,\"temperature\":%.2f%s}",
           maxTokens, temperature, stream ? ",\"stream\":true" : "");
  body += tail;
  return body;
}

// ---- 从 SSE data: 帧里抽增量文本；[DONE] 返回 false ----
static bool sse_extract(const std::string& line, std::string& deltaOut) {
  deltaOut.clear();
  if (line.rfind("data:", 0) != 0) return true;
  std::string payload = line.substr(5);
  while (!payload.empty() && payload.front() == ' ') payload.erase(payload.begin());
  if (payload.rfind("[DONE]", 0) == 0) return false;
  cJSON* root = cJSON_Parse(payload.c_str());
  if (!root) return true;
  cJSON* choices = cJSON_GetObjectItem(root, "choices");
  if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
    cJSON* delta = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "delta");
    if (delta) {
      cJSON* content = cJSON_GetObjectItem(delta, "content");
      if (cJSON_IsString(content) && content->valuestring) deltaOut = content->valuestring;
    }
  }
  cJSON_Delete(root);
  return true;
}

// ---- 切句：完整 UTF-8 分隔串匹配（原 find_first_of("。！？\n") 是字节集合——
//      '，'=EF BC 8C 与 '！'=EF BC 81 共享字节 EF/BC，会在逗号处切成半截汉字；
//      与 v3.76g laapLabelSepAt 同族教训） ----
static size_t sent_cut(const std::string& s) {
  size_t best = s.find('\n');
  auto cons = [&](const char* sep) {
    size_t p = s.find(sep);
    if (p != std::string::npos && (best == std::string::npos || p < best)) best = p;
  };
  cons("。");
  cons("！");
  cons("？");
  return best;
}

// ---- 响应体处理：stream=SSE 逐行；非 stream=整包 JSON ----
LlmReply LlmClient::do_chat(const LlmMsg* msgs, int count, int maxTokens, float temperature,
                            std::function<bool(const std::string&)> onSentence) {
  LlmReply r;
  if (cfg.llmKey.empty()) { lastError = "API Key 未配置"; return r; }
  bool stream = (onSentence != nullptr);
  std::string body = build_body(msgs, count, maxTokens, temperature, stream);

  std::string url = cfg.llmBase;
  if (url.find("/chat/completions") == std::string::npos)
    url += (url.back() == '/' ? "" : "/") + std::string("chat/completions");

  esp_http_client_config_t conf = {};
  conf.url = url.c_str();
  conf.method = HTTP_METHOD_POST;
  conf.timeout_ms = HTTP_TIMEOUT_MS;
  conf.buffer_size = 4096;
  conf.event_handler = nullptr;
  conf.keep_alive_enable = false;

  esp_http_client_handle_t hc = esp_http_client_init(&conf);
  if (!hc) { lastError = "http client init 失败"; return r; }
  esp_http_client_set_header(hc, "Content-Type", "application/json");
  std::string auth = "Bearer " + cfg.llmKey;
  esp_http_client_set_header(hc, "Authorization", auth.c_str());
  esp_http_client_set_header(hc, "Accept", stream ? "text/event-stream" : "application/json");
  esp_http_client_set_post_field(hc, body.c_str(), (int)body.length());

  std::string fullText;      // 非流式正文 / 流式累计
  std::string carry;         // 流式切句残留
  std::string lineBuf;       // SSE 行缓冲
  bool done = false;
  int httpCode = 0;

  esp_http_client_open(hc, (int)body.length());
  esp_http_client_fetch_headers(hc);
  httpCode = esp_http_client_get_status_code(hc);

  char buf[2048];
  int readLen;
  size_t bodyCap = 256 * 1024;   // 响应体上限：畸形/恶意大响应会撑爆堆（v3.76e 教训）
  while ((readLen = esp_http_client_read(hc, buf, sizeof(buf) - 1)) > 0 && !done) {
    buf[readLen] = 0;
    std::string chunk(buf, readLen);
    if (stream) {
      lineBuf += chunk;
      size_t nl;
      while ((nl = lineBuf.find('\n')) != std::string::npos) {
        std::string line = lineBuf.substr(0, nl);
        lineBuf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::string delta;
        if (!sse_extract(line, delta)) { done = true; break; }
        if (delta.empty()) continue;
        fullText += delta;
        carry += delta;
        // 切句（。！？\n 结尾即出句）
        size_t cut;
        while ((cut = sent_cut(carry)) != std::string::npos) {
          size_t seplen = ((unsigned char)carry[cut] >= 0xE0) ? 3 : 1;   // 全角标点 3 字节
          std::string sentence = carry.substr(0, cut + seplen);
          carry.erase(0, cut + seplen);
          if (sentence.size() >= 2 && onSentence && !onSentence(sentence)) { done = true; break; }
        }
        if (done) break;
      }
    } else {
      fullText += chunk;
    }
    if (fullText.size() > bodyCap) break;
  }
  esp_http_client_close(hc);
  esp_http_client_cleanup(hc);

  if (httpCode != 200) {
    lastError = "HTTP " + std::to_string(httpCode) + " " + fullText.substr(0, 120);
    ESP_LOGE(TAG, "%s", lastError.c_str());
    return r;
  }

  if (stream) {
    if (!carry.empty() && carry.size() >= 2) fullText += carry;   // 残句并入
    r.say = sanitize_utf8(fullText);
  } else {
    cJSON* root = cJSON_Parse(fullText.c_str());
    if (!root) { lastError = "响应非 JSON: " + fullText.substr(0, 100); return r; }
    cJSON* choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
      cJSON* msg = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message");
      cJSON* content = msg ? cJSON_GetObjectItem(msg, "content") : nullptr;
      if (cJSON_IsString(content) && content->valuestring) r.say = content->valuestring;
    }
    cJSON_Delete(root);
    r.say = sanitize_utf8(r.say);
  }
  r.ok = !r.say.empty();
  if (!r.ok) lastError = "响应无正文";
  return r;
}

LlmReply LlmClient::chatMsgs(const LlmMsg* msgs, int count, int maxTokens, float temperature) {
  return do_chat(msgs, count, maxTokens, temperature, nullptr);
}

LlmReply LlmClient::chatMsgsStream(const LlmMsg* msgs, int count, int maxTokens, float temperature,
                                   std::function<bool(const std::string& sentence)> onSentence) {
  return do_chat(msgs, count, maxTokens, temperature, onSentence);
}

// ---- embeddings：/embeddings {model, input:[text]} → data[0].embedding float 数组 ----
bool LlmClient::embed(const std::string& text, std::string& binOut) {
  std::string base = cfg.embBase.empty() ? "https://api.siliconflow.cn/v1" : cfg.embBase;
  std::string key = cfg.embKey.empty() ? cfg.llmKey : cfg.embKey;
  std::string model = cfg.embModel.empty() ? "BAAI/bge-m3" : cfg.embModel;
  if (key.empty()) { lastError = "embedding Key 未配置"; return false; }

  std::string body = "{\"model\":\"" + json_escape(model) + "\",\"input\":[\"" + json_escape(text) + "\"]}";
  std::string url = base;
  if (url.find("/embeddings") == std::string::npos)
    url += (url.back() == '/' ? "" : "/") + std::string("embeddings");

  esp_http_client_config_t conf = {};
  conf.url = url.c_str();
  conf.method = HTTP_METHOD_POST;
  conf.timeout_ms = 20000;
  conf.keep_alive_enable = false;
  esp_http_client_handle_t hc = esp_http_client_init(&conf);
  if (!hc) return false;
  esp_http_client_set_header(hc, "Content-Type", "application/json");
  std::string auth = "Bearer " + key;
  esp_http_client_set_header(hc, "Authorization", auth.c_str());
  esp_http_client_set_post_field(hc, body.c_str(), (int)body.length());
  esp_http_client_open(hc, (int)body.length());
  esp_http_client_fetch_headers(hc);
  int code = esp_http_client_get_status_code(hc);

  std::string fullText;
  char buf[2048];
  int n;
  while ((n = esp_http_client_read(hc, buf, sizeof(buf) - 1)) > 0) {
    fullText.append(buf, n);
    if (fullText.size() > 256 * 1024) break;   // 响应体上限（同 do_chat）
  }
  esp_http_client_close(hc);
  esp_http_client_cleanup(hc);

  if (code != 200) { lastError = "embed HTTP " + std::to_string(code); return false; }

  cJSON* root = cJSON_Parse(fullText.c_str());
  if (!root) { lastError = "embed 响应非 JSON"; return false; }
  cJSON* data = cJSON_GetObjectItem(root, "data");
  bool ok = false;
  if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
    cJSON* emb = cJSON_GetObjectItem(cJSON_GetArrayItem(data, 0), "embedding");
    if (cJSON_IsArray(emb)) {
      binOut.clear();
      binOut.reserve(cJSON_GetArraySize(emb) * 4);
      cJSON* item;
      cJSON_ArrayForEach(item, emb) {
        float v = (float)cJSON_GetNumberValue(item);
        binOut.append((const char*)&v, 4);
      }
      ok = (binOut.size() >= 1024 * 4);
    }
  }
  cJSON_Delete(root);
  return ok;
}

// laap_memory 的 weak 存根在此接通（M3b：语义召回通道正式生效）
bool laap_embed(const std::string& text, std::string& binOut) {
  return llm.embed(text, binOut);
}

}  // namespace laap
