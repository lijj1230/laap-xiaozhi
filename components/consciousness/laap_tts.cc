#include "laap_tts.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_websocket_client.h>
#include <mbedtls/sha256.h>
#include <esp_timer.h>
#include <time.h>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "mp3dec.h"
}

static const char* TAG = "laap_tts";
static const char* EDGE_HOST = "speech.platform.bing.com";
static const char* TCT = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
static const char* GEC_VER = "1-143.0.3650.75";

namespace laap {

LaapTts laapTts;

void laap_tts_set_output(AudioOutFn fn) { laapTts.setOutput(std::move(fn)); }

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000LL); }

static std::string uuid_no_dash() {
  uint8_t b[16];
  for (auto& x : b) x = (uint8_t)(esp_random() & 0xFF);
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  char s[33];
  for (int i = 0; i < 16; i++) sprintf(s + i * 2, "%02x", b[i]);
  s[32] = 0;
  return std::string(s);
}

// Sec-MS-GEC：SHA256((unix+11644473600 向下取整 300s)*1e7 十进制 + TCT) 大写 hex
static std::string gen_sec_ms_gec() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";
  uint64_t t = (uint64_t)(now + 11644473600ULL);
  t -= t % 300ULL;
  unsigned long long ft = t * 10000000ULL;
  char num[32]; int i = 31; num[31] = 0;
  unsigned long long v = ft;
  std::string in;
  if (v == 0) in = "0";
  else {
    while (v > 0 && i >= 0) { num[--i] = (char)('0' + (v % 10)); v /= 10; }
    in = std::string(&num[i]);
  }
  in += TCT;
  uint8_t hash[32];
  mbedtls_sha256((const uint8_t*)in.c_str(), in.length(), hash, 0);
  char hex[65];
  for (int k = 0; k < 32; k++) sprintf(hex + k * 2, "%02X", hash[k]);
  hex[64] = 0;
  return std::string(hex);
}

