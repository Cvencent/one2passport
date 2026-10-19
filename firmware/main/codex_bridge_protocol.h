#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CODEX_BRIDGE_LINE_MAX 768
#define CODEX_BRIDGE_BODY_MAX 4096
#define CODEX_BRIDGE_ID_MAX 80
#define CODEX_BRIDGE_CAPABILITY_MAX 8
#define CODEX_BRIDGE_CAPABILITY_LEN 24

#define CBDP_MAGIC_0 0xCBU
#define CBDP_MAGIC_1 0x01U
#define CBDP_VERSION_MAJOR 1U
#define CBDP_VERSION_MINOR 0U
#define CBDP_FIXED_HEADER_BYTES 28U
#define CBDP_MAX_TLVS 256U
#define CBDP_MAX_TLV_DEPTH 8U
#define CBDP_MAX_FRAME_BYTES 1024U
#define CBDP_MAX_REASSEMBLY_BYTES 65536U
#define CBDP_MAX_TEXT_BYTES 1536U
#define CBDP_MAX_TEXT_CHARS 512U
#define CBDP_MAX_CHUNK_BYTES 384U
#define CBDP_QUEUE_DEPTH 32U
#define CBDP_HEARTBEAT_SECONDS 15U
#define CBDP_WRITER_LEASE_SECONDS 300U
#define CBDP_MAX_SUBSCRIPTIONS 4U
#define CBDP_SNAPSHOT_PAGE_OBJECTS 2U
#define CBDP_FRAGMENT_MAX 512U

uint32_t cbdp_crc32(const void *data, size_t length);
bool cbdp_sha256(const void *data, size_t length, uint8_t digest[32]);

/* 17.4: build the 16-byte idempotency key
 * first_16_bytes(SHA-256(device_id || request_id || method_id || params)).
 * Streamed into the digest so callers never need a combined scratch buffer. */
void cbdp_build_idem(uint8_t idem[16], uint32_t request_id, uint16_t method_id,
                     const uint8_t *params, size_t params_length);

typedef enum {
    CBDP_PROFILE_MICRO = 0,
    CBDP_PROFILE_PANEL = 1,
    CBDP_PROFILE_FULL = 2,
} cbdp_profile_t;

typedef enum {
    CBDP_MT_HELLO = 0x01,
    CBDP_MT_WELCOME = 0x02,
    CBDP_MT_AUTH = 0x03,
    CBDP_MT_AUTH_OK = 0x04,
    CBDP_MT_PING = 0x05,
    CBDP_MT_PONG = 0x06,
    CBDP_MT_SUBSCRIBE = 0x07,
    CBDP_MT_UNSUBSCRIBE = 0x08,
    CBDP_MT_SNAPSHOT = 0x09,
    CBDP_MT_DELTA = 0x0A,
    CBDP_MT_ACK = 0x0B,
    CBDP_MT_RESYNC = 0x0C,
    CBDP_MT_COMMAND = 0x0D,
    CBDP_MT_RESULT = 0x0E,
    CBDP_MT_ERROR = 0x0F,
    CBDP_MT_SEND_BEGIN = 0x10,
    CBDP_MT_SEND_CHUNK = 0x11,
    CBDP_MT_SEND_END = 0x12,
    CBDP_MT_AUDIO_BEGIN = 0x13,
    CBDP_MT_AUDIO_CHUNK = 0x14,
    CBDP_MT_AUDIO_END = 0x15,
    CBDP_MT_APPROVAL_REQUEST = 0x16,
    CBDP_MT_APPROVAL_RESOLVE = 0x17,
    CBDP_MT_DEVICE_STATUS = 0x18,
    CBDP_MT_CAPABILITY_UPDATE = 0x19,
    CBDP_MT_CLOSE = 0x1A,
    CBDP_MT_FRAGMENT = 0x1B,
    CBDP_MT_DISCOVERY = 0x1C,
    CBDP_MT_DISCOVERY_REPLY = 0x1D,
} cbdp_message_type_t;

