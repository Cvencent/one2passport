#include "codex_bridge_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_rom_crc.h"
#include "mbedtls/md.h"

#define CBDP_DEVICE_MODEL "FoloOS-AI-Passport"
#define CBDP_DEVICE_NAME "AI Passport"

static char s_device_id[64];
static char s_firmware[64] = "unknown";

static uint64_t capability_bit(uint8_t bit)
{
    return bit < 64U ? (UINT64_C(1) << bit) : 0U;
}

static uint64_t supported_capabilities(void)
{
    return capability_bit(CBDP_CAP_STATUS) |
           capability_bit(CBDP_CAP_TEXT) |
           capability_bit(CBDP_CAP_TASKS) |
           capability_bit(CBDP_CAP_RESULTS) |
           capability_bit(CBDP_CAP_APPROVAL) |
           capability_bit(CBDP_CAP_AUDIO) |
           capability_bit(CBDP_CAP_VOICE) |
           capability_bit(CBDP_CAP_READ_ACK) |
           capability_bit(CBDP_CAP_CONTROL) |
           capability_bit(CBDP_CAP_HISTORY) |
           capability_bit(CBDP_CAP_TELEMETRY) |
           capability_bit(CBDP_CAP_SUBSCRIBE_LIVE) |
           capability_bit(CBDP_CAP_SUBSCRIBE_THREAD) |
           capability_bit(CBDP_CAP_SUBSCRIBE_SUMMARY) |
           capability_bit(CBDP_CAP_WRITER_LEASE) |
           capability_bit(CBDP_CAP_MULTI_PAGE) |
           capability_bit(CBDP_CAP_DEVICE_CONFIG) |
           capability_bit(CBDP_CAP_COMMAND_STATUS) |
           capability_bit(CBDP_CAP_READ_UNREAD) |
           capability_bit(CBDP_CAP_PROJECT_MANAGE) |
           capability_bit(CBDP_CAP_EVENTS_SINCE);
}

static bool write_u16(uint8_t *buffer, size_t capacity, size_t *offset, uint16_t value)
{
    if (buffer == NULL || offset == NULL || *offset > capacity || capacity - *offset < 2U) {
        return false;
    }
    buffer[(*offset)++] = (uint8_t)(value >> 8);
    buffer[(*offset)++] = (uint8_t)value;
    return true;
}

static bool write_u32(uint8_t *buffer, size_t capacity, size_t *offset, uint32_t value)
{
    if (buffer == NULL || offset == NULL || *offset > capacity || capacity - *offset < 4U) {
        return false;
    }
    buffer[(*offset)++] = (uint8_t)(value >> 24);
    buffer[(*offset)++] = (uint8_t)(value >> 16);
    buffer[(*offset)++] = (uint8_t)(value >> 8);
    buffer[(*offset)++] = (uint8_t)value;
    return true;
}

uint16_t cbdp_read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint64_t read_u64(const uint8_t *data)
{
    uint64_t value = 0U;
    for (size_t index = 0; index < 8U; ++index) {
        value = (value << 8) | data[index];
    }
    return value;
}

bool cbdp_header_encode(const cbdp_frame_header_t *header, uint8_t *out, size_t capacity)
{
    if (header == NULL || out == NULL || capacity < CBDP_FIXED_HEADER_BYTES) return false;
    out[0] = CBDP_MAGIC_0;
    out[1] = CBDP_MAGIC_1;
    out[2] = header->version_major != 0U ? header->version_major : CBDP_VERSION_MAJOR;
    out[3] = header->message_type;
    size_t offset = 4U;
    if (!write_u16(out, capacity, &offset, header->flags) ||
        !write_u16(out, capacity, &offset, header->header_len) ||
        !write_u32(out, capacity, &offset, header->payload_len) ||
        !write_u32(out, capacity, &offset, header->topic_id) ||
        !write_u32(out, capacity, &offset, header->seq) ||
        !write_u32(out, capacity, &offset, header->ack_seq) ||
        !write_u16(out, capacity, &offset, header->code)) {
        return false;
    }
    out[offset++] = header->qos;
    out[offset++] = 0U;
    return offset == CBDP_FIXED_HEADER_BYTES;
}

bool cbdp_header_decode(const uint8_t *data, size_t length, cbdp_frame_header_t *header)
{
    if (data == NULL || header == NULL || length < CBDP_FIXED_HEADER_BYTES) return false;
    if (data[0] != CBDP_MAGIC_0 || data[1] != CBDP_MAGIC_1) return false;
    memset(header, 0, sizeof(*header));
    header->version_major = data[2];
    header->message_type = data[3];
    header->flags = cbdp_read_u16(data + 4);
    header->header_len = cbdp_read_u16(data + 6);
    header->payload_len = read_u32(data + 8);
    header->topic_id = read_u32(data + 12);
    header->seq = read_u32(data + 16);
    header->ack_seq = read_u32(data + 20);
    header->code = cbdp_read_u16(data + 24);
    header->qos = data[26];
    return data[27] == 0U;
}

bool cbdp_frame_validate(const uint8_t *data, size_t length, cbdp_frame_header_t *header)
{
    cbdp_frame_header_t decoded;
    if (!cbdp_header_decode(data, length, &decoded)) return false;
    if (decoded.version_major != CBDP_VERSION_MAJOR) return false;
    if (decoded.header_len < CBDP_FIXED_HEADER_BYTES) return false;
    if (decoded.header_len > CBDP_MAX_FRAME_BYTES || decoded.payload_len > CBDP_MAX_FRAME_BYTES) {
        return false;
    }
    const uint64_t total = (uint64_t)decoded.header_len + decoded.payload_len;
    if (total != length || total > CBDP_MAX_FRAME_BYTES) return false;
    if (header != NULL) *header = decoded;
    return true;
}

bool cbdp_tlv_next(cbdp_tlv_cursor_t *cursor, cbdp_tlv_t *tlv)
{
    if (cursor == NULL || tlv == NULL || cursor->data == NULL ||
        cursor->offset > cursor->length || cursor->length - cursor->offset == 0U) {
        return false;
    }
    if (cursor->length - cursor->offset < 4U) return false;
    const uint16_t tag = cbdp_read_u16(cursor->data + cursor->offset);
    const uint16_t value_length = cbdp_read_u16(cursor->data + cursor->offset + 2U);
    cursor->offset += 4U;
    if (value_length > cursor->length - cursor->offset) return false;
    tlv->tag = tag;
    tlv->length = value_length;
    tlv->value = cursor->data + cursor->offset;
    cursor->offset += value_length;
    return true;
}

