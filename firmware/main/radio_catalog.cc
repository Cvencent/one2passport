#include "radio_catalog.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/errno.h"
#include "lwip/sockets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr char kTag[] = "radio_catalog";
constexpr char kLocationUrl[] =
    "http://ip-api.com/json/?fields=status,countryCode,regionName,city";
constexpr char kBackupLocationUrl[] =
    "http://ipwho.is/?fields=success,country_code,region,city";
// 第二定位源: 实测与 ip-api 一样给出正确的中文省市(Chongqing/Chongqing/CN),
// 比 ipwho.is 准。走 http: TLS 握手要一整块 ~10KB 连续堆, C3 上跑起 WiFi 后
// 根本凑不出来, 用 https 等于这个源永久不可用(实测 ESP_ERR_HTTP_CONNECT)。
constexpr char kIpinfoLocationUrl[] = "http://ipinfo.io/json";
constexpr char kDirectoryBase[] =
    "http://de1.api.radio-browser.info/json/stations/search";
// ⚠ 响应缓冲**不能太大**: WiFi 连上后堆碎片化, largest 常只有 ~7.7KB。
// 早期用 24576 -> malloc 必然失败 -> query_directory 直接 return 0 ->
// 本地电台/城市搜索永远"返回 0 条"(实测现象: 定位成功却搜不到任何台)。
// 现在收到 8192(堆可承受), 同时 limit 收敛到 6 保证响应 <8KB。
constexpr std::size_t kResponseCapacity = 6144;  // 2026-10-18: was 8192; borrowed pool is only 6656B
// 市名查询少于这个条数时, 才补一次本省 state 查询(区县台台名不带市名)。
// 广州/成都这类市名命中充足的城市因此不会掺进邻市台。
constexpr std::size_t kThinResultCount = 3;

// HTTP 头部与正文的分隔。用字符常量拼出来, 免得源码里的转义被编辑器吃掉。
constexpr char CRLF_CRLF[] = {0x0D, 0x0A, 0x0D, 0x0A, 0x00};

// esp_http_client_init 需要一整块连续内部堆(实测约 4KB: TCP pcb + TLS 上下文
// + 收发缓冲)。WiFi 跑起来后堆常年碎到 largest=1792~3456, 这时**每一次**请求
// 都会打印 `HTTP_CLIENT: Allocation failed` 然后静默返回 0 条 —— 这就是
// "定位明明成功却搜不到台"的真正原因(2026-10-18 实测日志)。所以发请求之前
// 先等堆合并出足够连续块, 等不到就明确放弃而不是空跑一轮。
// 1024B: buffer_size 降到 512 之后, 建一个纯 HTTP 客户端只需要 1KB 左右的连续块
// (客户端结构 + 512 收 + 256 发各自一次 malloc)。实测 WiFi 跑起来后 largest
// 常在 1728~7680 之间波动, 阈值取 1024 既放行正常情况, 也能在堆真的枯了时
// 早退而不是一路 Allocation failed。
constexpr std::size_t kMinHttpHeapBlock = 1024;
// 低于它时最多等 4s 让堆自然合并，等不到就明确放弃这次请求。

bool wait_http_heap(int budget_ms) {
    for (int waited = 0; waited < budget_ms; waited += 200) {
        if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= kMinHttpHeapBlock)
            return true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= kMinHttpHeapBlock;
}

// 借用的播放池只有 kOutputSize(实测 6656B), 不能按 kResponseCapacity(8192B)
// 声明容量, 否则 http_get 会越界写坏相邻内存。借用时取两者的较小值。
std::size_t response_capacity(bool borrowed) {
    if (!borrowed) return kResponseCapacity;
    return std::min(radio_player_search_buffer_size(), kResponseCapacity);
}

struct CityTranslation {
    const char *english;
    const char *chinese;
};

// ip-api returns English city names. Radio Browser's Chinese entries are much
// easier to find by their Chinese city name, so map the most common cities and
// fall back to a province/state query for every other place.
constexpr CityTranslation kChineseCities[] = {
    {"Beijing", "北京"},       {"Shanghai", "上海"},
    {"Tianjin", "天津"},       {"Chongqing", "重庆"},
    {"Guangzhou", "广州"},     {"Shenzhen", "深圳"},
    {"Dongguan", "东莞"},      {"Foshan", "佛山"},
    {"Zhuhai", "珠海"},        {"Huizhou", "惠州"},
    {"Zhongshan", "中山"},     {"Shantou", "汕头"},
    {"Hangzhou", "杭州"},      {"Ningbo", "宁波"},
    {"Wenzhou", "温州"},       {"Jiaxing", "嘉兴"},
    {"Shaoxing", "绍兴"},      {"Jinhua", "金华"},
    {"Nanjing", "南京"},       {"Suzhou", "苏州"},
    {"Wuxi", "无锡"},          {"Changzhou", "常州"},
    {"Nantong", "南通"},       {"Xuzhou", "徐州"},
    {"Chengdu", "成都"},       {"Mianyang", "绵阳"},
    {"Wuhan", "武汉"},         {"Yichang", "宜昌"},
    {"Changsha", "长沙"},      {"Zhengzhou", "郑州"},
    {"Luoyang", "洛阳"},       {"Jinan", "济南"},
    {"Qingdao", "青岛"},       {"Yantai", "烟台"},
    {"Shenyang", "沈阳"},      {"Dalian", "大连"},
    {"Harbin", "哈尔滨"},      {"Changchun", "长春"},
    {"Shijiazhuang", "石家庄"}, {"Taiyuan", "太原"},
    {"Xi'an", "西安"},         {"Xian", "西安"},
    {"Kunming", "昆明"},       {"Guiyang", "贵阳"},
    {"Nanning", "南宁"},       {"Haikou", "海口"},
    {"Fuzhou", "福州"},        {"Xiamen", "厦门"},
    {"Nanchang", "南昌"},      {"Hefei", "合肥"},
    {"Lanzhou", "兰州"},       {"Urumqi", "乌鲁木齐"},
    {"Hohhot", "呼和浩特"},    {"Yinchuan", "银川"},
    {"Xining", "西宁"},        {"Lhasa", "拉萨"},
};