static std::string js_date() {
  static const char* wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  static const char* mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  time_t now = time(nullptr);
  struct tm t; gmtime_r(&now, &t);
  char buf[80];
  snprintf(buf, sizeof(buf), "%s %s %02d %d %02d:%02d:%02d GMT+0000 (Coordinated Universal Time)",
           wd[t.tm_wday], mo[t.tm_mon], t.tm_mday, t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
  return std::string(buf);
}

// ---- MP3 环形流解码（16KB PSRAM 缓冲；24kHz 输出直写 codec，零重采样） ----
static uint8_t* mp3_buf() {
  static uint8_t* p = nullptr;
  if (!p) p = (uint8_t*)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
  return p;
}
#define MP3_BUF_SIZE (16 * 1024)
static size_t s_mp3Len = 0;

static void mp3_reset() { s_mp3Len = 0; }

static bool mp3_feed_play(HMP3Decoder dec, const uint8_t* data, size_t len, bool& interrupted) {
  uint8_t* bufp = mp3_buf();
  if (!bufp) return false;
  if (s_mp3Len + len > MP3_BUF_SIZE) {   // 满：先播腾
    int offs = MP3_FindSyncWord(bufp, s_mp3Len);
    if (offs < 0) { s_mp3Len = 0; return false; }
    memmove(bufp, bufp + offs, s_mp3Len - offs);
    s_mp3Len -= offs;
  }
  memcpy(bufp + s_mp3Len, data, len);
  s_mp3Len += len;

  bool any = false;
  int pos = 0;
  while (pos + 4 < (int)s_mp3Len) {
    if (stopReq_) { interrupted = true; break; }
    int off = MP3FindSyncWord(bufp + pos, s_mp3Len - pos);
    if (off < 0) { pos = s_mp3Len; break; }
    pos += off;
    int bytesLeft = (int)(s_mp3Len - pos);
    MP3FrameInfo fi;
    int er = MP3Decode(dec, (unsigned char**)&(bufp[pos]), &bytesLeft, nullptr, 0);
    if (er) { pos += 1; continue; }
    MP3GetLastFrameInfo(dec, &fi);
    // fi.outputSamps = 采样数*声道；24kHz 单声道直写
    short* pcm = MP3GetSampBufferPtr(dec);
    if (fi.outputSamps > 0 && laapTts.out_) {
      std::vector<int16_t> out(pcm, pcm + fi.outputSamps);
      laapTts.out_(out);
      any = true;
    }
    pos += bytesLeft;
  }
  if (pos > 0 && pos <= (int)s_mp3Len) {
    memmove(bufp, bufp + pos, s_mp3Len - pos);
    s_mp3Len -= pos;
  }
  return any;
}

bool LaapTts::speak(const std::string& text, const std::string& voice, const std::string& rate) {
  lastError.clear();
  stopReq_ = false;
  time_t now = time(nullptr);
  if (now < 1700000000) { lastError = "NTP 未同步，无法生成鉴权"; return false; }
  if (speaking_) { lastError = "已有播报在飞"; return false; }
  speaking_ = true;

  std::string cid = uuid_no_dash();
  std::string path = std::string("/consumer/speech/synthesize/readaloud/edge/v1?TrustedClientToken=") + TCT +
    "&Sec-MS-GEC=" + gen_sec_ms_gec() + "&Sec-MS-GEC-Version=" + GEC_VER + "&ConnectionId=" + cid;
  std::string wsUrl = std::string("wss://") + EDGE_HOST + path;

  esp_websocket_client_config_t wsConf = {};
  wsConf.uri = wsUrl.c_str();
  wsConf.buffer_size = 8192;
  wsConf.network_timeout_ms = 10000;
  wsConf.keep_alive_enable = false;
  esp_websocket_client_handle_t ws = esp_websocket_client_init(&wsConf);
  if (!ws) { speaking_ = false; lastError = "ws init 失败"; return false; }
  esp_websocket_client_set_headers(ws,
    "Origin: chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold\r\n"
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0\r\n");
  if (esp_websocket_client_start(ws) != ESP_OK) {
    esp_websocket_client_destroy(ws);
    speaking_ = false;
    lastError = "ws 启动失败";
    return false;
  }
  // 等连接建立（≤10s）
  int wait = 0;
  while (!esp_websocket_client_is_connected(ws) && wait < 100 && !stopReq_) {
    vTaskDelay(pdMS_TO_TICKS(100));
    wait++;
  }
  if (!esp_websocket_client_is_connected(ws)) {
    esp_websocket_client_destroy(ws);
    speaking_ = false;
    lastError = "ws 连接超时";
    return false;
  }

  // speech.config + SSML
  std::string ts = js_date();
  std::string cfgmsg = "X-Timestamp:" + ts + "\r\nContent-Type:application/json; charset=utf-8\r\nPath:speech.config\r\n\r\n"
    "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":\"false\"},"
    "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}";
  esp_websocket_client_send_text(ws, cfgmsg.c_str(), cfgmsg.size(), portMAX_DELAY);

  std::string esc;
  for (char c : text) {
    if (c == '&') esc += "&amp;";
    else if (c == '<') esc += "&lt;";
    else if (c == '>') esc += "&gt;";
    else esc += c;
  }
  std::string ssml = "X-RequestId:" + cid + "\r\nContent-Type:application/ssml+xml\r\nX-Timestamp:" + ts +
    "Z\r\nPath:ssml\r\n\r\n<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='zh-CN'>"
    "<voice name='" + voice + "'><prosody pitch='+0Hz' rate='" + rate + "' volume='+0%'>" + esc +
    "</prosody></voice></speak>";
  esp_websocket_client_send_text(ws, ssml.c_str(), ssml.size(), portMAX_DELAY);

  // 收流：二进制帧=带2B头的 MP3 块 → feed 解码播；文本帧含 turn.end 结束
  mp3_reset();
  HMP3Decoder dec = MP3InitDecoder();
  bool ok = false, turnEnd = false, interrupted = false;
  uint32_t lastProgress = now_ms();

  while (!turnEnd && !interrupted) {
    if (now_ms() - lastProgress > 30000) { lastError = "30s 无进展超时"; break; }
    esp_websocket_event_data_t ev = {};
    // 轮询读帧：用 timeout=100ms 的 receive（IDF ws 无 poll，借 wait_for 数据事件）
    int len = esp_websocket_client_receive(ws, nullptr, 0, pdMS_TO_TICKS(100));
    (void)len;
    // 事件驱动数据在 receive 返回后经 esp_websocket_client_get_data? IDF5/6 用 read 路径：
    static uint8_t rbuf[8192];
    int rl = esp_websocket_client_receive(ws, rbuf, sizeof(rbuf), pdMS_TO_TICKS(1000));
    if (rl <= 0) continue;
    bool isText = false;
    const uint8_t* data = (const uint8_t*)rbuf;
    int dlen = rl;
    // esp_websocket_client_receive 返回 payload；op_code 需从事件上下文取——
    // 简化判定：文本帧必含 "Path:"；二进制 MP3 块头两字节为大端头长
    {
      // Path: 文本帧判定（看前 64B 是否含 "Path:"）
      int probe = dlen < 64 ? dlen : 64;
      for (int i = 0; i + 5 < probe; i++) {
        if (memcmp(data + i, "Path:", 5) == 0) { isText = true; break; }
      }
    }
    if (isText) {
      std::string text((const char*)data, dlen);
      if (text.find("Path:ping") != std::string::npos) {
        std::string pong = "X-RequestId:" + cid + "\r\nContent-Type:application/json; charset=utf-8\r\nPath:pong\r\n\r\n";
        esp_websocket_client_send_text(ws, pong.c_str(), pong.size(), 0);
        continue;
      }
      lastProgress = now_ms();
      if (text.find("Path:turn.end") != std::string::npos) turnEnd = true;
    } else {
      if (dlen < 2) continue;
      size_t hdrLen = ((size_t)data[0] << 8) | data[1];
      if ((int)hdrLen + 2 > dlen) continue;
      lastProgress = now_ms();
      if (mp3_feed_play(dec, data + hdrLen + 2, dlen - hdrLen - 2, interrupted)) ok = true;
    }
  }
  if (dec) MP3FreeDecoder(dec);
  esp_websocket_client_stop(ws);
  esp_websocket_client_destroy(ws);
  speaking_ = false;
  if (interrupted) return true;   // 打断算成功（说了半截）
  if (!ok && lastError.empty()) lastError = "未收到音频";
  return ok;
}

void LaapTts::stop() { stopReq_ = true; }

}  // namespace laap