bool cbdp_tlv_validate_block(const uint8_t *tlv, size_t length, uint16_t *duplicate_tag)
{
    if (tlv == NULL && length != 0U) return false;
    cbdp_tlv_cursor_t cursor = {.data = tlv, .length = length, .offset = 0U};
    uint16_t seen[CBDP_MAX_TLVS];
    size_t seen_count = 0U;
    cbdp_tlv_t item;
    while (cursor.offset < cursor.length) {
        if (!cbdp_tlv_next(&cursor, &item) || seen_count >= CBDP_MAX_TLVS) return false;
        for (size_t index = 0; index < seen_count; ++index) {
            if (seen[index] == item.tag) {
                if (duplicate_tag != NULL) *duplicate_tag = item.tag;
                return false;
            }
        }
        seen[seen_count++] = item.tag;
    }
    return cursor.offset == cursor.length;
}

const cbdp_tlv_t *cbdp_tlv_find(const uint8_t *tlv, size_t length, uint16_t tag)
{
    static cbdp_tlv_t found;
    cbdp_tlv_cursor_t cursor = {.data = tlv, .length = length, .offset = 0U};
    cbdp_tlv_t item;
    while (cursor.offset < cursor.length) {
        if (!cbdp_tlv_next(&cursor, &item)) return NULL;
        if (item.tag == tag) {
            found = item;
            return &found;
        }
    }
    return NULL;
}

bool cbdp_tlv_read_u8(const cbdp_tlv_t *tlv, uint8_t *value)
{
    if (tlv == NULL || value == NULL || tlv->length != 1U) return false;
    *value = tlv->value[0];
    return true;
}

bool cbdp_tlv_read_u16(const cbdp_tlv_t *tlv, uint16_t *value)
{
    if (tlv == NULL || value == NULL || tlv->length != 2U) return false;
    *value = cbdp_read_u16(tlv->value);
    return true;
}

bool cbdp_tlv_read_u32(const cbdp_tlv_t *tlv, uint32_t *value)
{
    if (tlv == NULL || value == NULL || tlv->length != 4U) return false;
    *value = read_u32(tlv->value);
    return true;
}

bool cbdp_tlv_read_u64(const cbdp_tlv_t *tlv, uint64_t *value)
{
    if (tlv == NULL || value == NULL || tlv->length != 8U) return false;
    *value = read_u64(tlv->value);
    return true;
}

bool cbdp_tlv_read_i64(const cbdp_tlv_t *tlv, int64_t *value)
{
    uint64_t raw = 0U;
    if (!cbdp_tlv_read_u64(tlv, &raw)) return false;
    *value = (int64_t)raw;
    return true;
}

bool cbdp_tlv_read_bool(const cbdp_tlv_t *tlv, bool *value)
{
    uint8_t raw = 0U;
    if (!cbdp_tlv_read_u8(tlv, &raw)) return false;
    *value = raw != 0U;
    return true;
}

bool cbdp_tlv_read_bytes(const cbdp_tlv_t *tlv, const uint8_t **data, size_t *length)
{
    if (tlv == NULL || data == NULL || length == NULL) return false;
    *data = tlv->value;
    *length = tlv->length;
    return true;
}

bool cbdp_tlv_write_bytes(uint8_t *buffer, size_t capacity, size_t *offset,
                          uint16_t tag, const void *data, size_t length)
{
    if (offset == NULL || length > UINT16_MAX || (*offset > capacity) ||
        capacity - *offset < 4U + length) {
        return false;
    }
    if (length != 0U && data == NULL) return false;
    if (!write_u16(buffer, capacity, offset, tag) ||
        !write_u16(buffer, capacity, offset, (uint16_t)length)) {
        return false;
    }
    if (length != 0U) memcpy(buffer + *offset, data, length);
    *offset += length;
    return true;
}

bool cbdp_tlv_write_u8(uint8_t *buffer, size_t capacity, size_t *offset,
                       uint16_t tag, uint8_t value)
{
    return cbdp_tlv_write_bytes(buffer, capacity, offset, tag, &value, 1U);
}

bool cbdp_tlv_write_u16(uint8_t *buffer, size_t capacity, size_t *offset,
                        uint16_t tag, uint16_t value)
{
    uint8_t encoded[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    return cbdp_tlv_write_bytes(buffer, capacity, offset, tag, encoded, sizeof(encoded));
}

bool cbdp_tlv_write_u32(uint8_t *buffer, size_t capacity, size_t *offset,
                        uint16_t tag, uint32_t value)
{
    uint8_t encoded[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value,
    };
    return cbdp_tlv_write_bytes(buffer, capacity, offset, tag, encoded, sizeof(encoded));
}

bool cbdp_tlv_write_u64(uint8_t *buffer, size_t capacity, size_t *offset,
                        uint16_t tag, uint64_t value)
{
    uint8_t encoded[8];
    for (size_t index = 0; index < sizeof(encoded); ++index) {
        encoded[index] = (uint8_t)(value >> (56U - 8U * index));
    }
    return cbdp_tlv_write_bytes(buffer, capacity, offset, tag, encoded, sizeof(encoded));
}

bool cbdp_tlv_write_i64(uint8_t *buffer, size_t capacity, size_t *offset,
                        uint16_t tag, int64_t value)
{
    return cbdp_tlv_write_u64(buffer, capacity, offset, tag, (uint64_t)value);
}

uint32_t cbdp_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0U;
    if (bytes == NULL && length != 0U) return 0U;
    while (length != 0U) {
        const size_t chunk = length > 4096U ? 4096U : length;
        crc = esp_rom_crc32_le(crc, bytes, (uint32_t)chunk);
        bytes += chunk;
        length -= chunk;
    }
    return crc;
}

bool cbdp_sha256(const void *data, size_t length, uint8_t digest[32])
{
    if ((data == NULL && length != 0U) || digest == NULL) return false;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) return false;
    return mbedtls_md(info, (const unsigned char *)data, length, digest) == 0;
}

void cbdp_build_idem(uint8_t idem[16], uint32_t request_id, uint16_t method_id,
                     const uint8_t *params, size_t params_length)
{
    if (idem == NULL) return;
    memset(idem, 0, 16U);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) return;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 0) != 0 ||
        mbedtls_md_starts(&ctx) != 0) {
        mbedtls_md_free(&ctx);
        return;
    }
    const char *device_id = codex_bridge_protocol_device_id();
    uint8_t header[6];
    header[0] = (uint8_t)(request_id >> 24);
    header[1] = (uint8_t)(request_id >> 16);
    header[2] = (uint8_t)(request_id >> 8);
    header[3] = (uint8_t)request_id;
    header[4] = (uint8_t)(method_id >> 8);
    header[5] = (uint8_t)method_id;
    uint8_t digest[32];
    const bool ok =
        mbedtls_md_update(&ctx, (const unsigned char *)device_id,
                          strlen(device_id)) == 0 &&
        mbedtls_md_update(&ctx, header, sizeof(header)) == 0 &&
        (params == NULL || params_length == 0U ||
         mbedtls_md_update(&ctx, params, params_length) == 0) &&
        mbedtls_md_finish(&ctx, digest) == 0;
    if (ok) memcpy(idem, digest, 16U);
    mbedtls_md_free(&ctx);
}

