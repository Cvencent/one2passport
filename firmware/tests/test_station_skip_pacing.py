from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main" / "radio_player.cc").read_text(encoding="utf-8")


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


class StationSkipPacingTest(unittest.TestCase):
    """Failed dials must cool down before advancing the dial.

    Measured on COM4 (2026-10-25): after one playback RST the worker dialed
    ten stations within ~400ms (20-40ms apart). The CDN answered the rapid
    redials with HTTP 307 rate-limit responses, so even the KNOWN-GOOD
    stations (all 14 URLs verified 200/audio-mpeg from a PC minutes later)
    were refused - the "拒联" storm was self-inflicted pacing, not dead
    stations.
    """

    def setUp(self):
        self.body = function_body(SOURCE, "void player_task(void *)")

    def test_skip_branch_waits_before_next_dial(self):
        skip = self.body.find("unreachable, skipping to the next one")
        self.assertGreater(skip, -1)
        window = self.body[skip:skip + 800]
        self.assertIn("interruptible_delay_ms(skip_cooldown_ms", window,
                      "advancing the dial must cool down first, otherwise the "
                      "rapid redials trip the CDN rate limiter (307 storm)")
        self.assertIn("skip_cooldown_ms +=", window,
                      "repeated skips must escalate the cooldown")

    def test_cooldown_resets_after_a_successful_stream(self):
        played = self.body.find("if (played) {")
        window = self.body[played:played + 500]
        self.assertIn("skip_cooldown_ms = 1500", window,
                      "a stream that played proves the CDN is fine again; "
                      "the cooldown must reset")

    def test_cooldown_is_bounded(self):
        self.assertIn("skip_cooldown_ms < 4500", self.body,
                      "the cooldown must stay short enough that a full table "
                      "sweep still finishes in seconds")

    def test_starved_station_skips_after_two_sessions(self):
        # 2026-10-25 (COM4): a trickle-feeding source decodes 26ms of audio,
        # so the old played_audio check kept reconnecting to the same silent
        # station forever. Two consecutive starved sessions must skip.
        played = self.body.find("if (played) {")
        window = self.body[played:played + 1600]
        self.assertIn("s_last_prebuffer_starved.exchange", window,
                      "the played branch must consult the starvation marker")
        self.assertIn("starved_sessions >= 2", window,
                      "two consecutive starved sessions = broken source")
        self.assertIn("radio_player_select_relative(1)", window,
                      "a starved station must be skipped like a dead one")
        self.assertIn("starved_sessions = 0;", window,
                      "a healthy session must reset the starvation counter")


if __name__ == "__main__":
    unittest.main()