bool starts_with(const char *text, const char *prefix) {
    return text && prefix && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

bool contains(const char *text, const char *needle) {
    return text && needle && std::strstr(text, needle) != nullptr;
}


// ---- 裸 socket 的极简 HTTP GET --------------------------------------------
//
// 为什么不用 esp_http_client: 它在 init 时要一整块连续内部堆放收发缓冲, 而 C3
// 跑起 WiFi + LVGL + 音频池之后堆常年碎到 largest=1728~3456(实测), 于是每一次
// 定位/搜台都打印 `HTTP_CLIENT: Allocation failed` 并静默返回 0 条 —— 用户看到
// 的就是"定位成功却没有本地电台"。目录查询只是几十~几千字节的 JSON, 用 lwip
// 裸 socket 直接收发即可, 全程只占栈, 不碰堆。
struct HttpTarget {
    char host[64];
    char path[320];
    int port;
};

bool parse_http_target(const char *url, HttpTarget *out) {
    if (!url || !out) return false;
    const char *rest = nullptr;
    if (std::strncmp(url, "http://", 7) == 0) {
        rest = url + 7;
        out->port = 80;
    } else {
        return false;   // https 仍走 esp_http_client(TLS 无法手写)
    }
    const char *slash = std::strchr(rest, '/');
    const char *colon = std::strchr(rest, ':');
    std::size_t host_len = 0;
    if (slash) {
        const char *end = (colon && colon < slash) ? colon : slash;
        host_len = static_cast<std::size_t>(end - rest);
        std::snprintf(out->path, sizeof(out->path), "%s", slash);
    } else {
        const char *end = colon ? colon : rest + std::strlen(rest);
        host_len = static_cast<std::size_t>(end - rest);
        std::snprintf(out->path, sizeof(out->path), "/");
    }
    if (host_len == 0 || host_len >= sizeof(out->host)) return false;
    std::memcpy(out->host, rest, host_len);
    out->host[host_len] = '\0';
    if (colon && (!slash || colon < slash)) {
        const int parsed = std::atoi(colon + 1);
        if (parsed <= 0 || parsed > 65535) return false;
        out->port = parsed;
    }
    return true;
}

// 读满 want 字节或对端关闭/超时。返回实际读到的字节数(<0 表示错误)。
// 读到"对端关闭"或读超时为止, 最多 want 字节。请求都带 Connection: close,
// 服务端发完正文就关连接, 所以这里能自然结束; 缓冲区塞满也会返回。
int socket_read_n(int fd, char *buf, std::size_t want) {
    std::size_t got = 0;
    while (got < want) {
        const int received = ::recv(fd, buf + got, want - got, 0);
        if (received > 0) {
            got += static_cast<std::size_t>(received);
            continue;
        }
        if (received == 0) break;                 // 对端关闭: 正文收完
        const int err = errno;
        if (err == EINTR) continue;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            // 读超时。已收到内容就当作成功(有些目录节点不主动关连接)。
            return got > 0 ? static_cast<int>(got) : -1;
        }
        return -1;
    }
    return static_cast<int>(got);
}

bool raw_http_get(const char *url, char *response, std::size_t capacity,
                  int timeout_ms) {
    HttpTarget target = {};
    if (!parse_http_target(url, &target)) return false;

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addr = nullptr;
    char port_text[8] = {};
    std::snprintf(port_text, sizeof(port_text), "%d", target.port);
    const int dns_err = ::getaddrinfo(target.host, port_text, &hints, &addr);
    if (dns_err != 0 || !addr) {
        ESP_LOGW(kTag, "DNS %s failed: %d", target.host, dns_err);
        if (addr) ::freeaddrinfo(addr);
        return false;
    }

    const int fd = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    bool ok = false;
    if (fd >= 0) {
        struct timeval tv = {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(fd, addr->ai_addr, addr->ai_addrlen) == 0) {
            // 请求写在栈上, 不占堆。
            char request[512] = {};
            const int length = std::snprintf(
                request, sizeof(request),
                "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: AI-Passport-Radio/1.1.1\r\n"
                "Accept: application/json\r\nAccept-Encoding: identity\r\n"
                "Icy-MetaData: 0\r\nConnection: close\r\n\r\n",
                target.path, target.host);
            if (length > 0 && ::send(fd, request, static_cast<std::size_t>(length), 0) == length) {
                // 把整段响应(含头部)读进 response, 再就地切出头/体。
                // 以前用单独的 512B 头缓冲: radio-browser 会多带几行 CORS 头,
                // 头部本身就超过 512B, 于是分页请求全部判成 header 不完整而失败。
                const int got = socket_read_n(fd, response, capacity - 1);
                if (got > 0) {
                    response[got] = '\0';
                    char *sep = std::strstr(response, CRLF_CRLF);
                    const bool status_ok =
                        got > 12 && response[0] == 'H' &&
                        std::strncmp(response + 9, "200", 3) == 0;
                    if (sep && status_ok) {
                        const std::size_t head_len =
                            static_cast<std::size_t>(sep - response) + 4;
                        const std::size_t body_len =
                            static_cast<std::size_t>(got) - head_len;
                        std::memmove(response, response + head_len, body_len);
                        response[body_len] = '\0';
                        ok = body_len > 0;
                    } else if (sep) {
                        ESP_LOGW(kTag, "GET %s -> bad status", url);
                    } else {
                        ESP_LOGW(kTag, "GET %s -> no complete header", url);
                    }
                } else {
                    ESP_LOGW(kTag, "GET %s -> empty response", url);
                }
            }
        } else {
            ESP_LOGW(kTag, "connect %s:%d failed: errno %d", target.host,
                     target.port, errno);
        }
        ::close(fd);
    }
    ::freeaddrinfo(addr);
    return ok;
}

