#include "ui_pixel_math.h"
#include <assert.h>
#include <string.h>

static bool fits_two_lines(const char *text, void *context)
{
    (void)context;
    unsigned columns = 0, lines = 1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if ((*p & 0xc0) == 0x80) continue;
        if (*p == '\n') {
            columns = 0;
            ++lines;
        } else {
            if (columns == 4) { ++lines; columns = 0; }
            ++columns;
        }
    }
    return lines <= 2;
}

int main(void)
{
    char page[32];
    assert(ui_pixel_text_fit_copy(page, sizeof(page), "abcdefghij",
                                 fits_two_lines, NULL) == 8);
    assert(strcmp(page, "abcdefgh") == 0);
    assert(ui_pixel_text_fit_copy(page, sizeof(page), "ab\ncd\nef",
                                 fits_two_lines, NULL) == 5);
    assert(strcmp(page, "ab\ncd") == 0);
    const char utf8[] = "\xe4\xb8\xad\xe6\x96\x87";
    assert(ui_pixel_text_fit_copy(page, 5, utf8, fits_two_lines, NULL) == 3);
    assert(strcmp(page, "\xe4\xb8\xad") == 0);
    assert(ui_pixel_text_fit_copy(page, sizeof(page), "", fits_two_lines, NULL) == 0);
    return 0;
}
