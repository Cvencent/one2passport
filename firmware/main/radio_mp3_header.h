#pragma once

#include <cstdint>

inline bool radio_mp3_header_valid(uint8_t first, uint8_t second, uint8_t third) {
    return first == 0xff && (second & 0xe0) == 0xe0 &&
           (second & 0x18) != 0x08 && (second & 0x06) == 0x02 &&
           (third & 0xf0) != 0 && (third & 0xf0) != 0xf0 &&
           (third & 0x0c) != 0x0c;
}
