#include "radio_ui.h"

#include "radio_display_settings.h"
#include "radio_ui_city.h"
#include "radio_home.h"
#include "radio_sleep_timer.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "ui_pixel.h"   // ui_cn_font(): 电台名用全覆盖字体(lv_font_cn_16)
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "radio_font.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

// plays/438「口袋百宝箱」荧光绿单色配色。
// 色值从官方主图逐像素采样得到(见 ui_phosphor.h 的说明),不是凭感觉调的。
// 关键是只用两三个明度层级:纯黑底 -> 深绿面 -> 亮绿字,靠描边分层,不用阴影。
constexpr uint32_t kBackground = 0x000F05;  // 纯黑绿底
constexpr uint32_t kPanel      = 0x0A251A;  // 卡片面
constexpr uint32_t kPanelSoft  = 0x123D28;  // 次级填充 / 进度槽
constexpr uint32_t kGrid       = 0x2ECC7F;  // 描边 / 分割线
constexpr uint32_t kText       = 0x9AFCC6;  // 主前景(亮绿)
constexpr uint32_t kMuted      = 0x5FD9A0;  // 次要文字
constexpr uint32_t kAmber      = 0xD8FF7A;  // 选中/激活(偏黄的绿,保持同色相)
constexpr uint32_t kAmberSoft  = 0x14512F;  // 选中态底色
constexpr uint32_t kGreen      = 0x9AFCC6;  // 网络正常(绿即成功,不引入第二色相)
constexpr uint32_t kRed        = 0xFF9E9E;  // 出错时才破例用偏红色

// Dial geometry. The scale spans 88-108 MHz, the band used by Chinese FM
// broadcasting, so a real frequency maps onto it directly.
constexpr int kDialBandLow = 880;    // 88.0 MHz in tenths
constexpr int kDialBandHigh = 1080;  // 108.0 MHz in tenths
constexpr int kDialTrackLeft = 16;
constexpr int kDialTrackRight = 198;
constexpr int kDialNeedleWidth = 3;
constexpr int kDialNeedleHome = (kDialTrackLeft + kDialTrackRight) / 2;
constexpr std::size_t kMeterCount = 18;
constexpr std::size_t kWifiVisibleRows = 6;
constexpr std::size_t kWifiMaxKeys = 34;
constexpr uint16_t kVideoWidth = 240;
constexpr uint16_t kVideoHeight = 320;
constexpr uint16_t kJpegMcuHeight = 16;
constexpr uint8_t kMaxVideoScale = 2;
constexpr std::size_t kJpegWorkSize = 4096;

enum class Page {
    Main,
    Settings,
    Config,
    WifiList,
    WifiKeyboard,
    WifiConnecting,
    EasterEgg,
};

constexpr const char *kLowerKeys[] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
    "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x",
    "y", "z", "ABC", "123", "<", "GO",
};
constexpr const char *kUpperKeys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",
    "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "abc", "123", "<", "GO",
};
constexpr const char *kSymbolKeys[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", "_",
    ".", "@", "#", "$", "%", "&", "*", "!", "+", "=", "?", "/",
    ":", ";", "(", ")", "[", "]", "abc", "ABC", "<", "GO",
};

lv_obj_t *s_screen;
lv_obj_t *s_main_group;
lv_obj_t *s_station_number;
lv_obj_t *s_station_name;
lv_obj_t *s_station_description;
lv_obj_t *s_play_icon;
lv_obj_t *s_status;
lv_obj_t *s_location;
lv_obj_t *s_clock;
lv_obj_t *s_battery;
lv_obj_t *s_sleep_timer;
lv_obj_t *s_wifi_bars[3];
lv_obj_t *s_meter[kMeterCount];
lv_obj_t *s_volume_value;
lv_obj_t *s_volume_bar;
// 列表页行对象池。城市列表按 9 行分页, 其余页面最多 9 行, 留到 10 防越界。
constexpr std::size_t kSettingsRowsMax = 10;
lv_obj_t *s_settings_rows[kSettingsRowsMax];
lv_obj_t *s_settings_labels[kSettingsRowsMax];
lv_obj_t *s_pair_dot;
lv_obj_t *s_dial_needle;
lv_obj_t *s_dial_glow;
lv_obj_t *s_dial_readout;
lv_obj_t *s_wifi_state;
lv_obj_t *s_wifi_list_hint;
lv_obj_t *s_wifi_row_box[kWifiVisibleRows];
lv_obj_t *s_wifi_row_ssid[kWifiVisibleRows];
lv_obj_t *s_wifi_row_info[kWifiVisibleRows];
lv_obj_t *s_wifi_keyboard_network;
lv_obj_t *s_wifi_password_mask;
lv_obj_t *s_wifi_keyboard_status;
lv_obj_t *s_easter_message;
lv_obj_t *s_wifi_key_box[kWifiMaxKeys];
lv_obj_t *s_wifi_key_label[kWifiMaxKeys];
uint8_t *s_jpeg_work;
uint16_t *s_video_stripe;
bool s_video_color_pending;

struct DirectJpegContext {
    const uint8_t *data;
    std::size_t size;
    std::size_t position;
    uint16_t source_width;
    uint16_t source_height;
    uint8_t scale;
    bool failed;
};

Page s_page = Page::Main;
// LEO 原版页面当前是否可见。timer_callback(50ms) 只在 true 时更新
// LEO 的对象;切到 plays/438 首页或 Codex 页后置 false,跳过后台页面更新。
bool s_page_visible = false;
bool s_network_connected;
uint8_t s_volume = 55;
RadioSettingsPage s_settings_page = RadioSettingsPage::Menu;
std::size_t s_settings_selected;
// 城市列表分页: 行池只有 10 格, 而城市有 49 项(自动定位 + 表内全部城市),
// 所以一次只显示一页, s_city_offset 记录当前页第一项的绝对索引。
std::size_t s_city_offset = 0;
char s_city_title[48] = "城市电台";
std::size_t s_settings_row_count;
std::size_t s_station_index;
std::size_t s_station_count = 1;
char s_station_name_text[64] = "清晨音乐";
char s_station_description_text[96] = "轻松音乐 · 网络直播";
char s_network_detail[64] = "等待网络";
char s_location_text[48] = "NATIONAL";
char s_playback_detail[64] = "准备播放";
RadioPlaybackState s_playback_state = RadioPlaybackState::Stopped;
std::atomic<uint32_t> s_level_generation{0};
uint8_t s_levels[kMeterCount] = {};
uint32_t s_last_level_generation;
// 上一帧实际下发的高度/颜色, 用于跳过无变化的 set_pos/set_size/set_style
// (减少 LVGL 重绘 -> 降低 UI 对播放任务的抢占)。
int      s_meter_height[kMeterCount] = {};
uint32_t s_meter_color[kMeterCount] = {};
uint8_t s_animation;
uint32_t s_battery_ticks;
uint32_t s_last_sleep_seconds = UINT32_MAX;
uint16_t s_station_frequency;
int32_t s_needle_x = kDialNeedleHome;

bool drain_video_color_transfers() {
    if (!bsp_display_io()) return true;
    const esp_err_t result =
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    if (result != ESP_OK) {
        ESP_LOGE("radio_ui", "Panel color drain failed: %s",
                 esp_err_to_name(result));
        return false;
    }
    return true;
}

std::size_t direct_jpeg_input(JDEC *decoder, uint8_t *buffer,
                              std::size_t requested) {
    auto *context = static_cast<DirectJpegContext *>(decoder->device);
    if (!context || context->position >= context->size) return 0;
    const std::size_t count =
        std::min(requested, context->size - context->position);
    if (buffer) std::memcpy(buffer, context->data + context->position, count);
    context->position += count;
    return count;
}

