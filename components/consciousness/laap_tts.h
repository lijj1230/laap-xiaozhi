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
#include <vector>
#include <cstdint>
#include <functional>

namespace laap {

// 喇叭输出回调（由 main 在 init 时注入 AudioCodec::OutputData 的适配器——
// 意识组件不直接依赖宿主页眉，解耦 board.h 传染链）
using AudioOutFn = std::function<void(std::vector<int16_t>& data)>;

class LaapTts {
public:
  void setOutput(AudioOutFn fn) { out_ = fn; }   // main 注入
  // text 要说的话；返回 true=已播出（含被打断）。voice 如 zh-CN-XiaoxiaoNeural
  bool speak(const std::string& text, const std::string& voice, const std::string& rate = "+0%");
  void stop();                        // 请求中断当前播报（M5 打断用）
  bool speaking() const { return speaking_; }
  std::string lastError;

private:
  AudioOutFn out_;
  volatile bool speaking_ = false;
  volatile bool stopReq_ = false;
};

extern LaapTts laapTts;

// main 注入适配器用的 C++ 桥（避免 main 直接摸 LaapTts 内部）
void laap_tts_set_output(AudioOutFn fn);

}  // namespace laap
