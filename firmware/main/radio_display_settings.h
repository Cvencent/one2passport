#pragma once

#include <cstddef>
#include <cstdint>

// Screen preferences are persisted in NVS.
//
// radio_display_note_activity() 刷新活动时间并点亮屏幕, 返回"屏幕之前是否已经亮着"。
// 注意这个返回值语义: true = 本次按键只是普通操作; false = 本次按键把息屏的屏幕唤醒了。
// 调用方若想"唤醒的那一下不触发切台", 应先调 radio_display_was_blanked() 判定,
// 不要写成 if (!radio_display_note_activity()) continue; —— 那样会把唤醒键吞掉,
// 表现为息屏后按一下没反应(2026-10-12 修复)。
bool radio_display_settings_init();
bool radio_display_note_activity();

// 屏幕当前是否处于息屏状态。
bool radio_display_was_blanked();

// 唤醒后延长活动计时, 避免刚点亮就二次息屏。
void radio_display_grace_after_wake(uint32_t ms);

uint8_t radio_display_brightness();
std::size_t radio_display_brightness_selection();
void radio_display_set_brightness_selection(std::size_t selection);

uint16_t radio_display_timeout_seconds();
std::size_t radio_display_timeout_selection();
void radio_display_set_timeout_selection(std::size_t selection);

// Codex 监控页在录音/播报期间要求屏幕保持常亮:息屏任务会跳过计时。
// 默认关闭,收音机路径完全不受影响。
void radio_display_set_keep_awake(bool keep_awake);

