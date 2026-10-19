#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codex_bridge_protocol.h"

static void test_header_codec(void)
{
    cbdp_frame_header_t input = {
        .version_major = CBDP_VERSION_MAJOR,
        .message_type = CBDP_MT_PING,
        .flags = CBDP_FLAG_REQUEST,
        .header_len = CBDP_FIXED_HEADER_BYTES,
        .payload_len = 0U,
        .topic_id = 0x50000003U,
        .seq = 7U,
        .ack_seq = 6U,
        .code = CBDP_ERR_NONE,
        .qos = CBDP_QOS_TELEMETRY,
    };
    uint8_t bytes[CBDP_FIXED_HEADER_BYTES];
    cbdp_frame_header_t decoded = {0};
    assert(cbdp_header_encode(&input, bytes, sizeof(bytes)));
    assert(bytes[0] == 0xCBU && bytes[1] == 0x01U);
    assert(bytes[2] == 1U && bytes[3] == CBDP_MT_PING);
    assert(bytes[27] == 0U);
    assert(cbdp_header_decode(bytes, sizeof(bytes), &decoded));
    assert(decoded.header_len == CBDP_FIXED_HEADER_BYTES);
    assert(decoded.topic_id == 0x50000003U);
    assert(decoded.seq == 7U && decoded.ack_seq == 6U);

    uint8_t valid[CBDP_FIXED_HEADER_BYTES + 1U] = {0};
    memcpy(valid, bytes, sizeof(bytes));
    valid[6] = 0U;
    valid[7] = CBDP_FIXED_HEADER_BYTES + 1U;
    assert(cbdp_frame_validate(valid, sizeof(valid), NULL));
    valid[7] = CBDP_FIXED_HEADER_BYTES + 2U;
    assert(!cbdp_frame_validate(valid, sizeof(valid), NULL));
    valid[7] = CBDP_FIXED_HEADER_BYTES;
    assert(!cbdp_frame_validate(valid, sizeof(valid), NULL));
    valid[27] = 1U;
    assert(!cbdp_frame_validate(valid, CBDP_FIXED_HEADER_BYTES, NULL));
}

static void test_tlv_codec(void)
{
    uint8_t buffer[128];
    size_t length = 0U;
    assert(cbdp_tlv_write_u32(buffer, sizeof(buffer), &length,
                              CBDP_TLV_TOPIC_ID, 0x12345678U));
    assert(cbdp_tlv_write_bytes(buffer, sizeof(buffer), &length,
                                CBDP_TLV_THREAD_ID, "thread", 6U));
    assert(length == 18U);
    assert(cbdp_tlv_validate_block(buffer, length, NULL));
    const cbdp_tlv_t *found = cbdp_tlv_find(buffer, length, CBDP_TLV_TOPIC_ID);
    assert(found != NULL);
    const cbdp_tlv_t topic = *found;
    found = cbdp_tlv_find(buffer, length, CBDP_TLV_THREAD_ID);
    assert(found != NULL);
    const cbdp_tlv_t thread = *found;
    uint32_t topic_value = 0U;
    const uint8_t *thread_value = NULL;
    size_t thread_length = 0U;
    assert(cbdp_tlv_read_u32(&topic, &topic_value) && topic_value == 0x12345678U);
    assert(cbdp_tlv_read_bytes(&thread, &thread_value, &thread_length));
    assert(thread_length == 6U && memcmp(thread_value, "thread", 6U) == 0);

    uint8_t duplicate[8] = {0x00U, 0x01U, 0x00U, 0x01U, 0U,
                            0x00U, 0x01U, 0x00U};
    assert(!cbdp_tlv_validate_block(duplicate, sizeof(duplicate), NULL));
    assert(!cbdp_tlv_validate_block(buffer, length - 1U, NULL));

    uint8_t trailing[5] = {0x00U, 0x01U, 0x00U, 0x00U, 0xFFU};
    assert(!cbdp_tlv_validate_block(trailing, sizeof(trailing), NULL));
}

