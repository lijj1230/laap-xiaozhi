#pragma once
// ============================================================
// LAAP 设备侧联网搜索（MCP 工具用；单源=必应新闻 RSS，DDG 已删）
//   云 LLM 经 self.web_search 调用：设备自己出网、零云配额（云端搜索仅
//   50 次/月）、零 API Key。运行在协议任务（云对话期间，宿主 TLS 在场）——
//   8s 预算封顶 + 45KB 堆门禁软失败（v3.76h 教训）。
// ============================================================
#include <string>

namespace laap {

class LaapSearch {
public:
  // 返回 ≤maxLen 字节知识文本（"标题：摘要；…"）；失败/无结果返回 ""
  std::string search(const std::string& query, int maxHit = 3, int maxLen = 500);
  std::string lastError;
};

extern LaapSearch laapSearch;

}  // namespace laap