#include "laap_search.h"
#include "laap_util.h"
#include "laap_skills.h"   // utf8_cut
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cstdlib>
#include <cctype>

static const char* TAG = "laap_search";
static const int SEARCH_BUDGET_MS = 8000;   // 协议任务阻塞上限（MCP 工具同步等结果）
static const uint32_t HEAP_FLOOR = 45000;   // 云对话期间双 TLS 并存，退化区不点火

namespace laap {

LaapSearch laapSearch;

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000LL); }

// ---- HTTP GET 全量读取（预算内；上限 maxBody）----
static bool http_get(const std::string& url, int32_t budgetMs, int maxBody, std::string& out) {
  esp_http_client_config_t conf = {};
  conf.url = url.c_str();
  conf.timeout_ms = (int)budgetMs;
  conf.buffer_size = 4096;
  conf.keep_alive_enable = false;
  // 必应 https 走宿主全局 CA bundle；失败即失败（无主源回落——单源设计）
  esp_http_client_handle_t hc = esp_http_client_init(&conf);
  if (!hc) return false;
  esp_http_client_set_header(hc, "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) laap");
  esp_err_t e = esp_http_client_open(hc, 0);
  if (e != ESP_OK) { esp_http_client_cleanup(hc); return false; }
  esp_http_client_fetch_headers(hc);
  int code = esp_http_client_get_status_code(hc);
  if (code != 200) { esp_http_client_close(hc); esp_http_client_cleanup(hc); return false; }
  out.clear();
  char buf[2048];
  int n;
  uint32_t t0 = now_ms();
  while ((n = esp_http_client_read(hc, buf, sizeof(buf) - 1)) > 0) {
    out.append(buf, n);
    if ((int)out.size() > maxBody) break;
    if ((int32_t)(now_ms() - t0) > budgetMs) break;
  }
  esp_http_client_close(hc);
  esp_http_client_cleanup(hc);
  return !out.empty();
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

// ---- 必应新闻 RSS 解析（<item><title>/<description>，CDATA 剥壳 + 实体还原）----
static std::string extract_rss_items(const std::string& xml, int maxHit, int maxLen) {
  std::string out;
  int hit = 0;
  size_t pos = 0;
  while (hit < maxHit) {
    size_t ip = xml.find("<item>", pos);
    if (ip == std::string::npos) break;
    size_t ie = xml.find("</item>", ip);
    if (ie == std::string::npos) break;
    std::string item = xml.substr(ip, ie - ip);
    pos = ie + 7;
    auto tag_inner = [&](const std::string& t) -> std::string {
      size_t a = item.find("<" + t + ">");
      if (a == std::string::npos) return "";
      size_t b = item.find("</" + t + ">", a);
      if (b == std::string::npos) return "";
      std::string v = item.substr(a + t.size() + 2, b - a - t.size() - 2);
      if (v.rfind("<![CDATA[", 0) == 0) {
        size_t ce = v.find("]]>");
        if (ce != std::string::npos) v = v.substr(9, ce - 9);
      }
      auto repl = [&](const std::string& a2, const std::string& b2) {
        size_t p;
        while ((p = v.find(a2)) != std::string::npos) v.replace(p, a2.size(), b2);
      };
      repl("&amp;", "&");
      repl("&lt;", "<");
      repl("&gt;", ">");
      repl("&quot;", "\"");
      return sanitize_utf8(v);
    };
    std::string title = tag_inner("title");
    std::string desc = tag_inner("description");
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
  return out;
}

std::string LaapSearch::search(const std::string& query, int maxHit, int maxLen) {
  lastError.clear();
  // 堆门禁：本函数运行在协议任务（云对话进行中，宿主 TLS 在场）——
  // 内部堆退化区再叠一层 TLS 会把碎片压穿（v3.76h 血泪），软失败让云端降级回答
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < HEAP_FLOOR) {
    lastError = "堆紧张，搜索跳过";
    ESP_LOGW(TAG, "%s", lastError.c_str());
    return "";
  }
  if (query.empty()) { lastError = "空查询"; return ""; }
  std::string url = "https://cn.bing.com/news/search?q=" + url_encode(query) + "&format=rss";
  std::string xml;
  if (!http_get(url, SEARCH_BUDGET_MS, 65536, xml)) { lastError = "RSS 连接失败"; return ""; }
  if (xml.find("<rss") == std::string::npos) { lastError = "RSS 非法响应"; return ""; }
  std::string out = extract_rss_items(xml, maxHit, maxLen);
  if (out.empty()) lastError = "RSS 无结果";
  ESP_LOGI(TAG, "搜索「%s」→ %uB%s", query.c_str(), (unsigned)out.size(),
           out.empty() ? lastError.c_str() : "");
  return out.empty() ? out : sanitize_utf8(out);
}

}  // namespace laap