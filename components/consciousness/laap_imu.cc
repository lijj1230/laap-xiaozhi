#include "laap_imu.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdint.h>

namespace laap {

static const char* TAG = "laap_imu";

static constexpr uint8_t QMI_ADDR    = 0x6A;
static constexpr uint8_t REG_WHO_AM_I = 0x00;
static constexpr uint8_t REG_CTRL1    = 0x02;
static constexpr uint8_t REG_CTRL2    = 0x03;
static constexpr uint8_t REG_CTRL3    = 0x04;
static constexpr uint8_t REG_CTRL7    = 0x08;
static constexpr uint8_t REG_RESET    = 0x60;
static constexpr uint8_t REG_AX_L     = 0x35;

static i2c_master_dev_handle_t s_dev = nullptr;
static bool s_ok = false;
static float s_baseline = 1.0f;   // 静止时 |acc| ≈ 1g
static uint32_t s_miss = 0;       // 未就绪时的重试节流

static bool wr(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(s_dev, buf, 2, 20) == ESP_OK;
}

// Wire 的 requestFrom 语义 = repeated-start 读：transmit_receive 一次完成
static bool rd(uint8_t reg, uint8_t* buf, int n) {
  return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 20) == ESP_OK;
}

bool imuInit() {
  if (s_ok) return true;
  if (s_dev == nullptr) {
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_master_get_bus_handle(1, &bus) != ESP_OK || bus == nullptr) return false;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = QMI_ADDR;
    cfg.scl_speed_hz = 100000;   // 与音频编解码器共线，保守速率
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
      s_dev = nullptr;
      return false;
    }
  }
  uint8_t id = 0;
  for (int i = 0; i < 3; i++) {
    if (rd(REG_WHO_AM_I, &id, 1) && id == 0x05) break;
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  if (id != 0x05) {
    ESP_LOGW(TAG, "QMI8658 未应答（WHO_AM_I=0x%02X），世界模型 motion 置 0", id);
    return false;
  }
  wr(REG_RESET, 0xB0);
  vTaskDelay(pdMS_TO_TICKS(20));
  wr(REG_CTRL1, 0x40);       // 地址自增
  wr(REG_CTRL7, 0x03);       // 使能加速度+陀螺仪
  wr(REG_CTRL2, 0x95);       // ACC 4g 250Hz
  wr(REG_CTRL3, 0xD5);       // GYR 512dps 250Hz
  vTaskDelay(pdMS_TO_TICKS(30));
  uint8_t chk = 0;
  if (rd(REG_CTRL7, &chk, 1) && chk != 0x03) {
    ESP_LOGW(TAG, "CTRL7 回读 0x%02X != 0x03，配置未生效", chk);
    return false;
  }
  s_ok = true;
  ESP_LOGI(TAG, "QMI8658 就绪（I2C1 总线复用，世界模型 motion 通道激活）");
  return true;
}

bool imuReady() { return s_ok; }

float imuMotionLevel() {
  if (!s_ok) {
    // 惰性重试：每 36 拍（~6 分钟）再探一次，失败期间读数为 0
    if (++s_miss % 36 != 1) return 0;
    if (!imuInit()) return 0;
  }
  uint8_t b[6];
  if (!rd(REG_AX_L, b, 6)) return 0;
  float x = (int16_t)(b[0] | (b[1] << 8)) / 8192.0f;   // ±4g 量程
  float y = (int16_t)(b[2] | (b[3] << 8)) / 8192.0f;
  float z = (int16_t)(b[4] | (b[5] << 8)) / 8192.0f;
  float mag = sqrtf(x * x + y * y + z * z);
  s_baseline = s_baseline * 0.98f + mag * 0.02f;       // 慢自适应基线
  float v = fabsf(mag - s_baseline) * 6.0f;
  return v > 1 ? 1 : v;
}

}  // namespace laap
