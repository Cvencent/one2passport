#include "ui_sfx.h"

#include "esp_log.h"

#include <stddef.h>
#include <string.h>

#include "bsp_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SFX_SAMPLE_RATE 16000
#define SFX_CHUNK_SAMPLES 128

static QueueHandle_t s_queue;
static uint8_t s_volume = 50;
static bool s_available;
static volatile bool s_suspended;
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

// 让位播放期间收音机仍然"在播放", 不能用 radio_is_playing_c() 判断中断。
static volatile bool s_radio_yielded;
static void play_speech(const uint8_t *wav, size_t wav_size)
{
    const uint8_t *pcm = NULL;
    size_t remaining = 0;
    if (!find_wav_pcm(wav, wav_size, &pcm, &remaining)) return;

    while (remaining > 0) {
        if (s_radio_yielded) return;
        size_t count = remaining > 512 ? 512 : remaining;
        if (bsp_audio_write(pcm, count) != ESP_OK) return;
        pcm += count;
        remaining -= count;
    }
}

static void tone(uint16_t frequency_hz, uint16_t duration_ms)
{
    int16_t samples[SFX_CHUNK_SAMPLES];
    const int total = SFX_SAMPLE_RATE * duration_ms / 1000;
    const int envelope = SFX_SAMPLE_RATE * 3 / 1000;
    uint32_t phase = 0;
    int emitted = 0;

    while (emitted < total) {
        if (s_radio_yielded) return;
        int count = total - emitted;
        if (count > SFX_CHUNK_SAMPLES) count = SFX_CHUNK_SAMPLES;
        for (int i = 0; i < count; ++i) {
            int position = emitted + i;
            int edge = position;
            if (total - 1 - position < edge) edge = total - 1 - position;
            int amplitude = 2600;
            if (edge < envelope) amplitude = amplitude * edge / envelope;

            phase += frequency_hz;
            if (phase >= SFX_SAMPLE_RATE) phase -= SFX_SAMPLE_RATE;
            samples[i] = phase < SFX_SAMPLE_RATE / 2 ? amplitude : -amplitude;
        }
        if (bsp_audio_write(samples, (size_t)count * sizeof(samples[0])) != ESP_OK) {
            return;
        }
        emitted += count;
    }
}

static void silence(uint16_t duration_ms)
{
    int16_t samples[SFX_CHUNK_SAMPLES] = {0};
    int remaining = SFX_SAMPLE_RATE * duration_ms / 1000;
    while (remaining > 0) {
        if (s_radio_yielded) return;
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
        if (!s_available || s_suspended || s_volume == 0) continue;
        // 收音机正在播放时, 让它让出 I2S(继续解码/攒缓冲), 播完再交还。
        // 否则"任务完成提示"会被正在播放的电台吞掉。
        const bool yield_radio = radio_is_playing_c();
        if (yield_radio) {
            s_radio_yielded = true;
            radio_audio_yield(true);
            vTaskDelay(pdMS_TO_TICKS(60));
        }
        if (bsp_audio_set_format(SFX_SAMPLE_RATE, 16, 1) != ESP_OK) {
            if (yield_radio) radio_audio_yield(false);
            s_radio_yielded = false;
            continue;
        }
        bsp_audio_set_volume(s_volume);
        ESP_LOGI("ui_sfx", "play effect=%d yield_radio=%d vol=%u",
                 (int)request.effect, (int)yield_radio, (unsigned)s_volume);

        if (request.wav != NULL) {
            play_speech(request.wav, request.wav_size);
        } else if (request.effect == UI_SFX_MOVE) {
            tone(1180, 28);
        } else if (request.effect == UI_SFX_CONFIRM) {
            tone(920, 32);
            silence(8);
            tone(1480, 48);
        } else if (request.effect == UI_SFX_APPROVAL_SPEECH) {
            play_speech(approval_wav_start,
                        (size_t)(approval_wav_end - approval_wav_start));
        } else if (request.effect == UI_SFX_DONE_SPEECH) {
            play_speech(done_wav_start,
                        (size_t)(done_wav_end - done_wav_start));
        } else if (request.effect == UI_SFX_FAIL) {
            tone(660, 130);
            silence(45);
            tone(330, 220);
        }
        if (yield_radio) {
            // 交还音频设备, 收音机下一帧继续写 I2S。
            s_radio_yielded = false;
            radio_audio_yield(false);
            vTaskDelay(pdMS_TO_TICKS(20));
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