enum {
    CBDP_FLAG_MORE_FRAGMENTS = 0x0001,
    CBDP_FLAG_CRC32_PRESENT = 0x0004,
    CBDP_FLAG_URGENT = 0x0008,
    CBDP_FLAG_NO_ACK = 0x0010,
    CBDP_FLAG_TEXT_TRUNCATED = 0x0020,
    CBDP_FLAG_SNAPSHOT_PAGE = 0x0040,
    CBDP_FLAG_RESUMED = 0x0080,
    CBDP_FLAG_REQUEST = 0x0100,
    CBDP_FLAG_RESPONSE = 0x0200,
    CBDP_FLAG_EVENT = 0x0400,
    CBDP_FLAG_COMMAND = 0x0800,
};

enum {
    CBDP_QOS_CRITICAL = 0,
    CBDP_QOS_STATE = 1,
    CBDP_QOS_TELEMETRY = 2,
};

enum {
    CBDP_AUTH_PAIRING_TOKEN = 0,
    CBDP_AUTH_HMAC_SHA256 = 1,
    CBDP_AUTH_MTLS = 2,
};

enum {
    CBDP_TRANSPORT_PLAIN_LAN = 0,
    CBDP_TRANSPORT_TLS = 1,
};

enum {
    CBDP_SCOPE_SUMMARY = 0,
    CBDP_SCOPE_THREAD = 1,
    CBDP_SCOPE_LIVE = 2,
    CBDP_SCOPE_DEVICE = 3,
};

enum {
    CBDP_CAP_STATUS = 0,
    CBDP_CAP_TEXT = 1,
    CBDP_CAP_TASKS = 2,
    CBDP_CAP_RESULTS = 3,
    CBDP_CAP_APPROVAL = 4,
    CBDP_CAP_AUDIO = 5,
    CBDP_CAP_VOICE = 6,
    CBDP_CAP_READ_ACK = 7,
    CBDP_CAP_CONTROL = 8,
    CBDP_CAP_HISTORY = 9,
    CBDP_CAP_TELEMETRY = 10,
    CBDP_CAP_SUBSCRIBE_LIVE = 11,
    CBDP_CAP_SUBSCRIBE_THREAD = 12,
    CBDP_CAP_SUBSCRIBE_SUMMARY = 13,
    CBDP_CAP_WRITER_LEASE = 14,
    CBDP_CAP_MULTI_PAGE = 15,
    CBDP_CAP_TLS = 17,
    CBDP_CAP_DEVICE_CONFIG = 19,
    CBDP_CAP_COMMAND_STATUS = 20,
    CBDP_CAP_READ_UNREAD = 45,
    CBDP_CAP_PROJECT_MANAGE = 46,
    CBDP_CAP_EVENTS_SINCE = 61,
};