static void test_auth_vector(void)
{
    static const uint8_t session_id[16] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
        0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U,
    };
    static const uint8_t client_nonce[16] = {0};
    static const uint8_t server_nonce[16] = {
        0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
        0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU, 0x20U,
    };
    static const uint8_t expected[32] = {
        0x7DU, 0xF2U, 0xDEU, 0xC2U, 0xFAU, 0xE6U, 0x1AU, 0x43U,
        0xD6U, 0x91U, 0x00U, 0xEAU, 0x21U, 0x7FU, 0x30U, 0x06U,
        0x7EU, 0x05U, 0x93U, 0x7BU, 0xF1U, 0x87U, 0xBCU, 0xE0U,
        0x2AU, 0xAFU, 0x35U, 0x5FU, 0x7DU, 0xC7U, 0x4AU, 0x92U,
    };
    uint8_t proof[32];
    assert(cbdp_auth_proof_hmac_sha256("0123456789abcdef0123456789abcdef",
                                       session_id, client_nonce, server_nonce,
                                       "esp32-lab", proof));
    assert(memcmp(proof, expected, sizeof(expected)) == 0);
}

static void test_send_and_audio_frames(void)
{
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    uint8_t idem[16] = {0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
                        0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU,
                        0x1FU, 0x20U};
    assert(cbdp_build_send_begin(frame, sizeof(frame), &length, 42U, 0U, 5U,
                                  77U, NULL, false, idem));
    cbdp_frame_header_t header = {0};
    if (!cbdp_frame_validate(frame, length, &header)) {
        fprintf(stderr, "send_begin length=%zu bytes=", length);
        for (size_t index = 0U; index < length; ++index) fprintf(stderr, "%02X", frame[index]);
        fprintf(stderr, "\n");
    }
    assert(cbdp_frame_validate(frame, length, &header));
    assert(header.message_type == CBDP_MT_SEND_BEGIN && header.flags == CBDP_FLAG_REQUEST);
    assert(cbdp_tlv_find(frame + CBDP_FIXED_HEADER_BYTES,
                         header.header_len - CBDP_FIXED_HEADER_BYTES,
                         CBDP_TLV_TX_ID) != NULL);
    assert(cbdp_tlv_find(frame + CBDP_FIXED_HEADER_BYTES,
                         header.header_len - CBDP_FIXED_HEADER_BYTES,
                         CBDP_TLV_TOTAL_BYTES) != NULL);

    const uint8_t payload[] = {0xAAU, 0xBBU, 0xCCU};
    assert(cbdp_build_send_chunk(frame, sizeof(frame), &length, 42U,
                                 7U, payload, sizeof(payload)));
    assert(cbdp_frame_validate(frame, length, &header));
    assert(header.message_type == CBDP_MT_SEND_CHUNK);
    assert(header.payload_len == sizeof(payload));
    assert(memcmp(frame + header.header_len, payload, sizeof(payload)) == 0);

    assert(cbdp_build_audio_begin(frame, sizeof(frame), &length, 43U, 0U,
                                  16000U, 1U, 16U, 640U, "hi",
                                  77U, NULL, false, idem));
    if (!cbdp_frame_validate(frame, length, &header)) {
        fprintf(stderr, "audio_begin length=%zu bytes=", length);
        for (size_t index = 0U; index < length; ++index) fprintf(stderr, "%02X", frame[index]);
        fprintf(stderr, "\n");
    }
    assert(cbdp_frame_validate(frame, length, &header));
    assert(header.message_type == CBDP_MT_AUDIO_BEGIN && header.payload_len == 0U);
    {
        const cbdp_tlv_t *thread = cbdp_tlv_find(
            frame + CBDP_FIXED_HEADER_BYTES,
            header.header_len - CBDP_FIXED_HEADER_BYTES, CBDP_TLV_THREAD_REF);
        uint32_t thread_value = 0U;
        assert(cbdp_tlv_read_u32(thread, &thread_value) && thread_value == 77U);
    }
    const cbdp_tlv_t *codec = cbdp_tlv_find(
        frame + CBDP_FIXED_HEADER_BYTES,
        header.header_len - CBDP_FIXED_HEADER_BYTES, CBDP_TLV_CODEC);
    uint8_t codec_value = 0U;
    assert(cbdp_tlv_read_u8(codec, &codec_value) && codec_value == 0U);
    assert(cbdp_build_audio_chunk(frame, sizeof(frame), &length, 43U, 0U,
                                  payload, sizeof(payload)));
    assert(cbdp_frame_validate(frame, length, &header));
    assert(header.message_type == CBDP_MT_AUDIO_CHUNK &&
           header.payload_len == sizeof(payload));
}

