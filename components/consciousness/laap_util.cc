#include "laap_util.h"
#include <cstring>
#include <vector>

namespace laap {

// ---- 严格 UTF-8 清洗（v3.67 语义 1:1：逐字节校验，非字符/代理区整类拒收） ----
std::string sanitize_utf8(const std::string& s) {
  std::string out;
  out.reserve(s.length());
  size_t i = 0;
  const size_t n = s.length();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    int need;
    bool reject = false;
    if (c < 0x80) need = 0;
    else if (c == 0xC0 || c == 0xC1) { reject = true; need = 0; }   // 过长编码
    else if ((c & 0xE0) == 0xC0) need = 1;
    else if ((c & 0xF0) == 0xE0) need = 2;
    else if (c == 0xF0)          need = 3;
    else if (c == 0xF4)          need = 3;
    else if ((c & 0xF8) == 0xF0) { reject = true; need = 3; }       // F5-FF 非法
    else { i++; continue; }                                          // 孤立续字节
    bool ok = !reject && (i + (size_t)need < n);
    unsigned cp = 0;
    for (int k = 1; ok && k <= need; k++)
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) ok = false;
    if (ok) {
      // 解码码点（原始：need==0 时 cp=c）
      cp = c & (unsigned)(0x7F >> (need == 0 ? 7 : need));
      for (int k = 1; k <= need; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
      if (cp == 0xFFFE || cp == 0xFFFF || (cp >= 0xD800 && cp <= 0xDFFF)) ok = false;   // 非字符
    }
    if (ok) {
      out.append(s, i, need + 1);
      i += need + 1;
    } else {
      out += "?";   // 替换符用 ?（原实现同款：不用 U+FFFD 防再次中毒）
      i += need + 1;
    }
  }
  return out;
}

// ---- JSON 转义（v3.55 语义：引号/反斜杠/控制字符 <0x20 → \uXXXX） ----
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.length() + 16);
  char buf[8];
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20) {
          snprintf(buf, sizeof(buf), "\\u%04X", c);
          out += buf;
        } else out += (char)c;
    }
  }
  return out;
}

// ---- 二字组覆盖率 ----
// UTF-8 感知的"字符"切分：取每个字符的首字节位置序列，二字组 = 相邻字符对。
// small 的字符对（去重）有多少出现在 big 中。
static void collect_char_starts(const std::string& s, std::vector<size_t>& starts) {
  starts.clear();
  for (size_t i = 0; i < s.size();) {
    starts.push_back(i);
    unsigned char c = (unsigned char)s[i];
    size_t len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xF0) ? 4 : 3;
    i += len;
  }
}

float bigram_coverage(const std::string& small, const std::string& big) {
  if (small.size() < 2 || big.size() < 2) return 0;
  std::vector<size_t> bs, gs;
  collect_char_starts(small, bs);
  collect_char_starts(big, gs);
  int total = 0, hit = 0;
  for (size_t i = 0; i + 1 < bs.size(); i++) {
    // 二字组 = 字符 i 与字符 i+1 的完整 UTF-8 字节段
    std::string pair = small.substr(bs[i], (i + 2 < bs.size() ? bs[i + 2] : small.size()) - bs[i]);
    total++;
    if (big.find(pair) != std::string::npos) hit++;
  }
  return total >= 3 && hit * 10 >= total * 3 ? (float)hit / total : (total ? (float)hit / total : 0);
}

bool bigram_mostly_in(const std::string& small, const std::string& big) {
  if (small.size() < 2 || big.size() < 2) return false;
  std::vector<size_t> bs, gs;
  collect_char_starts(small, bs);
  collect_char_starts(big, gs);
  int total = 0, hit = 0;
  for (size_t i = 0; i + 1 < bs.size(); i++) {
    std::string pair = small.substr(bs[i], (i + 2 < bs.size() ? bs[i + 2] : small.size()) - bs[i]);
    total++;
    if (big.find(pair) != std::string::npos) hit++;
  }
  return total >= 3 && hit * 10 >= total * 3;   // v3.53 语义：覆盖率 ≥30%
}

}  // namespace laap
