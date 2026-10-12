#include "radio_display_settings.h"

#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <atomic>

namespace {

constexpr char kTag[] = "display_settings";
constexpr char kNvsNamespace[] = "radio_display";
constexpr uint8_t kBrightnessChoices[] = {25, 50, 72, 100};
constexpr uint16_t kTimeoutChoices[] = {0, 30, 60, 300, 600};

std::atomic<uint8_t> s_brightness{72};
std::atomic<uint16_t> s_timeout_seconds{0};
std::atomic<TickType_t> s_last_activity{0};
std::atomic<bool> s_screen_on{true};
// Codex 页的"保持常亮"请求。置位时不触发息屏,也不改变用户设置的 timeout。
std::atomic<bool> s_keep_awake{false};
TaskHandle_t s_blank_task;

template <typename T, std::size_t N>
std::size_t choice_index(const T (&choices)[N], T value, std::size_t fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (choices[i] == value) return i;
    }
    return fallback;
}

void save_u8(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, key, value) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

void save_u16(const char *key, uint16_t value) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u16(handle, key, value) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

void blank_task(void *) {
    while (true) {
        const uint16_t timeout = s_timeout_seconds.load(std::memory_order_acquire);
        // Codex 页要求常亮时直接跳过:每次循环都会刷新活动时间戳,
        // 免得解除常亮后"欠"了一大截时长导致立即息屏。
        if (s_keep_awake.load(std::memory_order_acquire)) {
            s_last_activity.store(xTaskGetTickCount(), std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (timeout && s_screen_on.load(std::memory_order_acquire)) {
            const TickType_t elapsed =
                xTaskGetTickCount() - s_last_activity.load(std::memory_order_acquire);
            if (elapsed >= pdMS_TO_TICKS(static_cast<uint32_t>(timeout) * 1000U)) {
                s_screen_on.store(false, std::memory_order_release);
                bsp_display_backlight(0);
                ESP_LOGI(kTag, "Screen blanked after %u seconds",
                         static_cast<unsigned>(timeout));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

}  // namespace

void radio_display_set_keep_awake(bool keep_awake) {
    s_keep_awake.store(keep_awake, std::memory_order_release);
}

bool radio_display_settings_init() {
    uint8_t brightness = 72;
    uint16_t timeout = 0;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "brightness", &brightness);
        nvs_get_u16(handle, "timeout", &timeout);
        nvs_close(handle);
    }

    const std::size_t brightness_index =
        choice_index(kBrightnessChoices, brightness, 2);
    const std::size_t timeout_index = choice_index(kTimeoutChoices, timeout, 0);
    s_brightness.store(kBrightnessChoices[brightness_index], std::memory_order_release);
    s_timeout_seconds.store(kTimeoutChoices[timeout_index], std::memory_order_release);
    s_last_activity.store(xTaskGetTickCount(), std::memory_order_release);
    s_screen_on.store(true, std::memory_order_release);
    bsp_display_backlight(kBrightnessChoices[brightness_index]);

    if (s_blank_task) return true;
    if (xTaskCreate(blank_task, "radio_screen", 2048, nullptr, 3,
                    &s_blank_task) != pdPASS) {
        s_blank_task = nullptr;
        return false;
    }
    return true;
}

bool radio_display_note_activity() {
    s_last_activity.store(xTaskGetTickCount(), std::memory_order_release);
    const bool was_on = s_screen_on.exchange(true, std::memory_order_acq_rel);
    if (!was_on) {
        bsp_display_backlight(s_brightness.load(std::memory_order_acquire));
        ESP_LOGI(kTag, "screen woke by activity");
    }
    return was_on;
}

// 本次按键是否只是"把息屏的屏幕点亮"。
//
// ⚠ 曾经的 bug: input_task 写成 if (!radio_display_note_activity()) continue;,
//   而 note_activity() 返回的是"屏幕之前是否已经亮着"。于是息屏后第一次按键
//   返回 false -> 被 continue 吞掉 -> 屏幕亮了但按键没生效, 用户只按一下就
//   感觉"唤不醒"。现在改成显式的两段语义, 不再靠取反猜。
bool radio_display_was_blanked() {
    return !s_screen_on.load(std::memory_order_acquire);
}

// 唤醒后给 UI 一点"别立刻再息屏"的缓冲: 用户唤醒后往往马上要继续操作,
// 若沿用息屏前剩余的那点计时, 屏幕会在几秒内二次熄灭, 同样像"唤不醒"。
void radio_display_grace_after_wake(uint32_t ms) {
    const TickType_t grace = pdMS_TO_TICKS(ms);
    const TickType_t now = xTaskGetTickCount();
    const TickType_t last = s_last_activity.load(std::memory_order_acquire);
    if ((now - last) < grace) {
        // 把活动时间戳往回拨, 等价于"刚刚才有过活动"。
        s_last_activity.store(now - grace, std::memory_order_release);
    }
}

uint8_t radio_display_brightness() {
    return s_brightness.load(std::memory_order_acquire);
}

std::size_t radio_display_brightness_selection() {
    return choice_index(kBrightnessChoices, radio_display_brightness(), 2);
}

void radio_display_set_brightness_selection(std::size_t selection) {
    selection %= sizeof(kBrightnessChoices) / sizeof(kBrightnessChoices[0]);
    const uint8_t brightness = kBrightnessChoices[selection];
    s_brightness.store(brightness, std::memory_order_release);
    s_last_activity.store(xTaskGetTickCount(), std::memory_order_release);
    s_screen_on.store(true, std::memory_order_release);
    bsp_display_backlight(brightness);
    save_u8("brightness", brightness);
}

uint16_t radio_display_timeout_seconds() {
    return s_timeout_seconds.load(std::memory_order_acquire);
}

std::size_t radio_display_timeout_selection() {
    return choice_index(kTimeoutChoices, radio_display_timeout_seconds(), 0);
}

void radio_display_set_timeout_selection(std::size_t selection) {
    selection %= sizeof(kTimeoutChoices) / sizeof(kTimeoutChoices[0]);
    const uint16_t timeout = kTimeoutChoices[selection];
    s_timeout_seconds.store(timeout, std::memory_order_release);
    s_last_activity.store(xTaskGetTickCount(), std::memory_order_release);
    save_u16("timeout", timeout);
}

