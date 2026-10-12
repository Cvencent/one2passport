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
        // '1'..'4' 是自动化测试的应用跳转命令(收音机/Codex/小智AI/设置)。
        // 必须和按键字符一起在这里被识别, 否则会被当成普通文本攒进 line,
        // 命令永远到不了 serial_key_inject。
        if (strchr("udoljk1234", character)) {
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
