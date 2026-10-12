// main/wireless_bridge.c
// 局域网桥接层:TCP 8765 承载行分隔 JSON 事件,UDP 8764 负责设备发现。
//
// 【与 FoloOS 原版的架构差异 —— 必读】
// FoloOS 原版在这里完整接管 WiFi 栈并自建一套配网门户。本工程基于 LEO RADIO,
// WiFi 已由 esp-wifi-connect(WifiManager)独占管理,重复初始化必然失败。
// 故本文件被裁剪为【纯传输层】,只保留:
//   - TCP 8765 / UDP 8764 socket 服务与 bridge_auth token 鉴权
//   - token 的 NVS 持久化(独立 namespace,不碰 esp-wifi-connect 的数据)
//   - 通过回调把换网请求交给 C++ 侧的 WifiManager
// 被移除的部分:esp_netif/esp_wifi 初始化、STA 连接与重连状态机、多 profile
// 轮换、FoloOS-Setup AP 配网页、WiFi 事件处理。这些职责统一归 WifiManager。
#include "wireless_bridge.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "codex_bridge_protocol.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BRIDGE_PORT 8765
#define DISCOVERY_PORT 8764
#define NETWORK_LINE_MAX 768
/* 必须与 FoloOS 原版一致(7168),不可下调。本任务不只是收发 socket:
 * 收到一行 JSON 后会经 s_line_cb -> process_wireless_line -> process_line
 * -> dispatch_message -> app_coding_*(持 LVGL 锁,内部再走 coding_render 的
 * 完整绘制链)。这条链的栈深度由 FoloOS 原版实测标定在 7168;而本版回调
 * 链与它完全相同,没有任何变浅的理由。NETWORK_LINE_MAX 也保持 768,
 * 因为同行缓冲区就落在这条链的栈上。 */
#define NETWORK_TASK_STACK 7168
#define TOKEN_MAX 64

static const char *TAG = "wireless_bridge";
static const char *NVS_NAMESPACE = "folo_bridge";
static const char *NVS_TOKEN_KEY = "token";
static const char *DISCOVERY_REQUEST = "CODEXBRIDGE_DISCOVER";

static wireless_bridge_line_cb_t s_line_cb;
static wireless_bridge_status_cb_t s_status_cb;
static wireless_bridge_wifi_configure_t s_wifi_configure_cb;
static wireless_bridge_authenticate_t s_authenticate_cb;
static char s_device_id[24];
static SemaphoreHandle_t s_client_lock;
static int s_client_fd = -1;
static bool s_client_authenticated;
// 由 main.cc 经 wireless_bridge_set_wifi_state() 单向写入,本文件不自行探测。
static bool s_wifi_connected;
static char s_token[TOKEN_MAX + 1];
static char s_ip[16];
static volatile bool s_task_stop_requested;
static TaskHandle_t s_task_handle;

static esp_err_t save_token(const char *token);

static void client_replace(int new_fd)
{
    if (s_client_lock == NULL) {
        if (new_fd >= 0) close(new_fd);
        return;
    }
    if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        if (new_fd >= 0) close(new_fd);
        return;
    }
    if (s_client_fd >= 0) {
        shutdown(s_client_fd, SHUT_RDWR);
        close(s_client_fd);
    }
    s_client_fd = new_fd;
    s_client_authenticated = false;
    xSemaphoreGive(s_client_lock);
}