static bool begin_frame(uint8_t *buffer, size_t capacity, size_t *length,
                        uint8_t type, uint16_t flags, uint8_t qos,
                        uint32_t topic_id, uint32_t seq)
{
    if (length != NULL) *length = 0U;
    if (buffer == NULL || length == NULL || capacity < CBDP_FIXED_HEADER_BYTES) return false;
    memset(buffer, 0, CBDP_FIXED_HEADER_BYTES);
    cbdp_frame_header_t header = {
        .version_major = CBDP_VERSION_MAJOR,
        .message_type = type,
        .flags = flags,
        .header_len = CBDP_FIXED_HEADER_BYTES,
        .topic_id = topic_id,
        .seq = seq,
        .qos = qos,
    };
    if (!cbdp_header_encode(&header, buffer, capacity)) {
        return false;
    }
    *length = CBDP_FIXED_HEADER_BYTES;
    return true;
}

static bool finish_frame(uint8_t *buffer, size_t capacity, size_t *length,
                         uint32_t payload_len)
{
    if (buffer == NULL || length == NULL || *length < CBDP_FIXED_HEADER_BYTES) return false;
    if (*length > UINT16_MAX || *length + payload_len > capacity) return false;
    buffer[6] = (uint8_t)(*length >> 8);
    buffer[7] = (uint8_t)*length;
    buffer[8] = (uint8_t)(payload_len >> 24);
    buffer[9] = (uint8_t)(payload_len >> 16);
    buffer[10] = (uint8_t)(payload_len >> 8);
    buffer[11] = (uint8_t)payload_len;
    *length += payload_len;
    return true;
}

bool cbdp_auth_proof_hmac_sha256(const char *pairing_token,
                                 const uint8_t session_id[16],
                                 const uint8_t client_nonce[16],
                                 const uint8_t server_nonce[16],
                                 const char *device_id,
                                 uint8_t proof[32])
{
    static const uint8_t prefix[] = "CBDP1-AUTH";
    if (pairing_token == NULL || session_id == NULL || client_nonce == NULL ||
        server_nonce == NULL || device_id == NULL || proof == NULL) {
        return false;
    }
    uint8_t data[64 + 16U + 16U + 16U + 64U];
    size_t offset = 0U;
    memcpy(data + offset, prefix, sizeof(prefix) - 1U);
    offset += sizeof(prefix) - 1U;
    memcpy(data + offset, session_id, 16U); offset += 16U;
    memcpy(data + offset, client_nonce, 16U); offset += 16U;
    memcpy(data + offset, server_nonce, 16U); offset += 16U;
    const size_t device_length = strnlen(device_id, 64U);
    memcpy(data + offset, device_id, device_length);
    offset += device_length;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) return false;
    return mbedtls_md_hmac(info, (const uint8_t *)pairing_token, strlen(pairing_token),
                           data, offset, proof) == 0;
}

