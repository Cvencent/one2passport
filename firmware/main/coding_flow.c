#include "coding_flow.h"

#include <string.h>

static size_t utf8_width(unsigned char byte)
{
    if ((byte & 0x80U) == 0U) return 1U;
    if ((byte & 0xE0U) == 0xC0U) return 2U;
    if ((byte & 0xF0U) == 0xE0U) return 3U;
    if ((byte & 0xF8U) == 0xF0U) return 4U;
    return 1U;
}

static void copy_text(char *destination, size_t size, const char *source)
{
    if (size == 0U) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    size_t length = 0U;
    while (source[length] != '\0') {
        size_t width = utf8_width((unsigned char)source[length]);
        size_t remaining = strlen(source + length);
        if (width > remaining) width = 1U;
        if (length + width >= size) break;
        length += width;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void clear_sensitive(coding_flow_t *flow)
{
    flow->transcript[0] = '\0';
    flow->approval_question[0] = '\0';
    flow->approval_detail[0] = '\0';
    flow->decision = CODING_DECISION_APPROVE_ONCE;
}

void coding_flow_init(coding_flow_t *flow)
{
    if (flow == NULL) {
        return;
    }
    memset(flow, 0, sizeof(*flow));
    flow->stage = CODING_STAGE_OFFLINE;
    flow->decision = CODING_DECISION_APPROVE_ONCE;
    copy_text(flow->task_status, sizeof(flow->task_status), "请连接电脑端");
}

void coding_flow_set_connected(coding_flow_t *flow, bool connected)
{
    if (flow == NULL) {
        return;
    }
    bool was_connected = flow->connected;
    flow->connected = connected;
    if (flow->stage == CODING_STAGE_DONE || flow->stage == CODING_STAGE_ERROR ||
        flow->stage == CODING_STAGE_RUNNING || flow->stage == CODING_STAGE_QUEUED ||
        flow->stage == CODING_STAGE_APPROVAL_REVIEW ||
        flow->stage == CODING_STAGE_APPROVAL_CONFIRM) {
        return;
    }
    if (connected && was_connected) return;
    clear_sensitive(flow);
    flow->stage = connected ? CODING_STAGE_IDLE : CODING_STAGE_OFFLINE;
    copy_text(flow->task_status, sizeof(flow->task_status),
              connected ? "可以开始语音编程" : "请连接电脑端");
}

coding_action_t coding_flow_begin_recording(coding_flow_t *flow)
{
    if (flow == NULL || !flow->connected ||
        (flow->stage != CODING_STAGE_IDLE && flow->stage != CODING_STAGE_DONE &&
         flow->stage != CODING_STAGE_ERROR)) {
        return CODING_ACTION_NONE;
    }
    clear_sensitive(flow);
    flow->stage = CODING_STAGE_RECORDING;
    copy_text(flow->task_status, sizeof(flow->task_status), "正在聆听");
    return CODING_ACTION_START_CAPTURE;
}

coding_action_t coding_flow_end_recording(coding_flow_t *flow)
{
    if (flow == NULL || flow->stage != CODING_STAGE_RECORDING) {
        return CODING_ACTION_NONE;
    }
    flow->stage = CODING_STAGE_TRANSCRIBING;
    copy_text(flow->task_status, sizeof(flow->task_status), "正在识别语音");
    return CODING_ACTION_STOP_CAPTURE;
}

bool coding_flow_set_transcript(coding_flow_t *flow, const char *transcript)
{
    if (flow == NULL || flow->stage != CODING_STAGE_TRANSCRIBING ||
        transcript == NULL || transcript[0] == '\0') {
        return false;
    }
    copy_text(flow->transcript, sizeof(flow->transcript), transcript);
    // CBDP v1 §19.4: AUDIO_END 之后 Bridge 已经完成 STT 并提交 turn —— 这里的
    // transcript 只是回显, 只能用于显示/状态收敛。旧流程在此进入"确认文字"页,
    // 用户按确认后走 SEND_* 再提交一遍, 造成同文两投(2026-10-25 Bridge 取证:
    // cbdp-audio:350674 与 cbdp-tx:350675 同文两条)。
    flow->stage = CODING_STAGE_QUEUED;
    copy_text(flow->task_status, sizeof(flow->task_status), "已识别并提交电脑端");
    return true;
}

coding_action_t coding_flow_confirm_voice(coding_flow_t *flow)
{
    // 语音提交入口已收敛为 AUDIO_* 单路径(见 set_transcript 注释):
    // confirm 不能再触发 SEND_VOICE, 否则同一句话会被提交两次。
    (void)flow;
    return CODING_ACTION_NONE;
}

void coding_flow_retry_voice(coding_flow_t *flow)
{
    if (flow == NULL || flow->stage != CODING_STAGE_VOICE_REVIEW) {
        return;
    }
    flow->transcript[0] = '\0';
    flow->stage = CODING_STAGE_IDLE;
    copy_text(flow->task_status, sizeof(flow->task_status), "请再次按一下确认键开始录音");
}

void coding_flow_cancel_voice(coding_flow_t *flow)
{
    if (flow == NULL || (flow->stage != CODING_STAGE_VOICE_REVIEW &&
                         flow->stage != CODING_STAGE_TRANSCRIBING)) {
        return;
    }
    flow->transcript[0] = '\0';
    flow->stage = flow->connected ? CODING_STAGE_IDLE : CODING_STAGE_OFFLINE;
    copy_text(flow->task_status, sizeof(flow->task_status),
              flow->connected ? "已取消语音指令" : "请连接电脑端");
}

void coding_flow_set_queued(coding_flow_t *flow, const char *status)
{
    if (flow == NULL || !flow->connected) {
        return;
    }
    flow->stage = CODING_STAGE_QUEUED;
    copy_text(flow->task_status, sizeof(flow->task_status), status);
}

void coding_flow_set_task_status(coding_flow_t *flow, const char *status)
{
    if (flow == NULL || !flow->connected) {
        return;
    }
    flow->stage = CODING_STAGE_RUNNING;
    copy_text(flow->task_status, sizeof(flow->task_status), status);
}

void coding_flow_set_done(coding_flow_t *flow, const char *summary)
{
    if (flow == NULL) {
        return;
    }
    flow->stage = CODING_STAGE_DONE;
    copy_text(flow->task_status, sizeof(flow->task_status), summary);
}

void coding_flow_set_error(coding_flow_t *flow, const char *message)
{
    if (flow == NULL) {
        return;
    }
    flow->stage = CODING_STAGE_ERROR;
    copy_text(flow->task_status, sizeof(flow->task_status), message);
}

bool coding_flow_show_approval(coding_flow_t *flow, const char *question,
                               const char *detail)
{
    if (flow == NULL || !flow->connected || question == NULL || question[0] == '\0') {
        return false;
    }
    copy_text(flow->approval_question, sizeof(flow->approval_question), question);
    copy_text(flow->approval_detail, sizeof(flow->approval_detail), detail);
    flow->decision = CODING_DECISION_APPROVE_ONCE;
    flow->stage = CODING_STAGE_APPROVAL_REVIEW;
    copy_text(flow->task_status, sizeof(flow->task_status), "需要你的审批");
    return true;
}

void coding_flow_next_decision(coding_flow_t *flow)
{
    if (flow == NULL || flow->stage != CODING_STAGE_APPROVAL_REVIEW) {
        return;
    }
    flow->decision = (coding_decision_t)((flow->decision + 1) % CODING_DECISION_COUNT);
}

void coding_flow_previous_decision(coding_flow_t *flow)
{
    if (flow == NULL || flow->stage != CODING_STAGE_APPROVAL_REVIEW) {
        return;
    }
    flow->decision = (coding_decision_t)((flow->decision + CODING_DECISION_COUNT - 1) %
                                         CODING_DECISION_COUNT);
}

bool coding_flow_open_approval_confirm(coding_flow_t *flow)
{
    if (flow == NULL || flow->stage != CODING_STAGE_APPROVAL_REVIEW) {
        return false;
    }
    flow->stage = CODING_STAGE_APPROVAL_CONFIRM;
    copy_text(flow->task_status, sizeof(flow->task_status), "请再次确认选择");
    return true;
}

void coding_flow_back_to_approval(coding_flow_t *flow)
{
    if (flow == NULL || flow->stage != CODING_STAGE_APPROVAL_CONFIRM) {
        return;
    }
    flow->stage = CODING_STAGE_APPROVAL_REVIEW;
    copy_text(flow->task_status, sizeof(flow->task_status), "需要你的审批");
}

coding_action_t coding_flow_confirm_approval(coding_flow_t *flow)
{
    if (flow == NULL || flow->stage != CODING_STAGE_APPROVAL_CONFIRM) {
        return CODING_ACTION_NONE;
    }
    flow->stage = CODING_STAGE_RUNNING;
    copy_text(flow->task_status, sizeof(flow->task_status), "正在发送审批结果");
    return CODING_ACTION_SEND_APPROVAL;
}

coding_action_t coding_flow_cancel_task(coding_flow_t *flow)
{
    if (flow == NULL || !flow->connected ||
        (flow->stage != CODING_STAGE_QUEUED &&
         flow->stage != CODING_STAGE_RUNNING)) {
        return CODING_ACTION_NONE;
    }
    flow->stage = CODING_STAGE_IDLE;
    copy_text(flow->task_status, sizeof(flow->task_status), "任务已取消");
    return CODING_ACTION_CANCEL_TASK;
}

const char *coding_flow_stage_name(coding_stage_t stage)
{
    static const char *const names[] = {
        "未连接", "等待指令", "正在聆听", "正在识别", "确认语音文字",
        "等待电脑接收", "任务进行中", "等待审批", "再次确认", "任务完成",
        "发生错误",
    };
    return stage >= CODING_STAGE_OFFLINE && stage <= CODING_STAGE_ERROR
               ? names[stage]
               : "未知状态";
}

const char *coding_flow_decision_name(coding_decision_t decision)
{
    static const char *const names[] = {
        "仅本次允许", "本次会话允许", "拒绝", "取消任务",
    };
    return decision >= CODING_DECISION_APPROVE_ONCE && decision < CODING_DECISION_COUNT
               ? names[decision]
               : "未知选择";
}

const char *coding_flow_error_title(const char *message)
{
    if (message == NULL) return "发生错误";
    if (strstr(message, "权限") || strstr(message, "permission")) return "权限受限";
    if (strstr(message, "超时") || strstr(message, "timeout")) return "等待超时";
    if (strstr(message, "连接") || strstr(message, "断开") ||
        strstr(message, "offline")) return "连接中断";
    if (strstr(message, "麦克风") || strstr(message, "录音")) return "录音失败";
    if (strstr(message, "识别") || strstr(message, "音频编码")) return "语音识别失败";
    return "发生错误";
}
