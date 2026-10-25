// app_registry.cc —— V6 通用应用框架实现
//
// 结构:
//   [首页桌面]  --OK/长按UP-->  [应用菜单·轮播]  --OK-->  [应用]
//   全局返回: 应用内长按UP=回菜单; 菜单长按UP=回首页
//   页面生命周期仿原厂 demo_navigation: enter建页 / exit删页 / key转发
//
// 内存策略(效果优先下尽量省):
//   首页与菜单两棵 LVGL 树常驻(换页用 lv_screen_load 切换, 不删建);
//   应用页同一时刻只存在一棵, 退出即删;
//   宠物用纯 LVGL 对象绘制(复用首页树, 不占额外 flash 图片资产)。

#include "app_registry.h"

extern "C" void xz_heap_report(const char *tag);  // xiaozhi_client.c 临时heap诊断

#include <cstring>
#include <cstdio>
#include <ctime>

#include "lvgl.h"
#include "bsp_display.h"
#include "esp_log.h"

extern "C" {
#include "ui_pixel.h"
#include "ui_phosphor.h"
}
#include "radio_player.h"
#include "coding_bridge.h"
#include "wireless_bridge.h"
#include "radio_font.h"
#include "app.h"
#include "bsp_battery.h"
#include "wifi_manager.h"   // 顶栏信号格: WifiManager::IsConnected()/GetRssi()

static const char *kTag = "app_fw";
static volatile bool s_defer_minimized_services;

// ---------------------------------------------------------------------------
// 收音机/设置接口(外部实现); Codex 由 app_coding.c 提供
// ---------------------------------------------------------------------------
bool radio_app_enter(void);
void radio_app_exit(void);
void radio_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

bool codex_app_enter(void);
void codex_app_exit(void);
void codex_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

bool xiaozhi_app_enter(void);
void xiaozhi_app_exit(void);
void xiaozhi_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

bool settings_app_enter(void);
void settings_app_exit(void);
void settings_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void radio_app_pause(void);
void radio_app_resume(void);

// 应用注册表: 顺序 = 轮播顺序。新增应用 = 加一项 + 实现三函数。
// 注: 菜单副标题已按用户要求移除, 表项去掉 subtitle 字段(省字符串 flash)。
const app_entry_t APPS[APP_ID_COUNT] = {
    [APP_ID_RADIO]    = {"收音机",  radio_app_enter,    radio_app_exit,    radio_app_key,    nullptr,           nullptr},
    [APP_ID_CODEX]    = {"Codex",   codex_app_enter,    codex_app_exit,    codex_app_key,    radio_app_pause, nullptr},
    [APP_ID_XIAOZHI]  = {"小智AI",  xiaozhi_app_enter,  xiaozhi_app_exit,  xiaozhi_app_key,  radio_app_pause, nullptr},
    [APP_ID_SETTINGS] = {"设置",    settings_app_enter, settings_app_exit, settings_app_key, nullptr,           nullptr},
};



// ---------------------------------------------------------------------------
// 框架状态
// ---------------------------------------------------------------------------
static app_level_t s_level = APP_LEVEL_HOME;
static app_id_t    s_active = APP_ID_RADIO;
static int         s_menu_sel = 0;

// ---- 首页桌面 ----
static lv_obj_t *s_home_scr;
static lv_obj_t *s_home_pet;        // 宠物容器(含四态)
static lv_obj_t *s_home_body;       // 宠物身体(变色的部件)
static lv_obj_t *s_home_deco;       // 头顶装饰(gear/bubble/cross/zzz 共用容器)
static lv_obj_t *s_home_clock;
static lv_obj_t *s_home_date;
static lv_obj_t *s_home_np_card;    // 正在播放卡
static lv_obj_t *s_home_np_name;
static lv_obj_t *s_home_batt;
// Codex 计数徽标: 宠物右侧的"进行中 / 已完成 / 失败"。
// 用色块 + 数字表达, 不再用一整行文字(原先那行会被下方播放卡压住, 且
// 240px 宽下三组中文既挤又挡)。索引 0=进行中 1=已完成 2=失败。
static lv_obj_t *s_home_badge[3];
static lv_obj_t *s_home_badge_dot[3];
static lv_obj_t *s_home_badge_num[3];
static int32_t s_home_badge_last[3] = {-1, -1, -1};
// 顶栏信号格: 首页/菜单共用一组(单页面驻留, 同一时刻只有一棵树 + 一个 s_home_batt)
static lv_obj_t *s_home_sig[4];
static lv_timer_t *s_home_tick;
// 首页频谱条动画: 独立低频定时器(300ms ≈ 3.3 次/秒), 只在播放中动画。
// 与 home_tick(1000ms, 管时钟/顶栏)分开, 免得把整页刷新频率拉高。
static lv_timer_t *s_home_eq_tick;
static uint32_t s_eq_frame;              // 动画帧计数(驱动波形平移)
static int s_eq_last_h[20];              // 每根柱子上次高度(仅变化时才重绘)

// ---- 菜单 ----
static lv_obj_t *s_menu_scr;
static lv_obj_t *s_transition_scr;
static lv_obj_t *s_menu_icon;       // 中央大图标(切应用时换样式)
static lv_obj_t *s_menu_name;
static lv_obj_t *s_menu_dots[APP_ID_COUNT];
static lv_obj_t *s_menu_side[2];    // 左右预览小图标
// 菜单顶栏信号格复用 s_home_sig[](见上); 不再单独定义

// Codex 状态(首页宠物)
static volatile codex_state_t s_codex_state = CODEX_STATE_IDLE;
static volatile uint32_t s_home_codex_running;
static volatile uint32_t s_home_codex_done;
static volatile uint32_t s_home_codex_failed;
// 首页/菜单的最小化收音机播放意图。完整 Codex/小智会临时停播，但退出后
// 只恢复用户在最小化层真正想要的播放状态，不能把用户手动暂停覆盖掉。
static bool s_minimized_radio_should_play = true;

// 色板(与 ui_pixel.h 一致)
#define C_BG      0x000F05
#define C_SURFACE 0x0A251A
#define C_SOFT    0x123D28
#define C_BORDER  0x2ECC7F
#define C_FG      0x9AFCC6
#define C_MUTED   0x5FD9A0
#define C_ACCENT  0xD8FF7A
#define C_DANGER  0xFF9E9E

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

