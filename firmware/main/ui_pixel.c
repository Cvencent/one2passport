#include "ui_pixel.h"

#include <stdio.h>

LV_FONT_DECLARE(lv_font_cn_16);

lv_obj_t *plain_box(lv_obj_t *parent, int x, int y, int w, int h,
                           uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

const lv_font_t *ui_cn_font(void)
{
    return &lv_font_cn_16;
}

lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *base_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    return screen;
}

lv_obj_t *ui_pixel_bare_screen_create(void)
{
    return base_screen_create();
}

lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color)
{
    lv_obj_t *panel = plain_box(parent, x, y, w, h, color);
    // plays/438 的卡片:4px 圆角 + 1px 亮绿描边。
    // 原版这里是 radius 0 / border 2,那是 LEO 的粗框风格,与目标观感不符。
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 6, 0);
    return panel;
}

void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled)
{
    uint32_t background = !enabled ? UI_DISABLED
                                   : (selected ? UI_SELECTED : UI_SURFACE);
    uint32_t foreground = !enabled ? UI_MUTED : (selected ? UI_ACCENT : UI_INK);
    lv_obj_set_style_bg_color(panel, lv_color_hex(background), 0);
    lv_obj_set_style_border_color(
        panel, lv_color_hex(enabled ? UI_BORDER : UI_DISABLED), 0);
    lv_obj_set_style_border_width(panel, 1, 0);

    uint32_t count = lv_obj_get_child_count(panel);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(panel, (int32_t)i);
        lv_obj_set_style_text_color(child, lv_color_hex(foreground), 0);
    }
}
