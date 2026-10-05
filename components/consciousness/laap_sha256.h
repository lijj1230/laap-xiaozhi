#pragma once
// ============================================================
// 极简 SHA256（FIPS 180-4，无依赖）
// 用途：Edge TTS 的 Sec-MS-GEC 鉴权（IDF6 mbedtls 公共头迁移 PSA 后
// 不再稳定暴露 mbedtls/sha256.h，内置实现避开版本迁移坑）。
// 输入 <64MB；仅用于非安全场景的指纹哈希。
// ============================================================
#include <cstdint>
#include <cstring>
#include <string>

namespace laap {

inline void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  size_t total = len;
  auto rotr = [](uint32_t x, int r) { return (x >> r) | (x << (32 - r)); };
  auto process = [&](const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
      uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
  };
  size_t i = 0;
  for (; i + 64 <= len; i += 64) process(data + i);
  uint8_t tail[128];
  size_t rem = len - i;
  memcpy(tail, data + i, rem);
  tail[rem++] = 0x80;
  size_t pad = (rem <= 56) ? 56 - rem : 120 - rem;
  memset(tail + rem, 0, pad);
  uint64_t bits = (uint64_t)total * 8;
  for (int k = 0; k < 8; k++) tail[rem + pad + k] = (uint8_t)(bits >> (56 - k * 8));
  process(tail);
  for (int k = 0; k < 8; k++) {
    out[k*4]   = (uint8_t)(h[k] >> 24);
    out[k*4+1] = (uint8_t)(h[k] >> 16);
    out[k*4+2] = (uint8_t)(h[k] >> 8);
    out[k*4+3] = (uint8_t)h[k];
  }
}

inline std::string sha256_hex_upper(const std::string& in) {
  uint8_t d[32];
  sha256((const uint8_t*)in.data(), in.size(), d);
  char hex[65];
  for (int k = 0; k < 32; k++) sprintf(hex + k * 2, "%02X", d[k]);
  hex[64] = 0;
  return std::string(hex);
}

}  // namespace laap