bool cbdp_build_hello(uint8_t *buffer, size_t capacity, size_t *length,
                      uint32_t request_id, const uint8_t client_nonce[16])
{
    if (!begin_frame(buffer, capacity, length, CBDP_MT_HELLO, CBDP_FLAG_REQUEST,
                     CBDP_QOS_CRITICAL, 0U, 0U)) return false;
    if (!cbdp_tlv_write_u16(buffer, capacity, length, CBDP_TLV_VERSION_MINOR,
                            CBDP_VERSION_MINOR) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_DEVICE_ID,
                              s_device_id, strlen(s_device_id)) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_MODEL,
                              CBDP_DEVICE_MODEL, strlen(CBDP_DEVICE_MODEL)) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_FIRMWARE,
                              s_firmware, strlen(s_firmware)) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_NAME,
                              CBDP_DEVICE_NAME, strlen(CBDP_DEVICE_NAME)) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_PROFILE,
                           CBDP_PROFILE_PANEL) ||
        !cbdp_tlv_write_u64(buffer, capacity, length, CBDP_TLV_SUPPORTED_CAPS,
                            supported_capabilities()) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_TRANSPORT_SECURITY,
                           CBDP_TRANSPORT_PLAIN_LAN) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_REQUEST_ID,
                            request_id)) return false;
    if (client_nonce != NULL &&
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_CLIENT_NONCE,
                              client_nonce, 16U)) return false;
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_auth(uint8_t *buffer, size_t capacity, size_t *length,
                     const cbdp_session_t *session, const char *pairing_token,
                     const uint8_t client_nonce[16], uint32_t request_id)
{
    if (session == NULL || pairing_token == NULL) return false;
    uint8_t proof[32];
    const uint8_t zero_nonce[16] = {0};
    if (!cbdp_auth_proof_hmac_sha256(pairing_token, session->session_id_bytes,
                                      client_nonce != NULL ? client_nonce : zero_nonce,
                                      session->server_nonce, s_device_id, proof)) {
        return false;
    }
    if (!begin_frame(buffer, capacity, length, CBDP_MT_AUTH, CBDP_FLAG_REQUEST,
                     CBDP_QOS_CRITICAL, 0U, 0U) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_SESSION_ID,
                              session->session_id_bytes, 16U) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_AUTH_PROOF,
                              proof, sizeof(proof)) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_AUTH_MODE,
                           CBDP_AUTH_HMAC_SHA256) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_REQUEST_ID,
                            request_id)) return false;
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_ping(uint8_t *buffer, size_t capacity, size_t *length, uint32_t ping_id)
{
    if (!begin_frame(buffer, capacity, length, CBDP_MT_PING, 0U, CBDP_QOS_TELEMETRY,
                     0U, 0U)) return false;
    if (ping_id != 0U &&
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_PING_ID, ping_id)) {
        return false;
    }
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_pong(uint8_t *buffer, size_t capacity, size_t *length, uint32_t echo)
{
    if (!begin_frame(buffer, capacity, length, CBDP_MT_PONG, 0U, CBDP_QOS_TELEMETRY,
                     0U, 0U)) return false;
    if (echo != 0U && !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_ECHO, echo)) {
        return false;
    }
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_ack(uint8_t *buffer, size_t capacity, size_t *length,
                    uint32_t ack_seq, uint32_t topic_id, uint8_t scope, uint64_t cursor)
{
    if (!begin_frame(buffer, capacity, length, CBDP_MT_ACK, 0U, CBDP_QOS_STATE,
                     0U, 0U)) return false;
    buffer[20] = (uint8_t)(ack_seq >> 24);
    buffer[21] = (uint8_t)(ack_seq >> 16);
    buffer[22] = (uint8_t)(ack_seq >> 8);
    buffer[23] = (uint8_t)ack_seq;
    if (cursor != 0U) {
        if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TOPIC_ID, topic_id) ||
            !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_SCOPE, scope) ||
            !cbdp_tlv_write_u64(buffer, capacity, length, CBDP_TLV_CURSOR, cursor)) {
            return false;
        }
    }
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_resync(uint8_t *buffer, size_t capacity, size_t *length,
                       uint32_t request_id, uint8_t reason)
{
    if (!begin_frame(buffer, capacity, length, CBDP_MT_RESYNC, CBDP_FLAG_REQUEST,
                     CBDP_QOS_CRITICAL, 0U, 0U)) return false;
    if (!cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_RESYNC_REASON, reason) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_REQUEST_ID, request_id)) {
        return false;
    }
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_subscribe(uint8_t *buffer, size_t capacity, size_t *length,
                          uint32_t request_id, uint32_t topic_id, uint8_t scope,
                          uint8_t qos, uint64_t cursor)
{
    if (!begin_frame(buffer, capacity, length, CBDP_MT_SUBSCRIBE, CBDP_FLAG_REQUEST,
                     qos, topic_id, 0U)) return false;
    if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TOPIC_ID, topic_id) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_SCOPE, scope) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_QOS, qos) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_REQUEST_ID, request_id)) {
        return false;
    }
    // 15.4: `cursor` is optional. Sending an explicit 0 asks the host to resume
    // from the very start of history, which replays the whole topic backlog in
    // one burst and swamps a small device. Omitting it tells the host to fix a
    // snapshot revision and stream only new events.
    if (cursor != 0U &&
        !cbdp_tlv_write_u64(buffer, capacity, length, CBDP_TLV_CURSOR, cursor)) {
        return false;
    }
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_command(uint8_t *buffer, size_t capacity, size_t *length,
                        uint32_t request_id, uint16_t method_id,
                        const uint8_t *params, size_t params_length,
                        const uint8_t idem[16])
{
    if ((params_length != 0U && params == NULL) || idem == NULL) return false;
    if (!begin_frame(buffer, capacity, length, CBDP_MT_COMMAND,
                     CBDP_FLAG_REQUEST | CBDP_FLAG_COMMAND, CBDP_QOS_CRITICAL,
                     0U, 0U)) return false;
    if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_REQUEST_ID, request_id) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_IDEM,
                              idem, 16U) ||
        !cbdp_tlv_write_u16(buffer, capacity, length, CBDP_TLV_METHOD_ID, method_id) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_PARAMS,
                              params, params_length)) return false;
    return finish_frame(buffer, capacity, length, 0U);
}


// 18.1 / 19.1: a SEND_* or AUDIO_* transaction must identify its target.
// thread_ref/thread_id selects explicitly; use_active_thread delegates to the
// host's active thread. Returning false means the caller has nothing to target.
static bool write_thread_selector(uint8_t *buffer, size_t capacity, size_t *length,
                                  uint32_t thread_ref, const char *thread_id,
                                  bool use_active_thread)
{
    if (thread_ref != 0U) {
        return cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_THREAD_REF, thread_ref);
    }
    if (thread_id != NULL && thread_id[0] != '\0') {
        return cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_THREAD_ID,
                                    thread_id, strlen(thread_id));
    }
    if (use_active_thread) {
        return cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_USE_ACTIVE_THREAD, 1U);
    }
    return false;
}

bool cbdp_build_send_begin(uint8_t *buffer, size_t capacity, size_t *length,
                           uint32_t tx_id, uint8_t mode, uint32_t total_bytes,
                           uint32_t thread_ref, const char *thread_id,
                           bool use_active_thread, const uint8_t idem[16])
{
    if (idem == NULL) return false;
    if (!begin_frame(buffer, capacity, length, CBDP_MT_SEND_BEGIN, CBDP_FLAG_REQUEST,
                     CBDP_QOS_CRITICAL, 0U, 0U)) return false;
    if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TX_ID, tx_id) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_IDEM,
                              idem, 16U) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_MODE, mode) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TOTAL_BYTES, total_bytes) ||
        !write_thread_selector(buffer, capacity, length, thread_ref, thread_id,
                               use_active_thread)) {
        return false;
    }
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_send_chunk(uint8_t *buffer, size_t capacity, size_t *length,
                           uint32_t tx_id, uint32_t offset,
                           const uint8_t *payload, size_t payload_length)
{
    if (payload_length > CBDP_MAX_CHUNK_BYTES || (payload_length != 0U && payload == NULL)) {
        return false;
    }
    if (!begin_frame(buffer, capacity, length, CBDP_MT_SEND_CHUNK, 0U,
                     CBDP_QOS_CRITICAL, 0U, 0U)) return false;
    if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TX_ID, tx_id) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_OFFSET, offset)) return false;
    if (payload_length != 0U) {
        if (capacity - *length < payload_length) return false;
        memcpy(buffer + *length, payload, payload_length);
    }
    return finish_frame(buffer, capacity, length, (uint32_t)payload_length);
}

bool cbdp_build_send_end(uint8_t *buffer, size_t capacity, size_t *length,
                         uint32_t tx_id, uint32_t total_bytes)
{
    if (!begin_frame(buffer, capacity, length, CBDP_MT_SEND_END, 0U,
                     CBDP_QOS_CRITICAL, 0U, 0U)) return false;
    if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TX_ID, tx_id) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TOTAL_BYTES, total_bytes)) {
        return false;
    }
    return finish_frame(buffer, capacity, length, 0U);
}

