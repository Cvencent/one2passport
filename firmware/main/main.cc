// ---- Codex 监控页(移植自 FoloOS「口袋百宝箱」)----
// app.h 声明 Codex 页的对外接口,并借 app_voice_mode_t 约定语音/提示音档位。
// 头文件本身带 extern "C",因为 app_coding.c / coding_bridge.c 是 C 文件。
// ui_sfx 提供按键音与"待确认/已完成"两条语音提示(内嵌 WAV)。
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "app.h"
#include "xiaozhi_client.h"
#include "coding_bridge.h"
#include "ui_sfx.h"
#include "wireless_bridge.h"

#include "radio_catalog.h"
#include "radio_display_settings.h"
#include "radio_easter_egg.h"
#include "fap_screenshot.h"
#include "radio_player.h"
#include "radio_sleep_timer.h"
#include "radio_ui.h"
#include "app_registry.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"   // catalog 搜索前报告可用连续堆

extern "C" void xz_heap_report(const char *tag);  // xiaozhi_client.c 临时heap诊断
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

// 城市电台列表: 第 0 项是自动定位, 其余直接来自 radio_catalog 的城市表
// (同一张表也提供查询用的省名与旧式 state 拼写)。历史上这里和 UI 的
// kCityItems[]、app_apps 的 kCityNames[] 各存一份, 顺序一旦不同就会
// "选 A 城市却搜 B 城市", 甚至索引越界。现在三处共用一个来源。
std::size_t city_choice_count() {
    return 1 + radio_catalog_city_count();
}

const char *city_query_for(std::size_t index) {
    if (index == 0 || index >= city_choice_count()) return nullptr;
    return radio_catalog_city_name(index - 1);
}

const char *city_display_for(std::size_t index) {
    return index == 0 ? "自动定位" : city_query_for(index);
}

