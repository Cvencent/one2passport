// main/radio_home.cc —— plays/438「口袋百宝箱」风格的首页
//
// ## 布局来源
//
// 照 plays/438 主图里的实机截图复刻。官方主图:
//   https://ai-passport.folotoy.cn/api/media/plays/community-1353f966.webp
// 屏幕区像素比例 0.754,对比 240/320 = 0.75,可确认是本硬件标准屏。
//
// 屏幕自上而下分四带:
//
//   ┌───────────────────────────┐
//   │ ▂▄▆ PASSPORT          97% │ 22px  顶栏:信号格 + 标题 + 电量格
//   ├───────────────────────────┤
//   │ ┌────────┐  ┌───────────┐ │
//   │ │ 像素人形│  │  19:22    │ │ 96px 卡片带
//   │ │ 名字   │  │ 09月16日  │ │ 左形象卡 + 右信息卡
//   │ │ 口号   │  │ ▾城市 25° │ │
//   │ └────────┘  └───────────┘ │
//   │ ┌───────────────────────┐ │
//   │ │ ▸ 城市电台      94.7  │ │ 44px 电台卡
//   │ └───────────────────────┘ │
//   │ ▁▂▃▄▅▆▇ 频段条            │ 46px
//   │  长按UP 切换 Codex        │ 底部提示
//   └───────────────────────────┘
//
// ## ⚠️ 两个必须避开的坑(第一版就踩了,屏幕会闪)
//
// 1. **屏幕必须只建一次。** main.cc 里有 7 处会调 radio_ui_show_main()
//    切回主界面。若本函数每次都新建屏,就会与那些动作来回切换,
//    表现为屏幕一闪一闪。故此处做成幂等:屏已存在就只载入。
//
// 2. **定时器必须单例。** 第一版在每次进首页时 lv_timer_create(),
//    进出几十次就累积几十个 1s 定时器,内存泄漏 + 高频重绘。
//    现在 s_tick 缓存句柄,退出时删除。
//
// ## 坐标全部硬编码
//
// 按 240x320 写死,与 plays/438 一致,不做响应式。
// 屏幕右侧有滚轮遮挡(见主图),故内容宽度收在 220 以内。
#include "radio_home.h"

#include "bsp_battery.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui_phosphor.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *kTag = "home";

