#include "ui_sfx.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bsp_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// The bundled WAV assets are 16 kHz mono. Tones are synthesized at the codec's
// current rate so a UI effect never forces a 44.1/48 kHz radio stream through a
// close/reopen at 16 kHz. That reopen was audible as a short electrical hiss.
#define SFX_WAV_SAMPLE_RATE 16000U
#define SFX_FALLBACK_SAMPLE_RATE 16000U
#define SFX_CHUNK_SAMPLES 128

// Give the radio task time to observe the yield flag and stop writing. The I2S
// DMA queue is about 23 ms at 44.1 kHz; 20 ms is enough to hand over cleanly
// without inserting an obvious pause before the prompt.
#define SFX_RADIO_MUTE_SETTLE_MS 20

static QueueHandle_t s_queue;
static uint8_t s_volume = 50;
static bool s_available;
static volatile bool s_suspended;
static int64_t s_last_drop_log_us;
extern bool radio_is_playing_c(void);
/* 收音机在播放时, 让它临时停止写 I2S 而继续解码/缓冲, 把音频设备借给提示音。
 * 之前 ui_sfx 直接用 radio_is_playing_c() 丢弃提醒, 导致"播着电台时永远听不到
 * 任务完成提示"。 */
extern void radio_audio_yield(bool yield);

typedef struct {
    ui_sfx_t effect;
    const uint8_t *wav;
    size_t wav_size;
} sfx_request_t;

extern const uint8_t approval_wav_start[] asm("_binary_approval_wav_start");
extern const uint8_t approval_wav_end[] asm("_binary_approval_wav_end");
extern const uint8_t done_wav_start[] asm("_binary_done_wav_start");
extern const uint8_t done_wav_end[] asm("_binary_done_wav_end");

static uint32_t read_le32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static bool find_wav_pcm(const uint8_t *wav, size_t wav_size,
                         const uint8_t **pcm, size_t *pcm_size)
{
    if (wav_size < 12 || memcmp(wav, "RIFF", 4) != 0 ||
        memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }

    size_t offset = 12;
    while (offset + 8 <= wav_size) {
        uint32_t chunk_size = read_le32(wav + offset + 4);
        size_t data_offset = offset + 8;
        if (chunk_size > wav_size - data_offset) {
            return false;
        }
        if (memcmp(wav + offset, "data", 4) == 0) {
            *pcm = wav + data_offset;
            *pcm_size = chunk_size;
            return true;
        }
        offset = data_offset + chunk_size + (chunk_size & 1U);
    }
    return false;
}

static void play_speech(const uint8_t *wav, size_t wav_size, uint32_t output_rate)
{
    const uint8_t *pcm = NULL;
    size_t pcm_size = 0;
    if (!find_wav_pcm(wav, wav_size, &pcm, &pcm_size) || pcm_size < 2) return;

    if (output_rate == 0) output_rate = SFX_FALLBACK_SAMPLE_RATE;
    if (output_rate == SFX_WAV_SAMPLE_RATE) {
        while (pcm_size > 0) {
            size_t count = pcm_size > 512 ? 512 : pcm_size;
            if (bsp_audio_write(pcm, count) != ESP_OK) return;
            pcm += count;
            pcm_size -= count;
        }
        return;
    }

    // Linear resampling in 16.16 fixed point. The source assets are 16-bit
    // mono, and this keeps the prompt at the radio's current clock without
    // allocating a second full-size PCM buffer.
    const int16_t *src = (const int16_t *)pcm;
    const size_t src_count = pcm_size / sizeof(int16_t);
    if (src_count == 0) return;
    const uint32_t step_q16 =
        (SFX_WAV_SAMPLE_RATE * 65536U) / output_rate;
    uint64_t pos_q16 = 0;
    const uint64_t end_q16 = (uint64_t)src_count << 16;
    int16_t out[SFX_CHUNK_SAMPLES];

    while (pos_q16 < end_q16) {
        size_t count = 0;
        while (count < SFX_CHUNK_SAMPLES && pos_q16 < end_q16) {
            const size_t index = (size_t)(pos_q16 >> 16);
            const int32_t frac = (int32_t)(pos_q16 & 0xFFFFU);
            const int32_t first = src[index];
            const int32_t second = src[index + 1 < src_count ? index + 1 : index];
            const int32_t interpolated =
                first + (int32_t)(((int64_t)(second - first) * frac) >> 16);
            out[count++] = (int16_t)interpolated;
            pos_q16 += step_q16;
        }
        if (bsp_audio_write(out, count * sizeof(out[0])) != ESP_OK) return;
    }
}

