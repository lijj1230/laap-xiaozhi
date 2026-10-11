#include "laap_fs.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_littlefs.h>

static const char* TAG = "laap_fs";
static const char* MOUNT = "/conscious";
static bool s_mounted = false;

namespace laap {

bool fs_begin() {
  if (s_mounted) return true;
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = MOUNT;
  conf.partition_label = "conscious";
  conf.format_if_mount_failed = true;   // 首次启动自动格式化（快照/记忆随后的 M2 再谈保护）
  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "conscious 分区挂载失败: %s", esp_err_to_name(err));
    return false;
  }
  s_mounted = true;
  size_t total = 0, used = 0;
  esp_littlefs_info("conscious", &total, &used);
  ESP_LOGI(TAG, "conscious 挂载成功：总 %uKB / 已用 %uKB", (unsigned)(total / 1024), (unsigned)(used / 1024));
  return true;
}

bool fs_mkdir(const std::string& path) {
  if (!s_mounted) return false;
  if (mkdir((MOUNT + path).c_str(), 0755) == 0) return true;
  return errno == EEXIST;   // 已存在=成功
}

char* fs_read_alloc(const std::string& path, size_t maxBytes, size_t& len) {
  len = 0;
  if (!s_mounted) return nullptr;
  FILE* f = fopen((MOUNT + path).c_str(), "rb");
  if (!f) return nullptr;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return nullptr; }
  long sz = ftell(f);
  if (sz <= 0) { fclose(f); return nullptr; }
  size_t n = (size_t)sz < maxBytes ? (size_t)sz : maxBytes;
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == nullptr) buf = (char*)malloc(n + 1);   // 无 PSRAM 芯片兜底
  if (buf == nullptr) { fclose(f); return nullptr; }
  fseek(f, 0, SEEK_SET);
  size_t rd = fread(buf, 1, n, f);
  fclose(f);
  buf[rd] = 0;
  len = rd;
  if (rd == 0) { heap_caps_free(buf); return nullptr; }
  return buf;
}

bool fs_read(const std::string& path, std::string& out) {
  if (!s_mounted) return false;
  FILE* f = fopen((MOUNT + path).c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

bool fs_write(const std::string& path, const std::string& content) {
  if (!s_mounted) return false;
  std::string tmp = MOUNT + path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) { ESP_LOGE(TAG, "写失败(开tmp): %s", path.c_str()); return false; }
  size_t w = fwrite(content.data(), 1, content.size(), f);
  fclose(f);
  if (w != content.size()) { remove(tmp.c_str()); return false; }
  std::string dst = MOUNT + path;
  // 先试覆盖 rename（littlefs 的 rename 是原子替换），失败才 remove——
  // 缩掉"remove 与 rename 之间掉电=文件全丢"的窗口（审计 2026-10-05）
  if (rename(tmp.c_str(), dst.c_str()) == 0) return true;
  remove(dst.c_str());
  return rename(tmp.c_str(), dst.c_str()) == 0;
}

bool fs_append(const std::string& path, const std::string& line) {
  if (!s_mounted) return false;
  FILE* f = fopen((MOUNT + path).c_str(), "ab");
  if (!f) return false;
  size_t w = fwrite(line.data(), 1, line.size(), f);
  fclose(f);
  return w == line.size();
}

bool fs_remove(const std::string& path) {
  return s_mounted && remove((MOUNT + path).c_str()) == 0;
}

bool fs_exists(const std::string& path) {
  if (!s_mounted) return false;
  FILE* f = fopen((MOUNT + path).c_str(), "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

size_t fs_size(const std::string& path) {
  if (!s_mounted) return 0;
  FILE* f = fopen((MOUNT + path).c_str(), "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  size_t sz = ftell(f);
  fclose(f);
  return sz;
}

char fs_last_char(const std::string& path) {
  if (!s_mounted) return 0;
  FILE* f = fopen((MOUNT + path).c_str(), "rb");
  if (!f) return 0;
  fseek(f, -1, SEEK_END);
  int c = fgetc(f);   // 空文件：fseek 到 -1 后读=EOF
  fclose(f);
  return (c == EOF) ? 0 : (char)c;
}

}  // namespace laap