int direct_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rectangle) {
    auto *context = static_cast<DirectJpegContext *>(decoder->device);
    if (!context || !bitmap || !rectangle || context->failed) return 0;

    const uint16_t block_width = rectangle->right - rectangle->left + 1;
    const uint16_t block_height = rectangle->bottom - rectangle->top + 1;
    if (rectangle->right >= context->source_width ||
        rectangle->bottom >= context->source_height) {
        context->failed = true;
        return 0;
    }

    if (rectangle->left == 0 && s_video_color_pending) {
        // The shared stripe is still owned by the previous color transfer.
        // A parameter command drains every queued color transaction before we
        // overwrite it with the next band.
        if (!drain_video_color_transfers()) {
            context->failed = true;
            return 0;
        }
        s_video_color_pending = false;
    }
    uint16_t *target = s_video_stripe;
    if (!target) {
        context->failed = true;
        return 0;
    }

    // TJPGD's JD_FORMAT=0 output is stored as B, G, R bytes (matching
    // LVGL's little-endian RGB888 layout), not conventional R, G, B.
    const auto *bgr = static_cast<const uint8_t *>(bitmap);
    for (uint16_t y = 0; y < block_height; ++y) {
        for (uint16_t x = 0; x < block_width; ++x) {
            const std::size_t input =
                (static_cast<std::size_t>(y) * block_width + x) * 3U;
            const uint16_t color =
                static_cast<uint16_t>(((bgr[input + 2] & 0xf8U) << 8U) |
                                      ((bgr[input + 1] & 0xfcU) << 3U) |
                                      (bgr[input] >> 3U));
            const uint16_t panel_color = __builtin_bswap16(color);
            const uint16_t output_x =
                static_cast<uint16_t>((rectangle->left + x) * context->scale);
            const uint16_t output_y = static_cast<uint16_t>(y * context->scale);
            for (uint8_t repeat_y = 0; repeat_y < context->scale; ++repeat_y) {
                uint16_t *row = target +
                    static_cast<std::size_t>(output_y + repeat_y) * kVideoWidth;
                for (uint8_t repeat_x = 0; repeat_x < context->scale; ++repeat_x) {
                    row[output_x + repeat_x] = panel_color;
                }
            }
        }
    }

    if (rectangle->right + 1U == context->source_width) {
        const uint16_t panel_top = rectangle->top * context->scale;
        const uint16_t panel_bottom =
            static_cast<uint16_t>((rectangle->bottom + 1U) * context->scale);
        if (esp_lcd_panel_draw_bitmap(bsp_display_panel(), 0, panel_top,
                                      kVideoWidth, panel_bottom, target) != ESP_OK) {
            context->failed = true;
            return 0;
        }
        s_video_color_pending = true;
    }
    return 1;
}

bool ensure_direct_video_buffers() {
    if (!s_jpeg_work) {
        s_jpeg_work = static_cast<uint8_t *>(std::malloc(kJpegWorkSize));
    }
    constexpr std::size_t kStripePixels =
        static_cast<std::size_t>(kVideoWidth) * kJpegMcuHeight * kMaxVideoScale;
    if (!s_video_stripe) {
        s_video_stripe = static_cast<uint16_t *>(
            std::malloc(kStripePixels * sizeof(uint16_t)));
    }
    return s_jpeg_work && s_video_stripe;
}

void release_direct_video_buffers() {
    // Color transfers are queued by the SPI panel driver. A parameter command
    // first drains every in-flight color transaction, so the buffers are no
    // longer owned by DMA when they are released below. ST7789 command 0x00 is
    // NOP and does not alter the visible frame.
    if (bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }
    std::free(s_jpeg_work);
    s_jpeg_work = nullptr;
    std::free(s_video_stripe);
    s_video_stripe = nullptr;
    s_video_color_pending = false;
}

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height,
                   uint32_t color, int radius = 0) {
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    return object;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, int width,
                     uint32_t color, const lv_font_t *font = nullptr) {
    // 默认字体改成全量中文字体。radio_font 是**白名单**字体(约 450 字),
    // 城市名(番禺/嘉陵/北碚…)、提示语("翻页"…)只要不在名单里就渲染成"口"。
    // lv_font_cn_16 覆盖 GB2312 全集, 已在固件内, 不增加体积。
    if (font == nullptr) font = ui_cn_font();
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(object, LV_OPA_COVER, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

void set_centered(lv_obj_t *object) {
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
}

const char *playback_text() {
    switch (s_playback_state) {
        case RadioPlaybackState::Stopped: return "已暂停";
        case RadioPlaybackState::Connecting: return "正在连接电台";
        case RadioPlaybackState::Buffering: return "正在缓冲";
        case RadioPlaybackState::Playing: return "正在播放";
        case RadioPlaybackState::Reconnecting: return "信号中断，正在重连";
        case RadioPlaybackState::Error: return "暂时无法播放";
    }
    return "准备播放";
}

void apply_network() {
    for (int i = 0; i < 3; ++i) {
        if (s_wifi_bars[i]) {
            // 用颜色明暗表达"未连接", 不用 style_opa —— style_opa < 255 会让
            // LVGL 走 simple layer, 单次就 malloc 24KB(见 sdkconfig 里
            // LV_DRAW_LAYER_SIMPLE_BUF_SIZE)。C3 播放时堆只剩几 KB, 那条
            // 路径一失败就是绘制异常/看门狗。
            lv_obj_set_style_bg_color(s_wifi_bars[i],
                                      lv_color_hex(s_network_connected ? kGreen : kPanelSoft), 0);
        }
    }
}

// Maps a station onto the dial. A real frequency lands where it belongs on the
// scale. Most directory entries carry no frequency, so those stations are
// spread evenly across the band by their position in the list, which keeps the
// needle meaningful (it still says "which station") and always moving.
int dial_x_for_station() {
    const int span = kDialTrackRight - kDialTrackLeft;
    if (s_station_frequency >= kDialBandLow && s_station_frequency <= kDialBandHigh) {
        const int offset = (s_station_frequency - kDialBandLow) * span /
                           (kDialBandHigh - kDialBandLow);
        return kDialTrackLeft + offset;
    }
    // Inset the synthetic positions so the needle never pins to either end,
    // which would read as "stuck" rather than "tuned".
    const int count = static_cast<int>(std::max<std::size_t>(s_station_count, 1));
    const int index = static_cast<int>(std::min<std::size_t>(s_station_index,
                                                             s_station_count - 1));
    const int inset = span / 8;
    const int usable = span - inset * 2;
    const int offset = count > 1 ? inset + usable * index / (count - 1)
                                 : inset + usable / 2;
    return kDialTrackLeft + offset;
}

void place_needle(int32_t x) {
    s_needle_x = x;
    if (s_dial_needle) {
        lv_obj_set_pos(s_dial_needle, static_cast<int>(x) - kDialNeedleWidth / 2, 27);
    }
    if (s_dial_glow) {
        lv_obj_set_pos(s_dial_glow, static_cast<int>(x) - 9, 33);
    }
}

void needle_anim_cb(void *, int32_t value) { place_needle(value); }

void apply_dial(bool animate) {
    if (!s_dial_needle) return;
    const int target = dial_x_for_station();

    if (s_dial_readout) {
        char text[16];
        if (s_station_frequency >= kDialBandLow && s_station_frequency <= kDialBandHigh) {
            std::snprintf(text, sizeof(text), "%u.%u",
                          static_cast<unsigned>(s_station_frequency / 10U),
                          static_cast<unsigned>(s_station_frequency % 10U));
        } else {
            // Without a real frequency the readout stays blank rather than
            // inventing a number the station does not broadcast on.
            text[0] = '\0';
        }
        lv_label_set_text(s_dial_readout, text);
    }

    lv_anim_delete(s_dial_needle, needle_anim_cb);
    if (!animate || s_needle_x == target) {
        place_needle(target);
        return;
    }

    // Sweep time scales with distance so a neighbouring station feels quick and
    // a jump across the band still reads as a deliberate tune.
    const int distance = std::abs(target - static_cast<int>(s_needle_x));
    const uint32_t duration =
        static_cast<uint32_t>(std::clamp(160 + distance * 3, 200, 620));

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_dial_needle);
    lv_anim_set_exec_cb(&animation, needle_anim_cb);
    lv_anim_set_values(&animation, s_needle_x, target);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

void apply_station() {
    if (!s_station_name || !s_station_number || !s_station_description) return;
    char preset[20];
    std::snprintf(preset, sizeof(preset), "CH %02u / %02u",
                  static_cast<unsigned>(s_station_index + 1),
                  static_cast<unsigned>(s_station_count));
    lv_label_set_text(s_station_number, preset);
    lv_label_set_text(s_station_name, s_station_name_text);
    lv_label_set_text(s_station_description, s_station_description_text);
    apply_dial(true);
}

void apply_playback() {
    if (!s_status || !s_play_icon) return;
    lv_label_set_text(s_status, s_playback_detail[0] ? s_playback_detail : playback_text());
    const bool playing = s_playback_state == RadioPlaybackState::Playing ||
                         s_playback_state == RadioPlaybackState::Buffering ||
                         s_playback_state == RadioPlaybackState::Connecting ||
                         s_playback_state == RadioPlaybackState::Reconnecting;
    lv_label_set_text(s_play_icon, playing ? "II" : ">" );
    lv_obj_set_style_text_color(s_play_icon,
                                lv_color_hex(s_playback_state == RadioPlaybackState::Error
                                                 ? kRed
                                                 : kAmber), 0);
}

void apply_volume() {
    if (!s_volume_value || !s_volume_bar) return;
    char text[16];
    std::snprintf(text, sizeof(text), "%u%%", s_volume);
    lv_label_set_text(s_volume_value, text);
    lv_bar_set_value(s_volume_bar, s_volume, LV_ANIM_ON);
}

void apply_sleep_timer() {
    if (!s_sleep_timer) return;
    const uint32_t seconds = radio_sleep_timer_remaining_seconds();
    if (seconds == s_last_sleep_seconds) return;
    s_last_sleep_seconds = seconds;
    if (!seconds) {
        lv_label_set_text(s_sleep_timer, "");
        return;
    }
    char text[24];
    std::snprintf(text, sizeof(text), "定时 %02u:%02u",
                  static_cast<unsigned>(seconds / 60U),
                  static_cast<unsigned>(seconds % 60U));
    lv_label_set_text(s_sleep_timer, text);
}

void draw_dial_ticks(lv_event_t *event) {
    lv_obj_t *dial = static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_area_t origin;
    lv_obj_get_coords(dial, &origin);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.radius = 1;
    for (int i = 0; i <= 40; i += 2) {
        const bool major = i % 8 == 0;
        const int x = origin.x1 + 1 + kDialTrackLeft +
                      i * (kDialTrackRight - kDialTrackLeft) / 40;
        const int height = major ? 11 : 6;
        lv_area_t tick = {x, origin.y1 + 1 + 36 - height,
                          x + (major ? 1 : 0), origin.y1 + 36};
        descriptor.bg_color = lv_color_hex(major ? kGrid : kAmberSoft);
        lv_draw_rect(layer, &descriptor, &tick);
    }
}

void draw_meter_bars(lv_event_t *event) {
    lv_area_t origin;
    lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(event)), &origin);
    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.radius = 2;
    for (std::size_t i = 0; i < kMeterCount; ++i) {
        const int height = 3 + s_levels[i] * 34 / 100;
        descriptor.bg_color = lv_color_hex(s_levels[i] > 85 ? kRed :
                                           s_levels[i] > 62 ? kAmber : kGreen);
        const int x = origin.x1 + 9 + static_cast<int>(i) * 11;
        lv_area_t bar = {x, origin.y1 + 40 - height, x + 5, origin.y1 + 39};
        lv_draw_rect(lv_event_get_layer(event), &descriptor, &bar);
    }
}

