#include "radio_player.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "radio_ui.h"
#include "radio_stream_buffer.h"
#include "radio_mp3_header.h"
#include "radio_retry.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <strings.h>

namespace {

constexpr char kTag[] = "radio_player";
// ⚠ 输入块大小必须保持 1024: 实测改成 4096 会让 esp_audio_simple_dec_process
// 连续报 "ESP_MP3_DEC: Fail to init MP3 decoder ret 10" + "Decode error reach
// limited 20"(即使内存充裕也不解), 说明该库对单次投喂的块大小敏感。
// 卡顿问题改用 I2S DMA 缓冲 + 任务优先级 + HTTP 接收缓冲解决, 不动这里。
constexpr std::size_t kInputSize = 1024;
// 输出缓冲 = MP3 最大一帧 PCM(4608B) + 预留给网络块的尾部。
// MPEG Layer III 每声道最多 1152 样本, 16bit 立体声即 1152*2ch*2B = 4608B。
constexpr std::size_t kPcmCapacity = 4608;
// 尾部 2KB = 2 个网络块(与输出缓冲共享), 开机随解码器池一起预分配。
// 其余网络块在连接时按剩余堆现分配(ring 是生产者/消费者之间的短命缓冲,
// 不必全部常驻); kNetworkBlocks 仍是上限 6。
//
// 取 2 而不是 6 的原因(2026-10-12 实测): 输出缓冲每多 1KB, 开机就少
// 1KB 可用堆, 到 post_coding_bridge 时 largest 只剩 4~6KB —— 而 HTTP client
// 本身还要分配 ~1KB 连续块, 预分配过头反而 "Allocation failed"。
//
// ⚠⚠ 2026-10-12 回归修复: 上面 kPcmCapacity 曾被写成 kOutputSize, 于是
// kSharedNetworkBlocks 恒等于 0, "与输出缓冲共享" 被静默关闭 —— 播放时 6 个
// 网络块全部现分配 malloc, 而那正是堆碎到 largest<2.4KB 的时刻, UI 重绘
// 拿不到内存 -> LVGL 卡死 -> task_wdt 每 5s 触发一次 -> CPU 满载, 就是
// "一进收音机就发烫" 的根因。static_assert 就是为了让它再也回不来。
constexpr std::size_t kSharedNetworkBlocks = 2;
constexpr std::size_t kOutputSize = kPcmCapacity + kSharedNetworkBlocks * kInputSize;
// 编译期自检: 上面三行一旦被改回 kPcmCapacity == kOutputSize, 共享块会静默
// 归零(播放期改走 malloc, 挤死 UI 缓冲 -> LVGL 卡死 -> 满载发热)。这里让它
// 直接编译失败, 而不是等到真机复现。
static_assert(kPcmCapacity >= 4608,
              "MP3 full stereo frame needs 1152*2ch*2B = 4608B");
static_assert(kSharedNetworkBlocks >= 1,
              "network blocks must share the output pool (see comment above)");
static_assert(kPcmCapacity + kSharedNetworkBlocks * kInputSize <= kOutputSize,
              "shared network blocks must fit inside the output pool");
// MP3 解码器初始化所需的**连续**内部堆(实测约 19~20KB)。
// 用于在解码失败时判断"重试是否有意义": largest 低于此值时重试必然失败,
// 必须直接交外层退避, 否则会以 ~5 次/秒刷 "Fail to init" 并烧 CPU(实测发热)。
constexpr std::size_t kDecoderMinBlock = 20 * 1024;
constexpr std::size_t kLevelCount = 18;
// 电台列表容量。兜底表 10 项(7 中文 + 3 国际)已占大半, 再给目录搜索结果
// 留几个位置。RadioStation ≈ 418B, 14 项 ≈ 5.9KB 静态(比 12 项多 836B,
// 在 C3 上可接受)。
constexpr std::size_t kMaxStations = 14;
// Consecutive silent failures tolerated before moving to the next station.
constexpr uint8_t kMaxFailedAttempts = 3;

// These national streams are not tied to one local transmitter, so they carry
// no dial frequency and the needle spreads them across the scale by position.
// 全部为"实测可播"的流(2026-10-09 用 PC 端逐条验证: http 明文可达 +
// 首字节为标准 MPEG 帧同步字 0xFF 或 ID3 标签头, 编码均为 MP3 Layer III)。
// 教训: qtfm/qingting.fm 的 64k.mp3 实测返回随机字节(Content-Type 谎报
// audio/mpeg), 是不可解码的加密/防盗链流, 已从兜底表移除。
//
// 【顺序很重要】中文台排最前(用户主要听中文), 国际台排后。合并时本表整体
// 排在目录搜索结果之前, 所以列表第 1/2 位永远是已验证可播的中文台 ——
// 避免用户一切台就是坏源(此前"频道1和2播放不了"即目录结果里的加密流占位)。
const RadioStation kFallbackStations[] = {
    // ---- 中文台(实测 http 明文可播) ----
    {"RFI 中文", "法国国际广播 · 中文",
     "http://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64", 0},
    {"中文新闻台", "每5分钟新闻 · 中文",
     "http://replaynewszh.ice.infomaniak.ch/replaynewszh-128.mp3", 0},
    {"FM891 音乐台", "中文流行 · 在线",
     "http://live.fm891.net/96k.aac", 0},
    {"Fred 电影电台", "电影文化 · 普通话",
     "http://ice1.somafm.com/dronezone-128-mp3", 0},
    // ⚠ 动漫音乐台(pool.anison.fm:9000) 已移除: 实测该源码率/编码不稳定,
    // 播放时持续一卡一卡(用户明确要求删除)。
    {"亚洲音乐台", "亚洲流行 · 中文",
     "http://ice1.somafm.com/secretagent-128-mp3", 0},
    {"怀旧金曲", "乐享音乐 · 中文",
     "http://cn.bigbigmix.com/audio/stream", 0},
    // ---- 国际台(氛围/爵士, 音质稳定, 作备选) ----
    {"SomaFM 环境", "轻松氛围 · 网络直播",
     "http://ice1.somafm.com/lush-128-mp3", 0},
    {"爵士电台", "Smooth Jazz · 网络直播",
     "http://ice1.somafm.com/sonicuniverse-128-mp3", 0},
    {"复古频道", "Retro · 网络直播",
     "http://ice1.somafm.com/u80s-128-mp3", 0},
};

std::atomic<bool> s_network_connected{false};
std::atomic<bool> s_wanted_playing{false};
std::atomic<std::size_t> s_station_index{0};
std::atomic<std::size_t> s_station_count{0};
static bool s_catalog_cached;
std::atomic<uint8_t> s_volume{55};
std::atomic<uint32_t> s_generation{1};
std::atomic<bool> s_stream_active{false};
// 最近一次 stream_station 失败的原因是否为"内存不足"(而非电台本身坏)。
// 播放循环据此决定: 真坏台 -> 换台; 内存不足 -> 换台也没用, 用长退避等堆恢复。
std::atomic<bool> s_last_fail_lowmem{false};
// 池里的解码器是否已完成"首帧预热"。
std::atomic<bool> s_pool_warmed{false};
std::atomic<TaskHandle_t> s_player_task{nullptr};
SemaphoreHandle_t s_worker_mutex;
bool s_worker_quiescent;
SemaphoreHandle_t s_station_mutex;
SemaphoreHandle_t s_pool_mutex;
std::atomic<uint32_t> s_playback_revision{0};
RadioStation s_stations[kMaxStations] = {};

// ---- MP3 解码器内存池(开机预分配) ----
// C3 上 WiFi 连上后堆碎片化(largest 仅 ~7.6KB), 现场 open 解码器(需 ~19KB
// 连续块)必然失败。故在启动早期(堆最大 largest≈114KB)一次性抢下解码器与
// 收发缓冲, 之后切台只 reset 复用, 不再反复分配。收音机是主功能, 值得优先。
esp_audio_simple_dec_handle_t s_decoder_pool = nullptr;
uint8_t *s_input_pool = nullptr;
uint8_t *s_output_pool = nullptr;

RadioStation station_snapshot(std::size_t index) {
    RadioStation station = {};
    if (!s_station_mutex ||
        xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return station;
    }
    const std::size_t count = std::max<std::size_t>(s_station_count.load(), 1);
    station = s_stations[index % count];
    xSemaphoreGive(s_station_mutex);
    return station;
}

void save_setting(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

uint8_t load_setting(const char *key, uint8_t fallback) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

void save_station_name(const char *name) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    if (name && name[0]) {
        nvs_set_str(handle, "station_name", name);
    } else {
        nvs_erase_key(handle, "station_name");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

// Copies the station name the listener last chose. Returns false when no
// station has been remembered yet.
bool load_station_name(char *name, std::size_t capacity) {
    if (!name || capacity == 0) return false;
    name[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t length = capacity;
    const esp_err_t error = nvs_get_str(handle, "station_name", name, &length);
    nvs_close(handle);
    if (error != ESP_OK) {
        name[0] = '\0';
        return false;
    }
    return name[0] != '\0';
}

// Installs a catalog under the station mutex and reports the index that should
// stay selected. `preferred_name` keeps the previous station when the fresh
// catalog still carries it.
bool install_stations(const RadioStation *stations, std::size_t count,
                      const char *preferred_name, std::size_t *selected) {
    if (!stations || count == 0 || !s_station_mutex) return false;
    count = std::min(count, kMaxStations);

    // Stop the current request before replacing its backing catalog. The
    // streaming task uses a value snapshot, so the swap is atomic to users.
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    std::memcpy(s_stations, stations, count * sizeof(RadioStation));
    if (count < kMaxStations) {
        std::memset(s_stations + count, 0,
                    (kMaxStations - count) * sizeof(RadioStation));
    }
    std::size_t index = 0;
    if (preferred_name && preferred_name[0]) {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(s_stations[i].name, preferred_name) == 0) {
                index = i;
                break;
            }
        }
    }
    s_station_count.store(count, std::memory_order_release);
    s_station_index.store(index, std::memory_order_release);
    xSemaphoreGive(s_station_mutex);
    if (selected) *selected = index;
    return true;
}

bool request_still_current(uint32_t generation, std::size_t station) {
    return s_network_connected.load(std::memory_order_acquire) &&
           s_wanted_playing.load(std::memory_order_acquire) &&
           s_generation.load(std::memory_order_acquire) == generation &&
           s_station_index.load(std::memory_order_acquire) == station;
}

// 可中断的延时。⚠ 失败退避最长会到 120 秒, 若用一次性 vTaskDelay, 用户在
// 退避期间按"暂停"再按"恢复"时, 播放任务仍在睡 -> 表现为"暂停后再也恢复不了"
// (实测用户反馈)。这里切成 100ms 一片, 每片检查"用户是否改过播放意图/换台",
// 一旦变化立即返回, 让外层循环立刻响应。
// 返回 true = 正常睡满; false = 被用户操作打断(应立刻重新处理)。
bool interruptible_delay_ms(uint32_t total_ms, uint32_t generation,
                            std::size_t station) {
    const uint32_t slice = 100;
    for (uint32_t waited = 0; waited < total_ms; waited += slice) {
        if (radio_retry_cancelled(
                s_wanted_playing.load(std::memory_order_acquire),
                s_network_connected.load(std::memory_order_acquire), generation,
                s_generation.load(std::memory_order_acquire), station,
                s_station_index.load(std::memory_order_acquire))) return false;
        const uint32_t remaining = total_ms - waited;
        vTaskDelay(pdMS_TO_TICKS(remaining < slice ? remaining : slice));
    }
    return true;
}
void calculate_levels(const uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || bytes < 64) return;
    const auto *samples = reinterpret_cast<const int16_t *>(pcm);
    const std::size_t sample_count = bytes / sizeof(int16_t);
    if (sample_count < kLevelCount) return;
    const std::size_t frames = sample_count / std::max<uint8_t>(channels, 1);
    if (frames < kLevelCount) return;

    uint8_t levels[kLevelCount] = {};
    for (std::size_t band = 0; band < kLevelCount; ++band) {
        const std::size_t begin = band * frames / kLevelCount;
        const std::size_t end = (band + 1) * frames / kLevelCount;
        int64_t energy = 0;
        std::size_t count = 0;
        for (std::size_t frame = begin; frame < end; ++frame) {
            int32_t mixed = 0;
            for (uint8_t channel = 0; channel < channels; ++channel) {
                mixed += samples[frame * channels + channel];
            }
            mixed /= std::max<uint8_t>(channels, 1);
            energy += static_cast<int64_t>(mixed) * mixed;
            ++count;
        }
        const float rms = count ? std::sqrt(static_cast<float>(energy) / count) : 0.0f;
        float db = rms > 1.0f ? 20.0f * std::log10(rms / 32768.0f) : -80.0f;
        int value = static_cast<int>((db + 58.0f) * 2.25f);
        value += static_cast<int>((band * 13 + s_generation.load()) % 9) - 4;
        levels[band] = static_cast<uint8_t>(std::clamp(value, 2, 100));
    }
    radio_ui_set_audio_levels(levels, kLevelCount);
}

// ---------------------------------------------------------------------------
// 流级上下文: 通过 HTTP 事件回调捕获响应头里的 icy-metaint。
// (esp_http_client_get_header 只能读**请求**头, 读不到响应头, 必须走事件。)
struct StreamContext {
    int icy_metaint = 0;
};

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    auto *ctx = static_cast<StreamContext *>(evt->user_data);
    if (!ctx || !evt->header_key) return ESP_OK;
    if (strcasecmp(evt->header_key, "icy-metaint") == 0 && evt->header_value) {
        ctx->icy_metaint = std::atoi(evt->header_value);
    }
    return ESP_OK;
}