static bool build_audio_begin(uint8_t *buffer, size_t capacity, size_t *length,
                              uint32_t tx_id, uint8_t codec, uint32_t sample_rate,
                              uint8_t channels, uint8_t bits_per_sample,
                              uint32_t total_bytes, const char *transcript,
                              uint32_t thread_ref, const char *thread_id,
                              bool use_active_thread,
                              uint8_t message_type, const uint8_t idem[16])
{
    if (idem == NULL) return false;
    if (!begin_frame(buffer, capacity, length, message_type, CBDP_FLAG_REQUEST,
                     CBDP_QOS_CRITICAL, 0U, 0U)) return false;
    if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TX_ID, tx_id) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_IDEM,
                              idem, 16U) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_CODEC, codec) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_SAMPLE_RATE, sample_rate) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_CHANNELS, channels) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_BITS_PER_SAMPLE,
                           bits_per_sample) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TOTAL_BYTES, total_bytes) ||
        !write_thread_selector(buffer, capacity, length, thread_ref, thread_id,
                               use_active_thread)) {
        return false;
    }
    if (transcript != NULL && transcript[0] != '\0' &&
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_TRANSCRIPT,
                              transcript, strlen(transcript))) return false;
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_audio_begin(uint8_t *buffer, size_t capacity, size_t *length,
                            uint32_t tx_id, uint8_t codec, uint32_t sample_rate,
                            uint8_t channels, uint8_t bits_per_sample,
                            uint32_t total_bytes, const char *transcript,
                            uint32_t thread_ref, const char *thread_id,
                            bool use_active_thread, const uint8_t idem[16])
{
    return build_audio_begin(buffer, capacity, length, tx_id, codec, sample_rate,
                             channels, bits_per_sample, total_bytes, transcript,
                             thread_ref, thread_id, use_active_thread,
                             CBDP_MT_AUDIO_BEGIN, idem);
}

bool cbdp_build_audio_chunk(uint8_t *buffer, size_t capacity, size_t *length,
                            uint32_t tx_id, uint32_t offset,
                            const uint8_t *payload, size_t payload_length)
{
    if (payload_length > CBDP_MAX_CHUNK_BYTES || (payload_length != 0U && payload == NULL)) {
        return false;
    }
    if (!begin_frame(buffer, capacity, length, CBDP_MT_AUDIO_CHUNK, 0U,
                     CBDP_QOS_CRITICAL, 0U, 0U)) return false;
    if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TX_ID, tx_id) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_OFFSET, offset)) return false;
    if (payload_length != 0U) {
        if (capacity - *length < payload_length) return false;
        memcpy(buffer + *length, payload, payload_length);
    }
    return finish_frame(buffer, capacity, length, (uint32_t)payload_length);
}

bool cbdp_build_audio_end(uint8_t *buffer, size_t capacity, size_t *length,
                          uint32_t tx_id, uint32_t total_bytes)
{
    if (!begin_frame(buffer, capacity, length, CBDP_MT_AUDIO_END, 0U,
                     CBDP_QOS_CRITICAL, 0U, 0U)) return false;
    if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TX_ID, tx_id) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_TOTAL_BYTES, total_bytes)) {
        return false;
    }
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_approval_resolve(uint8_t *buffer, size_t capacity, size_t *length,
                                 uint32_t request_id, uint32_t approval_ref,
                                 uint8_t decision, const char *response,
                                 const uint8_t idem[16])
{
    if (!begin_frame(buffer, capacity, length, CBDP_MT_APPROVAL_RESOLVE,
                     CBDP_FLAG_REQUEST, CBDP_QOS_CRITICAL, 0U, 0U)) return false;
    if (!cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_APPROVAL_REF,
                            approval_ref) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_DECISION, decision) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_IDEM,
                              idem, 16U) ||
        !cbdp_tlv_write_u32(buffer, capacity, length, CBDP_TLV_REQUEST_ID,
                            request_id)) return false;
    if (response != NULL && response[0] != '\0' &&
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_RESPONSE,
                              response, strlen(response))) return false;
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_build_discovery_reply(uint8_t *buffer, size_t capacity, size_t *length,
                                const uint8_t nonce[16])
{
    if (nonce == NULL) return false;
    if (!begin_frame(buffer, capacity, length, CBDP_MT_DISCOVERY_REPLY,
                     CBDP_FLAG_RESPONSE, CBDP_QOS_TELEMETRY, 0U, 0U)) return false;
    if (!cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_DEVICE_ID,
                              s_device_id, strlen(s_device_id)) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_MODEL,
                              CBDP_DEVICE_MODEL, strlen(CBDP_DEVICE_MODEL)) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_FIRMWARE,
                              s_firmware, strlen(s_firmware)) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_PROFILE,
                           CBDP_PROFILE_PANEL) ||
        !cbdp_tlv_write_u64(buffer, capacity, length, CBDP_TLV_SUPPORTED_CAPS,
                            supported_capabilities()) ||
        !cbdp_tlv_write_u16(buffer, capacity, length, CBDP_TLV_PORT, 8765U) ||
        !cbdp_tlv_write_bytes(buffer, capacity, length, CBDP_TLV_DISCOVERY_NONCE,
                              nonce, 16U) ||
        !cbdp_tlv_write_u8(buffer, capacity, length, CBDP_TLV_TRANSPORT_SECURITY,
                           CBDP_TRANSPORT_PLAIN_LAN)) return false;
    return finish_frame(buffer, capacity, length, 0U);
}