void build_main() {
    // lv_obj_clean 会销毁 s_screen 下所有子对象;随后重置相关指针并重建页面。
    lv_obj_clean(s_screen);
    lv_screen_load(s_screen);   // 进应用时菜单屏可能已被删, 必须重设活动屏
    s_page = Page::Main;
    s_battery_ticks = 30;       // 主界面一上屏就尽快刷新电量(见 timer_callback)
    s_page_visible = true;
    s_station_number = nullptr;
    s_station_name = nullptr;
    s_station_description = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_location = nullptr;
    s_volume_value = nullptr;
    s_volume_bar = nullptr;
    s_pair_dot = nullptr;
    s_sleep_timer = nullptr;
    s_dial_needle = nullptr;
    s_dial_glow = nullptr;
    s_dial_readout = nullptr;
    s_needle_x = kDialNeedleHome;
    s_last_sleep_seconds = UINT32_MAX;

    s_main_group = make_box(s_screen, 0, 0, 240, 320, kBackground);
    // 左侧标题: 屏幕四角有圆角, 内收 10px 避免被圆角遮挡
    make_label(s_main_group, "LEO RADIO", 10, 10, 92, kAmber,
               &lv_font_montserrat_14);
    // 状态栏三元素统一用 14px 字体 + 同一垂直基线(y=8)。
    s_clock = make_label(s_main_group, "--:--", 94, 8, 52, kText,
                         &lv_font_montserrat_14);
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_CENTER, 0);
    // 电量: 右对齐到屏幕右边缘内收 10px(避让圆角)。位置由 apply_battery()
    // 每次刷新时重设, 这里只创建对象。
    s_battery = make_label(s_main_group, "--%", 182, 8, 40, kMuted,
                           &lv_font_montserrat_14);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    // 信号格(3 格): 紧贴电量左侧。apply_battery() 会按电量实际 x 重排。
    for (int i = 0; i < 3; ++i) {
        const int height = 5 + i * 3;
        s_wifi_bars[i] = make_box(s_main_group, 168 + i * 5, 19 - height,
                                  3, height, kMuted, 1);
    }
    make_box(s_main_group, 12, 28, 216, 1, kGrid);

    s_station_number = make_label(s_main_group, "CH 01 / 06", 13, 48, 102, kAmber,
                                  &lv_font_montserrat_14);
    s_sleep_timer = make_label(s_main_group, "", 115, 48, 112, kAmber);
    lv_obj_set_style_text_align(s_sleep_timer, LV_TEXT_ALIGN_RIGHT, 0);
    // ⚠ 电台名用 ui_cn_font()(= lv_font_cn_16, 6600+ 汉字) 而非白名单 radio_font。
    // 原因: 本地/城市电台名是**运行时从网络目录动态获取**的(如"北碚综合广播·
    // 重庆嘉陵之声FM88.7""广州市番禺区广播电台"), 无法预先枚举进白名单字体,
    // 用 radio_font 必然出现方框"口"。lv_font_cn_16 覆盖常用字, 一劳永逸。
    s_station_name = make_label(s_main_group, "清晨音乐", 12, 70, 216, kText,
                                ui_cn_font());
    set_centered(s_station_name);
    s_station_description = make_label(s_main_group, "轻松音乐 · 网络直播",
                                       12, 109, 216, kMuted, ui_cn_font());
    set_centered(s_station_description);

    lv_obj_t *dial = make_box(s_main_group, 13, 135, 214, 69, kPanel, 8);
    lv_obj_set_style_border_color(dial, lv_color_hex(kGrid), 0);
    lv_obj_set_style_border_width(dial, 1, 0);

    // Tick marks every 0.5 MHz across the 88-108 band, with taller marks and a
    // printed number on each whole 4 MHz step.
    // 静态刻度直接绘制，避免 21 个装饰对象占用播放时所需的堆。
    lv_obj_add_event_cb(dial, draw_dial_ticks, LV_EVENT_DRAW_MAIN, nullptr);
    for (int mhz = 88; mhz <= 108; mhz += 4) {
        const int x = kDialTrackLeft + (mhz - 88) *
                      (kDialTrackRight - kDialTrackLeft) / 20;
        make_label(dial, mhz == 88 ? "88" : mhz == 92 ? "92" : mhz == 96 ? "96"
                       : mhz == 100 ? "100" : mhz == 104 ? "104" : "108",
                   x - 15, 6, 30, kMuted, &lv_font_montserrat_14);
    }
    make_box(dial, kDialTrackLeft, 36, kDialTrackRight - kDialTrackLeft + 1, 2,
             kGrid, 1);

    // Soft halo under the needle so the sweep reads clearly against the ticks.
    // 预混成暗色而不是半透明: 避免 simple layer 的 24KB 临时分配。
    s_dial_glow = make_box(dial, kDialNeedleHome - 9, 33, 18, 8, 0x2A3D1E, 4);
    s_dial_needle = make_box(dial, kDialNeedleHome - kDialNeedleWidth / 2, 27,
                             kDialNeedleWidth, 22, kAmber, 1);

    s_dial_readout = make_label(dial, "", 138, 47, 68, kAmber,
                                &lv_font_montserrat_14);
    lv_obj_set_style_text_align(s_dial_readout, LV_TEXT_ALIGN_RIGHT, 0);
    s_location = make_label(dial, s_location_text, 8, 47, 130, kAmber,
                            ui_cn_font());

    // 圆角档位尽量收敛(与 dial 同为 8): LVGL 的圆角遮罩缓存槽位有限
    // (LV_DRAW_SW_CIRCLE_CACHE_SIZE), 档位一多就要每帧重新 malloc 遮罩,
    // 堆紧张时那次分配失败会让绘制路径直接崩(实测 c=NULL -> Load access fault)。
    lv_obj_t *meter_panel = make_box(s_main_group, 13, 214, 214, 49, kPanelSoft, 8);
    s_meter[0] = meter_panel;
    lv_obj_add_event_cb(meter_panel, draw_meter_bars, LV_EVENT_DRAW_MAIN, nullptr);

    s_play_icon = make_label(s_main_group, ">", 13, 270, 24, kAmber,
                             &lv_font_montserrat_20);
    s_status = make_label(s_main_group, "准备播放", 39, 272, 187, kText);
    make_box(s_main_group, 12, 296, 216, 1, kGrid);
    lv_obj_t *hint = make_label(s_main_group, "上下换台  确定播放  长按设置",
                                12, 300, 216, kMuted);
    set_centered(hint);

    apply_network();
    apply_station();
    apply_playback();
    apply_sleep_timer();
}

