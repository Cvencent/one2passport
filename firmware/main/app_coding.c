#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coding_flow.h"
#include "coding_workspace.h"
#include "coding_pet.h"
#include "coding_bridge.h"
#include "codex_bridge_protocol.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui_pixel.h"
#include "ui_pixel_math.h"
#include "ui_sfx.h"
#include "radio_home.h"
#include "app_registry.h"

static const char *kTag = "app_coding";

#define ACTION_COUNT_MAX 4
#define WAVE_BAR_COUNT 7
#define PET_DECOR_COUNT 6
#define BODY_PANEL_Y 95
#define ACTION_TOP_Y 238
#define ACTION_BOTTOM_Y 278
#define ACTION_HEIGHT 34
#define CATALOG_MAX 8
#define CATALOG_LINE_BUDGET 8
#define CATALOG_LINE_UNITS 22
#define CATALOG_ID_MAX 12
#define CATALOG_TITLE_MAX 192
#define CATALOG_DETAIL_MAX 192
#define RENDER_DETAIL_MAX (CODING_STATUS_MAX + CODING_DETAIL_MAX + 16)
#define CATALOG_INPUT_GUARD_MS 320U
#define SEND_ACK_TIMEOUT_MS 8000U

#define PET_TEAL   0x55D9CC
#define PET_DARK   0x071526
#define PET_METAL  0xC8D6DC
#define PET_SHADOW 0x082451
#define PET_PINK   0xFF7C8E

typedef struct {
    lv_obj_t *root;
    lv_obj_t *shadow;
    lv_obj_t *antenna;
    lv_obj_t *antenna_tip;
    lv_obj_t *ear_l;
    lv_obj_t *ear_r;
    lv_obj_t *ear_inner_l;
    lv_obj_t *ear_inner_r;
    lv_obj_t *body;
    lv_obj_t *arm_l;
    lv_obj_t *arm_r;
    lv_obj_t *hand_l;
    lv_obj_t *hand_r;
    lv_obj_t *head_shadow;
    lv_obj_t *head;
    lv_obj_t *face;
    lv_obj_t *face_highlight;
    lv_obj_t *face_shadow;
    lv_obj_t *eye_l;
    lv_obj_t *eye_r;
    lv_obj_t *shine_l;
    lv_obj_t *shine_r;
    lv_obj_t *brow_l;
    lv_obj_t *brow_r;
    lv_obj_t *mouth_l;
    lv_obj_t *mouth_m;
    lv_obj_t *mouth_r;
    lv_obj_t *mouth_inner;
    lv_obj_t *cheek_l;
    lv_obj_t *cheek_r;
    lv_obj_t *keyboard;
    lv_obj_t *key_light;
    lv_obj_t *decor[PET_DECOR_COUNT];
} pet_ui_t;

typedef enum {
    CATALOG_NONE = 0,
    CATALOG_LOADING_PROJECTS,
    CATALOG_PROJECTS,
    CATALOG_LOADING_THREADS,
    CATALOG_THREADS,
} catalog_mode_t;

typedef coding_catalog_item_t catalog_item_t;
static coding_workspace_t *s_workspace;

static coding_flow_t s_flow;
static coding_pet_t s_pet_state;
static char s_result_thread[64];
static char s_result_turn[64];
static char s_active_message_id[80];
static char s_read_ack_message_id[80];
static uint32_t s_read_count;
static bool s_local_unread;
static bool s_initialized;
static bool s_audio_available;
static bool s_full_ui_active;
static bool s_have_rendered_stage;
static coding_stage_t s_rendered_stage;
static lv_obj_t *s_screen;
static lv_obj_t *s_stage;
static lv_obj_t *s_body_panel;
static lv_obj_t *s_body;
static lv_obj_t *s_meta_bar;
static lv_obj_t *s_meta;
static lv_obj_t *s_page_label;
static lv_obj_t *s_pet_phrase_panel;
static lv_obj_t *s_pet_phrase;
static lv_obj_t *s_pet_phrase_tail[2];
static lv_obj_t *s_record_dot;
static lv_obj_t *s_wave_bars[WAVE_BAR_COUNT];
static lv_timer_t *s_record_timer;
static pet_ui_t s_pet_ui;
static lv_obj_t *s_action_panels[ACTION_COUNT_MAX];
static lv_obj_t *s_action_labels[ACTION_COUNT_MAX];
static int s_action_count;
static int s_selected;
static bool s_cancel_confirm;
static uint8_t s_capture_level;
static unsigned s_record_frame;
static size_t s_page;
static size_t s_page_count;
static int s_body_height = 120;
static uint32_t s_record_started_ms;
static uint32_t s_transcribing_started_ms;
static uint32_t s_task_started_ms;
static uint32_t s_last_task_elapsed_ms;
static bool s_has_remote_task_elapsed;

#define s_detail (s_workspace->detail)
#define s_render_detail (s_workspace->detail)
static char s_page_text[512];
static catalog_mode_t s_catalog_mode;
#define s_catalog (s_workspace->catalog)
static int s_catalog_count;
static int s_catalog_selected;
static int s_catalog_window_start;
static uint32_t s_catalog_input_unlock_ms;
static catalog_item_t *s_projects;
static int s_projects_count;
static int s_projects_selected;
static int s_projects_window_start;
static size_t s_catalog_return_page;
static int s_catalog_return_selected;
static bool s_selecting_thread;
static bool s_discard_selection_updates;
static uint32_t s_send_started_ms;
static char s_current_task[CATALOG_TITLE_MAX];
static char s_current_project[CATALOG_DETAIL_MAX];
// 选中会话的"最后一段文本"(电脑端随 codex_selected 下发)。
// 只保留这一段用于展示, 不缓存整段对话 —— 设备内存有限。
static char s_last_message[RENDER_DETAIL_MAX];

static void coding_render(void);

// The CBDP receive callback runs on the network task. Rendering straight from
// there meant the wireless_bridge task could sit inside LVGL text layout for
// several seconds, which starved low-priority work, tripped the task watchdog,
// and made the host drop the session mid-recording. Mark the screen dirty and
// let the LVGL timer below repaint on the LVGL task instead.
static volatile bool s_render_pending;

static void coding_render_deferred(void)
{
    s_render_pending = true;
}

static void release_project_cache(void)
{
    free(s_projects);
    s_projects = NULL;
}

static lv_obj_t *record_block(lv_obj_t *parent, int width, int height,
                              uint32_t color)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(block, width, height);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_radius(block, 0, 0);
    lv_obj_set_style_bg_color(block, lv_color_hex(color), 0);
    return block;
}

