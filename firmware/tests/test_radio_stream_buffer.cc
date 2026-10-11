#include "radio_stream_buffer.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <thread>

int main() {
    uint8_t blocks[3][4] = {};
    uint8_t *storage[] = {blocks[0], blocks[1], blocks[2]};
    RadioStreamBuffer ring(storage, 3, 4);
    uint8_t out[16] = {};
    assert(ring.read(out, sizeof(out)) == 0);
    assert(ring.write(reinterpret_cast<const uint8_t *>("abcdefghij"), 10) == 10);
    assert(ring.write(reinterpret_cast<const uint8_t *>("XYZ"), 3) == 2);
    assert(ring.read(out, 7) == 7);
    assert(std::memcmp(out, "abcdefg", 7) == 0);
    assert(ring.write(reinterpret_cast<const uint8_t *>("1234567"), 7) == 7);
    assert(ring.read(out, 12) == 12);
    assert(std::memcmp(out, "hijXY1234567", 12) == 0);
    assert(ring.available() == 0);

    // A slow producer cannot reorder or lose bytes when the consumer wraps.
    std::thread producer([&] {
        for (unsigned i = 0; i < 100000; ++i) {
            uint8_t value = i % 251;
            while (ring.write(&value, 1) == 0) std::this_thread::yield();
        }
    });
    for (unsigned i = 0; i < 100000; ++i) {
        uint8_t value;
        while (ring.read(&value, 1) == 0) std::this_thread::yield();
        assert(value == i % 251);
    }
    producer.join();
    assert(ring.available() == 0);
}