bool http_get(const char *url, char *response, std::size_t capacity,
              std::size_t *response_size) {
    if (!url || !response || capacity < 2) return false;
    response[0] = '\0';

    // 明文 HTTP 一律走裸 socket: 目录接口(ip-api / ipinfo / radio-browser)响应
    // 只有几 KB, 而 esp_http_client 的连续堆需求在碎片化的 C3 上就是"永远失败"。
    HttpTarget probe = {};
    if (parse_http_target(url, &probe)) {
        if (!raw_http_get(url, response, capacity, 8000)) return false;
        if (response_size) *response_size = std::strlen(response);
        return true;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 8000;
    // 2026-10-18: 2048 -> 512。这 2KB 连续块是 esp_http_client_init 失败的直接原因
    // (实测 WiFi 起来后 largest 常只剩 1728)。512B 时同样的碎片堆也能建起客户端,
    // 响应体由下面的循环分次读满, 只是 syscall 次数多一点。
    config.buffer_size = 512;
    config.buffer_size_tx = 256;
    config.user_agent = "AI-Passport-Radio/1.1.1";
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 3;
    if (starts_with(url, "https://")) config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    bool success = false;
    std::size_t used = 0;
    do {
        const esp_err_t opened = esp_http_client_open(client, 0);
        if (opened != ESP_OK) {
            // 不打印就永远查不出来: 实测定位失败全部来自这里(DNS/连接),
            // 上层只看到"Unable to determine network location"。
            ESP_LOGW(kTag, "open %s failed: %s", url, esp_err_to_name(opened));
            break;
        }
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(kTag, "GET %s returned HTTP %d", url, status);
            break;
        }

        while (used + 1 < capacity) {
            const int received = esp_http_client_read(
                client, response + used, static_cast<int>(capacity - used - 1));
            if (received < 0) {
                used = 0;
                break;
            }
            if (received == 0) {
                success = used > 0;
                break;
            }
            used += static_cast<std::size_t>(received);
        }
        if (used + 1 >= capacity) {
            ESP_LOGW(kTag, "Response from %s exceeded %u bytes", url,
                     static_cast<unsigned>(capacity));
            success = false;
        }
    } while (false);

    response[used] = '\0';
    if (response_size) *response_size = used;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
}

const char *json_string(cJSON *object, const char *name) {    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

void copy_text(char *target, std::size_t capacity, const char *source) {
    if (!target || capacity == 0) return;
    std::snprintf(target, capacity, "%s", source ? source : "");
}

// 单个定位服务读一次并解析。字段名两套服务不同, 由调用方给好映射。
struct LocationFields {
    const char *country;
    const char *region;
    const char *city;
};

bool parse_location_json(const char *response, const LocationFields &fields,
                         RadioLocation *location) {
    cJSON *root = cJSON_Parse(response);
    if (!root) return false;
    const char *country = json_string(root, fields.country);
    const char *region = json_string(root, fields.region);
    const char *city = json_string(root, fields.city);
    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "success");
    const bool service_ok =
        (cJSON_IsString(status) && std::strcmp(status->valuestring, "success") == 0) ||
        (cJSON_IsTrue(ok) && ok->valueint != 0);
    bool filled = false;
    if (service_ok && country[0] && (city[0] || region[0])) {
        copy_text(location->country_code, sizeof(location->country_code), country);
        copy_text(location->region, sizeof(location->region), region);
        copy_text(location->city, sizeof(location->city), city);
        filled = true;
    }
    cJSON_Delete(root);
    return filled;
}

// 2026-10-17: 定位是"只显示本地电台"的前提, 但实测**单次尝试经常失败**
// (真机日志: GET ip-api ... returned HTTP -1 -> 直接放弃 -> 用户看到的全是
// 全国兜底台)。HTTP -1 来自 DNS/连接瞬时抖动(堆碎片化时尤其明显), 重试一次
// 基本就能成, 所以这里把每个服务改成两次尝试, 中间留 600ms 让栈合并。
bool locate(RadioLocation *location) {
    // The prewarmed PCM buffer is idle during discovery; do not require a new
    // 2KB contiguous allocation when Wi-Fi has fragmented the heap.
    char *response = reinterpret_cast<char *>(radio_player_search_buffer());
    const bool borrowed = response != nullptr;
    if (!response) response = static_cast<char *>(std::malloc(2048));
    if (!response) return false;
    // 顺序很关键: ip-api 与 ipinfo 实测都返回 Chongqing(本机所在), 而
    // ipwho.is 实测把同一出口 IP 报成 Beijing —— 它会让用户看到别的城市的台,
    // 所以只作最后兜底。每个源重试两次, 单次失败(多为 DNS 抖动)不再直接放弃。
    struct Source {
        const char *url;
        LocationFields fields;
    };
    static constexpr Source kSources[] = {
        {kLocationUrl, {"countryCode", "regionName", "city"}},
        {kIpinfoLocationUrl, {"country", "region", "city"}},
        {kBackupLocationUrl, {"country_code", "region", "city"}},
    };
    bool success = false;
    for (const Source &source : kSources) {
        for (int attempt = 0; attempt < 2 && !success; ++attempt) {
            if (attempt) vTaskDelay(pdMS_TO_TICKS(600));
            if (!wait_http_heap(4000)) {
                ESP_LOGW(kTag, "no contiguous heap for location request");
                break;
            }
            if (!http_get(source.url, response, 2048, nullptr)) continue;
            success = parse_location_json(response, source.fields, location);
            if (success) {
                ESP_LOGI(kTag, "Location via %s (attempt %d)",
                         source.url, attempt + 1);
            }
        }
        if (success) break;
    }
    if (borrowed) radio_player_release_search_buffer();
    else std::free(response);
    return success;
}