static void tone(uint16_t frequency_hz, uint16_t duration_ms, uint32_t sample_rate)
{
    int16_t samples[SFX_CHUNK_SAMPLES];
    if (sample_rate == 0) sample_rate = SFX_FALLBACK_SAMPLE_RATE;
    const int total = (int)(((uint64_t)sample_rate * duration_ms) / 1000U);
    int envelope = (int)(((uint64_t)sample_rate * 3U) / 1000U);
    if (envelope < 1) envelope = 1;
    uint32_t phase = 0;
    int emitted = 0;

    while (emitted < total) {
        int count = total - emitted;
        if (count > SFX_CHUNK_SAMPLES) count = SFX_CHUNK_SAMPLES;
        for (int i = 0; i < count; ++i) {
            int position = emitted + i;
            int edge = position;
            if (total - 1 - position < edge) edge = total - 1 - position;
            int amplitude = 2600;
            if (edge < envelope) amplitude = amplitude * edge / envelope;

            phase += frequency_hz;
            while (phase >= sample_rate) phase -= sample_rate;
            samples[i] = phase < sample_rate / 2 ? amplitude : -amplitude;
        }
        if (bsp_audio_write(samples, (size_t)count * sizeof(samples[0])) != ESP_OK) {
            return;
        }
        emitted += count;
    }
}

static void silence(uint16_t duration_ms, uint32_t sample_rate)
{
    int16_t samples[SFX_CHUNK_SAMPLES] = {0};
    if (sample_rate == 0) sample_rate = SFX_FALLBACK_SAMPLE_RATE;
    int remaining = (int)(((uint64_t)sample_rate * duration_ms) / 1000U);
    while (remaining > 0) {
        int count = remaining > SFX_CHUNK_SAMPLES ? SFX_CHUNK_SAMPLES : remaining;
        if (bsp_audio_write(samples, (size_t)count * sizeof(samples[0])) != ESP_OK) {
            return;
        }
        remaining -= count;
    }
}

