// ============================================================
// LAAP 局域网 Web 面板（main 侧粘合层，与 laap_mcp_tools 同定位）
//   http://<设备IP>:8080 —— 状态 / 需求 / 实时内存 / NVS 节奏配置 /
//   记忆搜索 / 设备联网搜索 / 最近记忆 / 技能·规则·目标
// 端口 8080：配网模式（WifiConfigurationAp）独占 80，避开冲突
// 线程契约：全部走 laap_life/记忆的只读快照或既有 MCP 同款接口；
//   配置写与 consciousness.tune 同路径（cfg + NVS，字段级原子读）
// ============================================================
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <string>

#include "laap_config.h"
#include "laap_life.h"
#include "laap_memory.h"
#include "laap_skills.h"
#include "laap_rules.h"
#include "laap_cognition.h"
#include "laap_search.h"
#include "laap_util.h"

#include "wifi_manager.h"

#define TAG "laap_web"

static httpd_handle_t s_server = nullptr;

// ---------------- 页面（纯内联，无外网依赖） ----------------
static const char* PAGE_HTML = R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Aris · 状态面板</title><style>
:root{--bg:#0f1115;--card:#181b21;--line:#262a33;--fg:#e8eaf0;--dim:#8b93a3;--accent:#6b9dff;--ok:#5fd08a;--warn:#f0b34e}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.55 system-ui,-apple-system,"PingFang SC",sans-serif}
.wrap{max-width:960px;margin:0 auto;padding:20px 16px 44px}
header{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap;margin:4px 0 16px}
h1{font-size:22px;margin:0;font-weight:600}
.sub{color:var(--dim);font-size:13px}
.grid{display:grid;grid-template-columns:1fr;gap:12px}
@media(min-width:780px){.grid{grid-template-columns:1fr 1fr}.card.wide{grid-column:1/-1}}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 16px}
h2{font-size:12.5px;margin:0 0 10px;color:var(--dim);font-weight:600;letter-spacing:1px}
.pills{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px}
.pill{background:#222733;border:1px solid var(--line);border-radius:999px;padding:2px 10px;font-size:12.5px}
.pill b{color:var(--accent);font-weight:600;font-variant-numeric:tabular-nums}
.row{display:flex;justify-content:space-between;gap:10px;padding:5px 0;border-bottom:1px solid var(--line);font-size:13.5px}
.row:last-child{border:none}
.row .k{color:var(--dim);white-space:nowrap}
.row .v{text-align:right;font-variant-numeric:tabular-nums;word-break:break-all}
.row.blk{display:block}.row.blk .v{text-align:left;line-height:1.5;margin-top:2px}
.meter{display:grid;grid-template-columns:60px 1fr 52px;align-items:center;gap:8px;padding:4px 0;font-size:13px}
.meter .l{color:var(--dim)}
.meter .n{text-align:right;font-variant-numeric:tabular-nums;color:#c9cfda}
.bar{background:#222733;border-radius:6px;height:9px;overflow:hidden}
.bar>i{display:block;height:100%;border-radius:6px}
.actions{display:flex;gap:8px;align-items:center;margin-top:10px;flex-wrap:wrap}
button{background:var(--accent);border:0;color:#0b0d11;font-weight:600;border-radius:9px;padding:8px 14px;font-size:13.5px;cursor:pointer}
button.ghost{background:transparent;border:1px solid var(--line);color:var(--fg)}
input{background:#12141a;border:1px solid var(--line);color:var(--fg);border-radius:9px;padding:8px 11px;font-size:13.5px;min-width:0}
.cfg{display:grid;grid-template-columns:1fr 1fr;gap:8px 16px}
.cfg label{display:flex;justify-content:space-between;align-items:center;gap:8px;color:var(--dim);font-size:13px}
.cfg input{width:76px;text-align:right}
pre{white-space:pre-wrap;word-break:break-all;font-size:12.5px;line-height:1.5;max-height:300px;overflow:auto;margin:8px 0 0;color:#c9cfda}
.ok{color:var(--ok);font-size:13px}
</style></head><body><div class="wrap">
<header><h1>✨ Aris</h1><div class="sub" id="sub"></div></header>
<div class="grid">
<section class="card"><h2>状态</h2><div class="pills" id="pills"></div><div id="status"></div></section>
<section class="card"><h2>需求</h2><div id="needs"></div>
<div id="phys" style="margin-top:8px"></div></section>
<section class="card"><h2>实时内存</h2><div id="heap"></div></section>
<section class="card"><h2>配置（NVS · 与语音调参同源）</h2><div class="cfg" id="cfg"></div>
<div class="actions"><button onclick="saveCfg()">保存</button><span id="cfgmsg" class="ok"></span></div></section>
<section class="card wide"><h2>搜索</h2>
<div class="actions"><input id="q" placeholder="关键词…" style="flex:1"><button onclick="doSearch()">搜记忆</button><button class="ghost" onclick="doWebSearch()">搜网络（设备）</button></div>
<pre id="mres"></pre></section>
<section class="card wide"><h2>最近记忆（对话 / 事件 / 看见）</h2><pre id="chat"></pre></section>
<section class="card wide"><h2>技能 · 规则 · 目标</h2><pre id="prompt"></pre></section>
</div></div>
<script>
const $=id=>document.getElementById(id);
const esc=t=>String(t??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const MOOD={happy:'😊 开心',curious:'🤔 好奇',calm:'😌 平静',excited:'✨ 兴奋',lonely:'😔 孤独',anxious:'😟 焦虑',tired:'😪 疲惫'};
async function j(u,o){const r=await fetch(u,o);return await r.text()}
function row(k,v){const lv=esc(v);return lv.length>14
 ?'<div class="row blk"><span class="k">'+esc(k)+'</span><span class="v">'+lv+'</span></div>'
 :'<div class="row"><span class="k">'+esc(k)+'</span><span class="v">'+lv+'</span></div>'}
function meter(l,p,n,c){return '<div class="meter"><span class="l">'+l+'</span><div class="bar"><i style="width:'+Math.max(0,Math.min(100,p))+'%;background:'+(c||'var(--accent)')+'"></i></div><span class="n">'+n+'</span></div>'}
function renderStatus(s){try{s=JSON.parse(s)}catch(e){$('status').textContent=s;return}
 $('sub').textContent=(s.ip||'')+' · 设备 '+(s.time=='known'?(s.hour+' 时'):'时间未同步')+' · 已运行 '+s.uptime_h+' h';
 $('pills').innerHTML='<span class="pill">'+esc(MOOD[s.mood]||s.mood||'—')+'</span>'
  +'<span class="pill">目标 <b>'+esc(s.goal||'—')+'</b></span>'
  +'<span class="pill">信任 <b>'+s.trust+'</b></span>'
  +'<span class="pill">世代 <b>G'+s.generation+'</b></span>'
  +'<span class="pill">记忆 <b>'+s.memory_count+'</b> 条</span>'
  +'<span class="pill">独处 <b>'+s.alone_min+'</b> m</span>';
 let h='';
 h+=row('体温',s.body_temp?s.body_temp+' °C':'—（传感器不可用）');
 h+=row('运动 / 紧张',s.motion+' / '+s.body_strain);
 h+=row('信号',s.wifi_rssi+' dBm');
 if(s.last_seen)h+=row('最近看见',s.last_seen+(s.last_seen_min!=null?'（'+s.last_seen_min+' 分钟前）':''));
 if(s.cloud_emotion)h+=row('云情绪',s.cloud_emotion);
 if(s.broadcast)h+=row('此刻注意',s.broadcast);
 if(s.pending_say)h+=row('待说心声',s.pending_say);
 if(s.rhythm)h+=row('节奏',s.rhythm);
 $('status').innerHTML=h;
 const n=s.needs||{};
 const cols={energy:'#f0b34e',curiosity:'#b58cff',social:'#ff8fb1',security:'#6b9dff',expression:'#5fd08a'};
 const labs={energy:'能量',curiosity:'好奇',social:'社交',security:'安全',expression:'表达'};
 let hb='';for(const k of['energy','curiosity','social','security','expression']){const p=Math.round((n[k]||0)*100);hb+=meter(labs[k],p,p+'%',cols[k])}
 $('needs').innerHTML=hb}
async function loadStatus(){renderStatus(await j('/api/status'))}
function renderHeap(s){
 const fi=s.heap_int_kb, lg=s.heap_int_largest_kb, mn=s.heap_int_min_kb, ps=s.psram_free_kb, tot=s.free_heap_kb;
 let h='';
 h+=meter('内部空闲',fi/200*100,fi+' KB',fi<40?'var(--warn)':'var(--ok)');
 h+=meter('最大连续',lg/200*100,lg+' KB',lg<25?'var(--warn)':'var(--accent)');
 h+=meter('历史最低',mn/200*100,mn+' KB','var(--dim)');
 h+=meter('PSRAM',ps/8192*100,(ps/1024).toFixed(1)+' MB','#7d8cff');
 h+=meter('总空闲',tot/10240*100,(tot/1024).toFixed(1)+' MB','var(--accent)');
 h+=row('碎片度（最大块/空闲）',fi?Math.round(lg/fi*100)+'%':'—');
 $('heap').innerHTML=h}
async function loadHeap(){const s=JSON.parse(await j('/api/status'));renderHeap(s)}
async function loadCfg(){const c=JSON.parse(await j('/api/config'));let h='';
 for(const[k,lab]of[['express_cd_min','表达冷却（分）'],['idle_silence_min','独白静默（分）'],['idle_every_min','独白间隔（分）'],['quiet_start','静音窗起（时）'],['quiet_end','静音窗止（时）'],['screen_off_min','息屏（分）']])
  h+='<label>'+lab+'<input id="c_'+k+'" type="number" value="'+c[k]+'"></label>';
 $('cfg').innerHTML=h}
async function saveCfg(){const fd=new URLSearchParams();
 for(const k of['express_cd_min','idle_silence_min','idle_every_min','quiet_start','quiet_end','screen_off_min'])fd.append(k,$('c_'+k).value);
 await j('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:fd});
 $('cfgmsg').textContent='已保存';setTimeout(()=>$('cfgmsg').textContent='',2000);loadStatus()}
async function doSearch(){const q=$('q').value;if(!q)return;$('mres').textContent='搜索中…';
 try{$('mres').textContent='【记忆】'+ (JSON.parse(await j('/api/memory?q='+encodeURIComponent(q))).text||'（无结果）')}catch(e){$('mres').textContent='搜索失败'}}
async function doWebSearch(){const q=$('q').value;if(!q)return;$('mres').textContent='设备联网搜索中（最多 8 秒）…';
 try{$('mres').textContent='【网络】'+JSON.parse(await j('/api/websearch?q='+encodeURIComponent(q))).text}catch(e){$('mres').textContent='搜索失败'}}
async function loadChat(){try{$('chat').textContent=JSON.parse(await j('/api/chat')).text}catch(e){}}
async function loadPrompt(){try{$('prompt').textContent=JSON.parse(await j('/api/prompt')).text||'（还没有教过技能/规则/目标）'}catch(e){}}
function tick(){loadStatus();loadHeap()}
tick();loadCfg();loadChat();loadPrompt();setInterval(tick,5000);
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
  if (httpd_query_key_value(qbuf, key, out, n) != ESP_OK) return false;
  // esp_http_server 不解码百分号：中文查询词会以 %E4%BB%8A 原样到达
  // （不解码=二次编码搜出乱结果，实测；+ 号同按空格还原）
  char* o = out;
  for (char* p = out; *p; p++) {
    if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
      auto hex = [](char c) -> int {
        return (c >= '0' && c <= '9') ? c - '0' : (tolower((unsigned char)c) - 'a' + 10);
      };
      *o++ = (char)(hex(p[1]) * 16 + hex(p[2]));
      p += 2;
    } else if (*p == '+') {
      *o++ = ' ';
    } else {
      *o++ = *p;
    }
  }
  *o = 0;
  return true;
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

static esp_err_t h_websearch(httpd_req_t* req) {
  char q[160] = {0};
  if (!url_get(req, "q", q, sizeof(q)) || q[0] == 0)
    return send_json(req, std::string("{\"text\":\"（缺少 q 参数）\"}"));
  // 面板在线测设备搜索（与 self.web_search 同源；httpd 任务上下文，8s 预算）
  std::string hit = laap::laapSearch.search(q);
  return send_json(req, json_str_field(hit.empty() ? "（无结果或网络暂不可用）" : hit));
}

static esp_err_t h_train(httpd_req_t* req) {
  char q[400] = {0};
  if (!url_get(req, "q", q, sizeof(q)) || q[0] == 0)
    return send_json(req, std::string("{\"ok\":false,\"text\":\"（缺少 q 参数）\"}"));
  // 训练通道：敲门注入思考题（与心声门铃同链路；冷却/占用时拒绝并如实返回）
  bool ok = laap::life_train_ask(q);
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"ok\":%s}", ok ? "true" : "false");
  ESP_LOGI(TAG, "训练题注入：%s「%s」", ok ? "✓" : "✗", q);
  return send_json(req, std::string(buf));
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
      {.uri = "/api/websearch", .method = HTTP_GET, .handler = h_websearch},
      {.uri = "/api/train", .method = HTTP_GET, .handler = h_train},
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