static void test_approval_and_command(void)
{
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    uint8_t idem[16] = {0};
    assert(cbdp_build_approval_resolve(frame, sizeof(frame), &length, 9U, 77U,
                                       1U, "approved", idem));
    cbdp_frame_header_t header = {0};
    assert(cbdp_frame_validate(frame, length, &header));
    assert(header.message_type == CBDP_MT_APPROVAL_RESOLVE &&
           header.flags == CBDP_FLAG_REQUEST);
    uint8_t decision = 0U;
    uint32_t approval_ref = 0U;
    const uint8_t *tlv = frame + CBDP_FIXED_HEADER_BYTES;
    const size_t tlv_len = header.header_len - CBDP_FIXED_HEADER_BYTES;
    assert(cbdp_tlv_read_u8(cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_DECISION),
                            &decision) && decision == 1U);
    assert(cbdp_tlv_read_u32(cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_APPROVAL_REF),
                             &approval_ref) && approval_ref == 77U);

    uint8_t params[32];
    size_t params_len = 0U;
    assert(cbdp_tlv_write_u32(params, sizeof(params), &params_len,
                              CBDP_TLV_THREAD_REF, 1001U));
    assert(cbdp_build_command(frame, sizeof(frame), &length, 10U,
                              CBDP_METHOD_THREAD_READ, params, params_len, idem));
    assert(cbdp_frame_validate(frame, length, &header));
    assert(header.message_type == CBDP_MT_COMMAND &&
           header.flags == (CBDP_FLAG_REQUEST | CBDP_FLAG_COMMAND));
    uint16_t method = 0U;
    assert(cbdp_tlv_read_u16(
        cbdp_tlv_find(frame + CBDP_FIXED_HEADER_BYTES,
                      header.header_len - CBDP_FIXED_HEADER_BYTES,
                      CBDP_TLV_METHOD_ID), &method));
    assert(method == CBDP_METHOD_THREAD_READ);
}