void url_encode(const char *source, char *target, std::size_t capacity) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t out = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(source);
         p && *p && out + 1 < capacity; ++p) {
        const unsigned char value = *p;
        if (std::isalnum(value) || value == '-' || value == '_' || value == '.' ||
            value == '~') {
            target[out++] = static_cast<char>(value);
        } else if (out + 3 < capacity) {
            target[out++] = '%';
            target[out++] = kHex[value >> 4];
            target[out++] = kHex[value & 0x0F];
        } else {
            break;
        }
    }
    target[out] = '\0';
}

const char *translated_city(const RadioLocation &location) {
    if (std::strcmp(location.country_code, "CN") != 0) return location.city;
    for (const auto &entry : kChineseCities) {
        if (std::strcmp(entry.english, location.city) == 0) return entry.chinese;
    }
    return nullptr;
}

// 省/直辖市/自治区英文名 -> 中文名。用于城市搜不到时退化到"省份"搜索。
// ip-api 的 regionName 对直辖市返回同名(如重庆->"Chongqing"), 对省返回英文
// (如广东->"Guangdong"), 而 radio-browser 的 `state` 字段拼写混乱不可靠 ——
// 所以统一翻成中文, 用 name 搜索(实测命中率明显更高)。
constexpr CityTranslation kChineseProvinces[] = {
    {"Beijing", "北京"},       {"Tianjin", "天津"},
    {"Shanghai", "上海"},      {"Chongqing", "重庆"},
    {"Guangdong", "广东"},     {"Guangxi", "广西"},
    {"Zhejiang", "浙江"},      {"Jiangsu", "江苏"},
    {"Shandong", "山东"},      {"Henan", "河南"},
    {"Hebei", "河北"},         {"Shanxi", "山西"},
    {"Liaoning", "辽宁"},      {"Jilin", "吉林"},
    {"Heilongjiang", "黑龙江"}, {"Anhui", "安徽"},
    {"Fujian", "福建"},        {"Jiangxi", "江西"},
    {"Hubei", "湖北"},         {"Hunan", "湖南"},
    {"Sichuan", "四川"},       {"Guizhou", "贵州"},
    {"Yunnan", "云南"},        {"Shaanxi", "陕西"},
    {"Gansu", "甘肃"},         {"Qinghai", "青海"},
    {"Inner Mongolia", "内蒙古"}, {"Ningxia", "宁夏"},
    {"Xinjiang", "新疆"},      {"Tibet", "西藏"},
    {"Xizang", "西藏"},        {"Hainan", "海南"},
};

const char *translated_province(const RadioLocation &location) {
    if (std::strcmp(location.country_code, "CN") != 0) return nullptr;
    for (const auto &entry : kChineseProvinces) {
        if (std::strcmp(entry.english, location.region) == 0) return entry.chinese;
    }
    return nullptr;
}

// 中文城市名 -> 本省中文简称 + 目录里该省的旧式 state 拼写。radio-browser 的
// state 字段用邮政式/韦氏旧拼(重庆=Chungking, 广东=Kwangtung, 四川=Szechuan),
// 实测按旧拼查 state 能一次带出**全省含区县**的电台(重庆: 都市/永川/巴南/綦江/
// 北碚/大足), 而 name=<中文市名> 只有市名对得上的少数几条。两个查询合起来才够
// 一个地区的一屏频道。
bool ascii_only(const char *text) {
    if (!text || !text[0]) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        if (*p >= 0x80) return false;
    }
    return true;
}

struct CityRegion {
    const char *city;
    const char *province;
    const char *state;
};

