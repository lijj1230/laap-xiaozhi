#include "laap_search.h"
#include "laap_config.h"
#include "laap_util.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cstdlib>
#include <ctime>

static const char* TAG = "laap_search";

namespace laap {

LaapSearch laapSearch;

static SemaphoreHandle_t s_netMtx = nullptr;
static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000LL); }

bool LaapSearch::net_lock() {
  if (!s_netMtx) s_netMtx = xSemaphoreCreateMutex();
  return xSemaphoreTake(s_netMtx, pdMS_TO_TICKS(400)) == pdTRUE;
}
void LaapSearch::net_unlock() { xSemaphoreGive(s_netMtx); }

void LaapSearch::begin() { ok_ = true; }

// ---- HTTP GET 全量读取（预算内；上限 maxBody）----
static bool http_get(const std::string& url, int32_t budgetMs, int maxBody, std::string& out) {
  esp_http_client_config_t conf = {};
  conf.url = url.c_str();
  conf.timeout_ms = (int)budgetMs;
  conf.buffer_size = 4096;
  conf.keep_alive_enable = false;
  // 必应/DDG 的 https 走默认证书（IDF 全局 CA bundle 由宿主配置）；失败即失败
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
    else { snprintf(buf, sizeof(buf), "%%%02X", c); o += buf; }
  }
  return o;
}

// ---- 必应 RSS：最轻量（3-4KB），大陆可达 ----
static std::string extract_rss_items(const std::string& xml, int maxHit, int maxLen) {
  // <item><title>..</title>.. <description>..</description>
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
      // CDATA 剥壳 + 常见实体
      if (v.rfind("<![CDATA[", 0) == 0) {
        size_t ce = v.find("]]>");
        if (ce != std::string::npos) v = v.substr(9, ce - 9);
      }
      auto repl = [&](const std::string& a2, const std::string& b2) {
        size_t p;
        while ((p = v.find(a2)) != std::string::npos) v.replace(p, a2.size(), b2);
      };
      repl("&amp;", "&"); repl("&lt;", "<"); repl("&gt;", ">"); repl("&quot;", "\"");
      return sanitize_utf8(v);
    };
    std::string title = tag_inner("title");
    std::string desc = tag_inner("description");
    std::string one = (title.size() >= 4 && desc.size() >= 8) ? title + "：" + desc : (title.size() >= 4 ? title : desc);
    if (one.size() < 4) continue;
    if (one.size() > 220) one = utf8_cut(one, 220);
    if (!out.empty()) out += "；";
    out += one;
    hit++;
    if ((int)out.size() > maxLen) break;
  }
  return out;
}

std::string LaapSearch::search_rss(const std::string& q, int maxHit, int maxLen, int32_t budgetMs) {
  std::string url = "https://cn.bing.com/news/search?q=" + url_encode(q) + "&format=rss";
  std::string xml;
  if (!http_get(url, budgetMs, 65536, xml)) { lastError = "RSS 连接失败"; return ""; }
  if (xml.find("<rss") == std::string::npos) { lastError = "RSS 非法响应"; return ""; }
  std::string out = extract_rss_items(xml, maxHit, maxLen);
  if (out.empty()) lastError = "RSS 无结果";
  return out;
}

// ---- DDG Instant Answer（被墙快败，退避期跳过）----
static uint8_t s_ddgFail = 0;
static uint32_t s_ddgSkipMs = 0;

std::string LaapSearch::search_ddg(const std::string& q, int maxHit, int maxLen, int32_t budgetMs) {
  if (s_ddgFail >= 2 && (int32_t)(now_ms() - s_ddgSkipMs) < 600000) {
    lastError = "DDG跳过(连败退避)";
    return "";
  }
  std::string url = "https://api.duckduckgo.com/?q=" + url_encode(q) + "&format=json&no_html=1";
  std::string body;
  if (!http_get(url, budgetMs, 24576, body)) { lastError = "DDG连接失败"; s_ddgFail++; s_ddgSkipMs = now_ms(); return ""; }
  // AbstractText 优先
  std::string out;
  size_t ai = body.find("\"AbstractText\":\"");
  if (ai != std::string::npos) {
    std::string v = body.substr(ai + 15);
    size_t ve = v.find('"');
    if (ve != std::string::npos && ve > 2) out = v.substr(0, ve);
  }
  if (!out.empty()) s_ddgFail = 0;
  else { s_ddgFail++; s_ddgSkipMs = now_ms(); lastError = "DDG 无结果"; }
  return out;
}

std::string LaapSearch::search_locked(const std::string& q, int maxHit, int maxLen) {
  if (!ok_) { lastError = "搜索未启用"; return ""; }
  lastError = "";
  // v3.76e 总预算语义保留：25s 封顶，源间查剩余
  const uint32_t t0 = now_ms();
  auto remain = [&t0]() -> int32_t { return (int32_t)(25000UL - (now_ms() - t0)); };
  auto remOrMin = [&remain]() -> int32_t { int32_t r = remain(); return r > 3000 ? r : 3000; };

  // 配置模板主源（{q} 替换）
  if (!cfg.searchApi.empty()) {
    std::string url = cfg.searchApi;
    size_t qp = url.find("{q}");
    if (qp != std::string::npos) url.replace(qp, 3, url_encode(q));
    std::string body;
    if (http_get(url, remOrMin(), 65536, body)) {
      std::string out = extract_rss_items(body, maxHit, maxLen);   // 模板默认出 RSS
      if (!out.empty()) return out;
      lastError = "主源 无结果";
    } else lastError = "主源连接失败";
  }

  std::string err1 = lastError;
  std::string r = search_rss(q, maxHit, maxLen, remOrMin());
  if (!r.empty()) return r;
  std::string err2 = lastError;
  if (remain() < 3000) { lastError = err1 + " | " + err2 + " | DDG跳过(预算尽)"; return ""; }
  r = search_ddg(q, maxHit, maxLen, remOrMin());
  if (!r.empty()) return r;
  lastError = err1 + " | " + err2 + " | " + lastError;
  return "";
}

std::string LaapSearch::search(const std::string& query, int maxHit, int maxLen) {
  if (!net_lock()) {
    ESP_LOGI(TAG, "搜索正忙，本次放弃");
    return "";
  }
  std::string r = search_locked(query, maxHit, maxLen);
  net_unlock();
  return r.empty() ? r : sanitize_utf8(r);
}

}  // namespace laap
