// main/fap_screenshot.c -- FAP_SCREENSHOT_V1 串口截屏协议(社区发布助手)。
//
// 协议(与发布助手 references/serial-screenshot.md 逐字对齐):
//   主机 -> 设备: ASCII 行 "FAP_SCREENSHOT_V1\n"(115200 波特;
//                 USB-Serial-JTAG 是原生 USB-CDC,波特率仅为名义值)。
//   设备 -> 主机: 头行 "FAP_SCREENSHOT_V1 <宽> <高> RGB565LE <字节数>\n"
//                 紧跟 <字节数> 字节小端 RGB565 行主序像素。
//
// 实现要点:
//   * 只读:整屏快照 + 回传,不重启/刷机/改设置/暴露凭据;
//   * 不复刻整屏缓冲:借助 LVGL 逐行块刷新回调,在请求帧内把每个
//     20 行刷新块以小端 RGB565 直接转发,内存占用仅一个刷新块大小;
//   * 回传期间临时关闭日志输出,避免日志字节混入二进制流
//     (主机按声明长度精确读取,任何插入字节都会错位)。
#include "fap_screenshot.h"
#include "bsp_display.h"       // bsp_lvgl_lock / bsp_lvgl_unlock
#include "bsp_pins.h"          // BSP_LCD_W / BSP_LCD_H(快照缓冲尺寸)
#include "driver/usb_serial_jtag_vfs.h"  // 控制台 VFS 切到驱动通道
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "serial_dispatch.h"
#include "lvgl.h"
#include "display/lv_display_private.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "fap_shot";

#define FAP_CMD        "FAP_SCREENSHOT_V1"
#define FAP_CMD_LEN    (sizeof(FAP_CMD) - 1)
#define FAP_LINE_MAX   24    // 命令行缓冲:略大于命令长度,溢出即丢弃重来
#define FAP_TASK_STACK 8192  // lv_snapshot 在本任务内做整屏软件渲染,栈要余量
#define FAP_TASK_PRIO  3     // 低于 LVGL(4)/采集:截屏是偶发操作,不抢交互

// ---- 串口按键注入(自动化测试) ----
static fap_key_inject_cb_t s_key_hook;
void fap_set_key_inject_hook(fap_key_inject_cb_t cb) { s_key_hook = cb; }
static fap_json_line_cb_t s_json_hook;
static void (*s_tick_hook)(void);
static SemaphoreHandle_t s_tx_lock;
static TaskHandle_t s_task;
void fap_set_json_line_hook(fap_json_line_cb_t cb) { s_json_hook = cb; }
void fap_set_tick_hook(void (*cb)(void)) { s_tick_hook = cb; }

bool fap_usb_write(const void *data, size_t length) {
    if (!s_tx_lock || xSemaphoreTake(s_tx_lock, 0) != pdTRUE) return false;
    int count = usb_serial_jtag_write_bytes(data, length, pdMS_TO_TICKS(250));
    xSemaphoreGive(s_tx_lock);
    return count == (int)length;
}

static void dispatch_json(const char *line) {
    if (s_json_hook) s_json_hook(line);
}

static void dispatch_key(char key) {
    if (s_key_hook) s_key_hook(key);
}

// 循环写直到全部发出,必须分块:底层 xRingbufferSend 对超过环形缓冲容量
// (1024B)的整块写入会立即失败("data will never ever fit"),一次性提交
// 153600B 的载荷等于一个字节都发不出去。512B/块留足余量,每块由驱动
// 阻塞至真正入队,ISR 按 USB 速度把数据送走。
#define FAP_TX_CHUNK 512
static void write_all(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        size_t want = len > FAP_TX_CHUNK ? FAP_TX_CHUNK : len;
        int n = usb_serial_jtag_write_bytes(p, want, pdMS_TO_TICKS(2000));
        if (n <= 0) return;  // 主机掉线:放弃本次,任务继续等下一条命令
        p += n;
        len -= (size_t)n;
    }
}

// 渲染当前屏幕并以协议格式回传。失败只记日志不应答,主机会以超时给出
// 明确错误,不破坏流格式。
//
// 整屏快照缓冲:静态预留。运行时堆虽然还有 ~220KB,却拿不出 153600
// 字节的连续块(碎片化),动态分配的快照必然失败;编译期预留则永不受
// 碎片影响。64 字节对齐满足软件渲染的行距/搬运要求。
static lv_display_t *s_capture_display;
static lv_display_flush_cb_t s_original_flush_cb;
static bool s_capture_active;
static size_t s_capture_sent;