constexpr CityRegion kCityRegions[] = {
    {"北京", "北京", "Beijing"},        {"上海", "上海", "Shanghai"},
    {"天津", "天津", "Tientsin"},       {"重庆", "重庆", "Chungking"},
    {"广州", "广东", "Kwangtung"},      {"深圳", "广东", "Kwangtung"},
    {"东莞", "广东", "Kwangtung"},      {"佛山", "广东", "Kwangtung"},
    {"珠海", "广东", "Kwangtung"},      {"汕头", "广东", "Kwangtung"},
    {"惠州", "广东", "Kwangtung"},      {"中山", "广东", "Kwangtung"},
    {"杭州", "浙江", "Zhejiang"},       {"宁波", "浙江", "Zhejiang"},
    {"温州", "浙江", "Chekiang"},       {"嘉兴", "浙江", "Zhejiang"},
    {"绍兴", "浙江", "Zhejiang"},       {"金华", "浙江", "Zhejiang"},
    {"南京", "江苏", "Kiangsu"},        {"苏州", "江苏", "Kiangsu"},
    {"无锡", "江苏", "Kiangsu"},        {"常州", "江苏", "Kiangsu"},
    {"南通", "江苏", "Kiangsu"},        {"徐州", "江苏", "Kiangsu"},
    {"成都", "四川", "Szechuan"},       {"绵阳", "四川", "Szechuan"},
    {"武汉", "湖北", "Hupei"},          {"宜昌", "湖北", "Hupei"},
    {"长沙", "湖南", "Hunan"},          {"郑州", "河南", "Honan"},
    {"洛阳", "河南", "Honan"},          {"济南", "山东", "Shantung"},
    {"青岛", "山东", "Shantung"},       {"烟台", "山东", "Shantung"},
    {"潍坊", "山东", "Shantung"},       {"沈阳", "辽宁", "Liaoning"},
    {"大连", "辽宁", "Liaoning"},       {"哈尔滨", "黑龙江", "Amur River"},
    {"长春", "吉林", "Jilin"},          {"石家庄", "河北", "Hopei"},
    {"太原", "山西", "Shansi"},         {"西安", "陕西", "Shensi"},
    {"昆明", "云南", "Yunnan"},         {"贵阳", "贵州", "Kweichow"},
    {"南宁", "广西", "Kwangsi"},        {"海口", "海南", "Hainan"},
    {"福州", "福建", "Fukien"},         {"厦门", "福建", "Fukien"},
    {"南昌", "江西", "Kiangsi"},        {"合肥", "安徽", "Anhwei"},
    {"兰州", "甘肃", "Kansu"},          {"西宁", "青海", "Tsinghai"},
    {"银川", "宁夏", "Ningsia"},        {"呼和浩特", "内蒙古", "Inner Mongolia"},
    {"乌鲁木齐", "新疆", "Sinkiang"},   {"拉萨", "西藏", "Tibet"},
};

const CityRegion *region_for_city(const char *city) {
    if (!city || !city[0]) return nullptr;
    for (const auto &entry : kCityRegions) {
        if (std::strcmp(entry.city, city) == 0) return &entry;
    }
    return nullptr;
}

// 中文省名 -> 目录里的旧式 state 拼写。手选城市直接查表; 自动定位拿到的是英文
// region, 先经 kChineseProvinces 翻成中文省名再查这里。
const char *legacy_state_for_province(const char *province) {
    if (!province || !province[0]) return nullptr;
    for (const auto &entry : kCityRegions) {
        if (std::strcmp(entry.province, province) == 0) return entry.state;
    }
    return nullptr;
}

// 本次查询的地理键: 中文市名、中文省名、本省旧式 state 拼写。为空表示该维度
// 不可用, 区域过滤随之放宽(海外定位或非中文城市名时不做名字过滤)。
struct RegionKeys {
    const char *city = nullptr;
    const char *province = nullptr;
    const char *state = nullptr;
    bool filtered() const { return city != nullptr || province != nullptr; }
};

RegionKeys region_keys(const RadioLocation &location) {
    RegionKeys keys;
    const char *city = translated_city(location);
    if ((!city || !city[0]) && location.city[0]) city = location.city;
    if (city && ascii_only(city)) city = nullptr;   // 英文市名对不上中文台名
    keys.city = city;
    keys.province = translated_province(location);
    if (keys.province && !keys.province[0]) keys.province = nullptr;
    const CityRegion *manual = region_for_city(city);
    if (manual) {
        if (!keys.province) keys.province = manual->province;
        keys.state = manual->state;
    } else if (keys.province) {
        keys.state = legacy_state_for_province(keys.province);
    }
    return keys;
}

// 目录的 state 字段拼写不统一("Chungking" / "chongqing" / "Szechuan"), 且偶尔
// 写成 "Guangdong,Shenzhen" 这样带市名的列表。这里只比较整段中的某一项。
bool matches_state(const char *entry_state, const char *wanted) {
    if (!entry_state || !wanted) return false;
    const char *token = entry_state;
    while (true) {
        while (token[0] == ' ' || token[0] == ',') ++token;
        const char *end_token = token;
        while (end_token[0] && end_token[0] != ',') ++end_token;
        std::size_t length = static_cast<std::size_t>(end_token - token);
        while (length > 0 && (token[length - 1] == ' ' || token[length - 1] == '\r' ||
                              token[length - 1] == '\n')) {
            --length;
        }
        if (length == std::strlen(wanted)) {
            bool same = true;
            for (std::size_t i = 0; i < length; ++i) {
                if (std::tolower(static_cast<unsigned char>(token[i])) !=
                    std::tolower(static_cast<unsigned char>(wanted[i]))) {
                    same = false;
                    break;
                }
            }
            if (same) return true;
        }
        if (!end_token[0]) return false;
        token = end_token;
    }
}

void normalize_stream_url(const char *source, char *target, std::size_t capacity) {
    // v1.2.17: https 流在 13KB 堆上必然挤压 LVGL(mbedtls 上下文 5-6KB),
    // 目录来源一律降级为 http。以下域名均已在 PC 端逐台实测 http 200:
    // 蜻蜓(qtfm/qingting)、听云(tyradio)、infomaniak、dribbcast。
    if (starts_with(source, "https://") &&
        (contains(source, ".qtfm.cn/") || contains(source, ".qingting.fm/") ||
         contains(source, "tyradio.uk") || contains(source, ".ice.infomaniak.ch") ||
         contains(source, "dribbcast.com"))) {
        std::snprintf(target, capacity, "http://%s", source + 8);
    } else {
        copy_text(target, capacity, source);
    }
}

bool duplicate_url(const RadioStation *stations, std::size_t count, const char *url) {
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(stations[i].url, url) == 0) return true;
    }
    return false;
}

