#include "codex_bridge_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_mac.h"

#define CODEX_BRIDGE_CHUNK_BODY_MAX 480
#define CODEX_BRIDGE_TRUNCATION_SUFFIX "\n...[已截断]"
#define CODEX_BRIDGE_TOKEN_MIN 16
#define CODEX_BRIDGE_DEVICE_MODEL "FoloOS-AI-Passport"
#define CODEX_BRIDGE_DEVICE_NAME "AI Passport"

static const char *const SUPPORTED_CAPABILITIES[] = {
    "status", "text", "tasks", "results", "read_ack",
};

static char s_device_id[24];

static const char *json_string(const cJSON *root, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring != NULL
               ? value->valuestring
               : "";
}

static int json_int(const cJSON *root, const char *key, int fallback)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

static bool supported_capability(const char *value)
{
    for (size_t index = 0;
         index < sizeof(SUPPORTED_CAPABILITIES) / sizeof(SUPPORTED_CAPABILITIES[0]);
         ++index) {
        if (strcmp(value, SUPPORTED_CAPABILITIES[index]) == 0) return true;
    }
    return false;
}

static cJSON *create_capabilities(void)
{
    cJSON *capabilities = cJSON_CreateArray();
    if (capabilities == NULL) return NULL;
    for (size_t index = 0;
         index < sizeof(SUPPORTED_CAPABILITIES) / sizeof(SUPPORTED_CAPABILITIES[0]);
         ++index) {
        cJSON_AddItemToArray(
            capabilities,
            cJSON_CreateString(SUPPORTED_CAPABILITIES[index])
        );
    }
    return capabilities;
}

static bool send_json(codex_bridge_send_line_t send_line, cJSON *root)
{
    if (send_line == NULL || root == NULL) {
        cJSON_Delete(root);
        return false;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return false;
    size_t length = strlen(json);
    char *wire = malloc(length + 2);
    bool sent = false;
    if (wire != NULL) {
        memcpy(wire, json, length);
        wire[length] = '\n';
        wire[length + 1] = '\0';
        sent = send_line(wire, length + 1);
        free(wire);
    }
    cJSON_free(json);
    return sent;
}

static bool authenticate(
    const cJSON *root,
    codex_bridge_send_line_t send_line,
    const char *device_id
)
{
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
    if (!cJSON_IsString(token) || token->valuestring == NULL ||
        strlen(token->valuestring) < CODEX_BRIDGE_TOKEN_MIN) {
        return false;
    }
    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) return false;
    cJSON_AddStringToObject(reply, "type", "bridge_auth_ok");
    cJSON_AddStringToObject(reply, "device_id", device_id != NULL ? device_id : "");
    cJSON_AddStringToObject(reply, "model", CODEX_BRIDGE_DEVICE_MODEL);
    cJSON_AddStringToObject(reply, "name", CODEX_BRIDGE_DEVICE_NAME);
    const esp_app_desc_t *description = esp_app_get_description();
    cJSON_AddStringToObject(
        reply,
        "firmware",
        description != NULL ? description->version : "unknown"
    );
    cJSON *capabilities = create_capabilities();
    if (capabilities != NULL) {
        cJSON_AddItemToObject(reply, "capabilities", capabilities);
    }
    return send_json(send_line, reply);
}

bool codex_bridge_protocol_accept_line(
    const char *line,
    codex_bridge_send_line_t send_line,
    const char *device_id
)
{
    if (line == NULL) return false;
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) return false;
    const char *type = json_string(root, "type");
    bool accepted = false;
    if (strcmp(type, "bridge_auth") == 0) {
        accepted = authenticate(root, send_line, device_id);
    }
    cJSON_Delete(root);
    return accepted;
}

static void reset_chunks(codex_bridge_parser_t *parser)
{
    if (parser == NULL) return;
    for (size_t index = 0; index < CODEX_BRIDGE_CHUNK_MAX; ++index) {
        free(parser->chunks[index]);
        parser->chunks[index] = NULL;
    }
    parser->message_id[0] = '\0';
    parser->total = 0;
}