bool wireless_bridge_write(const char *data, size_t length)
{
    if (data == NULL || length == 0 || s_client_lock == NULL ||
        xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    bool complete = false;
    if (s_client_fd >= 0 && s_client_authenticated) {
        size_t sent = 0;
        TickType_t retry_started = xTaskGetTickCount();
        while (sent < length) {
            int result = send(s_client_fd, data + sent, length - sent, 0);
            if (result <= 0) {
                int error = errno;
                if (result < 0 &&
                    (error == EAGAIN || error == ENOMEM || error == ENOBUFS) &&
                    xTaskGetTickCount() - retry_started < pdMS_TO_TICKS(1000)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                ESP_LOGW(TAG, "send failed: errno=%d bytes=%u/%u largest=%u",
                         error, (unsigned)sent, (unsigned)length,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                shutdown(s_client_fd, SHUT_RDWR);
                s_client_authenticated = false;
                break;
            }
            sent += (size_t)result;
        }
        complete = sent == length;
    }
    xSemaphoreGive(s_client_lock);
    return complete;
}

static bool authenticate_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    const cJSON *type = root != NULL
                            ? cJSON_GetObjectItemCaseSensitive(root, "type")
                            : NULL;
    const cJSON *token = root != NULL
                             ? cJSON_GetObjectItemCaseSensitive(root, "token")
                             : NULL;
    bool is_auth = cJSON_IsString(type) && cJSON_IsString(token) &&
                   strcmp(type->valuestring, "bridge_auth") == 0 &&
                   strlen(token->valuestring) >= 16;
    bool matches = is_auth && s_token[0] != '\0' &&
                   strcmp(token->valuestring, s_token) == 0;
    /* TOFU(首次信任配对):设备本地没有 token 时,接受电脑端带来的第一个
     * 合法 token 并落盘。配对之后 token 固定,后续连接必须严格匹配。 */
    bool pair_first_time = is_auth && s_token[0] == '\0';
    if (pair_first_time) {
        strlcpy(s_token, token->valuestring, sizeof(s_token));
    }
    cJSON_Delete(root);
    if (!matches && !pair_first_time) return false;
    if (pair_first_time) {
        esp_err_t saved = save_token(s_token);
        ESP_LOGI(TAG, "paired with client, token persist=%s",
                 saved == ESP_OK ? "ok" : esp_err_to_name(saved));
    }

    /* Mark the session before invoking the protocol callback: the callback
     * writes bridge_auth_ok through the normal authenticated transport. */
    if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_client_authenticated = true;
        xSemaphoreGive(s_client_lock);
    }
    bool accepted = s_authenticate_cb != NULL &&
                    s_authenticate_cb(line, s_device_id);
    if (!accepted) {
        if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_client_authenticated = false;
            xSemaphoreGive(s_client_lock);
        }
        return false;
    }
    ESP_LOGI(TAG, "bridge client authenticated");
    return true;
}

static void process_client_bytes(const uint8_t *input, size_t count,
                                 char *line, size_t *line_length,
                                 bool *dropping_line)
{
    for (size_t index = 0; index < count; ++index) {
        char character = (char)input[index];
        if (character == '\r') continue;
        if (character == '\n') {
            if (!*dropping_line && *line_length > 0) {
                line[*line_length] = '\0';
                bool authenticated = false;
                /* A bounded audio send may hold this lock during backpressure.
                 * Waiting must not turn an authenticated packet into bad auth. */
                if (xSemaphoreTake(s_client_lock, portMAX_DELAY) == pdTRUE) {
                    authenticated = s_client_authenticated;
                    xSemaphoreGive(s_client_lock);
                }
                if (!authenticated) {
                    if (!authenticate_line(line)) client_replace(-1);
                } else if (s_line_cb != NULL) {
                    s_line_cb(line);
                }
            }
            *line_length = 0;
            *dropping_line = false;
        } else if (!*dropping_line) {
            if (*line_length < NETWORK_LINE_MAX) {
                line[(*line_length)++] = character;
            } else {
                *dropping_line = true;
            }
        }
    }
}

