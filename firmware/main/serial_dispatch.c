#include "serial_dispatch.h"
#include <string.h>

void serial_dispatch_feed(serial_dispatch_t *parser, char character,
                          void (*key)(char), void (*screenshot)(void),
                          void (*json_line)(const char *))
{
    if (character == '\r') return;
    if (character == '\n') {
        if (!parser->dropping && parser->used && parser->line[0] == '{') {
            parser->line[parser->used] = '\0';
            if (json_line) json_line(parser->line);
        }
        parser->used = 0;
        parser->dropping = false;
        return;
    }
    if (parser->dropping) return;
    if (parser->used == 0) {
        if (character == ' ' || character == '\t') return;
        // '1'..'4' are the automated app-jump commands (radio/Codex/XiaoZhi/settings).
        // 2026-10-12: added 'r' = OK long-press release (hold-to-talk stop for XiaoZhi).
        if (strchr("udolrjk1234", character)) {
            if (key) key(character);
            return;
        }
    }
    if (parser->used >= SERIAL_LINE_MAX) {
        parser->dropping = true;
        return;
    }
    parser->line[parser->used++] = character;
    static const char command[] = "FAP_SCREENSHOT_V1";
    if (parser->used == sizeof(command) - 1 &&
        memcmp(parser->line, command, sizeof(command) - 1) == 0) {
        parser->used = 0;
        if (screenshot) screenshot();
    }
}
