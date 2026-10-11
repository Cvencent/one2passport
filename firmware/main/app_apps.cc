// app_apps.cc —— 四个应用对 app_registry 的薄适配
//
// 收音机/设置: 复用现有 radio_ui.cc 的页面构建与按键处理(radio_ui_show_leo_main /
// radio_ui_show_settings 等), 那套代码已实机验证过, 不重写。
// Codex: app_coding.c 的 enter/exit/key 原样接入。
// 小智AI: 本轮先落占位页(宠物+提示), 语音协议接入在下个迭代。
//
// 应用页 UI 挂在 radio_ui 自管理的 s_screen 树上(与框架首页/菜单是不同的 screen),
// enter=建页并 lv_screen_load, exit=回框架首页并恢复首页 screen。

#include "app_registry.h"
#include "radio_ui.h"
#include "radio_player.h"
#include "radio_font.h"          // radio_font_big(设备激活码大字显示)
#include "radio_sleep_timer.h"
#include "coding_bridge.h"
#include "xiaozhi_client.h"
#include "radio_display_settings.h"
#include "ssid_manager.h"
#include <algorithm>
#include <array>
extern "C" {
#include "ui_pixel.h"
}
#include "app.h"
#include "lvgl.h"
#include "bsp_display.h"
#include "esp_log.h"

// ---- main.cc 提供的桥(C 链接) ----
extern "C" void app_radio_request_catalog(void);
extern "C" void app_radio_request_city(const char *city, const char *display);
extern "C" void app_radio_enter_wifi_setup(void);
extern "C" void app_radio_show_saved_wifi(void);

static const char *kTag = "app_apps";
static bool s_settings_open;
static bool s_bluetooth_placeholder;
static void settings_open_menu(void);
bool settings_app_enter(void);
void settings_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
bool settings_app_back(void);

extern void app_framework_go_home(void);  // 由 app_registry.cc 提供(内部 go_home)
extern bool radio_player_init();

// ---- 收音机 ----
bool radio_app_enter(void) {
    s_settings_open = false;
    // radio_ui_show_* 内部自己拿 LVGL 锁; 外层再拿会双重锁死(进应用卡住的根因)
    radio_ui_show_leo_main();
    // ⚠ 顺序关键: **先建 UI 再恢复播放器内存池**。
    // 反过来的话, 池里的 MP3 解码器(需 ~19KB 连续块)会先占走内存, 随后 UI 重建
    // 只能挤到极小空间(实测 enter_app 瞬间 dma=100!), 既拖慢 UI 又让后续
    // "点继续"时解码器复用失败 -> 卡在"正在缓冲"。
    // 池本身是**懒恢复**的: stream_station 开头发现池缺失也会自行 ensure,
    // 所以这里晚一点恢复不影响播放, 但保住了 UI 的空间。
    radio_player_ensure_pool();
    radio_player_set_playing(true);
    app_radio_request_catalog();
    return true;
}
void radio_app_exit(void) {
    s_settings_open = false;
    // Keep the audio core; release full controls for lightweight home playback.
    radio_ui_leave_leo_page();
}
void radio_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_settings_open) {
        settings_app_key(btn, ev);
        return;
    }
    if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP)       radio_player_select_relative(-1);
    else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) radio_player_select_relative(1);
    else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK)   radio_player_toggle();
    else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK)    settings_app_enter();
}

// ---- Codex ----
bool codex_app_enter(void) {
    // Pause shared audio while retaining the prewarmed radio decoder.
    radio_player_set_playing(false);
    radio_player_wait_idle(1500);
    esp_err_t bridge_result = coding_bridge_init();
    app_coding_enter();   // app_coding 内部自管理锁
    if (bridge_result != ESP_OK) app_coding_bridge_error("桥接启动失败，请退出后重试");
    return app_coding_full_ready();
}
void codex_app_exit(void) {
    app_coding_exit();
}
void codex_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    app_coding_key(btn, ev);
}

// ---- 小智AI: 直连小智云端协议(78/xiaozhi-esp32 同款, 不经电脑) ----
// 交互流: 进应用→OTA取配置→WS连接hello→长按OK(listen start+opus上行)→
// 短按OK(listen stop)→云端回 stt(识别文本)+tts文本→对话卡更新。
// 云端 TTS 音频播放: v1.0 先显示文本+状态, 音频播放接bsp_audio播放链路(下一步)。
static lv_obj_t *xz_scr;
static lv_obj_t *xz_pet_body;
static lv_obj_t *xz_eye_l, *xz_eye_r;
static lv_obj_t *xz_status;
static lv_obj_t *xz_msg;
static lv_obj_t *xz_code_label;   // 设备激活码(大字)
static lv_timer_t *xz_evt_timer;
static bool xz_recording;
static bool xz_connecting;