// LVGL 的原生 RGB565 缓冲是小端字节序;真正的 SPI 刷屏回调会在这里之后
// 才交换为屏幕所需的大端序。因此在原回调之前拦截并转发即可得到协议要求
// 的 RGB565LE。240x20 的刷新块一次只有约 9.6KB,不需要整屏静态缓冲。
static void capture_flush_callback(lv_display_t *display,
                                   const lv_area_t *area, uint8_t *color_map) {
    if (s_capture_active && area && color_map &&
        area->x1 == 0 && area->x2 == BSP_LCD_W - 1) {
        const size_t row_count = (size_t)(area->y2 - area->y1 + 1);
        const size_t bytes = row_count * (size_t)BSP_LCD_W * 2U;
        write_all(color_map, bytes);
        s_capture_sent += bytes;
    }
    if (s_original_flush_cb) s_original_flush_cb(display, area, color_map);
}

static void dump_screen(void) {
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "拿不到 LVGL 锁,放弃本次截屏");
        return;
    }
    lv_display_t *display = s_capture_display;
    if (s_capture_active) {
        ESP_LOGE(TAG, "已有截屏进行中");
        bsp_lvgl_unlock();
        return;
    }
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        bsp_lvgl_unlock();
        return;
    }

    const size_t expected = (size_t)BSP_LCD_W * BSP_LCD_H * 2U;
    char header[48];
    int n = snprintf(header, sizeof(header), "%s %d %d RGB565LE %lu\n",
                     FAP_CMD, (int)BSP_LCD_W, (int)BSP_LCD_H,
                     (unsigned long)expected);
    esp_log_level_set("*", ESP_LOG_NONE);  // 传输窗口内禁一切日志,见文件头注释
    write_all(header, (size_t)n);
    s_capture_sent = 0;
    s_capture_active = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    s_capture_active = false;
    xSemaphoreGive(s_tx_lock);
    bsp_lvgl_unlock();
    esp_log_level_set("*", ESP_LOG_INFO);  // 恢复到 sdkconfig 的默认日志级别
    if (s_capture_sent != expected) {
        ESP_LOGE(TAG, "截屏只回传 %lu/%lu 字节", (unsigned long)s_capture_sent,
                 (unsigned long)expected);
        return;
    }
    ESP_LOGI(TAG, "已回传截屏 %dx%d(%lu 字节)", (int)BSP_LCD_W, (int)BSP_LCD_H,
             (unsigned long)s_capture_sent);
}

// 串口应答任务:攒行匹配命令,其余输入(日志回显、换行等)一律忽略。
static void fap_task(void *arg) {
    (void)arg;
    serial_dispatch_t parser = {0};
    uint8_t buf[64];
    for (;;) {
        // 三重防空转:任何一条失败都必须让出 CPU——高优先级忙等会饿死
        // 空闲任务、触发任务看门狗,设备反复重启,表现为屏幕一直闪。
        if (!usb_serial_jtag_is_driver_installed()) {
            vTaskDelay(pdMS_TO_TICKS(500));  // 驱动未就绪:低频重试
            continue;
        }
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(500));
        if (s_tick_hook) s_tick_hook();
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));  // 读取出错:退避后再试
            continue;
        }
        if (n == 0) continue;  // 无输入:超时后继续等,任务常驻不退出
        for (int i = 0; i < n; i++) {
            serial_dispatch_feed(&parser, (char)buf[i], dispatch_key,
                                 dump_screen, dispatch_json);
        }
    }
}

void fap_screenshot_start(void) {
    if (s_task) return;
    if (!s_tx_lock) s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) return;
    // 本应用不启动 REPL,而控制台日志走的是 VFS 寄存器直写通道,
    // 全固件没有任何人安装过 USB 串口驱动——不装驱动,read_bytes 会
    // 解引用空指针(这正是早期"反复重启闪屏"的根因)。这里按官方 REPL
    // 的标准组合补装驱动,并把控制台 VFS 切到驱动通道,收发共用一条路。
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = {
            .rx_buffer_size = 2048,
            .tx_buffer_size = 1024,  // 截屏载荷 150KB 分块流式发送,稍大更顺
        };
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "串口驱动安装失败(%d),截屏协议不可用", err);
            return;
        }
    }
    usb_serial_jtag_vfs_use_driver();

    lv_display_t *display = lv_display_get_default();
    if (display && !s_capture_display) {
        s_capture_display = display;
        s_original_flush_cb = display->flush_cb;
        lv_display_set_flush_cb(display, capture_flush_callback);
    } else if (!display) {
        ESP_LOGE(TAG, "未找到默认显示,截屏协议不可用");
        return;
    }

    BaseType_t ok = xTaskCreate(fap_task, "fap_shot", FAP_TASK_STACK, NULL,
                                FAP_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "截屏任务创建失败(栈 %d 字节)", FAP_TASK_STACK);
        return;
    }
    ESP_LOGI(TAG, "截屏协议就绪:等待 FAP_SCREENSHOT_V1 命令");
}