// 蜻蜓/青听的同一路直播流有多个 CDN 域名(lhttp / lhttp-hw / https), 目录里会
// 作为不同电台出现, 完整 URL 判重拦不住。取 /live/<id>/ 里的 id 作为流身份。
bool live_stream_key(const char *url, char *key, std::size_t capacity) {
    const char *mark = std::strstr(url, "/live/");
    if (!mark) return false;
    mark += 6;
    std::size_t length = 0;
    while (mark[length] >= '0' && mark[length] <= '9' && length + 1 < capacity) {
        key[length] = mark[length];
        ++length;
    }
    if (length == 0) return false;
    key[length] = '\0';
    return true;
}

bool duplicate_stream(const RadioStation *stations, std::size_t count, const char *url) {
    char candidate[16] = {};
    if (!live_stream_key(url, candidate, sizeof(candidate))) return false;
    for (std::size_t i = 0; i < count; ++i) {
        char existing[16] = {};
        if (live_stream_key(stations[i].url, existing, sizeof(existing)) &&
            std::strcmp(existing, candidate) == 0) {
            return true;
        }
    }
    return false;
}

// 卫星电视频道的伴音流会被目录当成电台收录, 它们不是 MP3 广播。
bool is_tv_companion(const char *name) {
    static const char *const kMarks[] = {
        "卫视",
        "电视台",
        "伴音",
    };
    for (const char *mark : kMarks) {
        if (contains(name, mark)) return true;
    }
    return false;
}


// Chinese FM broadcasting occupies 87.0-108.0 MHz. Station names in the public
// directory spell the frequency in three ways, so they are tried in order of
// confidence: an explicit "FM" prefix, a bare decimal such as "94.7", and
// finally a compact form such as "971" that stations use as a brand. The last
// form is the least reliable, so it is only accepted when the digits are not
// glued to other numbers and still land inside the broadcast band.
constexpr uint16_t kBandLowDecihz = 870;
constexpr uint16_t kBandHighDecihz = 1080;

bool in_band(long decihz) {
    return decihz >= kBandLowDecihz && decihz <= kBandHighDecihz;
}

bool is_digit(char value) { return value >= '0' && value <= '9'; }

// Reads "94.7" or "1017" style digits at `text` and returns tenths of a MHz.
// Digit runs without a decimal point are ambiguous: "971" is 97.1 MHz while
// "101" is 101.0 MHz. Both readings are tried and the one inside the broadcast
// band wins, checking the tenths reading first because it is far more common.
long read_decihz(const char *text, std::size_t *consumed) {
    std::size_t whole = 0;
    while (is_digit(text[whole])) ++whole;
    if (whole < 2 || whole > 4) return 0;

    long value = 0;
    for (std::size_t i = 0; i < whole; ++i) value = value * 10 + (text[i] - '0');

    if (text[whole] == '.' && is_digit(text[whole + 1])) {
        if (whole > 3 || is_digit(text[whole + 2])) return 0;
        *consumed = whole + 2;
        return value * 10 + (text[whole + 1] - '0');
    }

    *consumed = whole;
    if (whole == 2) return value * 10;      // "94" is 94.0 MHz
    if (in_band(value)) return value;       // "971" is 97.1 MHz
    if (in_band(value * 10)) return value * 10;  // "101" is 101.0 MHz
    return value;
}

uint16_t parse_frequency(const char *name) {
    if (!name) return 0;

    for (const char *p = name; p[0] && p[1]; ++p) {
        if ((p[0] != 'F' && p[0] != 'f') || (p[1] != 'M' && p[1] != 'm')) continue;
        const char *digits = p + 2;
        while (*digits == ' ') ++digits;
        std::size_t consumed = 0;
        const long value = read_decihz(digits, &consumed);
        if (in_band(value)) return static_cast<uint16_t>(value);
    }

    for (const char *p = name; *p; ++p) {
        if (!is_digit(*p)) continue;
        const bool starts_run = p == name || (!is_digit(p[-1]) && p[-1] != '.');
        if (!starts_run) continue;
        std::size_t consumed = 0;
        const long value = read_decihz(p, &consumed);
        if (consumed && in_band(value)) return static_cast<uint16_t>(value);
        while (is_digit(p[1])) ++p;
    }
    return 0;
}

void make_display_name(char *target, std::size_t capacity, const char *raw_name,
                       const char *city, std::size_t sequence) {
    if (ascii_only(raw_name)) {
        copy_text(target, capacity, raw_name);
        return;
    }

    const char *frequency = nullptr;
    for (const char *p = raw_name; p && p[0] && p[1]; ++p) {
        if ((p[0] == 'F' || p[0] == 'f') && (p[1] == 'M' || p[1] == 'm')) {
            frequency = p;
            break;
        }
    }
    if (frequency) {
        char suffix[20] = {};
        std::size_t length = 0;
        while (frequency[length] && length + 1 < sizeof(suffix)) {
            const unsigned char value = static_cast<unsigned char>(frequency[length]);
            if (value >= 0x80 || (!std::isalnum(value) && value != '.' && value != '-' &&
                                  value != ' ')) {
                break;
            }
            suffix[length] = frequency[length];
            ++length;
        }
        while (length > 0 && suffix[length - 1] == ' ') suffix[--length] = '\0';
        if (length >= 2) {
            std::snprintf(target, capacity, "%s %s", city, suffix);
            return;
        }
    }
    std::snprintf(target, capacity, "%s RADIO %u", city,
                  static_cast<unsigned>(sequence + 1));
}

