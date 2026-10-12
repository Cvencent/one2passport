// main/wireless_bridge.h
// 局域网桥接层:TCP 8765 承载行分隔 JSON 事件,UDP 8764 负责设备发现。
//
// 【与 FoloOS 原版的架构差异 —— 重要】
// FoloOS 版会自行接管整个 WiFi 栈(esp_netif_create_default_wifi_sta /
// esp_wifi_init / esp_wifi_start)并自建一套 "FoloOS-Setup" 配网门户。
// 本工程基于 LEO RADIO,WiFi 生命周期已由 esp-wifi-connect(WifiManager)独占,
// 两者重复初始化必然失败。
//
// 因此本层被降级为【纯传输层】:
//   - 不再初始化/配置 WiFi,只做 socket 收发与 token 鉴权。
//   - WiFi 连接状态由 main.cc 在收到 WifiEvent 后单向通知
//     (wireless_bridge_set_wifi_state)。
//   - 换网请求通过注册的 C 回调转发给 WifiManager::ConfigureNetwork
//     (wireless_bridge_set_wifi_configure),避免在 C 文件里直接依赖 C++ 类。
#pragma once

/* 实现在 wireless_bridge.c(C),调用方 main.cc 是 C++。
 * 缺了下面的 extern "C" 会因 C++ name mangling 在链接期报
 * undefined reference to `wireless_bridge_set_wifi_state(bool, char const*)`。
 * FoloOS 原版入口是 C 文件,所以它的头文件省了这层守卫。 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef void (*wireless_bridge_line_cb_t)(const char *line);
typedef void (*wireless_bridge_status_cb_t)(const char *status, const char *ip);
// 换网回调。由 main.cc 实现,内部转调 WifiManager::ConfigureNetwork。
typedef esp_err_t (*wireless_bridge_wifi_configure_t)(const char *ssid,
                                                      const char *password);

typedef bool (*wireless_bridge_authenticate_t)(const char *line,
                                               const char *device_id);

// 初始化传输层。必须在 WifiManager::Initialize() 之后调用
// (socket 依赖已建立的 netif)。内部只加载 token 并启动网络任务。
esp_err_t wireless_bridge_init(wireless_bridge_line_cb_t line_cb,
                               wireless_bridge_status_cb_t status_cb);

// 停止网络任务并关闭 TCP/UDP socket。下一次 wireless_bridge_init() 可重新启动。
void wireless_bridge_stop(void);

// 安装统一协议握手处理。返回 true 表示该行已完成 bridge_auth 应答，
// 传输层不再把这一行交给应用消息回调。
void wireless_bridge_set_authenticate(
    wireless_bridge_authenticate_t authenticate, const char *device_id);

// 注册换网回调(见上方说明)。未注册时 wireless_bridge_configure 返回
// ESP_ERR_INVALID_STATE。
void wireless_bridge_set_wifi_configure(wireless_bridge_wifi_configure_t configure);

// 由 main.cc 在 WifiEvent 变化时通知。connected=false 时会断开当前客户端连接,
// 避免向已失效的链路继续写入。
void wireless_bridge_set_wifi_state(bool connected, const char *ip);

// 向已通过 token 鉴权的电脑端发送一行数据(含结尾换行)。成功返回 true。
bool wireless_bridge_write(const char *data, size_t length);

// 保存 token 并转发换网请求(设备端 UI 或电脑端 wifi_setup 事件触发)。
esp_err_t wireless_bridge_configure(const char *ssid, const char *password,
                                    const char *token);

bool wireless_bridge_is_connected(void);
void wireless_bridge_get_ip(char *buffer, size_t size);
// 主动断开当前 PC 会话。下一次 PC 轮询发现设备时会重新认证并让主机重放
// 当前未读消息与已读状态；设备端不需要保存消息缓存。
void wireless_bridge_disconnect(void);

#ifdef __cplusplus
}
#endif