static void xz_apply_state(int st) {
    if (!xz_pet_body || !xz_status) return;
    uint32_t c = 0x9AFCC6;
    const char *txt = "空闲 · 长按 OK 说话";
    if (st == 1) { c = 0xD8FF7A; txt = "正在聆听 · 短按OK结束"; }
    else if (st == 2) { c = 0xFFD27A; txt = "思考中…"; }
    else if (st == 3) { c = 0x6EFF9E; txt = "回答中…"; }
    else if (st == 4) { c = 0xFF9E9E; txt = "连接中…"; }
    else if (st == 5) { c = 0xFF9E9E; txt = "连接失败 · 退出重试"; }
    else if (st == 6) { c = 0xFFD27A; txt = "待激活 · 输入下方设备码"; }
    else if (st == 7) { c = 0xFFD27A; txt = "需重新绑定 · 查看下方提示"; }
    lv_obj_set_style_bg_color(xz_pet_body, lv_color_hex(c), 0);
    lv_label_set_text(xz_status, txt);
    const bool big = (st == 1);
    if (xz_eye_l && xz_eye_r) {
        lv_obj_set_size(xz_eye_l, big ? 12 : 11, big ? 17 : 14);
        lv_obj_set_size(xz_eye_r, big ? 12 : 11, big ? 17 : 14);
    }
}

static lv_obj_t *xz_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_border_width(o, 0, 0); lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

