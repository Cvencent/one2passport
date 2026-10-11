#include "serial_dispatch.h"
#include <assert.h>
#include <string.h>

static char keys[16];
static unsigned key_count, shots, lines;
static char received[769];
static void key(char c) { keys[key_count++] = c; }
static void shot(void) { ++shots; }
static void line(const char *text) { ++lines; strcpy(received, text); }
static void feed(serial_dispatch_t *parser, const char *text)
{
    while (*text) serial_dispatch_feed(parser, *text++, key, shot, line);
}
int main(void)
{
    serial_dispatch_t parser = {0};
    feed(&parser, "\nu\nd\no\n");
    assert(key_count == 3 && memcmp(keys, "udo", 3) == 0);
    feed(&parser, " {\"type\":\"done\",\"text\":\"look up FAP_SCREENSHOT_V1\"}\n");
    assert(key_count == 3 && shots == 0 && lines == 1);
    assert(strstr(received, "\"type\":\"done\"") != 0);
    feed(&parser, "FAP_SCREENSHOT_V1");
    assert(shots == 1);
    feed(&parser, "\nk\n");
    assert(key_count == 4);
    feed(&parser, "{");
    for (int i = 0; i < 900; ++i) feed(&parser, "o");
    feed(&parser, "\nFAP_SCREENSHOT_V1\n{\"type\":\"bridge_ready\"}\n");
    assert(key_count == 4 && shots == 2 && lines == 2);
    return 0;
}