static void test_fragment_reassembly(void)
{
    uint8_t original_header[16];
    size_t original_header_len = 0U;
    assert(cbdp_tlv_write_u32(original_header, sizeof(original_header),
                              &original_header_len, CBDP_TLV_THREAD_REF, 1001U));
    const uint8_t payload[] = {0x00U, 0x11U, 0x22U, 0x33U, 0x44U};
    const uint8_t *halves[] = {payload, payload + 2U};
    const uint16_t lengths[] = {2U, 3U};
    const uint32_t crc = cbdp_crc32(payload, sizeof(payload));
    cbdp_fragment_assembly_t assembly = {0};
    uint8_t *complete_header = NULL;
    uint8_t *complete_payload = NULL;
    size_t complete_header_len = 0U;
    size_t complete_payload_len = 0U;
    for (uint16_t order = 0U; order < 2U; ++order) {
        const uint16_t index = order == 0U ? 1U : 0U;
        uint8_t fragment_tlv[256];
        size_t fragment_tlv_len = 0U;
        assert(cbdp_tlv_write_u32(fragment_tlv, sizeof(fragment_tlv),
                                  &fragment_tlv_len, CBDP_TLV_FRAGMENT_STREAM_ID, 5U));
        assert(cbdp_tlv_write_u16(fragment_tlv, sizeof(fragment_tlv),
                                  &fragment_tlv_len, CBDP_TLV_FRAGMENT_INDEX, index));
        assert(cbdp_tlv_write_u16(fragment_tlv, sizeof(fragment_tlv),
                                  &fragment_tlv_len, CBDP_TLV_FRAGMENT_TOTAL, 2U));
        assert(cbdp_tlv_write_u8(fragment_tlv, sizeof(fragment_tlv),
                                 &fragment_tlv_len, CBDP_TLV_ORIGINAL_TYPE,
                                 CBDP_MT_SEND_CHUNK));
        assert(cbdp_tlv_write_u16(fragment_tlv, sizeof(fragment_tlv),
                                  &fragment_tlv_len, CBDP_TLV_ORIGINAL_FLAGS, 0U));
        assert(cbdp_tlv_write_u32(fragment_tlv, sizeof(fragment_tlv),
                                  &fragment_tlv_len, CBDP_TLV_ORIGINAL_PAYLOAD_LEN,
                                  sizeof(payload)));
        if (index == 0U) {
            assert(cbdp_tlv_write_bytes(fragment_tlv, sizeof(fragment_tlv),
                                        &fragment_tlv_len, CBDP_TLV_ORIGINAL_HEADER_TLV,
                                        original_header, original_header_len));
        }
        assert(cbdp_tlv_write_u32(fragment_tlv, sizeof(fragment_tlv),
                                  &fragment_tlv_len, CBDP_TLV_PAYLOAD_CRC32, crc));
        const bool accepted = cbdp_fragment_accept(
            &assembly, fragment_tlv, fragment_tlv_len, CBDP_FLAG_CRC32_PRESENT,
            halves[index], lengths[index], &complete_header, &complete_header_len,
            &complete_payload, &complete_payload_len);
        assert(accepted == (order == 1U));
    }
    assert(complete_header != NULL && complete_payload != NULL);
    assert(complete_header_len == original_header_len);
    assert(memcmp(complete_header, original_header, original_header_len) == 0);
    assert(complete_payload_len == sizeof(payload));
    assert(memcmp(complete_payload, payload, sizeof(payload)) == 0);
    free(complete_header);
    free(complete_payload);
    cbdp_fragment_reset(&assembly);

    uint8_t oversized_tlv[64];
    size_t oversized_len = 0U;
    assert(cbdp_tlv_write_u32(oversized_tlv, sizeof(oversized_tlv),
                              &oversized_len, CBDP_TLV_FRAGMENT_STREAM_ID, 6U));
    assert(cbdp_tlv_write_u16(oversized_tlv, sizeof(oversized_tlv),
                              &oversized_len, CBDP_TLV_FRAGMENT_INDEX, 0U));
    assert(cbdp_tlv_write_u16(oversized_tlv, sizeof(oversized_tlv),
                              &oversized_len, CBDP_TLV_FRAGMENT_TOTAL, 1U));
    assert(cbdp_tlv_write_u8(oversized_tlv, sizeof(oversized_tlv),
                             &oversized_len, CBDP_TLV_ORIGINAL_TYPE, CBDP_MT_DELTA));
    assert(cbdp_tlv_write_u16(oversized_tlv, sizeof(oversized_tlv),
                              &oversized_len, CBDP_TLV_ORIGINAL_FLAGS, 0U));
    assert(cbdp_tlv_write_u32(oversized_tlv, sizeof(oversized_tlv),
                              &oversized_len, CBDP_TLV_ORIGINAL_PAYLOAD_LEN,
                              CBDP_MAX_REASSEMBLY_BYTES + 1U));
    assert(cbdp_tlv_write_bytes(oversized_tlv, sizeof(oversized_tlv),
                                &oversized_len, CBDP_TLV_ORIGINAL_HEADER_TLV,
                                original_header, original_header_len));
    assert(!cbdp_fragment_accept(&assembly, oversized_tlv, oversized_len, 0U,
                                 payload, sizeof(payload), &complete_header,
                                 &complete_header_len, &complete_payload,
                                 &complete_payload_len));
    cbdp_fragment_reset(&assembly);
}