enum {
    CBDP_TLV_VERSION_MINOR = 0x0001,
    CBDP_TLV_DEVICE_ID = 0x0002,
    CBDP_TLV_MODEL = 0x0003,
    CBDP_TLV_FIRMWARE = 0x0004,
    CBDP_TLV_NAME = 0x0005,
    CBDP_TLV_PROFILE = 0x0006,
    CBDP_TLV_SUPPORTED_CAPS = 0x0007,
    CBDP_TLV_ENABLED_CAPS = 0x0008,
    CBDP_TLV_EFFECTIVE_CAPS = 0x0009,
    CBDP_TLV_TRANSPORT_SECURITY = 0x000A,
    CBDP_TLV_CLIENT_NONCE = 0x000B,
    CBDP_TLV_SESSION_ID = 0x000C,
    CBDP_TLV_SERVER_NONCE = 0x000D,
    CBDP_TLV_AUTH_PROOF = 0x000E,
    CBDP_TLV_AUTH_MODE = 0x000F,
    CBDP_TLV_MAX_FRAME_BYTES = 0x0010,
    CBDP_TLV_MAX_TEXT_CHARS = 0x0011,
    CBDP_TLV_QUEUE_DEPTH = 0x0012,
    CBDP_TLV_HEARTBEAT_SECONDS = 0x0013,
    CBDP_TLV_HISTORY_DEPTH = 0x0014,
    CBDP_TLV_MAX_SUBSCRIPTIONS = 0x0015,
    CBDP_TLV_MAX_AUDIO_BYTES = 0x0016,
    CBDP_TLV_POLICY_VERSION = 0x0017,
    CBDP_TLV_TRANSPORT_ALLOWED = 0x0018,
    CBDP_TLV_SNAPSHOT_PAGE_OBJECTS = 0x0019,
    CBDP_TLV_MAX_CHUNK_BYTES = 0x001A,
    CBDP_TLV_WRITER_LEASE_SECONDS = 0x001E,
    CBDP_TLV_MAX_TEXT_BYTES = 0x001F,
    CBDP_TLV_DEVICE_REF = 0x0020,
    CBDP_TLV_THREAD_REF = 0x0021,
    CBDP_TLV_THREAD_ID = 0x0022,
    CBDP_TLV_TURN_REF = 0x0023,
    CBDP_TLV_TURN_ID = 0x0024,
    CBDP_TLV_MESSAGE_REF = 0x0025,
    CBDP_TLV_MESSAGE_ID = 0x0026,
    CBDP_TLV_APPROVAL_REF = 0x0027,
    CBDP_TLV_APPROVAL_ID = 0x0028,
    CBDP_TLV_EVENT_ID = 0x0029,
    CBDP_TLV_REQUEST_ID = 0x002A,
    CBDP_TLV_IDEM = 0x002B,
    CBDP_TLV_METHOD_ID = 0x002C,
    CBDP_TLV_TX_ID = 0x002D,
    CBDP_TLV_SNAPSHOT_ID = 0x002E,
    CBDP_TLV_TOPIC_ID = 0x0030,
    CBDP_TLV_SCOPE = 0x0031,
    CBDP_TLV_QOS = 0x0032,
    CBDP_TLV_CURSOR = 0x0033,
    CBDP_TLV_TOPIC_SEQ = 0x0034,
    CBDP_TLV_GLOBAL_SEQ = 0x0035,
    CBDP_TLV_REVISION = 0x0036,
    CBDP_TLV_EVENT_TYPE = 0x0037,
    CBDP_TLV_EVENT_PAYLOAD = 0x0038,
    CBDP_TLV_SNAPSHOT_REVISION = 0x003C,
    CBDP_TLV_PAGE_INDEX = 0x003D,
    CBDP_TLV_PAGE_COUNT = 0x003E,
    CBDP_TLV_OBJECT_TYPE = 0x003F,
    CBDP_TLV_TITLE = 0x0040,
    CBDP_TLV_SUMMARY = 0x0041,
    CBDP_TLV_BODY = 0x0042,
    CBDP_TLV_BODY_REF = 0x0043,
    CBDP_TLV_STATUS = 0x0044,
    CBDP_TLV_KIND = 0x0045,
    CBDP_TLV_CONTENT_TYPE = 0x0046,
    CBDP_TLV_TOTAL_BYTES = 0x0047,
    CBDP_TLV_OFFSET = 0x0048,
    CBDP_TLV_DATA = 0x0049,
    CBDP_TLV_CRC32 = 0x004A,
    CBDP_TLV_CREATED_AT_MS = 0x004B,
    CBDP_TLV_UPDATED_AT_MS = 0x004C,
    CBDP_TLV_STARTED_AT_MS = 0x004D,
    CBDP_TLV_COMPLETED_AT_MS = 0x004E,
    CBDP_TLV_CURRENT_ACTION = 0x004F,
    CBDP_TLV_MODEL_NAME = 0x0051,
    CBDP_TLV_READ_AT_MS = 0x0054,
    CBDP_TLV_SENDER = 0x0055,
    CBDP_TLV_NEXT_OFFSET = 0x0059,
    CBDP_TLV_PARAMS = 0x0060,
    CBDP_TLV_RESULT = 0x0061,
    CBDP_TLV_ERROR_MESSAGE = 0x0062,
    CBDP_TLV_ERROR_DETAIL = 0x0063,
    CBDP_TLV_STATE = 0x0064,
    CBDP_TLV_ACCEPTED_AT_MS = 0x0065,
    CBDP_TLV_COMMAND_COMPLETED_AT_MS = 0x0066,
    CBDP_TLV_RETRY_AFTER_MS = 0x0067,
    CBDP_TLV_PRIORITY = 0x0068,
    CBDP_TLV_DEADLINE_MS = 0x0069,
    CBDP_TLV_FORCE = 0x006A,
    CBDP_TLV_EXPECTED_REVISION = 0x006B,
    CBDP_TLV_LEASE_GENERATION = 0x006C,
    CBDP_TLV_LEASE_EXPIRES_AT_MS = 0x006D,
    CBDP_TLV_MODE = 0x0070,
    CBDP_TLV_BASE_REVISION = 0x0071,
    CBDP_TLV_CLIENT_TIMESTAMP_MS = 0x0072,
    CBDP_TLV_CODEC = 0x0073,
    CBDP_TLV_SAMPLE_RATE = 0x0074,
    CBDP_TLV_CHANNELS = 0x0075,
    CBDP_TLV_BITS_PER_SAMPLE = 0x0076,
    CBDP_TLV_DURATION_MS = 0x0077,
    CBDP_TLV_TRANSCRIPT = 0x0078,
    CBDP_TLV_SHA256 = 0x0079,
    CBDP_TLV_AUDIO_FORMAT = 0x007A,
    CBDP_TLV_USE_ACTIVE_THREAD = 0x007B,
    CBDP_TLV_PROMPT = 0x0080,
    CBDP_TLV_OPTIONS = 0x0081,
    CBDP_TLV_DECISION = 0x0082,
    CBDP_TLV_RESPONSE = 0x0083,
    CBDP_TLV_RISK = 0x0084,
    CBDP_TLV_EXPIRES_AT_MS = 0x0085,
    CBDP_TLV_DEFAULT_DECISION = 0x0086,
    CBDP_TLV_APPROVAL_KIND = 0x0087,
    CBDP_TLV_APPROVAL_DETAIL = 0x0146,
    CBDP_TLV_PROJECT_REF = 0x0122,
    CBDP_TLV_PROJECT_ID = 0x0123,
    CBDP_TLV_ONLINE = 0x0090,
    CBDP_TLV_DEVICE_STATE = 0x0091,
    CBDP_TLV_QUEUE_DEPTH_CURRENT = 0x0092,
    CBDP_TLV_LAST_SEEN_MS = 0x0094,
    CBDP_TLV_PING_ID = 0x0097,
    CBDP_TLV_SERVER_TIME_MS = 0x0098,
    CBDP_TLV_ECHO = 0x0099,
    CBDP_TLV_OBJECT = 0x00A0,
    CBDP_TLV_OBJECTS = 0x00A1,
    CBDP_TLV_OBJECT_COUNT = 0x00A2,
    CBDP_TLV_COMPLETE = 0x00A3,
    CBDP_TLV_NEXT_CURSOR = 0x00A5,
    CBDP_TLV_SNAPSHOT_KIND = 0x00A6,
    CBDP_TLV_THREAD_STATE = 0x00A7,
    CBDP_TLV_OWNER_STATE = 0x00A8,
    CBDP_TLV_RESYNC_REASON = 0x00A9,
    CBDP_TLV_CAPABILITY_CATALOG = 0x00AA,
    CBDP_TLV_CAPABILITY_SOURCE = 0x00AB,
    CBDP_TLV_CAPABILITY_BITS = 0x00AC,
    CBDP_TLV_PORT = 0x00B0,
    CBDP_TLV_DISCOVERY_NONCE = 0x00B1,
    CBDP_TLV_DISCOVERY_SCOPE = 0x00B2,
    CBDP_TLV_BRIDGE_INSTANCE_ID = 0x00B3,
    CBDP_TLV_PRIORITY_CLASS = 0x00B4,
    CBDP_TLV_CAPABILITY_BLOB = 0x00B5,
    CBDP_TLV_WINDOW_FRAMES = 0x00B6,
    CBDP_TLV_WINDOW_BYTES = 0x00B7,
    CBDP_TLV_RETRANSMIT_TIMEOUT_MS = 0x00B8,
    CBDP_TLV_MAX_RETRIES = 0x00B9,
    CBDP_TLV_FRAGMENT_TIMEOUT_MS = 0x00BA,
    CBDP_TLV_ACK_DELAY_MS = 0x00BB,
    CBDP_TLV_FRAGMENT_STREAM_ID = 0x00F0,
    CBDP_TLV_FRAGMENT_INDEX = 0x00F1,
    CBDP_TLV_FRAGMENT_TOTAL = 0x00F2,
    CBDP_TLV_ORIGINAL_TYPE = 0x00F3,
    CBDP_TLV_ORIGINAL_FLAGS = 0x00F4,
    CBDP_TLV_ORIGINAL_PAYLOAD_LEN = 0x00F5,
    CBDP_TLV_ORIGINAL_HEADER_TLV = 0x00F6,
    CBDP_TLV_PAYLOAD_CRC32 = 0x00FF,
};

