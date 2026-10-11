// main/app.h —— Codex 监控页的对外接口(RADIO + CODEX 版)
//
// 【为什么是这个样子】
// 本文件源自 FoloOS(口袋百宝箱)的 main/app.h。那一版是"多应用菜单"架构:
// 菜单里有 编程伴侣 / 番茄专注 / 单词熊 / 系统设置 四个平级应用,各应用通过
// app_entry_t 向菜单注册。本工程要把 Codex 页接到 LEO RADIO 的收音机主界面上
// (长按 UP 切入),不再需要菜单,故:
//   - 删除 app_entry_t 与所有 app_pomodoro_* / app_word_bear_* / app_settings_*_key
//     声明(番茄钟与单词熊已按要求裁掉,设置页由 LEO 自己提供)。
//   - 删除 app_request_home():FoloOS 里由子应用请求回菜单用,本工程无人调用。
//   - app_settings_voice_mode() / app_settings_sfx_volume() 保留声明,但改为
//     在 main.cc 里直接实现(LEO 没有"语音模式"设置项,恒定走语音播报)。
//
// 【必须保留 extern "C"】
// 本工程 main 是 C++(main.cc),而这些 app_coding_* 接口实现在 C 文件里
// (app_coding.c / coding_bridge.c)。不加 extern "C" 会因 C++ name mangling
// 导致链接期找不到符号。FoloOS 原版 main.c 是 C 所以没写,这里必须补上。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp_button.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 提示音档位。LEO 收音机没有"语音模式"设置页,app_settings_voice_mode()
// 恒定返回 APP_VOICE_SPEECH —— 即 Codex 的通知一律走语音合成播报。
typedef enum {
    APP_VOICE_SILENT = 0,
    APP_VOICE_TONE,
    APP_VOICE_SPEECH,
} app_voice_mode_t;

// —— 电源/休眠钩子。由 main.cc 实现,转调 LEO 的 radio_display_settings ——
// app_power_activity(): 有交互发生,重置息屏计时(否则长任务跑着屏幕会灭)。
// app_power_hold_awake(): Codex 录音期间要求保持唤醒。
void app_power_activity(void);
void app_power_hold_awake(bool keep_awake);

// —— 主持人(Host)提供的设置读取。由 main.cc 实现 ——
app_voice_mode_t app_settings_voice_mode(void);
uint8_t app_settings_sfx_volume(void);

// —— Codex 页(原"编程伴侣")。实现在 app_coding.c ——
// 由 coding_bridge 在收到电脑端事件时回调,推送状态到界面。
void app_coding_set_audio_available(bool available);
void app_coding_bridge_connected(bool connected);
void app_coding_bridge_transcript(const char *text);
void app_coding_bridge_task_queued(const char *text);
void app_coding_bridge_task_status(const char *text);
void app_coding_bridge_approval(const char *question, const char *detail);
void app_coding_bridge_done(const char *text);
void app_coding_bridge_error(const char *text);
void app_coding_bridge_done_with_elapsed(const char *text, uint32_t elapsed_ms);
void app_coding_bridge_error_with_elapsed(const char *text, uint32_t elapsed_ms);
void app_coding_bridge_state(uint32_t running, uint32_t done, uint32_t failed);
void app_coding_result_identity(const char *thread_id, const char *turn_id);
// 项目/会话列表(电脑端分批下发,begin -> item* -> end)。
void app_coding_catalog_begin(const char *scope);
void app_coding_catalog_item(const char *scope, const char *id,
                             const char *title, const char *detail);
void app_coding_catalog_end(const char *scope);
// 选中会话。last_message 为该会话**最后一段**助手文本(可为 NULL);
// 只显示这一段, 不加载整段对话 —— 设备内存有限, 全量传入会卡。
void app_coding_task_selected(const char *title, const char *project,
                              const char *last_message);
void app_coding_capture_level(uint8_t level);
// OK 键此刻是否等于"开始/停止录音"语义(决定是否放确认音)。
bool app_coding_ok_is_record_action(void);
void app_coding_display_active(bool active);
// 进入/退出。enter 会新建并加载 LVGL 屏幕,exit 销毁之。
// 首页宠物四态(与 app_registry 的 codex_state_t 对应)
typedef enum {
    APP_CODEX_IDLE = 0,
    APP_CODEX_BUSY,
    APP_CODEX_DONE,
    APP_CODEX_FAIL,
    APP_CODEX_OFFLINE,
    APP_CODEX_ATTENTION,
} app_codex_state_t;

void app_coding_enter(void);
bool app_coding_full_ready(void);
app_codex_state_t app_coding_get_pet_state(void);
void app_coding_exit(void);
void app_coding_key(bsp_btn_t button, bsp_btn_ev_t event);
bool app_coding_back(void);

#ifdef __cplusplus
}
#endif