namespace {

constexpr char kTag[] = "internet_radio";
struct KeyMessage {
    bsp_btn_t button;
    bsp_btn_ev_t event;
    // 自动化测试用: >=0 表示直接跳到该应用, 忽略 button/event。
    // 生产路径所有按键都显式写 -1, 所以正常按键行为完全不变。
    int8_t app_jump = -1;
};

enum class ConfigInputMode {
    NetworkList,
    Keyboard,
    Connecting,
};

enum class SettingsInputMode {
    Menu,
    Volume,
    City,
    SleepTimer,
    Display,
    WifiMenu,
    SavedWifi,
    WifiAction,
    WifiDeleteConfirm,
};

constexpr uint16_t kSleepTimerMinutes[] = {0, 15, 30, 60, 90};

constexpr std::size_t kMaxVisibleNetworks = 12;

QueueHandle_t s_key_queue;
std::atomic<bool> s_config_mode{false};
std::atomic<bool> s_connected{false};
std::atomic<bool> s_settings_mode{false};
// Codex 监控页是否处于前台。主界面长按 UP 置真,Codex 页内长按 UP 置假。
std::atomic<bool> s_codex_mode{false};
std::atomic<bool> s_sntp_started{false};
std::atomic<bool> s_config_can_cancel{true};   // V6: 配网页永远可长按OK退出
std::atomic<bool> s_portal_auto_opened{false};   // 看门狗每次开机只自动开一次配网
TaskHandle_t s_catalog_task;
TaskHandle_t s_wifi_scan_task;
TaskHandle_t s_wifi_connect_task;
std::mutex s_wifi_ui_mutex;
ConfigInputMode s_config_input_mode = ConfigInputMode::NetworkList;
std::array<WifiNetworkView, kMaxVisibleNetworks> s_wifi_networks = {};
std::size_t s_wifi_network_count;
std::size_t s_wifi_network_selected;
std::string s_wifi_selected_ssid;
std::string s_wifi_password;
WifiKeyboardPage s_wifi_keyboard_page = WifiKeyboardPage::Lower;
std::size_t s_wifi_key_selected;
std::string s_config_ap_ssid;
SettingsInputMode s_settings_input_mode = SettingsInputMode::Menu;
std::size_t s_settings_selected;
std::size_t s_saved_wifi_index;
std::mutex s_catalog_mutex;
bool s_catalog_loaded;
uint32_t s_catalog_generation;
char s_requested_city[16];
char s_requested_city_display[16];

void start_local_catalog_search();
void start_city_catalog_search(const char *city, const char *display);
void start_remembered_catalog_search();

// =====================================================================
// Codex 页 Host 钩子
//
// FoloOS 原版把电源管理、语音档位、设置读取都实现在它的 main.c 里。
// 本工程 main 是收音机,这些职责分别对应到 radio_display_settings 与
// WifiManager,故在此实现同一组接口,让移植过来的 C 代码链接得上。
// 全部是 extern "C":调用方是 C 文件。
// =====================================================================

// "有交互/有事件发生" → 重置息屏计时。Codex 收到电脑端推送时也会调用它,
// 否则跑了十几分钟的长任务会让屏幕自己灭掉。
extern "C" void app_power_activity(void) {
    radio_display_note_activity();
}

// 要求保持唤醒(Codex 录音/播报期间)。转达给息屏任务,让它跳过计时。
extern "C" void app_power_hold_awake(bool keep_awake) {
    radio_display_set_keep_awake(keep_awake);
}

// LEO 收音机没有"语音模式"设置项。Codex 通知一律走语音合成(APP_VOICE_SPEECH),
// 这样"待确认/已完成"会用内嵌 WAV 报出来,而不是只有滴的一声。
extern "C" app_voice_mode_t app_settings_voice_mode(void) {
    return APP_VOICE_SPEECH;
}

// 提示音音量跟随收音机音量,用户在设置页调完音量对 Codex 页同样生效。
extern "C" uint8_t app_settings_sfx_volume(void) {
    const uint8_t volume = radio_player_volume();
    return volume > 0 ? volume : 5;
}

// 电脑端通过桥接请求换网时,转发给 WifiManager(它独占 WiFi 栈)。
// 返回 ESP_OK 只代表"已受理",实际连接结果经 WifiEvent 异步回传。
extern "C" esp_err_t codex_wifi_configure(const char *ssid, const char *password) {
    if (ssid == nullptr || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    // 换网期间退出 Codex 页,让用户能看见配网/连接进度界面。
    // 注意:本回调由桥接任务在【不持 LVGL 锁】的上下文中调用
    // (见 wireless_bridge.c 的 process_client_bytes 链路),故此处自行加锁。
    if (s_codex_mode.load(std::memory_order_acquire)) {
        if (bsp_lvgl_lock(500)) {
            app_coding_exit();
            bsp_lvgl_unlock();
        }
        s_codex_mode.store(false, std::memory_order_release);
        radio_ui_show_main();
    }
    const bool ok = WifiManager::GetInstance().ConfigureNetwork(
        ssid, password != nullptr ? password : "");
    ESP_LOGI(kTag, "bridge requested wifi change to %s: %s", ssid,
             ok ? "accepted" : "rejected");
    return ok ? ESP_OK : ESP_FAIL;
}

// Xiaozhi's C client asks the host for the station state before it needs to
// resolve the XiaoZhi service.  These are thin adapters over WifiManager so
// the C translation units do not have to depend on the C++ singleton.
extern "C" int wifi_manager_is_connected(void) {
    return WifiManager::GetInstance().IsConnected() ? 1 : 0;
}

extern "C" const char *wifi_manager_get_ip_address(void) {
    static std::string address;
    address = WifiManager::GetInstance().GetIpAddress();
    return address.c_str();
}

extern "C" esp_netif_t *wifi_manager_get_station_netif(void) {
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

// Persists the city the listener picked in Settings so a restart comes back to
// the same place instead of relocating from scratch every time.
//
// 存的是**城市名**而不是表索引: 表会随版本增删(旧表 9 项, 现在 50 项), 索引一
// 移位就会"上次选广州、这次自动选天津"。旧固件只写过 "city" 这个 u8, 所以这里
// 保留一张旧表做一次性迁移。
constexpr const char *kLegacyCityOrder[] = {
    "", "北京", "上海", "广州", "深圳", "重庆", "长沙", "杭州", "成都",
};

void save_selected_city(std::size_t choice_index) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    const char *name = city_query_for(choice_index);
    if (name) {
        nvs_set_str(handle, "city_name", name);
    } else {
        nvs_set_str(handle, "city_name", "");
    }
    nvs_set_u8(handle, "city", static_cast<uint8_t>(choice_index));
    nvs_commit(handle);
    nvs_close(handle);
}

std::size_t find_city_index(const char *name) {
    if (!name || !name[0]) return 0;
    const std::size_t count = city_choice_count();
    for (std::size_t i = 1; i < count; ++i) {
        const char *query = city_query_for(i);
        if (query && std::strcmp(query, name) == 0) return i;
    }
    return 0;
}

std::size_t load_selected_city() {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return 0;
    char name[24] = {};
    std::size_t length = sizeof(name);
    std::size_t index = 0;
    if (nvs_get_str(handle, "city_name", name, &length) == ESP_OK) {
        index = find_city_index(name);
    } else {
        uint8_t legacy = 0;
        if (nvs_get_u8(handle, "city", &legacy) == ESP_OK &&
            legacy < sizeof(kLegacyCityOrder) / sizeof(kLegacyCityOrder[0])) {
            index = find_city_index(kLegacyCityOrder[legacy]);
        }
    }
    nvs_close(handle);
    return index;
}

// The station cache must remember which city it was searched for. NVS is small
// and a failed catalog_v3 write (ESP_ERR_NVS_NOT_ENOUGH_SPACE) leaves the old
// blob behind; comparing the stamped city at boot turns that into a forced
// re-search instead of silently showing the previous city's stations.
constexpr char kCatalogCityKey[] = "catalog_city";

void save_catalog_city(const char *city) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_str(handle, kCatalogCityKey, city ? city : "");
    nvs_commit(handle);
    nvs_close(handle);
}

bool load_catalog_city(char *out, std::size_t size) {
    if (out == nullptr || size == 0) return false;
    out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t length = size;
    const esp_err_t error = nvs_get_str(handle, kCatalogCityKey, out, &length);
    nvs_close(handle);
    return error == ESP_OK;
}

// Starts the search for whichever city was last chosen, falling back to
// automatic detection the first time the radio is used.
void start_remembered_catalog_search() {
    const char *query = city_query_for(load_selected_city());
    if (query) {
        start_city_catalog_search(query, query);
    } else {
        start_local_catalog_search();
    }
}

// 组件回调桥: iot_button 官方驱动(与原厂固件同路径) -> 现有按键队列
static void button_event_bridge(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    KeyMessage msg{};
    switch (ev) {
        case BSP_BTN_CLICK:
            msg = {btn, BSP_BTN_CLICK, -1};
            xQueueSend(s_key_queue, &msg, 0);
            break;
        case BSP_BTN_LONG:
            msg = {btn, BSP_BTN_LONG, -1};
            xQueueSend(s_key_queue, &msg, 0);
            break;
        case BSP_BTN_RELEASE:
            // 2026-10-12: forward the long-press release. XiaoZhi needs this to end
            // a hold-to-talk recording; without it recording never stops.
            msg = {btn, BSP_BTN_RELEASE, -1};
            xQueueSend(s_key_queue, &msg, 0);
            break;
        default:  // PRESS/DOUBLE 走现有逻辑不需要
            break;
    }
}

// 自动化测试: 请求跳到指定应用。转发到 UI 队列由 LVGL 定时器执行。
static void serial_app_jump_request(int app_id);
// 串口按键注入(自动化测试): fap_shot 任务回调, 只入队
static void serial_key_inject(char c) {
    KeyMessage msg{};
    switch (c) {
        case 'u': msg = {BSP_BTN_UP,   BSP_BTN_CLICK, -1}; break;
        case 'd': msg = {BSP_BTN_DOWN, BSP_BTN_CLICK, -1}; break;
        case 'o': msg = {BSP_BTN_OK,   BSP_BTN_CLICK, -1}; break;
        case 'j': msg = {BSP_BTN_DOWN, BSP_BTN_LONG, -1};  break;
        case 'l': msg = {BSP_BTN_OK,   BSP_BTN_LONG, -1};  break;
        case 'r': msg = {BSP_BTN_OK,   BSP_BTN_RELEASE, -1}; break;  // long-press release (hold-to-talk stop)
        case 'k': msg = {BSP_BTN_UP,   BSP_BTN_LONG, -1};  break;
        case '1': case '2': case '3': case '4':
            serial_app_jump_request(c - '1');
            return;
        default: return;
    }
    if (s_key_queue) xQueueSend(s_key_queue, &msg, 0);
}

int button_from_mv(int millivolts) {
    if (millivolts < 0 || millivolts >= 1900) return -1;
    if (millivolts < 150) return BSP_BTN_UP;
    if (millivolts < 447) return BSP_BTN_DOWN;
    return BSP_BTN_OK;
}

void button_task(void *) {
    int candidate = -1;
    int stable = -1;
    uint8_t samples = 0;
    TickType_t pressed_at = 0;
    bool long_sent = false;
    int diag_count = 0;
    int last_diag_mv = -999;

    while (true) {
        const int mv_raw = bsp_button_read_mv();
        // 临时诊断: 电压变化>80mV 或每25次(500ms)打印一次,验证ADC链路
        if (++diag_count % 25 == 0 || (last_diag_mv>-900 && (mv_raw-last_diag_mv>80 || last_diag_mv-mv_raw>80))) {
            const int b = button_from_mv(mv_raw);
            ESP_LOGI("btn_diag", "ADC=%dmV -> %s", mv_raw,
                     b<0?"none":(b==0?"UP":(b==1?"DOWN":"OK")));
        }
        last_diag_mv = mv_raw;
        const int current = button_from_mv(mv_raw);
        if (current == candidate) {
            if (samples < 3) ++samples;
        } else {
            candidate = current;
            samples = 1;
        }

        if (samples >= 3 && candidate != stable) {
            const int previous = stable;
            stable = candidate;
            if (stable >= 0) {
                pressed_at = xTaskGetTickCount();
                long_sent = false;
            } else if (previous >= 0 && !long_sent) {
                const KeyMessage message = {static_cast<bsp_btn_t>(previous), BSP_BTN_CLICK, -1};
                xQueueSend(s_key_queue, &message, 0);
            }
        }

        if (stable >= 0 && !long_sent &&
            xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(850)) {
            const KeyMessage message = {static_cast<bsp_btn_t>(stable), BSP_BTN_LONG, -1};
            xQueueSend(s_key_queue, &message, 0);
            long_sent = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// SoftAP 的 beacon 缓冲(hostap attach)每次要 752 字节, 且分配失败时驱动不会返回
// 错误 —— 它直接解引用空指针, 必崩(实测 "wifi:alloc eb len=752 type=4 fail" 之后
// 紧跟 Load access fault)。所以必须在开 AP 之前自己确认 DMA 够用,
// 不够就延后重试, 绝不能把设备送进重启循环。
//
// 阈值 6KB: 一次 hostap attach 连续要 2 块 752B, 再加 beacon/STA 缓冲与后续
// 扫描/DNS/HTTPD 的余量。低于 6KB 时开 AP 属于赌博, 宁可晚点开。
static constexpr unsigned kSetupApDmaBudgetBytes = 6144;

// 开机时没有可用凭据 -> 需要进配网。但**绝不能直接在 app_main 里同步开**:
//   此刻 I2S DMA + USB 桥接把 dma 压到 ~1.7KB, 而 hostap 的 beacon 缓冲
//   (752B x 2) 分配失败后驱动不返回错误、直接解引用空指针:
//     wifi:alloc eb len=752 type=4 fail
//     Guru Meditation Error: Core 0 panic'ed (Load access fault)
//   设备会陷入"一直响、反复刷首页"的重启循环。
// 开机只置 need_setup_ap, 真正的开 AP 交给 30s 守护任务走 enter_wifi_config(),
// 它会先停播放并释放解码器池, 再在专用任务上开 AP。
//
// StartConfigAp() 本身又是一次很重的调用: 起 DNS server + httpd(24 个 URI
// handler) + cJSON + 扫描定时器, 实测能在 2KB 栈上直接吃穿
// (2026-10-12: watchdog 任务 radio_wifi_guard 栈 2048, 同步调它 ->
//  "Stack protection fault", 栈区间只有 2032 字节)。
// 所以它绝不能在 watchdog / LVGL 这类小栈任务上同步执行 —— 统一挪到
// 这个专用任务上, 12KB 静态栈, 与调用者无关。
static void start_config_ap_on_dedicated_task();

static void config_ap_start_task(void *arg)
{
    auto *manager = static_cast<WifiManager *>(arg);
    manager->StartConfigAp();
    vTaskDelete(nullptr);
}

static void start_config_ap_on_dedicated_task()
{
    auto &manager = WifiManager::GetInstance();
    // 栈从堆上分配, **不占静态内存**。这一点很关键:
    //   开机常驻的每一块静态内存都在跟收音机解码器抢堆。实测给本任务
    //   预留 12KB 静态栈后, post_coding_bridge 的可用堆从 8760 掉到 6148,
    //   进收音机时 MP3 解码器(需 ~20KB 连续块)彻底开不出来 ->
    //   free_all 掉到 156 字节 -> task_wdt 每 5s 触发 -> 卡死发烫。
    //   配网页不用收音机, 进配网前刚释放过解码器池, 那时堆上借 12KB 很安全。
    if (xTaskCreate(config_ap_start_task, "cfg_ap_start", 3072,
                    &manager, 5, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "config AP start task failed; retrying inline");
        manager.StartConfigAp();
    }
}
void enter_wifi_config(bool clear_saved_networks, bool allow_cancel = false) {
    if (s_config_mode.load()) return;
    ESP_LOGI(kTag, "Entering Wi-Fi configuration mode");
    radio_player_set_network(false);
    if (!radio_player_wait_idle(3000)) {
        ESP_LOGW(kTag, "Player still stopping; retry Wi-Fi setup");
        radio_player_set_network(s_connected.load());
        return;
    }
    // 配网门户和扫描任务需要连续栈空间；暂停后释放解码器池。
    // 退出配网后播放路径会按需恢复池。
    radio_player_release_pool();
    if (clear_saved_networks) SsidManager::GetInstance().Clear();
    (void)allow_cancel;   // V6: 配网页无条件可退出(长按OK), 否则无网设备会被锁死在配网页
    s_config_can_cancel.store(true);
    // 同 deferred_setup_ap_task: hostap 的 beacon 缓冲分配失败后驱动不会返回错误,
    // 而是直接解引用空指针崩溃, 所以开 AP 前先确认 DMA 余量, 不够就记日志。
    ESP_LOGW(kTag, "config AP: dma=%u (budget %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), kSetupApDmaBudgetBytes);
    start_config_ap_on_dedicated_task();
}

void show_saved_wifi() {
    const auto &saved = SsidManager::GetInstance().GetSsidList();
    std::array<const char *, 10> names = {};
    const std::size_t count = std::min(saved.size(), names.size());
    for (std::size_t i = 0; i < count; ++i) names[i] = saved[i].ssid.c_str();
    s_settings_selected = std::min(s_settings_selected, count);
    radio_ui_show_saved_wifi(names.data(), count, s_settings_selected);
}

std::string saved_wifi_ssid(std::size_t index) {
    const auto &saved = SsidManager::GetInstance().GetSsidList();
    return index < saved.size() ? saved[index].ssid : std::string();
}

void show_wifi_action() {
    const std::string ssid = saved_wifi_ssid(s_saved_wifi_index);
    if (ssid.empty()) {
        s_settings_input_mode = SettingsInputMode::SavedWifi;
        s_settings_selected = 0;
        show_saved_wifi();
        return;
    }
    radio_ui_show_wifi_action(ssid.c_str(), s_settings_selected);
}

void show_wifi_delete_confirm() {
    const std::string ssid = saved_wifi_ssid(s_saved_wifi_index);
    if (ssid.empty()) {
        s_settings_input_mode = SettingsInputMode::SavedWifi;
        s_settings_selected = 0;
        show_saved_wifi();
        return;
    }
    radio_ui_show_wifi_delete_confirm(ssid.c_str(), s_settings_selected);
}

void show_wifi_list(bool scanning) {
    std::array<WifiNetworkView, kMaxVisibleNetworks> networks = {};
    std::size_t count = 0;
    std::size_t selected = 0;
    std::string setup_ssid;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        networks = s_wifi_networks;
        count = s_wifi_network_count;
        selected = count ? std::min(s_wifi_network_selected, count - 1) : 0;
        setup_ssid = s_config_ap_ssid;
    }
    radio_ui_show_wifi_list(networks.data(), count, selected, scanning,
                            setup_ssid.c_str(), s_config_can_cancel.load());
}

void show_wifi_keyboard(const char *status = nullptr) {
    std::string ssid;
    std::size_t password_length = 0;
    WifiKeyboardPage page;
    std::size_t selected = 0;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        ssid = s_wifi_selected_ssid;
        password_length = s_wifi_password.size();
        page = s_wifi_keyboard_page;
        selected = s_wifi_key_selected;
    }
    radio_ui_show_wifi_keyboard(ssid.c_str(), password_length, page, selected, status);
}

void wifi_scan_task(void *) {
    for (int attempt = 0; attempt < 16 && s_config_mode.load(); ++attempt) {
        auto access_points = WifiManager::GetInstance().GetConfigAccessPoints();
        if (!access_points.empty()) {
            std::sort(access_points.begin(), access_points.end(),
                      [](const wifi_ap_record_t &left, const wifi_ap_record_t &right) {
                          return left.rssi > right.rssi;
                      });
            std::array<WifiNetworkView, kMaxVisibleNetworks> networks = {};
            std::size_t count = 0;
            for (const auto &access_point : access_points) {
                const char *ssid = reinterpret_cast<const char *>(access_point.ssid);
                if (!ssid[0]) continue;
                bool duplicate = false;
                for (std::size_t i = 0; i < count; ++i) {
                    if (std::strncmp(networks[i].ssid, ssid,
                                     sizeof(networks[i].ssid)) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate || count >= networks.size()) continue;
                std::snprintf(networks[count].ssid, sizeof(networks[count].ssid),
                              "%.*s", 32, ssid);
                networks[count].rssi = access_point.rssi;
                networks[count].secure = access_point.authmode != WIFI_AUTH_OPEN;
                ++count;
            }
            if (count) {
                {
                    std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                    s_wifi_networks = networks;
                    s_wifi_network_count = count;
                    s_wifi_network_selected = 0;
                }
                show_wifi_list(false);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(750));
    }
    bool show_empty_list = false;
    if (s_config_mode.load()) {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        show_empty_list = s_wifi_network_count == 0;
    }
    if (show_empty_list && s_config_mode.load()) show_wifi_list(false);
    s_wifi_scan_task = nullptr;
    vTaskDelete(nullptr);
}

void start_wifi_scan_ui() {
    if (s_wifi_scan_task) return;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        s_wifi_network_count = 0;
        s_wifi_network_selected = 0;
    }
    show_wifi_list(true);
    if (xTaskCreate(wifi_scan_task, "wifi_screen_scan", 4096, nullptr, 4,
                    &s_wifi_scan_task) != pdPASS) {
        s_wifi_scan_task = nullptr;
        show_wifi_list(false);
    }
}

void wifi_connect_task(void *) {
    std::string ssid;
    std::string password;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        ssid = s_wifi_selected_ssid;
        password = s_wifi_password;
    }
    const bool connected = WifiManager::GetInstance().ConfigureNetwork(ssid, password);
    if (!connected && s_config_mode.load()) {
        {
            std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
            s_config_input_mode = ConfigInputMode::Keyboard;
        }
        show_wifi_keyboard("连接失败");
    }
    s_wifi_connect_task = nullptr;
    vTaskDelete(nullptr);
}

void start_wifi_connection() {
    if (s_wifi_connect_task) return;
    std::string ssid;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        if (s_wifi_selected_ssid.empty()) return;
        s_config_input_mode = ConfigInputMode::Connecting;
        ssid = s_wifi_selected_ssid;
    }
    radio_ui_show_wifi_connecting(ssid.c_str(), "正在连接");
    if (xTaskCreate(wifi_connect_task, "wifi_screen_join", 5120, nullptr, 5,
                    &s_wifi_connect_task) != pdPASS) {
        s_wifi_connect_task = nullptr;
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        s_config_input_mode = ConfigInputMode::Keyboard;
    }
}

void handle_config_input(const KeyMessage &message) {
    ConfigInputMode mode;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        mode = s_config_input_mode;
    }
    if (mode == ConfigInputMode::Connecting) return;

    if (mode == ConfigInputMode::NetworkList) {
        if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
            if (s_config_can_cancel.load(std::memory_order_acquire)) {
                WifiManager::GetInstance().StopConfigAp();
            } else {
                start_wifi_scan_ui();
            }
            return;
        }
        bool redraw = false;
        bool open_keyboard = false;
        {
            std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
            if (s_wifi_network_count == 0) return;
            if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
                s_wifi_network_selected =
                    (s_wifi_network_selected + s_wifi_network_count - 1) %
                    s_wifi_network_count;
                redraw = true;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
                s_wifi_network_selected =
                    (s_wifi_network_selected + 1) % s_wifi_network_count;
                redraw = true;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
                s_wifi_selected_ssid = s_wifi_networks[s_wifi_network_selected].ssid;
                s_wifi_password.clear();
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
                s_config_input_mode = ConfigInputMode::Keyboard;
                open_keyboard = true;
            }
        }
        if (redraw) show_wifi_list(false);
        if (open_keyboard) show_wifi_keyboard();
        return;
    }

    if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
        start_wifi_connection();
        return;
    }

    bool redraw = false;
    bool connect = false;
    bool back_to_list = false;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        const std::size_t count = radio_ui_wifi_key_count(s_wifi_keyboard_page);
        if (!count) return;
        if (message.button == BSP_BTN_UP && message.event == BSP_BTN_CLICK) {
            s_wifi_key_selected = (s_wifi_key_selected + count - 1) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_DOWN && message.event == BSP_BTN_CLICK) {
            s_wifi_key_selected = (s_wifi_key_selected + 1) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_UP && message.event == BSP_BTN_LONG) {
            s_wifi_key_selected = (s_wifi_key_selected + count - std::min<std::size_t>(6, count)) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_DOWN && message.event == BSP_BTN_LONG) {
            s_wifi_key_selected = (s_wifi_key_selected + std::min<std::size_t>(6, count)) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_OK && message.event == BSP_BTN_CLICK) {
            const char *key = radio_ui_wifi_key(s_wifi_keyboard_page, s_wifi_key_selected);
            if (std::strcmp(key, "ABC") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Upper;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "abc") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "123") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Symbols;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "<") == 0) {
                if (!s_wifi_password.empty()) s_wifi_password.pop_back();
                else {
                    s_config_input_mode = ConfigInputMode::NetworkList;
                    back_to_list = true;
                }
            } else if (std::strcmp(key, "GO") == 0) {
                connect = true;
            } else if (s_wifi_password.size() < 63) {
                s_wifi_password += key;
            }
            redraw = true;
        }
    }
    if (back_to_list) show_wifi_list(false);
    else if (connect) start_wifi_connection();
    else if (redraw) show_wifi_keyboard();
}

