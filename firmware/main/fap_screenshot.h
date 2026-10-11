// main/fap_screenshot.h -- FAP_SCREENSHOT_V1 串口截屏协议(社区发布助手)。
//
// 发布流程要求固件实现该协议作为"真机运行"凭证:主机(发布工具)经
// USB-CDC 发来一行 ASCII 命令,设备回一行头 + RGB565LE 帧缓冲。
// 命令严格只读:不重启、不刷机、不改设置、不暴露任何设备凭据。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动截屏应答任务(内部只创建一次)。在 UI 就绪后调用。
void fap_screenshot_start(void);

// 串口按键注入(自动化测试用): fap_task 读到对应单字符时回调一次。
// 映射: 'u'=UP单击 'd'=DOWN单击 'o'=OK单击 'l'=OK长按 'k'=UP长按
// 回调运行在 fap_shot 任务上下文, 只能做入队等有界操作。
typedef void (*fap_key_inject_cb_t)(char c);
void fap_set_key_inject_hook(fap_key_inject_cb_t cb);

// 由 USB CDC 的唯一读者(coding_bridge)喂入字符: 单字符按键命令回调 hook,
// 其余字符按截屏协议滑窗处理。返回 true = 该字符已被注入消费。
typedef void (*fap_json_line_cb_t)(const char *line);
void fap_set_json_line_hook(fap_json_line_cb_t cb);
void fap_set_tick_hook(void (*cb)(void));
bool fap_usb_write(const void *data, size_t length);

#ifdef __cplusplus
}
#endif
