from pathlib import Path
import re
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


class PrebufferDeadlineTest(unittest.TestCase):
    """The prebuffer wait must be bounded.

    Root cause (2026-10-25, COM4): a station whose CDN trickles data can keep
    the ring below the resume threshold forever without ever tripping the 20s
    "network feed idle" timer, because that timer resets on every byte read.
    The stream loop then stayed in "正在缓冲" indefinitely. The wait must have
    a deadline after which we either start with what we have or abandon the
    attempt (and let the outer loop switch stations).
    """

    def test_stream_loop_uses_a_prebuffer_deadline(self):
        body = function_body(SOURCE, "bool stream_station(")
        self.assertIn("kPrebufferDeadlineMs", SOURCE,
                      "a bounded prebuffer deadline constant must exist")
        self.assertRegex(
            body, r"waited_ms|prebuffer_wait",
            "the buffering wait must track how long it has been waiting")
        self.assertRegex(
            body, r"kPrebufferDeadlineMs|deadline",
            "the buffering wait must consult the deadline, otherwise a slow "
            "source leaves the UI stuck on '正在缓冲' forever")

    def test_deadline_and_floor_are_sane(self):
        deadline = re.search(r"kPrebufferDeadlineMs\s*=\s*([0-9]+)", SOURCE)
        floor = re.search(r"kPrebufferMinStartBytes\s*=\s*([0-9]+)", SOURCE)
        self.assertIsNotNone(deadline, "kPrebufferDeadlineMs missing")
        self.assertIsNotNone(floor, "kPrebufferMinStartBytes missing")
        self.assertGreaterEqual(int(deadline.group(1)), 5000,
                                "deadline must give a slow CDN a fair chance")
        self.assertLessEqual(int(deadline.group(1)), 60000,
                             "deadline must be bounded, not effectively infinite")
        self.assertGreater(int(floor.group(1)), 0)


if __name__ == "__main__":
    unittest.main()