// ---- 按键 UI 操作投递机制(根治跨任务 LVGL 竞态) ----
// input_task 只做: 亮屏/彩蛋/入二级队列。
// 全部 UI 操作(handle_config_input / app_framework_dispatch)由
// ui_flush_timer 在 LVGL 任务上下文执行 —— 按键路径与渲染任务同线程,
// 从根上消除 lv_inv_area 竞态挂死。
static QueueHandle_t s_ui_queue;   // 二级队列: input_task -> LVGL任务

// 实现: 投到 UI 队列, 由 ui_flush_timer 在 LVGL 任务上下文执行。
// 不能在串口注入回调里直接 enter_app: 那是 fap 截图任务, 会与 LVGL 抢锁死锁。
static void serial_app_jump_request(int app_id) {
    if (!s_ui_queue) return;
    KeyMessage jump = {BSP_BTN_OK, BSP_BTN_CLICK, -1};
    jump.app_jump = (int8_t)app_id;
    xQueueSend(s_ui_queue, &jump, 0);
}

static void ui_flush_timer_cb(lv_timer_t *) {
    KeyMessage message;
    // 一次最多处理 4 条, 避免长按连发时饿死渲染
    for (int i = 0; i < 4; ++i) {
        if (xQueueReceive(s_ui_queue, &message, 0) != pdTRUE) break;
        if (message.app_jump >= 0) {
            app_framework_launch_app(message.app_jump);
            continue;
        }
        if (s_config_mode.load(std::memory_order_acquire)) {
            handle_config_input(message);
        } else {
            app_framework_dispatch(message.button, message.event);
        }
    }
}

