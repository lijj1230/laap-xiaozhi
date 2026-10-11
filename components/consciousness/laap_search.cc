#include "laap_search.h"
#include "laap_util.h"
#include "laap_skills.h"   // utf8_cut
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cstdlib>
#include <cctype>

static const char* TAG = "laap_search";
static const int SEARCH_BUDGET_MS = 8000;   // 协议任务阻塞上限（MCP 工具同步等结果）
static const uint32_t HEAP_FLOOR = 30000;   // TLS 握手内部堆底线（2026-10-11 实测：本机稳态
                                            // 最大内部块 <45KB 是常态，45K 门禁把搜索全挡了；
                                            // 响应缓冲已挪 PSRAM，内部堆只担 TLS ~30KB）
static const int BODY_MAX = 64000;          // 覆盖桌面版页面（结果块 56KB 起）与精简页
// 精简页 UA：服务端给老手机发的页面比桌面版小一半且结果块提前（桌面 98KB/56KB 起，
// 精简 60KB/39KB 起）；format=rss 端点已被必应下线（302→首页，M6 实测）
static const char* UA = "Mozilla/5.0 (Linux; U; Android 2.3; zh-cn) AppleWebKit/533.1";

namespace laap {

LaapSearch laapSearch;

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000LL); }

// ---- HTTP GET → 调用方缓冲（预算内；30x 手动跟随——open/read 流模式不自动跟随，
//      必应 302 实测；返回 false=失败，bodyLen=有效字节数）----
// 缓冲用 malloc（判空安全）：-fno-exceptions 下 std::string 大块分配失败是 abort
static bool http_get_buf(const std::string& url, int32_t budgetMs, char* body, int bodyMax,
                         int& bodyLen) {
  std::string cur = url;
  for (int hop = 0; hop < 3; hop++) {
    esp_http_client_config_t conf = {};
    conf.url = cur.c_str();
    conf.timeout_ms = (int)budgetMs;
    conf.buffer_size = 4096;
    conf.keep_alive_enable = false;
    conf.crt_bundle_attach = esp_crt_bundle_attach;   // 全局 CA bundle（缺这行=mbedtls 拒建链，M6 实测）
    esp_http_client_handle_t hc = esp_http_client_init(&conf);
    if (!hc) return false;
    esp_http_client_set_header(hc, "User-Agent", UA);
    esp_err_t e = esp_http_client_open(hc, 0);
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "http open 失败 0x%x（DNS/TCP/TLS）", e);
      esp_http_client_cleanup(hc);
      return false;
    }
    esp_http_client_fetch_headers(hc);
    int code = esp_http_client_get_status_code(hc);
    if (code >= 300 && code < 400) {   // 重定向：取 Location 换 URL 重来
      char* loc = nullptr;
      bool has = esp_http_client_get_header(hc, "Location", &loc) == ESP_OK && loc && loc[0];
      ESP_LOGI(TAG, "http %d → %s", code, has ? loc : "(无 Location)");
      esp_http_client_close(hc);
      esp_http_client_cleanup(hc);
      if (!has) return false;
      std::string next = loc;
      if (next.rfind("http", 0) != 0) {   // 相对地址：按当前源补全
        size_t sp = cur.find("//");
        std::string origin = (sp == std::string::npos) ? cur : cur.substr(0, cur.find('/', sp + 2));
        next = (next[0] == '/') ? origin + next : origin + "/" + next;
      }
      cur = next;
      continue;
    }
    if (code != 200) {
      ESP_LOGW(TAG, "http 状态码 %d（非 200）", code);
      esp_http_client_close(hc);
      esp_http_client_cleanup(hc);
      return false;
    }
    bodyLen = 0;
    char buf[2048];
    int n;
    uint32_t t0 = now_ms();
    while ((n = esp_http_client_read(hc, buf, sizeof(buf))) > 0) {
      if (bodyLen + n > bodyMax) {
        memcpy(body + bodyLen, buf, bodyMax - bodyLen);
        bodyLen = bodyMax;
        break;
      }
      memcpy(body + bodyLen, buf, n);
      bodyLen += n;
      if ((int32_t)(now_ms() - t0) > budgetMs) break;
    }
    esp_http_client_close(hc);
    esp_http_client_cleanup(hc);
    return bodyLen > 0;
  }
  return false;
}

// ---- URL 编码（查询词通常是中文/空格）----
static std::string url_encode(const std::string& q) {
  std::string o;
  char buf[8];
  for (unsigned char c : q) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else if (c == ' ') o += '+';
    else {
      snprintf(buf, sizeof(buf), "%%%02X", c);
      o += buf;
    }
  }
  return o;
}

// ---- HTML 内联标签剥离（<h2><strong>标题</strong></h2> → 标题）----
static void strip_tags(const char* s, size_t n, std::string& out) {
  bool intag = false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '<') { intag = true; continue; }
    if (c == '>') { intag = false; continue; }
    if (!intag) out += c;
  }
}