// 云端事件轮询(30ms LVGL定时器, 在LVGL任务上下文执行UI)
static void xz_evt_timer_cb(lv_timer_t *) {
    if (!xz_scr) return;
    xz_ui_event_t ev;
    while (xQueueReceive(xiaozhi_client_events(), &ev, 0) == pdTRUE) {
        if (ev == XZ_EV_WS_OPENED) {
            xz_connecting = false;
            xz_apply_state(0);
            // 激活完成/已激活: 收起设备码, 恢复对话区提示
            if (xz_code_label) lv_obj_add_flag(xz_code_label, LV_OBJ_FLAG_HIDDEN);
            if (xz_msg) lv_label_set_text(xz_msg, "长按 OK 说话");
        } else if (ev == XZ_EV_ACTIVATION) {
            // 设备未激活: 大字显示设备码, 提示去 xiaozhi.me 绑定。
            // (官方固件 rc.10 同款流程: OTA 返回 activation.code -> 用户输入)
            xz_connecting = false;
            const char *code = xiaozhi_client_activation_code();
            xz_apply_state(code[0] ? 6 : 7);
            if (xz_code_label) {
                lv_label_set_text(xz_code_label, code[0] ? code : "------");
                if (code[0]) lv_obj_remove_flag(xz_code_label, LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(xz_code_label, LV_OBJ_FLAG_HIDDEN);
            }
            if (xz_msg) {
                const char *m = xiaozhi_client_activation_message();
                // 官方文案: 访问 xiaozhi.me 输入验证码; message 为空时用默认文案
                lv_label_set_text(xz_msg,
                                  m[0] ? m
                                       : "请访问 xiaozhi.me\n输入上方设备码完成绑定");
            }
        } else if (ev == XZ_EV_STT) {
            char buf[400];
            snprintf(buf, sizeof(buf), "问: %s\n答: (思考中)", xiaozhi_client_stt());
            if (xz_msg) lv_label_set_text(xz_msg, buf);
            xz_apply_state(2);
        } else if (ev == XZ_EV_TTS_START) {
            char buf[400];
            snprintf(buf, sizeof(buf), "问: %s\n答: %s",
                     xiaozhi_client_stt(), xiaozhi_client_tts());
            if (xz_msg) lv_label_set_text(xz_msg, buf);
            xz_apply_state(3);
        } else if (ev == XZ_EV_TTS_STOP) {
            xz_apply_state(0);
        } else if (ev == XZ_EV_WS_CLOSED || ev == XZ_EV_ERROR) {
            xz_connecting = false;
            xz_recording = false;
            xz_apply_state(5);
        }
    }
}

bool xiaozhi_app_enter(void) {
    ESP_LOGI(kTag, "xiaozhi enter begin");
    xz_recording = false;
    xz_connecting = true;
    // 小智需要大块连续内存(opus 25KB + mic 栈 18KB + TLS ~20KB), 先释放收音机池
    // (此时收音机已暂停)。退出小智时 radio_player_ensure_pool() 会重新抢占。
    radio_player_set_playing(false);
    // HTTP 流任务自身的 socket 超时为 7s；1.5s 只停掉播放器状态，
    // 网络任务仍可能持有缓冲和任务栈，随后释放池会与它并发抢内存。
    const bool radio_idle = radio_player_wait_idle(10000);
    if (!radio_idle) {
        ESP_LOGW(kTag, "radio did not become idle before XiaoZhi; keep pool intact");
    } else {
        radio_player_release_pool();
    }
    // ⚠ opus 预热**不在这里做**: 它要抢 25KB 连续块, 会使后面的 OTA TLS 握手
    // (需约 20KB 连续块)失败 —— 实测 largest 从 13824 掉到不够。
    // opus 预热移到 OTA 完成之后(见 xiaozhi_async_connect_task), 二者互斥。
    if (!bsp_lvgl_lock(1000)) return false;
    xz_scr = xz_box(nullptr, 0, 0, 240, 320, 0x000F05);
    xz_box(xz_scr, 0, 0, 240, 22, 0x04180C);
    xz_box(xz_scr, 0, 21, 240, 1, 0x123D28);
    lv_obj_t *ttl = lv_label_create(xz_scr);
    lv_label_set_text(ttl, "小智AI");
    lv_obj_set_style_text_font(ttl, ui_cn_font(), 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0x9AFCC6), 0);
    lv_obj_set_pos(ttl, 42, 4);

    xz_box(xz_scr, 92, 42, 4, 10, 0x9AFCC6);
    xz_pet_body = xz_box(xz_scr, 92, 50, 56, 50, 0x9AFCC6);
    lv_obj_set_style_radius(xz_pet_body, 24, 0);
    xz_eye_l = xz_box(xz_scr, 105, 68, 11, 14, 0x000F05);
    lv_obj_set_style_radius(xz_eye_l, LV_RADIUS_CIRCLE, 0);
    xz_eye_r = xz_box(xz_scr, 124, 68, 11, 14, 0x000F05);
    lv_obj_set_style_radius(xz_eye_r, LV_RADIUS_CIRCLE, 0);

    xz_status = lv_label_create(xz_scr);
    lv_label_set_text(xz_status, "");
    lv_obj_set_style_text_font(xz_status, ui_cn_font(), 0);
    lv_obj_set_style_text_color(xz_status, lv_color_hex(0xD8FF7A), 0);
    lv_obj_set_pos(xz_status, 0, 104); lv_obj_set_width(xz_status, 240);
    lv_obj_set_style_text_align(xz_status, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *chat = xz_box(xz_scr, 10, 126, 220, 150, 0x0A251A);
    lv_obj_set_style_border_color(chat, lv_color_hex(0x2ECC7F), 0);
    lv_obj_set_style_border_width(chat, 1, 0);
    lv_obj_set_style_radius(chat, 6, 0);
    // 设备激活码(大字, 30px, 居中) —— 用户需拿它去 xiaozhi.me 绑定设备。
    // 未激活时显示 "------", 激活后隐藏。
    xz_code_label = lv_label_create(chat);
    lv_label_set_text(xz_code_label, "------");
    lv_obj_set_style_text_font(xz_code_label, &radio_font_big, 0);
    lv_obj_set_style_text_color(xz_code_label, lv_color_hex(0xFFD27A), 0);
    lv_obj_set_pos(xz_code_label, 6, 12);
    lv_obj_set_width(xz_code_label, 208);
    lv_obj_set_style_text_align(xz_code_label, LV_TEXT_ALIGN_CENTER, 0);

    xz_msg = lv_label_create(chat);
    lv_label_set_text(xz_msg, "正在连接小智服务…");
    lv_obj_set_style_text_font(xz_msg, ui_cn_font(), 0);
    lv_obj_set_style_text_color(xz_msg, lv_color_hex(0x9AFCC6), 0);
    lv_obj_set_pos(xz_msg, 10, 52);
    lv_obj_set_width(xz_msg, 200);
    lv_obj_set_height(xz_msg, 92);
    lv_label_set_long_mode(xz_msg, LV_LABEL_LONG_WRAP);

    lv_obj_t *h = lv_label_create(xz_scr);
    lv_label_set_text(h, "长按OK 说话");
    lv_obj_set_style_text_font(h, ui_cn_font(), 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x3F8F68), 0);
    lv_obj_set_pos(h, 0, 298); lv_obj_set_width(h, 240);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    xz_apply_state(4);
    lv_screen_load(xz_scr);
    xz_evt_timer = lv_timer_create(xz_evt_timer_cb, 30, nullptr);
    bsp_lvgl_unlock();

    // 后台连接(阻塞式OTA+WS, 放独立任务避免卡UI)
    xiaozhi_client_init();
    ESP_LOGI(kTag, "xiaozhi page ready, starting connect task");
    extern void xiaozhi_async_connect_task(void *);
    if (xTaskCreate((TaskFunction_t)xiaozhi_async_connect_task, "xz_conn", 6144,
                    nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE("xiaozhi", "xz_conn task create failed (heap?)");
        xz_connecting = false;
        xz_apply_state(5);
    }
    return true;
}

// 连接任务(OTA+WS+hello, 完成后经事件队列通知UI)
void xiaozhi_async_connect_task(void *) {
    // 用户在网页输入设备码后，服务端可能需要几秒才更新 OTA 状态。
    // 保持当前页面并轮询，避免必须退出再进入小智才能触发下一次请求。
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (xiaozhi_client_connect()) {
            vTaskDelete(nullptr);
            return;
        }
        if (!xiaozhi_client_activation_code()[0] &&
            !xiaozhi_client_activation_message()[0]) {
            xz_ui_event_t event = XZ_EV_ERROR;
            xQueueSend(xiaozhi_client_events(), &event, 0);
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    xz_ui_event_t event = XZ_EV_ERROR;
    xQueueSend(xiaozhi_client_events(), &event, 0);
    vTaskDelete(nullptr);
}

void xiaozhi_app_exit(void) {
    if (xz_recording) {
        xiaozhi_client_stop_listening();
        xz_recording = false;
    }
    if (bsp_lvgl_lock(500)) {
        if (xz_evt_timer) { lv_timer_delete(xz_evt_timer); xz_evt_timer = nullptr; }
        if (xz_scr) { lv_obj_delete(xz_scr); xz_scr = nullptr; }
        xz_pet_body = xz_status = xz_msg = xz_code_label = xz_eye_l = xz_eye_r = nullptr;
        bsp_lvgl_unlock();
    }
    xiaozhi_client_close();
    // 释放 opus 编码器, 把 25KB 内存还给收音机(否则收音机 MP3 无内存解不出)
    xiaozhi_opus_release();
    // 回收音机内存池(进小智时释放过), 让收音机再次可播。
    radio_player_ensure_pool();
    radio_player_set_playing(true);
}

void xiaozhi_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn != BSP_BTN_OK || xz_connecting) return;
    if (ev == BSP_BTN_LONG && !xz_recording) {
        if (!xiaozhi_client_is_open()) return;
        xz_recording = true;
        xiaozhi_client_start_listening();
        xz_apply_state(1);
    } else if (ev == BSP_BTN_CLICK && xz_recording) {
        xz_recording = false;
        xiaozhi_client_stop_listening();
        xz_apply_state(2);
    }
}