void input_task(void *) {
    KeyMessage message;
    while (true) {
        if (xQueueReceive(s_key_queue, &message, portMAX_DELAY) != pdTRUE) continue;
        ESP_LOGI("key_evt", "KEY btn=%d ev=%d", (int)message.button, (int)message.event);
        // 息屏时: 任意键只负责点亮屏幕, **不**执行该键的功能。
        // 这样黑屏状态下误碰电源键不会切台/进出应用; 想操作就先点亮再按一次。
        // (必须在 note_activity() 之前判断: 它会把 s_screen_on 置回 true。)
        if (radio_display_was_blanked()) {
            radio_display_note_activity();          // 点亮 + 刷新计时
            radio_display_grace_after_wake(3000); // 别刚点亮就二次息屏
            ESP_LOGI(kTag, "key consumed to wake screen");
            continue;
        }
        radio_display_note_activity();
        if (radio_easter_egg_is_playing()) {
            radio_easter_egg_stop();
            continue;
        }
        // UI 操作投递到 LVGL 任务(由 ui_flush_timer 在锁内安全执行)
        xQueueSend(s_ui_queue, &message, 0);
    }
}

void start_sntp_once() {
    bool expected = false;
    if (!s_sntp_started.compare_exchange_strong(expected, true)) return;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.start = true;
    config.server_from_dhcp = false;
    config.renew_servers_after_new_IP = false;
    const esp_err_t error = esp_netif_sntp_init(&config);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "SNTP init failed: %s", esp_err_to_name(error));
        s_sntp_started.store(false);
    }
}
void catalog_task(void *) {
    constexpr std::size_t kDiscoveryCapacity = 8;
    auto *stations = static_cast<RadioStation *>(
        std::calloc(kDiscoveryCapacity, sizeof(RadioStation)));
    while (stations) {
        uint32_t generation = 0;
        char city[sizeof(s_requested_city)] = {};
        char display[sizeof(s_requested_city_display)] = {};
        {
            std::lock_guard<std::mutex> lock(s_catalog_mutex);
            generation = s_catalog_generation;
            std::snprintf(city, sizeof(city), "%s", s_requested_city);
            std::snprintf(display, sizeof(display), "%s", s_requested_city_display);
        }

        std::memset(stations, 0, kDiscoveryCapacity * sizeof(RadioStation));
        const RadioCatalogPause pause = radio_player_pause_for_catalog();
        const bool idle = radio_player_wait_idle(3000);
        ESP_LOGI(kTag, "catalog: heap free=%u largest=%u (playing=%d idle=%d)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (int)pause.was_playing, (int)idle);

        RadioLocation location = {};
        const std::size_t count = !idle ? 0 : city[0]
                                      ? radio_catalog_discover_city(
                                            city, stations, kDiscoveryCapacity, &location)
                                      : radio_catalog_discover(
                                            stations, kDiscoveryCapacity, &location);

        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        if (generation != s_catalog_generation) {
            radio_player_resume_after_catalog(pause);
            continue;
        }
        if (s_connected.load(std::memory_order_acquire)) {
            // The national-fallback guard exists so an auto-located search does
            // not replace a good local list with generic top stations. When the
            // user *explicitly* picked a city it must not win: keeping the old
            // list is exactly why choosing 重庆 still showed 泉州.
            const bool keep_cached_on_fallback =
                city[0] == '\0' && location.national_fallback &&
                radio_player_has_cached_catalog();
            if (count > 0 && !keep_cached_on_fallback &&
                radio_player_replace_stations_resuming(stations, count)) {
                s_catalog_loaded = true;
                save_catalog_city(city);
                radio_ui_set_location(city[0]
                                          ? display
                                          : (location.city[0] ? location.city
                                                              : location.region));
            } else {
                radio_ui_set_location(city[0] ? display : "NATIONAL");
                radio_ui_set_station(
                    radio_player_station_index(), radio_player_station_count(),
                    radio_player_station(radio_player_station_index()));
            }
            radio_player_set_network(true);
        }
        // Give the decoder its contiguous block back *before* resuming, or the
        // playback worker starts without a pool and fails to open the stream.
        radio_player_ensure_pool();
        radio_player_resume_after_catalog(pause);
        s_catalog_task = nullptr;
        break;
    }
    std::free(stations);
    if (!stations) {
        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        s_catalog_task = nullptr;
        if (s_connected.load()) radio_player_set_network(true);
    }
    vTaskDelete(nullptr);
}

void start_catalog_search(const char *city, const char *display) {
    // Release the stream's HTTP client, network worker and jitter buffers before
    // allocating the catalog worker. Keep playback intent for its pause token.
    radio_player_set_network(false);
    // The stream worker tears down asynchronously: HTTP client, socket and the
    // jitter buffer all have to be released before the catalog worker's stack
    // can be allocated. Waiting a fixed 2s was not enough on hardware -- the
    // task then failed with free=3788/largest=1280, the failure path silently
    // kept the previous city's station list, and the user saw "picked 重庆 but
    // still 泉州". Wait for the worker to go idle AND for a contiguous block
    // big enough for the 6KB catalog stack, with a bounded overall budget.
    radio_player_wait_idle(3000);
    // The catalog search needs an HTTP client (~2KB contiguous) *plus* its 6KB
    // task stack. While the minimized radio keeps its 20KB decoder pool, only
    // ~1.8KB contiguous is left, so the search silently fails and the old
    // city's list stays on screen. The decoder is not needed while searching
    // (playback is paused), so hand the pool back and re-acquire it afterwards.
    // Never release while the worker still owns the pool: stream_station holds
    // the same buffers and release would race a use-after-free. If the stream
    // did not wind down in time, skip the release and let the task creation
    // fail loudly instead of corrupting memory.
    if (!radio_player_is_stream_active()) {
        radio_player_release_pool();
    } else {
        ESP_LOGW(kTag, "catalog: stream still active, keeping decoder pool");
    }
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 8192) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    bool task_running = false;
    bool task_available = true;
    {
        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        // The previous catalog may have finished during wait_idle above.
        radio_player_set_network(false);
        ++s_catalog_generation;
        std::snprintf(s_requested_city, sizeof(s_requested_city), "%s",
                      city ? city : "");
        std::snprintf(s_requested_city_display, sizeof(s_requested_city_display), "%s",
                      display ? display : "");
        task_running = s_catalog_task != nullptr;
        if (!task_running &&
            xTaskCreate(catalog_task, "radio_catalog", 6144, nullptr, 5,
                        &s_catalog_task) != pdPASS) {
            s_catalog_task = nullptr;
            task_available = false;
            ESP_LOGW(kTag, "catalog task create failed (free=%u largest=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        } else if (!task_running) {
            ESP_LOGI(kTag, "catalog task started for city '%s' (free=%u largest=%u)",
                     city ? city : "(auto)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
    }
    radio_ui_set_location(city && city[0] ? display : "LOCATING...");
    radio_ui_set_playback(RadioPlaybackState::Buffering,
                          city && city[0] ? "正在加载城市电台"
                                          : "正在搜索本地电台");
    if (!task_running && !task_available) {
        radio_ui_set_location(city && city[0] ? display : "NATIONAL");
        // The search never started, so nothing will give the decoder pool back.
        // Re-acquire it here or the radio stays silent until the next restart.
        radio_player_ensure_pool();
        radio_player_set_network(true);
    }
}

void start_local_catalog_search() {
    start_catalog_search(nullptr, nullptr);
}

void start_city_catalog_search(const char *city, const char *display) {
    start_catalog_search(city, display);
}

void wifi_event(WifiEvent event, const std::string &data) {
    auto &manager = WifiManager::GetInstance();
    switch (event) {
        case WifiEvent::Scanning:
            radio_ui_set_network(false, "正在寻找网络");
            break;
        case WifiEvent::Connecting:
            radio_ui_set_network(false, data.c_str());
            radio_ui_set_playback(RadioPlaybackState::Stopped, "正在连接无线网络");
            break;
        case WifiEvent::Connected: {
            ESP_LOGI(kTag, "Wi-Fi connected: %s, IP=%s", data.c_str(),
                     manager.GetIpAddress().c_str());
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            // Resolve through the LAN gateway first. The PC on this network
            // resolves every radio host through the router instantly, while
            // both the old 223.5.5.5 override and the DHCP-supplied 114
            // resolver intermittently returned getaddrinfo()=202 from the
            // device. The DHCP resolver stays as a portable backup.
            esp_netif_dns_info_t dhcp_dns = {};
            const bool have_dhcp_dns =
                netif &&
                esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dhcp_dns) == ESP_OK &&
                dhcp_dns.ip.type == ESP_IPADDR_TYPE_V4 &&
                dhcp_dns.ip.u_addr.ip4.addr != 0;
            esp_netif_ip_info_t ip_info = {};
            const bool have_gateway =
                netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
                ip_info.gw.addr != 0;
            esp_netif_dns_info_t dns = {};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            if (have_gateway) {
                dns.ip.u_addr.ip4 = ip_info.gw;
            } else if (have_dhcp_dns) {
                dns.ip.u_addr.ip4 = dhcp_dns.ip.u_addr.ip4;
            } else {
                dns.ip.u_addr.ip4.addr = ipaddr_addr("223.5.5.5");
            }
            char primary_dns[16] = {};
            esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, primary_dns, sizeof(primary_dns));
            for (int attempt = 0; attempt < 3 && netif; ++attempt) {
                esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            esp_netif_dns_info_t backup = {};
            backup.ip.type = ESP_IPADDR_TYPE_V4;
            if (have_dhcp_dns &&
                dhcp_dns.ip.u_addr.ip4.addr != dns.ip.u_addr.ip4.addr) {
                backup.ip.u_addr.ip4 = dhcp_dns.ip.u_addr.ip4;
            } else if (dns.ip.u_addr.ip4.addr != ipaddr_addr("223.5.5.5")) {
                backup.ip.u_addr.ip4.addr = ipaddr_addr("223.5.5.5");
            } else {
                backup.ip.u_addr.ip4.addr = ipaddr_addr("114.114.114.114");
            }
            if (netif) esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &backup);
            char backup_dns[16] = {};
            esp_ip4addr_ntoa(&backup.ip.u_addr.ip4, backup_dns, sizeof(backup_dns));
            ESP_LOGI(kTag, "radio DNS: %s / %s (gateway=%d dhcp=%d)",
                     primary_dns[0] ? primary_dns : "unknown",
                     backup_dns[0] ? backup_dns : "unknown",
                     (int)have_gateway, (int)have_dhcp_dns);
            s_connected.store(true);
            s_config_mode.store(false);
            s_config_can_cancel.store(false);
            s_settings_mode.store(false);
            // 把链路状态单向通知桥接层:它据此应答 UDP 发现请求,并在断链时
            // 主动关闭旧 socket(避免向失效链路继续写)。
            wireless_bridge_set_wifi_state(true, manager.GetIpAddress().c_str());
            // V6: 网络状态只更新数据, 不切屏(首页/各应用由框架自管理)。
            radio_ui_set_network(true, data.c_str());
            radio_ui_set_station(radio_player_station_index(),
                                 radio_player_station_count(),
                                 radio_player_station(radio_player_station_index()));
            // ⚠⚠ 连网默认 BALANCED(MIN_MODEM): 射频周期性休眠, 明显降温。
            // 网络电台是实时音频流, 播放时才由 radio_player 调
            // radio_wifi_set_streaming(true) 临时切 PERFORMANCE, 停止再切回。
            manager.SetPowerSaveLevel(WifiPowerSaveLevel::BALANCED);
            start_sntp_once();
            radio_player_set_network(true);
            // 首页/菜单的最小化收音机: 已有网络后自动播放当前频道。若当前在
            // 完整收音机/Codex/小智应用内，则保持暂停，由应用自己控制音频。
            app_framework_minimize_services(true);
            // ⚠⚠ 连网事件**不要**在这里触发目录搜索(2026-10-12 修复"收音机一直
            //   卡在正在切换电台/不出声")。
            //
            //   起因: WiFi 每次重连都会走到这个分支, 搜索于是被反复触发。搜索本身
            //   先 pause 播放(radio_player_pause_for_catalog), 再要 8KB 响应缓冲
            //   和一个 6144 栈的 catalog 任务 —— 而这台 C3 此时 largest 只有 ~4.8KB。
            //   结果 socket 分不到内部 RAM:
            //     transport_base: Failed to open a new connection: 32774 (NO_MEM)
            //   搜索又常常对 CN 城市查不到结果(返回 0 个台), 最后:
            //     radio_player: playback task could not restart after catalog
            //   播放任务再也起不来, 界面就一直停在"正在连接电台/正在切换电台"。
            //
            //   目录只在真正需要时加载: 进入收音机且没有缓存目录时, 或用户手动选城市。
            if (app_framework_level() == APP_LEVEL_APP &&
                app_framework_active() == APP_ID_RADIO &&
                !radio_player_has_cached_catalog()) {
                start_remembered_catalog_search();
            }
            break;
        }
        case WifiEvent::Disconnected:
            s_connected.store(false);
            wireless_bridge_set_wifi_state(false, nullptr);
            radio_player_set_network(false);
            radio_ui_set_network(false, "网络已断开");
            break;
        case WifiEvent::ConfigModeEnter: {
            s_config_mode.store(true);
            s_settings_mode.store(false);
            s_connected.store(false);
            radio_player_set_network(false);
            {
                std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                s_config_input_mode = ConfigInputMode::NetworkList;
                s_wifi_networks = {};
                s_wifi_network_count = 0;
                s_wifi_network_selected = 0;
                s_wifi_selected_ssid.clear();
                s_wifi_password.clear();
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
                s_config_ap_ssid = manager.GetApSsid();
            }
            start_wifi_scan_ui();
            break;
        }
        case WifiEvent::ConfigModeExit: {
            s_config_mode.store(false);
            s_config_can_cancel.store(false);
            {
                std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                s_config_input_mode = ConfigInputMode::NetworkList;
                s_wifi_networks = {};
                s_wifi_network_count = 0;
                s_wifi_network_selected = 0;
                s_wifi_selected_ssid.clear();
                s_wifi_password.clear();
                s_wifi_key_selected = 0;
                s_config_ap_ssid.clear();
            }
            // V6: 退出配网回框架首页(旧 radio_ui_show_main 会载入废弃页面)
            app_framework_go_home();
            radio_ui_set_network(false, "正在连接网络");
            if (!manager.IsConnected()) manager.StartStation();
            break;
        }
    }
}

void connection_watchdog_task(void *) {
    int tick = 0;
    while (true) {
        int waiting_seconds = 0;
        while (!s_connected.load() && !s_config_mode.load() && waiting_seconds < 30) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ++waiting_seconds;
            if (++tick % 5 == 0) xz_heap_report("idle5s");
        }
        if (!s_connected.load() && !s_config_mode.load() &&
            !s_portal_auto_opened.exchange(true)) {
            // 每次开机最多自动开一次配网门户; 用户手动退出后不再强制拉回
            // (V6 设计: 重新配网走 设置->WiFi 管理页)
            ESP_LOGW(kTag, "No usable network after 30 seconds; opening setup portal");
            enter_wifi_config(false);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++tick % 5 == 0) xz_heap_report("idle5s");
    }
}

}  // namespace

