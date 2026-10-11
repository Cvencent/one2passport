// main/radio_home.h —— plays/438「口袋百宝箱」风格的首页
#pragma once

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#else
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#endif

#include "lvgl.h"

#ifdef __cplusplus
#include "radio_player.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 建立 plays/438 风格首页(荧光绿 + 卡片网格)。
//
// **幂等**:重复调用只会在屏已存在时直接载入,不会重建。
// 这一点很关键 —— main.cc 里有 7 处会切回主界面,若每次都重建屏幕,
// 会与其它切屏动作来回打架,表现为屏幕闪烁。
lv_obj_t *radio_home_screen_create(void);

// 首页是否已经建好(供 main.cc 判断该不该重载)
bool radio_home_ready(void);

// ── 真实数据注入 ──────────────────────────────────────────────────
// 首页自己不去拉数据,由外部在拿到后灌进来。这样 UI 层不依赖
// radio_player / WifiManager 的内部状态,耦合低、易测。

#ifdef __cplusplus
// 电台信息(来自 radio_ui_set_station 的同一份数据)
void radio_home_set_station(const RadioStation &station);
#endif

// 城市名(来自 radio_ui_set_location)
void radio_home_set_location(const char *location);

// 播放状态(来自 radio_ui_set_playback),影响频段条与电台卡
void radio_home_set_playback(const char *text, bool playing);

// 网络状态(来自 radio_ui_set_network)
void radio_home_set_network(bool connected, const char *detail);

// Codex 工作状态(影响机器人显示)
// status: "空闲" / "工作中" / "任务完成" / "任务失败" / 或具体状态文本
void radio_home_set_codex_status(const char *status);

// 销毁首页屏幕与定时器(退出到其他页面时调用,防泄漏)
void radio_home_destroy(void);

#ifdef __cplusplus
}
#endif