std::size_t parse_stations(const char *json, const RadioLocation &location,
                           RadioStation *stations, std::size_t capacity) {
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return 0;
    }

    std::size_t count = 0;
    cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, root) {
        if (count >= capacity || !cJSON_IsObject(entry)) break;
        const char *raw_name = json_string(entry, "name");
        const char *raw_url = json_string(entry, "url_resolved");
        if (!raw_url[0]) raw_url = json_string(entry, "url");
        cJSON *last_ok = cJSON_GetObjectItemCaseSensitive(entry, "lastcheckok");
        cJSON *hls = cJSON_GetObjectItemCaseSensitive(entry, "hls");
        if (!raw_name[0] || !raw_url[0] || !contains(raw_url, ".mp3") ||
            (cJSON_IsNumber(last_ok) && last_ok->valueint != 1) ||
            (cJSON_IsNumber(hls) && hls->valueint != 0)) {
            continue;
        }

        // 外地台剔除: 台名既不含本市也不含本省的一律丢弃。用户明确要求
        // "选了定位之后只展示那个地方的频道", 实测重庆列表里混进的广东地方台
        // 全部来自这条路径(name 查询命中的同名跨省台)。
        const RegionKeys keys = region_keys(location);
        if (keys.filtered() && !location.national_fallback) {
            const bool local = contains(raw_name, keys.city) ||
                               contains(raw_name, keys.province);
            // state 查询会带回全省台与邻省台: 台名认不出本地时, 再认条目自带的
            // state 字段(目录里区县台通常挂本省旧拼)。
            const char *entry_state = json_string(entry, "state");
            const bool tagged = keys.state && entry_state[0] &&
                                matches_state(entry_state, keys.state);
            if (!local && !tagged) continue;
        }
        // 卫视频道的伴音流会被目录当成电台收录, 它们不是可播的 MP3 广播。
        if (is_tv_companion(raw_name)) {
            continue;
        }

        RadioStation candidate = {};
        normalize_stream_url(raw_url, candidate.url, sizeof(candidate.url));
        if ((!starts_with(candidate.url, "http://") &&
             !starts_with(candidate.url, "https://")) ||
            duplicate_url(stations, count, candidate.url) ||
            duplicate_stream(stations, count, candidate.url)) {
            continue;
        }
        candidate.frequency_decihz = parse_frequency(raw_name);
        copy_text(candidate.name, sizeof(candidate.name), raw_name);
        copy_text(candidate.description, sizeof(candidate.description),
                  "城市电台 · 网络直播");
        // 注意: 这里**不做**运行时首字节探测。曾试过在入库前逐条 probe, 但
        // esp_http_client 每次分配较大缓冲, catalog 任务(栈 6144)连续探测会让
        // 堆从 ~50KB 掉到 ~6KB 并触发 wifi:mem fail —— 代价远大于收益。
        // 改由两点保证质量: ① radio_player 播放时的首字节校验(判死即退避换台);
        // ② kFallbackStations 全部为实测可播流, 且排在列表尾部兜底。
        stations[count++] = candidate;
    }
    cJSON_Delete(root);
    return count;
}

std::size_t query_directory(const RadioLocation &location, const char *field,
                            const char *value, RadioStation *stations,
                            std::size_t capacity) {
    char country[16] = {};
    char encoded[192] = {};
    url_encode(location.country_code, country, sizeof(country));
    url_encode(value, encoded, sizeof(encoded));
    char url[512] = {};
    // limit 收敛到 6: 实测 national limit=6 响应 6747B < 8192 缓冲;
    // limit=8 时 9046B 会超限(被 http_get 判失败)。
    char *response = reinterpret_cast<char *>(radio_player_search_buffer());
    const bool borrowed = response != nullptr;
    if (!response) response = static_cast<char *>(std::malloc(kResponseCapacity));
    if (!response) return 0;
    // 借用池只有 6656B, 按 8192B 声明会让 http_get 越界写坏相邻内存。
    const std::size_t cap = response_capacity(borrowed);
    std::size_t count = 0;
    for (unsigned offset = 0; offset < 6 && count < capacity; offset += 2) {
        if (!wait_http_heap(4000)) {
            ESP_LOGW(kTag, "no contiguous heap for HTTP client (field=%s)", field);
            break;
        }
        std::snprintf(url, sizeof(url),
                      "%s?countrycode=%s&%s=%s&codec=MP3&hidebroken=true"
                      "&order=clickcount&reverse=true&limit=2&offset=%u",
                      kDirectoryBase, country, field, encoded, offset);
        if (!http_get(url, response, cap, nullptr)) break;
        const std::size_t added = parse_stations(response, location, stations + count,
                                                capacity - count);
        count += added;
    }
    if (borrowed) radio_player_release_search_buffer();
    else std::free(response);
    return count;
}

// 全国热门(不加 name/state 过滤, 仅按国家 + 点击量)。用于本地搜索落空时的兜底。
std::size_t query_national_top(const RadioLocation &location, RadioStation *stations,
                               std::size_t capacity) {
    char country[16] = {};
    url_encode(location.country_code, country, sizeof(country));
    char url[320] = {};
    char *response = reinterpret_cast<char *>(radio_player_search_buffer());
    const bool borrowed = response != nullptr;
    if (!response) response = static_cast<char *>(std::malloc(kResponseCapacity));
    if (!response) return 0;
    const std::size_t cap = response_capacity(borrowed);
    std::size_t count = 0;
    for (unsigned offset = 0; offset < 6 && count < capacity; offset += 2) {
        if (!wait_http_heap(4000)) {
            ESP_LOGW(kTag, "no contiguous heap for HTTP client (national)");
            break;
        }
        std::snprintf(url, sizeof(url),
                      "%s?countrycode=%s&codec=MP3&hidebroken=true"
                      "&order=clickcount&reverse=true&limit=2&offset=%u",
                      kDirectoryBase, country, offset);
        if (!http_get(url, response, cap, nullptr)) break;
        const std::size_t added = parse_stations(response, location, stations + count,
                                                capacity - count);
        count += added;
    }
    if (borrowed) radio_player_release_search_buffer();
    else std::free(response);
    return count;
}

}  // namespace

