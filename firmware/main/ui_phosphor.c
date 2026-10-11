// main/ui_phosphor.c —— plays/438 荧光绿单色 UI 的【增量组件】
//
// ## 为什么这个文件只放一部分
//
// 第一次写的时候把 ui_pixel.c 的实现整份复制过来了,结果链接期报
// `multiple definition of ui_cn_font / ui_pixel_label / ...` —— 两个 .obj
// 各自导出了同名符号。共用组件只能有一份实现。
//
// 所以分工是:
//
//   ui_pixel.c     共用基元(bare_screen / panel / label / set_selected)
//                  配色已切到 plays/438 荧光绿,是全工程唯一实现
//   ui_phosphor.c  仅放 plays/438 特有形态(顶栏 / 分割线 / 像素块)
//
// 组件形态严格照 plays/438 主图:深绿面 + 1px 亮绿描边 + 4px 圆角,
// 全程不用阴影(用阴影就脱离"旧终端"的观感了)。层次只靠描边和明度差。
#include "ui_phosphor.h"

#include "ui_pixel.h"  // plain_box / 共用基元由 ui_pixel.c 提供

lv_obj_t *ui_phosphor_block_create(lv_obj_t *parent, int x, int y, int w, int h,
                                   uint32_t color)
{
    return plain_box(parent, x, y, w, h, color);
}

lv_obj_t *ui_phosphor_divider_create(lv_obj_t *parent, int x, int y, int w)
{
    // 1px 高的暗线。plays/438 用它分行,比留白更紧凑、也更"终端"。
    return plain_box(parent, x, y, w, 1, UI_BORDER);
}

lv_obj_t *ui_phosphor_header_create(lv_obj_t *parent, const char *title)
{
    lv_obj_t *header = plain_box(parent, 0, 0, 240, UI_HEADER_H, UI_BG);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // 左侧信号格:四格高度递增,【底边对齐】向右上爬升 —— plays/438 的样子。
    // 之前写的是顶边对齐(y 都从 8 起),视觉上就是"信号图标反了"。
    for (int i = 0; i < 4; ++i) {
        const int h = 3 + i;
        plain_box(header, 10 + i * 4, 15 - h, 3, h, UI_WHITE);
    }
    lv_obj_t *label = ui_pixel_label(header, title, ui_cn_font(), UI_WHITE);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 32, 0);
    plain_box(header, 0, UI_HEADER_H - 1, 240, 1, UI_BORDER);
    return header;
}
