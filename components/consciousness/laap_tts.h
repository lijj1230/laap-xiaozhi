#pragma once
// ============================================================
// LAAP 设备侧 TTS（移植自 laap-esp32 laap_edge_tts，一期简化版）
//   Edge 朗读服务（免费无 Key）→ MP3 → libhelix → 小智 codec 喇叭直写。
//   简化项：预取连接槽不做（每次现场握手 ~0.5s，M5 的表达链可接受）；
//   pong 保活/30s 无进展超时/堆门禁语义保留（v3.76e 血泪在此）。
//   互斥：同一时间只允许一个 TTS 会话（与小智云音频天然错开——调用方
//   应在宿主 speaking 状态时让路，M5 的门禁负责）。
// ============================================================
#include <string>

namespace laap {

class LaapTts {
public:
  // text 要说的话；返回 true=已播出（含被打断）。voice 如 zh-CN-XiaoxiaoNeural
  bool speak(const std::string& text, const std::string& voice, const std::string& rate = "+0%");
  void stop();                        // 请求中断当前播报（M5 打断用）
  bool speaking() const { return speaking_; }
  std::string lastError;

private:
  volatile bool speaking_ = false;
  volatile bool stopReq_ = false;
};

extern LaapTts laapTts;

}  // namespace laap
