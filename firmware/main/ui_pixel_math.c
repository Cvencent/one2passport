#include "ui_pixel_math.h"

#include <string.h>

int ui_pixel_blink_frame(uint32_t elapsed_ms)
{
    uint32_t phase = elapsed_ms % 2000U;
    return phase >= 1650U && phase < 1800U;
}

int ui_pixel_jump_offset(unsigned frame)
{
    static const int offsets[] = { 0, -3, -5, -3, 0 };
    return frame < sizeof(offsets) / sizeof(offsets[0]) ? offsets[frame] : 0;
}

ui_power_state_t ui_power_state_for_idle(uint32_t idle_ms, bool keep_awake)
{
    if (keep_awake || idle_ms < 30000U) return UI_POWER_ACTIVE;
    if (idle_ms < 120000U) return UI_POWER_DIMMED;
    return UI_POWER_DISPLAY_OFF;
}

uint8_t ui_power_dim_backlight(uint8_t normal_percent)
{
    uint8_t dimmed = (uint8_t)((normal_percent * 30U) / 100U);
    if (dimmed < 8U) dimmed = 8U;
    if (dimmed > 25U) dimmed = 25U;
    return dimmed;
}

static size_t utf8_width(unsigned char byte)
{
    if ((byte & 0x80U) == 0U) return 1U;
    if ((byte & 0xE0U) == 0xC0U) return 2U;
    if ((byte & 0xF0U) == 0xE0U) return 3U;
    if ((byte & 0xF8U) == 0xF0U) return 4U;
    return 1U;
}

size_t ui_pixel_text_fit_copy(char *output, size_t output_size, const char *text,
                             ui_text_fits_t fits, void *context)
{
    if (output == NULL || output_size == 0U) return 0U;
    output[0] = '\0';
    if (text == NULL || fits == NULL) return 0U;
    size_t written = 0U, remaining = strlen(text);
    while (written < remaining) {
        size_t width = utf8_width((unsigned char)text[written]);
        if (width > remaining - written) width = 1U;
        if (written + width >= output_size) break;
        memcpy(output + written, text + written, width);
        output[written + width] = '\0';
        if (written > 0U && !fits(output, context)) break;
        written += width;
    }
    output[written] = '\0';
    return written;
}

size_t ui_pixel_text_page_count(const char *text, size_t chars_per_page)
{
    if (text == NULL || text[0] == '\0' || chars_per_page == 0U) return 1U;
    size_t chars = 0U;
    for (size_t i = 0U; text[i] != '\0';) {
        size_t width = utf8_width((unsigned char)text[i]);
        size_t remaining = strlen(text + i);
        i += width <= remaining ? width : 1U;
        ++chars;
    }
    return (chars + chars_per_page - 1U) / chars_per_page;
}

size_t ui_pixel_text_page_copy(char *output, size_t output_size,
                               const char *text, size_t page,
                               size_t chars_per_page)
{
    if (output == NULL || output_size == 0U) return 0U;
    output[0] = '\0';
    if (text == NULL || chars_per_page == 0U) return 0U;

    size_t start = page * chars_per_page;
    size_t char_index = 0U;
    size_t input_index = 0U;
    while (text[input_index] != '\0' && char_index < start) {
        size_t width = utf8_width((unsigned char)text[input_index]);
        size_t remaining = strlen(text + input_index);
        input_index += width <= remaining ? width : 1U;
        ++char_index;
    }

    size_t written = 0U;
    size_t copied_chars = 0U;
    while (text[input_index] != '\0' && copied_chars < chars_per_page) {
        size_t width = utf8_width((unsigned char)text[input_index]);
        size_t remaining = strlen(text + input_index);
        if (width > remaining) width = 1U;
        if (written + width >= output_size) break;
        memcpy(output + written, text + input_index, width);
        written += width;
        input_index += width;
        ++copied_chars;
    }
    output[written] = '\0';
    return written;
}