// ---- 设置 ----
bool settings_app_enter(void) {
    settings_open_menu();
    return true;
}
void settings_app_exit(void) {
    s_settings_open = false;
    // 设置页与收音机共用 radio_ui 的 screen; 退出即回框架首页
    radio_ui_leave_leo_page();
}
// ---- 设置应用: 完整子页导航 ----
// 页面模型与 radio_ui 的 RadioSettingsPage 一一对应:
//   Menu(音量/城市电台/定时播放/屏幕设置/网络设置/蓝牙/返回)
//   Volume(上下调节,OK返回) City(8行,OK选择) SleepTimer(OK开始定时)
//   Display(OK切换) WifiMenu(连接新网络/已存网络/返回)
// 长按UP = 返回上一级(框架统一回菜单, 这里处理子页间回退)。
namespace {

RadioSettingsPage s_page = RadioSettingsPage::Menu;
std::size_t s_sel = 0;
std::size_t s_saved_network_index = 0;
int s_sleep_minutes_idx = 0;          // 0=无,1=15,2=30,3=60,4=90
static constexpr int kSleepMinutes[] = {0, 15, 30, 60, 90};
// ⚠ 必须与 radio_ui.cc 的 kCityItems[] **逐项、逐序**一致(含"重庆"这个新增项),
// 否则会"选 A 城市却搜 B 城市"。历史上这里漏了"重庆"且顺序错位:
// UI 第5项显示"重庆", 本表第5项却是"长沙"; 选"成都"(第8项)时索引 8 直接越界
// (本表只有 8 项)。同时 page_rows(City) 必须等于本表项数。
static constexpr const char *kCityNames[] = {
    "", "北京", "上海", "广州", "深圳", "重庆", "长沙", "杭州", "成都",
};   // 与 radio_ui 的 kCityItems 顺序一致(空=自动定位)

std::size_t page_rows(RadioSettingsPage page) {
    switch (page) {
        case RadioSettingsPage::Menu: return 7;
        case RadioSettingsPage::City: return 9;
        case RadioSettingsPage::SleepTimer: return 6;
        case RadioSettingsPage::Display: return 3;
        case RadioSettingsPage::WifiMenu: return 3;
        case RadioSettingsPage::SavedWifi:
            return std::min<std::size_t>(SsidManager::GetInstance().GetSsidList().size(), 10) + 1;
        case RadioSettingsPage::WifiAction: return 3;
        case RadioSettingsPage::WifiDeleteConfirm: return 2;
        default: return 1;
    }
}

void show(RadioSettingsPage page, std::size_t sel) {
    s_page = page;
    s_sel = sel % page_rows(page);
    if (page == RadioSettingsPage::SavedWifi) {
        const auto &saved = SsidManager::GetInstance().GetSsidList();
        std::array<const char *, 10> names{};
        const std::size_t count = std::min(saved.size(), names.size());
        for (std::size_t i = 0; i < count; ++i) names[i] = saved[i].ssid.c_str();
        radio_ui_show_saved_wifi(names.data(), count, s_sel);
        return;
    }
    if (page == RadioSettingsPage::WifiAction ||
        page == RadioSettingsPage::WifiDeleteConfirm) {
        const auto &saved = SsidManager::GetInstance().GetSsidList();
        if (s_saved_network_index >= saved.size()) {
            show(RadioSettingsPage::SavedWifi, 0);
            return;
        }
        const char *ssid = saved[s_saved_network_index].ssid.c_str();
        if (page == RadioSettingsPage::WifiAction) radio_ui_show_wifi_action(ssid, s_sel);
        else radio_ui_show_wifi_delete_confirm(ssid, s_sel);
        return;
    }
    radio_ui_show_settings(page, sel, radio_player_volume());
}

void refresh_current() { show(s_page, s_sel); }

void start_sleep_timer() {
    radio_sleep_timer_set_minutes(static_cast<uint16_t>(kSleepMinutes[s_sleep_minutes_idx]));
}

}  // namespace