enum {
    CBDP_METHOD_STATUS_READ = 0x0001,
    CBDP_METHOD_THREADS_LIST = 0x0002,
    CBDP_METHOD_THREAD_READ = 0x0003,
    CBDP_METHOD_THREAD_START = 0x0004,
    CBDP_METHOD_TURN_START = 0x0005,
    CBDP_METHOD_TURN_STEER = 0x0006,
    CBDP_METHOD_TURN_INTERRUPT = 0x0007,
    CBDP_METHOD_APPROVALS_LIST = 0x0008,
    CBDP_METHOD_APPROVAL_RESOLVE = 0x0009,
    CBDP_METHOD_THREAD_CLAIM = 0x000A,
    CBDP_METHOD_THREAD_RELEASE = 0x000B,
    CBDP_METHOD_MODELS_LIST = 0x000C,
    CBDP_METHOD_PROJECTS_LIST = 0x000D,
    CBDP_METHOD_DEVICE_POLICY_READ = 0x000E,
    CBDP_METHOD_COMMAND_STATUS = 0x000F,
    CBDP_METHOD_THREAD_SEARCH = 0x0011,
    CBDP_METHOD_MESSAGE_READ_UPDATE = 0x00A0,
    CBDP_METHOD_THREAD_READ_STATE_UPDATE = 0x001E,
    CBDP_METHOD_EVENTS_SINCE = 0x00B3,
};