// ---- V6 应用层桥(供 app_apps.cc 调用; 统一 C 链接) ----
extern "C" {
void app_radio_request_catalog(void) {
    char cached_city[24] = {};
    const bool have_cached_city = load_catalog_city(cached_city, sizeof(cached_city));
    const char *selected_query = city_query_for(load_selected_city());
    const bool cache_matches_city =
        have_cached_city &&
        ((selected_query == nullptr && cached_city[0] == '\0') ||
         (selected_query != nullptr && std::strcmp(cached_city, selected_query) == 0));
    if (!cache_matches_city && radio_player_has_cached_catalog()) {
        ESP_LOGI(kTag, "catalog cache city '%s' != selected '%s'; re-searching",
                 have_cached_city ? cached_city : "(none)",
                 selected_query ? selected_query : "(auto)");
        radio_player_invalidate_catalog_cache();
    }
    bool needs_catalog;
    {
        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        needs_catalog = !s_catalog_loaded && !radio_player_has_cached_catalog() && s_catalog_task == nullptr;
    }
    if (s_connected.load() && needs_catalog) start_remembered_catalog_search();
}
bool app_radio_catalog_busy(void) {
    // 城市目录任务(定位+搜台)是否仍在运行。设置页的加载态据此轮询。
    return s_catalog_task != nullptr;
}

void app_radio_request_city(const char *city, const char *display) {
    const std::size_t count = city_choice_count();
    for (std::size_t i = 0; i < count; ++i) {
        const char *query = city_query_for(i);
        const bool matches = (!city || !city[0]) ? query == nullptr
                                                 : query && std::strcmp(city, query) == 0;
        if (matches) {
            save_selected_city(i);
            break;
        }
    }
    // 2026-10-26: 优先走实测白名单预设表(零网络延迟、全部可持续)。
    // 自动定位/预设未覆盖的城市仍走在线目录搜索。
    if (city && city[0] && radio_player_load_city_preset(city)) {
        radio_ui_set_location(display && display[0] ? display : city);
        radio_ui_set_station(0, radio_player_station_count(),
                             radio_player_station(0));
        radio_ui_set_playback(RadioPlaybackState::Stopped,
                              "已加载实测电台列表");
        radio_player_set_network(true);
        return;
    }
    start_city_catalog_search(city, display);
}
void app_radio_wifi_performance(void) {
    // ⚠ 必须 PERFORMANCE: 实时音频流不能进 modem-sleep, 否则周期性卡顿。
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
}
void app_radio_enter_wifi_setup(void) {
    enter_wifi_config(false, true);
}
void app_radio_show_saved_wifi(void) {
    show_saved_wifi();
}
}