bool cbdp_parse_welcome(const uint8_t *tlv, size_t length, cbdp_session_t *session)
{
    if (session == NULL || !cbdp_tlv_validate_block(tlv, length, NULL)) return false;
    memset(session, 0, sizeof(*session));
    const cbdp_tlv_t *item = cbdp_tlv_find(tlv, length, CBDP_TLV_VERSION_MINOR);
    uint16_t minor = 0U;
    if (!cbdp_tlv_read_u16(item, &minor)) return false;
    session->version_minor = minor;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_SESSION_ID);
    if (item == NULL || item->length != 16U) return false;
    memcpy(session->session_id_bytes, item->value, 16U);
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_SERVER_NONCE);
    if (item == NULL || item->length != 16U) return false;
    memcpy(session->server_nonce, item->value, 16U);
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_AUTH_MODE);
    if (!cbdp_tlv_read_u8(item, &session->auth_mode)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_MAX_FRAME_BYTES);
    if (!cbdp_tlv_read_u32(item, &session->max_frame_bytes)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_MAX_TEXT_CHARS);
    if (!cbdp_tlv_read_u32(item, &session->max_text_chars)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_MAX_TEXT_BYTES);
    if (!cbdp_tlv_read_u32(item, &session->max_text_bytes)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_QUEUE_DEPTH);
    if (!cbdp_tlv_read_u16(item, &session->queue_depth)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_HEARTBEAT_SECONDS);
    if (!cbdp_tlv_read_u16(item, &session->heartbeat_seconds)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_MAX_CHUNK_BYTES);
    if (!cbdp_tlv_read_u16(item, &session->max_chunk_bytes)) return false;
    // AUDIO_BEGIN/CHUNK/END must respect the host-declared accumulation cap.
    // The field is optional in some WELCOME variants; fall back to the
    // protocol default rather than rejecting the session.
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_MAX_AUDIO_BYTES);
    if (item != NULL) {
        (void)cbdp_tlv_read_u32(item, &session->max_audio_bytes);
    }
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_WRITER_LEASE_SECONDS);
    if (!cbdp_tlv_read_u32(item, &session->writer_lease_seconds)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_REQUEST_ID);
    if (!cbdp_tlv_read_u32(item, &session->request_id)) return false;
    session->max_text_bytes = session->max_text_bytes != 0U
                                  ? session->max_text_bytes : CBDP_MAX_TEXT_BYTES;
    session->max_text_chars = session->max_text_chars != 0U
                                  ? session->max_text_chars : CBDP_MAX_TEXT_CHARS;
    session->max_chunk_bytes = session->max_chunk_bytes != 0U
                                   ? session->max_chunk_bytes : CBDP_MAX_CHUNK_BYTES;
    session->max_audio_bytes = session->max_audio_bytes != 0U
                                   ? session->max_audio_bytes : CBDP_MAX_REASSEMBLY_BYTES;
    session->queue_depth = session->queue_depth != 0U ? session->queue_depth : CBDP_QUEUE_DEPTH;
    session->heartbeat_seconds = session->heartbeat_seconds != 0U
                                     ? session->heartbeat_seconds : CBDP_HEARTBEAT_SECONDS;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_SERVER_TIME_MS);
    uint64_t server_time_ms = 0U;
    if (!cbdp_tlv_read_u64(item, &server_time_ms)) return false;
    session->supported_caps = supported_capabilities();
    return true;
}

bool cbdp_parse_auth_ok(const uint8_t *tlv, size_t length, cbdp_session_t *session)
{
    if (session == NULL || !cbdp_tlv_validate_block(tlv, length, NULL)) return false;
    const cbdp_tlv_t *item = cbdp_tlv_find(tlv, length, CBDP_TLV_DEVICE_ID);
    const uint8_t *device_id = NULL;
    size_t device_id_length = 0U;
    if (item == NULL || !cbdp_tlv_read_bytes(item, &device_id, &device_id_length) ||
        device_id_length == 0U) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_EFFECTIVE_CAPS);
    if (!cbdp_tlv_read_u64(item, &session->effective_caps)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_POLICY_VERSION);
    if (!cbdp_tlv_read_u32(item, &session->policy_version)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_DEVICE_REF);
    if (!cbdp_tlv_read_u32(item, &session->device_ref)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_REQUEST_ID);
    if (!cbdp_tlv_read_u32(item, &session->request_id)) return false;
    session->ready = true;
    return true;
}

bool cbdp_parse_capability_update(const uint8_t *tlv, size_t length, cbdp_session_t *session)
{
    if (session == NULL || !cbdp_tlv_validate_block(tlv, length, NULL)) return false;
    const cbdp_tlv_t *item = cbdp_tlv_find(tlv, length, CBDP_TLV_EFFECTIVE_CAPS);
    if (!cbdp_tlv_read_u64(item, &session->effective_caps)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_POLICY_VERSION);
    if (!cbdp_tlv_read_u32(item, &session->policy_version)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_REQUEST_ID);
    if (item != NULL) (void)cbdp_tlv_read_u32(item, &session->request_id);
    return true;
}

bool cbdp_parse_event_header(const uint8_t *tlv, size_t length, cbdp_event_header_t *event)
{
    if (event == NULL || !cbdp_tlv_validate_block(tlv, length, NULL)) return false;
    memset(event, 0, sizeof(*event));
    const cbdp_tlv_t *item = cbdp_tlv_find(tlv, length, CBDP_TLV_EVENT_TYPE);
    uint16_t value16 = 0U;
    if (!cbdp_tlv_read_u16(item, &value16)) return false;
    event->event_type = value16;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_EVENT_PAYLOAD);
    if (item == NULL) return false;
    event->event_payload = item->value;
    event->event_payload_length = item->length;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_GLOBAL_SEQ);
    (void)cbdp_tlv_read_u64(item, &event->global_seq);
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_TOPIC_SEQ);
    (void)cbdp_tlv_read_u64(item, &event->topic_seq);
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_TOPIC_ID);
    (void)cbdp_tlv_read_u32(item, &event->topic_id);
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_REVISION);
    (void)cbdp_tlv_read_u64(item, &event->revision);
    return true;
}

void cbdp_fragment_reset(cbdp_fragment_assembly_t *assembly)
{
    if (assembly == NULL) return;
    if (assembly->fragments != NULL) {
        for (uint16_t index = 0U; index < assembly->total; ++index) {
            free(assembly->fragments[index]);
        }
    }
    free(assembly->fragments);
    free(assembly->fragment_lengths);
    free(assembly->original_header_tlv);
    free(assembly->received_map);
    memset(assembly, 0, sizeof(*assembly));
}

static bool fragment_control(const uint8_t *tlv, size_t length,
                             uint32_t *stream_id, uint16_t *index,
                             uint16_t *total, uint8_t *original_type,
                             uint16_t *original_flags, uint32_t *original_payload_len,
                             const cbdp_tlv_t **original_header)
{
    const cbdp_tlv_t *item = cbdp_tlv_find(tlv, length, CBDP_TLV_FRAGMENT_STREAM_ID);
    if (!cbdp_tlv_read_u32(item, stream_id)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_FRAGMENT_INDEX);
    if (!cbdp_tlv_read_u16(item, index)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_FRAGMENT_TOTAL);
    if (!cbdp_tlv_read_u16(item, total)) return false;
    if (*total == 0U || *total > CBDP_FRAGMENT_MAX || *index >= *total) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_ORIGINAL_TYPE);
    if (!cbdp_tlv_read_u8(item, original_type)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_ORIGINAL_FLAGS);
    if (!cbdp_tlv_read_u16(item, original_flags)) return false;
    item = cbdp_tlv_find(tlv, length, CBDP_TLV_ORIGINAL_PAYLOAD_LEN);
    if (!cbdp_tlv_read_u32(item, original_payload_len)) return false;
    *original_header = cbdp_tlv_find(tlv, length, CBDP_TLV_ORIGINAL_HEADER_TLV);
    return true;
}