static void block_place(lv_obj_t *block, int x, int y, int width, int height,
                        uint32_t color)
{
    lv_obj_clear_flag(block, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(block, x, y);
    lv_obj_set_size(block, width, height);
    lv_obj_set_style_bg_color(block, lv_color_hex(color), 0);
}

static void block_hide(lv_obj_t *block)
{
    lv_obj_add_flag(block, LV_OBJ_FLAG_HIDDEN);
}

static void pet_mouth_smile(void)
{
    block_place(s_pet_ui.mouth_l, 39, 45, 4, 2, PET_DARK);
    block_place(s_pet_ui.mouth_m, 43, 47, 10, 2, PET_DARK);
    block_place(s_pet_ui.mouth_r, 53, 45, 4, 2, PET_DARK);
    block_hide(s_pet_ui.mouth_inner);
}

static void pet_mouth_flat(void)
{
    block_hide(s_pet_ui.mouth_l);
    block_place(s_pet_ui.mouth_m, 41, 46, 14, 2, PET_DARK);
    block_hide(s_pet_ui.mouth_r);
    block_hide(s_pet_ui.mouth_inner);
}

static void pet_mouth_open(void)
{
    block_hide(s_pet_ui.mouth_l);
    block_place(s_pet_ui.mouth_m, 43, 42, 10, 10, PET_DARK);
    block_hide(s_pet_ui.mouth_r);
    block_place(s_pet_ui.mouth_inner, 46, 45, 4, 4, PET_TEAL);
}

static void pet_mouth_frown(void)
{
    block_place(s_pet_ui.mouth_l, 39, 47, 4, 2, PET_DARK);
    block_place(s_pet_ui.mouth_m, 43, 45, 10, 2, PET_DARK);
    block_place(s_pet_ui.mouth_r, 53, 47, 4, 2, PET_DARK);
    block_hide(s_pet_ui.mouth_inner);
}

static void pet_eyes_open(bool wide)
{
    int width = wide ? 8 : 6;
    int height = wide ? 12 : 10;
    int left = wide ? 31 : 33;
    int right = wide ? 57 : 59;
    int top = wide ? 28 : 30;
    block_place(s_pet_ui.eye_l, left, top, width, height, PET_DARK);
    block_place(s_pet_ui.eye_r, right, top, width, height, PET_DARK);
    block_place(s_pet_ui.shine_l, left + 1, top + 1, 2, 3, UI_WHITE);
    block_place(s_pet_ui.shine_r, right + 1, top + 1, 2, 3, UI_WHITE);
}

static void pet_eyes_closed(void)
{
    block_place(s_pet_ui.eye_l, 31, 35, 9, 2, PET_DARK);
    block_place(s_pet_ui.eye_r, 57, 35, 9, 2, PET_DARK);
    block_hide(s_pet_ui.shine_l);
    block_hide(s_pet_ui.shine_r);
}

static void pet_reset_pose(void)
{
    lv_obj_set_pos(s_pet_ui.root, 14, 4);
    lv_obj_set_style_opa(s_pet_ui.root, LV_OPA_COVER, 0);
    block_place(s_pet_ui.shadow, 22, 75, 54, 3, PET_SHADOW);
    block_place(s_pet_ui.antenna, 47, 5, 3, 11, PET_METAL);
    block_place(s_pet_ui.antenna_tip, 43, 0, 11, 8, PET_TEAL);
    block_place(s_pet_ui.ear_l, 9, 27, 9, 18, PET_TEAL);
    block_place(s_pet_ui.ear_r, 78, 27, 9, 18, PET_TEAL);
    block_place(s_pet_ui.ear_inner_l, 12, 31, 3, 10, PET_SHADOW);
    block_place(s_pet_ui.ear_inner_r, 81, 31, 3, 10, PET_SHADOW);
    block_place(s_pet_ui.body, 35, 56, 27, 11, 0x158C88);
    block_place(s_pet_ui.arm_l, 18, 56, 18, 5, PET_METAL);
    block_place(s_pet_ui.arm_r, 61, 56, 18, 5, PET_METAL);
    block_place(s_pet_ui.hand_l, 12, 53, 10, 8, PET_TEAL);
    block_place(s_pet_ui.hand_r, 75, 53, 10, 8, PET_TEAL);
    block_place(s_pet_ui.head_shadow, 20, 17, 63, 45, PET_SHADOW);
    block_place(s_pet_ui.head, 17, 14, 63, 45, PET_METAL);
    block_place(s_pet_ui.face, 22, 19, 53, 35, PET_TEAL);
    block_place(s_pet_ui.face_highlight, 25, 21, 47, 3, 0xA7FFF2);
    block_place(s_pet_ui.face_shadow, 22, 50, 53, 4, 0x249F99);
    pet_eyes_open(false);
    block_hide(s_pet_ui.brow_l);
    block_hide(s_pet_ui.brow_r);
    pet_mouth_smile();
    block_place(s_pet_ui.cheek_l, 25, 43, 7, 3, PET_PINK);
    block_place(s_pet_ui.cheek_r, 65, 43, 7, 3, PET_PINK);
    block_place(s_pet_ui.keyboard, 12, 65, 74, 10, 0x243548);
    block_place(s_pet_ui.key_light, 78, 67, 4, 3, UI_WARNING);
    for (int i = 0; i < PET_DECOR_COUNT; ++i) block_hide(s_pet_ui.decor[i]);
}

static void pet_create(lv_obj_t *parent)
{
    s_pet_ui.root = lv_obj_create(parent);
    lv_obj_remove_flag(s_pet_ui.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pet_ui.root, 96, 80);
    lv_obj_set_style_pad_all(s_pet_ui.root, 0, 0);
    lv_obj_set_style_border_width(s_pet_ui.root, 0, 0);
    lv_obj_set_style_bg_opa(s_pet_ui.root, LV_OPA_TRANSP, 0);

    s_pet_ui.shadow = record_block(s_pet_ui.root, 1, 1, PET_SHADOW);
    s_pet_ui.antenna = record_block(s_pet_ui.root, 1, 1, PET_METAL);
    s_pet_ui.antenna_tip = record_block(s_pet_ui.root, 1, 1, PET_TEAL);
    s_pet_ui.body = record_block(s_pet_ui.root, 1, 1, 0x158C88);
    s_pet_ui.arm_l = record_block(s_pet_ui.root, 1, 1, PET_METAL);
    s_pet_ui.arm_r = record_block(s_pet_ui.root, 1, 1, PET_METAL);
    s_pet_ui.hand_l = record_block(s_pet_ui.root, 1, 1, PET_TEAL);
    s_pet_ui.hand_r = record_block(s_pet_ui.root, 1, 1, PET_TEAL);
    s_pet_ui.ear_l = record_block(s_pet_ui.root, 1, 1, PET_TEAL);
    s_pet_ui.ear_r = record_block(s_pet_ui.root, 1, 1, PET_TEAL);
    s_pet_ui.ear_inner_l = record_block(s_pet_ui.root, 1, 1, PET_SHADOW);
    s_pet_ui.ear_inner_r = record_block(s_pet_ui.root, 1, 1, PET_SHADOW);
    s_pet_ui.head_shadow = record_block(s_pet_ui.root, 1, 1, PET_SHADOW);
    s_pet_ui.head = record_block(s_pet_ui.root, 1, 1, PET_METAL);
    lv_obj_set_style_border_width(s_pet_ui.head, 2, 0);
    lv_obj_set_style_border_color(s_pet_ui.head, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_radius(s_pet_ui.head, 5, 0);
    s_pet_ui.face = record_block(s_pet_ui.root, 1, 1, PET_TEAL);
    lv_obj_set_style_radius(s_pet_ui.face, 3, 0);
    s_pet_ui.face_highlight = record_block(s_pet_ui.root, 1, 1, 0xA7FFF2);
    s_pet_ui.face_shadow = record_block(s_pet_ui.root, 1, 1, 0x249F99);
    s_pet_ui.eye_l = record_block(s_pet_ui.root, 1, 1, PET_DARK);
    s_pet_ui.eye_r = record_block(s_pet_ui.root, 1, 1, PET_DARK);
    s_pet_ui.shine_l = record_block(s_pet_ui.root, 1, 1, UI_WHITE);
    s_pet_ui.shine_r = record_block(s_pet_ui.root, 1, 1, UI_WHITE);
    s_pet_ui.brow_l = record_block(s_pet_ui.root, 1, 1, PET_DARK);
    s_pet_ui.brow_r = record_block(s_pet_ui.root, 1, 1, PET_DARK);
    s_pet_ui.mouth_l = record_block(s_pet_ui.root, 1, 1, PET_DARK);
    s_pet_ui.mouth_m = record_block(s_pet_ui.root, 1, 1, PET_DARK);
    s_pet_ui.mouth_r = record_block(s_pet_ui.root, 1, 1, PET_DARK);
    s_pet_ui.mouth_inner = record_block(s_pet_ui.root, 1, 1, PET_TEAL);
    s_pet_ui.cheek_l = record_block(s_pet_ui.root, 1, 1, PET_PINK);
    s_pet_ui.cheek_r = record_block(s_pet_ui.root, 1, 1, PET_PINK);
    s_pet_ui.keyboard = record_block(s_pet_ui.root, 1, 1, 0x243548);
    lv_obj_set_style_border_width(s_pet_ui.keyboard, 2, 0);
    lv_obj_set_style_border_color(s_pet_ui.keyboard, lv_color_hex(PET_METAL), 0);
    s_pet_ui.key_light = record_block(s_pet_ui.root, 1, 1, UI_WARNING);
    for (int i = 0; i < PET_DECOR_COUNT; ++i) {
        s_pet_ui.decor[i] = record_block(s_pet_ui.root, 1, 1, UI_ACCENT);
    }
    pet_reset_pose();
}

static const char *pet_phrase(coding_stage_t stage)
{
    static const char *const phrases[] = {
        "连接断啦\n有点担心", "准备好啦", "我在听", "听懂中…", "是这样吗？",
        "等电脑接收", "努力工作！", "帮我决定", "再确认哦", "完成啦！",
        "我来看看",
    };
    return stage >= CODING_STAGE_OFFLINE && stage <= CODING_STAGE_ERROR
               ? phrases[stage]
               : "你好呀";
}

static uint32_t stage_accent(coding_stage_t stage)
{
    if (stage == CODING_STAGE_APPROVAL_REVIEW ||
        stage == CODING_STAGE_APPROVAL_CONFIRM) return UI_WARNING;
    if (stage == CODING_STAGE_DONE) return UI_SUCCESS;
    if (stage == CODING_STAGE_ERROR) return UI_DANGER;
    if (stage == CODING_STAGE_OFFLINE) return UI_WARNING;
    return UI_PRIMARY;
}

static void pet_animate(void)
{
    if (s_pet_ui.root == NULL) return;
    pet_reset_pose();
    int x = 14;
    int y = 4;
    bool blink = ui_pixel_blink_frame(lv_tick_get());

    switch (s_flow.stage) {
    case CODING_STAGE_OFFLINE:
        x += (s_record_frame / 5U) % 2U;
        pet_eyes_open(true);
        pet_mouth_frown();
        block_place(s_pet_ui.brow_l, 30, 25, 10, 2, PET_DARK);
        block_place(s_pet_ui.brow_r, 57, 25, 10, 2, PET_DARK);
        block_place(s_pet_ui.antenna, 47, 7, 12, 3, UI_WARNING);
        block_place(s_pet_ui.antenna_tip, 58, 5, 8, 7, UI_WARNING);
        block_place(s_pet_ui.arm_l, 17, 48, 13, 5, PET_METAL);
        block_place(s_pet_ui.hand_l, 10, 41, 10, 9, PET_TEAL);
        block_place(s_pet_ui.decor[0], 68, 42, 3, 7, UI_PRIMARY);
        block_place(s_pet_ui.decor[1], 69, 51, 2, 3, UI_PRIMARY);
        break;
    case CODING_STAGE_IDLE:
        y -= (s_record_frame / 8U) % 2U;
        if (blink) pet_eyes_closed();
        block_place(s_pet_ui.arm_r, 68, 48, 12, 5, PET_METAL);
        block_place(s_pet_ui.hand_r, 78, 42 - (int)((s_record_frame / 4U) % 2U) * 3,
                    10, 9, PET_TEAL);
        break;
    case CODING_STAGE_RECORDING:
        pet_eyes_open(true);
        if ((s_record_frame / 4U) % 2U) pet_mouth_open();
        else pet_mouth_flat();
        block_place(s_pet_ui.hand_l, 7, 28, 10, 10, PET_TEAL);
        block_place(s_pet_ui.hand_r, 80, 28, 10, 10, PET_TEAL);
        block_place(s_pet_ui.ear_l, 7, 25, 11, 22, UI_WARNING);
        block_place(s_pet_ui.ear_r, 78, 25, 11, 22, UI_WARNING);
        break;
    case CODING_STAGE_TRANSCRIBING: {
        pet_mouth_flat();
        int dots = (int)((s_record_frame / 4U) % 4U);
        for (int i = 0; i < dots; ++i) {
            block_place(s_pet_ui.decor[i], 72 + i * 7, 5, 4, 4, UI_PRIMARY);
        }
        block_place(s_pet_ui.eye_l, 35, 28, 6, 10, PET_DARK);
        block_place(s_pet_ui.eye_r, 61, 28, 6, 10, PET_DARK);
        break;
    }
    case CODING_STAGE_VOICE_REVIEW:
        if (blink) pet_eyes_closed();
        block_place(s_pet_ui.arm_r, 67, 49, 15, 5, PET_METAL);
        block_place(s_pet_ui.hand_r, 80, 43, 10, 9, PET_TEAL);
        break;
    case CODING_STAGE_QUEUED: {
        pet_mouth_flat();
        if (blink) pet_eyes_closed();
        int dots = (int)((s_record_frame / 5U) % 4U);
        for (int i = 0; i < dots; ++i) {
            block_place(s_pet_ui.decor[i], 70 + i * 7, 7, 4, 4, UI_PRIMARY);
        }
        break;
    }
    case CODING_STAGE_RUNNING:
        y -= (s_record_frame / 3U) % 2U;
        block_place(s_pet_ui.brow_l, 31, 27, 9, 2, PET_DARK);
        block_place(s_pet_ui.brow_r, 57, 27, 9, 2, PET_DARK);
        block_place(s_pet_ui.hand_l, 24, 59, 11, 8, PET_TEAL);
        block_place(s_pet_ui.hand_r, 63, 59, 11, 8, PET_TEAL);
        block_place(s_pet_ui.arm_l, 29, 55, 12, 5, PET_METAL);
        block_place(s_pet_ui.arm_r, 56, 55, 12, 5, PET_METAL);
        break;
    case CODING_STAGE_APPROVAL_REVIEW:
        pet_eyes_open(true);
        pet_mouth_open();
        block_place(s_pet_ui.brow_l, 31, 24, 9, 2, PET_DARK);
        block_place(s_pet_ui.brow_r, 57, 24, 9, 2, PET_DARK);
        block_place(s_pet_ui.arm_l, 10, 40, 13, 5, PET_METAL);
        block_place(s_pet_ui.hand_l, 4, 31, 11, 11, PET_TEAL);
        block_place(s_pet_ui.decor[0], 85, 3, 4, 14, UI_WARNING);
        block_place(s_pet_ui.decor[1], 85, 21, 4, 4, UI_WARNING);
        break;
    case CODING_STAGE_APPROVAL_CONFIRM:
        pet_eyes_open(true);
        pet_mouth_flat();
        block_place(s_pet_ui.brow_l, 31, 25, 9, 2, PET_DARK);
        block_place(s_pet_ui.brow_r, 57, 25, 9, 2, PET_DARK);
        block_place(s_pet_ui.hand_l, 34, 56, 10, 8, PET_TEAL);
        block_place(s_pet_ui.hand_r, 54, 56, 10, 8, PET_TEAL);
        break;
    case CODING_STAGE_DONE: {
        static const int jump[] = {0, -4, -7, -4, 0};
        y += jump[(s_record_frame / 2U) % 5U];
        pet_eyes_closed();
        pet_mouth_smile();
        block_place(s_pet_ui.arm_l, 12, 31, 15, 5, PET_METAL);
        block_place(s_pet_ui.arm_r, 70, 31, 15, 5, PET_METAL);
        block_place(s_pet_ui.hand_l, 5, 23, 11, 10, PET_TEAL);
        block_place(s_pet_ui.hand_r, 82, 23, 11, 10, PET_TEAL);
        static const int dx[PET_DECOR_COUNT] = {2, 17, 75, 90, 7, 86};
        static const int dy[PET_DECOR_COUNT] = {9, 2, 4, 15, 55, 54};
        for (int i = 0; i < PET_DECOR_COUNT; ++i) {
            block_place(s_pet_ui.decor[i], dx[i], dy[i], 4, 4,
                        i % 2 ? UI_ACCENT : UI_PRIMARY);
        }
        break;
    }
    case CODING_STAGE_ERROR:
        x += (s_record_frame % 4U < 2U) ? -2 : 2;
        pet_mouth_frown();
        block_place(s_pet_ui.brow_l, 31, 26, 9, 2, PET_DARK);
        block_place(s_pet_ui.brow_r, 57, 26, 9, 2, PET_DARK);
        block_place(s_pet_ui.decor[0], 84, 6, 12, 3, UI_DANGER);
        block_place(s_pet_ui.decor[1], 88, 2, 3, 12, UI_DANGER);
        break;
    default:
        break;
    }
    lv_obj_set_pos(s_pet_ui.root, x, y);
}

static void record_visual_set(bool active)
{
    if (s_record_dot == NULL) return;
    if (active) {
        lv_obj_clear_flag(s_record_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_label, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < WAVE_BAR_COUNT; ++i) {
            lv_obj_clear_flag(s_wave_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_record_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_page_label, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < WAVE_BAR_COUNT; ++i) {
            lv_obj_add_flag(s_wave_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
        s_capture_level = 0;
    }
}

static void format_elapsed(char *output, size_t output_size, uint32_t elapsed_ms)
{
    uint32_t seconds = elapsed_ms / 1000U;
    snprintf(output, output_size, "%02lu:%02lu",
             (unsigned long)(seconds / 60U), (unsigned long)(seconds % 60U));
}

static bool body_text_fits(const char *text, void *context)
{
    (void)context;
    lv_point_t size;
    lv_text_get_size(&size, text, ui_cn_font(),
                    lv_obj_get_style_text_letter_space(s_body, 0),
                    lv_obj_get_style_text_line_space(s_body, 0),
                    194, LV_TEXT_FLAG_NONE);
    return size.y <= s_body_height;
}

static size_t body_page_count(const char *text)
{
    size_t pages = 0U;
    while (*text) {
        size_t bytes = ui_pixel_text_fit_copy(s_page_text, sizeof(s_page_text),
                                              text, body_text_fits, NULL);
        if (bytes == 0U) break;
        text += bytes;
        ++pages;
    }
    return pages ? pages : 1U;
}

static void render_body_page(void)
{
    if (s_body == NULL) return;
    char page_number[16];
    s_page_count = body_page_count(s_detail);
    if (s_page >= s_page_count) s_page = 0U;
    const char *text = s_detail;
    for (size_t page = 0U; page <= s_page; ++page) {
        size_t bytes = ui_pixel_text_fit_copy(s_page_text, sizeof(s_page_text),
                                              text, body_text_fits, NULL);
        text += bytes;
    }
    lv_label_set_text(s_body, s_page_text);
    if (s_page_count > 1U) {
        snprintf(page_number, sizeof(page_number), "%u/%u",
                 (unsigned)(s_page + 1U), (unsigned)s_page_count);
        lv_label_set_text(s_page_label, page_number);
    } else {
        lv_label_set_text(s_page_label, "");
    }
}

static void update_meta(void)
{
    if (s_meta == NULL) return;
    if (s_catalog_mode != CATALOG_NONE) {
        bool ready = s_catalog_mode == CATALOG_PROJECTS ||
                     s_catalog_mode == CATALOG_THREADS;
        lv_label_set_text(s_meta, ready ? "上下选择 · OK确认" : "请稍候");
        return;
    }
    char elapsed[16];
    char meta[96];
    uint32_t now = lv_tick_get();

    switch (s_flow.stage) {
    case CODING_STAGE_OFFLINE:
        snprintf(meta, sizeof(meta), "请启动电脑端桥接");
        break;
    case CODING_STAGE_IDLE:
        snprintf(meta, sizeof(meta), "按 OK 开始说话");
        break;
    case CODING_STAGE_RECORDING:
        format_elapsed(elapsed, sizeof(elapsed), now - s_record_started_ms);
        snprintf(meta, sizeof(meta), "录音 %s · OK结束", elapsed);
        break;
    case CODING_STAGE_TRANSCRIBING:
        snprintf(meta, sizeof(meta), "正在识别语音");
        break;
    case CODING_STAGE_VOICE_REVIEW:
        snprintf(meta, sizeof(meta), "发送前请确认文字");
        break;
    case CODING_STAGE_QUEUED:
        snprintf(meta, sizeof(meta), "等待电脑端接收");
        break;
    case CODING_STAGE_RUNNING:
        format_elapsed(elapsed, sizeof(elapsed), now - s_task_started_ms);
        snprintf(meta, sizeof(meta), "已用时 %s", elapsed);
        break;
    case CODING_STAGE_APPROVAL_REVIEW:
    case CODING_STAGE_APPROVAL_CONFIRM:
        snprintf(meta, sizeof(meta), "任务暂停，等待你的选择");
        break;
    case CODING_STAGE_DONE:
        format_elapsed(elapsed, sizeof(elapsed), s_last_task_elapsed_ms);
        if (s_read_count > 0U) {
            snprintf(meta, sizeof(meta), "完成用时 %s · %u 人已读", elapsed,
                     (unsigned)s_read_count);
        } else {
            snprintf(meta, sizeof(meta), "完成用时 %s", elapsed);
        }
        break;
    case CODING_STAGE_ERROR:
        if (s_read_count > 0U) {
            snprintf(meta, sizeof(meta), "失败 · %u 人已读",
                     (unsigned)s_read_count);
        } else {
            snprintf(meta, sizeof(meta), "可重新录音后再试");
        }
        break;
    default:
        meta[0] = '\0';
        break;
    }
    lv_label_set_text(s_meta, meta);
}

static void record_animate(lv_timer_t *timer)
{
    (void)timer;
    if (s_screen == NULL) return;

    if (s_render_pending) {
        s_render_pending = false;
        coding_render();
    }
    s_record_frame++;
    if ((s_flow.stage == CODING_STAGE_DONE || s_flow.stage == CODING_STAGE_ERROR) &&
        s_screen != NULL && s_active_message_id[0] != '\0' &&
        strcmp(s_active_message_id, s_read_ack_message_id) != 0 &&
        codex_read_tracker_take_due(s_active_message_id, "", lv_tick_get())) {
        if (coding_bridge_send_read_ack(s_active_message_id)) {
            snprintf(s_read_ack_message_id, sizeof(s_read_ack_message_id), "%s",
                     s_active_message_id);
        } else {
            codex_read_tracker_rearm(s_active_message_id, "", lv_tick_get());
        }
    }
    if (s_flow.stage == CODING_STAGE_TRANSCRIBING &&
        lv_tick_elaps(s_transcribing_started_ms) >= 18000U) {
        coding_bridge_cancel_record();
        coding_flow_set_error(&s_flow, "语音识别超时，请重新录音");
        coding_render();
    }
    if (s_flow.stage == CODING_STAGE_QUEUED && s_send_started_ms != 0U &&
        strcmp(s_flow.task_status, "正在发送到电脑端") == 0 &&
        lv_tick_elaps(s_send_started_ms) >= SEND_ACK_TIMEOUT_MS) {
        s_send_started_ms = 0U;
        coding_flow_set_error(&s_flow, "电脑端未确认接收，请重新发送");
        coding_render();
    }
    pet_animate();
    update_meta();
    if (s_flow.stage != CODING_STAGE_RECORDING || s_record_dot == NULL) return;

    static const uint8_t profile[WAVE_BAR_COUNT] = {45, 68, 88, 100, 82, 62, 42};
    lv_obj_set_style_bg_color(s_record_dot,
                              lv_color_hex((s_record_frame / 4) % 2
                                               ? UI_DANGER : UI_WARNING), 0);
    int base_level = s_capture_level;
    for (int i = 0; i < WAVE_BAR_COUNT; ++i) {
        int idle_motion = 2 * (int)((s_record_frame + i * 2) % 4);
        int height = 6 + base_level * profile[i] * 32 / 10000 + idle_motion;
        if (height > 42) height = 42;
        lv_obj_set_height(s_wave_bars[i], height);
        lv_obj_align(s_wave_bars[i], LV_ALIGN_BOTTOM_MID,
                     (i - WAVE_BAR_COUNT / 2) * 16, -31);
    }
}

static void ensure_initialized(void)
{
    if (!s_initialized) {
        coding_flow_init(&s_flow);
        s_initialized = true;
    }
}

static void set_actions(const char *first, const char *second,
                        const char *third, const char *fourth)
{
    const char *items[ACTION_COUNT_MAX] = {first, second, third, fourth};
    s_action_count = 0;
    for (int i = 0; i < ACTION_COUNT_MAX; ++i) {
        if (items[i] == NULL) {
            lv_obj_add_flag(s_action_panels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        ++s_action_count;
        lv_obj_clear_flag(s_action_panels[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_action_labels[i], items[i]);
    }

    int panel_height;
    if (s_action_count == 0) {
        panel_height = 216;
    } else if (s_action_count <= 3) {
        panel_height = 174;
        for (int i = 0; i < s_action_count; ++i) {
            int width = (216 - (s_action_count - 1) * 4) / s_action_count;
            int x = 12 + i * (width + 4);
            lv_obj_set_pos(s_action_panels[i], x, ACTION_BOTTOM_Y);
            lv_obj_set_size(s_action_panels[i], width, ACTION_HEIGHT);
        }
    } else {
        panel_height = 134;
        for (int i = 0; i < s_action_count; ++i) {
            bool full_last = s_action_count == 3 && i == 2;
            int x = full_last ? 12 : 12 + (i % 2) * 110;
            int y = full_last ? ACTION_BOTTOM_Y
                              : ACTION_TOP_Y + (i / 2) * 40;
            lv_obj_set_pos(s_action_panels[i], x, y);
            lv_obj_set_size(s_action_panels[i], full_last ? 216 : 106,
                            ACTION_HEIGHT);
        }
    }

    if (s_action_count > 0 && s_selected >= s_action_count) {
        s_selected = s_action_count - 1;
    }
    lv_obj_set_height(s_body_panel, panel_height);
    s_body_height = panel_height - 54;
    lv_obj_set_height(s_body, s_body_height);
    lv_obj_align(s_meta_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    for (int i = 0; i < s_action_count; ++i) {
        lv_obj_center(s_action_labels[i]);
        ui_pixel_set_selected(s_action_panels[i], s_selected == i, true);
    }
}

static int catalog_total(void)
{
    return s_catalog_mode == CATALOG_THREADS
               ? s_catalog_count + 1
               : s_catalog_count;
}

static const char *catalog_title_at(int index)
{
    if (s_catalog_mode == CATALOG_THREADS) {
        if (index == 0) return "新建任务";
        --index;
    }
    return index >= 0 && index < s_catalog_count
               ? s_catalog[index].title
               : "";
}

static int catalog_title_lines(const char *title)
{
    int units = 2; /* selection marker */
    const unsigned char *cursor = (const unsigned char *)title;
    while (*cursor != '\0') {
        if (*cursor < 0x80U) {
            ++units;
            ++cursor;
        } else {
            units += 2;
            ++cursor;
            while ((*cursor & 0xC0U) == 0x80U) ++cursor;
        }
    }
    int lines = (units + CATALOG_LINE_UNITS - 1) / CATALOG_LINE_UNITS;
    return lines > 0 ? lines : 1;
}

static void catalog_window_bounds(int total, int *start_out, int *end_out)
{
    int start = s_catalog_window_start;
    if (start < 0 || start >= total || s_catalog_selected < start) {
        start = s_catalog_selected;
    }

    for (;;) {
        int used = 0;
        for (int i = start; i <= s_catalog_selected; ++i) {
            used += catalog_title_lines(catalog_title_at(i));
        }
        if (used <= CATALOG_LINE_BUDGET || start >= s_catalog_selected) break;
        ++start;
    }

    int end = start;
    int used = 0;
    while (end < total) {
        int lines = catalog_title_lines(catalog_title_at(end));
        if (used > 0 && used + lines > CATALOG_LINE_BUDGET) break;
        used += lines;
        ++end;
    }
    s_catalog_window_start = start;
    *start_out = start;
    *end_out = end;
}

static void catalog_detail(char *output, size_t output_size)
{
    output[0] = '\0';
    if (s_catalog_mode == CATALOG_LOADING_PROJECTS && s_catalog_count == 0) {
        snprintf(output, output_size, "正在读取真实 Codex 项目…");
        return;
    }
    if (s_catalog_mode == CATALOG_LOADING_THREADS && s_catalog_count == 0) {
        snprintf(output, output_size, "正在读取这个项目的任务…");
        return;
    }

    int total = catalog_total();
    if (total <= 0) {
        snprintf(output, output_size, "没有找到可以打开的项目。");
        return;
    }

    int start;
    int end;
    catalog_window_bounds(total, &start, &end);
    size_t used = 0U;
    for (int i = start; i < end && used + 1U < output_size; ++i) {
        int written = snprintf(output + used, output_size - used,
                               "%s%s%s",
                               i == s_catalog_selected ? "> " : "  ",
                               catalog_title_at(i),
                               i + 1 < end ? "\n" : "");
        if (written < 0) break;
        if ((size_t)written >= output_size - used) break;
        used += (size_t)written;
    }
}

static void render_catalog(void)
{
    bool projects = s_catalog_mode == CATALOG_LOADING_PROJECTS ||
                    s_catalog_mode == CATALOG_PROJECTS;
    int total = catalog_total();
    char position[48];
    catalog_detail(s_detail, sizeof(s_detail));
    lv_label_set_text(s_stage, projects ? "选择项目" : "选择任务");
    lv_label_set_text(s_pet_phrase, projects ? "要去哪里？" : "继续哪个？");
    lv_obj_set_style_text_color(s_stage, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_text_color(s_pet_phrase, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_border_color(s_body_panel, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_border_color(s_pet_phrase_panel, lv_color_hex(UI_PRIMARY), 0);
    for (int i = 0; i < 2; ++i) {
        lv_obj_set_style_bg_color(s_pet_phrase_tail[i], lv_color_hex(UI_PRIMARY), 0);
    }
    s_page = 0U;
    set_actions(NULL, NULL, NULL, NULL);
    s_page_count = 1U;
    lv_label_set_text(s_body, s_detail);
    if (total > 0) {
        int start;
        int end;
        catalog_window_bounds(total, &start, &end);
        snprintf(position, sizeof(position), "%d-%d/%d", start + 1, end, total);
        lv_label_set_text(s_page_label, position);
    } else {
        lv_label_set_text(s_page_label, "");
    }
    lv_label_set_text(s_meta,
                      s_catalog_mode == CATALOG_PROJECTS ||
                              s_catalog_mode == CATALOG_THREADS
                          ? "上下选择 · OK确认"
                          : "请稍候");
    lv_obj_set_style_bg_color(s_body_panel, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_color(s_meta_bar, lv_color_hex(UI_SURFACE), 0);
    record_visual_set(false);
    pet_animate();
}

static void coding_render(void)
{
    char *detail = NULL;
    uint32_t panel_color = UI_SURFACE;
    const char *a0 = NULL;
    const char *a1 = NULL;
    const char *a2 = NULL;
    const char *a3 = NULL;

    if (s_screen == NULL || s_workspace == NULL) {
        return;
    }
    detail = s_render_detail;
    app_power_hold_awake(s_flow.stage == CODING_STAGE_RECORDING);
    if (s_catalog_mode != CATALOG_NONE) {
        render_catalog();
        return;
    }
    bool stage_changed = !s_have_rendered_stage || s_rendered_stage != s_flow.stage;
    if (stage_changed) {
        s_cancel_confirm = false;
        uint32_t now = lv_tick_get();
        s_selected = 0;
        s_page = 0U;
        if (s_flow.stage == CODING_STAGE_RECORDING) s_record_started_ms = now;
        if (s_flow.stage == CODING_STAGE_TRANSCRIBING) s_transcribing_started_ms = now;
        if (s_flow.stage == CODING_STAGE_QUEUED &&
            strcmp(s_flow.task_status, "正在发送到电脑端") == 0) {
            s_send_started_ms = now;
        } else if (s_flow.stage != CODING_STAGE_QUEUED) {
            s_send_started_ms = 0U;
        }
        if (s_flow.stage == CODING_STAGE_QUEUED ||
            s_flow.stage == CODING_STAGE_RUNNING ||
            s_flow.stage == CODING_STAGE_APPROVAL_REVIEW) {
            if (s_flow.stage == CODING_STAGE_QUEUED &&
                strcmp(s_flow.task_status, "正在发送到电脑端") == 0) {
                s_has_remote_task_elapsed = false;
                s_last_task_elapsed_ms = 0U;
            }
            if ((s_flow.stage == CODING_STAGE_RUNNING ||
                 s_flow.stage == CODING_STAGE_APPROVAL_REVIEW) &&
                s_task_started_ms == 0U) {
                s_task_started_ms = now;
            }
        }
        if (s_flow.stage == CODING_STAGE_DONE || s_flow.stage == CODING_STAGE_ERROR) {
            if (s_has_remote_task_elapsed) {
                s_task_started_ms = 0U;
            } else if (s_task_started_ms != 0U) {
                s_last_task_elapsed_ms = now - s_task_started_ms;
                s_task_started_ms = 0U;
            }
        }
        if (s_flow.stage == CODING_STAGE_IDLE || s_flow.stage == CODING_STAGE_OFFLINE) {
            s_task_started_ms = 0U;
        }
        s_rendered_stage = s_flow.stage;
        s_have_rendered_stage = true;

        // 更新首页机器人状态显示
        const char *status_text = NULL;
        switch (s_flow.stage) {
            case CODING_STAGE_OFFLINE:     status_text = "离线"; break;
            case CODING_STAGE_IDLE:        status_text = "空闲"; break;
            case CODING_STAGE_RECORDING:   status_text = "录音中"; break;
            case CODING_STAGE_TRANSCRIBING: status_text = "识别中"; break;
            case CODING_STAGE_VOICE_REVIEW: status_text = "确认中"; break;
            case CODING_STAGE_QUEUED:      status_text = "排队中"; break;
            case CODING_STAGE_RUNNING:     status_text = "工作中"; break;
            case CODING_STAGE_APPROVAL_REVIEW:
            case CODING_STAGE_APPROVAL_CONFIRM: status_text = "待审批"; break;
            case CODING_STAGE_DONE:        status_text = "任务完成"; break;
            case CODING_STAGE_ERROR:       status_text = "任务失败"; break;
        }
        if (status_text) {
            radio_home_set_codex_status(status_text);
        }
    }

    switch (s_flow.stage) {
    case CODING_STAGE_OFFLINE:
        snprintf(detail, sizeof(s_render_detail),
                 "电脑端连接已中断。请重新启动桥接，任务和审批会在恢复后继续显示。");
        break;
    case CODING_STAGE_IDLE:
        // 有"最后一段文本"时优先展示它(用户要求: 点开会话只看最后一段,
        // 不加载整段对话)。文本较长时由 render_body_page 自动分页。
        if (s_last_message[0] != '\0') {
            // 用 snprintf 返回值累加偏移, 避免编译器误判溢出(-Werror=format-truncation)
            const int head = snprintf(detail, sizeof(s_render_detail), "%s",
                                      s_last_message);
            if (head > 0 && (size_t)head < sizeof(s_render_detail) - 4) {
                snprintf(detail + head, sizeof(s_render_detail) - (size_t)head,
                         "\n— %s",
                         s_current_task[0] != '\0' ? s_current_task : "未命名任务");
            }
        } else {
            snprintf(detail, sizeof(s_render_detail),
                     "当前任务：%s\n项目：%s\n按 OK 开始说话。",
                     s_current_task[0] != '\0' ? s_current_task : "尚未选择",
                     s_current_project[0] != '\0' ? s_current_project : "尚未选择");
        }
        a0 = "开始录音";
        a1 = "切换任务";
        break;
    case CODING_STAGE_RECORDING:
        snprintf(detail, sizeof(s_render_detail), "正在认真听你说话…");
        a0 = "结束录音";
        panel_color = UI_SOFT;
        break;
    case CODING_STAGE_TRANSCRIBING:
        snprintf(detail, sizeof(s_render_detail), "录音完成，正在等待文字识别结果。");
        a0 = "取消识别";
        break;
    case CODING_STAGE_VOICE_REVIEW:
        snprintf(detail, sizeof(s_render_detail), "%s", s_flow.transcript);
        a0 = "发送指令";
        a1 = "重新录音";
        a2 = "取消";
        panel_color = UI_SOFT;
        break;
    case CODING_STAGE_QUEUED:
    case CODING_STAGE_RUNNING:
        if (s_cancel_confirm) {
            snprintf(detail, sizeof(s_render_detail),
                     "确定停止当前任务？\n返回菜单不会停止任务。");
            a0 = "继续任务";
            a1 = "确认停止";
        } else {
            snprintf(detail, sizeof(s_render_detail), "%s", s_flow.task_status);
            a0 = "切换任务";
            a1 = "返回菜单";
            a2 = "停止任务";
        }
        break;
    case CODING_STAGE_APPROVAL_REVIEW:
        snprintf(detail, sizeof(s_render_detail), "%s\n%s",
                 s_flow.approval_question, s_flow.approval_detail);
        s_selected = (int)s_flow.decision;
        a0 = "仅本次";
        a1 = "本次会话";
        a2 = "拒绝";
        a3 = "取消任务";
        panel_color = UI_SOFT;
        break;
    case CODING_STAGE_APPROVAL_CONFIRM:
        snprintf(detail, sizeof(s_render_detail), "你选择了：%s。确认后才会发送。",
                 coding_flow_decision_name(s_flow.decision));
        a0 = "确认发送";
        a1 = "返回修改";
        panel_color = UI_SOFT;
        break;
    case CODING_STAGE_DONE:
        snprintf(detail, sizeof(s_render_detail), "%s", s_flow.task_status);
        a0 = "再次录音";
        a1 = "切换任务";
        panel_color = UI_SOFT;
        break;
    case CODING_STAGE_ERROR:
        snprintf(detail, sizeof(s_render_detail), "%s", s_flow.task_status);
        a0 = "重新录音";
        a1 = "切换任务";
        break;
    default:
        snprintf(detail, sizeof(s_render_detail), "未知状态");
        break;
    }

    int selected = s_selected;
    set_actions(a0, a1, a2, a3);
    size_t estimated_pages = body_page_count(detail);
    if (s_flow.stage == CODING_STAGE_VOICE_REVIEW) {
        if (estimated_pages > 1U) {
            /* Four actions use two rows, so measure the actual smaller body. */
            set_actions(a0, a1, a2, "下一页");
            estimated_pages = body_page_count(detail);
            a3 = s_page + 1U >= estimated_pages ? "回第一页" : "下一页";
        }
    } else if (s_flow.stage == CODING_STAGE_DONE ||
               s_flow.stage == CODING_STAGE_ERROR ||
               s_flow.stage == CODING_STAGE_IDLE) {
        if (estimated_pages > 1U) {
            a0 = "录音";
            a1 = "切换";
            a2 = s_page + 1U >= estimated_pages ? "首 页" : "翻 页";
        }
    } else if ((s_flow.stage == CODING_STAGE_RUNNING ||
                s_flow.stage == CODING_STAGE_QUEUED) && !s_cancel_confirm &&
               estimated_pages > 1U) {
        a0 = s_page + 1U >= estimated_pages ? "回第一页" : "下一页";
        a1 = "切换任务";
        a2 = "停止任务";
        a3 = NULL;
        /* Paging promotion changes the action labels after the first layout pass. */
        set_actions(a0, a1, a2, a3);
    }

    const char *title = s_flow.stage == CODING_STAGE_ERROR
                            ? coding_flow_error_title(s_flow.task_status)
                            : coding_flow_stage_name(s_flow.stage);
    lv_label_set_text(s_stage, title);
    lv_label_set_text(s_pet_phrase, pet_phrase(s_flow.stage));
    uint32_t accent = stage_accent(s_flow.stage);
    lv_obj_set_style_text_color(s_stage, lv_color_hex(accent), 0);
    lv_obj_set_style_text_color(s_pet_phrase, lv_color_hex(accent), 0);
    lv_obj_set_style_border_color(s_body_panel, lv_color_hex(accent), 0);
    lv_obj_set_style_border_color(s_pet_phrase_panel, lv_color_hex(accent), 0);
    for (int i = 0; i < 2; ++i) {
        lv_obj_set_style_bg_color(s_pet_phrase_tail[i], lv_color_hex(accent), 0);
    }
    /* s_detail and detail intentionally share one workspace buffer now. */
    if ((s_flow.stage == CODING_STAGE_DONE || s_flow.stage == CODING_STAGE_ERROR) &&
        s_active_message_id[0] != '\0') {
        codex_read_tracker_visit(s_active_message_id, "", lv_tick_get());
    } else {
        codex_read_tracker_reset();
    }
    s_selected = selected;
    set_actions(a0, a1, a2, a3);
    render_body_page();
    update_meta();
    lv_obj_set_style_bg_color(s_body_panel, lv_color_hex(panel_color), 0);
    lv_obj_set_style_bg_color(s_meta_bar, lv_color_hex(panel_color), 0);
    record_visual_set(s_flow.stage == CODING_STAGE_RECORDING);
    pet_animate();
    if (s_flow.stage == CODING_STAGE_DONE || s_flow.stage == CODING_STAGE_ERROR) {
        s_local_unread = false;
        if (s_result_thread[0] && s_result_turn[0] && s_flow.connected &&
            coding_bridge_result_seen(s_result_thread, s_result_turn)) {
            s_result_thread[0] = '\0';
            s_result_turn[0] = '\0';
        }
    }
}

void app_coding_set_audio_available(bool available)
{
    s_audio_available = available;
}

bool app_coding_is_connected(void)
{
    return s_flow.connected;
}

void app_coding_bridge_connected(bool connected)
{

    app_power_activity();
    ensure_initialized();
    s_pet_state.connected = connected;
    coding_flow_set_connected(&s_flow, connected);
    if (!connected) {
        if (s_catalog_mode != CATALOG_NONE) coding_bridge_cancel_catalog();
        s_selecting_thread = false;
        s_catalog_mode = CATALOG_NONE;
    }
    coding_render_deferred();
}

void app_coding_catalog_begin(const char *scope)
{
    ensure_initialized();
    bool threads = scope != NULL && strcmp(scope, "threads") == 0;
    if (s_catalog_mode != (threads ? CATALOG_LOADING_THREADS :
                                     CATALOG_LOADING_PROJECTS)) return;
    s_catalog_count = 0;
    s_catalog_selected = 0;
    s_catalog_window_start = 0;
    s_catalog_input_unlock_ms = lv_tick_get() + CATALOG_INPUT_GUARD_MS;
    coding_render_deferred();
}

void app_coding_catalog_item(const char *scope, const char *id,
                             const char *title, const char *detail)
{
    (void)detail;
    bool threads = scope != NULL && strcmp(scope, "threads") == 0;
    if ((threads && s_catalog_mode != CATALOG_LOADING_THREADS) ||
        (!threads && s_catalog_mode != CATALOG_LOADING_PROJECTS) ||
        s_catalog_count >= CATALOG_MAX) {
        return;
    }
    catalog_item_t *item = &s_catalog[s_catalog_count++];
    snprintf(item->id, sizeof(item->id), "%s", id != NULL ? id : "");
    snprintf(item->title, sizeof(item->title), "%s",
             title != NULL ? title : "未命名");
}

void app_coding_catalog_end(const char *scope)
{
    bool threads = scope != NULL && strcmp(scope, "threads") == 0;
    if (s_catalog_mode != (threads ? CATALOG_LOADING_THREADS :
                                     CATALOG_LOADING_PROJECTS)) return;
    s_catalog_mode = threads ? CATALOG_THREADS : CATALOG_PROJECTS;
    s_catalog_selected = 0;
    s_catalog_window_start = 0;
    s_catalog_input_unlock_ms = lv_tick_get() + CATALOG_INPUT_GUARD_MS;
    coding_render_deferred();
}

void app_coding_task_selected(const char *title, const char *project,
                              const char *last_message)
{
    if (!s_selecting_thread) {
        /* Selection responses end with codex_state. Ignore the entire canceled
         * response, including its optional pending result/running status. */
        s_discard_selection_updates = true;
        return;
    }
    s_selecting_thread = false;
    s_discard_selection_updates = false;
    release_project_cache();
    coding_bridge_confirm_selection();
    app_power_activity();
    ensure_initialized();
    snprintf(s_current_task, sizeof(s_current_task), "%s",
             title != NULL ? title : "未命名任务");
    snprintf(s_current_project, sizeof(s_current_project), "%s",
             project != NULL ? project : "未分组项目");
    // 会话最后一段文本(电脑端只下发这一段, 不传整段对话)。
    // Only keep a preview. A full thread message can be kilobytes of CJK,
    // and laying that out plus flushing a 240x320 frame from the LVGL task
    // ran past the 5 s task watchdog. The body is paged anyway, so the screen
    // only needs the first line or two.
    snprintf(s_last_message, sizeof(s_last_message), "%.220s",
             last_message != NULL ? last_message : "");
    // 便于排查"点开会话只拿到最后一段"这条链路(日志里能看到是否带内容)。
    ESP_LOGI(kTag, "task selected: '%s' last_message=%u bytes",
             s_current_task, (unsigned)strlen(s_last_message));
    s_catalog_mode = CATALOG_NONE;
    s_flow.connected = true;
    s_flow.stage = CODING_STAGE_IDLE;
    s_result_thread[0] = '\0';
    s_result_turn[0] = '\0';
    coding_render_deferred();
}

void app_coding_bridge_transcript(const char *text)
{
    app_power_activity();
    ensure_initialized();
    coding_flow_set_transcript(&s_flow, text);
    coding_render_deferred();
}

void app_coding_bridge_task_queued(const char *text)
{
    app_power_activity();
    ensure_initialized();
    coding_flow_set_queued(&s_flow, text);
    coding_render_deferred();
}

void app_coding_bridge_task_status(const char *text)
{
    if (s_discard_selection_updates) return;
    ensure_initialized();
    coding_flow_set_task_status(&s_flow, text);
    coding_render_deferred();
}

void app_coding_bridge_approval(const char *question, const char *detail)
{
    app_power_activity();
    ensure_initialized();
    s_catalog_mode = CATALOG_NONE;
    coding_flow_show_approval(&s_flow, question, detail);
    coding_render_deferred();
    app_voice_mode_t mode = app_settings_voice_mode();
    if (mode == APP_VOICE_TONE) ui_sfx_play(UI_SFX_CONFIRM);
    if (mode == APP_VOICE_SPEECH) ui_sfx_play(UI_SFX_APPROVAL_SPEECH);
}

void app_coding_bridge_done(const char *text)
{
    app_coding_bridge_done_with_elapsed(text, 0U);
}

void app_coding_bridge_done_with_elapsed(const char *text, uint32_t elapsed_ms)
{
    if (s_discard_selection_updates) {
        s_result_thread[0] = '\0';
        s_result_turn[0] = '\0';
        return;
    }
    app_power_activity();
    ensure_initialized();
    s_local_unread = true;
    s_has_remote_task_elapsed = elapsed_ms > 0U;
    if (s_has_remote_task_elapsed) s_last_task_elapsed_ms = elapsed_ms;
    coding_flow_set_done(&s_flow, text);
    coding_render_deferred();
    app_voice_mode_t mode = app_settings_voice_mode();
    if (mode == APP_VOICE_SPEECH) ui_sfx_play(UI_SFX_DONE_SPEECH);
    else if (mode == APP_VOICE_TONE) ui_sfx_play(UI_SFX_CONFIRM);
}

void app_coding_bridge_error(const char *text)
{
    app_coding_bridge_error_with_elapsed(text, 0U);
}

void app_coding_bridge_error_with_elapsed(const char *text, uint32_t elapsed_ms)
{
    // While a thread pick is in flight the only outstanding request is the
    // optional thread.read metadata refresh. The selection already completed
    // from threads.list, so a host error on that refresh must not push the whole
    // UI into CODING_STAGE_ERROR and strand the user.
    if (s_discard_selection_updates || s_selecting_thread ||
        s_catalog_mode != CATALOG_NONE) {
        s_result_thread[0] = '\0';
        s_result_turn[0] = '\0';
        return;
    }
    app_power_activity();
    ensure_initialized();
    s_local_unread = true;
    s_has_remote_task_elapsed = elapsed_ms > 0U;
    if (s_has_remote_task_elapsed) s_last_task_elapsed_ms = elapsed_ms;
    coding_flow_set_error(&s_flow, text);
    coding_render_deferred();
    ui_sfx_play(UI_SFX_FAIL);
}

void app_coding_bridge_state(uint32_t running, uint32_t done, uint32_t failed)
{
    ensure_initialized();
    coding_pet_update(&s_pet_state, running, done, failed);
    s_discard_selection_updates = false;
}

void app_coding_result_identity(const char *thread_id, const char *turn_id)
{
    snprintf(s_result_thread, sizeof(s_result_thread), "%s", thread_id);
    snprintf(s_result_turn, sizeof(s_result_turn), "%s", turn_id);
}

void app_coding_remote_status(const char *text)
{
    app_power_activity();
    ensure_initialized();
    coding_flow_set_task_status(&s_flow, text);
    coding_render_deferred();
}

void app_coding_remote_task(const char *thread_id, const char *text,
                            const char *message_id)
{
    app_power_activity();
    ensure_initialized();
    snprintf(s_result_thread, sizeof(s_result_thread), "%s",
             thread_id != NULL ? thread_id : "");
    snprintf(s_active_message_id, sizeof(s_active_message_id), "%s",
             message_id != NULL ? message_id : "");
    s_read_count = 0U;
    coding_flow_set_task_status(&s_flow, text);
    coding_render_deferred();
}

void app_coding_remote_result(const char *thread_id, const char *status,
                              const char *text, const char *message_id)
{
    app_power_activity();
    ensure_initialized();
    snprintf(s_result_thread, sizeof(s_result_thread), "%s",
             thread_id != NULL ? thread_id : "");
    s_result_turn[0] = '\0';
    snprintf(s_active_message_id, sizeof(s_active_message_id), "%s",
             message_id != NULL ? message_id : "");
    s_read_count = 0U;
    if (status != NULL && strcmp(status, "failed") == 0) {
        coding_flow_set_error(&s_flow, text);
        ui_sfx_play(UI_SFX_FAIL);
    } else {
        coding_flow_set_done(&s_flow, text);
        app_voice_mode_t mode = app_settings_voice_mode();
        if (mode == APP_VOICE_SPEECH) ui_sfx_play(UI_SFX_DONE_SPEECH);
        else if (mode == APP_VOICE_TONE) ui_sfx_play(UI_SFX_CONFIRM);
    }
    coding_render_deferred();
}

void app_coding_capture_level(uint8_t level)
{
    s_capture_level = level > 100 ? 100 : level;
}

bool app_coding_ok_is_record_action(void)
{
    return s_catalog_mode == CATALOG_NONE &&
           (s_flow.stage == CODING_STAGE_RECORDING ||
           ((s_flow.stage == CODING_STAGE_IDLE ||
             s_flow.stage == CODING_STAGE_DONE ||
             s_flow.stage == CODING_STAGE_ERROR) && s_selected == 0));
}

void app_coding_display_active(bool active)
{
    if (s_record_timer == NULL) return;
    if (active) lv_timer_resume(s_record_timer);
    else lv_timer_pause(s_record_timer);
}

void app_coding_apply_read_state(const char *message_id, uint32_t read_count)
{
    if (message_id == NULL ||
        strcmp(message_id, s_active_message_id) != 0) {
        return;
    }
    s_read_count = read_count;
    if (s_meta != NULL) update_meta();
}

void app_coding_get_task_counts(uint32_t *running, uint32_t *done,
                                uint32_t *failed)
{
    if (running != NULL) *running = s_pet_state.running;
    if (done != NULL) *done = s_pet_state.done;
    if (failed != NULL) *failed = s_pet_state.failed;
}

// 首页宠物用的 Codex 状态查询(线程安全只读)
app_codex_state_t app_coding_get_pet_state(void)
{
    if (s_flow.connected &&
        (s_flow.stage == CODING_STAGE_APPROVAL_REVIEW ||
         s_flow.stage == CODING_STAGE_APPROVAL_CONFIRM ||
         s_flow.stage == CODING_STAGE_VOICE_REVIEW)) return APP_CODEX_ATTENTION;
    if (s_pet_state.synced) {
        return (app_codex_state_t)coding_pet_state(&s_pet_state);
    }
    if (s_local_unread && s_flow.stage == CODING_STAGE_ERROR) return APP_CODEX_FAIL;
    if (s_local_unread && s_flow.stage == CODING_STAGE_DONE) return APP_CODEX_DONE;
    if (!s_flow.connected) return APP_CODEX_OFFLINE;
    switch (s_flow.stage) {
    case CODING_STAGE_QUEUED:
    case CODING_STAGE_RUNNING:
    case CODING_STAGE_TRANSCRIBING:
        return APP_CODEX_BUSY;
    case CODING_STAGE_APPROVAL_REVIEW:
    case CODING_STAGE_APPROVAL_CONFIRM:
    case CODING_STAGE_VOICE_REVIEW:
        return APP_CODEX_ATTENTION;
    case CODING_STAGE_OFFLINE:
    case CODING_STAGE_IDLE:
    case CODING_STAGE_RECORDING:
    case CODING_STAGE_DONE:
    case CODING_STAGE_ERROR:
    default:
        return APP_CODEX_IDLE;
    }
}

void app_coding_enter(void)
{
    ensure_initialized();
    if (!coding_workspace_acquire(&s_workspace)) {
        ESP_LOGE(kTag, "full Codex workspace unavailable");
        return;
    }
    s_full_ui_active = true;

    // 进入 Codex 时更新首页状态
    radio_home_set_codex_status(s_flow.connected ? "工作中" : "离线");

    bool task_in_progress = s_flow.stage == CODING_STAGE_RECORDING ||
                            s_flow.stage == CODING_STAGE_TRANSCRIBING ||
                            s_flow.stage == CODING_STAGE_VOICE_REVIEW ||
                            s_flow.stage == CODING_STAGE_QUEUED ||
                            s_flow.stage == CODING_STAGE_RUNNING ||
                            s_flow.stage == CODING_STAGE_APPROVAL_REVIEW ||
                            s_flow.stage == CODING_STAGE_APPROVAL_CONFIRM;
    bool pending_results = s_pet_state.done || s_pet_state.failed;
    if (s_flow.connected && pending_results) {
        s_catalog_mode = CATALOG_NONE;
        s_selecting_thread = true;
        coding_bridge_request_review();
    } else if (s_flow.connected && coding_bridge_capability_enabled("tasks") &&
               !task_in_progress && !s_local_unread) {
        /* Opening Coding Companion always lands on a fresh project catalog.
         * No navigation key is required to reveal the first project. */
        s_catalog_mode = CATALOG_LOADING_PROJECTS;
        s_catalog_count = 0;
        s_catalog_selected = 0;
        s_catalog_window_start = 0;
        coding_bridge_request_projects();
    }

    s_have_rendered_stage = false;
    s_detail[0] = '\0';
    s_page = 0U;
    s_page_count = 1U;
    s_record_frame = 0U;
    s_record_started_ms = 0U;
    s_task_started_ms = 0U;
    s_last_task_elapsed_ms = 0U;
    s_has_remote_task_elapsed = false;
    s_read_count = 0U;
    s_read_ack_message_id[0] = '\0';
    codex_read_tracker_reset();
    s_screen = ui_pixel_bare_screen_create();
    pet_create(s_screen);

    s_pet_phrase_panel = ui_pixel_panel_create(s_screen, 116, 10, 112, 58,
                                               UI_SURFACE);
    lv_obj_set_style_radius(s_pet_phrase_panel, 8, 0);
    s_pet_phrase = ui_pixel_label(s_pet_phrase_panel, "", ui_cn_font(), UI_PRIMARY);
    lv_label_set_long_mode(s_pet_phrase, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_pet_phrase, 92);
    lv_obj_set_style_text_align(s_pet_phrase, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_pet_phrase);
    s_pet_phrase_tail[0] = record_block(s_screen, 8, 6, UI_PRIMARY);
    lv_obj_set_pos(s_pet_phrase_tail[0], 108, 31);
    s_pet_phrase_tail[1] = record_block(s_screen, 6, 6, UI_PRIMARY);
    lv_obj_set_pos(s_pet_phrase_tail[1], 112, 27);

    s_body_panel = ui_pixel_panel_create(s_screen, 12, BODY_PANEL_Y, 216, 174,
                                         UI_SURFACE);
    lv_obj_set_style_radius(s_body_panel, 8, 0);
    s_stage = ui_pixel_label(s_body_panel, "", ui_cn_font(), UI_PRIMARY);
    lv_obj_align(s_stage, LV_ALIGN_TOP_LEFT, 0, 0);
    s_page_label = ui_pixel_label(s_body_panel, "", ui_cn_font(), UI_MUTED);
    lv_obj_align(s_page_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    s_body = ui_pixel_label(s_body_panel, "", ui_cn_font(), UI_INK);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body, 194);
    lv_obj_set_height(s_body, 46);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, 20);

    s_meta_bar = record_block(s_body_panel, 194, 18, UI_SURFACE);
    lv_obj_align(s_meta_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    s_meta = ui_pixel_label(s_meta_bar, "", ui_cn_font(), UI_MUTED);
    lv_obj_align(s_meta, LV_ALIGN_LEFT_MID, 0, 0);

    s_record_dot = record_block(s_body_panel, 9, 9, UI_DANGER);
    lv_obj_align(s_record_dot, LV_ALIGN_TOP_RIGHT, 0, 0);
    for (int i = 0; i < WAVE_BAR_COUNT; ++i) {
        s_wave_bars[i] = record_block(s_body_panel, 8, 6,
                                      i == WAVE_BAR_COUNT / 2
                                          ? UI_ACCENT : UI_PRIMARY);
    }
    record_visual_set(false);
    s_record_timer = lv_timer_create(record_animate, 100, NULL);

    for (int i = 0; i < ACTION_COUNT_MAX; ++i) {
        s_action_panels[i] = ui_pixel_panel_create(s_screen, 12,
                                                   ACTION_BOTTOM_Y, 106,
                                                   ACTION_HEIGHT,
                                                   UI_SURFACE);
        lv_obj_set_style_radius(s_action_panels[i], 6, 0);
        s_action_labels[i] = ui_pixel_label(s_action_panels[i], "",
                                            ui_cn_font(), UI_INK);
        lv_obj_set_style_text_align(s_action_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_action_labels[i]);
    }
    coding_render();
    lv_screen_load(s_screen);
}

void app_coding_exit(void)
{
    s_full_ui_active = false;
    codex_read_tracker_reset();
    release_project_cache();
    coding_bridge_cancel_catalog();
    s_catalog_mode = CATALOG_NONE;
    s_selecting_thread = false;
    if (s_flow.stage == CODING_STAGE_RECORDING ||
        s_flow.stage == CODING_STAGE_TRANSCRIBING) {
        coding_bridge_cancel_record();
        coding_flow_end_recording(&s_flow);
        coding_flow_cancel_voice(&s_flow);
    }
    if (s_record_timer != NULL) {
        lv_timer_delete(s_record_timer);
        s_record_timer = NULL;
    }
    if (s_screen != NULL) {
        lv_obj_delete(s_screen);
    }
    s_screen = NULL;
    s_stage = NULL;
    s_body_panel = NULL;
    s_body = NULL;
    s_meta_bar = NULL;
    s_meta = NULL;
    s_page_label = NULL;
    s_pet_phrase_panel = NULL;
    s_pet_phrase = NULL;
    s_record_dot = NULL;
    for (int i = 0; i < 2; ++i) {
        s_pet_phrase_tail[i] = NULL;
    }
    memset(&s_pet_ui, 0, sizeof(s_pet_ui));
    for (int i = 0; i < WAVE_BAR_COUNT; ++i) s_wave_bars[i] = NULL;
    for (int i = 0; i < ACTION_COUNT_MAX; ++i) {
        s_action_panels[i] = NULL;
        s_action_labels[i] = NULL;
    }

    coding_workspace_release(&s_workspace);

    // 退出 Codex 时恢复首页状态为"空闲"
    radio_home_set_codex_status("空闲");
}

static void open_projects(void)
{
    const bool cap = coding_bridge_capability_enabled("tasks");
    ESP_LOGI(kTag, "open_projects cap=%d stage=%d sel=%d", (int)cap,
             (int)s_flow.stage, s_selected);
    if (!cap) return;
    s_cancel_confirm = false;
    release_project_cache();
    s_catalog_return_page = s_page;
    s_catalog_return_selected = s_selected;
    s_projects_count = 0;
    s_selecting_thread = false;
    s_catalog_mode = CATALOG_LOADING_PROJECTS;
    s_catalog_count = 0;
    s_catalog_selected = 0;
    s_catalog_window_start = 0;
    coding_bridge_request_projects();
}

bool app_coding_back(void)
{
    if (s_cancel_confirm) {
        s_cancel_confirm = false;
        s_selected = 0;
        coding_render();
        return true;
    }
    if (s_catalog_mode == CATALOG_THREADS ||
        s_catalog_mode == CATALOG_LOADING_THREADS) {
        coding_bridge_cancel_catalog();
        s_selecting_thread = false;
        s_catalog_mode = s_projects ? CATALOG_PROJECTS : CATALOG_LOADING_PROJECTS;
        if (s_projects) {
            memcpy(s_catalog, s_projects, sizeof(s_catalog));
            release_project_cache();
        } else {
            coding_bridge_request_projects();
        }
        s_catalog_count = s_projects_count;
        s_catalog_selected = s_projects_selected;
        s_catalog_window_start = s_projects_window_start;
        coding_render();
        return true;
    }
    if (s_catalog_mode != CATALOG_NONE) {
        release_project_cache();
        coding_bridge_cancel_catalog();
        s_catalog_mode = CATALOG_NONE;
        coding_render();
        s_page = s_catalog_return_page;
        s_selected = s_catalog_return_selected;
        coding_render();
        return true;
    }
    return false;
}

void app_coding_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    coding_action_t action = CODING_ACTION_NONE;
    if (event == BSP_BTN_CLICK && button == BSP_BTN_OK) {
        ESP_LOGI(kTag, "OK stage=%d sel=%d actions=%d catalog=%d",
                 (int)s_flow.stage, s_selected, s_action_count, (int)s_catalog_mode);
    }

    if (s_catalog_mode != CATALOG_NONE) {
        if (event == BSP_BTN_CLICK &&
            (int32_t)(s_catalog_input_unlock_ms - lv_tick_get()) > 0) {
            return;
        }
        int total = catalog_total();
        if (event == BSP_BTN_CLICK && total > 0 &&
            (button == BSP_BTN_UP || button == BSP_BTN_DOWN)) {
            int delta = button == BSP_BTN_UP ? total - 1 : 1;
            s_catalog_selected = (s_catalog_selected + delta) % total;
            s_catalog_input_unlock_ms =
                lv_tick_get() + CATALOG_INPUT_GUARD_MS;
        } else if (event == BSP_BTN_CLICK && button == BSP_BTN_OK) {
            s_catalog_input_unlock_ms =
                lv_tick_get() + CATALOG_INPUT_GUARD_MS;
            if (s_catalog_mode == CATALOG_PROJECTS && total > 0) {
                char id[CATALOG_ID_MAX];
                snprintf(id, sizeof(id), "%s", s_catalog[s_catalog_selected].id);
                if (!s_projects) s_projects = malloc(sizeof(s_catalog));
                if (s_projects) memcpy(s_projects, s_catalog, sizeof(s_catalog));
                s_projects_count = s_catalog_count;
                s_projects_selected = s_catalog_selected;
                s_projects_window_start = s_catalog_window_start;
                s_catalog_mode = strcmp(id, "p_more") == 0
                                     ? CATALOG_LOADING_PROJECTS : CATALOG_LOADING_THREADS;
                s_catalog_count = 0;
                s_catalog_selected = 0;
                s_catalog_window_start = 0;
                coding_bridge_select_project(id);
            } else if (s_catalog_mode == CATALOG_THREADS) {
                s_selecting_thread = true;
                if (s_catalog_selected == 0) {
                    s_catalog_mode = CATALOG_LOADING_THREADS;
                    coding_bridge_create_thread();
                } else {
                    char id[CATALOG_ID_MAX];
                    snprintf(id, sizeof(id), "%s",
                             s_catalog[s_catalog_selected - 1].id);
                    s_catalog_mode = CATALOG_LOADING_THREADS;
                    coding_bridge_select_thread(id);
                }
            }
        }
        coding_render();
        return;
    }

    if (event == BSP_BTN_CLICK &&
               (button == BSP_BTN_UP || button == BSP_BTN_DOWN)) {
        if (s_flow.stage == CODING_STAGE_APPROVAL_REVIEW) {
            if (button == BSP_BTN_UP) {
                coding_flow_previous_decision(&s_flow);
            } else {
                coding_flow_next_decision(&s_flow);
            }
            s_selected = (int)s_flow.decision;
        } else if (s_action_count > 0) {
            int delta = button == BSP_BTN_UP ? s_action_count - 1 : 1;
            s_selected = (s_selected + delta) % s_action_count;
        }
    } else if (button == BSP_BTN_OK && event == BSP_BTN_CLICK) {
        switch (s_flow.stage) {
        case CODING_STAGE_OFFLINE:
            break;
        case CODING_STAGE_IDLE:
        case CODING_STAGE_ERROR:
            if (s_selected == 2 && s_page_count > 1U) {
                s_page = (s_page + 1U) % s_page_count;
            } else if (s_selected == 1) {
                open_projects();
            } else if (!s_audio_available) {
                coding_flow_set_error(&s_flow, "麦克风不可用");
            } else {
                action = coding_flow_begin_recording(&s_flow);
            }
            break;
        case CODING_STAGE_DONE:
            if (s_selected == 2 && s_page_count > 1U) {
                s_page = (s_page + 1U) % s_page_count;
            } else if (s_selected == 1) {
                open_projects();
            } else if (!s_audio_available) {
                coding_flow_set_error(&s_flow, "麦克风不可用");
            } else {
                action = coding_flow_begin_recording(&s_flow);
            }
            break;
        case CODING_STAGE_RECORDING:
            action = coding_flow_end_recording(&s_flow);
            break;
        case CODING_STAGE_TRANSCRIBING:
            coding_bridge_cancel_record();
            coding_flow_cancel_voice(&s_flow);
            break;
        case CODING_STAGE_VOICE_REVIEW:
            if (s_selected == 0) action = coding_flow_confirm_voice(&s_flow);
            else if (s_selected == 1) coding_flow_retry_voice(&s_flow);
            else if (s_selected == 2) coding_flow_cancel_voice(&s_flow);
            else if (s_page_count > 1U) {
                s_page = (s_page + 1U) % s_page_count;
            }
            break;
        case CODING_STAGE_QUEUED:
        case CODING_STAGE_RUNNING:
            if (s_cancel_confirm) {
                if (s_selected == 1) action = coding_flow_cancel_task(&s_flow);
                s_cancel_confirm = false;
                s_selected = 0;
            } else if (s_page_count > 1U && s_selected == 0) {
                s_page = (s_page + 1U) % s_page_count;
            } else if ((s_page_count > 1U && s_selected == 1) ||
                       (s_page_count <= 1U && s_selected == 0)) {
                open_projects();
            } else if ((s_page_count > 1U && s_selected == 2) ||
                       (s_page_count <= 1U && s_selected == 1)) {
                if (s_page_count <= 1U) {
                    app_framework_go_menu();
                    return;
                }
                s_cancel_confirm = true;
                s_selected = 0;
            } else if (s_page_count <= 1U && s_selected == 2) {
                s_cancel_confirm = true;
                s_selected = 0;
            }
            break;
        case CODING_STAGE_APPROVAL_REVIEW:
            coding_flow_open_approval_confirm(&s_flow);
            break;
        case CODING_STAGE_APPROVAL_CONFIRM:
            if (s_selected == 0) action = coding_flow_confirm_approval(&s_flow);
            else coding_flow_back_to_approval(&s_flow);
            break;
        default:
            break;
        }
    }

    switch (action) {
    case CODING_ACTION_START_CAPTURE:
        release_project_cache();
        coding_bridge_send_record_start();
        break;
    case CODING_ACTION_STOP_CAPTURE:
        coding_bridge_send_record_stop();
        break;
    case CODING_ACTION_SEND_VOICE:
        if (!coding_bridge_send_voice(s_flow.transcript)) {
            s_send_started_ms = 0U;
            coding_flow_set_error(&s_flow, "指令没有发出，请检查连接后重试");
        }
        break;
    case CODING_ACTION_SEND_APPROVAL:
        coding_bridge_send_approval(s_flow.decision);
        break;
    case CODING_ACTION_CANCEL_TASK:
        coding_bridge_send_cancel();
        break;
    default:
        break;
    }
    coding_render();
}

bool app_coding_full_ready(void) { return s_screen != NULL; }

bool app_coding_full_ui_active(void) { return s_full_ui_active; }
