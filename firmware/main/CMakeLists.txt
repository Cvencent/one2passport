idf_component_register(
    SRCS "main.cc" "radio_catalog.cc" "radio_display_settings.cc"
         "radio_easter_egg.cc"
         "radio_player.cc" "radio_sleep_timer.cc"
         "radio_ui.cc"
         # ---- plays/438 风格首页(卡片网格 + 荧光绿)----
         # 与 radio_ui.cc 并存:后者是 LEO 原版电台界面,本文件是复刻 plays/438
         # 的卡片式首页。互不依赖,可单独摘除。
         "radio_home.cc"
         "radio_font.c" "radio_font_title.c" "radio_font_big.c" "fap_screenshot.c"
         # ---- Codex 监控页(移植自 FoloOS「口袋百宝箱」,已按要求裁剪)----
         "app_coding.c"
         "coding_bridge.c"
         "coding_flow.c" "serial_dispatch.c"
         "wireless_bridge.c"
         "ui_pixel.c" "ui_pixel_math.c" "ui_sfx.c"
         # ---- plays/438 荧光绿单色 UI 组件层 ----
         # 色值与设计规范见 ui_phosphor.h。ui_pixel.c/.h 保留原有宏名并已换配色,
         # 所以 app_coding.c 的引用无需改动;本文件提供顶栏/分割线/像素块等
         # plays/438 特有形态,供首页复刻布局时使用。
         "ui_phosphor.c"
         # ---- V6 通用应用框架(首页桌面/轮播菜单/应用注册表) ----
         "app_registry.cc" "app_apps.cc"
         "xiaozhi_client.c"
         "lv_font_cn_16.c"
    INCLUDE_DIRS "."
    REQUIRES
        bsp
        nvs_flash
        esp_http_client
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_lcd
        esp_partition
        # esp_ota_ops.h 属于 app_update 组件,不是 esp_partition。
        # coding_bridge.c 用 esp_ota_mark_app_valid_cancel_rollback() 与
        # esp_ota_begin/write/end 接收电脑端推送的固件。
        app_update
        esp_rom
        esp_websocket_client
        esp_audio_codec
        spi_flash
        mbedtls
        json
        esp_audio_codec
        esp_driver_usb_serial_jtag
        esp-wifi-connect
    # Codex 页在收到电脑端事件时会播放语音提示,这两个 WAV 被嵌入本组件,
    # 导出 _binary_approval_wav_start / _binary_done_wav_start 等符号供
    # ui_sfx.c 引用。原版 word_bear 的 20 个 WAV 已随该功能删除,不再嵌入。
    EMBED_FILES "audio/approval.wav" "audio/done.wav" "tenclass_ca.pem"
)

# lv_font_conv 生成的字体源码带双 include 守卫:
#   #ifdef LV_LVGL_H_INCLUDE_SIMPLE -> "lvgl.h"   (ESP-IDF 的实际路径)
#   #else                            -> "lvgl/lvgl.h" (不存在,会 fatal error)
# LEO 自带的 radio_font.c 两个分支都写成 "lvgl.h" 所以从未暴露这个问题;
# 新生成的 lv_font_cn_16.c 走的是 else 分支。这里沿用 FoloOS 上游的做法,
# 定义该宏选中正确分支,而不是去改生成物(便于日后重新生成字体)。
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
