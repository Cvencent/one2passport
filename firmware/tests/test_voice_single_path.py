from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
FLOW = (ROOT / "main" / "coding_flow.c").read_text(encoding="utf-8")
BRIDGE = (ROOT / "main" / "coding_bridge.c").read_text(encoding="utf-8")
XZ_H = (ROOT / "main" / "xiaozhi_client.h").read_text(encoding="utf-8")
XZ = (ROOT / "main" / "xiaozhi_client.c").read_text(encoding="utf-8")
RADIO = (ROOT / "main" / "radio_player.cc").read_text(encoding="utf-8")


def function_body(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:pos + 1]
    raise AssertionError("unterminated function: " + signature)


class VoiceSinglePathTest(unittest.TestCase):
    """One recording = exactly one uplink path (CBDP v1 §19.4).

    Bridge forensics (2026-10-25): the device uploaded one recording twice -
    AUDIO_END (cbdp-audio:350674, Bridge STT auto-submits the turn) and then
    SEND_END (cbdp-tx:350675, the device re-submitted the transcript echo
    after its "confirm text" review screen). Codex correctly treated them as
    two user messages.
    """

    def test_transcript_goes_straight_to_submitted(self):
        body = function_body(FLOW, "bool coding_flow_set_transcript")
        self.assertNotIn("CODING_STAGE_VOICE_REVIEW", body,
                         "the transcript is an echo: no review-then-SEND step")
        self.assertIn("CODING_STAGE_QUEUED", body,
                      "the bridge already submitted the turn at AUDIO_END")

    def test_confirm_cannot_send(self):
        body = function_body(FLOW, "coding_action_t coding_flow_confirm_voice")
        self.assertNotIn("CODING_ACTION_SEND_VOICE", body,
                         "confirm must never trigger a SEND_* re-submission")

    def test_send_voice_refuses_after_audio_submission(self):
        body = function_body(BRIDGE, "bool coding_bridge_send_voice")
        self.assertIn("s_voice_turn_submitted", body,
                      "the recording-submitted mutex must gate the text path")

    def test_audio_end_sets_and_begin_clears_the_mutex(self):
        end_body = function_body(BRIDGE, "static bool send_audio_end")
        self.assertIn("s_voice_turn_submitted = true", end_body,
                      "a successful AUDIO_END marks the recording submitted")
        self.assertNotIn("next_tx_id", end_body,
                         "an AUDIO_END (re)send must reuse the same tx_id - "
                         "a new tx_id is always a NEW user input")
        begin_body = function_body(BRIDGE, "static bool send_audio_begin")
        self.assertIn("s_voice_turn_submitted = false", begin_body,
                      "a new recording period clears the mutex")

    def test_tail_arena_leaves_room_for_the_jitter_ring(self):
        # 2026-10-25 user report: with the arena taking the WHOLE 9.2KB tail
        # the ring shrank to 11 blocks and sources starved into auto-skips
        # ('莫名切台' + segmented playback). The arena is capped so the ring
        # keeps a slice of the tail.
        import re as _re
        m = _re.search(r"XZ_MIC_TAIL_ARENA_CAP (\d+)UL", XZ_H)
        self.assertIsNotNone(m, "the arena cap must be an explicit constant")
        cap = int(m.group(1))
        self.assertGreaterEqual(cap, 4500,
                                "the arena must still host the 4224B decoder "
                                "sub-block (the biggest heap consumer)")
        self.assertLessEqual(cap, 7000,
                             "the ring must keep >=2.2KB of the tail; a full-"
                             "tail arena shrank the ring to 11 blocks and "
                             "caused the starvation skips")
        idle = function_body(XZ, "uint8_t *xiaozhi_idle_buffer")
        self.assertIn("XZ_MIC_TAIL_ARENA_CAP", idle,
                      "the ring borrow must start after the arena cap")
        calloc_body = function_body(RADIO, "void *media_lib_module_calloc")
        self.assertIn("XZ_MIC_TAIL_ARENA_CAP", calloc_body,
                      "the bump allocator must be bounded by the cap")

    def test_rebuffer_storm_abandons_the_session(self):
        # 2026-10-25 device log: a trickle source produced 2-3 rebuffers per
        # 10s window; the session dragged 30-60s before the starvation skip.
        # Three rebuffers in one session = broken source: abandon immediately
        # and let the starve-skip advance the dial.
        decode = function_body(RADIO, "bool stream_station(std::size_t station, uint32_t generation)")
        self.assertIn("session starved", decode,
                      "2 rebuffers in one 10s stats window must abandon the session")
        self.assertIn("rebuffers >= 2", decode,
                      "the trickle signature is exactly 2 rebuffers per 10s "
                      "window (stats reset bounds the counter)")
        self.assertIn("s_last_prebuffer_starved.store(true", decode)

    def test_audio_end_reuses_the_begin_tx_id(self):
        begin_body = function_body(BRIDGE, "static bool send_audio_begin")
        end_body = function_body(BRIDGE, "static bool send_audio_end")
        self.assertIn("s_wireless_tx_id = next_tx_id()", begin_body,
                      "AUDIO_BEGIN owns the transaction tx_id")
        self.assertIn("s_wireless_tx_id,", end_body,
                      "AUDIO_END must reuse AUDIO_BEGIN's tx_id")


if __name__ == "__main__":
    unittest.main()
