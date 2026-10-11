#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// One network producer and one decoder consumer. Small separately allocated
// blocks avoid requiring a large contiguous allocation on the ESP32-C3.
class RadioStreamBuffer {
public:
    RadioStreamBuffer(uint8_t **blocks, std::size_t count, std::size_t block_size)
        : blocks_(blocks), capacity_(count * block_size), block_size_(block_size) {}

    std::size_t available() const {
        return written_.load(std::memory_order_acquire) -
               read_.load(std::memory_order_acquire);
    }

    std::size_t write(const uint8_t *data, std::size_t length) {
        const uint32_t written = written_.load(std::memory_order_relaxed);
        const uint32_t read = read_.load(std::memory_order_acquire);
        const std::size_t count = std::min<std::size_t>(length, capacity_ - (written - read));
        copy(data, nullptr, write_position_, count);
        write_position_ = (write_position_ + count) % capacity_;
        written_.store(written + count, std::memory_order_release);
        return count;
    }

    std::size_t read(uint8_t *data, std::size_t length) {
        const uint32_t read = read_.load(std::memory_order_relaxed);
        const uint32_t written = written_.load(std::memory_order_acquire);
        const std::size_t count = std::min<std::size_t>(length, written - read);
        copy(nullptr, data, read_position_, count);
        read_position_ = (read_position_ + count) % capacity_;
        read_.store(read + count, std::memory_order_release);
        return count;
    }

private:
    void copy(const uint8_t *source, uint8_t *target, std::size_t position,
              std::size_t length) {
        std::size_t copied = 0;
        while (copied < length) {
            const std::size_t index = position / block_size_;
            const std::size_t offset = position % block_size_;
            const std::size_t count = std::min(length - copied, block_size_ - offset);
            if (source) std::memcpy(blocks_[index] + offset, source + copied, count);
            else std::memcpy(target + copied, blocks_[index] + offset, count);
            copied += count;
            position = (position + count) % capacity_;
        }
    }

    uint8_t **blocks_;
    const std::size_t capacity_, block_size_;
    std::size_t write_position_ = 0, read_position_ = 0;
    std::atomic<uint32_t> written_{0}, read_{0};
};