namespace {

// ── 版面常量(单位 px)──
// 240x320 逐段排布:顶栏22 + 卡片110 + 电台卡58 + 频段条48 + 提示18,
// 垂直方向铺满整屏,不再留 70px 的空洞(用户反馈"屏幕没占完")。
constexpr int kHeaderH = 22;
constexpr int kMargin = 10;
constexpr int kContentW = 220;  // 右侧留 20 给滚轮

constexpr int kCardY = 30;
constexpr int kCardH = 110;
constexpr int kAvatarX = kMargin;
constexpr int kAvatarW = 86;
constexpr int kInfoX = 102;
constexpr int kInfoW = 118;

constexpr int kStationY = 148;
constexpr int kStationH = 58;

constexpr int kBandY = 214;
constexpr int kBandH = 48;

constexpr int kHintY = 288;

constexpr int kBandCells = 20;

// 像素人形:11x11 点阵。
// 不用图片有两个原因:① 图片占 flash(本工程已用 2.2MB);
// ② plays/438 的形象本身就是低分辨率点阵风,方块拼出来更贴近原味。
constexpr int kAvatarCols = 11;
constexpr int kAvatarRows = 11;
const char *kAvatarArt[kAvatarRows] = {
    "00000100000",
    "00001110000",
    "00011111000",
    "00111111100",
    "01101110110",
    "01111111110",
    "01101110110",
    "00111111100",
    "00011111100",
    "00001101100",
    "00001101100",
};

// ── 状态 ──
lv_obj_t *s_screen = nullptr;
lv_timer_t *s_tick = nullptr;  // 关键:单例,避免泄漏

lv_obj_t *s_battery_text = nullptr;

lv_obj_t *s_avatar_card = nullptr;
lv_obj_t *s_avatar_cells[kAvatarRows][kAvatarCols] = {};
lv_obj_t *s_name = nullptr;
lv_obj_t *s_motto = nullptr;

lv_obj_t *s_info_card = nullptr;
lv_obj_t *s_clock = nullptr;
lv_obj_t *s_date = nullptr;
lv_obj_t *s_location = nullptr;
lv_obj_t *s_weather = nullptr;

lv_obj_t *s_station_card = nullptr;
lv_obj_t *s_station_name = nullptr;
lv_obj_t *s_station_freq = nullptr;

lv_obj_t *s_band_card = nullptr;
lv_obj_t *s_band_cells[kBandCells] = {};
lv_obj_t *s_band_label = nullptr;

lv_obj_t *s_hint = nullptr;

// 由外部灌入的数据
char s_city[32] = "";
char s_station[64] = "";
char s_status[48] = "";
char s_net[48] = "";
char s_codex_status[48] = "空闲";  // Codex 工作状态
uint16_t s_freq_decihz = 0;
bool s_playing = false;
bool s_network = false;

// ── 内部工具 ──

void build_avatar(lv_obj_t *parent)
{
    // cell 6->5: 11*5=55px 高,给下方两行文字留出空间。
    // 之前 66px 高的点阵与"我的/保存好奇"两行文字重叠(用户反馈"字和图叠在一起")。
    const int cell = 5;
    const int x0 = (kAvatarW - cell * kAvatarCols) / 2;
    const int y0 = 6;
    for (int row = 0; row < kAvatarRows; ++row) {
        for (int col = 0; col < kAvatarCols; ++col) {
            const bool on = kAvatarArt[row][col] == '1';
            s_avatar_cells[row][col] =
                ui_phosphor_block_create(parent, x0 + col * cell, y0 + row * cell,
                                         cell, cell, on ? UI_WHITE : UI_SOFT);
        }
    }
}

// 时钟。必须用 std::time() 的真实系统时间(SNTP 校准后),不能用
// lv_tick_get() —— 那是开机时长,会显示成 00:00、01:23 这种假时间。
// 判据与 LEO 原版一致:now > 1700000000 才认为 SNTP 已生效,
// 否则显示 "--:--" 而不是误导性的 00:00。
void refresh_clock()
{
    // 用全局命名空间的 time_t/tm,不用 std:: —— 本工程里 <time.h> 会被
    // LVGL 头文件以 extern "C" 包住,<ctime> 的 std 名字因此不可见。
    const time_t now = ::time(nullptr);
    if (now > 1700000000) {
        tm local = {};
        ::localtime_r(&now, &local);
        char text[8];
        snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
        lv_label_set_text(s_clock, text);

        static const char *kWeekday[] = {"日", "一", "二", "三", "四",
                                         "五", "六"};
        char date_text[24];
        snprintf(date_text, sizeof(date_text), "%02d月%02d日 周%s",
                 local.tm_mon + 1, local.tm_mday, kWeekday[local.tm_wday % 7]);
        lv_label_set_text(s_date, date_text);
    } else {
        lv_label_set_text(s_clock, "--:--");
        lv_label_set_text(s_date, "校时中…");
    }
}

void refresh_battery()
{
    // bsp_battery_level() 无参数,直接返回 0..100,读不到返回 -1。
    const int percent = bsp_battery_level();
    if (percent < 0) {
        lv_label_set_text(s_battery_text, "--");
        return;
    }
    // 缓冲区给 12 字节:percent 上界 100,"100%" 需 5 字节。给 8 字节时
    // gcc 无法证明不会截断,会报 -Werror=format-truncation 而工程开了 -Werror。
    char text[12];
    snprintf(text, sizeof(text), "%d%%", percent);
    lv_label_set_text(s_battery_text, text);
}

// 频段条。plays/438 用一排高低起伏的方块表示信号。
// 播放中且已联网 = 全亮;否则按状态给不同的"不满"程度,像终端一样直白。
void refresh_band()
{
    int lit = 14;  // 默认
    if (!s_network) {
        lit = 3;
    } else if (s_playing) {
        lit = kBandCells;
    } else {
        lit = 10;
    }
    for (int i = 0; i < kBandCells; ++i) {
        // 峰值高度受卡内剩余空间限制: 卡高48 - 标签行22 = 26。
        const int h = 8 + (i % 5) * 4;
        lv_obj_set_pos(s_band_cells[i], 12 + i * 9, 0);
        lv_obj_set_size(s_band_cells[i], 7, h);
        lv_obj_set_style_bg_color(s_band_cells[i],
                                  lv_color_hex(i < lit ? UI_WHITE : UI_SOFT), 0);
    }
}

void refresh_station()
{
    lv_label_set_text(s_station_name, s_station[0] ? s_station : "未选择电台");
    // 频率。目录里没有可用频率时(frequency_decihz == 0)不能瞎编,
    // 显示 "--.-" 并让上层知道这是网络电台。
    if (s_freq_decihz > 0) {
        char text[16];
        snprintf(text, sizeof(text), "%u.%u MHz", (unsigned)(s_freq_decihz / 10),
                 (unsigned)(s_freq_decihz % 10));
        lv_label_set_text(s_station_freq, text);
    } else {
        lv_label_set_text(s_station_freq, "网络电台");
    }
    lv_label_set_text(s_location, s_city[0] ? s_city : "未定位");
    lv_label_set_text(s_weather, s_status[0] ? s_status : (s_network ? "在线" : "离线"));
    lv_label_set_text(s_band_label, s_network ? "FM" : "OFF");
}

// 1s 心跳:时钟与电量都在走,plays/438 的顶栏也是活的。
void home_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_screen == nullptr) return;
    refresh_clock();
    refresh_battery();
}

}  // namespace