void build_config(const char *ssid, const char *url) {
    lv_obj_clean(s_screen);
    lv_screen_load(s_screen);   // 配网页: 确保 s_screen 是活动屏
    s_page = Page::Config;
    // 配网页仍在 s_screen 上,50ms 定时器需要继续工作(它负责配对点的
    // 呼吸动画),所以这里必须保持可见标志为 true。
    s_page_visible = true;
    s_station_number = nullptr;
    s_station_name = nullptr;
    s_station_description = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_location = nullptr;
    s_clock = nullptr;
    s_battery = nullptr;
    s_sleep_timer = nullptr;
    s_dial_needle = nullptr;
    s_dial_glow = nullptr;
    s_dial_readout = nullptr;
    for (auto &bar : s_wifi_bars) bar = nullptr;
    for (auto &bar : s_meter) bar = nullptr;
    std::memset(s_meter_height, 0, sizeof(s_meter_height));
    std::memset(s_meter_color, 0, sizeof(s_meter_color));

    make_label(s_screen, "联网收音机", 12, 12, 216, kText, ui_cn_font());
    lv_obj_t *tag = make_label(s_screen, "首次配网", 12, 45, 216, kAmber);
    set_centered(tag);
    s_pair_dot = make_box(s_screen, 112, 77, 16, 16, kRed, LV_RADIUS_CIRCLE);

    lv_obj_t *panel = make_box(s_screen, 18, 111, 204, 132, kPanel, 10);
    lv_obj_set_style_border_color(panel, lv_color_hex(kGrid), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_t *hint = make_label(panel, "手机连接热点", 0, 14, 204, kMuted);
    set_centered(hint);
    lv_obj_t *ssid_label = make_label(panel, ssid && ssid[0] ? ssid : "LEO Radio",
                                      8, 43, 188, kAmber);
    set_centered(ssid_label);
    hint = make_label(panel, "连接后会自动打开配网页面", 0, 77, 204, kText);
    set_centered(hint);
    lv_obj_t *address = make_label(panel, url && url[0] ? url : "192.168.4.1",
                                   0, 104, 204, kMuted,
                                   &lv_font_montserrat_14);
    set_centered(address);
    hint = make_label(s_screen, "请选择 2.4G 无线网络", 12, 259, 216, kText);
    set_centered(hint);
    hint = make_label(s_screen, "配网完成后会自动开始播放", 12, 286, 216, kMuted);
    set_centered(hint);
}

const lv_font_t *ssid_font(const char *ssid) {
    if (!ssid) return &lv_font_montserrat_14;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(ssid); *p; ++p) {
        if (*p >= 0x80) return ui_cn_font();
    }
    return &lv_font_montserrat_14;
}

void reset_transient_objects(Page page) {
    lv_obj_clean(s_screen);
    // 【关键】必须把 s_screen 设为当前活动屏。
    // 框架采用"单页面驻留": 进应用时会 lv_obj_delete 掉菜单屏。若这里不切屏,
    // 活动屏会指向被删对象 -> 屏幕卡死不动(音乐仍在独立任务里播)。
    // 本函数被所有设置/WiFi/彩蛋页的 build_* 调用, 一处覆盖全部。
    lv_screen_load(s_screen);
    s_page = page;
    // 新页面一上屏就尽快刷新电量(不等 3s): 让计数直接到阈值, 下个 tick 即刷。
    s_battery_ticks = 30;
    // 设置页 / WiFi 页同样画在 s_screen 上,50ms 定时器要继续工作。
    s_page_visible = true;
    s_station_number = nullptr;
    s_station_name = nullptr;
    s_station_description = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_location = nullptr;
    s_volume_value = nullptr;
    s_volume_bar = nullptr;
    s_clock = nullptr;
    s_battery = nullptr;
    s_sleep_timer = nullptr;
    s_pair_dot = nullptr;
    s_dial_needle = nullptr;
    s_dial_glow = nullptr;
    s_dial_readout = nullptr;
    s_wifi_state = nullptr;
    s_wifi_list_hint = nullptr;
    s_wifi_keyboard_network = nullptr;
    s_wifi_password_mask = nullptr;
    s_wifi_keyboard_status = nullptr;
    s_easter_message = nullptr;
    for (auto &bar : s_wifi_bars) bar = nullptr;
    for (auto &bar : s_meter) bar = nullptr;
    // 页面重建后旧的高度/颜色缓存失效, 清零以便新柱子首帧就正确绘制。
    std::memset(s_meter_height, 0, sizeof(s_meter_height));
    std::memset(s_meter_color, 0, sizeof(s_meter_color));
    for (auto &row : s_settings_rows) row = nullptr;
    for (auto &label : s_settings_labels) label = nullptr;
    for (auto &box : s_wifi_row_box) box = nullptr;
    for (auto &label : s_wifi_row_ssid) label = nullptr;
    for (auto &label : s_wifi_row_info) label = nullptr;
    for (auto &box : s_wifi_key_box) box = nullptr;
    for (auto &label : s_wifi_key_label) label = nullptr;
}

void apply_settings_selection() {
    const bool cities = s_settings_page == RadioSettingsPage::City;
    for (std::size_t i = 0; i < s_settings_row_count; ++i) {
        if (!s_settings_rows[i] || !s_settings_labels[i]) continue;
        const bool active = cities ? (s_city_offset + i == s_settings_selected)
                                   : (i == s_settings_selected);
        lv_obj_set_style_bg_color(s_settings_rows[i],
                                  lv_color_hex(active ? kAmber : kPanel), 0);
        lv_obj_set_style_text_color(s_settings_labels[i],
                                    lv_color_hex(active ? kBackground : kText), 0);
    }
}

