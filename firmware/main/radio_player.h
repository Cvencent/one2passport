#pragma once

#include <cstddef>
#include <cstdint>

struct RadioStation {
    char name[64];
    char description[96];
    char url[256];
    // Real FM frequency in units of 0.1 MHz (for example 947 means 94.7 MHz).
    // Zero means the directory entry carried no usable frequency, and the dial
    // falls back to spreading the station across the scale by its position.
    uint16_t frequency_decihz;
};

enum class RadioPlaybackState {
    Stopped,
    Connecting,
    Buffering,
    Playing,
    Reconnecting,
    Error,
};

bool radio_player_init();
// 启动早期调用(堆最大时): 预分配 MP3 解码器与收发缓冲, 之后切台复用。
// C3 上 WiFi 连接后堆碎片化, 现场分配解码器会失败 -> 收音机无声。
bool radio_player_prewarm(void);
void radio_player_release_pool(void);   // 进小智等大内存应用前释放
void radio_player_ensure_pool(void);    // 退出后重新抢占
void radio_player_rewake_after_pool_ready(void);  // 池就绪后唤醒播放任务
bool radio_player_has_pool();           // 首页最小化服务仅在有池时恢复播放

// 取电台目录搜索用的 8KB 预分配缓冲(开机时随解码器池一起抢下)。
// 返回 nullptr 表示尚未预分配, 调用方应回退到自己 malloc。
// 目的: 让"城市搜索"无需释放解码器池就能拿到连续块(2026-10-10 发热事故修复)。
uint8_t *radio_player_search_buffer(void);
std::size_t radio_player_search_buffer_size(void);
void radio_player_release_search_buffer(void);
struct RadioCatalogPause {
    bool was_playing;
    uint32_t revision;
};
RadioCatalogPause radio_player_pause_for_catalog(void);
void radio_player_resume_after_catalog(RadioCatalogPause pause);
void radio_player_set_network(bool connected);
// 播放状态变化时切换 WiFi 省电级别。实现位于 main.cc，属于播放器的宿主
// 钩子；radio_player.cc 只依赖这个声明。
void radio_wifi_set_streaming(bool streaming);
void radio_player_toggle();
void radio_player_set_playing(bool playing);
bool radio_player_is_playing();
// 首页频谱条的实时音量级 0-1000(播放循环每块发布)。
uint16_t radio_player_eq_level();
bool radio_player_wait_idle(uint32_t timeout_ms);
// 播放任务当前是否正在 stream_station 内(持有 input/output 缓冲)。
// 释放内存池前必须确认为 false, 否则 stream_station 会 use-after-free。
bool radio_player_is_stream_active();
void radio_player_select_relative(int delta);
void radio_player_set_station(std::size_t index);
std::size_t radio_player_station_index();
std::size_t radio_player_station_count();
bool radio_player_has_cached_catalog();
// 作废电台目录缓存(内存 + NVS), 供城市变更后强制重新搜索。
void radio_player_invalidate_catalog_cache();
RadioStation radio_player_station(std::size_t index);
bool radio_player_replace_stations(const RadioStation *stations, std::size_t count);
// 选城市时从实测白名单预设表加载(城市名->省/市组); 无匹配返回 false。
bool radio_player_load_city_preset(const char *city);
// Installs a catalog and keeps listening to the station the user last chose
// when the new catalog still offers it, so a restart resumes where it left off.
bool radio_player_replace_stations_resuming(const RadioStation *stations,
                                           std::size_t count);
void radio_player_set_volume(uint8_t volume);
uint8_t radio_player_volume();
