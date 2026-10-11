// components/bsp/include/bsp_display.h
// ST7789P3 240x320 显示:SPI 面板初始化 + 厂商专属寄存器 + LEDC 背光调光。
//
// 本文件是 LEO RADIO 与 FoloOS 两版 BSP 的【API 并集】:
//   - 保留 LEO RADIO 的 extern "C" 与 panel/io 取句柄接口。
//   - 新增 FoloOS 的 bsp_display_enable()(息屏休眠时需要)。
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 SPI 总线、面板、厂商寄存器、背光 LEDC。成功后屏幕已上电但内容未定。
// 注意:本版会先接管背光并保持【熄灭】,等 LVGL 首帧真正渲染后再由 main 打开,
// 以此避免面板上电默认白底造成的开机白闪。
esp_err_t bsp_display_init(void);

// 取底层面板句柄。想直接 esp_lcd_panel_draw_bitmap 画,或接 LVGL 以外的 GUI 时用。
// 未初始化返回 NULL。
esp_lcd_panel_handle_t bsp_display_panel(void);

// 取底层 panel io 句柄(LVGL 接入需要)。未初始化返回 NULL。
esp_lcd_panel_io_handle_t bsp_display_io(void);

// 背光亮度 0..100(%)。LEDC PWM,0=全灭。
void bsp_display_backlight(uint8_t percent);

// 关闭/恢复 LCD 扫描输出。关闭前应先把背光调为 0,恢复时先开面板再开背光。
// 与背光是两条独立通路:背光控制看得见与否,本函数控制面板是否继续扫描。
void bsp_display_enable(bool enabled);

// ---------------------------------------------------------------------------
// LVGL 接入(可选层)。必须先 bsp_display_init() 成功后再调。
// 不想用 LVGL 的开发者可忽略本段,直接用 bsp_display_panel() 自己画。
// ---------------------------------------------------------------------------
// 前向声明:LVGL 里 lv_display_t 是 `typedef struct _lv_display_t lv_display_t;`,
// 故此处用 struct 形式即可,避免本头文件强行 include lvgl.h。
struct _lv_display_t;

// 启动 LVGL 与其渲染任务,返回 lv_display_t*。失败返回 NULL。
struct _lv_display_t *bsp_lvgl_init(void);

// LVGL 非线程安全:在【非 LVGL 任务】里操作任何 lv_* 对象前后必须加解锁。
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