enum {
    CBDP_EVENT_THREAD_CREATED = 0x0101,
    CBDP_EVENT_THREAD_UPDATED = 0x0102,
    CBDP_EVENT_THREAD_STATUS_CHANGED = 0x0104,
    CBDP_EVENT_THREAD_OWNER_CHANGED = 0x0105,
    CBDP_EVENT_TURN_STARTED = 0x0201,
    CBDP_EVENT_TURN_UPDATED = 0x0202,
    CBDP_EVENT_TURN_COMPLETED = 0x0203,
    CBDP_EVENT_TURN_FAILED = 0x0204,
    CBDP_EVENT_TURN_INTERRUPTED = 0x0205,
    CBDP_EVENT_TURN_PROGRESS = 0x0206,
    CBDP_EVENT_MESSAGE_APPENDED = 0x0301,
    CBDP_EVENT_MESSAGE_UPDATED = 0x0302,
    CBDP_EVENT_MESSAGE_READ = 0x0303,
    CBDP_EVENT_APPROVAL_REQUESTED = 0x0401,
    CBDP_EVENT_APPROVAL_RESOLVED = 0x0402,
    CBDP_EVENT_APPROVAL_EXPIRED = 0x0403,
    CBDP_EVENT_SYSTEM_RESYNC_REQUIRED = 0x0701,
    CBDP_EVENT_ACTIVE_THREAD_CHANGED = 0x0E08,
    CBDP_EVENT_ACTIVE_TURN_CHANGED = 0x0E09,
    CBDP_EVENT_RESUME_REQUIRED = 0x0E0B,
};