static void settings_open_menu(void) {
    s_settings_open = true;
    s_bluetooth_placeholder = false;
    show(RadioSettingsPage::Menu, 0);
}

bool settings_app_back(void) {
    if (!s_settings_open) return false;
    if (s_bluetooth_placeholder) {
        s_bluetooth_placeholder = false;
        show(RadioSettingsPage::Menu, 5);
        return true;
    }
    switch (s_page) {
        case RadioSettingsPage::SavedWifi: show(RadioSettingsPage::WifiMenu, 1); break;
        case RadioSettingsPage::WifiAction: show(RadioSettingsPage::SavedWifi, s_saved_network_index); break;
        case RadioSettingsPage::WifiDeleteConfirm: show(RadioSettingsPage::WifiAction, 1); break;
        case RadioSettingsPage::Volume: show(RadioSettingsPage::Menu, 0); break;
        case RadioSettingsPage::City: show(RadioSettingsPage::Menu, 1); break;
        case RadioSettingsPage::SleepTimer: show(RadioSettingsPage::Menu, 2); break;
        case RadioSettingsPage::Display: show(RadioSettingsPage::Menu, 3); break;
        case RadioSettingsPage::WifiMenu: show(RadioSettingsPage::Menu, 4); break;
        default:
            if (app_framework_active() != APP_ID_RADIO) return false;
            s_settings_open = false;
            radio_ui_show_leo_main();
            break;
    }
    return true;
}

