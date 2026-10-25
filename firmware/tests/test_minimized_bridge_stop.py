from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
REG = (ROOT / "main" / "app_registry.cc").read_text(encoding="utf-8")
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


class MinimizedRadioBridgeStopTest(unittest.TestCase):
    """Plan A: the Codex bridge must stop while the minimized radio plays.

    Measured on COM4 (2026-10-25): home playback with the resident bridge left
    the dma floor at 652-936B while each WiFi RX buffer needs ~1600B contiguous
    DMA - the receive allocation failed, the server backed off and reset the
    stream after ~15s ("one stutter per second" home playback). Stopping the
    bridge returns ~2.4KB and puts the floor above the measured 2.5KB stable
    line.
    """

    def test_playing_branch_stops_bridge_and_skips_start(self):
        body = function_body(REG, "void app_framework_minimize_services")
        playing = body.find("s_minimized_radio_should_play")
        stop = body.find("coding_bridge_stop_wireless()", playing)
        ret = body.find("return;", playing)
        start = body.find("coding_bridge_start_wireless()", playing)
        self.assertGreater(stop, -1,
                           "a still-running bridge must be stopped when the "
                           "minimized radio takes the heap")
        self.assertGreater(ret, stop)
        self.assertGreater(start, ret,
                           "minimize_services must return before starting the "
                           "bridge while the minimized radio plays")

    def test_restore_ordered_matches_policy(self):
        body = function_body(REG, "void app_framework_restore_minimized_services_ordered")
        playing = body.find("s_minimized_radio_should_play")
        stop = body.find("coding_bridge_stop_wireless()", playing)
        ret = body.find("return;", playing)
        start = body.find("coding_bridge_start_wireless()", playing)
        self.assertGreater(stop, -1)
        self.assertGreater(ret, stop)
        self.assertGreater(start, ret)

    def test_idle_home_still_restores_the_bridge(self):
        body = function_body(REG, "void app_framework_minimize_services")
        playing = body.find("s_minimized_radio_should_play")
        ret = body.find("return;", playing)
        tail = body[ret:]
        self.assertIn("coding_bridge_start_wireless()", tail,
                      "when the minimized radio is NOT playing, the bridge "
                      "must come back for discovery/pairing")

    def test_connection_gate_sits_between_eai_memory_and_stable_rx(self):
        match = re.search(r"kConnectionMinBlock = (\d+)", RADIO)
        self.assertIsNotNone(match, "kConnectionMinBlock must stay explicit")
        value = int(match.group(1))
        # 1.2KB: measured getaddrinfo()=202 (EAI_MEMORY) -> must hold.
        # 2.5KB: measured stable WiFi RX supply (radio_player.cc header note).
        # 6KB: rejected - with Plan A the legal home-playback floor is ~3.1KB
        # and a 6KB gate would hold the dial forever (never plays).
        self.assertGreaterEqual(value, 2500,
                                "below 2.5KB the connection cannot even start "
                                "(EAI_MEMORY territory)")
        self.assertLessEqual(value, 3200,
                             "above ~3.2KB the gate blocks the legal "
                             "post-decoder home-playback floor (~3.1KB) and "
                             "the minimized radio never connects")


if __name__ == "__main__":
    unittest.main()