enum {
    CBDP_ERR_NONE = 0x0000,
    CBDP_ERR_NOT_FOUND = 0x0001,
    CBDP_ERR_BUSY = 0x0002,
    CBDP_ERR_UNAUTHORIZED = 0x0003,
    CBDP_ERR_CAPABILITY_NOT_ENABLED = 0x0004,
    CBDP_ERR_INVALID_FRAME = 0x0005,
    CBDP_ERR_INVALID_PARAM = 0x0006,
    CBDP_ERR_INVALID_STATE = 0x0007,
    CBDP_ERR_IDEMPOTENCY_CONFLICT = 0x0008,
    CBDP_ERR_STALE_CURSOR = 0x0009,
    CBDP_ERR_RESYNC_REQUIRED = 0x000A,
    CBDP_ERR_UPSTREAM_UNAVAILABLE = 0x000B,
    CBDP_ERR_WRITER_CONFLICT = 0x000C,
    CBDP_ERR_PAYLOAD_TOO_LARGE = 0x000D,
    CBDP_ERR_UNSUPPORTED_FEATURE = 0x000E,
    CBDP_ERR_TIMEOUT = 0x000F,
    CBDP_ERR_POLICY_DENIED = 0x0010,
    CBDP_ERR_REVISION_CONFLICT = 0x0011,
    CBDP_ERR_DUPLICATE = 0x0012,
    CBDP_ERR_RATE_LIMITED = 0x0013,
    CBDP_ERR_CANCELLED = 0x0014,
    CBDP_ERR_INTERNAL = 0x0015,
    CBDP_ERR_PROTOCOL_VERSION = 0x0016,
    CBDP_ERR_AUTH_EXPIRED = 0x0017,
    CBDP_ERR_TOKEN_ROTATED = 0x0018,
    CBDP_ERR_QUEUE_FULL = 0x0019,
    CBDP_ERR_FRAGMENT_ERROR = 0x001A,
    CBDP_ERR_CONTENT_UNAVAILABLE = 0x001B,
};

typedef struct {
    uint8_t version_major;
    uint8_t message_type;
    uint16_t flags;
    uint16_t header_len;
    uint32_t payload_len;
    uint32_t topic_id;
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t code;
    uint8_t qos;
} cbdp_frame_header_t;

typedef struct {
    uint16_t tag;
    uint16_t length;
    const uint8_t *value;
} cbdp_tlv_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
} cbdp_tlv_cursor_t;

typedef struct {
    uint32_t request_id;
    uint32_t device_ref;
    uint16_t version_minor;
    uint32_t max_frame_bytes;
    uint32_t max_text_chars;
    uint32_t max_text_bytes;
    uint16_t queue_depth;
    uint16_t heartbeat_seconds;
    uint16_t max_chunk_bytes;
    uint32_t max_audio_bytes;
    uint32_t writer_lease_seconds;
    uint32_t policy_version;
    uint8_t profile;
    uint8_t auth_mode;
    uint8_t transport_security;
    uint8_t session_id_bytes[16];
    uint8_t server_nonce[16];
    uint64_t supported_caps;
    uint64_t effective_caps;
    bool ready;
} cbdp_session_t;

typedef struct {
    uint8_t message_type;
    uint16_t flags;
    uint32_t topic_id;
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t code;
    uint8_t qos;
    uint8_t original_type;
    uint16_t original_flags;
    uint32_t original_payload_len;
    uint32_t event_type;
    uint32_t method_id;
    uint32_t request_id;
    uint32_t tx_id;
    uint32_t stream_id;
    uint16_t fragment_index;
    uint16_t fragment_total;
    uint64_t cursor;
    uint64_t revision;
    uint64_t global_seq;
    uint64_t topic_seq;
    uint8_t decision;
    uint8_t approval_kind;
    const uint8_t *event_payload;
    size_t event_payload_length;
} cbdp_event_header_t;

typedef struct {
    uint32_t stream_id;
    uint16_t total;
    uint16_t received;
    uint32_t expected_payload_len;
    uint8_t original_type;
    uint16_t original_flags;
    uint8_t *original_header_tlv;
    uint16_t original_header_len;
    bool original_header_present;
    uint8_t **fragments;
    uint16_t *fragment_lengths;
    uint32_t received_bytes;
    uint8_t *received_map;
    uint32_t expected_crc32;
    bool crc32_present;
    bool active;
} cbdp_fragment_assembly_t;

