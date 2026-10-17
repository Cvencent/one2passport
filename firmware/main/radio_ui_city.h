#pragma once
// 城市电台选择表的共享访问接口。定义在 main.cc, 数据源是 radio_catalog.cc 的
// kCityRegions(城市 -> 本省简称 -> 目录旧式 state 拼写), 因此"UI 显示的顺序"、
//"按键选中的索引"和"实际发出去查询的键"永远是同一份表。
#include <cstddef>

std::size_t city_choice_count();
const char *city_query_for(std::size_t index);
const char *city_display_for(std::size_t index);