void build_settings(RadioSettingsPage page, std::size_t selected, uint8_t volume) {
    reset_transient_objects(Page::Settings);
    s_settings_page = page;
    s_settings_selected = selected;
    s_volume = volume;
    s_settings_row_count = 0;

    if (page == RadioSettingsPage::Volume) {
        lv_obj_t *title = make_label(s_screen, "音量", 12, 16, 216, kText,
                                     ui_cn_font());
        set_centered(title);
        s_volume_value = make_label(s_screen, "55%", 12, 84, 216, kAmber,
                                    &lv_font_montserrat_20);
        set_centered(s_volume_value);
        s_volume_bar = lv_bar_create(s_screen);
        lv_obj_set_pos(s_volume_bar, 30, 136);
        lv_obj_set_size(s_volume_bar, 180, 14);
        lv_bar_set_range(s_volume_bar, 0, 100);
        lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(kGrid), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(kAmber), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_volume_bar, 7, LV_PART_MAIN);
        lv_obj_set_style_radius(s_volume_bar, 7, LV_PART_INDICATOR);
        lv_obj_t *hint = make_label(s_screen, "上下调节  确定返回",
                                    12, 276, 216, kMuted);
        set_centered(hint);
        apply_volume();
        return;
    }

    static constexpr const char *kMenuItems[] = {
        // NOTE: 本机硬件没有蓝牙, 菜单不再列出"蓝牙"占位项。
        "音量", "城市电台", "定时播放", "屏幕设置", "网络设置", "返回",
    };
    static constexpr const char *kSleepTimerItems[] = {
        "无定时", "15 分钟", "30 分钟", "60 分钟", "90 分钟", "返回",
    };
    char brightness_item[32];
    char timeout_item[48];
    std::snprintf(brightness_item, sizeof(brightness_item), "亮度 %u%%",
                  static_cast<unsigned>(radio_display_brightness()));
    const uint16_t timeout = radio_display_timeout_seconds();
    if (!timeout) {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 关闭");
    } else if (timeout < 60) {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 秒",
                      static_cast<unsigned>(timeout));
    } else {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 分钟",
                      static_cast<unsigned>(timeout / 60));
    }
    const char *display_items[] = {brightness_item, timeout_item, "返回"};
    static constexpr const char *kWifiMenuItems[] = {
        "连接新网络", "已存网络", "返回",
    };

    const char *const *items = nullptr;
    std::size_t item_count = 0;
    const char *title_text = "设置";
    const char *hint_text = "上下选择  确定进入";
    switch (page) {
        case RadioSettingsPage::Menu:
            items = kMenuItems;
            item_count = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
            break;
        case RadioSettingsPage::City: {
            // 城市表来自共享来源, 一屏 9 行 + 自动定位, 其余分页。
            item_count = city_choice_count();
            constexpr std::size_t kRows = 9;
            if (item_count > kRows) {
                const std::size_t pages = (item_count + kRows - 1) / kRows;
                const std::size_t page =
                    std::min(pages - 1, s_settings_selected / kRows);
                s_city_offset = std::min(item_count - 1, page * kRows);
            } else {
                s_city_offset = 0;
            }
            title_text = "城市电台";
            std::snprintf(s_city_title, sizeof(s_city_title), "城市电台 %u/%u",
                          static_cast<unsigned>(s_city_offset / 9 + 1),
                          static_cast<unsigned>((item_count + 8) / 9));
            title_text = s_city_title;
            hint_text = "上下选择  确定切换城市  长按返回";
            break;
        }
        case RadioSettingsPage::SleepTimer:
            items = kSleepTimerItems;
            item_count = sizeof(kSleepTimerItems) / sizeof(kSleepTimerItems[0]);
            title_text = "定时播放";
            hint_text = "确定开始  长按返回";
            break;
        case RadioSettingsPage::Display:
            items = display_items;
            item_count = sizeof(display_items) / sizeof(display_items[0]);
            title_text = "屏幕设置";
            hint_text = "确定切换  长按返回";
            break;
        case RadioSettingsPage::WifiMenu:
            items = kWifiMenuItems;
            item_count = sizeof(kWifiMenuItems) / sizeof(kWifiMenuItems[0]);
            title_text = "网络设置";
            hint_text = "确定进入  长按返回";
            break;
        default:
            return;
    }

    char city_row[24];
    const bool paging_cities = page == RadioSettingsPage::City;
    const std::size_t render_count =
        paging_cities ? std::min<std::size_t>(item_count - s_city_offset, 9) : item_count;
    const bool compact = render_count > 3;
    const bool extra_compact = item_count > 6;
    const bool city_rows = paging_cities || render_count >= 8;
    // 城市列表一屏 9 项: 34 + 9*30 = 304, 底部提示需留位 -> 收紧到间距 28。
    const int first_y = city_rows ? 32 : (extra_compact ? 39 : (compact ? 45 : 70));
    const int row_height = city_rows ? 25 : (extra_compact ? 29 : (compact ? 32 : 42));
    const int row_gap = city_rows ? 28 : (extra_compact ? 34 : (compact ? 38 : 55));
    s_settings_row_count = render_count;
    if (s_settings_selected >= item_count) s_settings_selected = 0;
    if (paging_cities && s_settings_selected < s_city_offset) {
        s_city_offset = (s_settings_selected / 9) * 9;
        s_settings_row_count = std::min<std::size_t>(item_count - s_city_offset, 9);
    }

    lv_obj_t *title = make_label(s_screen, title_text, 12, 13, 216, kText,
                                 ui_cn_font());
    set_centered(title);
    for (std::size_t i = 0; i < render_count; ++i) {
        const int y = first_y + static_cast<int>(i) * row_gap;
        const char *text = paging_cities ? "" : items[i];
        if (paging_cities) {
            const char *name = city_display_for(s_city_offset + i);
            std::snprintf(city_row, sizeof(city_row), "%s",
                          s_city_offset + i == 0 ? "自动定位"
                           : (name && name[0] ? name : "?"));
            text = city_row;
        }
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, row_height, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], text, 0,
                                          city_rows ? 5 :
                                          extra_compact ? 5 : (compact ? 7 : 12),
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
    lv_obj_t *hint = make_label(s_screen, hint_text, 12, 283, 216, kMuted);
    set_centered(hint);
}

void build_saved_wifi(const char *const *ssids, std::size_t count,
                      std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::SavedWifi;
    const std::size_t total = count + 1;
    selected = std::min(selected, total - 1);

    lv_obj_t *title = make_label(s_screen, "已存网络", 12, 12, 216, kText,
                                 ui_cn_font());
    set_centered(title);
    lv_obj_t *state = make_label(s_screen,
                                 count ? "上下选择  确定设置" : "无保存网络",
                                 12, 40, 216, count ? kMuted : kAmber);
    set_centered(state);

    const std::size_t first = total <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             total - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        if (index >= total) break;
        const bool active = index == selected;
        const int y = 67 + static_cast<int>(row) * 34;
        s_settings_rows[row] = make_box(s_screen, 18, y, 204, 29,
                                        active ? kAmberSoft : kPanel, 5);
        const char *text = index < count ? ssids[index] : "返回";
        s_settings_labels[row] = make_label(
            s_settings_rows[row], text, 8, 6, 188, active ? kText : kMuted,
            index < count ? ssid_font(text) : ui_cn_font());
        set_centered(s_settings_labels[row]);
    }
    lv_obj_t *hint = make_label(s_screen, "长按确定返回", 12, 286, 216, kMuted);
    set_centered(hint);
}

