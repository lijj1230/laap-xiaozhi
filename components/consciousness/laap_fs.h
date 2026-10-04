#pragma once
// ============================================================
// LAAP-lite 意识组件 —— 存储垫片（Arduino LittleFS → esp_littlefs）
//   conscious 分区挂载在 /conscious；所有意识数据走 laap_fs_*，
//   不让上层感知 VFS 细节（M2 在 laap_life 初始化时挂载）。
//   语义与 LAAP 固件的 LittleFS 用法逐条对齐：原子写=写 .tmp 后 rename。
// ============================================================
#include <string>

namespace laap {

// 挂载 conscious 分区（幂等；挂载失败返回 false，上层打 ERROR 并禁用持久功能）
bool fs_begin();

bool fs_read(const std::string& path, std::string& out);   // 不存在=false
bool fs_write(const std::string& path, const std::string& content);          // 原子：tmp+rename
bool fs_append(const std::string& path, const std::string& line);            // 追加一行（含\n）
bool fs_remove(const std::string& path);
bool fs_exists(const std::string& path);
size_t fs_size(const std::string& path);

}  // namespace laap
