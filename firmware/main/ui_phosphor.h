// main/ui_phosphor.h —— plays/438「口袋百宝箱」荧光绿单色 UI 规范
//
// ## 风格来源
//
// 目标 UI 来自社区玩法 438(plays/438,作者 Chun)。该玩法的说明里写明
// 「感谢 Leo 大佬的 LEO RADIO 城市电台项目」—— 所以这套 UI 与 LEO RADIO
// 同源,框架可以复用,差异主要在**配色**与**首页布局**:
//
//   LEO RADIO 原版:深蓝底 + 青色高亮(UI_PRIMARY 0x00D9FF)
//   plays/438     :纯黑底 + 荧光绿单色(UI_PRIMARY 0x9AFCC6)
//
// ## 色值来源(不是猜的)
//
// 从官方主图 https://ai-passport.folotoy.cn/api/media/plays/community-1353f966.webp
// 逐像素采样提取,方法:先按 G 通道显著高于 R/B 筛出全部绿色像素,
// 再取出现频次最高的簇。原始采样:
//
//   主前景  RGB(154,252,198)  出现 114 次  -> #9AFCC6
//   次要文字 RGB( 95,217,160)             -> #5FD9A0
//   卡片底  RGB( 10, 37, 26)               -> #0A251A
//   背景    RGB(  0, 15,  5)               -> #000F05
//
// 卡片描边色由前景压暗得到(保持同一色相,形成单色层次的层次感)。
//
// ## 为什么只用两三个明度层级
//
// 这种风格的说服力来自"像旧终端/电子墨水"——层��少、对比强、绝不用阴影。
// 所以全工程只有:纯黑底 -> 深绿面 -> 亮绿字,靠**描边**而非阴影分层。
#pragma once

// 实现是 C 文件(ui_phosphor.c),调用方含 C++(radio_home.cc / app_coding.c)。
// 缺了下面的守卫,C++ 侧会去找带 name mangling 的 _Z24ui_phosphor_block_create...,
// 而 C 侧导出的是无 mangling 的 ui_phosphor_block_create,链接期直接 undefined reference。
// 本工程已有 ui_sfx.h / wireless_bridge.h / coding_bridge.h 三处同类守卫,这里是第四处。
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "lvgl.h"

// ── 色板(plays/438 荧光绿)────────────────────────────────────────────────
#define UI_BG       0x000F05  // 纯黑绿底,最暗
#define UI_SURFACE  0x0A251A  // 卡片面
#define UI_SOFT     0x123D28  // 进度槽 / 次级填充
#define UI_SELECTED 0x14512F  // 选中卡片面
#define UI_WHITE    0x9AFCC6  // 主前景(亮绿)
#define UI_INK      0x9AFCC6  // 正文同主前景
#define UI_MUTED    0x5FD9A0  // 次要文字
#define UI_BORDER   0x2ECC7F  // 卡片描边
#define UI_PRIMARY  0x9AFCC6  // 高亮
#define UI_ACCENT   0x5FD9A0  // 强调(选中态文字)

// 语义色:仍取同一色相族,避免破坏单色感。仅在必要时用明度区分。
#define UI_SUCCESS  0x9AFCC6  // 绿即成功,不再引入第二种色相
#define UI_WARNING  0xD8FF7A  // 偏黄的绿
#define UI_DANGER   0xFF9E9E  // 出错时才破例,用偏红的浅色
#define UI_DISABLED 0x1A3A28  // 禁用态的面

// ── 组件尺寸(取自 plays/438 主图的比例)──────────────────────────────
// 屏幕 240x320。plays/438 的卡片间距约 8px,描边 1px,圆角 4px。
#define UI_RADIUS      4
#define UI_STROKE      1
#define UI_GAP         8
// 首页顶栏高度(信号条 + 标题)
#define UI_HEADER_H    22
// 首页卡片网格:两列
#define UI_GRID_X      11
#define UI_GRID_Y      52
#define UI_CARD_W      102
#define UI_CARD_H      40
#define UI_CARD_DX     112
#define UI_CARD_DY     47

// 中文正文/按钮字体(沿用工程内已有的 lv_font_cn_16,GB2312 全集)
const lv_font_t *ui_cn_font(void);

// ── 通用组件 ─────────────────────────────────────────────────────────

// 纯净空屏(仅底色)
lv_obj_t *ui_pixel_bare_screen_create(void);

// 带描边的面板。plays/438 的卡片全部是这个形态:
// 深绿面 + 1px 亮绿描边 + 4px 圆角,没有阴影。
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);

lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);

// 高亮/禁用某个面板(连同其子标签的字色)
void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled);

// 顶部信号条 + 电量,与 plays/438 顶栏一致。
// 放在屏幕顶端,返回该屏的根对象。
lv_obj_t *ui_phosphor_header_create(lv_obj_t *parent, const char *title);

// 细分割线。plays/438 用它分隔卡片内的信息行,不用留白。
lv_obj_t *ui_phosphor_divider_create(lv_obj_t *parent, int x, int y, int w);

// 像素方块(用于信号格、进度段、图标底)
lv_obj_t *ui_phosphor_block_create(lv_obj_t *parent, int x, int y, int w, int h,
                                   uint32_t color);

#ifdef __cplusplus
}
#endif
