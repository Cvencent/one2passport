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


class RadioPoolOrderTest(unittest.TestCase):
    """Regression guard for 'buffer then insufficient contiguous heap'.

    Root cause (2026-10-25, COM4): radio_player_ensure_pool() opened the MP3
    decoder, malloc'd the output/input pool, and only then warmed the decoder.
    The two mallocs fragmented the only block big enough for the decoder's
    internal ~22.9KB calloc, so warmup failed with ESP_MP3_DEC ret 10 and the
    stream later reported 'not enough contiguous heap'. The decoder's big
    internal block must be claimed before any pool buffer is allocated.
    """

    def test_decoder_warmup_precedes_pool_buffer_allocation(self):
        body = function_body(SOURCE, "void radio_player_ensure_pool(void)")
        warm = body.find("warm_decoder_with_scratch")
        if warm < 0:
            self.fail("ensure_pool must warm the decoder through "
                      "warm_decoder_with_scratch while the heap is contiguous")
        first_pool = body.find("s_output_pool = static_cast")
        self.assertGreater(first_pool, -1, "output pool allocation missing")
        self.assertLess(
            warm, first_pool,
            "decoder warmup must run before s_output_pool/s_input_pool are "
            "malloc'd, otherwise the decoder's ~22.9KB internal block is "
            "fragmented and warmup fails with ESP_MP3_DEC ret 10")

    def test_decoder_min_block_matches_measured_requirement(self):
        match = re.search(r"kDecoderMinBlock\s*=\s*([0-9]+)\s*\*\s*1024", SOURCE)
        if not match:
            self.fail("kDecoderMinBlock must stay an explicit KiB constant")
        kib = int(match.group(1))
        self.assertGreaterEqual(
            kib, 22,
            "measured ESP_MP3_DEC internal requirement is 0x5978 (22904B); a "
            "smaller kDecoderMinBlock lets stream_station attempt a decode that "
            "cannot succeed")


if __name__ == "__main__":
    unittest.main()