void settings_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_bluetooth_placeholder) {
        if ((btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) ||
            (btn == BSP_BTN_UP && ev == BSP_BTN_LONG)) settings_app_back();
        return;
    }
    const std::size_t rows = page_rows(s_page);
    if (ev == BSP_BTN_LONG &&
        (btn == BSP_BTN_UP || (btn == BSP_BTN_OK &&
         (s_page == RadioSettingsPage::SavedWifi ||
          s_page == RadioSettingsPage::WifiAction ||
          s_page == RadioSettingsPage::WifiDeleteConfirm)))) {
        settings_app_back();
        return;
    }

    // ---- 音量页: 上下调节, OK 返回菜单 ----
    if (s_page == RadioSettingsPage::Volume) {
        if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            int v = radio_player_volume();
            const int delta = (ev == BSP_BTN_LONG) ? 10 : 5;
            v += (btn == BSP_BTN_UP) ? delta : -delta;
            radio_player_set_volume(static_cast<uint8_t>(std::clamp(v, 10, 100)));
            refresh_current();
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            show(RadioSettingsPage::Menu, 0);
        }
        return;
    }

    // ---- 通用列表页 ----
    if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
        s_sel = (s_sel + rows - 1) % rows;
        refresh_current();
        return;
    }
    if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
        s_sel = (s_sel + 1) % rows;
        refresh_current();
        return;
    }
    // 长按UP: 子页回菜单
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        show(RadioSettingsPage::Menu, 0);
        return;
    }
    if (ev != BSP_BTN_CLICK || btn != BSP_BTN_OK) return;

    switch (s_page) {
        case RadioSettingsPage::Menu:
            switch (s_sel) {
                case 0: show(RadioSettingsPage::Volume, 0); break;
                case 1: show(RadioSettingsPage::City, 0); break;
                case 2: show(RadioSettingsPage::SleepTimer, 0); break;
                case 3: show(RadioSettingsPage::Display, 0); break;
                case 4: show(RadioSettingsPage::WifiMenu, 0); break;
                case 5:
                    s_bluetooth_placeholder = true;
                    radio_ui_show_easter_egg("暂无蓝牙功能\n长按上键返回");
                    break;
                case 6:
                    if (!settings_app_back()) app_framework_go_menu();
                    break;
            }
            break;
        case RadioSettingsPage::City: {
            // 选城市: 空=自动定位, 其他按名字搜台
            const char *city = kCityNames[s_sel];
            if (s_sel == 0 || city[0] == '\0') {
                app_radio_request_city("", "");
            } else {
                app_radio_request_city(city, city);
            }
            // 选择完成回菜单
            show(RadioSettingsPage::Menu, 1);
            break;
        }
        case RadioSettingsPage::SleepTimer:
            if (s_sel < 5) {
                s_sleep_minutes_idx = static_cast<int>(s_sel);
                start_sleep_timer();
            }
            show(RadioSettingsPage::Menu, 2);
            break;
        case RadioSettingsPage::Display:
            if (s_sel == 0) {
                // 亮度四档循环，不重建页面。
                const std::size_t next = radio_display_brightness_selection() + 1;
                radio_display_set_brightness_selection(next);
                radio_ui_refresh_settings_row_texts();
            } else if (s_sel == 1) {
                // 息屏五档循环: 0/30s/60s/300s/600s。
                radio_display_set_timeout_selection(radio_display_timeout_selection() + 1);
                radio_ui_refresh_settings_row_texts();
            } else {
                show(RadioSettingsPage::Menu, 3);
                return;
            }
            break;
        case RadioSettingsPage::WifiMenu:
            if (s_sel == 0) {
                // 连接新网络: 进配网门户(保留已存网络, 可取消)
                extern void app_radio_enter_wifi_setup(void);
                app_radio_enter_wifi_setup();
            } else if (s_sel == 1) {
                show(RadioSettingsPage::SavedWifi, 0);
            } else {
                show(RadioSettingsPage::Menu, 4);
            }
            break;
        case RadioSettingsPage::SavedWifi:
            if (s_sel + 1 == rows) show(RadioSettingsPage::WifiMenu, 1);
            else {
                s_saved_network_index = s_sel;
                show(RadioSettingsPage::WifiAction, 0);
            }
            break;
        case RadioSettingsPage::WifiAction:
            if (s_sel == 0) {
                SsidManager::GetInstance().SetDefaultSsid(static_cast<int>(s_saved_network_index));
                show(RadioSettingsPage::SavedWifi, 0);
            } else if (s_sel == 1) show(RadioSettingsPage::WifiDeleteConfirm, 0);
            else show(RadioSettingsPage::SavedWifi, s_saved_network_index);
            break;
        case RadioSettingsPage::WifiDeleteConfirm:
            if (s_sel == 1) SsidManager::GetInstance().RemoveSsid(static_cast<int>(s_saved_network_index));
            show(RadioSettingsPage::SavedWifi, s_saved_network_index);
            break;
        default:
            show(RadioSettingsPage::Menu, 0);
            break;
    }
}
