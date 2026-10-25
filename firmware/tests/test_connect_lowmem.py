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


class ConnectLowMemoryTest(unittest.TestCase):
    """A starved heap must not be mistaken for a dead station.

    Root cause (2026-10-25, COM4): on the home/menu layer the heap legitimately
    drops to ~1.2KB free. Selecting the radio then failed to build a connection
    (getaddrinfo() 202 / esp_http_client_open failed), but the code counted that
    as "station unreachable" and advanced the dial. It walked the entire station
    table in milliseconds, flooding the log, saturating the CPU, and ending in
    an RTC_SW_CPU_RST reset. Connection failures explained by a starved heap
    must be reported as low-memory so the outer loop holds the same station and
    backs off instead of switching.
    """

    def setUp(self):
        self.body = function_body(SOURCE, "bool stream_station(")

    def open_failure_branch(self):
        marker = "esp_http_client_open(client, 0) != ESP_OK)"
        start = self.body.index(marker)
        return self.body[start:start + 700]

    def test_open_failure_discriminates_out_of_memory(self):
        branch = self.open_failure_branch()
        self.assertIn(
            "connect_out_of_memory()", branch,
            "the esp_http_client_open failure branch must consult "
            "connect_out_of_memory(); otherwise a starved heap is treated as a "
            "dead station and the dial advances through the whole table")
        self.assertIn(
            "s_last_fail_lowmem.store(true", branch,
            "a low-memory open failure must raise s_last_fail_lowmem so the "
            "outer loop keeps the current station and backs off")

    def test_http_client_init_failure_is_low_memory(self):
        marker = "esp_http_client_handle_t client = esp_http_client_init(&config);"
        start = self.body.index(marker)
        branch = self.body[start:start + 300]
        self.assertIn("s_last_fail_lowmem.store(true", branch,
                      "an http client allocation failure is a memory failure")

    def test_helper_uses_dedicated_threshold(self):
        self.assertIn("kConnectionMinBlock", SOURCE,
                      "a dedicated connection threshold must exist")
        self.assertIn("static bool connect_out_of_memory()", SOURCE,
                      "the out-of-memory predicate must exist")


if __name__ == "__main__":
    unittest.main()