// 实时音频流(全速)与空闲态(射频休眠)之间切换 WiFi 省电级别。
// 由 radio_player 在开始/停止播放时调用; 首页/Codex/待机保持 BALANCED 降温。
void radio_wifi_set_streaming(bool streaming) {
    auto &manager = WifiManager::GetInstance();
    if (!manager.IsConnected()) return;
    manager.SetPowerSaveLevel(streaming ? WifiPowerSaveLevel::PERFORMANCE
                                        : WifiPowerSaveLevel::BALANCED);
}

extern "C" bool radio_is_playing_c(void);
extern "C" void app_wifi_xiaozhi_mode(bool active) {
    if (active) {
        radio_wifi_set_streaming(true);
    } else if (!radio_is_playing_c()) {
        radio_wifi_set_streaming(false);
    }
}

extern "C" void app_main(void) {
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    xz_heap_report("t0_boot");

    // 开机预热收音机解码器池 —— 必须在**任何**大块内存被占用之前完成:
    // 此时 heap largest=114688。HANDOFF 第 6 节把这一步列在 LVGL/UI 初始化
    // 之前是有原因的: LVGL + V6 框架对象树会吃掉约 46KB, 等框架建完再预热
    // 就只剩 90KB 可用, 且 WiFi/音频/桥接随后再层层分割, 最终播放期
    // largest 掉到 2~3KB —— socket() 拿不到内部 RAM, 每 ~18s 重试一次, 满载发热。
    //
    // 2026-10-12 回归修复: 这个调用曾被整体删掉, 解码器改到 radio_app_enter()
    // 才在碎片堆上抢占, 实测 DMA 可用仅剩 60~184 字节 -> 收音机永远连不上。
    // 首页与菜单需要真正的最小化收音机，所以池在 Wi-Fi/I2S 初始化完成后、
    // Codex 无线桥启动前预热；小智进入时会暂停播放并释放这块连续内存。
    ESP_ERROR_CHECK(bsp_display_init());
    if (!bsp_lvgl_init()) {
        ESP_LOGE(kTag, "Display initialization failed");
        return;
    }
    bsp_display_backlight(72);
    if (!radio_display_settings_init()) {
        ESP_LOGW(kTag, "Display settings initialization failed");
    }
    bsp_battery_init();
    if (!radio_ui_init()) {
        ESP_LOGE(kTag, "UI initialization failed");
        return;
    }
    fap_screenshot_start();
    fap_set_key_inject_hook(serial_key_inject);   // 串口注入: u/d/o/l/k

    // 按键 UI 投递: 二级队列 + LVGL 任务内冲刷定时器
    s_ui_queue = xQueueCreate(8, sizeof(KeyMessage));
    if (s_ui_queue && bsp_lvgl_lock(1000)) {
        lv_timer_create(ui_flush_timer_cb, 30, nullptr);
        bsp_lvgl_unlock();
    }
    // Create input before the WiFi driver takes its internal buffers. The
    // remaining contiguous heap after WiFi init is too small for this task.
    s_key_queue = xQueueCreate(10, sizeof(KeyMessage));
    // 切换到 iot_button 组件回调驱动(原厂同路径): 修复三键失灵。
    // 轮询 button_task 的 oneshot 读数恒 2984mV 且按键无变化, 原因未明,
    // 组件驱动(连续采样+组件消抖)是原厂验证过的路径。
    if (!s_key_queue) {
        ESP_LOGE(kTag, "Key queue create failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_button_init(button_event_bridge, nullptr));
    if (xTaskCreate(input_task, "radio_input", 3072, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Input task create failed");
        return;
    }

    // The V6 home screen owns most LVGL objects. Build it before the WiFi
    // driver takes its internal buffers; otherwise LVGL's malloc assertion
    // halts app_main on the remaining ~27KB and the task watchdog fires.
    app_framework_init();
    xz_heap_report("post_framework_pre_wifi");

    xz_heap_report("pre_wifi_init");
    auto &manager = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.ssid_prefix = "LEO Radio";
    config.fixed_ap_ssid = "LEO Radio";
    config.language = "zh-CN";
    config.station_hostname = "ai-radio";
    config.station_scan_min_interval_seconds = 5;
    config.station_scan_max_interval_seconds = 30;
    if (!manager.Initialize(config)) {
        ESP_LOGE(kTag, "Wi-Fi driver initialization failed");
        return;
    }
    xz_heap_report("post_wifi_init");
    // WiFi driver is already initialized before the radio decoder pool.
    manager.SetEventCallback(wifi_event);

    // I2S DMA and the ES8311 codec need a contiguous internal-RAM block.
    // Wi-Fi init must claim its buffers first; afterward the reduced 4x128
    // DMA pool fits in the remaining largest block.
    const bool audio_ready = radio_player_init();
    if (!audio_ready) {
        radio_ui_set_playback(RadioPlaybackState::Error, "音频初始化失败");
        ESP_LOGE(kTag, "Audio player initialization failed");
    }
    xz_heap_report("post_audio_init");

    // The home/menu layer always owns a minimized radio. Prewarm after Wi-Fi
    // and I2S have claimed their buffers, but before the Codex bridge task:
    // earlier boot prewarm starved Wi-Fi; later (on first radio entry) leaves
    // too little contiguous memory after the UI has fragmented the heap.
    if (audio_ready && !radio_player_prewarm()) {
        ESP_LOGW(kTag, "minimized radio decoder prewarm failed");
    }
    xz_heap_report("post_radio_prewarm");

    // ---- 小智 opus 编码器: 不在此预热 ----
    // 编码器(25KB 连续块)与收音机 MP3 解码器互斥: 开机预热会让收音机
    // 报 "There is no memory for MP3 required" 完全播不了。改为进小智 app
    // 时申请(此时收音机已暂停并释放解码缓冲), 退出小智时释放还给收音机。
    // 见 xiaozhi_client.c 的 xiaozhi_opus_prewarm / xiaozhi_opus_release。
    // ---- Codex 监控页子系统 ----
    // 音频就绪后再装提示音,避免在 codec 未打开时播放。
    // wireless_bridge 的真正 init 放在下方 WifiManager 之后(socket 依赖 netif)。
    app_coding_set_audio_available(audio_ready);
    ui_sfx_init(audio_ready);
    ui_sfx_set_volume(app_settings_sfx_volume());
    wireless_bridge_set_wifi_configure(codex_wifi_configure);
    if (!radio_sleep_timer_init()) {
        ESP_LOGW(kTag, "Sleep timer initialization failed");
    }


    // NVS 为空 => 全新设备没有任何可用凭据, 必须进配网。但**绝不能在这里直接开 AP**:
    //   此刻堆/DMA 刚被 WiFi 缓冲 + I2S DMA + codec 吃穿(实测 post_audio_init
    //   dma 只剩 7736 字节, largest 7680), SoftAP 的 beacon 缓冲(752 x 2)
    //   分配不出来 -> ieee80211_hostap_attach 里空指针被解引用:
    //     wifi:alloc eb len=752 type=4 fail
    //     Guru Meditation Error: Core 0 panic'ed (Load access fault)
    //   设备陷入无限重启, 表现为"一直响、反复刷首页"。
    // 修法: 推迟到所有开机初始化(音频/音效/桥接/守护任务)全部做完之后再开,
    // 那时收音机还没开始播放, DMA 余量充足, AP 能稳定起来。
    auto &ssid_manager = SsidManager::GetInstance();
    const auto &saved_networks = ssid_manager.GetSsidList();
    const bool need_setup_ap = saved_networks.empty();
    if (!need_setup_ap) {
        // Existing owners keep all networks already stored in NVS. Give those
        // networks 30 seconds to appear before opening the setup portal.
        manager.StartStation();
    }

    // WiFi station setup allocates event nodes and netif state. Start the
    // bridge only after those allocations so the bridge task cannot starve
    // the WiFi event loop during boot.
    // 首页/菜单需要常驻最小化 Codex，因此无线桥在开机后即启动；完整 Codex
    // 页面只在进入 Codex 应用时创建，桥接消息在后台只更新状态，不触碰页面。
    esp_err_t bridge_result = coding_bridge_start_wireless();
    xz_heap_report("post_coding_bridge");
    if (bridge_result != ESP_OK) {
        ESP_LOGW(kTag, "Codex bridge startup failed: %s", esp_err_to_name(bridge_result));
    }
    coding_bridge_set_foreground_ui(false);

    // 用静态栈创建: 这条任务负责"30s 拿不到网就开配网门户", 属于兜底安全网,
    // 绝不能因为开机瞬间堆紧张而静默消失(实测 post_coding_bridge 时
    // largest 仅 1024 字节, xTaskCreate(2048) 直接失败 -> "Wi-Fi watchdog task
    // failed", 于是断网后永远进不了配网页)。
    static StackType_t s_wifi_guard_stack[2048];
    static StaticTask_t s_wifi_guard_tcb;
    if (xTaskCreateStatic(connection_watchdog_task, "radio_wifi_guard", 2048,
                          nullptr, 4, s_wifi_guard_stack, &s_wifi_guard_tcb) == NULL) {
        ESP_LOGE(kTag, "Wi-Fi watchdog task failed");
    }


    ESP_LOGI(kTag, "Internet radio ready");
}