static void restore_entities(std::string& v) {
  auto repl = [&](const std::string& a, const std::string& b) {
    size_t p;
    while ((p = v.find(a)) != std::string::npos) v.replace(p, a.size(), b);
  };
  repl("&amp;", "&");
  repl("&lt;", "<");
  repl("&gt;", ">");
  repl("&quot;", "\"");
  repl("&#39;", "'");
}

// ---- 必应 HTML 结果解析：class="b_algo" 块 → <h2>标题</h2> + <p>摘要</p> ----
// 关键：h2/p 必须在**本结果块的 </li> 边界内**搜索——桌面版页面脚本区也含
// "b_algo" 字符串，无界搜索会捞到脚本噪声（实测'解析无结果'根因之一）
static const char* find_in(const char* from, const char* end, const char* needle, size_t n) {
  if (from >= end) return nullptr;
  return (const char*)memmem(from, (size_t)(end - from), needle, n);
}

static std::string extract_bing_items(const char* body, int len, int maxHit, int maxLen) {
  std::string out;
  int hit = 0;
  const char* p = body;
  const char* end = body + len;
  while (hit < maxHit && p < end) {
    const char* algo = find_in(p, end, "b_algo", 6);
    if (!algo) break;
    const char* blockEnd = find_in(algo, end, "</li>", 5);
    if (!blockEnd) blockEnd = end;          // 无闭合：以文末为界
    p = algo + 6;
    const char* h2 = find_in(algo, blockEnd, "<h2", 3);
    if (!h2) continue;                      // 本块没标题：跳过
    const char* h2e = find_in(h2, blockEnd, "</h2>", 5);
    if (!h2e) continue;
    std::string title;
    strip_tags(h2, h2e - h2, title);
    std::string desc;
    const char* pp = find_in(h2e, blockEnd, "<p", 2);
    if (pp) {
      const char* pe = find_in(pp, blockEnd, "</p>", 4);
      if (pe) strip_tags(pp, pe - pp, desc);
    }
    restore_entities(title);
    restore_entities(desc);
    std::string one = (title.size() >= 4 && desc.size() >= 8)
                          ? title + "：" + desc
                          : (title.size() >= 4 ? title : desc);
    if (one.size() < 4) continue;
    if (one.size() > 220) one = utf8_cut(one, 220);
    if (!out.empty()) out += "；";
    out += one;
    hit++;
    if ((int)out.size() > maxLen) break;
  }
  return sanitize_utf8(out);
}

std::string LaapSearch::search(const std::string& query, int maxHit, int maxLen) {
  lastError.clear();
  // 堆门禁：内部堆最大块 < TLS 握手需求时不点火（软失败让云端降级回答）；
  // 日志带实际块值，便于随版本标定
  {
    uint32_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (blk < HEAP_FLOOR) {
      char e[48];
      snprintf(e, sizeof(e), "堆紧张，搜索跳过（最大块%uB）", (unsigned)blk);
      lastError = e;
      ESP_LOGW(TAG, "%s", lastError.c_str());
      return "";
    }
  }
  if (query.empty()) {
    lastError = "空查询";
    ESP_LOGW(TAG, "搜索被调但 query 为空（云端没按 schema 传参？）");
    return "";
  }
  std::string url = "https://cn.bing.com/search?q=" + url_encode(query) + "&mkt=zh-CN";
  // 响应缓冲放 PSRAM（64KB 大块不占内部堆——内部堆留给 TLS 握手；malloc 判空，
  // -fno-exceptions 下 string 大块分配失败是 abort）
  char* body = (char*)heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (body == nullptr) body = (char*)malloc(BODY_MAX);   // 无 PSRAM 芯片的兜底
  if (body == nullptr) {
    lastError = "响应缓冲分配失败";
    ESP_LOGW(TAG, "%s", lastError.c_str());
    return "";
  }
  int bodyLen = 0;
  bool ok = http_get_buf(url, SEARCH_BUDGET_MS, body, BODY_MAX, bodyLen);
  std::string out;
  if (ok && memmem(body, bodyLen, "b_algo", 6) != nullptr) {
    out = extract_bing_items(body, bodyLen, maxHit, maxLen);
    if (out.empty()) {
      char e[64];
      snprintf(e, sizeof(e), "解析无结果（%dB）", bodyLen);
      lastError = e;
    }
  } else if (ok) {
    char e[64];
    snprintf(e, sizeof(e), "无结果块（反爬页？%dB）", bodyLen);
    lastError = e;
  } else
    lastError = "搜索页获取失败";
  free(body);
  if (out.empty()) ESP_LOGW(TAG, "%s", lastError.c_str());
  else ESP_LOGI(TAG, "搜索「%s」→ %uB", query.c_str(), (unsigned)out.size());
  return out;
}

}  // namespace laap
