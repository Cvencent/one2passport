#pragma once

/* 本工程 main 是 C++(main.cc),而实现在 ui_sfx.c(C 文件)里。
 * 缺了下面的 extern "C",C++ 侧会按 mangled 名去找符号,链接期报
 * undefined reference to `ui_sfx_init(bool)`。FoloOS 原版入口是 C,
 * 所以它的头文件省了这层守卫。 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    UI_SFX_MOVE = 0,
    UI_SFX_CONFIRM,
    UI_SFX_APPROVAL_SPEECH,
    UI_SFX_DONE_SPEECH,
    UI_SFX_FAIL,
} ui_sfx_t;

void ui_sfx_init(bool audio_available);
void ui_sfx_set_volume(uint8_t percent);
void ui_sfx_play(ui_sfx_t effect);
void ui_sfx_play_wav(const uint8_t *wav, size_t wav_size);
void ui_sfx_suspend(bool suspended);

#ifdef __cplusplus
}
#endif