static bool fragment_read_crc(const uint8_t *tlv, size_t length,
                              uint16_t frame_flags, uint32_t *crc32,
                              bool *present)
{
    if (crc32 == NULL || present == NULL) return false;
    *crc32 = 0U;
    *present = (frame_flags & CBDP_FLAG_CRC32_PRESENT) != 0U;
    if (!*present) return true;
    const cbdp_tlv_t *item = cbdp_tlv_find(tlv, length, CBDP_TLV_PAYLOAD_CRC32);
    return cbdp_tlv_read_u32(item, crc32);
}

static bool fragment_match_assembly(const cbdp_fragment_assembly_t *assembly,
                                    uint32_t stream_id, uint16_t total,
                                    uint8_t original_type, uint16_t original_flags,
                                    uint32_t original_payload_len,
                                    uint32_t crc32, bool crc32_present)
{
    return assembly != NULL && assembly->active &&
           assembly->stream_id == stream_id && assembly->total == total &&
           assembly->original_type == original_type &&
           assembly->original_flags == original_flags &&
           assembly->expected_payload_len == original_payload_len &&
           assembly->crc32_present == crc32_present &&
           (!crc32_present || assembly->expected_crc32 == crc32);
}

static bool fragment_prepare(cbdp_fragment_assembly_t *assembly, uint16_t total,
                             uint32_t stream_id, uint8_t original_type,
                             uint16_t original_flags, uint32_t original_payload_len,
                             uint32_t crc32, bool crc32_present,
                             const cbdp_tlv_t *original_header)
{
    if (assembly == NULL || total == 0U || total > CBDP_FRAGMENT_MAX ||
        original_payload_len > CBDP_MAX_REASSEMBLY_BYTES) {
        return false;
    }
    cbdp_fragment_reset(assembly);
    assembly->fragments = calloc(total, sizeof(*assembly->fragments));
    assembly->fragment_lengths = calloc(total, sizeof(*assembly->fragment_lengths));
    assembly->received_map = calloc(total, 1U);
    if (original_header != NULL && original_header->length != 0U) {
        assembly->original_header_tlv = malloc(original_header->length);
        if (assembly->original_header_tlv != NULL) {
            memcpy(assembly->original_header_tlv, original_header->value,
                   original_header->length);
        }
    }
    if (assembly->fragments == NULL || assembly->fragment_lengths == NULL ||
        assembly->received_map == NULL ||
        (original_header != NULL && original_header->length != 0U &&
         assembly->original_header_tlv == NULL)) {
        cbdp_fragment_reset(assembly);
        return false;
    }
    assembly->stream_id = stream_id;
    assembly->total = total;
    assembly->expected_payload_len = original_payload_len;
    assembly->original_type = original_type;
    assembly->original_flags = original_flags;
    assembly->original_header_len = original_header != NULL ? original_header->length : 0U;
    assembly->original_header_present = original_header != NULL;
    assembly->expected_crc32 = crc32;
    assembly->crc32_present = crc32_present;
    assembly->active = true;
    return true;
}

bool cbdp_fragment_begin(cbdp_fragment_assembly_t *assembly, const uint8_t *tlv,
                         size_t length, uint32_t payload_len, uint16_t frame_flags)
{
    if (assembly == NULL) return false;
    uint32_t stream_id = 0U, original_payload_len = 0U;
    uint16_t index = 0U, total = 0U, original_flags = 0U;
    uint8_t original_type = 0U;
    uint32_t crc32 = 0U;
    bool crc32_present = false;
    const cbdp_tlv_t *original_header = NULL;
    if (!fragment_read_crc(tlv, length, frame_flags, &crc32, &crc32_present) ||
        !fragment_control(tlv, length, &stream_id, &index, &total, &original_type,
                          &original_flags, &original_payload_len, &original_header) ||
        index != 0U || original_header == NULL ||
        payload_len > original_payload_len) {
        return false;
    }
    return fragment_prepare(assembly, total, stream_id, original_type, original_flags,
                            original_payload_len, crc32, crc32_present, original_header);
}

