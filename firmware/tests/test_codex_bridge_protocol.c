#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "codex_bridge_protocol.h"

static char sent_lines[8][768];
static size_t sent_count;

static bool capture_line(const char *line, size_t length)
{
    assert(length < sizeof(sent_lines[0]));
    memcpy(sent_lines[sent_count], line, length);
    sent_lines[sent_count][length] = '\0';
    ++sent_count;
    return true;
}

static void reset_capture(void)
{
    memset(sent_lines, 0, sizeof(sent_lines));
    sent_count = 0;
}

int main(void)
{
    reset_capture();
    codex_bridge_protocol_accept_line(
        "{\"type\":\"bridge_auth\",\"token\":\"0123456789abcdef0123456789abcdef\"}",
        capture_line,
        "aipassport-001122334455"
    );
    assert(sent_count == 1);
    assert(strstr(sent_lines[0], "\"type\":\"bridge_auth_ok\"") != NULL);
    assert(strstr(sent_lines[0], "\"device_id\":\"aipassport-001122334455\"") != NULL);
    assert(strstr(sent_lines[0], "\"model\":\"FoloOS-AI-Passport\"") != NULL);
    assert(strstr(sent_lines[0], "\"name\":\"AI Passport\"") != NULL);
    assert(strstr(
        sent_lines[0],
        "\"capabilities\":[\"status\",\"text\",\"tasks\",\"results\",\"read_ack\"]"
    ) != NULL);
    assert(sent_lines[0][strlen(sent_lines[0]) - 1] == '\n');

    reset_capture();
    codex_bridge_protocol_accept_line(
        "{\"type\":\"bridge_ready\",\"bridge\":\"CodexBridge\",\"version\":\"1\","
        "\"capabilities\":[\"status\",\"text\",\"read_ack\",\"voice\"]}",
        capture_line,
        "aipassport-001122334455"
    );
    assert(sent_count == 0);

    codex_bridge_parser_t parser = {0};
    codex_bridge_message_t message = {0};
    const char *first =
        "{\"type\":\"message\",\"message_id\":\"task:t1\",\"thread_id\":\"t1\","
        "\"kind\":\"task\",\"status\":\"working\",\"title\":\"Task\","
        "\"body\":\"first\",\"seq\":0,\"total\":2}";
    const char *second =
        "{\"type\":\"message\",\"message_id\":\"task:t1\",\"thread_id\":\"t1\","
        "\"kind\":\"task\",\"status\":\"working\",\"title\":\"Task\","
        "\"body\":\"second\",\"seq\":1,\"total\":2}";
    assert(codex_bridge_protocol_feed(&parser, first, strlen(first), &message));
    assert(codex_bridge_protocol_feed(&parser, second, strlen(second), &message));
    assert(strcmp(message.type, "message") == 0);
    assert(strcmp(message.message_id, "task:t1") == 0);
    assert(strcmp(message.kind, "task") == 0);
    assert(strcmp(message.body, "firstsecond") == 0);
    codex_bridge_protocol_reset_message(&message);
    codex_bridge_parser_reset(&parser);

    codex_read_tracker_reset();
    codex_read_tracker_visit("task:t1", "", 100);
    assert(!codex_read_tracker_take_due("task:t1", "", 2099));
    assert(codex_read_tracker_take_due("task:t1", "", 2100));
    assert(!codex_read_tracker_take_due("task:t1", "", 9000));
    codex_read_tracker_rearm("task:t1", "", 3000);
    assert(!codex_read_tracker_take_due("task:t1", "", 4999));
    assert(codex_read_tracker_take_due("task:t1", "", 5000));

    reset_capture();
    assert(codex_bridge_protocol_send_read_ack(capture_line, "result:t1:1"));
    assert(sent_count == 1);
    assert(strcmp(
        sent_lines[0],
        "{\"type\":\"read_ack\",\"message_id\":\"result:t1:1\"}\n"
    ) == 0);

    reset_capture();
    const char *read_state =
        "{\"type\":\"read_state\",\"message_id\":\"result:t1:1\","
        "\"read_by\":[\"dotii-a\",\"aipassport-001122334455\"]}";
    assert(codex_bridge_protocol_feed(&parser, read_state, strlen(read_state), &message));
    assert(strcmp(message.type, "read_state") == 0);
    assert(strcmp(message.message_id, "result:t1:1") == 0);
    assert(message.read_count == 2U);
    codex_bridge_protocol_reset_message(&message);
    codex_bridge_parser_reset(&parser);

    return 0;
}
