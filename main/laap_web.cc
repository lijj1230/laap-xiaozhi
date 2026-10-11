// ============================================================
// LAAP 局域网 Web 面板（main 侧粘合层，与 laap_mcp_tools 同定位）
//   http://<设备IP>:8080 —— 状态查看 / NVS 节奏配置修改 / 记忆搜索 /
//   最近记忆（对话+事件）/ 技能·规则·目标
// 端口 8080：配网模式（WifiConfigurationAp）独占 80，避开冲突
// 线程契约：全部走 laap_life/记忆的只读快照或既有 MCP 同款接口；
//   配置写与 consciousness.tune 同路径（cfg + NVS，字段级原子读）
// ============================================================
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>

#include "laap_config.h"
#include "laap_life.h"
#include "laap_memory.h"
#include "laap_skills.h"
#include "laap_rules.h"
#include "laap_cognition.h"
#include "laap_util.h"

#include "wifi_manager.h"

#define TAG "laap_web"

static httpd_handle_t s_server = nullptr;

// ---------------- 页面（纯内联，无外网依赖） ----------------
static const char* PAGE_HTML = R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Aris · 状态面板</title><style>
body{font-family:system-ui,-apple-system,"PingFang SC",sans-serif;background:#14161a;color:#e8e8ea;margin:0 auto;padding:16px;max-width:760px}
h1{font-size:20px;margin:8px 0 2px}h2{font-size:15px;margin:0 0 6px;color:#9aa3b2}
.card{background:#1e2127;border-radius:12px;padding:14px 16px;margin:10px 0}
.row{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:4px 0;font-size:14px;border-bottom:1px solid #2a2e36}
.row:last-child{border:none}
.row.blk{display:block;padding:6px 0}
.row.blk .k{color:#9aa3b2;font-size:13px;margin-bottom:3px}
.row.blk .v{font-size:14px;line-height:1.55;word-break:break-all}.bar{background:#2a2e36;border-radius:6px;height:10px;flex:1;overflow:hidden}
.bar>i{display:block;height:100%;background:#5b8def}
pre{white-space:pre-wrap;word-break:break-all;font-size:13px;line-height:1.5;max-height:340px;overflow:auto;margin:6px 0 0}
button{background:#5b8def;border:0;color:#fff;border-radius:8px;padding:8px 14px;font-size:14px}
input{background:#14161a;border:1px solid #2a2e36;color:#e8e8ea;border-radius:8px;padding:7px 10px;font-size:14px}
label{display:flex;justify-content:space-between;align-items:center;gap:8px;margin:6px 0;font-size:14px}
label input{width:90px;text-align:right}
</style></head><body>
<h1>✨ Aris</h1><div id="sub" style="color:#9aa3b2;font-size:13px"></div>
<div class="card"><h2>状态</h2><div id="status">加载中…</div></div>
<div class="card"><h2>配置（NVS · 与 consciousness.tune 同源）</h2><div id="cfg"></div>
<div style="margin-top:8px"><button onclick="saveCfg()">保存</button>
<span id="cfgmsg" style="color:#7fc97f;font-size:13px"></span></div></div>
<div class="card"><h2>记忆搜索</h2>
<div style="display:flex;gap:8px"><input id="q" placeholder="关键词" style="flex:1"><button onclick="doSearch()">搜索</button></div>
<pre id="mres"></pre></div>
<div class="card"><h2>最近记忆（对话 / 事件 / 看见）</h2><pre id="chat"></pre></div>
<div class="card"><h2>技能 · 规则 · 目标</h2><pre id="prompt"></pre></div>
<script>
const $=id=>document.getElementById(id);
const esc=t=>String(t??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function row(k,v){const lv=esc(v);return lv.length>12
 ?'<div class="row blk"><div class="k">'+esc(k)+'</div><div class="v">'+lv+'</div></div>'
 :'<div class="row"><span style="color:#9aa3b2">'+esc(k)+'</span><span>'+lv+'</span></div>'}
async function j(u,o){const r=await fetch(u,o);return await r.text()}
function renderStatus(s){try{s=JSON.parse(s)}catch(e){$('status').textContent=s;return}
 $('sub').textContent=(s.ip?('http://'+s.ip+':8080 · '):'')+(s.time=='known'?('设备 '+s.hour+' 时'):'时间未同步');
 const rows=[['情绪',s.mood],['目标',s.goal],['信任',s.trust],['世代','G'+s.generation],['记忆条数',s.memory_count],['体温',s.body_temp?s.body_temp+' °C':'—'],['运动',s.motion],['紧张度',s.body_strain],['运行',s.uptime_h+' h'],['堆',s.free_heap_kb+' KB'],['信号',s.wifi_rssi+' dBm'],['独处',s.alone_min+' min']];
 if(s.last_seen)rows.push(['最近看见',s.last_seen]);if(s.cloud_emotion)rows.push(['云情绪',s.cloud_emotion]);
 if(s.rhythm)rows.push(['节奏',s.rhythm]);
 let h='';for(const[k,v]of rows)h+=row(k,v);
 const n=s.needs||{};for(const[k,lab]of[['energy','能量'],['curiosity','好奇'],['social','社交'],['security','安全'],['expression','表达']]){const p=Math.round((n[k]||0)*100);h+='<div class="row"><span style="color:#9aa3b2">'+lab+'</span><div class="bar"><i style="width:'+p+'%"></i></div><span>'+p+'%</span></div>'}
 $('status').innerHTML=h}
async function loadStatus(){renderStatus(await j('/api/status'))}
async function loadCfg(){const c=JSON.parse(await j('/api/config'));let h='';
 for(const[k,lab]of[['express_cd_min','表达冷却（分）'],['idle_silence_min','独白静默（分）'],['idle_every_min','独白间隔（分）'],['quiet_start','静音窗起（时）'],['quiet_end','静音窗止（时）'],['screen_off_min','息屏（分）']])
  h+='<label>'+lab+'<input id="c_'+k+'" type="number" value="'+c[k]+'"></label>';
 $('cfg').innerHTML=h}
async function saveCfg(){const fd=new URLSearchParams();
 for(const k of['express_cd_min','idle_silence_min','idle_every_min','quiet_start','quiet_end','screen_off_min'])fd.append(k,$('c_'+k).value);
 await j('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:fd});
 $('cfgmsg').textContent='已保存';setTimeout(()=>$('cfgmsg').textContent='',2000);loadStatus()}
async function doSearch(){const q=$('q').value;if(!q)return;$('mres').textContent='搜索中…';
 try{$('mres').textContent=JSON.parse(await j('/api/memory?q='+encodeURIComponent(q))).text||'（无结果）'}catch(e){$('mres').textContent='搜索失败'}}
async function loadChat(){try{$('chat').textContent=JSON.parse(await j('/api/chat')).text}catch(e){}}
async function loadPrompt(){try{$('prompt').textContent=JSON.parse(await j('/api/prompt')).text||'（还没有教过技能/规则/目标）'}catch(e){}}
loadStatus();loadCfg();loadChat();loadPrompt();setInterval(loadStatus,5000);
</script></body></html>)HTML";

// ---------------- 工具 ----------------
static esp_err_t send_json(httpd_req_t* req, const std::string& body) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, body.c_str(), body.size());
}

static std::string json_str_field(const std::string& s) {
  return std::string("{\"text\":\"") + laap::json_escape(s) + "\"}";
}

static bool url_get(httpd_req_t* req, const char* key, char* out, size_t n) {
  char qbuf[512];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) return false;
  return httpd_query_key_value(qbuf, key, out, n) == ESP_OK;
}

static void apply_cfg_field(const char* key, long v) {
  if (strcmp(key, "express_cd_min") == 0) laap::cfg.expressCdMin = (uint32_t)(v < 0 ? 0 : v > 240 ? 240 : v);
  else if (strcmp(key, "idle_silence_min") == 0) laap::cfg.idleSilenceMin = (uint32_t)(v < 0 ? 0 : v > 1440 ? 1440 : v);
  else if (strcmp(key, "idle_every_min") == 0) laap::cfg.idleEveryMin = (uint32_t)(v < 0 ? 0 : v > 1440 ? 1440 : v);
  else if (strcmp(key, "quiet_start") == 0) laap::cfg.quietStart = (int)(v < 0 ? 0 : v > 23 ? 23 : v);
  else if (strcmp(key, "quiet_end") == 0) laap::cfg.quietEnd = (int)(v < 0 ? 0 : v > 23 ? 23 : v);
  else if (strcmp(key, "screen_off_min") == 0) laap::cfg.screenOffMin = (uint32_t)(v < 0 ? 0 : v > 240 ? 240 : v);
}

static std::string cfg_json() {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"express_cd_min\":%u,\"idle_silence_min\":%u,\"idle_every_min\":%u,"
           "\"quiet_start\":%d,\"quiet_end\":%d,\"screen_off_min\":%u}",
           (unsigned)laap::cfg.expressCdMin, (unsigned)laap::cfg.idleSilenceMin,
           (unsigned)laap::cfg.idleEveryMin, laap::cfg.quietStart, laap::cfg.quietEnd,
           (unsigned)laap::cfg.screenOffMin);
  return std::string(buf);
}

// ---------------- 路由 ----------------
static esp_err_t h_index(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t* req) {
  std::string s = laap::life_status_snapshot();
  std::string ip = WifiManager::GetInstance().GetIpAddress();
  if (!ip.empty() && s.size() > 1 && s.back() == '}') {
    s.pop_back();
    s += ",\"ip\":\"" + ip + "\"}";
  }
  return send_json(req, s);
}

static esp_err_t h_chat(httpd_req_t* req) {
  return send_json(req, json_str_field(laap::memory.episodicNumberedTail(30)));
}

static esp_err_t h_memory(httpd_req_t* req) {
  char q[160] = {0};
  if (!url_get(req, "q", q, sizeof(q)) || q[0] == 0)
    return send_json(req, std::string("{\"text\":\"（缺少 q 参数）\"}"));
  std::string hit = laap::life_recall(q, 500);
  return send_json(req, json_str_field(hit.empty() ? "（记忆里没有找到相关内容）" : hit));
}

static esp_err_t h_prompt(httpd_req_t* req) {
  std::string out;
  std::string sk = laap::skills.promptLine();
  if (!sk.empty()) out += sk;
  std::string rl = laap::rules.promptLine();
  if (!rl.empty()) out += (out.empty() ? "" : "\n") + rl;
  std::string it = laap::mind.intentsLine();
  if (!it.empty()) out += (out.empty() ? "" : "\n") + it;
  return send_json(req, json_str_field(out));
}

static esp_err_t h_cfg_get(httpd_req_t* req) { return send_json(req, cfg_json()); }

static esp_err_t h_cfg_post(httpd_req_t* req) {
  static const char* keys[] = {"express_cd_min",  "idle_silence_min", "idle_every_min",
                               "quiet_start",      "quiet_end",        "screen_off_min"};
  // 请求体只能读一次：先整读进缓冲，再逐个键解析（逐键 recv=第 2 键起拿到空 body，实测踩坑）
  char buf[512];
  int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
  bool any = false;
  if (len > 0) {
    buf[len] = 0;
    for (auto k : keys) {
      char v[16] = {0};
      if (httpd_query_key_value(buf, k, v, sizeof(v)) == ESP_OK && v[0]) {
        apply_cfg_field(k, strtol(v, nullptr, 10));
        any = true;
      }
    }
  }
  if (any && !laap::cfg.save()) ESP_LOGW(TAG, "NVS 写失败（本次有效，重启还原）");
  ESP_LOGI(TAG, "Web 面板改配置：%s", cfg_json().c_str());
  return send_json(req, cfg_json());
}

// ---------------- 启动（等 WiFi 拿到 IP 后开服） ----------------
static void web_task(void*) {
  for (;;) {
    if (!WifiManager::GetInstance().GetIpAddress().empty()) break;
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
  conf.server_port = 8080;        // 80 留给配网模式（WifiConfigurationAp）
  conf.max_uri_handlers = 12;
  conf.stack_size = 8192;
  conf.lru_purge_enable = true;
  if (httpd_start(&s_server, &conf) != ESP_OK) {
    ESP_LOGE(TAG, "Web 面板启动失败（意识功能不受影响）");
    vTaskDelete(nullptr);
    return;
  }
  static const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = h_index},
      {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
      {.uri = "/api/chat", .method = HTTP_GET, .handler = h_chat},
      {.uri = "/api/memory", .method = HTTP_GET, .handler = h_memory},
      {.uri = "/api/prompt", .method = HTTP_GET, .handler = h_prompt},
      {.uri = "/api/config", .method = HTTP_GET, .handler = h_cfg_get},
      {.uri = "/api/config", .method = HTTP_POST, .handler = h_cfg_post},
  };
  for (auto& r : routes) httpd_register_uri_handler(s_server, &r);
  ESP_LOGI(TAG, "LAAP 面板就绪：http://%s:8080",
           WifiManager::GetInstance().GetIpAddress().c_str());
  vTaskDelete(nullptr);
}

void laap_web_start() {
  if (xTaskCreate(web_task, "laap_web", 4096, nullptr, 2, nullptr) != pdPASS)
    ESP_LOGE(TAG, "Web 面板任务创建失败");
}