// 按需求字号选择字体。此前本函数**完全忽略 size_px**, 所有文字都渲染成
// ui_cn_font()(16px), 导致首页时钟/日期等设了大字号也不放大 —— 用户反馈
// "上面的内容都是小的"即根因于此。
//   >=24px -> radio_font_big(30px 点阵)
//   >=18px -> radio_font_title(20px)
//   其它   -> radio_font(16px)
static const lv_font_t *font_for_size(int size_px) {
    if (size_px >= 24) return &radio_font_big;
    if (size_px >= 18) return &radio_font_title;
    // ⚠ 这里曾经返回 &radio_font —— 那是按**白名单**生成的约 450 字字体,
    // 首页/菜单上任何不在名单里的汉字都会显示成"口"(用户反馈"好多口口")。
    // lv_font_cn_16 覆盖 GB2312 全集且已在固件内, 换成它彻底消除方框。
    // 18/24px 两档仍用专用字体: 它们只渲染固定的时间/日期字符, 白名单够用,
    // 且需要更大的字号。
    return ui_cn_font();
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int size_px, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font_for_size(size_px), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// ---------------------------------------------------------------------------
// 宠物绘制(纯LVGL对象, 复用; 状态只改颜色/眼睛/装饰)
// 结构: pet(56x60) { ant+antball, body{eyeL,eyeR,cheekL,cheekR}, deco }
// ---------------------------------------------------------------------------
static lv_obj_t *pet_eye_l, *pet_eye_r;

static lv_obj_t *pet_build(lv_obj_t *parent, int x, int y) {
    // 宠物整体放大到 72x78(原 56x60, 用户反馈"有点小")
    lv_obj_t *pet = box(parent, x, y, 72, 78, C_BG);

    // 天线
    lv_obj_t *ant = box(pet, 34, -9, 4, 11, C_FG);
    box(ant, -3, -7, 10, 10, C_ACCENT);
    // 身体
    lv_obj_t *body = box(pet, 0, 0, 72, 64, C_FG);
    lv_obj_set_style_radius(body, 31, 0);
    s_home_body = body;
    // 底部收口(弧形观感: 两条圆角块)
    lv_obj_t *skirt = box(pet, 5, 54, 62, 10, C_FG);
    lv_obj_set_style_radius(skirt, 6, 0);
    // 眼睛
    pet_eye_l = box(pet, 17, 23, 14, 18, C_BG);
    lv_obj_set_style_radius(pet_eye_l, LV_RADIUS_CIRCLE, 0);
    pet_eye_r = box(pet, 41, 23, 14, 18, C_BG);
    lv_obj_set_style_radius(pet_eye_r, LV_RADIUS_CIRCLE, 0);
    box(pet_eye_l, 4, 4, 5, 5, 0xFFFFFF);
    box(pet_eye_r, 4, 4, 5, 5, 0xFFFFFF);
    // 腮红
    box(pet, 6, 38, 9, 5, 0x4FCE9A);
    box(pet, 57, 38, 9, 5, 0x4FCE9A);

    // 状态装饰(\"z Z\" / \"*\" / \"!\" / \"x\")放到**宠物左侧**: 右侧已让给
    // Codex 计数徽标, 原来在 x+78 会与徽标重叠。
    s_home_deco = box(parent, x - 40, y + 10, 36, 24, C_BG);
    lv_obj_set_style_bg_opa(s_home_deco, LV_OPA_TRANSP, 0);
    return pet;
}

static void pet_apply_codex_state(codex_state_t st) {
    if (!s_home_body) return;
    uint32_t body = C_FG;
    uint32_t deco_fg = C_ACCENT;
    const char *deco_txt = "";
    switch (st) {
        case CODEX_STATE_BUSY: body = C_ACCENT; deco_txt = "*"; break;
        case CODEX_STATE_DONE: deco_txt = "!"; break;
        case CODEX_STATE_FAIL: body = C_DANGER; deco_txt = "x"; deco_fg = C_DANGER; break;
        case CODEX_STATE_OFFLINE: body = C_MUTED; deco_txt = "-"; deco_fg = C_MUTED; break;
        case CODEX_STATE_ATTENTION: body = C_ACCENT; deco_txt = "?"; break;
        case CODEX_STATE_IDLE:
        default: deco_txt = "z Z"; deco_fg = C_MUTED; break;
    }
    lv_obj_set_style_bg_color(s_home_body, lv_color_hex(body), 0);
    // 清掉旧装饰, 画新的
    lv_obj_clean(s_home_deco);
    if (deco_txt[0]) {
        lv_obj_t *d = label(s_home_deco, deco_txt, 10, deco_fg);
        lv_obj_center(d);
    }
}

// ---------------------------------------------------------------------------
// Codex 计数徽标: 宠物右侧竖排三枚"色点 + 数字"。
// 顺序固定为 进行中 / 已完成 / 失败, 靠颜色区分(黄绿 / 亮绿 / 红)。
// 计数为 0 的徽标整体隐藏, 所以空闲时右侧是干净的。
// ---------------------------------------------------------------------------
static void codex_badge_build(lv_obj_t *parent, int index, int y, uint32_t color) {
    s_home_badge[index] = box(parent, 166, y, 64, 20, C_BG);
    lv_obj_set_style_bg_opa(s_home_badge[index], LV_OPA_TRANSP, 0);
    s_home_badge_dot[index] = box(s_home_badge[index], 0, 7, 6, 6, color);
    lv_obj_set_style_radius(s_home_badge_dot[index], LV_RADIUS_CIRCLE, 0);
    s_home_badge_num[index] = label(s_home_badge[index], "0", 16, color);
    lv_obj_set_pos(s_home_badge_num[index], 12, 0);
    lv_obj_add_flag(s_home_badge[index], LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// 首页桌面
// 刷新顶栏状态(信号格 + 电量)。首页与菜单共用:
// 菜单页此前完全没有信号格、电量也从不刷新 -> 顶栏一直空白(用户反馈)。
//
// 布局策略(自适应, 避免硬编码坐标对不齐):
//   1) 电量右对齐到屏幕右边缘(固定右边距 4px);
//   2) 读取电量标签的**实际 x/宽度**, 把 4 格信号格紧贴摆到它左侧。
// 这样无论电量显示 "9%" 还是 "100%", 信号格与电量始终无缝, 且整组靠右。
static void refresh_top_bar(void) {
    // 电量
    const int pct = bsp_battery_level();
    if (pct >= 0 && s_home_batt) {
        char b[12];
        snprintf(b, sizeof(b), "%d%%", pct);
        lv_label_set_text(s_home_batt, b);
        lv_obj_set_style_text_color(s_home_batt,
                                    lv_color_hex(pct <= 15 ? C_DANGER : C_MUTED), 0);
    }
    // 电量: 右对齐到屏幕右边缘(内收 10px 避让屏幕圆角), y=4
    if (s_home_batt) {
        lv_obj_align(s_home_batt, LV_ALIGN_TOP_RIGHT, -10, 4);
    }
    // 信号格: 4 格, 按 RSSI 点亮。未连接 -> 全暗。
    bool connected = false;
    int rssi = -127;
    {
        auto &wifi = WifiManager::GetInstance();
        connected = wifi.IsConnected();
        if (connected) rssi = wifi.GetRssi();
    }
    // RSSI 分档: >=-55 满格, -55..-65 三格, -65..-75 两格, -75..-85 一格
    const int bars = !connected ? 0
                   : rssi >= -55 ? 4
                   : rssi >= -65 ? 3
                   : rssi >= -75 ? 2
                   : rssi >= -85 ? 1 : 0;
    // 信号格紧贴电量左侧: 电量标签实际左边界 - 组宽(15) - 间隙(6)
    int sig_left = 184;  // 兜底(标签尚未布局时)
    if (s_home_batt) {
        lv_obj_update_layout(s_home_batt);
        const int batt_x = lv_obj_get_x(s_home_batt);
        sig_left = batt_x - 15 - 6;
        if (sig_left < 4) sig_left = 4;
    }
    for (int i = 0; i < 4; ++i) {
        if (!s_home_sig[i]) continue;
        lv_obj_set_pos(s_home_sig[i], sig_left + i * 4, 12 + (3 - i));
        lv_obj_set_style_bg_color(s_home_sig[i],
            lv_color_hex(i < bars ? C_FG : C_SOFT), 0);
    }
}

// 首页频谱条动画(独立定时器, 300ms)。只在"首页可见 && 播放中"时真正重绘:
// 停止播放 -> 压成平线后停手; 离开首页 -> 定时器被 destroy_home 删掉。
// 高度用查表 + 帧偏移, 让波形缓慢流动, 视觉上"在动"但不刺眼。
static const uint8_t kEqPattern[20] = {
    8, 14, 20, 12, 24, 18, 10, 22, 16, 26,
    12, 20, 8, 18, 14, 24, 10, 16, 22, 12,
};

static void home_eq_draw(lv_event_t *event) {
    lv_obj_t *card = static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_area_t origin;
    lv_obj_get_coords(card, &origin);
    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.bg_color = lv_color_hex(C_MUTED);
    // 2026-10-26 崩溃修复: 频谱柱必须裁剪到卡片内再交给 lv_draw_rect。
    // 高度最高 32px, 卡片内高 78px 本应放得下; 但卡片自带 radius=6 圆角,
    // 柱子 x=13/63/... 的一根恰好压在圆角边缘时, lv_draw_sw_border 的
    // 复杂边框路径会在柱与圆角的交叠区越界写(lv_memset 崩溃, 反汇编
    // draw_border_complex -> lv_mask_radius 实测)。给柱子范围裁剪到
    // 卡片坐标 1..77(避开圆角与边框), 与圆角无交叠后路径保持简单矩形。
    lv_area_t clip = origin;
    clip.x1 += 2; clip.x2 -= 2;
    clip.y1 += 2; clip.y2 -= 2;
    for (int i = 0; i < 20; ++i) {
        const int height = s_eq_last_h[i] > 0 ? s_eq_last_h[i] : 6;
        const int x = origin.x1 + 13 + i * 10;
        lv_area_t bar = {x, origin.y1 + 77 - height, x + 6, origin.y1 + 77};
        // 裁剪到卡片安全区; 完全在外的柱子跳过。
        if (bar.x1 < clip.x1) bar.x1 = clip.x1;
        if (bar.x2 > clip.x2) bar.x2 = clip.x2;
        if (bar.y1 < clip.y1) bar.y1 = clip.y1;
        if (bar.y2 > clip.y2) bar.y2 = clip.y2;
        if (bar.x2 <= bar.x1 || bar.y2 <= bar.y1) continue;
        lv_draw_rect(lv_event_get_layer(event), &descriptor, &bar);
    }
}

static void home_eq_tick(lv_timer_t *timer) {
    (void)timer;
    if (!s_home_scr || !s_home_np_card) return;
    const bool playing = radio_player_is_playing();
    if (!playing) {
        // 停止: 只需压平一次(靠 s_eq_last_h 比对, 不会反复重绘)
        bool changed = false;
        for (int i = 0; i < 20; ++i) {
            if (s_eq_last_h[i] == 6) continue;
            s_eq_last_h[i] = 6;
            changed = true;
        }
        if (changed) lv_obj_invalidate(s_home_np_card);
        return;
    }
    ++s_eq_frame;
    for (int i = 0; i < 20; ++i) {
        // 帧偏移让波形向左流动(每帧错开 1 格)
        const int h = 6 + kEqPattern[(i + s_eq_frame) % 20];
        if (h == s_eq_last_h[i]) continue;   // ⚠ 只在变化时才动 LVGL 对象
        s_eq_last_h[i] = h;
        // ⚠ 底边固定 y=78, 高度变化时顶部上移(同时改 pos+size, 否则会向下长)
    }
    lv_obj_invalidate(s_home_np_card);
}

// 把聚合计数写进宠物右侧的三个徽标。数字为 0 时隐藏该徽标, 避免首页出现
// 一串 0。只在数字变化时改文案, 不做每秒重绘。
static void refresh_home_codex(void) {
    const uint32_t values[3] = {s_home_codex_running, s_home_codex_done,
                                s_home_codex_failed};
    for (int i = 0; i < 3; ++i) {
        if (s_home_badge_num[i] == nullptr) continue;
        const int32_t v = (int32_t)values[i];
        if (v == s_home_badge_last[i]) continue;
        s_home_badge_last[i] = v;
        char text[12];
        snprintf(text, sizeof(text), "%d", (int)v);
        lv_label_set_text(s_home_badge_num[i], text);
        // 计数为 0 时整枚徽标隐藏(含圆点与数字)。
        if (v > 0) lv_obj_remove_flag(s_home_badge[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_home_badge[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// 文案去抖缓存。必须与页面生命周期绑定: 首页/菜单切换会销毁并重建标签,
// 新标签回到占位符("--:--"/"--"), 而缓存里仍是旧值 -> strcmp 相等就直接跳过,
// 于是标签永远停在占位符。日期只在跨天时变化, 所以一旦命中会整天显示"--"。
// 归零后下一次 tick 必然写入真实值。
static char s_last_clock[8] = "";
static char s_last_date[32] = "";

static void home_tick(lv_timer_t *timer) {
    (void)timer;
    if (!s_home_scr) return;
    // 首页宠物只由最小化 Codex 的后台状态驱动；完整 Codex 页面即使退出后
    // 仍保留过状态，也不能反向覆盖首页/菜单的最小化状态。
    // 时钟
    const time_t now = ::time(nullptr);
    if (now > 1700000000) {
        tm local = {};
        ::localtime_r(&now, &local);
        char t[8], d[32];
        snprintf(t, sizeof(t), "%02d:%02d", local.tm_hour, local.tm_min);
        static const char *W[] = {"日","一","二","三","四","五","六"};
        snprintf(d, sizeof(d), "%02d月%02d日 周%s", local.tm_mon + 1, local.tm_mday,
                 W[local.tm_wday % 7]);
        // 只有文案真的变了才 set_text: LVGL 的 set_text 会无条件失效重绘,
        // 逐秒重画是首页闲置发热的来源之一。
        if (strcmp(s_last_clock, t) != 0) {
            strcpy(s_last_clock, t);
            lv_label_set_text(s_home_clock, t);
        }
        if (strcmp(s_last_date, d) != 0) {
            strcpy(s_last_date, d);
            lv_label_set_text(s_home_date, d);
        }
    }
    refresh_home_codex();
    // 顶栏状态(信号 + 电量): 电量/RSSI 变化很慢, 每 5 秒刷一次就够,
    // 不必 1Hz 重排一遍标签和信号格。
    static unsigned top_bar_tick = 5;
    if (++top_bar_tick >= 5U) {
        top_bar_tick = 0U;
        refresh_top_bar();
    }
    // 频谱条动画交给独立的低频定时器 home_eq_tick(300ms), 这里不再逐帧重绘。
    const bool playing = radio_player_is_playing();
    // 单行文案: "正在播放 音乐名"(拼接, 不再占两行); 同样只在变化时更新。
    if (s_home_np_name) {
        char line[96];
        if (playing) {
            const RadioStation st = radio_player_station(radio_player_station_index());
            snprintf(line, sizeof(line), "正在播放 %s", st.name[0] ? st.name : "");
        } else {
            snprintf(line, sizeof(line), "已停止");
        }
        static char last_line[96] = "";
        if (strcmp(last_line, line) != 0) {
            strcpy(last_line, line);
            lv_label_set_text(s_home_np_name, line);
        }
    }
}

static void home_build(void) {
    // 复位去抖缓存: 新标签内容是占位符, 缓存必须同步失效(见 s_last_clock 注释)。
    s_last_clock[0] = '\0';
    s_last_date[0] = '\0';
    s_home_scr = box(nullptr, 0, 0, 240, 320, C_BG);

    // 顶栏
    box(s_home_scr, 0, 0, 240, 22, 0x04180C);
    box(s_home_scr, 0, 21, 240, 1, C_SOFT);
    // 信号格(4 格) + 电量: 位置由 refresh_top_bar() 自适应排布(电量右对齐,
    // 信号格按电量实际宽度紧贴其左)。这里只创建对象, 坐标是兜底初值。
    for (int i = 0; i < 4; ++i) {
        s_home_sig[i] = box(s_home_scr, 184 + i * 4, 12 + (3 - i), 3, 3 + i, C_SOFT);
    }
    s_home_batt = label(s_home_scr, "--", 10, C_MUTED);
    lv_obj_set_pos(s_home_batt, 201, 4);
    // 左侧标题: 屏幕四角有圆角, 内收 10px(约一个字符宽)避免被圆角遮挡
    lv_obj_t *ttl = label(s_home_scr, "PASSPORT", 10, C_FG);
    lv_obj_set_pos(ttl, 10, 4);
    refresh_top_bar();   // 立即按实际宽度摆好信号格/电量

    // 宠物(整体 72x78; 顶部留 22 状态栏, 宠物占 34..112)
    s_home_pet = pet_build(s_home_scr, 84, 34);
    // 时钟/日期(时钟加大到 30px, 日期 16px —— 原都被强制渲染成 16px)
    s_home_clock = label(s_home_scr, "--:--", 30, C_FG);
    lv_obj_set_pos(s_home_clock, 0, 120);
    lv_obj_set_width(s_home_clock, 240);
    lv_obj_set_style_text_align(s_home_clock, LV_TEXT_ALIGN_CENTER, 0);
    s_home_date = label(s_home_scr, "--", 18, C_MUTED);
    lv_obj_set_pos(s_home_date, 0, 160);
    lv_obj_set_width(s_home_date, 240);
    lv_obj_set_style_text_align(s_home_date, LV_TEXT_ALIGN_CENTER, 0);

    // Codex 计数徽标: 宠物右侧竖排(166..230, 40..112), 与宠物同一视觉带。
    // 旧实现是一整行 240px 宽的中文, 位于 y=183, 而播放卡从 y=196 开始,
    // 且该标签实际用 16px 字体, 于是"运行"被卡片压掉一半 —— 用户反馈的
    // "已完成被框挡住"。改为数字徽标后彻底避开卡片区域。
    for (int i = 0; i < 3; ++i) s_home_badge_last[i] = -1;
    codex_badge_build(s_home_scr, 0, 40, C_ACCENT);   // 进行中
    codex_badge_build(s_home_scr, 1, 66, C_FG);       // 已完成
    codex_badge_build(s_home_scr, 2, 92, C_DANGER);   // 失败

    // 正在播放卡(196..276, 卡片内坐标 0..80)
    s_home_np_card = box(s_home_scr, 12, 196, 216, 80, C_SURFACE);
    lv_obj_set_style_border_color(s_home_np_card, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(s_home_np_card, 1, 0);
    lv_obj_set_style_radius(s_home_np_card, 6, 0);
    // 单行文案: "正在播放 音乐" —— 原来是"正在播放"(标题) + 电台名(第二行)两行,
    // 用户要求合成一行。用前缀 + 动态电台名拼成一条。
    s_home_np_name = label(s_home_np_card, "正在播放", 16, C_FG);
    lv_obj_set_style_text_font(s_home_np_name, ui_cn_font(), 0);
    lv_obj_set_pos(s_home_np_name, 10, 10);
    lv_obj_set_width(s_home_np_name, 196);              // 卡片宽 216 - 左右各 10
    lv_label_set_long_mode(s_home_np_name, LV_LABEL_LONG_DOT);  // 过长自动省略号
    // 频谱条: 底边紧贴卡片底部内侧。卡片高 80, 内高 78 -> 底边基准 y=78。
    // 原来底边在卡片内 y=62, 距底还有 18px 空隙(用户反馈"没贴到底部")。
    lv_obj_add_event_cb(s_home_np_card, home_eq_draw, LV_EVENT_DRAW_MAIN, nullptr);

    // 底部提示: 说明首页按键(长按OK = 退出收音机/暂停播放)
    lv_obj_t *h = label(s_home_scr, "OK 暂停/继续   长按OK 菜单", 10, 0x3F8F68);
    lv_obj_set_pos(h, 0, 296);
    lv_obj_set_width(h, 240);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_screen_load(s_home_scr);
    s_home_tick = lv_timer_create(home_tick, 1000, nullptr);
    // 频谱条动画: 独立 300ms 定时器(低频, 省 CPU)
    for (int i = 0; i < 20; ++i) s_eq_last_h[i] = -1;   // 强制首次全部重绘
    s_eq_frame = 0;
    s_home_eq_tick = lv_timer_create(home_eq_tick, 300, nullptr);
    pet_apply_codex_state((codex_state_t)s_codex_state);
    refresh_home_codex();
}

static void menu_build(void) {
    s_menu_scr = box(nullptr, 0, 0, 240, 320, C_BG);
    box(s_menu_scr, 0, 0, 240, 22, 0x04180C);
    box(s_menu_scr, 0, 21, 240, 1, C_SOFT);
    lv_obj_t *ttl = label(s_menu_scr, "APPS", 10, C_FG);
    lv_obj_set_pos(ttl, 10, 4);   // 内收 10px 避让屏幕圆角
    // 顶栏信号格(4 格) + 电量。此前只建了电量、且从未刷新 -> 顶栏一直空白。
    // 坐标由 refresh_top_bar() 自适应排布(与首页一致)。
    for (int i = 0; i < 4; ++i) {
        s_home_sig[i] = box(s_menu_scr, 184 + i * 4, 12 + (3 - i), 3, 3 + i, C_SOFT);
    }
    s_home_batt = label(s_menu_scr, "--", 10, C_MUTED);
    lv_obj_set_pos(s_home_batt, 201, 4);

    // 中央大图标 100x100 @ y 66..166(状态栏22+16 => 38 起, 顶部留白一致)
    s_menu_icon = box(s_menu_scr, 70, 66, 100, 100, C_SURFACE);
    lv_obj_set_style_border_color(s_menu_icon, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(s_menu_icon, 2, 0);
    lv_obj_set_style_radius(s_menu_icon, 22, 0);

    s_menu_name = label(s_menu_scr, "", 16, C_FG);
    lv_obj_set_pos(s_menu_name, 0, 182);
    lv_obj_set_width(s_menu_name, 240);
    lv_obj_set_style_text_align(s_menu_name, LV_TEXT_ALIGN_CENTER, 0);

    // 注: 菜单项副标题(如"网络电台 · 全国频道")已按用户要求移除, 只留应用名。

    for (int i = 0; i < APP_ID_COUNT; ++i) {
        s_menu_dots[i] = box(s_menu_scr, 0, 0, 6, 6, C_SOFT);
        lv_obj_set_style_radius(s_menu_dots[i], LV_RADIUS_CIRCLE, 0);
    }
    lv_obj_t *h = label(s_menu_scr, "上下选 · OK 进入", 10, 0x3F8F68);
    lv_obj_set_pos(h, 0, 298);
    lv_obj_set_width(h, 240);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
}

static uint32_t menu_icon_surface(lv_obj_t *parent) {
    return parent == s_menu_icon ? C_SURFACE : C_SOFT;
}

// 菜单图标统一由 LVGL 图元绘制, 不依赖字体里的 ASCII/Unicode 字形。
static lv_obj_t *icon_outline(lv_obj_t *parent, int x, int y, int w, int h,
                              int radius, int border, uint32_t color) {
    lv_obj_t *o = box(parent, x, y, w, h, color);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(o, border, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t *icon_line(lv_obj_t *parent, const lv_point_precise_t *points,
                           uint32_t count, int width, uint32_t color) {
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_set_pos(line, 18, 18);
    lv_obj_set_size(line, 64, 64);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_line_set_points(line, points, count);
    return line;
}

static lv_obj_t *icon_fill(lv_obj_t *parent, int x, int y, int w, int h,
                           uint32_t color, int radius) {
    lv_obj_t *o = box(parent, x, y, w, h, color);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static void menu_radio_icon(lv_obj_t *parent) {
    // 收音机: 机身 + 天线 + 扬声器 + 调谐旋钮。
    static const lv_point_precise_t antenna[] = {{48, 20}, {58, 10}};
    icon_line(parent, antenna, 2, 3, C_FG);
    icon_fill(parent, 73, 24, 6, 6, C_ACCENT, LV_RADIUS_CIRCLE);
    icon_outline(parent, 24, 38, 52, 36, 8, 4, C_FG);
    icon_outline(parent, 31, 45, 19, 19, LV_RADIUS_CIRCLE, 3, C_FG);
    icon_fill(parent, 35, 50, 11, 2, C_FG, 1);
    icon_fill(parent, 35, 54, 11, 2, C_FG, 1);
    icon_fill(parent, 35, 58, 11, 2, C_FG, 1);
    icon_fill(parent, 61, 49, 8, 8, C_ACCENT, LV_RADIUS_CIRCLE);
    icon_fill(parent, 64, 42, 3, 3, C_MUTED, LV_RADIUS_CIRCLE);
}

static void menu_codex_icon(lv_obj_t *parent) {
    // Codex: 终端窗口 + 提示符箭头 + 光标。
    static const lv_point_precise_t prompt[] = {{16, 32}, {24, 39}, {16, 46}};
    icon_outline(parent, 22, 28, 56, 44, 8, 4, C_FG);
    icon_fill(parent, 25, 39, 50, 2, C_FG, 1);
    icon_fill(parent, 29, 33, 3, 3, C_MUTED, LV_RADIUS_CIRCLE);
    icon_fill(parent, 35, 33, 3, 3, C_MUTED, LV_RADIUS_CIRCLE);
    icon_fill(parent, 41, 33, 3, 3, C_MUTED, LV_RADIUS_CIRCLE);
    icon_line(parent, prompt, 3, 4, C_ACCENT);
    icon_fill(parent, 44, 56, 14, 4, C_ACCENT, 2);
}

static void menu_xiaozhi_icon(lv_obj_t *parent) {
    // 小智 AI: 对话气泡 + 声波。
    static const lv_point_precise_t tail[] = {{20, 46}, {16, 64}, {28, 59}};
    icon_outline(parent, 22, 28, 56, 38, 13, 4, C_FG);
    icon_line(parent, tail, 3, 4, C_FG);
    icon_fill(parent, 43, 44, 3, 7, C_ACCENT, 2);
    icon_fill(parent, 49, 39, 3, 17, C_ACCENT, 2);
    icon_fill(parent, 55, 35, 3, 25, C_ACCENT, 2);
    icon_fill(parent, 61, 39, 3, 17, C_ACCENT, 2);
    icon_fill(parent, 67, 44, 3, 7, C_ACCENT, 2);
}

static void menu_settings_icon(lv_obj_t *parent) {
    // 设置: 三条滑杆 + 旋钮, 比星号更直观, 也与其它线性图标同宽。
    const uint32_t surface = menu_icon_surface(parent);
    icon_fill(parent, 28, 37, 44, 3, C_FG, 2);
    icon_fill(parent, 28, 51, 44, 3, C_FG, 2);
    icon_fill(parent, 28, 65, 44, 3, C_FG, 2);
    lv_obj_t *top = icon_outline(parent, 36, 32, 11, 11, LV_RADIUS_CIRCLE, 3, C_FG);
    lv_obj_set_style_bg_color(top, lv_color_hex(surface), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_t *middle = icon_outline(parent, 54, 46, 11, 11, LV_RADIUS_CIRCLE, 3, C_ACCENT);
    lv_obj_set_style_bg_color(middle, lv_color_hex(surface), 0);
    lv_obj_set_style_bg_opa(middle, LV_OPA_COVER, 0);
    lv_obj_t *bottom = icon_outline(parent, 46, 60, 11, 11, LV_RADIUS_CIRCLE, 3, C_FG);
    lv_obj_set_style_bg_color(bottom, lv_color_hex(surface), 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, 0);
}

static void menu_icon_symbol(lv_obj_t *parent, app_id_t id) {
    lv_obj_clean(parent);
    switch (id) {
        case APP_ID_RADIO:    menu_radio_icon(parent); break;
        case APP_ID_CODEX:    menu_codex_icon(parent); break;
        case APP_ID_XIAOZHI:  menu_xiaozhi_icon(parent); break;
        case APP_ID_SETTINGS: menu_settings_icon(parent); break;
        default: break;
    }
}

static void menu_refresh(void) {
    const app_entry_t &app = APPS[s_menu_sel];
    menu_icon_symbol(s_menu_icon, (app_id_t)s_menu_sel);
    lv_label_set_text(s_menu_name, app.name);
    // 顶栏信号格/电量: 菜单没有自己的 tick, 借 menu_refresh 刷新
    // (进入菜单时调一次; 菜单内上下选也会调, 顺带更新)
    refresh_top_bar();
    for (int i = 0; i < APP_ID_COUNT; ++i) {
        lv_obj_set_style_bg_color(s_menu_dots[i],
            lv_color_hex(i == s_menu_sel ? C_ACCENT : C_SOFT), 0);
        // 点阵摆位: 底部居中
        lv_obj_set_pos(s_menu_dots[i], 120 - APP_ID_COUNT * 6 + i * 12, 286);
    }
}

// ---------------------------------------------------------------------------
// 输入分发(全局导航)
// ---------------------------------------------------------------------------
// 【内存策略 — 单页面驻留(2026-10-08 重构)】
// 原设计: 首页树 + 菜单树常驻, 进应用再叠第三棵 -> 进应用时 free 仅 ~19KB,
// 收音机/小智要再分配缓冲就枯竭卡死。
// 现设计: 同一时刻**只保留一棵页面树**。切页时先销毁旧树、再建新树, 让
// LVGL 对象占用的堆在页面之间反复回收复用(用户提出的"没有后台应用概念")。
// 首页/菜单都是纯静态布局, 重建成本低(<5ms), 换来的是每个页面都能拿到
// 完整的空闲堆。
static void destroy_home(void) {
    if (s_home_tick) { lv_timer_delete(s_home_tick); s_home_tick = nullptr; }
    if (s_home_eq_tick) { lv_timer_delete(s_home_eq_tick); s_home_eq_tick = nullptr; }
    if (s_home_scr) { lv_obj_delete(s_home_scr); s_home_scr = nullptr; }
    s_home_pet = s_home_body = s_home_deco = s_home_clock = s_home_date = nullptr;
    s_home_np_card = s_home_np_name = s_home_batt = nullptr;
    for (int i = 0; i < 3; ++i) {
        s_home_badge[i] = s_home_badge_dot[i] = s_home_badge_num[i] = nullptr;
        s_home_badge_last[i] = -1;
    }
    for (int i = 0; i < 4; ++i) s_home_sig[i] = nullptr;
}

static void destroy_menu(void) {
    if (s_menu_scr) { lv_obj_delete(s_menu_scr); s_menu_scr = nullptr; }
    s_menu_icon = s_menu_name = nullptr;
    for (int i = 0; i < APP_ID_COUNT; ++i) s_menu_dots[i] = nullptr;
    s_menu_side[0] = s_menu_side[1] = nullptr;
    // s_home_batt / s_home_sig 被首页与菜单共用(见上方注释), 一并清空
    s_home_batt = nullptr;
    for (int i = 0; i < 4; ++i) s_home_sig[i] = nullptr;
}

// 注意: s_home_batt 被首页与菜单共用, 销毁任一页都要清掉, 重建时再赋。
static void ensure_home(void) {
    if (!s_home_scr) home_build();
    lv_screen_load(s_home_scr);
}

static void ensure_menu(void) {
    if (!s_menu_scr) menu_build();
    menu_refresh();
    lv_screen_load(s_menu_scr);
}

// 统一的"退出当前应用": 销毁应用页 + 恢复音频。仅当当前处于 APP 层时有效。
// 从 APP 回 MENU(长按UP) 与 从 APP 回 HOME 都要走它, 否则应用页的 LVGL 树
// 会残留且播放回调继续跑(实测触发看门狗)。
static void leave_active_app(void) {
    if (s_level != APP_LEVEL_APP) return;
    const app_entry_t &app = APPS[s_active];
    if (app.exit) app.exit();
    if (app.audio_resume) app.audio_resume();
    s_active = APP_ID_RADIO;
}

void app_framework_go_home(void);   // 导出: 供配网退出等系统路径调用
static void go_home(void) {
    xz_heap_report("go_home_in");
    if (!bsp_lvgl_lock(1500)) return;
    lv_screen_load(s_transition_scr);
    leave_active_app();
    s_level = APP_LEVEL_HOME;
    destroy_menu();
    ensure_home();
    bsp_lvgl_unlock();
    // app.exit() runs while s_level is still APP_LEVEL_APP, so the minimize
    // helper intentionally ignores it there. Restore only after the framework
    // has actually returned to the home layer and the LVGL lock is released.
    if (s_level == APP_LEVEL_HOME) app_framework_minimize_services(true);
    xz_heap_report("go_home_out");
}

static void go_menu(void) {
    xz_heap_report("go_menu_in");
    if (!bsp_lvgl_lock(1500)) return;
    lv_screen_load(s_transition_scr);
    leave_active_app();                       // 从应用回菜单: 先销毁应用页
    s_level = APP_LEVEL_MENU;
    s_menu_sel = 0;
    // LVGL 锁支持递归；整次切页使用同一把锁，避免停留在过渡屏。
    destroy_home();
    ensure_menu();
    bsp_lvgl_unlock();
    // The ordered restore may synchronously reclaim a 20KB decoder block,
    // which must not hold up the LVGL/input path.
    if (s_level == APP_LEVEL_MENU) app_framework_minimize_services(true);
    xz_heap_report("go_menu_out");
}

void app_framework_go_menu(void) { go_menu(); }

// 自动化测试入口: 与点击菜单项走同一条 enter_app 路径。
// 调用方负责保证在 LVGL 任务上下文(定时器/按键分发)里调用。


static void enter_app(app_id_t id) {
    ESP_LOGI(kTag, "enter_app id=%d name=%s", (int)id, APPS[id].name);
    // A full-screen app may need the control channel.
    wireless_bridge_set_home_only(false);
    xz_heap_report("enter_app_in");
    const app_entry_t &app = APPS[id];
    // 记住进入完整应用前的最小化收音机状态。Codex/小智会暂时停播并释放
    // 解码池，返回首页/菜单时再按用户原本的播放意图恢复。
    s_minimized_radio_should_play = radio_player_is_playing();
    if (app.audio_pause) app.audio_pause();
    if (!bsp_lvgl_lock(1500)) return;
    lv_screen_load(s_transition_scr);
    destroy_menu();
    // ⚠⚠ 内存(2026-10-12 修复"进应用就卡死发烫"):
    //   首页树必须在进应用前销毁, 否则它和应用页同时常驻。
    //   实测不销毁时进入收音机堆从 12076 掉到 1176、dma=48, LVGL 随即
    //   分配失败卡死, task_wdt 每 5s 触发一次 —— 表现就是"一进应用就卡住、
    //   发烫、还一直响"。首页重建很便宜(只在回首页时 ensure_home),
    //   换来的 ~7-10KB 余量是收音机能解码、能播的前提。
    destroy_home();
    // 当前活动屏已由空屏承接，应用创建时不再与菜单树争用堆。
    if (!app.enter()) {
        ESP_LOGE(kTag, "app %s enter failed", app.name);
        ensure_menu();
        bsp_lvgl_unlock();
        if (app.audio_resume) app.audio_resume();
        return;
    }
    xz_heap_report("enter_app_after_enter");
    s_active = id;
    s_level = APP_LEVEL_APP;
    bsp_lvgl_unlock();
    xz_heap_report("enter_app_out");
}

// input_task 调; 代替旧的 main.cc 按键分发
void app_framework_dispatch(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 配网模式优先(系统级): 由 wifi 模块内部处理, 这里不截获

    // 长按UP 全局返回
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        if (s_level == APP_LEVEL_APP &&
            (s_active == APP_ID_SETTINGS || s_active == APP_ID_RADIO)) {
            extern bool settings_app_back(void);
            if (settings_app_back()) return;
        }
        if (s_level == APP_LEVEL_APP && s_active == APP_ID_CODEX) {
            if (!bsp_lvgl_lock(300)) return;
            bool handled = app_coding_back();
            bsp_lvgl_unlock();
            if (handled) return;
        }
        if (s_level == APP_LEVEL_APP)   { go_menu(); }
        else if (s_level == APP_LEVEL_MENU) { go_home(); }
        else { go_menu(); }  // 首页长按UP = 进菜单
        return;
    }
    // 配网等待页长按OK 强制跳过(由 wifi 模块在自己 task 处理, 这里不管)

    if (s_level == APP_LEVEL_HOME) {
        // 首页最小化收音机: 点击 OK 暂停/恢复当前频道, 长按 OK 进菜单。
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) { go_menu(); }
        else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            radio_player_toggle();
            s_minimized_radio_should_play = radio_player_is_playing();
            if (bsp_lvgl_lock(200)) {
                if (s_home_np_name) lv_label_set_text(s_home_np_name, "正在切换");
                bsp_lvgl_unlock();
            }
        }
        else if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
            // 首页长按"下键" = 快捷进入 Codex(编程伴侣), 不看任务状态, 行为可预期。
            // 注意: 本机堆很紧(回到首页后空闲约 25KB), 首页树和 Codex 页不能同时驻留,
            // 所以先像 go_menu() 那样把首页拆掉, 再走"从菜单进入"的同一条路径。
            ESP_LOGI(kTag, "home: long-press DOWN -> Codex shortcut");
            xz_heap_report("home_codex_shortcut_in");
            if (bsp_lvgl_lock(1500)) {
                lv_screen_load(s_transition_scr);
                destroy_home();
                bsp_lvgl_unlock();
            }
            s_level = APP_LEVEL_MENU;      // 首页已拆掉, 失败回退 ensure_menu() 才自洽
            s_menu_sel = APP_ID_CODEX;
            enter_app(APP_ID_CODEX);
            xz_heap_report("home_codex_shortcut_out");
        }
        return;
    }
    if (s_level == APP_LEVEL_MENU) {
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            s_menu_sel = (s_menu_sel + APP_ID_COUNT - 1) % APP_ID_COUNT;
            if (bsp_lvgl_lock(300)) { menu_refresh(); bsp_lvgl_unlock(); }
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            s_menu_sel = (s_menu_sel + 1) % APP_ID_COUNT;
            if (bsp_lvgl_lock(300)) { menu_refresh(); bsp_lvgl_unlock(); }
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            enter_app((app_id_t)s_menu_sel);
        }
        return;
    }
    // APP_LEVEL_APP
    const app_entry_t &app = APPS[s_active];
    if (app.key) app.key(btn, ev);
}

// Codex 状态 -> 首页宠物
void app_home_set_codex_state(codex_state_t state) {
    s_codex_state = state;
    if (s_level == APP_LEVEL_HOME && s_home_body) {
        if (bsp_lvgl_lock(200)) {
            pet_apply_codex_state(state);
            bsp_lvgl_unlock();
        }
    }
}

void app_home_set_codex_counts(uint32_t running, uint32_t done,
                               uint32_t failed) {
    s_home_codex_running = running;
    s_home_codex_done = done;
    s_home_codex_failed = failed;
    if (s_level == APP_LEVEL_HOME && s_home_badge_num[0] != nullptr) {
        if (bsp_lvgl_lock(200)) {
            refresh_home_codex();
            bsp_lvgl_unlock();
        }
    }
}

app_level_t app_framework_level(void) { return s_level; }
app_id_t    app_framework_active(void) { return s_active; }

void app_framework_init(void) {
    // Boot lands on the home layer, so start in home-only transport mode.
    wireless_bridge_set_home_only(true);
    // 只建首页; 菜单与各应用页都在被使用时才建(单页面驻留, 见 go_menu/enter_app)。
    s_level = APP_LEVEL_HOME;
    if (bsp_lvgl_lock(1000)) {
        s_transition_scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(s_transition_scr, lv_color_hex(C_BG), 0);
        home_build();
        bsp_lvgl_unlock();
    }
    ESP_LOGI(kTag, "framework ready: %d apps", (int)APP_ID_COUNT);
}

// 收音机暂停/恢复: 停流但不切页(收音机UI由 radio_app_exit 处理)
void radio_app_pause(void)  {
    radio_player_set_playing(false);
    // HTTP 流请求最长等待 7s；必须等网络 worker 完整退出再让 radio_app_enter
    // 重新抢占解码池，否则会与旧 worker 并发持有缓冲，触发低内存/看门狗。
    if (!radio_player_wait_idle(2000)) {
        ESP_LOGW(kTag, "radio pause timed out before app switch (worker still winding down)");
    }
}
void radio_app_resume(void) { /* 不自动恢复播放; 用户回收音机应用自行按OK */ }

void app_framework_minimize_services(bool restore_radio) {
    if (s_level != APP_LEVEL_HOME && s_level != APP_LEVEL_MENU) return;
    // Home/menu: no CBDP TCP session; only UDP discovery + counters.
    wireless_bridge_set_home_only(true);
    if (s_defer_minimized_services) {
        // A cancelled XiaoZhi connect worker is still winding down and may
        // hold the Opus arena / TLS buffers. Pulling the radio decoder or the
        // Codex bridge back now would fight it for the same contiguous heap.
        // app_apps.cc restores services from the worker after Opus is freed.
        return;
    }

    // 2026-10-25 (尾部竞技场): 桥接任务栈占 mic 静态区**前 12KB**, 解码竞技场
    // 只借**尾巴 9.2KB**(见 xiaozhi_client.c)—— 两者不重叠, 所以桥接**常驻**:
    // 首页的 Codex 计数/完成提醒在收音机播放期间照常更新(首页小 Codex + 小
    // 收音机并存)。解码子块进尾巴后堆多 ~8.8KB, WiFi 收包(需 ~1600B 连续
    // DMA)不再被挤死 —— 旧方案 A(停桥)的离线代价取消。
    if (restore_radio && s_minimized_radio_should_play) {
        // 2026-10-25: 小智/完整 Codex 退出时停过桥 —— 首页恢复播放前必须
        // 补启, 否则计数/提醒死掉、电脑端看到设备离线(实测漏了这步)。
        if (!coding_bridge_wireless_started()) {
            if (coding_bridge_start_wireless() != ESP_OK) {
                ESP_LOGW(kTag, "minimized Codex bridge could not start");
            }
        }
        // This path runs after the caller has released the LVGL lock. Use the
        // synchronous ordered helper so the decoder owns its contiguous block
        // before any bridge resource could be allocated.
        radio_player_rewake_after_pool_ready();
        radio_player_set_playing(true);
        coding_bridge_set_foreground_ui(false);
        return;
    }

    /* Miniature services own the home/menu layer only. Full-screen apps stop
     * these services before taking their resources, and this is the single
     * place that brings them back when the framework leaves an app. */
    if (coding_bridge_start_wireless() != ESP_OK) {
        ESP_LOGW(kTag, "minimized Codex bridge could not start");
    }
    coding_bridge_set_foreground_ui(false);
}

void app_framework_note_minimized_radio_playing(bool playing) {
    s_minimized_radio_should_play = playing;
}

void app_framework_defer_minimized_services_restore(void) {
    s_defer_minimized_services = true;
}

void app_framework_restore_minimized_services_ordered(void) {
    // Clear the defer flag first: the restore may be requested from the
    // XiaoZhi worker after the user has already entered another app, and a
    // stuck flag would block every later home/menu restore.
    s_defer_minimized_services = false;
    if (s_level != APP_LEVEL_HOME && s_level != APP_LEVEL_MENU) return;
    // Rebuild the MP3 decoder first: the decoder's big sub-blocks go to the
    // static mic tail (bridge task stack in the FRONT 12KB does not overlap),
    // so the resident bridge keeps counters/alerts alive on home.
    if (s_minimized_radio_should_play) {
        if (!coding_bridge_wireless_started()) {
            if (coding_bridge_start_wireless() != ESP_OK) {
                ESP_LOGW(kTag, "minimized Codex bridge could not start");
            }
        }
        radio_player_rewake_after_pool_ready();
        radio_player_set_playing(true);
        coding_bridge_set_foreground_ui(false);
        return;
    }
    if (coding_bridge_start_wireless() != ESP_OK) {
        ESP_LOGW(kTag, "minimized Codex bridge could not start");
    }
    coding_bridge_set_foreground_ui(false);
}

// 系统级"回首页"入口(配网退出、异常恢复等): 退出当前应用并载入首页
void app_framework_go_home(void) { go_home(); }

// 自动化测试入口: 与点击菜单项走同一条 enter_app 路径。
// 调用方负责保证在 LVGL 任务上下文(定时器回调)里调用。
void app_framework_launch_app(int app_id) {
    if (app_id < 0 || app_id >= (int)APP_ID_COUNT) {
        ESP_LOGW(kTag, "launch_app: bad id %d", app_id);
        return;
    }
    enter_app((app_id_t)app_id);
}