static void test_known_method_and_object_ids(void)
{
    assert(CBDP_METHOD_THREADS_LIST == 0x0002U);
    assert(CBDP_METHOD_THREAD_READ == 0x0003U);
    assert(CBDP_METHOD_THREAD_START == 0x0004U);
    assert(CBDP_METHOD_TURN_INTERRUPT == 0x0007U);
    assert(CBDP_METHOD_PROJECTS_LIST == 0x000DU);
    assert(CBDP_METHOD_MESSAGE_READ_UPDATE == 0x00A0U);
    assert(CBDP_TLV_OBJECT == 0x00A0U);
    assert(CBDP_TLV_HISTORY_DEPTH == 0x0014U);
    assert(CBDP_TLV_READ_AT_MS == 0x0054U);
    assert(CBDP_TLV_APPROVAL_DETAIL == 0x0146U);
}

static void test_welcome_max_audio_bytes(void)
{
    uint8_t tlv[256];
    size_t length = 0U;
    const uint8_t session_id[16] = {1U};
    const uint8_t server_nonce[16] = {2U};
    assert(cbdp_tlv_write_u16(tlv, sizeof(tlv), &length, CBDP_TLV_VERSION_MINOR, 0U));
    assert(cbdp_tlv_write_bytes(tlv, sizeof(tlv), &length, CBDP_TLV_SESSION_ID,
                                session_id, sizeof(session_id)));
    assert(cbdp_tlv_write_bytes(tlv, sizeof(tlv), &length, CBDP_TLV_SERVER_NONCE,
                                server_nonce, sizeof(server_nonce)));
    assert(cbdp_tlv_write_u8(tlv, sizeof(tlv), &length, CBDP_TLV_AUTH_MODE,
                             CBDP_AUTH_HMAC_SHA256));
    assert(cbdp_tlv_write_u32(tlv, sizeof(tlv), &length, CBDP_TLV_MAX_FRAME_BYTES, 1024U));
    assert(cbdp_tlv_write_u32(tlv, sizeof(tlv), &length, CBDP_TLV_MAX_TEXT_CHARS, 512U));
    assert(cbdp_tlv_write_u32(tlv, sizeof(tlv), &length, CBDP_TLV_MAX_TEXT_BYTES, 1536U));
    assert(cbdp_tlv_write_u16(tlv, sizeof(tlv), &length, CBDP_TLV_QUEUE_DEPTH, 32U));
    assert(cbdp_tlv_write_u16(tlv, sizeof(tlv), &length, CBDP_TLV_HEARTBEAT_SECONDS, 15U));
    assert(cbdp_tlv_write_u16(tlv, sizeof(tlv), &length, CBDP_TLV_MAX_CHUNK_BYTES, 384U));
    assert(cbdp_tlv_write_u32(tlv, sizeof(tlv), &length, CBDP_TLV_WRITER_LEASE_SECONDS, 300U));
    assert(cbdp_tlv_write_u32(tlv, sizeof(tlv), &length, CBDP_TLV_MAX_AUDIO_BYTES, 960000U));
    assert(cbdp_tlv_write_u32(tlv, sizeof(tlv), &length, CBDP_TLV_REQUEST_ID, 7U));
    assert(cbdp_tlv_write_u64(tlv, sizeof(tlv), &length, CBDP_TLV_SERVER_TIME_MS, 123U));

    cbdp_session_t session = {0};
    assert(cbdp_parse_welcome(tlv, length, &session));
    assert(session.max_audio_bytes == 960000U);

    // When the host omits max_audio_bytes the device must still keep a bound.
    uint8_t without[256];
    size_t without_length = 0U;
    assert(cbdp_tlv_write_u16(without, sizeof(without), &without_length,
                              CBDP_TLV_VERSION_MINOR, 0U));
    assert(cbdp_tlv_write_bytes(without, sizeof(without), &without_length,
                                CBDP_TLV_SESSION_ID, session_id, sizeof(session_id)));
    assert(cbdp_tlv_write_bytes(without, sizeof(without), &without_length,
                                CBDP_TLV_SERVER_NONCE, server_nonce, sizeof(server_nonce)));
    assert(cbdp_tlv_write_u8(without, sizeof(without), &without_length,
                             CBDP_TLV_AUTH_MODE, CBDP_AUTH_HMAC_SHA256));
    assert(cbdp_tlv_write_u32(without, sizeof(without), &without_length,
                              CBDP_TLV_MAX_FRAME_BYTES, 1024U));
    assert(cbdp_tlv_write_u32(without, sizeof(without), &without_length,
                              CBDP_TLV_MAX_TEXT_CHARS, 512U));
    assert(cbdp_tlv_write_u32(without, sizeof(without), &without_length,
                              CBDP_TLV_MAX_TEXT_BYTES, 1536U));
    assert(cbdp_tlv_write_u16(without, sizeof(without), &without_length,
                              CBDP_TLV_QUEUE_DEPTH, 32U));
    assert(cbdp_tlv_write_u16(without, sizeof(without), &without_length,
                              CBDP_TLV_HEARTBEAT_SECONDS, 15U));
    assert(cbdp_tlv_write_u16(without, sizeof(without), &without_length,
                              CBDP_TLV_MAX_CHUNK_BYTES, 384U));
    assert(cbdp_tlv_write_u32(without, sizeof(without), &without_length,
                              CBDP_TLV_WRITER_LEASE_SECONDS, 300U));
    assert(cbdp_tlv_write_u32(without, sizeof(without), &without_length,
                              CBDP_TLV_REQUEST_ID, 7U));
    assert(cbdp_tlv_write_u64(without, sizeof(without), &without_length,
                              CBDP_TLV_SERVER_TIME_MS, 123U));
    cbdp_session_t fallback = {0};
    assert(cbdp_parse_welcome(without, without_length, &fallback));
    assert(fallback.max_audio_bytes == CBDP_MAX_REASSEMBLY_BYTES);
}