typedef struct {
    uint32_t tx_id;
    uint32_t offset;
    uint32_t total_bytes;
    uint32_t request_id;
    uint8_t mode;
} cbdp_send_transaction_t;

void codex_bridge_protocol_init(void);
const char *codex_bridge_protocol_device_id(void);
const char *codex_bridge_protocol_model(void);
const char *codex_bridge_protocol_firmware(void);
uint64_t codex_bridge_protocol_supported_caps(void);

bool cbdp_header_encode(const cbdp_frame_header_t *header, uint8_t *out, size_t capacity);
bool cbdp_header_decode(const uint8_t *data, size_t length, cbdp_frame_header_t *header);
bool cbdp_frame_validate(const uint8_t *data, size_t length, cbdp_frame_header_t *header);
bool cbdp_tlv_next(cbdp_tlv_cursor_t *cursor, cbdp_tlv_t *tlv);
const cbdp_tlv_t *cbdp_tlv_find(const uint8_t *tlv, size_t length, uint16_t tag);
bool cbdp_tlv_validate_block(const uint8_t *tlv, size_t length, uint16_t *duplicate_tag);
uint16_t cbdp_read_u16(const uint8_t *data);
bool cbdp_tlv_read_u8(const cbdp_tlv_t *tlv, uint8_t *value);
bool cbdp_tlv_read_u16(const cbdp_tlv_t *tlv, uint16_t *value);
bool cbdp_tlv_read_u32(const cbdp_tlv_t *tlv, uint32_t *value);
bool cbdp_tlv_read_u64(const cbdp_tlv_t *tlv, uint64_t *value);
bool cbdp_tlv_read_i64(const cbdp_tlv_t *tlv, int64_t *value);
bool cbdp_tlv_read_bool(const cbdp_tlv_t *tlv, bool *value);
bool cbdp_tlv_read_bytes(const cbdp_tlv_t *tlv, const uint8_t **data, size_t *length);
bool cbdp_tlv_write_u8(uint8_t *buffer, size_t capacity, size_t *offset,
                       uint16_t tag, uint8_t value);
bool cbdp_tlv_write_u16(uint8_t *buffer, size_t capacity, size_t *offset,
                        uint16_t tag, uint16_t value);
bool cbdp_tlv_write_u32(uint8_t *buffer, size_t capacity, size_t *offset,
                        uint16_t tag, uint32_t value);
bool cbdp_tlv_write_u64(uint8_t *buffer, size_t capacity, size_t *offset,
                        uint16_t tag, uint64_t value);
bool cbdp_tlv_write_i64(uint8_t *buffer, size_t capacity, size_t *offset,
                        uint16_t tag, int64_t value);
bool cbdp_tlv_write_bytes(uint8_t *buffer, size_t capacity, size_t *offset,
                          uint16_t tag, const void *data, size_t length);
uint32_t cbdp_crc32(const void *data, size_t length);

bool cbdp_auth_proof_hmac_sha256(const char *pairing_token,
                                  const uint8_t session_id[16],
                                  const uint8_t client_nonce[16],
                                  const uint8_t server_nonce[16],
                                  const char *device_id,
                                  uint8_t proof[32]);
bool cbdp_build_hello(uint8_t *buffer, size_t capacity, size_t *length,
                      uint32_t request_id, const uint8_t client_nonce[16]);
bool cbdp_build_auth(uint8_t *buffer, size_t capacity, size_t *length,
                     const cbdp_session_t *session, const char *pairing_token,
                     const uint8_t client_nonce[16], uint32_t request_id);
bool cbdp_build_ping(uint8_t *buffer, size_t capacity, size_t *length, uint32_t ping_id);
bool cbdp_build_pong(uint8_t *buffer, size_t capacity, size_t *length, uint32_t echo);
bool cbdp_build_ack(uint8_t *buffer, size_t capacity, size_t *length,
                    uint32_t ack_seq, uint32_t topic_id, uint8_t scope, uint64_t cursor);