std::size_t radio_catalog_city_count() {
    return sizeof(kCityRegions) / sizeof(kCityRegions[0]);
}

const char *radio_catalog_city_name(std::size_t index) {
    if (index >= radio_catalog_city_count()) return "";
    return kCityRegions[index].city;
}


std::size_t radio_catalog_discover(RadioStation *stations, std::size_t capacity,
                                   RadioLocation *location) {
    if (!stations || capacity == 0 || !location) return 0;
    *location = {};
    if (!locate(location)) {
        ESP_LOGW(kTag, "Unable to determine network location");
        return 0;
    }
    ESP_LOGI(kTag, "Detected location: %s, %s, %s", location->city,
             location->region, location->country_code);

    std::size_t count = 0;
    const RegionKeys keys = region_keys(*location);
    if (keys.city) {
        count = query_directory(*location, "name", keys.city, stations, capacity);
        ESP_LOGI(kTag, "City search '%s' returned %u playable stations", keys.city,
                 static_cast<unsigned>(count));
    } else if (std::strcmp(location->country_code, "CN") != 0 && location->city[0]) {
        count = query_directory(*location, "name", location->city, stations, capacity);
        ESP_LOGI(kTag, "City search '%s' returned %u playable stations", location->city,
                 static_cast<unsigned>(count));
    }
    // 市名查询常常只有 1~2 条(区县台台名里不带市名, 如"永川之声""北碚综合广播")。
    // 用本省旧式 state 再查一次, 能把同省区县台一次带全。
    if (count < kThinResultCount && count < capacity && keys.state && keys.state[0]) {
        count += query_directory(*location, "state", keys.state,
                                 stations + count, capacity - count);
        ESP_LOGI(kTag, "Province state '%s' now totals %u playable stations",
                 keys.state, static_cast<unsigned>(count));
    }

    // 城市搜不到时的退化路径。
    // ⚠ radio-browser 的 `state` 字段拼写极不一致(实测重庆同时存在 'Chungking'
    // (邮政式旧拼) / 'chongqing'(小写) / ''(空) / 甚至错标成 'Shanghai'),
    // 而 ip-api 返回的 region 是英文 'Chongqing' —— 两者对不上, 所以
    // `state=<英文region>` 实测对多数省份恒为 0(仅 Beijing/Shanghai 等少数命中)。
    // 因此这里改为: 用**中文省名**做 name 搜索(实测 广东 6 条 / 四川 6 条 /
    // 湖南 1 条, 明显优于英文 state), 再退回英文 state 作最后尝试。
    if (count == 0) {
        const char *province_cn = keys.province;
        if (province_cn && province_cn[0]) {
            count = query_directory(*location, "name", province_cn, stations, capacity);
            if (count > 0) {
                ESP_LOGI(kTag, "Province(name='%s') returned %u playable stations",
                         province_cn, static_cast<unsigned>(count));
            }
        }
        if (count == 0 && location->region[0]) {
            count = query_directory(*location, "state", location->region, stations, capacity);
            ESP_LOGI(kTag, "Region search '%s' returned %u playable stations",
                     location->region, static_cast<unsigned>(count));
        }
    }

    // 兜底: 定位成功但本地搜不到(常见 —— 该城市的电台恰好都是加密/防盗链流,
    // 被 parse_stations 的首字节与 .mp3 过滤剔除, 如重庆)。此时退化为"全国热门"
    // (仅按 countrycode + 点击量, 不加 name 过滤), 至少让用户有台可听。
    if (count == 0) {
        location->national_fallback = true;
        count = query_national_top(*location, stations, capacity);
        ESP_LOGW(kTag, "Local search empty, fallback to national top: %u stations",
                 static_cast<unsigned>(count));
    }
    return count;
}

std::size_t radio_catalog_discover_city(const char *city, RadioStation *stations,
                                        std::size_t capacity,
                                        RadioLocation *location) {
    if (!city || !city[0] || !stations || capacity == 0 || !location) return 0;
    *location = {};
    copy_text(location->country_code, sizeof(location->country_code), "CN");
    copy_text(location->city, sizeof(location->city), city);
    copy_text(location->region, sizeof(location->region), city);

    std::size_t count =
        query_directory(*location, "name", city, stations, capacity);
    ESP_LOGI(kTag, "Manual city search '%s' returned %u playable stations", city,
             static_cast<unsigned>(count));
    const RegionKeys keys = region_keys(*location);
    if (count < kThinResultCount && count < capacity && keys.state && keys.state[0]) {
        count += query_directory(*location, "state", keys.state,
                                 stations + count, capacity - count);
        ESP_LOGI(kTag, "Manual province state '%s' now totals %u stations",
                 keys.state, static_cast<unsigned>(count));
    }
    // 手动选城市也要兜底: 若该城市在目录里没有可播台(实测 杭州 等确实为 0),
    // 退回"全国热门", 否则用户选完会看到空列表。
    if (count == 0) {
        location->national_fallback = true;
        count = query_national_top(*location, stations, capacity);
        ESP_LOGW(kTag, "Manual city '%s' empty, fallback to national top: %u stations",
                 city, static_cast<unsigned>(count));
    }
    return count;
}