bool radio_home_ready()
{
    return s_screen != nullptr;
}

lv_obj_t *radio_home_screen_create()
{
    // 复用已有首页,仅在首页未激活时调用加载。
    // LVGL 9.5 的 lv_display.c:775 对同屏加载会早退,不能据此认定会清屏或白屏。
    if (s_screen != nullptr) {
        if (lv_screen_active() != s_screen) {
            ESP_LOGW(kTag, "重新载入首页(此前屏幕不在首页)");
            lv_screen_load(s_screen);
        } else {
            // 诊断:正常情况不该走到这以外的分支。反复出现说明有别处切屏。
            ESP_LOGD(kTag, "首页已激活,跳过重载");
        }
        return s_screen;
    }

    s_screen = ui_pixel_bare_screen_create();

    // ── 顶栏 ──
    ui_phosphor_header_create(s_screen, "PASSPORT");
    // 电量只留百分比文字,不再画4格电池块 —— 图标+数字重复,用户反馈"不需要图标"。
    s_battery_text = ui_pixel_label(s_screen, "--", ui_cn_font(), UI_WHITE);
    lv_obj_set_pos(s_battery_text, 204, 4);

    // ── 形象卡 ──
    s_avatar_card = ui_pixel_panel_create(s_screen, kAvatarX, kCardY, kAvatarW,
                                          kCardH, UI_SURFACE);
    build_avatar(s_avatar_card);
    s_name = ui_pixel_label(s_avatar_card, "我的", ui_cn_font(), UI_WHITE);
    lv_obj_align(s_name, LV_ALIGN_BOTTOM_LEFT, 0, -30);
    s_motto = ui_pixel_label(s_avatar_card, "保存好奇", ui_cn_font(), UI_MUTED);
    lv_obj_align(s_motto, LV_ALIGN_BOTTOM_LEFT, 0, -14);

    // ── 信息卡 ──
    s_info_card = ui_pixel_panel_create(s_screen, kInfoX, kCardY, kInfoW, kCardH,
                                        UI_SURFACE);
    s_clock = ui_pixel_label(s_info_card, "00:00", ui_cn_font(), UI_WHITE);
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -6, 4);
    ui_phosphor_divider_create(s_info_card, 0, 30, kInfoW - 12);
    s_date = ui_pixel_label(s_info_card, "待同步", ui_cn_font(), UI_MUTED);
    lv_obj_align(s_date, LV_ALIGN_TOP_LEFT, 6, 34);
    s_location = ui_pixel_label(s_info_card, "未定位", ui_cn_font(), UI_MUTED);
    lv_obj_align(s_location, LV_ALIGN_BOTTOM_LEFT, 6, -16);
    s_weather = ui_pixel_label(s_info_card, "", ui_cn_font(), UI_MUTED);
    lv_obj_align(s_weather, LV_ALIGN_BOTTOM_RIGHT, -6, -16);

    // ── 电台卡 ──
    s_station_card = ui_pixel_panel_create(s_screen, kMargin, kStationY, kContentW,
                                           kStationH, UI_SURFACE);
    s_station_name = ui_pixel_label(s_station_card, "未选择电台", ui_cn_font(),
                                    UI_WHITE);
    lv_obj_align(s_station_name, LV_ALIGN_LEFT_MID, 0, -6);
    s_station_freq = ui_pixel_label(s_station_card, "--", ui_cn_font(), UI_ACCENT);
    lv_obj_align(s_station_freq, LV_ALIGN_LEFT_MID, 0, 10);

    // ── 频段条 ──
    s_band_card = ui_pixel_panel_create(s_screen, kMargin, kBandY, kContentW,
                                        kBandH, UI_SURFACE);
    s_band_label = ui_pixel_label(s_band_card, "FM", ui_cn_font(), UI_MUTED);
    lv_obj_align(s_band_label, LV_ALIGN_TOP_LEFT, 6, 2);
    for (int i = 0; i < kBandCells; ++i) {
        s_band_cells[i] = ui_phosphor_block_create(s_band_card, 12 + i * 9, 22,
                                                    7, 8, UI_SOFT);
    }

    // ── 底部提示 ──
    s_hint = ui_pixel_label(s_screen, "长按UP 切换 Codex", ui_cn_font(), UI_MUTED);
    lv_obj_set_pos(s_hint, kMargin, kHintY);

    refresh_clock();
    refresh_battery();
    refresh_band();
    refresh_station();

    // 定时器单例:只在首次创建时挂,退出时删。详见文件头「坑 2」。
    if (s_tick == nullptr) {
        s_tick = lv_timer_create(home_tick, 1000, nullptr);
    }

    // 首次创建完成后加载首页,使其成为当前活动屏幕。
    lv_screen_load(s_screen);
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(kTag, "plays/438 首页已建立并载入 LVGL池: used=%u%% (%u/%u KiB) peak=%u KiB frag=%u%%",
             (unsigned)mon.used_pct, (unsigned)((mon.total_size - mon.free_size) / 1024),
             (unsigned)(mon.total_size / 1024), (unsigned)(mon.max_used / 1024),
             (unsigned)mon.frag_pct);
    return s_screen;
}

