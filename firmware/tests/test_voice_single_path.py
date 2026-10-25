from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
FLOW = (ROOT / "main" / "coding_flow.c").read_text(encoding="utf-8")
BRIDGE = (ROOT / "main" / "coding_bridge.c").read_text(encoding="utf-8")


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

    def test_audio_end_reuses_the_begin_tx_id(self):
        begin_body = function_body(BRIDGE, "static bool send_audio_begin")
        end_body = function_body(BRIDGE, "static bool send_audio_end")
        self.assertIn("s_wireless_tx_id = next_tx_id()", begin_body,
                      "AUDIO_BEGIN owns the transaction tx_id")
        self.assertIn("s_wireless_tx_id,", end_body,
                      "AUDIO_END must reuse AUDIO_BEGIN's tx_id")


if __name__ == "__main__":
    unittest.main()