static int create_bound_socket(int type, int port)
{
    int fd = socket(AF_INET, type, IPPROTO_IP);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void network_task(void *argument)
{
    (void)argument;
    int tcp_fd = create_bound_socket(SOCK_STREAM, BRIDGE_PORT);
    int udp_fd = create_bound_socket(SOCK_DGRAM, DISCOVERY_PORT);
    if (tcp_fd < 0 || udp_fd < 0 || listen(tcp_fd, 1) != 0) {
        ESP_LOGE(TAG, "network sockets failed: %d", errno);
        if (tcp_fd >= 0) close(tcp_fd);
        if (udp_fd >= 0) close(udp_fd);
        vTaskDelete(NULL);
        return;
    }

    char line[NETWORK_LINE_MAX + 1];
    size_t line_length = 0;
    bool dropping_line = false;
    uint8_t input[1024];
    ESP_LOGI(TAG, "LAN bridge ready on TCP %d", BRIDGE_PORT);

    while (true) {
        if (s_task_stop_requested) break;
        int client = -1;
        if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            client = s_client_fd;
            xSemaphoreGive(s_client_lock);
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(tcp_fd, &read_set);
        FD_SET(udp_fd, &read_set);
        int max_fd = tcp_fd > udp_fd ? tcp_fd : udp_fd;
        if (client >= 0) {
            FD_SET(client, &read_set);
            if (client > max_fd) max_fd = client;
        }
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(max_fd + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) continue;

        if (FD_ISSET(tcp_fd, &read_set)) {
            int accepted = accept(tcp_fd, NULL, NULL);
            if (accepted >= 0) {
                struct timeval send_timeout = {.tv_sec = 0, .tv_usec = 250000};
                setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO,
                           &send_timeout, sizeof(send_timeout));
                int no_delay = 1;
                setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                           &no_delay, sizeof(no_delay));
                client_replace(accepted);
                line_length = 0;
                dropping_line = false;
                client = accepted;
            }
        }
        if (FD_ISSET(udp_fd, &read_set)) {
            struct sockaddr_in source;
            socklen_t source_length = sizeof(source);
            int count = recvfrom(udp_fd, input, sizeof(input) - 1, 0,
                                 (struct sockaddr *)&source, &source_length);
            if (count > 0) {
                input[count] = '\0';
                /* 只在已联上 Wi-Fi 时才应答发现请求。此处刻意不要求 token
                 * 已存在 —— 未配对设备必须先被电脑端发现才能发起 TOFU 配对。
                 * 未完成配网的设备 netif 尚未就绪,不会出现在局域网上。 */
                if (s_wifi_connected &&
                    strcmp((const char *)input, DISCOVERY_REQUEST) == 0) {
                    /* device 字段必须与电脑端 mac_bridge.py 的
                     * discover_wireless_device() 判定值逐字一致,
                     * 否则电脑端收不到应答、报"局域网内没有发现已配网的
                     * AI Passport"。此处刻意沿用 FoloOS 原版字符串。 */
                    cJSON *reply = cJSON_CreateObject();
                    if (reply != NULL) {
                        cJSON_AddStringToObject(reply, "device", "FoloOS-AI-Passport");
                        cJSON_AddStringToObject(reply, "device_id", s_device_id);
                        cJSON_AddStringToObject(reply, "name", "AI Passport");
                        cJSON_AddNumberToObject(reply, "port", BRIDGE_PORT);
                        cJSON *capabilities = cJSON_CreateArray();
                        if (capabilities != NULL) {
                            size_t capability_count = 0;
                            const char *const *supported =
                                codex_bridge_protocol_capabilities(&capability_count);
                            for (size_t index = 0; index < capability_count; ++index) {
                                cJSON_AddItemToArray(
                                    capabilities,
                                    cJSON_CreateString(supported[index])
                                );
                            }
                            cJSON_AddItemToObject(reply, "capabilities", capabilities);
                        }
                        char *payload = cJSON_PrintUnformatted(reply);
                        if (payload != NULL) {
                            sendto(udp_fd, payload, strlen(payload), 0,
                                   (struct sockaddr *)&source, source_length);
                            cJSON_free(payload);
                        }
                        cJSON_Delete(reply);
                    }
                }
            }
        }
        if (client >= 0 && FD_ISSET(client, &read_set)) {
            int count = recv(client, input, sizeof(input), 0);
            if (count <= 0) {
                client_replace(-1);
                line_length = 0;
                dropping_line = false;
            } else {
                process_client_bytes(input, (size_t)count, line, &line_length,
                                     &dropping_line);
            }
        }
    }

    client_replace(-1);
    close(tcp_fd);
    close(udp_fd);
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

