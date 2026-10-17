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
