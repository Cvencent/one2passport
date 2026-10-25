from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BRIDGE = (ROOT / "main" / "coding_bridge.c").read_text(encoding="utf-8")
UI = (ROOT / "main" / "app_coding.c").read_text(encoding="utf-8")
GATE = (ROOT / "main" / "codex_alert.h").read_text(encoding="utf-8")


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


class CodexAlertDedupTest(unittest.TestCase):
    """One finished Codex task must produce exactly one sound.

    Real hardware symptom (2026-10-25): completing a single task played the
    alert two or three times, because the terminal turn event, the aggregate
    done/failed counters (which only ring after their settle window) and the
    foreground UI update each rang on their own.

    The approval prompt is a different event and keeps its own tone, so it is
    not part of this contract.
    """

    def test_gate_exists_and_is_time_bounded(self):
        self.assertIn("codex_alert_claim", GATE)
        self.assertIn("CODEX_ALERT_DEDUP_US", GATE)

    def test_bridge_channels_route_through_the_gate(self):
        counts = function_body(BRIDGE, "static void apply_codex_counts(")
        self.assertIn("codex_alert_play(", counts,
                      "the aggregate-counter channel must use the gate")
        self.assertNotIn("ui_sfx_play(", counts,
                         "the aggregate-counter channel must not ring directly")
        self.assertIn("codex_alert_play(failed)", BRIDGE,
                      "the terminal turn-event channel must use the gate")

    def test_ui_completion_paths_route_through_the_gate(self):
        for signature in ("void app_coding_bridge_done_with_elapsed(",
                          "void app_coding_bridge_error_with_elapsed(",
                          "void app_coding_remote_result("):
            body = function_body(UI, signature)
            self.assertIn("codex_alert_play(", body,
                          "%s must ring through the gate" % signature)
            self.assertNotIn("ui_sfx_play(", body,
                             "%s must not ring directly" % signature)

    def test_gate_actually_deduplicates(self):
        self.assertIn("claimed_us", GATE)
        self.assertRegex(
            GATE,
            r"now_us - gate->claimed_us < CODEX_ALERT_DEDUP_US",
            "the gate must reject a second claim inside the dedup window")

    def test_bridge_uses_the_tested_gate(self):
        self.assertIn("codex_alert_claim(&s_alert_gate", BRIDGE,
                      "codex_alert_play must consult the tested gate")


if __name__ == "__main__":
    unittest.main()