static void sfx_task(void *argument)
{
    (void)argument;
    sfx_request_t request;
    for (;;) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        if (!s_available || s_suspended || s_volume == 0) {
            // 2026-10-22: 这里原来直接 continue, 提示音被丢掉时**一条日志都没有**,
            // 用户报"息屏后 Codex 提示音没了"时完全查不出是没触发、被挂起、
            // 还是音量被清 0。现在把丢弃原因打出来(限频, 避免刷屏)。
            const int64_t now = esp_timer_get_time();
            if (now - s_last_drop_log_us > 2000000LL) {
                s_last_drop_log_us = now;
                ESP_LOGW("ui_sfx",
                         "drop effect=%d available=%d suspended=%d vol=%u",
                         (int)request.effect, (int)s_available,
                         (int)s_suspended, (unsigned)s_volume);
            }
            continue;
        }

        // 收音机正在播放时, 让它让出 I2S(继续解码/攒缓冲), 播完再交还。
        // 否则"任务完成提示"会被正在播放的电台吞掉。
        const bool yield_radio = radio_is_playing_c();
        uint32_t output_rate = SFX_FALLBACK_SAMPLE_RATE;
        if (yield_radio) {
            uint32_t active_rate = 0;
            uint8_t active_bits = 0;
            uint8_t active_channels = 0;
            if (bsp_audio_get_format(&active_rate, &active_bits, &active_channels) == ESP_OK &&
                active_bits == 16 && active_channels == 1 &&
                active_rate >= 8000U && active_rate <= 48000U) {
                output_rate = active_rate;
            }
            // Mute before the radio yields. If the radio is mid-frame, the
            // mute hides the tail of the frame it already handed to the DMA.
            bsp_audio_set_muted(true);
            radio_audio_yield(true);
            vTaskDelay(pdMS_TO_TICKS(SFX_RADIO_MUTE_SETTLE_MS));
        }

        // 2026-10-22: 设备空闲一段时间后堆很紧(largest 常见 1~4KB),而
        // bsp_audio_set_format() 内部可能要 close+open 编解码器。失败时原来
        // 也是静默 continue —— 提示音就这么没了。现在重试一次并把失败原因
        // 打出来, 让"息屏后没声音"能在串口上一眼定位。
        esp_err_t fmt = bsp_audio_set_format(output_rate, 16, 1);
        if (fmt != ESP_OK) {
            ESP_LOGW("ui_sfx", "set_format(%luHz) failed: %s, retrying",
                     (unsigned long)output_rate, esp_err_to_name(fmt));
            vTaskDelay(pdMS_TO_TICKS(20));
            fmt = bsp_audio_set_format(output_rate, 16, 1);
        }
        if (fmt != ESP_OK) {
            ESP_LOGW("ui_sfx", "set_format(%luHz) failed: %s, dropping effect=%d",
                     (unsigned long)output_rate, esp_err_to_name(fmt),
                     (int)request.effect);
            if (yield_radio) radio_audio_yield(false);
            else bsp_audio_set_muted(false);
            continue;
        }
        bsp_audio_set_volume(s_volume);
        // The radio was already opened at this rate, so set_format() is a
        // no-op and the codec never closes. Flush the last DMA frame as
        // digital silence, then unmute for the effect itself.
        if (yield_radio) silence(30, output_rate);
        bsp_audio_set_muted(false);
        ESP_LOGI("ui_sfx", "play effect=%d yield_radio=%d rate=%lu vol=%u",
                 (int)request.effect, (int)yield_radio,
                 (unsigned long)output_rate, (unsigned)s_volume);

        if (request.wav != NULL) {
            play_speech(request.wav, request.wav_size, output_rate);
        } else if (request.effect == UI_SFX_MOVE) {
            tone(1180, 28, output_rate);
        } else if (request.effect == UI_SFX_CONFIRM) {
            tone(920, 32, output_rate);
            silence(8, output_rate);
            tone(1480, 48, output_rate);
        } else if (request.effect == UI_SFX_APPROVAL_SPEECH) {
            play_speech(approval_wav_start,
                        (size_t)(approval_wav_end - approval_wav_start), output_rate);
        } else if (request.effect == UI_SFX_DONE_SPEECH) {
            play_speech(done_wav_start,
                        (size_t)(done_wav_end - done_wav_start), output_rate);
        } else if (request.effect == UI_SFX_FAIL) {
            tone(660, 130, output_rate);
            silence(45, output_rate);
            tone(330, 220, output_rate);
        }
        if (yield_radio) {
            // The radio owns the format again. Same rate, so its restore is a
            // no-op; it unmutes only once it writes a fresh frame.
            radio_audio_yield(false);
        }
    }
}

void ui_sfx_init(bool audio_available)
{
    s_available = audio_available;
    if (!audio_available || s_queue != NULL) return;
    s_queue = xQueueCreate(4, sizeof(sfx_request_t));
    if (s_queue != NULL) {
        xTaskCreate(sfx_task, "ui_sfx", 3072, NULL, 4, NULL);
    }
}

void ui_sfx_set_volume(uint8_t percent)
{
    s_volume = percent > 100 ? 100 : percent;
}

void ui_sfx_suspend(bool suspended)
{
    s_suspended = suspended;
}

void ui_sfx_play(ui_sfx_t effect)
{
    if (s_queue == NULL || s_suspended || s_volume == 0) return;
    const sfx_request_t request = { .effect = effect };
    (void)xQueueSend(s_queue, &request, 0);
}

void ui_sfx_play_wav(const uint8_t *wav, size_t wav_size)
{
    if (s_queue == NULL || s_suspended || s_volume == 0 ||
        wav == NULL || wav_size == 0) return;
    const sfx_request_t request = {
        .effect = UI_SFX_CONFIRM,
        .wav = wav,
        .wav_size = wav_size,
    };
    (void)xQueueSend(s_queue, &request, 0);
}