// ---------------------------------------------------------------------------
// ICY (SHOUTcast) 元数据剥离。
//
// 【为什么必须有】多数网络电台(实测中国之声/中文新闻台/FM891/Fred/动漫/亚洲/
// 怀旧金曲 等 7 个中文台全部命中)在响应头返回 `icy-metaint: N`(8192~32768),
// 表示**每 N 字节音频后插入一个元数据块**: 1 字节长度 L(单位 16), 后跟 L*16
// 字节文本(如 `StreamTitle='歌名';`)。ESP-IDF 的 esp_http_client **完全不做**
// 这件事(全局无 metaint 代码)。
//
// 不做剥离的后果: 元数据字节被当 MP3 数据送解码器 -> 解码器输出噪声,
// 听觉表现为**周期性电流声/咔哒**, 且解码错误计数上升。这正是"音质差"的根因。
//
// 本结构体跨多次 read 累积状态, 精确切掉元数据块。
//
// ⚠ 状态机只有两个计数, 不再需要额外的"待读长度字节"标志:
//   meta_left  > 0 → 正在丢弃元数据字节
//   audio_left > 0 → 正在拷贝音频字节
//   两者都为 0     → 下一个字节是本段音频之后紧跟的**长度字节**
//   因为 audio_left 是持久 int, "长度字节跨 read 边界"天然被处理
//   (它出现在 audio_left 恰好归零后的下一次调用里)。
// 早期版本在读到非空元数据时忘记重置 audio_left, 导致 audio_left 停在 0,
// 之后每个字节都被当长度字节消费 -> 每 1 字节吞掉 16 字节音频(丢 20% 数据),
// 听觉表现正是"每 1~2 秒卡一下 + 电流声"。
struct IcyStripper {
    int metaint = 0;        // 0 = 该流无元数据(不剥离)
    int audio_left = 0;     // 当前音频段还剩多少字节(>0 = 在音频中)
    int meta_left = 0;      // 当前元数据块还剩多少字节(>0 = 在元数据中)

    void begin(int metaint_value) {
        if (metaint_value > 0) {
            metaint = metaint_value;
            audio_left = metaint;
        } else {
            metaint = 0;
        }
        meta_left = 0;
    }

    // 原地剥离(把音频前移), 返回纯音频字节数。跨调用保持状态。
    std::size_t process(uint8_t *buf, std::size_t len) {
        if (metaint <= 0) return len;
        std::size_t rd = 0, wr = 0;
        while (rd < len) {
            if (meta_left > 0) {              // ① 丢弃元数据字节
                const std::size_t take = std::min<std::size_t>(
                    static_cast<std::size_t>(meta_left), len - rd);
                rd += take;
                meta_left -= static_cast<int>(take);
                continue;
            }
            if (audio_left > 0) {             // ② 拷贝音频字节
                const std::size_t take = std::min<std::size_t>(
                    static_cast<std::size_t>(audio_left), len - rd);
                if (wr != rd) std::memmove(buf + wr, buf + rd, take);
                wr += take;
                rd += take;
                audio_left -= static_cast<int>(take);
                continue;
            }
            // ③ 音频段读完: 本字节是长度 L(单位 16), 之后跟 L*16 字节元数据
            const int l = buf[rd] * 16;
            ++rd;
            audio_left = metaint;             // ★ 关键: 无论 L 是否为 0 都要重置
            if (l > 0) meta_left = l;
        }
        return wr;
    }
};

// ---------------------------------------------------------------------------
std::size_t downmix_to_mono(uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~static_cast<std::size_t>(1);

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const std::size_t frames = bytes / (sizeof(int16_t) * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        int32_t mixed = 0;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            mixed += samples[frame * channels + channel];
        }
        samples[frame] = static_cast<int16_t>(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

esp_audio_simple_dec_handle_t open_mp3_decoder() {
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    const esp_audio_err_t error = esp_audio_simple_dec_open(&config, &decoder);
    if (error != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "MP3 decoder open failed: %d", error);
        return nullptr;
    }
    return decoder;
}

}  // namespace

