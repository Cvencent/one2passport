// main/xiaozhi_client.h -- XiaoZhi 云端协议客户端(OTA + WebSocket + Opus)
// 协议: 参考 78/xiaozhi-esp32 官方实现(参考固件 rc.10 同款)。
//   OTA 激活取 ws 配置 -> wss 连接 -> hello 握手 -> listen/opus 语音交互。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// 小智云端事件(供 UI 层消费)
typedef enum {
    XZ_EV_WS_OPENED = 0,   // 通道就绪(服务端 hello 完成)
    XZ_EV_WS_CLOSED,
    XZ_EV_STT,             // 用户语音识别文本就绪(xiaozhi_client_stt)
    XZ_EV_TTS_START,       // 回答开始(xiaozhi_client_tts 为本轮文本)
    XZ_EV_TTS_STOP,        // 播报结束
    XZ_EV_ERROR,
    XZ_EV_ACTIVATION,      // 设备未激活: 需在 xiaozhi.me 输入设备码(见下方 getter)
    /* 2026-10-23: 连接阶段的进度事件。
     * 之前 UI 只在"开始连接"和"成功/失败"两端有状态, 中间那段(拉配置、
     * TLS 握手、WS 握手)最长十几秒, 用户看到的就是"界面不动 -> 直接失败",
     * 分不清是在干活还是已经死了。现在每进入一个阶段都推一条, 界面跟着变。 */
    XZ_EV_PROGRESS,        // 连接阶段变化, 文本见 xiaozhi_client_progress()
} xz_ui_event_t;

bool xiaozhi_client_init(void);          // 建事件队列(可重入)
bool xiaozhi_client_connect(void);       // OTA+WS连接+hello(阻塞, 慢操作,勿持锁调)
void xiaozhi_client_close(void);
bool xiaozhi_client_reset_cancel(void);  // 仅在连接任务空闲时清除取消标志
bool xiaozhi_client_cancel(void);        // true=当前无连接任务, 已立即关闭
bool xiaozhi_client_is_cancelled(void);
void xiaozhi_client_start_listening(void);   // 长按OK: listen start + opus上行
void xiaozhi_client_stop_listening(void);    // 松开OK: 停上行(任务自动发stop)
void xiaozhi_client_abort_turn(void);       // 强行打断当前轮(思考超时用); 正常结束不要调用
void xiaozhi_client_stop_listening(void);    // 松开OK: 停上行(任务自动发stop)
bool xiaozhi_client_is_open(void);
const char *xiaozhi_client_stt(void);        // 最近识别文本
const char *xiaozhi_client_tts(void);        // 最近回答文本
// 设备激活码(OTA 返回 activation.code 时才有值, 未激活为空串)。
// 用户在 https://xiaozhi.me 输入该码完成设备绑定。
const char *xiaozhi_client_activation_code(void);
const char *xiaozhi_client_activation_message(void);
QueueHandle_t xiaozhi_client_events(void);
bool xiaozhi_opus_prewarm(void);             // 启动早期预分配 opus 编码器内存
void xiaozhi_opus_release(void);             // 退出小智时释放编码器(还给收音机)
// 当前连接阶段的简短中文描述(供进度页显示), 无连接时为空串。
const char *xiaozhi_client_progress(void);

#ifdef __cplusplus
}
#endif