void build_wifi_action(const char *ssid, std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::WifiAction;
    s_settings_selected = selected % 3;
    s_settings_row_count = 3;
    lv_obj_t *title = make_label(s_screen, "网络设置", 12, 13, 216, kText,
                                 ui_cn_font());
    set_centered(title);
    lv_obj_t *network = make_label(s_screen, ssid ? ssid : "WIFI", 12, 45, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    static constexpr const char *kItems[] = {"设为首选", "删除网络", "返回"};
    for (std::size_t i = 0; i < 3; ++i) {
        const int y = 83 + static_cast<int>(i) * 55;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, 42, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], kItems[i], 0, 12,
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
    lv_obj_t *hint = make_label(s_screen, "长按确定返回", 12, 283, 216, kMuted);
    set_centered(hint);
}

void build_wifi_delete_confirm(const char *ssid, std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::WifiDeleteConfirm;
    s_settings_selected = selected % 2;
    s_settings_row_count = 2;
    lv_obj_t *title = make_label(s_screen, "确认删除", 12, 18, 216, kText,
                                 ui_cn_font());
    set_centered(title);
    lv_obj_t *network = make_label(s_screen, ssid ? ssid : "WIFI", 12, 55, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    lv_obj_t *warning = make_label(s_screen, "删除后无法自动连接", 12, 88, 216,
                                   kMuted);
    set_centered(warning);
    static constexpr const char *kItems[] = {"返回", "确认删除"};
    for (std::size_t i = 0; i < 2; ++i) {
        const int y = 132 + static_cast<int>(i) * 61;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, 45, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], kItems[i], 0, 13,
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
}

void build_easter_egg(const char *message) {
    reset_transient_objects(Page::EasterEgg);
    lv_obj_t *title = make_label(s_screen, "蓝牙", 12, 12, 216, kAmber,
                                 ui_cn_font());
    set_centered(title);
    s_easter_message = make_label(s_screen, message ? message : "正在加载彩蛋",
                                  12, 146, 216, kText);
    set_centered(s_easter_message);
    lv_obj_t *hint = make_label(s_screen, "确定返回", 12, 292, 216, kMuted);
    set_centered(hint);
}

void update_wifi_list(const WifiNetworkView *networks, std::size_t count,
                      std::size_t selected, bool scanning, bool can_cancel) {
    const char *state_text = scanning ? "正在寻找网络" :
                             count ? "上下选择  确定" :
                             can_cancel ? "等待网络  长按返回"
                                        : "等待网络  长按重试";
    lv_label_set_text(s_wifi_state, state_text);
    lv_obj_set_style_text_color(s_wifi_state,
                                lv_color_hex(scanning ? kAmber : kMuted), 0);
    if (s_wifi_list_hint) {
        lv_label_set_text(s_wifi_list_hint,
                          can_cancel ? "确定设置  长按返回" : "确定后设置网络");
    }

    const std::size_t first = count <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             count - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        const bool present = index < count;
        const bool active = present && index == selected;
        lv_obj_set_style_bg_color(s_wifi_row_box[row],
                                  lv_color_hex(active ? kAmberSoft : kPanel), 0);
        if (!present) {
            lv_label_set_text(s_wifi_row_ssid[row], "");
            lv_label_set_text(s_wifi_row_info[row], "");
            continue;
        }

        const uint32_t color = active ? kText : kMuted;
        lv_label_set_text(s_wifi_row_ssid[row], networks[index].ssid);
        lv_obj_set_style_text_font(s_wifi_row_ssid[row],
                                   ssid_font(networks[index].ssid), 0);
        lv_obj_set_style_text_color(s_wifi_row_ssid[row], lv_color_hex(color), 0);
        char info[16];
        std::snprintf(info, sizeof(info), "%s %ddB",
                      networks[index].secure ? "*" : "OPEN",
                      static_cast<int>(networks[index].rssi));
        lv_label_set_text(s_wifi_row_info[row], info);
        lv_obj_set_style_text_color(s_wifi_row_info[row],
                                    lv_color_hex(active ? kAmber : kMuted), 0);
    }
}

void build_wifi_list(const WifiNetworkView *networks, std::size_t count,
                     std::size_t selected, bool scanning, const char *setup_ap_ssid,
                     bool can_cancel) {
    reset_transient_objects(Page::WifiList);
    // The compact UI font contains the complete setup vocabulary. The larger
    // station-title font intentionally contains only station-name glyphs.
    make_label(s_screen, "选择无线网络", 12, 12, 216, kText, ui_cn_font());
    s_wifi_state = make_label(s_screen, "", 12, 42, 216, kMuted);
    set_centered(s_wifi_state);

    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const int y = 68 + static_cast<int>(row) * 34;
        s_wifi_row_box[row] = make_box(s_screen, 12, y, 216, 29, kPanel, 5);
        s_wifi_row_ssid[row] = make_label(s_wifi_row_box[row], "", 9, 6, 134,
                                          kMuted, &lv_font_montserrat_14);
        s_wifi_row_info[row] = make_label(s_wifi_row_box[row], "", 145, 7, 63,
                                          kMuted, &lv_font_montserrat_14);
        lv_obj_set_style_text_align(s_wifi_row_info[row], LV_TEXT_ALIGN_RIGHT, 0);
    }

    make_box(s_screen, 12, 277, 216, 1, kGrid);
    s_wifi_list_hint = make_label(s_screen, "", 12, 282, 216, kText);
    set_centered(s_wifi_list_hint);
    char portal[64];
    std::snprintf(portal, sizeof(portal), "WEB: %s",
                  setup_ap_ssid && setup_ap_ssid[0] ? setup_ap_ssid : "LEO Radio");
    lv_obj_t *hint = make_label(s_screen, portal, 12, 302, 216, kMuted,
                                &lv_font_montserrat_14);
    set_centered(hint);
    update_wifi_list(networks, count, selected, scanning, can_cancel);
}

void update_wifi_keyboard(const char *ssid, std::size_t password_length,
                          WifiKeyboardPage page, std::size_t selected,
                          const char *status) {
    lv_label_set_text(s_wifi_keyboard_network, ssid ? ssid : "WIFI");
    lv_obj_set_style_text_font(s_wifi_keyboard_network, ssid_font(ssid), 0);

    char password[48] = {};
    const std::size_t stars = std::min<std::size_t>(password_length, 38);
    std::memset(password, '*', stars);
    password[stars] = '\0';
    lv_label_set_text(s_wifi_password_mask, password_length ? password : "PASS");
    lv_obj_set_style_text_color(s_wifi_password_mask,
                                lv_color_hex(password_length ? kText : kMuted), 0);

    const std::size_t key_count = radio_ui_wifi_key_count(page);
    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        if (i >= key_count) {
            lv_obj_add_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
        const bool active = i == selected;
        lv_obj_set_style_bg_color(s_wifi_key_box[i],
                                  lv_color_hex(active ? kAmber : kPanelSoft), 0);
        lv_label_set_text(s_wifi_key_label[i], radio_ui_wifi_key(page, i));
        lv_obj_set_style_text_color(s_wifi_key_label[i],
                                    lv_color_hex(active ? kBackground : kText), 0);
    }

    lv_label_set_text(s_wifi_keyboard_status,
                      status && status[0] ? status : "上下选择  确定");
    lv_obj_set_style_text_color(s_wifi_keyboard_status,
                                lv_color_hex(status && status[0] ? kRed : kMuted), 0);
}

void build_wifi_keyboard(const char *ssid, std::size_t password_length,
                         WifiKeyboardPage page, std::size_t selected,
                         const char *status) {
    reset_transient_objects(Page::WifiKeyboard);
    make_label(s_screen, "网络设置", 12, 10, 216, kText, ui_cn_font());
    s_wifi_keyboard_network = make_label(s_screen, "", 12, 39, 216, kAmber,
                                          &lv_font_montserrat_14);
    set_centered(s_wifi_keyboard_network);

    lv_obj_t *password_box = make_box(s_screen, 18, 66, 204, 34, kPanel, 6);
    s_wifi_password_mask = make_label(password_box, "PASS", 8, 8, 188, kMuted,
                                      &lv_font_montserrat_14);
    set_centered(s_wifi_password_mask);

    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        constexpr int kColumns = 6;
        const int x = 15 + static_cast<int>(i % kColumns) * 35;
        const int y = 109 + static_cast<int>(i / kColumns) * 28;
        // One styled label per key keeps the keyboard within the 24 KB LVGL
        // heap. A separate box + label for every key exhausted that heap near
        // key 31 and caused an immediate Store access fault.
        s_wifi_key_label[i] = make_label(s_screen, "", x, y, 31, kText,
                                         &lv_font_montserrat_14);
        s_wifi_key_box[i] = s_wifi_key_label[i];
        lv_obj_set_size(s_wifi_key_label[i], 31, 24);
        lv_obj_set_style_bg_color(s_wifi_key_label[i], lv_color_hex(kPanelSoft), 0);
        lv_obj_set_style_bg_opa(s_wifi_key_label[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_wifi_key_label[i], 4, 0);
        lv_obj_set_style_pad_top(s_wifi_key_label[i], 4, 0);
        set_centered(s_wifi_key_label[i]);
    }

    make_box(s_screen, 12, 281, 216, 1, kGrid);
    s_wifi_keyboard_status = make_label(s_screen, "", 12, 287, 216, kMuted);
    set_centered(s_wifi_keyboard_status);
    lv_obj_t *hint = make_label(s_screen, "长按上下调节", 12, 304, 216, kMuted);
    set_centered(hint);
    update_wifi_keyboard(ssid, password_length, page, selected, status);
}