static void load_token(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = sizeof(s_token);
    if (nvs_get_str(handle, NVS_TOKEN_KEY, s_token, &length) != ESP_OK) {
        s_token[0] = '\0';
    }
    nvs_close(handle);
}

static esp_err_t save_token(const char *token)
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_str(handle, NVS_TOKEN_KEY, token);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return result;
}

void wireless_bridge_set_wifi_configure(wireless_bridge_wifi_configure_t configure)
{
    s_wifi_configure_cb = configure;
}

void wireless_bridge_set_authenticate(
    wireless_bridge_authenticate_t authenticate, const char *device_id)
{
    s_authenticate_cb = authenticate;
    strlcpy(s_device_id, device_id != NULL ? device_id : "", sizeof(s_device_id));
}

void wireless_bridge_set_wifi_state(bool connected, const char *ip)
{
    s_wifi_connected = connected;
    strlcpy(s_ip, ip != NULL ? ip : "", sizeof(s_ip));
    /* 链路已断,继续向旧 socket 写入只会堆积超时;主动断开让电脑端重连。 */
    if (!connected) client_replace(-1);
    if (s_status_cb != NULL) {
        s_status_cb(connected ? "connected" : "disconnected", s_ip);
    }
}

bool wireless_bridge_is_connected(void)
{
    return s_wifi_connected;
}

void wireless_bridge_disconnect(void)
{
    client_replace(-1);
}

void wireless_bridge_get_ip(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) return;
    strlcpy(buffer, s_ip, size);
}

esp_err_t wireless_bridge_configure(const char *ssid, const char *password,
                                    const char *token)
{
    /* token 由电脑端首配时下发;设备端 UI 已持有时可传 NULL 跳过覆盖。 */
    if (token != NULL && token[0] != '\0') {
        esp_err_t saved = save_token(token);
        if (saved != ESP_OK) return saved;
        strlcpy(s_token, token, sizeof(s_token));
    }
    if (s_wifi_configure_cb == NULL) {
        ESP_LOGW(TAG, "wifi configure callback not registered");
        return ESP_ERR_INVALID_STATE;
    }
    /* 实际连接由 WifiManager 负责,结果经 WifiEvent 异步回传。 */
    return s_wifi_configure_cb(ssid, password);
}

esp_err_t wireless_bridge_init(wireless_bridge_line_cb_t line_cb,
                               wireless_bridge_status_cb_t status_cb)
{
    s_line_cb = line_cb;
    s_status_cb = status_cb;
    if (s_client_lock == NULL) {
        s_client_lock = xSemaphoreCreateMutex();
        if (s_client_lock == NULL) return ESP_ERR_NO_MEM;
    }
    /* 注意:nvs_flash_init / esp_netif_init / esp_event_loop_create_default
     * 均已由 main.cc 与 WifiManager 完成,此处不再重复调用 —— 重复调用
     * esp_netif_create_default_wifi_sta() 会返回 NULL 并导致启动失败。 */
    load_token();

    if (s_task_handle != NULL) return ESP_OK;
    s_task_stop_requested = false;
    if (xTaskCreate(network_task, "wireless_bridge", NETWORK_TASK_STACK,
                    NULL, 4, &s_task_handle) != pdPASS) {
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "transport ready, token=%s", s_token[0] ? "loaded" : "missing");
    return ESP_OK;
}

void wireless_bridge_stop(void)
{
    s_task_stop_requested = true;
    client_replace(-1);
    for (int i = 0; i < 100 && s_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task_handle != NULL) {
        ESP_LOGW(TAG, "network task did not stop in time");
    }
}