// 一段最小的**合法 MP3 帧**(静音), 用于逼解码器把首帧内存分配出来。
// 取一个标准 MPEG-1 Layer III 帧头(0xFF 0xFB 0x90 0x00 = 128kbps/44.1kHz/单声道)
// 加上足够的零填充 —— 解码器解析到帧头后会分配内部解析上下文与帧缓冲(约 20KB)。
// 这一步务必在**堆充裕时**做, 做完把句柄复用(切台只 reset), 之后永不再需要那 20KB。
static const uint8_t kSilentMp3Frame[417] = {
    0xFF, 0xFB, 0x90, 0x00,   // MPEG-1 Layer III, 128kbps, 44.1kHz, mono
    // 其余留 0(帧数据不重要, 解码器只需成功解析帧头并完成内部初始化)
};

// 让池里的解码器完成"首帧预热"——把内部大块内存提前分配出来。
//
// ✅ 实测有效(2026-10-10, 修正后): 喂一个**合法 MPEG-1 Layer III 帧**
// (0xFF 0xFB 0x90 0x00 = 128kbps/44.1kHz/mono, 与主流电台一致) 能让解码器
// 真正解码出一帧 PCM: 日志 `decoded=4608, warmed=1`。
//   - 输出缓冲必须 >= 4608B(满帧 1152 样本 x 2ch x 2B), 故 kOutputSize=5120。
//     (曾误设 4096 -> result=-8, decoded=0, 预热失败。)
//   - 预热在开机(heap largest=114688)时完成, 之后**不 reset、不释放池**,
//     内部缓冲常驻 -> 播放时无需再要 20KB 连续块。
// 因此 stream_station 在 warmed 时可跳过大块内存检查(见那里的说明)。
static void warm_pool_decoder(const char *who) {
    if (!s_decoder_pool || !s_input_pool || !s_output_pool) return;
    std::memset(s_input_pool, 0, kInputSize);
    std::memcpy(s_input_pool, kSilentMp3Frame, sizeof(kSilentMp3Frame));
    esp_audio_simple_dec_raw_t raw = {
        .buffer = s_input_pool,
        .len = static_cast<uint32_t>(sizeof(kSilentMp3Frame)),
        .eos = false,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };
    esp_audio_simple_dec_out_t frame = {
        .buffer = s_output_pool,
        .len = kPcmCapacity,
        .needed_size = 0,
        .decoded_size = 0,
    };
    const esp_audio_err_t warm =
        esp_audio_simple_dec_process(s_decoder_pool, &raw, &frame);
    // ⚠ 只有**真的解出数据**(decoded_size > 0)才算预热成功。
    // 之前写成 `warm == OK` 会把"帧头没识别(decoded=0)"也误判为成功。
    const bool warmed = frame.decoded_size > 0;
    s_pool_warmed.store(warmed, std::memory_order_release);
    ESP_LOGW(kTag,
             "radio %s: first-frame warmup result=%d (decoded=%u, warmed=%d, "
             "heap largest=%u)",
             who, (int)warm, (unsigned)frame.decoded_size, (int)warmed,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

bool radio_player_prewarm(void) {
    if (!s_pool_mutex) s_pool_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_pool_mutex) return false;
    if (s_decoder_pool) return true;
    // 解码器注册必须先于 open。radio_player_init 里也会注册(幂等)。
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    s_decoder_pool = open_mp3_decoder();
    s_output_pool = static_cast<uint8_t *>(malloc(kOutputSize));
    s_input_pool = static_cast<uint8_t *>(malloc(kInputSize));

    // ⚠⚠ 关键(2026-10-10 发热事故的根本修复):
    // 解码器句柄打开 ≠ 首帧内存已就绪。真正的 ~20KB 连续块是在**首次
    // process() 内部**懒分配的。如果不在这里(堆最充足时)先分配出来,
    // 一旦池被释放后重建, 堆已碎, 就**永远拿不到 20KB** -> 收音机永久播不了,
    // 而播放循环会不停重试(实测每 14 秒一轮, 持续发热)。
    warm_pool_decoder("prewarm");

    const bool ok = s_decoder_pool && s_output_pool && s_input_pool;
    ESP_LOGW(kTag, "radio prewarm: decoder=%d in=%p out=%p (heap largest=%u)",
             s_decoder_pool != nullptr, (void *)s_input_pool, (void *)s_output_pool,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    // 即使部分失败也不回滚: 现场路径会在 !decoder 时报错并退避, 不至于更糟。
    return ok;
}

// 释放收音机内存池: 进入小智等需要大块连续内存的应用前调用(此时收音机已暂停)。
// 退出后再由 radio_player_ensure_pool() 重新抢占。
// 这是"无后台应用"内存模型的落地: 谁在用谁占内存, 不用即还。
bool radio_player_has_pool() {
    return s_decoder_pool != nullptr && s_output_pool != nullptr && s_input_pool != nullptr;
}

void radio_player_release_pool(void) {
    xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY);
    if (s_decoder_pool) { esp_audio_simple_dec_close(s_decoder_pool); s_decoder_pool = nullptr; }
    if (s_output_pool) { free(s_output_pool); s_output_pool = nullptr; }
    if (s_input_pool) { free(s_input_pool); s_input_pool = nullptr; }
    s_pool_warmed.store(false, std::memory_order_release);
    ESP_LOGW(kTag, "radio pool released by caller (heap largest=%u)",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    xSemaphoreGiveRecursive(s_pool_mutex);
}

// 重新抢占内存池(退出小智/回收音机时)。可能失败(碎片), 失败则收音机走
// 现场分配路径(大概率开不出解码器, 但不会崩)。
void radio_player_ensure_pool(void) {
    // Entering the radio UI must not wait for the lifetime of an active stream.
    if (s_stream_active.load(std::memory_order_acquire) &&
        s_pool_warmed.load(std::memory_order_acquire)) return;
    xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY);
    if (s_decoder_pool) {
        // 池在但可能没预热(例如上次预热失败) -> 补一次。
        if (!s_pool_warmed.load(std::memory_order_acquire)) {
            warm_pool_decoder("ensure");
            // A failed warmup can leave the MP3 decoder handle unusable
            // (ESP_MP3_DEC ret 10 on every later stream). Discard it and
            // rebuild once instead of carrying a poisoned handle forever.
            if (!s_pool_warmed.load(std::memory_order_acquire)) {
                ESP_LOGW(kTag, "radio pool warmup failed; rebuilding decoder");
                if (s_decoder_pool) {
                    esp_audio_simple_dec_close(s_decoder_pool);
                    s_decoder_pool = nullptr;
                }
                free(s_output_pool);
                s_output_pool = nullptr;
                free(s_input_pool);
                s_input_pool = nullptr;
            }
        }
        if (s_decoder_pool) {
            xSemaphoreGiveRecursive(s_pool_mutex);
            return;
        }
    }
    s_decoder_pool = open_mp3_decoder();
    s_output_pool = static_cast<uint8_t *>(malloc(kOutputSize));
    s_input_pool = static_cast<uint8_t *>(malloc(kInputSize));
    // ⚠ 重建失败必须**彻底清干净**(2026-10-12): 否则会留下一个半死的
    //   s_decoder_pool 句柄 —— 后续 stream_station 看到它非空就直接复用, 于是
    //   永远用着一个没成功预热过的解码器, 表现为收音机一直卡在"正在缓冲"/
    //   "正在切换电台", 而且再也不会重试(池"看起来"是齐的)。
    if (!s_decoder_pool || !s_output_pool || !s_input_pool) {
        ESP_LOGE(kTag, "pool rebuild failed (decoder=%p out=%p in=%p); clearing",
                 (void *)s_decoder_pool, (void *)s_output_pool, (void *)s_input_pool);
        if (s_decoder_pool) { esp_audio_simple_dec_close(s_decoder_pool); s_decoder_pool = nullptr; }
        free(s_output_pool); s_output_pool = nullptr;
        free(s_input_pool);   s_input_pool = nullptr;
        xSemaphoreGiveRecursive(s_pool_mutex);
        return;
    }    // ⚠ 重建的池**必须再预热一次**(2026-10-10): 否则新句柄的首帧内存又变成
    // "待懒分配"状态, 而此刻堆往往已碎 -> 收音机永久播不了 -> 播放循环空转发热。
    warm_pool_decoder("re-acquire");
    ESP_LOGW(kTag, "radio pool re-acquired: decoder=%d warmed=%d (largest=%u)",
             s_decoder_pool != nullptr,
             (int)s_pool_warmed.load(std::memory_order_acquire),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    xSemaphoreGiveRecursive(s_pool_mutex);
}

void radio_player_rewake_after_pool_ready(void) {
    // A canceled XiaoZhi task can briefly keep the decoder's large internal
    // block while the home layer is already visible. Warm the pool once more
    // after that task has returned Opus, then wake the player so a failed
    // first attempt does not leave it in the long low-memory backoff.
    radio_player_ensure_pool();
    if (!s_pool_warmed.load(std::memory_order_acquire)) {
        // The first warmup can leave the decoder handle in a failed state
        // (ESP_MP3_DEC ret 10). Keeping that handle makes every later stream
        // fail even after memory is available, so discard the whole pool and
        // try one clean rebuild now that Opus has actually been returned.
        radio_player_release_pool();
        vTaskDelay(pdMS_TO_TICKS(50));
        radio_player_ensure_pool();
    }
    if (s_player_task.load(std::memory_order_acquire) &&
        s_wanted_playing.load(std::memory_order_acquire) &&
        s_pool_warmed.load(std::memory_order_acquire)) {
        xTaskNotifyGive(s_player_task.load(std::memory_order_acquire));
    }
}

uint8_t *radio_player_search_buffer(void) {
    xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY);
    if (!s_stream_active.load(std::memory_order_acquire) && s_output_pool) return s_output_pool;
    xSemaphoreGiveRecursive(s_pool_mutex);
    return nullptr;
}
std::size_t radio_player_search_buffer_size(void) { return kOutputSize; }
void radio_player_release_search_buffer(void) {
    xSemaphoreGiveRecursive(s_pool_mutex);
}

namespace {

constexpr std::size_t kNetworkBlocks = 6;

// network_feed_task 的栈**静态分配**: 它要求一块 2048B **连续**堆, 而
// 播放时堆已被 LVGL/WiFi/音频分割到 largest<4KB, 动态申请必然失败
// (实测 "network task stack unavailable")。放进 .bss 就没有这个约束:
// 静态内存在开机就由 ELF 预留, 不参与堆碎片竞争。
constexpr std::size_t kNetworkFeedStack = 2048;
StaticTask_t s_network_feed_tcb;
StackType_t  s_network_feed_stack[kNetworkFeedStack];

struct NetworkFeed {
    esp_http_client_handle_t client;
    RadioStreamBuffer *ring;
    uint32_t generation;
    std::size_t station;
    std::atomic<bool> finished{false};
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> max_read_us{0};
};

void network_feed_task(void *argument) {
    auto *feed = static_cast<NetworkFeed *>(argument);
    // 先预留任务栈，等 HTTP 和环形缓冲就绪后才开始读取。
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t bytes[kInputSize];
    unsigned empty_reads = 0;
    while (!feed->stop.load() && request_still_current(feed->generation, feed->station)) {
        const int64_t start = esp_timer_get_time();
        const int received = esp_http_client_read(feed->client,
                                                  reinterpret_cast<char *>(bytes),
                                                  sizeof(bytes));
        const uint32_t elapsed = esp_timer_get_time() - start;
        uint32_t maximum = feed->max_read_us.load();
        while (maximum < elapsed &&
               !feed->max_read_us.compare_exchange_weak(maximum, elapsed)) {}
        if (received == -ESP_ERR_HTTP_EAGAIN) {
            if (++empty_reads >= 5) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (received < 0) {
            ESP_LOGW(kTag, "network feed stopped: read=%d", received);
            break;
        }
        if (!received) {
            if (esp_http_client_is_complete_data_received(feed->client) ||
                ++empty_reads >= 5) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        empty_reads = 0;
        std::size_t used = 0;
        while (used < static_cast<std::size_t>(received) &&
               !feed->stop.load() &&
               request_still_current(feed->generation, feed->station)) {
            used += feed->ring->write(bytes + used, received - used);
            if (used < static_cast<std::size_t>(received)) vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    // The owner deletes this worker before freeing its HTTP client or ring.
    feed->finished.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
}

bool stream_station(std::size_t station, uint32_t generation) {
    const RadioStation preset = station_snapshot(station);
    if (!preset.url[0]) {
        ESP_LOGE(kTag, "Station %u has no stream URL", static_cast<unsigned>(station));
        return false;
    }
    radio_ui_set_playback(RadioPlaybackState::Connecting, "正在连接电台");

    esp_http_client_config_t config = {};
    config.url = preset.url;
    config.timeout_ms = 7000;
    // ⚠⚠ 缓冲必须够大 —— 2026-10-10 踩坑还原:
    // 曾为"省连续内存给解码器"把接收缓冲降到 1024/发送降到 256, 结果音频
    // **一卡一卡**(吞吐不足, 解码器时常等数据饿死)。原始可工作版本用 2048/512。
    // 现解码器内部缓冲已在开机预热并常驻, 播放路径不再依赖大连续块,
    // 因此恢复原始值, 保证喂数据速率。
    config.buffer_size = 2048;
    config.buffer_size_tx = 512;
    config.user_agent = "AI-Passport-Radio/1.1.1";
    config.keep_alive_enable = true;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 4;
    // 捕获响应头里的 icy-metaint(必须走事件回调, get_header 读不到响应头)
    StreamContext stream_ctx;
    config.event_handler = http_event_handler;
    config.user_data = &stream_ctx;
    if (std::strncmp(preset.url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "HTTP client allocation failed");
        return false;
    }
    // 请求 ICY 元数据: 必须发 1, 服务器才会在响应头返回 icy-metaint,
    // 我们据此剥离插入的元数据块(见 IcyStripper)。
    // ⚠ 发 0 时服务器**依然**周期性插入元数据, 只是不告诉我们间隔 -> 无解,
    // 只能盲切; 发 1 拿到 metaint 才能精确剥离。
    esp_http_client_set_header(client, "Icy-MetaData", "1");
    esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");

    bool played_audio = false;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    uint8_t *input = nullptr;
    uint8_t *output = nullptr;
    uint8_t *network_blocks[kNetworkBlocks] = {};
    std::size_t block_count = kNetworkBlocks;
    std::size_t prebuffer_bytes = 0;
    std::optional<RadioStreamBuffer> ring_storage;
    NetworkFeed feed{client, nullptr, generation, station};
    TaskHandle_t network_task = nullptr;

    // 复用开机预分配的解码器与缓冲(见 radio_player_prewarm): 切台时只 reset,
    // 不再 close/open。C3 上 WiFi 连上后堆碎片化(largest 仅 ~7.6KB), 现场
    // open 解码器(需 ~19KB 连续块)必然失败 -> 收音机无声的根因。
    //
    // ⚠ 兜底: 池可能被"进小智"或"城市搜索"释放过。这里再试一次恢复,
    // 因为此刻播放任务未运行、堆最干净; 若仍失败再退回现场分配。
    // 不这样做的话, 用户在池缺失时"点继续"会反复 open 失败 ->
    // 永远卡在"正在缓冲"(实测 ret 10 刷屏)。
    if (!s_decoder_pool) {
        ESP_LOGW(kTag, "decoder pool missing, attempting re-acquire (largest=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        radio_player_ensure_pool();
    }
    xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY);
    if (s_decoder_pool) {
        decoder = s_decoder_pool;
        input = s_input_pool;
        output = s_output_pool;
        // ⚠ 不再调用 esp_audio_simple_dec_reset(2026-10-10 实测):
        // 该函数的语义是"把 internal parser 和 decoder 都重置到初始态" ——
        // 实测它会把开机预热时分配好的那 ~20KB 内部缓冲**释放掉**, 导致
        // 播放时又需要重新分配 20480 连续块, 而此时堆已被 HTTP/WiFi 打碎, 拿不到
        // -> 收音机播不了。
        // 我们每次都是"切台即重新开始解码一条全新的流", 而 reset 的目的是复用
        // 句柄避免 close/open 开销; 既然它反而破坏内存, 就不调用它。
        // (非帧模式下从流头开始解码, 与 reset 后的行为一致。)
    } else {
        decoder = open_mp3_decoder();
        input = static_cast<uint8_t *>(malloc(kInputSize));
        output = static_cast<uint8_t *>(malloc(kOutputSize));
    }
    if (!input || !output) {
        ESP_LOGE(kTag, "stream buffers unavailable");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        xSemaphoreGiveRecursive(s_pool_mutex);
        return false;
    }

        do {
        // 栈与 TCB 都在 .bss(见上方 kNetworkFeedStack), 不吃堆。
        network_task = xTaskCreateStatic(network_feed_task, "radio_network",
                                       kNetworkFeedStack, &feed, 5,
                                       s_network_feed_stack, &s_network_feed_tcb);
        if (network_task == nullptr) {
            s_last_fail_lowmem.store(true, std::memory_order_release);
            ESP_LOGW(kTag, "network feed task create failed");
            break;
        }
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(kTag, "Open failed: %s", preset.url);
            break;
        }
        // 内存探针(诊断): open 之后(socket/TCP 建立)的连续块大小。
        ESP_LOGW(kTag, "MEM after http_open: largest=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(kTag, "HTTP status %d", status);
            break;
        }

        if (!decoder) {
            // 解码器开不出来通常是内存不足(如 xiaozhi opus 预热后)。立即放弃
            // 本流并冷却, 否则外层立即重试会以毫秒级频率狂刷 MP3 init 失败,
            // CPU 满载烤机(实测 13:12 版固件发热根因)。
            ESP_LOGE(kTag, "MP3 decoder unavailable (heap?), backing off 10s");
            s_last_fail_lowmem.store(true, std::memory_order_release);
            interruptible_delay_ms(10000, generation, station);
            break;
        }
        bool blocks_ready = true;
        // 给页面重绘留下 8KB，避免缓冲耗尽堆后 LVGL malloc 断言卡死。
        const std::size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        constexpr std::size_t kUiReserve = 4096;
        const std::size_t affordable = free_bytes > kUiReserve
            ? kSharedNetworkBlocks + (free_bytes - kUiReserve) / kInputSize
            : kSharedNetworkBlocks;
        block_count = std::min(block_count, affordable);
        if (block_count < kSharedNetworkBlocks) {
            s_last_fail_lowmem.store(true, std::memory_order_release);
            ESP_LOGW(kTag, "insufficient heap for network buffer and UI reserve");
            break;
        }
        prebuffer_bytes = (block_count - 2) * kInputSize;
        ring_storage.emplace(network_blocks, block_count, kInputSize);
        RadioStreamBuffer &ring = *ring_storage;
        feed.ring = &ring;
        for (std::size_t i = 0; i < block_count; ++i) {
            network_blocks[i] = i < kSharedNetworkBlocks
                ? output + kPcmCapacity + i * kInputSize
                : static_cast<uint8_t *>(malloc(kInputSize));
            if (!network_blocks[i]) { blocks_ready = false; break; }
        }
        // Bound pause/cancel latency after the connection is established.
        esp_http_client_set_timeout_ms(client, 1000);
        if (!blocks_ready) {
            s_last_fail_lowmem.store(true, std::memory_order_release);
            ESP_LOGW(kTag, "network buffer unavailable");
            break;
        }
        xTaskNotifyGive(network_task);
        // 缓冲已在上面就位(池化复用或现场分配), 此处不再 malloc。

        radio_ui_set_playback(RadioPlaybackState::Buffering, "正在缓冲");
        bool format_ready = false;
        uint8_t source_channels = 1;
        bool buffering = true;
        bool playback_announced = false;
        uint32_t rebuffers = 0;
        int decode_fail_streak = 0;   // 连续解码失败计数(内存不足/坏流): 达限放弃本电台, 防3ms风暴
        bool stream_checked = false;  // 首块是否已做 MP3 同步字校验
        int64_t stats_start = esp_timer_get_time();
        int64_t last_write_end = 0;
        uint32_t max_read_us = 0, max_gap_us = 0, slow_gaps = 0;
        uint32_t pcm_samples = 0, pcm_clipped = 0, sample_rate = 0;
        uint8_t applied_volume = 0;
        IcyStripper icy;              // ICY 元数据剥离器(见 struct 定义)
        {
            // metaint 由 HTTP 事件回调在收响应头时写入 stream_ctx。
            // 无该头(如 SomaFM) -> metaint=0, process() 退化为直通。
            icy.begin(stream_ctx.icy_metaint);
            if (stream_ctx.icy_metaint > 0) {
                ESP_LOGI(kTag, "ICY metadata present: metaint=%d, will strip",
                         stream_ctx.icy_metaint);
            }
        }
        while (request_still_current(generation, station)) {
            const std::size_t available = ring.available();
            if (buffering && available < prebuffer_bytes && !feed.finished.load()) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            buffering = false;
            const int received = ring.read(input, kInputSize);
            if (!received) {
                if (feed.finished.load(std::memory_order_acquire)) break;
                buffering = true;
                playback_announced = false;
                ++rebuffers;
                radio_ui_set_playback(RadioPlaybackState::Buffering, "正在缓冲");
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            // ⚠ 必须在任何解析之前剥离 ICY 元数据: 否则元数据字节被当音频送
            // 解码器, 产生周期性电流声(用户反馈"音质差"的根因)。
            std::size_t audio_len = icy.process(input, static_cast<std::size_t>(received));
            if (audio_len == 0) continue;   // 本块全是元数据

            // 首块 MP3 同步字检查(在剥离元数据之后做, 否则首字节可能落在元数据里)。
            //
            // ⚠ 不能用"首字节必须是 0xFF/I"来判定 —— 那会**误杀合法的直播流**。
            // 实测重庆都市广播 / 北碚嘉陵之声均为标准 MP3(ffmpeg 解码 0 错误),
            // 但它们是从**帧中间接入**的: 首字节分别是 0x0E / 0x6C, 真正的
            // 帧同步字 0xFF 出现在偏移 101 / 11 处。这类"起始残片"所有解码器
            // 都能容忍, 硬判非 MP3 会让这些台永远播不了。
            // 正确做法: 在首个数据块内**搜索**帧同步字(0xFF 且高 3 位为 1),
            // 找到就把前面的残片丢掉; 找不到(如 HTML 错误页)才判为非法。
            if (!stream_checked) {
                stream_checked = true;
                std::size_t sync = audio_len;   // audio_len = 找不到
                for (std::size_t i = 0; i + 2 < audio_len; ++i) {
                    if (radio_mp3_header_valid(input[i], input[i + 1], input[i + 2])) {
                        sync = i;
                        break;
                    }
                }
                if (sync >= audio_len) {
                    // 前 512 字节内没有任何帧同步字: 极可能不是音频流(错误页等)
                    ESP_LOGW(kTag, "No MP3 sync word in first %u bytes (first=0x%02X url=%.70s), skipping",
                             (unsigned)audio_len, input[0], preset.url);
                    break;
                }
                if (sync > 0) {
                    // 丢掉起始残片, 从同步字开始送解码器
                    ESP_LOGI(kTag, "Stream starts mid-frame; skipping %u lead bytes",
                             (unsigned)sync);
                    std::memmove(input, input + sync, audio_len - sync);
                    audio_len -= sync;
                }
            }
            if (decode_fail_streak >= 8) break;   // 多次短暂停重试仍失败: 交外层冷却

            // ⚠⚠ 前置判据(2026-10-09/10 发热事故的根本修复):
            // MP3 解码器句柄打开 != 首帧缓冲已分配。真正的 ~20KB 连续内存在
            // **首次 esp_audio_simple_dec_process() 内部**才懒分配。
            // 因此本流还没解出首帧(format_ready==false)而 largest 又不足时,
            // 这次调用注定失败, 且底层库单次调用内部会连喷约 20 条
            // "Fail to init MP3 decoder ret 10"(实测每 10~15 秒一轮, 发热)。
            //
            // ✅ 例外(2026-10-10): 若池**已在开机预热成功**(s_pool_warmed,
            // 日志 `decoded=4608`), 说明解码器内部那 ~20KB 早在开机
            // (largest=114688)时就分配好并常驻(我们全程不 reset、不释放池),
            // 播放时**不需要任何新的大块内存** -> 此时必须跳过本判据,
            // 否则会误杀正常播放(实测: 池已预热却被判据拦 -> 收音机永远播不了)。
            // 安全性: 万一 warmed 判断有误、真的解不出来, 下方失败路径的
            // `memory_hopeful=false` 会立即把 decode_fail_streak 置 100 并跳出,
            // **只产生 1 条日志**, 不会形成风暴。
            if (!format_ready && !s_pool_warmed.load(std::memory_order_acquire)) {
                const std::size_t largest_now =
                    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                if (largest_now < kDecoderMinBlock) {
                    static int64_t s_last_skip_warn_us = 0;
                    const int64_t now_us = esp_timer_get_time();
                    if (now_us - s_last_skip_warn_us > 5LL * 1000 * 1000) {
                        s_last_skip_warn_us = now_us;
                        ESP_LOGW(kTag,
                                 "insufficient contiguous heap for decoder first frame "
                                 "(largest=%uB < %uB): skip attempt, back off",
                                 (unsigned)largest_now,
                                 (unsigned)kDecoderMinBlock);
                    }
                    // 标记"这次失败是内存不足引起的" -> 播放循环会走长退避,
                    // 且**不**换台(换到别的台同样解不出来)。
                    s_last_fail_lowmem.store(true, std::memory_order_release);
                    break;   // 不调用解码器 -> 无错误日志 -> 不发热
                }
            }
            esp_audio_simple_dec_raw_t raw = {
                .buffer = input,
                .len = static_cast<uint32_t>(audio_len),
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            while (raw.len > 0 && request_still_current(generation, station)) {
                esp_audio_simple_dec_out_t frame = {
                    .buffer = output,
                    .len = kPcmCapacity,
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                raw.consumed = 0;
                const esp_audio_err_t result =
                    esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    ESP_LOGE(kTag, "Decoder needs %u bytes, buffer has %u",
                             static_cast<unsigned>(frame.needed_size),
                             static_cast<unsigned>(kPcmCapacity));
                    raw.len = 0;
                    break;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    // 解码器对"首帧"会做懒分配(MP3 解析上下文/帧缓冲, 需约 20KB
                    // 连续块)。堆碎片化时返回 ret 10(内存不足) / -2。
                    //
                    // ⚠ 关键判据: **重试前先看堆里有没有足够连续块**。
                    // 若 largest 明显不够, 重试注定失败 —— 只会以 ~5 次/秒的频率
                    // 刷 "Fail to init" 并烧 CPU(实测: 一次 60s 打 107 条错误,
                    // 触发发热告警)。此时必须立刻放弃本块, 交给外层指数退避
                    // (1.5s -> 10s -> ... -> 120s), 而不是原地空转。
                    // 只有当内存看起来够(碎片短时抖动)才值得短暂停顿后重试。
                    const size_t largest =
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                    const bool memory_hopeful = largest >= kDecoderMinBlock;
                    if (++decode_fail_streak == 1)
                        ESP_LOGW(kTag, "MP3 decode failed: %d (largest=%u)",
                                 result, (unsigned)largest);
                    if (memory_hopeful && decode_fail_streak <= 3) {
                        // 让出 CPU 并给堆一点时间合并出连续块, 再试同一个块。
                        vTaskDelay(pdMS_TO_TICKS(200));
                    } else {
                        // 内存不够(或已重试多次): 立即放弃, 不再空转刷屏。
                        // 这里的"失败"由 decode_fail_streak 决定退出读取循环,
                        // 之后走外层退避, 不会形成风暴。
                        if (!memory_hopeful && decode_fail_streak == 1)
                            ESP_LOGW(kTag,
                                     "decoder needs ~%uB contiguous, largest=%uB"
                                     " -> skip retries, back off",
                                     (unsigned)kDecoderMinBlock, (unsigned)largest);
                        decode_fail_streak = 100;   // 直接判定本流不可解, 交给外层
                    }
                    raw.len = 0;
                    break;
                }
                if (raw.consumed > raw.len) raw.consumed = raw.len;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;

                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t info = {};
                    if (!format_ready &&
                        esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                        source_channels = std::clamp<uint8_t>(info.channel, 1, 2);
                        if (info.bits_per_sample != 16 || info.sample_rate < 8000 ||
                            info.sample_rate > 48000 ||
                            bsp_audio_set_format(info.sample_rate, 16, 1) != ESP_OK) {
                            ESP_LOGE(kTag, "Unsupported stream format: %luHz/%ubit/%uch",
                                     static_cast<unsigned long>(info.sample_rate),
                                     info.bits_per_sample, source_channels);
                            raw.len = 0;
                            break;
                        }
                        applied_volume = s_volume.load(std::memory_order_acquire);
                        bsp_audio_set_volume(applied_volume);
                        sample_rate = info.sample_rate;
                        format_ready = true;
                        ESP_LOGI(kTag, "Playing %s at %luHz/%uch -> mono", preset.name,
                                 static_cast<unsigned long>(info.sample_rate), source_channels);
                    }
                    if (format_ready) {
                        const uint8_t requested_volume = s_volume.load(std::memory_order_acquire);
                        if (requested_volume != applied_volume) {
                            bsp_audio_set_volume(requested_volume);
                            applied_volume = requested_volume;
                            ESP_LOGI(kTag, "volume applied from player: %u", applied_volume);
                        }
                        const std::size_t mono_bytes =
                            downmix_to_mono(frame.buffer, frame.decoded_size, source_channels);
                        const int64_t write_start = esp_timer_get_time();
                        if (last_write_end) {
                            const uint32_t gap = write_start - last_write_end;
                            max_gap_us = std::max(max_gap_us, gap);
                            if (gap > 30000) ++slow_gaps;
                        }
                        const auto *samples = reinterpret_cast<const int16_t *>(frame.buffer);
                        for (std::size_t i = 0; i < mono_bytes / 2; ++i) {
                            if (samples[i] >= 32760 || samples[i] <= -32760) ++pcm_clipped;
                        }
                        pcm_samples += mono_bytes / 2;
                        if (bsp_audio_write(frame.buffer, mono_bytes) != ESP_OK) continue;
                        last_write_end = esp_timer_get_time();
                        if (last_write_end - stats_start >= 10000000) {
                            max_read_us = feed.max_read_us.exchange(0);
                            ESP_LOGI(kTag, "audio stats: wall=%ums pcm=%ums read_max=%ums gap_max=%ums gaps30=%u clipped=%u rebuffers=%u queued=%u stack_free=%u",
                                     (unsigned)((last_write_end - stats_start) / 1000),
                                     (unsigned)(pcm_samples * 1000ULL / sample_rate),
                                     (unsigned)(max_read_us / 1000), (unsigned)(max_gap_us / 1000),
                                     (unsigned)slow_gaps, (unsigned)pcm_clipped,
                                     (unsigned)rebuffers, (unsigned)ring.available(),
                                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
                            stats_start = last_write_end;
                            max_read_us = max_gap_us = slow_gaps = pcm_samples = pcm_clipped = 0;
                            rebuffers = 0;
                        }
                        calculate_levels(frame.buffer, mono_bytes, 1);
                        if (!playback_announced) {
                            if (!played_audio) {
                            ESP_LOGI(kTag, "PCM playback active (%u bytes)",
                                     static_cast<unsigned>(mono_bytes));
                            }
                            played_audio = true;
                            playback_announced = true;
                            radio_ui_set_playback(RadioPlaybackState::Playing, "正在播放");
                        }
                    }
                }

                if (raw.consumed > 0 || frame.decoded_size > 0) {
                    decode_fail_streak = 0;
                } else {
                    // Keep unconsumed input intact, including after cached PCM output.
                    // Bound a stalled decoder instead of overwriting its pending bytes.
                    if (++decode_fail_streak >= 8) {
                        ESP_LOGW(kTag, "MP3 decoder stalled; reconnecting stream");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            if (decode_fail_streak >= 8) break;
        }
    } while (false);

    if (network_task) {
        // Also cancel on a decoder failure while the user's station is unchanged.
        feed.stop.store(true, std::memory_order_release);
        xTaskNotifyGive(network_task);
        while (!feed.finished.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(5));
        vTaskDelete(network_task);
    }
    for (std::size_t i = kSharedNetworkBlocks; i < block_count; ++i) free(network_blocks[i]);
    // 池化资源(开机预分配)不在此释放, 留给下次切台复用。
    if (!s_decoder_pool) {
        if (decoder) esp_audio_simple_dec_close(decoder);
        free(output);
        free(input);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    xSemaphoreGiveRecursive(s_pool_mutex);
    return played_audio;
}

void player_task(void *) {
    std::size_t last_attempted_station = SIZE_MAX;
    uint8_t failed_attempts = 0;
    // 连续失败升级冷却: 坏流(如目录里不可解码的电台)会被反复重试, 固定 10s
    // 冷却等于每 10s 烧一轮 CPU+解码风暴(实测发热)。按连续失败次数指数退避,
    // 上限 120s, 使坏流状态下 CPU 占用可忽略。
    uint32_t fail_backoff_ms = 1500;   // 首次短退避: 让堆有时间合并出解码器所需连续块
    while (true) {
        if (!s_wanted_playing.load(std::memory_order_acquire)) {
            radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
            xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
            if (!s_wanted_playing.load(std::memory_order_acquire)) {
                s_worker_quiescent = true;
                xSemaphoreGive(s_worker_mutex);
                while (!s_wanted_playing.load(std::memory_order_acquire)) {
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                }
                xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
                s_worker_quiescent = false;
            }
            xSemaphoreGive(s_worker_mutex);
            continue;
        }
        if (!s_network_connected.load(std::memory_order_acquire)) {
            radio_ui_set_playback(RadioPlaybackState::Stopped, "等待网络");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        const std::size_t station = s_station_index.load(std::memory_order_acquire);
        const uint32_t generation = s_generation.load(std::memory_order_acquire);
        if (station != last_attempted_station) {
            // 换台时重置"该台连续失败数", 但【不重置退避】—— 否则两三个坏台
            // 互相切换会让指数退避永远卡在 10s, 每 10s 烧一轮解码风暴(实测发热)。
            last_attempted_station = station;
            failed_attempts = 0;
        }
        s_stream_active.store(true, std::memory_order_release);
        s_last_fail_lowmem.store(false, std::memory_order_release);
        const bool played = stream_station(station, generation);
        s_stream_active.store(false, std::memory_order_release);
        // 请求已失效(换台/暂停/断网): 让出 CPU 再重试。此处若直接 continue,
        // 当 station 请求快速失败时会在无延时循环里空转饿死 IDLE -> task_wdt。
        if (!request_still_current(generation, station)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (played) {
            // The stream worked at least briefly, so this station is alive and
            // simply dropped. Keep it and reconnect.
            failed_attempts = 0;
            fail_backoff_ms = 1500;
            radio_ui_set_playback(RadioPlaybackState::Reconnecting,
                                  "信号中断，正在重连");
            interruptible_delay_ms(1200, generation, station);
            continue;
        }

        // ⚠⚠ 区分两类失败(2026-10-10 发热事故):
        //   A) 电台本身坏(地址失效/编码不支持) -> 换台是对的;
        //   B) **内存连续块不足, 解码器首帧分配不出来** -> 换到任何台都一样解不出,
        //      换台反而会在"每个台都失败"之间循环打转, 无限消耗。
        // 对 B 必须: 不换台 + 用**长退避**(默认 60s 起), 并明确提示用户在等内存。
        if (s_last_fail_lowmem.exchange(false, std::memory_order_acq_rel)) {
            // The minimized radio keeps its playback intent while a canceled
            // XiaoZhi task / Wi-Fi teardown is still holding contiguous RAM.
            // Poll quickly for that block instead of entering the old 60s
            // penalty, which made a healthy station look permanently dead.
            if (fail_backoff_ms < 1500) fail_backoff_ms = 1500;
            radio_ui_set_playback(RadioPlaybackState::Error, "内存不足 稍后重试");
            ESP_LOGW(kTag,
                     "stream failed: not enough contiguous heap; "
                     "hold same station, retry in %ums (no station switch)",
                     (unsigned)fail_backoff_ms);
            if (!interruptible_delay_ms(fail_backoff_ms, generation, station)) continue;
            if (fail_backoff_ms < 10000) fail_backoff_ms = 10000;
            continue;
        }

        // 未播出: 冷却。首次用**短退避(1.5s)**而非 10s ——
        // 实测"退出收音机再进来点继续"时, 首次 stream_station 常因堆尚未整理
        // 而解码器 init 失败(ESP_MP3_DEC ret 10); 堆在 1~2 秒内即可合并出
        // 连续块(largest 7680 -> 19456), 短退避后重试即成功。
        // 而原来固定 10s 首次退避会让用户干等 10 秒才出声, 体感就是"卡在缓冲
        // 一直出不来"。之后仍按指数退避(×2, 上限 120s)防止坏台烧 CPU。
        if (!interruptible_delay_ms(fail_backoff_ms, generation, station)) continue;
        if (fail_backoff_ms < 120000) {
            // 首轮 1.5s -> 下一轮起按 10s 起步再翻倍
            fail_backoff_ms = (fail_backoff_ms < 10000)
                                  ? 10000
                                  : (fail_backoff_ms > 60000 ? 120000
                                                             : fail_backoff_ms * 2);
        }

        ++failed_attempts;
        const std::size_t count = radio_player_station_count();
        // Public directories keep dead entries around. After a few silent
        // failures, move on instead of retrying the same address forever.
        if (failed_attempts >= kMaxFailedAttempts && count > 1) {
            ESP_LOGW(kTag, "Station %u unreachable, skipping to the next one",
                     static_cast<unsigned>(station));
            radio_ui_set_playback(RadioPlaybackState::Error,
                                  "电台无法播放 已换台");
            interruptible_delay_ms(1200, generation, station);
            failed_attempts = 0;
            radio_player_select_relative(1);
            continue;
        }

        radio_ui_set_playback(RadioPlaybackState::Error, "连接失败，正在重试");
        interruptible_delay_ms(2500, generation, station);
    }
}

}  // namespace

static bool start_player_task_locked() {
    if (s_player_task.load(std::memory_order_acquire)) return true;
    TaskHandle_t task = nullptr;
    s_worker_quiescent = false;
    if (xTaskCreate(player_task, "radio_stream", 4608, nullptr, 5, &task) != pdPASS) {
        return false;
    }
    s_player_task.store(task, std::memory_order_release);
    return true;
}

static bool set_playing_locked(bool playing) {
    s_playback_revision.fetch_add(1);
    s_wanted_playing.store(playing, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    const bool started = !playing || start_player_task_locked();
    if (!started) s_wanted_playing.store(false, std::memory_order_release);
    if (playing && started) xTaskNotifyGive(s_player_task.load(std::memory_order_acquire));
    return started;
}

RadioCatalogPause radio_player_pause_for_catalog(void) {
    xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
    const bool was_playing = s_wanted_playing.load();
    set_playing_locked(false);
    RadioCatalogPause pause{was_playing, s_playback_revision.load()};
    xSemaphoreGive(s_worker_mutex);
    return pause;
}

// 目录搜索把堆切碎后, 播放任务可能第一次创建失败。堆会在目录任务退出后
// 逐渐合并, 所以用这个自带退避的 worker 反复尝试, 直到播放任务真的起来。
// 没有它, 一次失败就意味着"界面永远停在正在切换电台"(实测)。
static void playback_retry_task(void *)
{
    for (int attempt = 0; attempt < 20 && !s_player_task.load(std::memory_order_acquire); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_wanted_playing.load(std::memory_order_acquire)) break;
        if (s_player_task.load(std::memory_order_acquire)) break;
        if (xSemaphoreTake(s_worker_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        const bool ok = start_player_task_locked();
        if (ok) {
            const TaskHandle_t task = s_player_task.load(std::memory_order_acquire);
            if (task) xTaskNotifyGive(task);
        }
        xSemaphoreGive(s_worker_mutex);
        if (ok) {
            ESP_LOGI(kTag, "playback recovered after catalog (attempt %d)", attempt + 1);
            break;
        }
    }
    vTaskDelete(nullptr);
}

static void radio_schedule_playback_retry()
{
    static volatile bool s_retry_queued = false;
    bool expected = false;
    if (!s_retry_queued) {
        s_retry_queued = true;
        if (xTaskCreate(playback_retry_task, "radio_retry", 3072, nullptr, 4, nullptr) != pdPASS) {
            s_retry_queued = false;
            return;
        }
        ESP_LOGW(kTag, "queued playback retry worker");
    }
    (void)expected;
}
void radio_player_resume_after_catalog(RadioCatalogPause pause) {
    xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
    // ⚠ 目录搜索结束后必须让播放**一定**能回来(2026-10-12 修复"收音机一直卡在
    //   正在切换电台")。
    //
    //   原实现在 start_player_task_locked() 失败时直接 s_wanted_playing=false,
    //   于是用户看到的就是: 界面停在"正在切换电台"像卡死, 再按 OK 也只是
    //   再调一次 set_playing(true), 而播放任务依然起不来。日志对应:
    //     radio_player: playback task could not restart after catalog
    //
    //   根因: 搜索期间堆被 8KB 响应缓冲 + 6144 栈的 catalog 任务切碎, 剩下的
    //   ~4.6KB 连续块不足以创建 4608 栈的 radio_stream 任务。
    //
    //   现在: 即使这次没起来, 也保留播放意图, 并交给一个自带退避的 worker
    //   继续重试 —— 堆稍并在目录任务退出后自然合并, 播放会自己爬回来。
    bool restarted = false;
    if (pause.was_playing) {
        s_wanted_playing.store(true, std::memory_order_release);
        restarted = start_player_task_locked();
        if (restarted) {
            const TaskHandle_t task = s_player_task.load(std::memory_order_acquire);
            if (task) xTaskNotifyGive(task);
        }
    }
    xSemaphoreGive(s_worker_mutex);
    if (pause.was_playing && !restarted) {
        ESP_LOGE(kTag, "playback restart failed after catalog; scheduling retry worker");
        radio_schedule_playback_retry();
    }
    radio_wifi_set_streaming(pause.was_playing && s_wanted_playing.load());
}
bool radio_player_init() {
    s_station_mutex = xSemaphoreCreateMutex();
    s_worker_mutex = xSemaphoreCreateMutex();
    if (!s_pool_mutex) s_pool_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_station_mutex || !s_worker_mutex || !s_pool_mutex) return false;
    std::memcpy(s_stations, kFallbackStations, sizeof(kFallbackStations));
    s_station_count.store(sizeof(kFallbackStations) / sizeof(kFallbackStations[0]));

    const uint8_t saved_station = load_setting("station", 0);
    const uint8_t saved_volume = load_setting("volume", 55);
    nvs_handle_t catalog_handle;
    if (nvs_open("radio", NVS_READONLY, &catalog_handle) == ESP_OK) {
        std::size_t bytes = sizeof(s_stations);
        if (nvs_get_blob(catalog_handle, "catalog_v1", s_stations, &bytes) == ESP_OK &&
            bytes > 0 && bytes % sizeof(RadioStation) == 0 && bytes <= sizeof(s_stations)) {
            const std::size_t cached_count = bytes / sizeof(RadioStation);
            bool valid = true;
            for (std::size_t i = 0; i < cached_count; ++i) {
                valid = valid && std::memchr(s_stations[i].name, 0, sizeof(s_stations[i].name)) &&
                        std::memchr(s_stations[i].url, 0, sizeof(s_stations[i].url)) &&
                        std::strncmp(s_stations[i].url, "http", 4) == 0;
            }
            if (valid) {
                s_station_count.store(cached_count);
                s_catalog_cached = true;
                ESP_LOGI(kTag, "Restored cached radio catalog: %u stations", (unsigned)cached_count);
            } else {
                std::memcpy(s_stations, kFallbackStations, sizeof(kFallbackStations));
            }
        }
        nvs_close(catalog_handle);
    }
    s_station_index.store(saved_station % radio_player_station_count());
    s_volume.store(std::clamp<uint8_t>(saved_volume, 10, 100));

    if (bsp_audio_init() != ESP_OK) return false;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    bsp_audio_set_volume(s_volume.load());

    radio_ui_set_station(s_station_index.load(), radio_player_station_count(),
                         radio_player_station(s_station_index.load()));
    radio_ui_set_volume(s_volume.load());
    // 优先级 5: 必须**高于** LVGL 刷新任务(默认 prio=4)。此前降到 3 低于 UI,
    // 导致 UI 刷新(尤其电平条)抢占播放任务, I2S DMA 断流 ->
    // 实测"声音一顿一顿 + 电流声"。5 既能压过 UI 保证不断流, 又因 I2S DMA
    // 缓冲已加大, 写入会在缓冲满时正常阻塞让出 CPU, 不会饿死 IDLE。
    // 优先级 5 (> LVGL 默认 4) 是 2026-10-08 验证过的值, 保持不动。
    xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
    bool started = start_player_task_locked();
    xSemaphoreGive(s_worker_mutex);
    return started;
}

void radio_player_set_network(bool connected) {
    s_network_connected.store(connected, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

void radio_player_toggle() {
    radio_player_set_playing(!radio_player_is_playing());
}

void radio_player_set_playing(bool playing) {
    if (!s_worker_mutex) return;
    xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
    const bool started = set_playing_locked(playing);
    xSemaphoreGive(s_worker_mutex);
    if (!started) {
        ESP_LOGE(kTag, "playback task could not restart");
        radio_ui_set_playback(RadioPlaybackState::Error, "播放内存不足");
    }
    if (!playing) radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
    radio_wifi_set_streaming(playing && started);
}

bool radio_player_is_playing() {
    return s_wanted_playing.load(std::memory_order_acquire);
}

bool radio_player_is_stream_active() {
    return s_stream_active.load(std::memory_order_acquire);
}

bool radio_player_wait_idle(uint32_t timeout_ms) {
    if (!s_worker_mutex) return !s_stream_active.load(std::memory_order_acquire);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true) {
        xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
        TaskHandle_t task = s_player_task.load(std::memory_order_acquire);
        const bool stopping = !s_wanted_playing.load(std::memory_order_acquire);
        const bool stream_idle = !s_stream_active.load(std::memory_order_acquire);
        if (task && stopping && stream_idle) {
            /* Once stream_station has returned, the worker owns no HTTP client,
             * decoder, ring buffer, or pool lock. It may still be waiting in a
             * retry delay, so do not require its quiescent flag: remove the
             * leftover worker now and return its task stack to XiaoZhi. */
            vTaskSuspend(task);
            vTaskDelete(task);
            s_player_task.store(nullptr, std::memory_order_release);
            s_worker_quiescent = false;
            task = nullptr;
        }
        bool idle = stream_idle && (!stopping || !task);
        xSemaphoreGive(s_worker_mutex);
        if (idle) return true;
        if (static_cast<int32_t>(deadline - xTaskGetTickCount()) <= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void radio_player_select_relative(int delta) {
    const int count = static_cast<int>(radio_player_station_count());
    const int current = static_cast<int>(s_station_index.load());
    const std::size_t next = static_cast<std::size_t>((current + delta + count) % count);
    radio_player_set_station(next);
}

void radio_player_set_station(std::size_t index) {
    index %= radio_player_station_count();
    s_station_index.store(index, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    save_setting("station", static_cast<uint8_t>(index));
    const RadioStation chosen = radio_player_station(index);
    save_station_name(chosen.name);
    radio_ui_set_station(index, radio_player_station_count(), radio_player_station(index));
    if (radio_player_is_playing()) {
        radio_ui_set_playback(RadioPlaybackState::Connecting, "正在切换电台");
    }
}

std::size_t radio_player_station_index() {
    return s_station_index.load(std::memory_order_acquire);
}

bool radio_player_has_cached_catalog() { return s_catalog_cached; }

std::size_t radio_player_station_count() {
    return std::max<std::size_t>(s_station_count.load(std::memory_order_acquire), 1);
}

RadioStation radio_player_station(std::size_t index) {
    return station_snapshot(index);
}

bool radio_player_replace_stations(const RadioStation *stations, std::size_t count) {
    std::size_t selected = 0;
    if (!install_stations(stations, count, nullptr, &selected)) return false;

    save_setting("station", static_cast<uint8_t>(selected));
    const RadioStation chosen = radio_player_station(selected);
    save_station_name(chosen.name);
    radio_ui_set_station(selected, radio_player_station_count(), chosen);
    ESP_LOGI(kTag, "Installed local catalog with %u stations",
             static_cast<unsigned>(radio_player_station_count()));
    return true;
}

bool radio_player_replace_stations_resuming(const RadioStation *stations,
                                           std::size_t count) {
    char remembered[sizeof(RadioStation::name)] = {};
    const bool has_remembered = load_station_name(remembered, sizeof(remembered));

    // 【顺序策略】本地城市台排**前**, 实测可播的兜底台排**后**。
    //
    // 2026-10-09 修正: 早期把兜底台放前面, 理由是"公共目录里 CN 电台多为
    // 加密/防盗链流"。但实测发现那个结论**只对 https 版本成立** ——
    // radio-browser 返回的 `https://lhttp.qtfm.cn/...` 经 normalize_stream_url
    // 降级为 `http://` 后**完全可播**(实测重庆/北京/上海/广州多个城市台
    // 剥离 ICY 后 MP3 帧都正常, metaint=16000)。且用户明确要"本地电台"。
    // 故改为: 目录搜索结果(本地城市台)优先, 兜底台随后补齐。
    //
    // 曾经试过在 catalog 里逐条探测首字节过滤坏源, 但 esp_http_client 每次
    // 分配较大缓冲, 在 6KB 栈的 catalog 任务里连续探测会让堆从 50KB 掉到 6KB
    // 并触发 wifi:mem fail —— 代价太大, 已放弃。改为"播放时首字节校验 + 退避换台"
    // (radio_player.cc 内已有) 兜住。
    // static: RadioStation 约 418B, 14 项 = 5.7KB, 放栈上会撑爆 6KB 的
    // radio_catalog 任务栈(实测 Stack protection fault 循环重启)。
    auto *merged = static_cast<RadioStation *>(calloc(kMaxStations, sizeof(RadioStation)));
    if (!merged) return false;
    std::size_t merged_count = 0;
    // 1) 先放目录搜索结果(本地城市台 / 全国热门), 去重
    for (std::size_t i = 0; i < count && merged_count < kMaxStations; ++i) {
        bool dup = false;
        for (std::size_t j = 0; j < merged_count; ++j) {
            if (std::strcmp(merged[j].url, stations[i].url) == 0) { dup = true; break; }
        }
        if (!dup) merged[merged_count++] = stations[i];
    }
    // 2) 再用实测可播的兜底台补齐(去重), 保证列表里始终有稳定能出声的台
    for (std::size_t i = 0; i < sizeof(kFallbackStations) / sizeof(kFallbackStations[0]) &&
                            merged_count < kMaxStations; ++i) {
        bool dup = false;
        for (std::size_t j = 0; j < merged_count; ++j) {
            if (std::strcmp(merged[j].url, kFallbackStations[i].url) == 0) { dup = true; break; }
        }
        if (!dup) merged[merged_count++] = kFallbackStations[i];
    }

    std::size_t selected = 0;
    if (!install_stations(merged, merged_count, has_remembered ? remembered : nullptr,
                          &selected)) {
        free(merged);
        return false;
    }
    s_catalog_cached = true;
    nvs_handle_t catalog_handle;
    if (nvs_open("radio", NVS_READWRITE, &catalog_handle) == ESP_OK) {
        esp_err_t saved = nvs_set_blob(catalog_handle, "catalog_v1", merged,
                                      merged_count * sizeof(RadioStation));
        if (saved == ESP_OK) saved = nvs_commit(catalog_handle);
        if (saved != ESP_OK) ESP_LOGW(kTag, "catalog cache save failed: %s", esp_err_to_name(saved));
        nvs_close(catalog_handle);
    }
    free(merged);

    save_setting("station", static_cast<uint8_t>(selected));
    const RadioStation chosen = radio_player_station(selected);
    save_station_name(chosen.name);
    radio_ui_set_station(selected, radio_player_station_count(), chosen);
    if (has_remembered && selected > 0) {
        ESP_LOGI(kTag, "Resumed remembered station '%s' at index %u",
                 chosen.name, static_cast<unsigned>(selected));
    } else {
        ESP_LOGI(kTag, "Installed local catalog with %u stations",
                 static_cast<unsigned>(radio_player_station_count()));
    }
    return true;
}

void radio_player_set_volume(uint8_t volume) {
    volume = std::clamp<uint8_t>(volume, 10, 100);
    s_volume.store(volume, std::memory_order_release);
    // 播放任务连续占用音频锁时，UI 同步等锁会被饿死；交给下一帧应用。
    save_setting("volume", volume);
    radio_ui_set_volume(volume);
}

uint8_t radio_player_volume() {
    return s_volume.load(std::memory_order_acquire);
}

// ---- C-linkage bridge for xiaozhi_client.c ----
extern "C" void radio_pause_for_xiaozhi(void) {
    radio_player_set_playing(false);
    radio_player_wait_idle(2000);
}
extern "C" bool radio_is_playing_c(void) {
    return radio_player_is_playing();
}