void build_wifi_connecting(const char *ssid, const char *detail) {
    reset_transient_objects(Page::WifiConnecting);
    make_label(s_screen, "正在连接无线网络", 12, 37, 216, kText,
               ui_cn_font());
    lv_obj_t *network = make_label(s_screen, ssid ? ssid : "WIFI", 12, 102, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    s_pair_dot = make_box(s_screen, 108, 151, 24, 24, kAmber, LV_RADIUS_CIRCLE);
    lv_obj_t *message = make_label(s_screen, detail ? detail : "正在连接",
                                   12, 205, 216, kMuted);
    set_centered(message);
}

void timer_callback(lv_timer_t *) {
    s_animation += 17;

    // LEO 页面不可见时跳过对象更新;首页由 radio_home.cc 的 1s 定时器更新。
    if (!s_page_visible) {
        return;
    }

    if (s_page == Page::Config || s_page == Page::WifiConnecting) {
        if (s_pair_dot) {
            const uint8_t triangle = s_animation < 128 ? s_animation : 255 - s_animation;
            // 用颜色明暗做呼吸效果, 不走 style_opa(见 apply_network 注释)。
            const uint8_t level = 70 + triangle;
            const uint8_t r = (uint8_t)(0xD8 * level / 255);
            const uint8_t g = (uint8_t)(0xFF * level / 255);
            const uint8_t b = (uint8_t)(0x7A * level / 255);
            lv_obj_set_style_bg_color(s_pair_dot,
                                      lv_color_make(r, g, b), 0);
        }
        return;
    }

    if (s_clock) {
        std::time_t now = std::time(nullptr);
        if (now > 1700000000) {
            std::tm local_time = {};
            localtime_r(&now, &local_time);
            char text[8];
            std::snprintf(text, sizeof(text), "%02d:%02d", local_time.tm_hour,
                          local_time.tm_min);
            lv_label_set_text(s_clock, text);
        }
    }
    apply_sleep_timer();

    // 电量: 用 bsp_battery_level()(带电压兜底, 比 soc 更早可用)。
    // 周期 300 -> 30 tick(100ms/次 = 3s)。原来 300tick=30s 才刷一次,
    // 加上开机时电量芯片尚未就绪, 用户看到"--%"要等很久(用户反馈)。
    if (++s_battery_ticks >= 30) {
        s_battery_ticks = 0;
        const int soc = bsp_battery_level();
        if (s_battery && soc >= 0) {
            char text[12];
            std::snprintf(text, sizeof(text), "%d%%", std::clamp(soc, 0, 100));
            lv_label_set_text(s_battery, text);
            lv_obj_set_style_text_color(s_battery,
                                        lv_color_hex(soc <= 15 ? kRed : kMuted), 0);
            // 电量右对齐到"屏幕右边缘内收 10px"(避让圆角); 信号格紧贴其左。
            // 用实际 x 反推, 保证任意位数电量下都无缝。
            lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -10, 8);
            lv_obj_update_layout(s_battery);
            const int batt_x = lv_obj_get_x(s_battery);
            for (int i = 0; i < 3; ++i) {
                if (!s_wifi_bars[i]) continue;
                lv_obj_set_pos(s_wifi_bars[i], batt_x - 20 + i * 5,
                               19 - (5 + i * 3));
            }
        }
    }

    const uint32_t generation = s_level_generation.load(std::memory_order_acquire);
    if (generation != s_last_level_generation) {
        s_last_level_generation = generation;
        if (s_meter[0]) lv_obj_invalidate(s_meter[0]);
    }
}

}  // namespace

bool radio_ui_init() {
    if (!bsp_lvgl_lock(1000)) return false;
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    // 默认首页是 plays/438 卡片式界面(radio_home_screen_create)。
    // 这里【不再】立即 build_main() —— LEO 主界面对象树很大,
    // 与首页同时驻留曾把 LVGL 静态池(48KiB)吃满,导致启动即崩溃重启。
    // LEO 界面延迟到 radio_ui_show_leo_main() 首次调用时才构建。
    s_page_visible = false;
    lv_screen_load(s_screen);
    // 刷新周期 50ms -> 100ms: 10Hz 对频谱/时钟/电池这类慢变化显示已足够,
    // 减半 LVGL 忙度, 少抢播放任务 CPU(用户反馈"闪烁条频率太高")。
    lv_timer_create(timer_callback, 100, nullptr);
    bsp_lvgl_unlock();
    return true;
}

void radio_ui_show_home() {
    if (!bsp_lvgl_lock(500)) return;
    // 离开 LEO 页面:停掉那个 50ms 定时器对 LEO 对象的操作。
    s_page_visible = false;
    // 首页的创建与加载由 radio_home_screen_create() 统一处理。
    radio_home_screen_create();
    bsp_lvgl_unlock();
}

// 返回"主界面"。
//
// 这里的判断是防闪烁的关键。main.cc 里有 7 处会调 radio_ui_show_main()
// (WiFi 连接事件、按键返回、配网完成等)。若它无条件切回 LEO 原版的
// build_main(),就会与启动时显示的 plays/438 首页来回切,屏幕一闪一闪。
//
// 规则:
//   - 已经在 plays/438 首页  -> 什么都不做(幂等)
//   - 正从子页面返回(设置/WiFi/Codex) -> 回 plays/438 首页
//   - 首次调用(尚无屏)      -> 建 plays/438 首页
// LEO 原版主界面仍可通过 radio_ui_show_leo_main() 显式进入。
// 离开 LEO 页面(切到 plays/438 首页 / Codex 页等外部页面)时调用。
// 跳过 50ms timer_callback 对 LEO 页面对象的更新,外部页面自行刷新。
void radio_ui_leave_leo_page() {
    if (!bsp_lvgl_lock(500)) return;
    // Destroy the full operation tree while preserving radio data for home.
    reset_transient_objects(Page::Main);
    s_page_visible = false;
    bsp_lvgl_unlock();
}

void radio_ui_show_main() {
    if (!bsp_lvgl_lock(500)) return;
    s_page_visible = false;
    // 首页的创建与加载由 radio_home_screen_create() 统一处理。
    radio_home_screen_create();
    bsp_lvgl_unlock();
}

