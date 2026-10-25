from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
REG = (ROOT / "main" / "app_registry.cc").read_text(encoding="utf-8")
RADIO = (ROOT / "main" / "radio_player.cc").read_text(encoding="utf-8")
XZ = (ROOT / "main" / "xiaozhi_client.c").read_text(encoding="utf-8")
XZ_H = (ROOT / "main" / "xiaozhi_client.h").read_text(encoding="utf-8")


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


class HomeMiniCodexTest(unittest.TestCase):
    """Home = mini Codex (counters/alerts) + mini radio, simultaneously.

    User requirement (2026-10-25): the home layer keeps the Codex connection
    (state counters + completion alerts via UDP) while the minimized radio
    plays. The bridge task stack occupies the FRONT 12KB of the mic static
    array; the decoder arena borrows only the TAIL (9.2KB) - they do not
    overlap, so the bridge stays resident and the decoder sub-blocks leave
    the heap (~8.8KB), keeping the WiFi RX supply above the stable line.
    """

    def test_bridge_restarted_when_resuming_playback(self):
        # 2026-10-25 regression: entering xiaozhi stops the bridge; the resume
        # path forgot to start it again, so home counters/alerts died until
        # the next full app cycle. The playing branch must restart it.
        body = function_body(REG, "void app_framework_minimize_services")
        playing = body.find("s_minimized_radio_should_play")
        ret = body.find("return;", playing)
        self.assertGreater(ret, -1)
        window = body[playing:ret]
        self.assertNotIn("coding_bridge_stop_wireless()", window,
                         "the bridge must stay resident on home: counters and "
                         "alerts must keep working while the radio plays")
        self.assertIn("coding_bridge_wireless_started()", window)
        self.assertIn("coding_bridge_start_wireless()", window,
                      "the resume branch must restart the bridge stopped by "
                      "the xiaozhi/codex enter paths")

    def test_restore_ordered_restarts_bridge_too(self):
        body = function_body(REG, "void app_framework_restore_minimized_services_ordered")
        playing = body.find("s_minimized_radio_should_play")
        ret = body.find("return;", playing)
        window = body[playing:ret]
        self.assertNotIn("coding_bridge_stop_wireless()", window)
        self.assertIn("coding_bridge_start_wireless()", window)

    def test_tail_arena_only_borrows_the_idle_tail(self):
        claim = function_body(XZ, "uint8_t *xiaozhi_radio_tail_claim")
        self.assertIn("xz_mic_stack_is_idle()", claim,
                      "the tail must only be borrowed while XiaoZhi is fully "
                      "closed (the mic task stack covers the whole array)")
        self.assertIn("XZ_BRIDGE_STACK_BYTES + XZ_RADIO_STACK_BYTES", claim,
                      "the tail starts AFTER the bridge carve, so the resident "
                      "bridge never overlaps the decoder blocks")
        idle = function_body(XZ, "uint8_t *xiaozhi_idle_buffer")
        self.assertIn("s_radio_tail_held", idle,
                      "the jitter ring must yield the tail while the arena "
                      "holds it")

    def test_listening_refused_while_tail_held(self):
        body = function_body(XZ, "void xiaozhi_client_start_listening")
        self.assertIn("s_radio_tail_held", body,
                      "the mic task stack covers the whole mic array; "
                      "recording must refuse while the tail is still borrowed")

    def test_pool_release_returns_the_tail(self):
        body = function_body(RADIO, "void radio_player_release_pool")
        self.assertIn("xiaozhi_radio_tail_release()", body,
                      "entering xiaozhi releases the pool: the tail must go "
                      "back so the mic task can use the full array")

    def test_media_lib_override_serves_tail_backed_bump(self):
        body = function_body(RADIO, "void *media_lib_module_calloc")
        self.assertIn("kArenaMinBlock", body)
        self.assertIn("s_tail_arena_bump", body)
        free_body = function_body(RADIO, "void media_lib_free")
        self.assertIn("s_tail_arena_base", free_body,
                      "media_lib_free must recognise arena pointers")

    def test_connection_gate_sits_between_eai_memory_and_stable_rx(self):
        match = re.search(r"kConnectionMinBlock = (\d+)", RADIO)
        self.assertIsNotNone(match, "kConnectionMinBlock must stay explicit")
        value = int(match.group(1))
        self.assertGreaterEqual(value, 2500,
                                "below 2.5KB the connection cannot even start "
                                "(measured EAI_MEMORY at 1.2KB)")
        self.assertLessEqual(value, 3200,
                             "the gate must not block the legal post-decoder "
                             "home-playback floor")


if __name__ == "__main__":
    unittest.main()