bool cbdp_fragment_accept(cbdp_fragment_assembly_t *assembly, const uint8_t *tlv,
                          size_t length, uint16_t frame_flags,
                          const uint8_t *payload, size_t payload_len,
                          uint8_t **complete_header, size_t *complete_header_len,
                          uint8_t **complete_payload, size_t *complete_payload_len)
{
    if (assembly == NULL ||
        complete_header == NULL || complete_header_len == NULL ||
        complete_payload == NULL || complete_payload_len == NULL ||
        payload_len > UINT16_MAX || (payload_len != 0U && payload == NULL)) {
        return false;
    }
    *complete_header = NULL;
    *complete_header_len = 0U;
    *complete_payload = NULL;
    *complete_payload_len = 0U;
    uint32_t stream_id = 0U, original_payload_len = 0U;
    uint16_t index = 0U, total = 0U, original_flags = 0U;
    uint8_t original_type = 0U;
    uint32_t crc32 = 0U;
    bool crc32_present = false;
    const cbdp_tlv_t *original_header = NULL;
    if (!fragment_read_crc(tlv, length, frame_flags, &crc32, &crc32_present) ||
        !fragment_control(tlv, length, &stream_id, &index, &total, &original_type,
                          &original_flags, &original_payload_len, &original_header)) {
        return false;
    }
    if (index != 0U && original_header != NULL) return false;
    if (index == 0U && original_header == NULL) return false;
    if (!assembly->active) {
        if (!fragment_prepare(assembly, total, stream_id, original_type,
                              original_flags, original_payload_len,
                              crc32, crc32_present, original_header)) {
            return false;
        }
    } else if (!fragment_match_assembly(assembly, stream_id, total, original_type,
                                        original_flags, original_payload_len,
                                        crc32, crc32_present)) {
        cbdp_fragment_reset(assembly);
        return false;
    }
    if (index == 0U) {
        if (!assembly->original_header_present) {
            if (original_header->length != 0U) {
                uint8_t *header_copy = malloc(original_header->length);
                if (header_copy == NULL) {
                    cbdp_fragment_reset(assembly);
                    return false;
                }
                memcpy(header_copy, original_header->value, original_header->length);
                assembly->original_header_tlv = header_copy;
            }
            assembly->original_header_len = original_header->length;
            assembly->original_header_present = true;
        } else if (assembly->original_header_len != original_header->length ||
                   (original_header->length != 0U &&
                    memcmp(assembly->original_header_tlv, original_header->value,
                           original_header->length) != 0)) {
            cbdp_fragment_reset(assembly);
            return false;
        }
    }
    if (assembly->received_map == NULL || index >= assembly->total) {
        cbdp_fragment_reset(assembly);
        return false;
    }
    if (assembly->received_bytes + payload_len > assembly->expected_payload_len) {
        cbdp_fragment_reset(assembly);
        return false;
    }
    if (assembly->received_map[index] != 0U) {
        if (assembly->fragment_lengths[index] != payload_len) {
            cbdp_fragment_reset(assembly);
            return false;
        }
        if (payload_len != 0U &&
            memcmp(assembly->fragments[index], payload, payload_len) != 0) {
            cbdp_fragment_reset(assembly);
            return false;
        }
        return true;
    }
    uint8_t *fragment = malloc(payload_len != 0U ? payload_len : 1U);
    if (fragment == NULL) {
        cbdp_fragment_reset(assembly);
        return false;
    }
    if (payload_len != 0U) memcpy(fragment, payload, payload_len);
    assembly->fragments[index] = fragment;
    assembly->fragment_lengths[index] = (uint16_t)payload_len;
    assembly->received_map[index] = 1U;
    assembly->received_bytes += (uint32_t)payload_len;
    ++assembly->received;
    if (assembly->received != assembly->total) return false;
    if (assembly->received_bytes != assembly->expected_payload_len) {
        cbdp_fragment_reset(assembly);
        return false;
    }

    uint8_t *combined = malloc(assembly->expected_payload_len != 0U
                                   ? assembly->expected_payload_len : 1U);
    if (combined == NULL) {
        cbdp_fragment_reset(assembly);
        return false;
    }
    size_t offset = 0U;
    for (uint16_t fragment_index = 0U; fragment_index < assembly->total; ++fragment_index) {
        const uint16_t fragment_length = assembly->fragment_lengths[fragment_index];
        if (fragment_length != 0U) {
            memcpy(combined + offset, assembly->fragments[fragment_index], fragment_length);
        }
        offset += fragment_length;
    }
    if (offset != assembly->expected_payload_len ||
        (assembly->crc32_present &&
         cbdp_crc32(combined, assembly->expected_payload_len) != assembly->expected_crc32)) {
        free(combined);
        cbdp_fragment_reset(assembly);
        return false;
    }

    *complete_header = assembly->original_header_tlv;
    *complete_header_len = assembly->original_header_len;
    *complete_payload = combined;
    *complete_payload_len = assembly->expected_payload_len;
    assembly->original_header_tlv = NULL;
    assembly->received_bytes = 0U;
    assembly->received = 0U;
    free(assembly->received_map);
    assembly->received_map = NULL;
    for (uint16_t fragment_index = 0U; fragment_index < assembly->total; ++fragment_index) {
        free(assembly->fragments[fragment_index]);
        assembly->fragments[fragment_index] = NULL;
    }
    free(assembly->fragments);
    assembly->fragments = NULL;
    free(assembly->fragment_lengths);
    assembly->fragment_lengths = NULL;
    assembly->active = false;
    return true;
}
bool codex_bridge_protocol_has_capability(const char *capability)
{
    if (capability == NULL) return false;
    static const struct { const char *name; uint8_t bit; } names[] = {
        {"status", CBDP_CAP_STATUS}, {"text", CBDP_CAP_TEXT},
        {"tasks", CBDP_CAP_TASKS}, {"results", CBDP_CAP_RESULTS},
        {"approval", CBDP_CAP_APPROVAL}, {"audio", CBDP_CAP_AUDIO},
        {"voice", CBDP_CAP_VOICE}, {"read_ack", CBDP_CAP_READ_ACK},
        {"control", CBDP_CAP_CONTROL}, {"history", CBDP_CAP_HISTORY},
        {"telemetry", CBDP_CAP_TELEMETRY}, {"subscribe_live", CBDP_CAP_SUBSCRIBE_LIVE},
        {"subscribe_thread", CBDP_CAP_SUBSCRIBE_THREAD},
        {"subscribe_summary", CBDP_CAP_SUBSCRIBE_SUMMARY},
        {"writer_lease", CBDP_CAP_WRITER_LEASE}, {"multi_page", CBDP_CAP_MULTI_PAGE},
        {"device_config", CBDP_CAP_DEVICE_CONFIG},
        {"command_status", CBDP_CAP_COMMAND_STATUS},
        {"read_unread", CBDP_CAP_READ_UNREAD},
        {"project_manage", CBDP_CAP_PROJECT_MANAGE},
        {"events_since", CBDP_CAP_EVENTS_SINCE},
    };
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strcmp(names[index].name, capability) == 0) {
            return (supported_capabilities() & capability_bit(names[index].bit)) != 0U;
        }
    }
    return false;
}

const char *codex_bridge_capability_name(uint8_t bit)
{
    switch (bit) {
    case CBDP_CAP_STATUS: return "status";
    case CBDP_CAP_TEXT: return "text";
    case CBDP_CAP_TASKS: return "tasks";
    case CBDP_CAP_RESULTS: return "results";
    case CBDP_CAP_APPROVAL: return "approval";
    case CBDP_CAP_AUDIO: return "audio";
    case CBDP_CAP_VOICE: return "voice";
    case CBDP_CAP_READ_ACK: return "read_ack";
    case CBDP_CAP_CONTROL: return "control";
    case CBDP_CAP_HISTORY: return "history";
    case CBDP_CAP_TELEMETRY: return "telemetry";
    case CBDP_CAP_SUBSCRIBE_LIVE: return "subscribe_live";
    case CBDP_CAP_SUBSCRIBE_THREAD: return "subscribe_thread";
    case CBDP_CAP_SUBSCRIBE_SUMMARY: return "subscribe_summary";
    case CBDP_CAP_WRITER_LEASE: return "writer_lease";
    case CBDP_CAP_MULTI_PAGE: return "multi_page";
    case CBDP_CAP_DEVICE_CONFIG: return "device_config";
    case CBDP_CAP_COMMAND_STATUS: return "command_status";
    case CBDP_CAP_READ_UNREAD: return "read_unread";
    case CBDP_CAP_PROJECT_MANAGE: return "project_manage";
    case CBDP_CAP_EVENTS_SINCE: return "events_since";
    default: return "unknown";
    }
}

void codex_bridge_protocol_init(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id),
                 "aipassport-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(s_device_id, sizeof(s_device_id), "aipassport-unknown");
    }
    const esp_app_desc_t *description = esp_app_get_description();
    snprintf(s_firmware, sizeof(s_firmware), "%s",
             description != NULL ? description->version : "unknown");
}

const char *codex_bridge_protocol_device_id(void) { return s_device_id; }
const char *codex_bridge_protocol_model(void) { return CBDP_DEVICE_MODEL; }
const char *codex_bridge_protocol_firmware(void) { return s_firmware; }
uint64_t codex_bridge_protocol_supported_caps(void) { return supported_capabilities(); }
