#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SERIAL_LINE_MAX 768
typedef struct {
    char line[SERIAL_LINE_MAX + 1];
    size_t used;
    bool dropping;
} serial_dispatch_t;

void serial_dispatch_feed(serial_dispatch_t *parser, char character,
                          void (*key)(char), void (*screenshot)(void),
                          void (*json_line)(const char *));