bool cbdp_build_resync(uint8_t *buffer, size_t capacity, size_t *length,
                       uint32_t request_id, uint8_t reason);
bool cbdp_build_subscribe(uint8_t *buffer, size_t capacity, size_t *length,
                          uint32_t request_id, uint32_t topic_id, uint8_t scope,
                          uint8_t qos, uint64_t cursor);
bool cbdp_build_command(uint8_t *buffer, size_t capacity, size_t *length,
                        uint32_t request_id, uint16_t method_id,
                        const uint8_t *params, size_t params_length,
                        const uint8_t idem[16]);
// 18.1: SEND_BEGIN/AUDIO_BEGIN must name the target thread unless the device
// explicitly opts into the host's active thread.
bool cbdp_build_send_begin(uint8_t *buffer, size_t capacity, size_t *length,
                           uint32_t tx_id, uint8_t mode, uint32_t total_bytes,
                           uint32_t thread_ref, const char *thread_id,
                           bool use_active_thread, const uint8_t idem[16]);
bool cbdp_build_send_chunk(uint8_t *buffer, size_t capacity, size_t *length,
                           uint32_t tx_id, uint32_t offset,
                           const uint8_t *payload, size_t payload_length);
bool cbdp_build_send_end(uint8_t *buffer, size_t capacity, size_t *length,
                         uint32_t tx_id, uint32_t total_bytes);
bool cbdp_build_audio_begin(uint8_t *buffer, size_t capacity, size_t *length,
                            uint32_t tx_id, uint8_t codec, uint32_t sample_rate,
                            uint8_t channels, uint8_t bits_per_sample,
                            uint32_t total_bytes, const char *transcript,
                            uint32_t thread_ref, const char *thread_id,
                            bool use_active_thread, const uint8_t idem[16]);
bool cbdp_build_audio_chunk(uint8_t *buffer, size_t capacity, size_t *length,
                            uint32_t tx_id, uint32_t offset,
                            const uint8_t *payload, size_t payload_length);
bool cbdp_build_audio_end(uint8_t *buffer, size_t capacity, size_t *length,
                          uint32_t tx_id, uint32_t total_bytes);
bool cbdp_build_approval_resolve(uint8_t *buffer, size_t capacity, size_t *length,
                                 uint32_t request_id, uint32_t approval_ref,
                                 uint8_t decision, const char *response,
                                 const uint8_t idem[16]);
bool cbdp_build_discovery_reply(uint8_t *buffer, size_t capacity, size_t *length,
                                const uint8_t nonce[16]);

bool cbdp_parse_welcome(const uint8_t *tlv, size_t length, cbdp_session_t *session);
bool cbdp_parse_auth_ok(const uint8_t *tlv, size_t length, cbdp_session_t *session);
bool cbdp_parse_capability_update(const uint8_t *tlv, size_t length, cbdp_session_t *session);
bool cbdp_parse_event_header(const uint8_t *tlv, size_t length, cbdp_event_header_t *event);
bool cbdp_fragment_begin(cbdp_fragment_assembly_t *assembly, const uint8_t *tlv,
                         size_t length, uint32_t payload_len, uint16_t frame_flags);
bool cbdp_fragment_accept(cbdp_fragment_assembly_t *assembly, const uint8_t *tlv,
                          size_t length, uint16_t frame_flags,
                          const uint8_t *payload, size_t payload_len,
                          uint8_t **complete_header, size_t *complete_header_len,
                          uint8_t **complete_payload, size_t *complete_payload_len);
void cbdp_fragment_reset(cbdp_fragment_assembly_t *assembly);

/* Compatibility helpers still used by the local USB JSON path. */
bool codex_bridge_protocol_has_capability(const char *capability);

const char *codex_bridge_capability_name(uint8_t bit);

void codex_read_tracker_reset(void);
void codex_read_tracker_visit(const char *message_id, const char *result_message_id,
                              unsigned now_ms);
bool codex_read_tracker_take_due(const char *message_id, const char *result_message_id,
                                 unsigned now_ms);
void codex_read_tracker_rearm(const char *message_id, const char *result_message_id,
                              unsigned now_ms);

#ifdef __cplusplus
}
#endif