void radio_home_destroy()
{
    if (s_tick != nullptr) {
        lv_timer_delete(s_tick);
        s_tick = nullptr;
    }
    if (s_screen != nullptr) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
    }
    // 指针全部失效,一并清空,避免重建后残留野指针
    memset(s_avatar_cells, 0, sizeof(s_avatar_cells));
    memset(s_band_cells, 0, sizeof(s_band_cells));
    s_battery_text = s_avatar_card = s_name = s_motto = nullptr;
    s_info_card = s_clock = s_date = s_location = s_weather = nullptr;
    s_station_card = s_station_name = s_station_freq = nullptr;
    s_band_card = s_band_label = s_hint = nullptr;
}

// ── 数据注入 ──────────────────────────────────────────────────────

void radio_home_set_station(const RadioStation &station)
{
    snprintf(s_station, sizeof(s_station), "%s", station.name);
    s_freq_decihz = station.frequency_decihz;
    if (s_station_card != nullptr) {
        refresh_station();
        refresh_band();
    }
}

void radio_home_set_location(const char *location)
{
    snprintf(s_city, sizeof(s_city), "%s", location != nullptr ? location : "");
    if (s_location != nullptr) refresh_station();
}

void radio_home_set_playback(const char *text, bool playing)
{
    snprintf(s_status, sizeof(s_status), "%s", text != nullptr ? text : "");
    s_playing = playing;
    if (s_station_card != nullptr) {
        refresh_station();
        refresh_band();
    }
}

void radio_home_set_network(bool connected, const char *detail)
{
    s_network = connected;
    snprintf(s_net, sizeof(s_net), "%s", detail != nullptr ? detail : "");
    if (s_station_card != nullptr) {
        refresh_station();
        refresh_band();
    }
}

void radio_home_set_codex_status(const char *status)
{
    snprintf(s_codex_status, sizeof(s_codex_status), "%s", status != nullptr ? status : "空闲");
    // 刷新机器人"口号"显示 Codex 状态
    if (s_motto != nullptr) {
        lv_label_set_text(s_motto, s_codex_status);
    }
}
