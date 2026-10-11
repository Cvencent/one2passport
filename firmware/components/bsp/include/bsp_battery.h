// components/bsp/include/bsp_battery.h
// CellWise CW2017 电量计:I2C 0x63,与 ES8311 共用总线。
//
// 本文件是 LEO RADIO 与 FoloOS 两版 BSP 的【API 并集】:
//   - 来自 LEO RADIO:保留 extern "C" 包裹(C++ 主程序需要)。
//   - 来自 FoloOS   :新增 soc_raw / level / config 三个诊断与兜底接口。
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化。内部会调 bsp_i2c_init()(幂等)。
// 芯片不应答时返回 ESP_ERR_NOT_FOUND —— 上层可据此在 UI 上标记该项不可用。
esp_err_t bsp_battery_init(void);

// 剩余电量百分比 0..100;读失败或寄存器未标定(高字节 > 100)返回 -1。
int bsp_battery_soc(void);

// SOC 寄存器原始 16bit 值;读失败返回 -1。
// 用于区分「芯片算法未就绪」与「I2C 总线错误」。
int bsp_battery_soc_raw(void);

// 可直接上屏的 0..100 电量。优先用芯片 SOC;SOC 无效时按实测电压曲线近似;
// 电压也读不到时返回 -1。给 UI 用,不适合作为电量算法的输入。
int bsp_battery_level(void);

// 电池电压 mV;读失败返回 -1。
int bsp_battery_mv(void);

// CONFIG 原始寄存器值;读失败返回 -1。0xF0=睡眠 / 0x30=复位 / 0x00=正常。
// 用于真机诊断电量计是否真的被唤醒。
int bsp_battery_config(void);

#ifdef __cplusplus
}
#endif
