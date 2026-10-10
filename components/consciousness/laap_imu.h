#pragma once
// ============================================================
// LAAP-lite 立创实战派板载 QMI8658 六轴 IMU（世界模型传感器）
// 移植自 laap-esp32 laap_imu.cpp（寄存器时序/基线算法 1:1），
// Wire 调用换成 IDF i2c_master；复用宿主板级已建的 I2C 端口 1
// 总线（SDA=1/SCL=2，与 ES8311/ES7210/PCA9557 共线，0x6A 不冲突，
// 总线带驱动锁并发安全）。初始化时序来自官方例程 02-attitude。
// ============================================================
namespace laap {

bool imuInit();          // 成功返回 true（找不到芯片不影响主流程）
bool imuReady();         // 已初始化且配置生效
float imuMotionLevel();  // 0..1 加速度扰动强度（静止≈0；未就绪自动惰性重试）

}  // namespace laap
