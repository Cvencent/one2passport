#pragma once
#include <cstddef>
#include <cstdint>
inline bool radio_retry_cancelled(bool wanted, bool connected,
                                  uint32_t expected_generation,
                                  uint32_t current_generation,
                                  std::size_t expected_station,
                                  std::size_t current_station) {
    return !wanted || !connected || expected_generation != current_generation ||
           expected_station != current_station;
}
