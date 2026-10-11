// components/bsp/src/bsp_button.c —— 三按键驱动（组件 ADC 驱动 + 显式时序）
//
// 【版本史与选择依据】
// 1. 自写轮询 oneshot 读: 实机 ADC 恒 2984mV, 按键无变化, 弃用
// 2. 原厂 button_level 自定义 driver: 实机 0 事件, 弃用
// 3. iot_button_new_adc_device 组件驱动: 实机验证 UP/DOWN/OK 三键事件全部正确
//    产生(KEY btn=0/1/2 ev=2), 【本文件采用此路径】
// 4. 自写混合数字/ADC 轮询: 多引脚轮询引入状态机卡死 bug(g1/g2 悬空读 0
//    导致永久卡在按住态), 全部实验代码已清除
//
// 时序不用组件 Kconfig 默认(长按 1500ms 对三键小设备太迟钝),
// 由 BSP 显式下发: 短按 180ms / 长按 500ms(见 bsp_pins.h)。

#include "bsp_button.h"
#include "bsp_pins.h"

#include <stdbool.h>
#include <string.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "button_adc.h"
#include "iot_button.h"
#include "esp_log.h"

static const char *TAG = "bsp_btn";

static const uint16_t BTN_MV[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static button_handle_t           s_btn[BSP_BTN_COUNT];
static bsp_btn_cb_t              s_user_cb;
static void                     *s_user_arg;

// 组件回调(esp_timer 任务上下文) → BSP 回调: usr_data 存的是按键索引
static void component_event_bridge(void *handle, void *usr_data) {
    const int idx = (int)(intptr_t)usr_data;
    if (!s_user_cb) return;
    const button_event_t ev = iot_button_get_event(handle);
    if (ev == BUTTON_SINGLE_CLICK) {
        s_user_cb((bsp_btn_t)idx, BSP_BTN_CLICK, s_user_arg);
    } else if (ev == BUTTON_LONG_PRESS_START) {
        s_user_cb((bsp_btn_t)idx, BSP_BTN_LONG, s_user_arg);
    }
    // PRESS_UP/PRESS_DOWN/HOLD 等事件上层不需要, 不转发
}

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user) {
    if (s_btn[0]) { s_user_cb = cb; s_user_arg = user; return ESP_OK; }

    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BSP_BTN_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    const adc_cali_curve_fitting_config_t cal = {
        .unit_id  = BSP_BTN_ADC_UNIT,
        .chan     = BSP_BTN_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGE(TAG, "ADC cali init failed");
        s_cali = NULL;
    }

    s_user_cb = cb;
    s_user_arg = user;

    // 组件内置 ADC 驱动: 连续采样 + 电压窗口判定(实机验证可用的路径)
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        const button_adc_config_t ac = {
            .adc_handle   = &s_adc,
            .unit_id      = BSP_BTN_ADC_UNIT,
            .adc_channel  = BSP_BTN_ADC_CHANNEL,
            .button_index = i,
            .min          = BTN_MV[i][0],
            .max          = BTN_MV[i][1],
        };
        const button_config_t bc = {
            .short_press_time = BSP_BTN_SHORT_PRESS_MS,
            .long_press_time  = BSP_BTN_LONG_PRESS_MS,
        };
        err = iot_button_new_adc_device(&bc, &ac, &s_btn[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "btn %d create failed: %s", i, esp_err_to_name(err));
            return err;
        }
        iot_button_register_cb(s_btn[i], BUTTON_SINGLE_CLICK, NULL,
                               component_event_bridge, (void *)(intptr_t)i);
        iot_button_register_cb(s_btn[i], BUTTON_LONG_PRESS_START, NULL,
                               component_event_bridge, (void *)(intptr_t)i);
    }

    ESP_LOGI(TAG, "按键就绪:ADC1_CH%d 三键分压,短按 %dms 长按 %dms(组件驱动)",
             (int)BSP_BTN_ADC_CHANNEL, (int)BSP_BTN_SHORT_PRESS_MS,
             (int)BSP_BTN_LONG_PRESS_MS);
    return ESP_OK;
}

int bsp_button_read_mv(void) {
    if (!s_adc || !s_cali) return -1;
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv;
}