void codex_bridge_parser_reset(codex_bridge_parser_t *parser)
{
    reset_chunks(parser);
}

void codex_bridge_protocol_reset_message(codex_bridge_message_t *message)
{
    if (message == NULL) return;
    free(message->body);
    memset(message, 0, sizeof(*message));
}

static size_t utf8_prefix_bytes(const char *text, size_t limit)
{
    size_t offset = 0;
    while (offset < limit) {
        unsigned char lead = (unsigned char)text[offset];
        size_t character_length = lead < 0x80 ? 1 :
                                  (lead & 0xE0) == 0xC0 ? 2 :
                                  (lead & 0xF0) == 0xE0 ? 3 :
                                  (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (character_length > limit - offset) break;
        bool valid = true;
        for (size_t index = 1; index < character_length; ++index) {
            if (((unsigned char)text[offset + index] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
        offset += character_length;
    }
    return offset;
}

static bool compose_body(
    codex_bridge_parser_t *parser,
    codex_bridge_message_t *message
)
{
    size_t length = 0;
    for (int index = 0; index < parser->total; ++index) {
        size_t chunk_length = parser->chunks[index] == NULL
                                  ? 0
                                  : strlen(parser->chunks[index]);
        if (chunk_length > SIZE_MAX - length) return false;
        length += chunk_length;
    }
    const size_t body_limit = CODEX_BRIDGE_BODY_MAX - 1;
    const size_t suffix_length = sizeof(CODEX_BRIDGE_TRUNCATION_SUFFIX) - 1;
    const bool truncated = length > body_limit;
    const size_t content_limit = truncated ? body_limit - suffix_length : body_limit;
    char *body = malloc(body_limit + 1);
    if (body == NULL) return false;
    size_t written = 0;
    for (int index = 0; index < parser->total && written < content_limit; ++index) {
        if (parser->chunks[index] == NULL) continue;
        size_t chunk_length = strlen(parser->chunks[index]);
        size_t available = content_limit - written;
        if (chunk_length > available) {
            chunk_length = utf8_prefix_bytes(parser->chunks[index], available);
        }
        memcpy(body + written, parser->chunks[index], chunk_length);
        written += chunk_length;
        if (chunk_length < strlen(parser->chunks[index])) break;
    }
    if (truncated) {
        memcpy(body + written, CODEX_BRIDGE_TRUNCATION_SUFFIX, suffix_length);
        written += suffix_length;
    }
    body[written] = '\0';
    free(message->body);
    message->body = body;
    return true;
}

bool codex_bridge_protocol_feed(
    codex_bridge_parser_t *parser,
    const char *line,
    size_t length,
    codex_bridge_message_t *message
)
{
    if (parser == NULL || line == NULL || message == NULL) return false;
    if (length == 0 || length > CODEX_BRIDGE_LINE_MAX) return false;
    cJSON *root = cJSON_ParseWithLength(line, length);
    if (root == NULL) return false;
    const char *type = json_string(root, "type");
    if (type[0] == '\0') {
        cJSON_Delete(root);
        return false;
    }
    if (strcmp(type, "message") != 0) {
        codex_bridge_protocol_reset_message(message);
        snprintf(message->type, sizeof(message->type), "%s", type);
        if (strcmp(type, "bridge_ready") != 0 &&
            strcmp(type, "capabilities_update") != 0 &&
            strcmp(type, "read_state") != 0 &&
            strcmp(type, "error") != 0) {
            message->ignored = true;
        }
        const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
        if (!cJSON_IsArray(capabilities)) {
            capabilities = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        }
        if (cJSON_IsArray(capabilities)) {
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, capabilities) {
                if (!cJSON_IsString(item) || item->valuestring == NULL) continue;
                if (!supported_capability(item->valuestring)) continue;
                if (message->capability_count >= CODEX_BRIDGE_CAPABILITY_MAX) break;
                snprintf(
                    message->capabilities[message->capability_count],
                    CODEX_BRIDGE_CAPABILITY_LEN,
                    "%s",
                    item->valuestring
                );
                ++message->capability_count;
            }
        }
        if (strcmp(type, "read_state") == 0) {
            snprintf(message->message_id, sizeof(message->message_id), "%s",
                     json_string(root, "message_id"));
            const cJSON *read_by = cJSON_GetObjectItemCaseSensitive(root, "read_by");
            if (cJSON_IsArray(read_by)) {
                message->read_count = (unsigned)cJSON_GetArraySize(read_by);
            }
        }
        cJSON_Delete(root);
        return true;
    }

    snprintf(message->message_id, sizeof(message->message_id), "%s", json_string(root, "message_id"));
    if (message->message_id[0] == '\0') {
        cJSON_Delete(root);
        return false;
    }
    int seq = json_int(root, "seq", 0);
    int total = json_int(root, "total", 1);
    if (seq < 0 || total <= 0 || seq >= total ||
        seq >= CODEX_BRIDGE_CHUNK_MAX || total > CODEX_BRIDGE_CHUNK_MAX) {
        cJSON_Delete(root);
        return false;
    }
    if (strcmp(parser->message_id, message->message_id) != 0 ||
        parser->total != total) {
        reset_chunks(parser);
        snprintf(parser->message_id, sizeof(parser->message_id), "%s", message->message_id);
        parser->total = total;
    }
    const char *body = json_string(root, "body");
    size_t chunk_length = strlen(body);
    if (chunk_length > CODEX_BRIDGE_CHUNK_BODY_MAX) {
        cJSON_Delete(root);
        return false;
    }
    free(parser->chunks[seq]);
    parser->chunks[seq] = malloc(chunk_length + 1);
    if (parser->chunks[seq] == NULL) {
        cJSON_Delete(root);
        return false;
    }
    memcpy(parser->chunks[seq], body, chunk_length + 1);

    codex_bridge_protocol_reset_message(message);
    snprintf(message->type, sizeof(message->type), "message");
    snprintf(message->message_id, sizeof(message->message_id), "%s", parser->message_id);
    snprintf(message->thread_id, sizeof(message->thread_id), "%s", json_string(root, "thread_id"));
    snprintf(message->kind, sizeof(message->kind), "%s", json_string(root, "kind"));
    snprintf(message->status, sizeof(message->status), "%s", json_string(root, "status"));
    snprintf(message->title, sizeof(message->title), "%s", json_string(root, "title"));
    message->seq = seq;
    message->total = total;
    bool composed = compose_body(parser, message);
    cJSON_Delete(root);
    if (!composed) {
        codex_bridge_protocol_reset_message(message);
        return false;
    }
    return true;
}

bool codex_bridge_protocol_has_capability(
    const codex_bridge_message_t *message,
    const char *capability
)
{
    if (message == NULL || capability == NULL) return false;
    for (size_t index = 0; index < message->capability_count; ++index) {
        if (strcmp(message->capabilities[index], capability) == 0) return true;
    }
    return false;
}

bool codex_bridge_protocol_send_read_ack(
    codex_bridge_send_line_t send_line,
    const char *message_id
)
{
    if (message_id == NULL || message_id[0] == '\0') return false;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return false;
    cJSON_AddStringToObject(root, "type", "read_ack");
    cJSON_AddStringToObject(root, "message_id", message_id);
    return send_json(send_line, root);
}

void codex_bridge_protocol_init(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(
            s_device_id,
            sizeof(s_device_id),
            "aipassport-%02x%02x%02x%02x%02x%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
        );
    } else {
        snprintf(s_device_id, sizeof(s_device_id), "aipassport-unknown");
    }
}

const char *codex_bridge_protocol_device_id(void)
{
    return s_device_id;
}

const char *const *codex_bridge_protocol_capabilities(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(SUPPORTED_CAPABILITIES) / sizeof(SUPPORTED_CAPABILITIES[0]);
    }
    return SUPPORTED_CAPABILITIES;
}
