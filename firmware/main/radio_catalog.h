#pragma once

#include "radio_player.h"

#include <cstddef>

struct RadioLocation {
    char country_code[4];
    char region[48];
    char city[48];
    bool national_fallback;
};

// Locates the radio from its public network address and loads playable MP3
// stations for the detected city. If the directory has no city result, it
// automatically falls back to the surrounding region/province.
std::size_t radio_catalog_discover(RadioStation *stations, std::size_t capacity,
                                   RadioLocation *location);

// Loads the most popular playable stations for a user-selected Chinese city.
// This skips IP geolocation and is used by the on-device city selector.
std::size_t radio_catalog_discover_city(const char *city, RadioStation *stations,
                                        std::size_t capacity,
                                        RadioLocation *location);

// 城市电台选择表的唯一来源(城市名 -> 本省简称 -> 目录旧式 state 拼写)。
// UI 的分页列表与查询用的键都从这里取, 避免"UI 显示 A 城市、实际搜 B 城市"
// 这类表序错位的老问题。
std::size_t radio_catalog_city_count();
const char *radio_catalog_city_name(std::size_t index);

// 轻量 IP 定位(不做目录搜索)。供自动定位先拿省份再命中省级预设。
bool radio_catalog_locate(RadioLocation *location);

// IP 定位结果的中文省份名(CN 且在翻译表内); 海外/未知返回 nullptr。
// 供自动定位优先命中省级预设表使用, 避开在线目录搜索的堆碎片化。
const char *radio_catalog_province_cn(const RadioLocation *location);
