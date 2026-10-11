// main/ui_pixel.h —— FoloOS 像素风基础控件(RADIO + CODEX 版)
//
// 【裁剪说明】
// 相对 FoloOS 原版删去了三项,原因都是"本工程不存在调用者":
//   - ui_pixel_menu_screen_create(): 它是 FoloOS 主菜单的底图,依赖
//     ui_menu_chrome(一张 240x320 RGB565 位图,源码 947KB)。
//     LEO 的主界面由 radio_ui.cc 自己画,Codex 页用 bare_screen + panel,
//     故整条依赖链(含该位图)可以摘掉。
//   - ui_pixel_screen_create(): 带标题栏的通用页,无人调用。
//   - ui_menu_font(): 返回 lv_font_cn_20,而本工程并未移植该字体
//     (Codex 页与收音机页统一用 ui_cn_font() = lv_font_cn_16)。
//     保留它会直接导致链接期 undefined reference。
// 另:原文件里随屏幕创建附带电量挂件,只被上面两个函数使用,一并移除;
// Codex 页是 bare 屏,本来就不显示电量(收音机界面自带)。
#pragma once

// 实现是 C 文件(ui_pixel.c),调用方含 C++(radio_home.cc / radio_ui.cc)。
// 没有 extern "C" 守卫时,C++ 侧会找带 mangling 的名字,链接期报 undefined reference。
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "lvgl.h"

// 配色已切到 plays/438「口袋百宝箱」的荧光绿单色方案。
// 色值来源与设计理由见 ui_phosphor.h(逐像素采样官方主图得到,非猜测)。
// 保留本文件原有的宏名,是为了让 app_coding.c 的 38 处引用无需改动。
#define UI_BG       0x000F05  // 纯黑绿底
#define UI_SURFACE  0x0A251A  // 卡片面
#define UI_WHITE    0x9AFCC6  // 主前景
#define UI_INK      0x9AFCC6
#define UI_MUTED    0x5FD9A0  // 次要文字
#define UI_BORDER   0x2ECC7F  // 描边
#define UI_DISABLED 0x1A3A28
#define UI_PRIMARY  0x9AFCC6
#define UI_ACCENT   0xD8FF7A  // 选中/激活(偏黄的绿,保持同色相)
#define UI_SELECTED 0x14512F
#define UI_SOFT     0x123D28
#define UI_SUCCESS  0x9AFCC6
#define UI_WARNING  0xD8FF7A
#define UI_DANGER   0xFF9E9E  // 出错时才破例用偏红色

// 中文正文/按钮字体(lv_font_cn_16,GB2312 全集)。
const lv_font_t *ui_cn_font(void);
// 纯净空屏(仅底色)。Codex 页的容器。
lv_obj_t *ui_pixel_bare_screen_create(void);
// 带描边的面板。Codex 页用它拼出"台词气泡 / 正文区 / 动作按钮"。
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);
lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);
// 高亮/禁用某个面板(连同其子标签的字色)。
void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled);

// 无边框纯色方块。所有面板/分割线/像素块都由它拼出来,
// 故导出给 ui_phosphor.c 复用 —— 否则那里只能复制一份实现,
// 链接期就会报 multiple definition。
lv_obj_t *plain_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color);

#ifdef __cplusplus
}
#endif