static void test_subscribe_cursor_is_optional(void)
{
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    cbdp_frame_header_t header = {0};

    // Section 15.4 lists `cursor` as optional. Omitting it must not emit a zero
    // cursor: an explicit 0 asks the host to replay the whole topic backlog,
    // which both floods a small device and strands the session.
    assert(cbdp_build_subscribe(frame, sizeof(frame), &length, 9U, 0x50000003U,
                                CBDP_SCOPE_SUMMARY, CBDP_QOS_STATE, 0U));
    assert(cbdp_frame_validate(frame, length, &header));
    assert(header.message_type == CBDP_MT_SUBSCRIBE);
    assert(cbdp_tlv_find(frame + CBDP_FIXED_HEADER_BYTES,
                         header.header_len - CBDP_FIXED_HEADER_BYTES,
                         CBDP_TLV_CURSOR) == NULL);
    assert(cbdp_tlv_find(frame + CBDP_FIXED_HEADER_BYTES,
                         header.header_len - CBDP_FIXED_HEADER_BYTES,
                         CBDP_TLV_TOPIC_ID) != NULL);

    // A non-zero cursor is a genuine resume point and must be carried.
    assert(cbdp_build_subscribe(frame, sizeof(frame), &length, 10U, 0x1000000dU,
                                CBDP_SCOPE_THREAD, CBDP_QOS_STATE, 4242U));
    assert(cbdp_frame_validate(frame, length, &header));
    const cbdp_tlv_t *cursor = cbdp_tlv_find(
        frame + CBDP_FIXED_HEADER_BYTES,
        header.header_len - CBDP_FIXED_HEADER_BYTES, CBDP_TLV_CURSOR);
    uint64_t value = 0U;
    assert(cursor != NULL);
    {
        const uint8_t *raw = NULL;
        size_t raw_length = 0U;
        assert(cbdp_tlv_read_bytes(cursor, &raw, &raw_length) && raw_length == 8U);
        for (size_t index = 0U; index < raw_length; ++index) {
            value = (value << 8) | raw[index];
        }
    }
    assert(value == 4242U);
}

int main(void)
{
    test_header_codec();
    test_tlv_codec();
    test_auth_vector();
    test_send_and_audio_frames();
    test_approval_and_command();
    test_fragment_reassembly();
    test_known_method_and_object_ids();
    test_welcome_max_audio_bytes();
    test_subscribe_cursor_is_optional();
    puts("codex_bridge_protocol: all tests passed");
    return 0;
}
