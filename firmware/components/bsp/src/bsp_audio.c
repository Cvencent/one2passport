// components/bsp/src/bsp_audio.c
// 移植自 trae_card/components/platform/platform_esp32/src/audio_es8311.c
#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_audio";

static esp_codec_dev_handle_t s_dev;
static i2s_chan_handle_t      s_tx, s_rx;
// 记录当前已打开的格式,用于判断"要不要 close 重开"(见头文件里的坑说明)。
static uint32_t s_hz;
static uint8_t  s_bits, s_ch;
static bool     s_opened;
// Keep the requested volume in the driver. esp_codec_dev_open() re-applies
// its cached volume, but the codec is rebuilt when the sample format changes,
// and the new codec starts from 0. Remembering the value here lets every open
// restore the user's level instead of silently muting until the next write.
static uint8_t  s_volume = 55;
// Output ownership changes (radio <-> UI effect) must not leave the DAC
// unmuted while the I2S DMA queue drains. Keep the last requested mute state
// here so every open restores it together with the volume.
static bool     s_muted;
// 串行化 codec 的开关与读写。
//
// 【为什么必需】收音机(radio_player)与 Codex 页(ui_sfx 提示音、coding_bridge
// 录音/播报)是两条独立任务,会并发调用本组件。而 bsp_audio_set_format() 内部是
// "close → i2s enable → open" 的复合序列,中间态不可用:若另一任务此刻正在
// bsp_audio_write,就会往已关闭的 codec 写,或与 open 中的重配交错,
// 表现为爆音、丢帧,甚至 esp_codec_dev 内部状态错乱。
// 用递归锁:none of 这些函数会相互调用,但递归锁能避免日后重构时误嵌套死锁。
static SemaphoreHandle_t s_lock;

static inline void audio_lock(void) {
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static inline void audio_unlock(void) {
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

// Prime the DAC path with a short block of digital silence after the codec is
// opened. Station changes force close+open because the sample rate changes;
// on this directly-coupled ES8311 output the open transient is audible as a
// brief tick/hiss. A silent write lets the DMA/PA settle before real PCM.
static void prime_output(void) {
    if (!s_dev || !s_opened) return;
    int16_t silence[128] = {};
    (void)esp_codec_dev_write(s_dev, silence, sizeof(silence));
}

static esp_err_t i2s_full_duplex_init(void) {
    // DMA 缓冲决定"抗网络抖动"能力, 但**必须省着用**: C3 内部 DMA RAM 很紧,
    // 且收音机 MP3 解码器需要 ~19KB 连续块, 挤占会导致
    // "ESP_MP3_DEC: Fail to init MP3 decoder ret 10"(实测 8x512=16KB 就触发)。
    // 4x256: 每帧 4B(stereo16bit) -> 4*256 = 1024 帧, 约 23ms @44.1kHz。
    // 2026-10-13: 4x128 只有 ~12ms, 而播放任务两次 bsp_audio_write 之间实测常有
    // 12~17ms(解码+电平+统计的 CPU 时间), DMA 刚好被抽干 -> 听感一卡一卡。
    // 折中取 4x256(约 23ms): 只比原来的 4x128 多占约 4KB DMA(TX+RX 各一份),
    // 又比 6x240(33ms, 多 7.4KB)省 3.4KB —— 播放时 DMA 池本就只剩几百字节,
    // 加太多会让首次连接直接 wifi:mem fail。
    i2s_chan_config_t chan = {
        .id = BSP_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 4,
        .dma_frame_num = 256,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t e = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(e)); return e; }

    // 这里的采样率只用于建通道;实际速率由 esp_codec_dev_open() 按需重配。
    i2s_std_config_t std = {
        .clk_cfg = {
            .sample_rate_hz = 16000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_BCLK, .ws = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT, .din = BSP_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if ((e = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx 初始化失败: %s", esp_err_to_name(e)); return e;
    }
    if ((e = i2s_channel_init_std_mode(s_rx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx 初始化失败: %s", esp_err_to_name(e)); return e;
    }
    // esp_codec_dev_open 内部重配前会先 i2s_channel_disable,而 disable 要求通道处于
    // RUNNING;刚 init 的通道是 READY,会打一条 "channel has not been enabled yet" 错误日志。
    // 这里先 enable 一次让那次 disable 合法(此时 codec 未配,不出声)。
    i2s_channel_enable(s_tx);
    i2s_channel_enable(s_rx);
    return ESP_OK;
}

esp_err_t bsp_audio_init(void) {
    if (s_dev) return ESP_OK;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) { ESP_LOGE(TAG, "音频互斥锁创建失败"); return ESP_ERR_NO_MEM; }
    }
    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = BSP_I2C_PORT,
        .addr = BSP_I2C_ES8311_ADDR << 1,   // 该接口要 8 位地址形式
        .bus_handle = bsp_i2c_bus(),
    });
    if (!ctrl) {
        ESP_LOGE(TAG, "ES8311 控制口创建失败 —— 用 bsp_i2c_scan() 确认 0x%02X 是否应答;"
                      "检查 SDA=GPIO%d / SCL=GPIO%d 接线与 codec 供电",
                 BSP_I2C_ES8311_ADDR, BSP_I2C_SDA, BSP_I2C_SCL);
        return ESP_FAIL;
    }

    if ((e = i2s_full_duplex_init()) != ESP_OK) return e;

    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = BSP_I2S_PORT, .tx_handle = s_tx, .rx_handle = s_rx,
    });
    if (!data) { ESP_LOGE(TAG, "I2S 数据口创建失败"); return ESP_FAIL; }

    const audio_codec_if_t *codec = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if     = ctrl,
        .gpio_if     = audio_codec_new_gpio(),
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH,   // 收音机只播,但 Codex 页要录音
        .pa_pin      = BSP_I2S_PA_CTRL,
        .pa_reverted = false,
        .master_mode = false,          // MCU I2S 为 master,codec 为 slave
        .use_mclk    = true,
        // ⚠ BSP_I2S_PA_CTRL = -1, 即**无独立功放控制引脚**(DAC 直驱或板载固定
        //   增益)。原写 pa_voltage=5.0(按外部 5V PA 算), 会让驱动算出的 hw_gain
        //   偏低 3.6dB -> db_value 被抬高 -> DAC 多放大, 码流接近满幅时削波
        //   ("电流声/破音")。改为与 codec_dac_voltage 同源 3.3V: hw_gain=0, 不再
        //   做无依据的增益补偿, 保留数字域的干净衰减。
        .hw_gain     = { .pa_voltage = 3.3f, .codec_dac_voltage = 3.3f },
        // ⚠ 单声道纯麦克风录音必须为 true。false 会让驱动写 REG44=0x58 进入
        //   ADCL+DACR 参考模式,单声道读到的那一路是 DAC 参考 → 【录音恒为 0】。
        .no_dac_ref  = true,
    });
    if (!codec) { ESP_LOGE(TAG, "es8311_codec_new 失败"); return ESP_FAIL; }

    s_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec,
        .data_if  = data,
    });
    if (!s_dev) { ESP_LOGE(TAG, "esp_codec_dev_new 失败"); return ESP_FAIL; }

    ESP_LOGI(TAG, "ES8311 就绪");
    return ESP_OK;
}

esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    audio_lock();
    if (s_opened && s_hz == hz && s_bits == bits && s_ch == ch) {   // 同格式复用
        audio_unlock();
        return ESP_OK;
    }

    if (s_opened) {
        esp_codec_dev_close(s_dev);
        s_opened = false;
        // close 把 I2S 通道退回 READY,而接下来的 open 内部又会 disable 一次 →
        // 会打 "channel has not been enabled yet"。补一次 enable 让它合法。
        if (s_tx) i2s_channel_enable(s_tx);
        if (s_rx) i2s_channel_enable(s_rx);
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel = ch,
        .channel_mask = ch == 1 ? ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) : 0,
        .sample_rate = hz,
        .mclk_multiple = 0,          // 0 → 驱动按默认 256xfs 取 MCLK
    };
    int r = esp_codec_dev_open(s_dev, &fs);
    if (r != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open 失败: %d", r);
        audio_unlock();
        return ESP_FAIL;
    }

    // ⚠ open 之后【不要】手动覆写 ES8311 的时钟分频寄存器(REG01~06):
    //   驱动已按采样率与 MCLK 精确算好,覆写会导致 ADC/DAC 时序错乱、录音回放全是杂音。
    //   这里只设麦克风模拟 PGA 增益。
    //
    // ⚠⚠ 30dB -> 40dB(2026-10-12 修复"小智一直只识别出'我'"):
    //   实测上行音频 RMS 只有 147(正常说话应在 1000 以上), 云端 ASR 从这份
    //   近乎静音的信号里只能挑出零星噪声, 于是无论说多久都识别成"我"。
    //   ES8311 的模拟 PGA 上限约 40dB, 留 0dB 余量给驱动内部做防削波处理。
    //   麦克风离喇叭较远时 30dB 明显不够(设备是桌面摆件, 人站在旁边说话)。
    esp_codec_dev_set_in_gain(s_dev, 40.0f);
    esp_codec_dev_set_out_vol(s_dev, s_volume);
    esp_codec_dev_set_out_mute(s_dev, s_muted);

    s_opened = true; s_hz = hz; s_bits = bits; s_ch = ch;
    prime_output();
    ESP_LOGI(TAG, "codec 打开 %luHz/%ubit/%uch", (unsigned long)hz, bits, ch);
    audio_unlock();
    return ESP_OK;
}

esp_err_t bsp_audio_get_format(uint32_t *hz, uint8_t *bits, uint8_t *ch) {
    if (!hz || !bits || !ch || !s_dev || !s_opened) return ESP_ERR_INVALID_STATE;
    audio_lock();
    *hz = s_hz;
    *bits = s_bits;
    *ch = s_ch;
    audio_unlock();
    return ESP_OK;
}

esp_err_t bsp_audio_write(const void *pcm, size_t bytes) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    audio_lock();
    esp_err_t result =
        esp_codec_dev_write(s_dev, (void *)pcm, bytes) == 0 ? ESP_OK : ESP_FAIL;
    audio_unlock();
    return result;
}

esp_err_t bsp_audio_read(void *pcm, size_t bytes) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    audio_lock();
    esp_err_t result = esp_codec_dev_read(s_dev, pcm, bytes) == 0 ? ESP_OK : ESP_FAIL;
    audio_unlock();
    return result;
}

void bsp_audio_set_volume(uint8_t percent) {
    s_volume = percent > 100 ? 100 : percent;
    if (!s_dev) return;
    audio_lock();
    esp_codec_dev_set_out_vol(s_dev, s_volume);
    audio_unlock();
}

void bsp_audio_set_muted(bool muted) {
    s_muted = muted;
    if (!s_dev || !s_opened) return;
    audio_lock();
    esp_codec_dev_set_out_mute(s_dev, muted);
    audio_unlock();
}