// 显式进入 LEO 原版的"单卡片 + 大旋钮"主界面。
// 保留它是为了随时能对比两种风格;默认流程不走这里。
//
// ⚠ 幂等优化(修复"进收音机卡一下"): 早期每次调用都无条件 build_main() ->
// lv_obj_clean 全屏销毁再重建几十个对象 + lv_screen_load 全屏重绘。而
// radio_app_enter 每次从菜单进入都会调它 —— 用户能明显看到一下卡顿/闪白。
// 现在: 若当前已停在 Main 页且对象树完好(s_screen 非空、s_station_name 等
// 关键指针有效), 只做必要的状态刷新(电量/锁屏), 不重建。
void radio_ui_show_leo_main() {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    const bool tree_alive = (s_page == Page::Main) && s_station_name != nullptr &&
                            s_volume_bar != nullptr;
    if (tree_alive) {
        // 已在收音机主界面: 只刷新易变状态, 避免全量重建。
        // 电量由 timer_callback 按 s_battery_ticks 周期刷新, 这里置满即触发。
        if (lv_screen_active() != s_screen) lv_screen_load(s_screen);
        s_page_visible = true;
        s_battery_ticks = 30;      // 强制下个 timer tick 刷新电量
    } else {
        build_main();
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_config(const char *ssid, const char *url) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_config(ssid, url);
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_list(const WifiNetworkView *networks, std::size_t count,
                             std::size_t selected, bool scanning,
                             const char *setup_ap_ssid, bool can_cancel) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::WifiList && s_wifi_state) {
        update_wifi_list(networks, count, selected, scanning, can_cancel);
    } else {
        build_wifi_list(networks, count, selected, scanning, setup_ap_ssid,
                        can_cancel);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_keyboard(const char *ssid, std::size_t password_length,
                                 WifiKeyboardPage page, std::size_t selected,
                                 const char *status) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::WifiKeyboard && s_wifi_password_mask) {
        update_wifi_keyboard(ssid, password_length, page, selected, status);
    } else {
        build_wifi_keyboard(ssid, password_length, page, selected, status);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_connecting(const char *ssid, const char *detail) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_connecting(ssid, detail);
    bsp_lvgl_unlock();
}

const char *radio_ui_wifi_key(WifiKeyboardPage page, std::size_t index) {
    switch (page) {
        case WifiKeyboardPage::Lower:
            return index < sizeof(kLowerKeys) / sizeof(kLowerKeys[0])
                       ? kLowerKeys[index]
                       : "";
        case WifiKeyboardPage::Upper:
            return index < sizeof(kUpperKeys) / sizeof(kUpperKeys[0])
                       ? kUpperKeys[index]
                       : "";
        case WifiKeyboardPage::Symbols:
            return index < sizeof(kSymbolKeys) / sizeof(kSymbolKeys[0])
                       ? kSymbolKeys[index]
                       : "";
    }
    return "";
}

std::size_t radio_ui_wifi_key_count(WifiKeyboardPage page) {
    switch (page) {
        case WifiKeyboardPage::Lower: return sizeof(kLowerKeys) / sizeof(kLowerKeys[0]);
        case WifiKeyboardPage::Upper: return sizeof(kUpperKeys) / sizeof(kUpperKeys[0]);
        case WifiKeyboardPage::Symbols:
            return sizeof(kSymbolKeys) / sizeof(kSymbolKeys[0]);
    }
    return 0;
}

void radio_ui_set_network(bool connected, const char *detail) {
    if (!bsp_lvgl_lock(250)) return;
    s_network_connected = connected;
    if (detail) std::snprintf(s_network_detail, sizeof(s_network_detail), "%s", detail);
    radio_home_set_network(connected, detail);
    if (s_screen) apply_network();
    bsp_lvgl_unlock();
}

void radio_ui_set_location(const char *location) {
    if (!bsp_lvgl_lock(250)) return;
    std::snprintf(s_location_text, sizeof(s_location_text), "%s",
                  location && location[0] ? location : "NATIONAL");
    radio_home_set_location(location);
    if (s_screen && s_location) lv_label_set_text(s_location, s_location_text);
    bsp_lvgl_unlock();
}

void radio_ui_set_station(std::size_t index, std::size_t count,
                          const RadioStation &station) {
    if (!bsp_lvgl_lock(250)) return;
    s_station_index = index;
    s_station_count = std::max<std::size_t>(count, 1);
    s_station_frequency = station.frequency_decihz;
    std::snprintf(s_station_name_text, sizeof(s_station_name_text), "%s", station.name);
    std::snprintf(s_station_description_text, sizeof(s_station_description_text), "%s",
                  station.description);
    // 同步给 plays/438 首页(它自己不去拉数据,由外部灌入)。
    radio_home_set_station(station);
    if (s_screen) apply_station();
    bsp_lvgl_unlock();
}

void radio_ui_set_playback(RadioPlaybackState state, const char *detail) {
    if (!bsp_lvgl_lock(250)) return;
    s_playback_state = state;
    std::snprintf(s_playback_detail, sizeof(s_playback_detail), "%s",
                  detail && detail[0] ? detail : playback_text());
    // 传状态枚举判断出的 playing 标志,首页据此决定频段条是否全亮。
    // 不靠匹配中文文本 —— 那种做法一改文案就失效。
    radio_home_set_playback(playback_text(), state == RadioPlaybackState::Playing);
    if (s_screen) apply_playback();
    bsp_lvgl_unlock();
}

void radio_ui_set_audio_levels(const uint8_t *levels, std::size_t count) {
    if (!levels) return;
    const std::size_t copy_count = std::min(count, kMeterCount);
    std::memcpy(s_levels, levels, copy_count);
    if (copy_count < kMeterCount) {
        std::memset(s_levels + copy_count, 0, kMeterCount - copy_count);
    }
    s_level_generation.fetch_add(1, std::memory_order_release);
}

void radio_ui_refresh_settings_row_texts(void) {
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    if (s_page == Page::Settings && s_settings_page == RadioSettingsPage::Display &&
        s_settings_row_count >= 2 && s_settings_labels[0] && s_settings_labels[1]) {
        char brightness_item[32];
        char timeout_item[48];
        std::snprintf(brightness_item, sizeof(brightness_item), "亮度 %u%%",
                      static_cast<unsigned>(radio_display_brightness()));
        const uint16_t timeout = radio_display_timeout_seconds();
        if (!timeout) {
            std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 关闭");
        } else if (timeout < 60) {
            std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 秒",
                          static_cast<unsigned>(timeout));
        } else {
            std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 分钟",
                          static_cast<unsigned>(timeout / 60));
        }
        lv_label_set_text(s_settings_labels[0], brightness_item);
        lv_label_set_text(s_settings_labels[1], timeout_item);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_settings(RadioSettingsPage page, std::size_t selected,
                            uint8_t volume) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::Settings && s_settings_page == page &&
        page != RadioSettingsPage::Volume &&
        page != RadioSettingsPage::Display && s_settings_row_count > 0) {
        if (page == RadioSettingsPage::City) {
            // 城市页的 selected 是跨页的绝对索引, 不能被当前页的行数取模。
            // 只有翻到另一页才重建, 同页移动只改变高亮, 避免每按一次键重建
            // 整屏 LVGL 对象(播放期间会抢堆)。
            const std::size_t want_page = selected / 9;
            if (want_page != s_city_offset / 9 || s_settings_row_count == 0) {
                build_settings(page, selected, volume);
            } else {
                s_settings_selected = selected;
                apply_settings_selection();
            }
        } else {
            s_settings_selected = selected % s_settings_row_count;
            apply_settings_selection();
        }
    } else {
        build_settings(page, selected, volume);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_saved_wifi(const char *const *ssids, std::size_t count,
                              std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_saved_wifi(ssids, count, selected);
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_action(const char *ssid, std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_action(ssid, selected);
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_delete_confirm(const char *ssid, std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_delete_confirm(ssid, selected);
    bsp_lvgl_unlock();
}

void radio_ui_show_easter_egg(const char *message) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::EasterEgg && s_easter_message) {
        lv_label_set_text(s_easter_message, message ? message : "正在加载彩蛋");
        lv_obj_remove_flag(s_easter_message, LV_OBJ_FLAG_HIDDEN);
    } else {
        build_easter_egg(message);
    }
    bsp_lvgl_unlock();
}

bool radio_ui_show_easter_egg_frame(const uint8_t *jpeg, std::size_t size,
                                    uint16_t width, uint16_t height) {
    if (!jpeg || !size || !s_screen || !width || !height ||
        kVideoWidth % width != 0 || kVideoHeight % height != 0) {
        ESP_LOGE("radio_ui", "Video frame rejected: jpeg=%p size=%u %ux%u screen=%p",
                 jpeg, static_cast<unsigned>(size),
                 static_cast<unsigned>(width), static_cast<unsigned>(height),
                 s_screen);
        return false;
    }
    const uint16_t scale_x = kVideoWidth / width;
    const uint16_t scale_y = kVideoHeight / height;
    if (scale_x != scale_y || scale_x == 0 || scale_x > kMaxVideoScale) {
        ESP_LOGE("radio_ui", "Video scale invalid %ux%u", scale_x, scale_y);
        return false;
    }
    if (!ensure_direct_video_buffers()) {
        ESP_LOGE("radio_ui", "Video buffers unavailable: work=%p stripe=%p",
                 s_jpeg_work, s_video_stripe);
        return false;
    }
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGE("radio_ui", "LVGL lock timeout for video frame");
        return false;
    }
    if (s_page != Page::EasterEgg) {
        ESP_LOGE("radio_ui", "Video frame page mismatch page=%d", static_cast<int>(s_page));
        bsp_lvgl_unlock();
        return false;
    }

    if (s_video_color_pending) {
        if (!drain_video_color_transfers()) {
            bsp_lvgl_unlock();
            return false;
        }
        s_video_color_pending = false;
    }

    DirectJpegContext context = {
        .data = jpeg,
        .size = size,
        .position = 0,
        .source_width = width,
        .source_height = height,
        .scale = static_cast<uint8_t>(scale_x),
        .failed = false,
    };
    JDEC decoder = {};
    JRESULT result = jd_prepare(&decoder, direct_jpeg_input, s_jpeg_work,
                                kJpegWorkSize, &context);
    if (result == JDR_OK && decoder.width == width && decoder.height == height) {
        result = jd_decomp(&decoder, direct_jpeg_output, 0);
    } else {
        ESP_LOGE("radio_ui", "JPEG prepare failed %d (%ux%u expected %ux%u)",
                 result, decoder.width, decoder.height, width, height);
    }
    if (result != JDR_OK || context.failed) {
        ESP_LOGE("radio_ui", "JPEG decomp failed %d context_failed=%d",
                 result, context.failed);
    }
    bsp_lvgl_unlock();
    return result == JDR_OK && !context.failed;
}

void radio_ui_finish_easter_egg() {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    release_direct_video_buffers();
    build_main();
    bsp_lvgl_unlock();
}

void radio_ui_set_volume(uint8_t volume) {
    s_volume = volume;
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    apply_volume();
    bsp_lvgl_unlock();
}
