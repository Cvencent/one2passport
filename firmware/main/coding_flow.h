#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CODING_TEXT_MAX 256
#define CODING_TRANSCRIPT_MAX 2048
#define CODING_STATUS_MAX 3072
#define CODING_DETAIL_MAX 512

typedef enum {
    CODING_STAGE_OFFLINE = 0,
    CODING_STAGE_IDLE,
    CODING_STAGE_RECORDING,
    CODING_STAGE_TRANSCRIBING,
    CODING_STAGE_VOICE_REVIEW,
    CODING_STAGE_QUEUED,
    CODING_STAGE_RUNNING,
    CODING_STAGE_APPROVAL_REVIEW,
    CODING_STAGE_APPROVAL_CONFIRM,
    CODING_STAGE_DONE,
    CODING_STAGE_ERROR,
} coding_stage_t;

typedef enum {
    CODING_DECISION_APPROVE_ONCE = 0,
    CODING_DECISION_APPROVE_SESSION,
    CODING_DECISION_DENY,
    CODING_DECISION_CANCEL_TASK,
    CODING_DECISION_COUNT,
} coding_decision_t;

typedef enum {
    CODING_ACTION_NONE = 0,
    CODING_ACTION_START_CAPTURE,
    CODING_ACTION_STOP_CAPTURE,
    CODING_ACTION_SEND_VOICE,
    CODING_ACTION_SEND_APPROVAL,
    CODING_ACTION_CANCEL_TASK,
} coding_action_t;

typedef struct {
    coding_stage_t stage;
    coding_decision_t decision;
    bool connected;
    char transcript[CODING_TRANSCRIPT_MAX];
    char task_status[CODING_STATUS_MAX];
    char approval_question[CODING_TEXT_MAX];
    char approval_detail[CODING_DETAIL_MAX];
} coding_flow_t;

void coding_flow_init(coding_flow_t *flow);
void coding_flow_set_connected(coding_flow_t *flow, bool connected);
coding_action_t coding_flow_begin_recording(coding_flow_t *flow);
coding_action_t coding_flow_end_recording(coding_flow_t *flow);
bool coding_flow_set_transcript(coding_flow_t *flow, const char *transcript);
coding_action_t coding_flow_confirm_voice(coding_flow_t *flow);
void coding_flow_retry_voice(coding_flow_t *flow);
void coding_flow_cancel_voice(coding_flow_t *flow);
void coding_flow_set_queued(coding_flow_t *flow, const char *status);
void coding_flow_set_task_status(coding_flow_t *flow, const char *status);
void coding_flow_set_done(coding_flow_t *flow, const char *summary);
void coding_flow_set_error(coding_flow_t *flow, const char *message);
bool coding_flow_show_approval(coding_flow_t *flow, const char *question,
                               const char *detail);
void coding_flow_next_decision(coding_flow_t *flow);
void coding_flow_previous_decision(coding_flow_t *flow);
bool coding_flow_open_approval_confirm(coding_flow_t *flow);
void coding_flow_back_to_approval(coding_flow_t *flow);
coding_action_t coding_flow_confirm_approval(coding_flow_t *flow);
coding_action_t coding_flow_cancel_task(coding_flow_t *flow);
const char *coding_flow_stage_name(coding_stage_t stage);
const char *coding_flow_decision_name(coding_decision_t decision);
const char *coding_flow_error_title(const char *message);
