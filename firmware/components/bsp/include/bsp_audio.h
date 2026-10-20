// components/bsp/include/bsp_audio.h
// ES8311 音频 codec:I2C 走控制口(复用 bsp_i2c 的共享总线),I2S 走全双工数据口。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 codec 与 I2S。内部会调 bsp_i2c_init()(幂等),无需外部先调。
esp_err_t bsp_audio_init(void);

// 设置采样格式。同格式重复调用是廉价的(直接复用已打开的 codec)。
//
// ⚠ 这里有个必须绕开的坑:esp_codec_dev_open() 在 codec【已打开】时会直接返回 OK 且
//   【不重新配置采样率】。若不先 close,16kHz 播完再播 8kHz 会以 16k 时钟送出 ——
//   音调和速度都快一倍。故本函数在格式变化时先 close 再 open。
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch);

// Read the currently opened codec format. Returns ESP_ERR_INVALID_STATE
// before the first bsp_audio_set_format() call. Used by UI effects so they can
// follow the radio clock instead of closing/reopening the codec at 16 kHz.
esp_err_t bsp_audio_get_format(uint32_t *hz, uint8_t *bits, uint8_t *ch);

// 播放 / 录音。bytes 为字节数(16bit 单声道时 = 采样数 x 2)。
esp_err_t bsp_audio_write(const void *pcm, size_t bytes);
esp_err_t bsp_audio_read(void *pcm, size_t bytes);

// 输出音量 0..100(%)。
void bsp_audio_set_volume(uint8_t percent);

// Mute the DAC output without closing/reopening the codec. Used when audio
// ownership changes between the radio and UI effects: I2S may keep running,
// but the analog output is held at digital silence so DMA underruns cannot be
// heard as a click or a burst of electrical noise.
void bsp_audio_set_muted(bool muted);

#ifdef __cplusplus
}
#endif
