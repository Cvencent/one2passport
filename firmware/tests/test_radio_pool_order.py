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
        """Heap-sourced decoder blocks must still be claimed before pools.

        2026-10-25 update: with the mic-tail arena the decoder's big
        sub-blocks no longer come from the heap, so the arena path may
        allocate the pool buffers first. The legacy order must survive in
        the fallback branch (arena unavailable -> block from the heap).
        """
        body = function_body(SOURCE, "void radio_player_ensure_pool(void)")
        self.assertIn(
            "xiaozhi_radio_tail_available()", body,
            "ensure_pool must branch on tail-arena availability")
        arena_idx = body.find("xiaozhi_radio_tail_available()")
        first_pool = body.find("s_output_pool = static_cast", arena_idx)
        self.assertGreater(first_pool, -1, "output pool allocation missing")
        open_after_pool = body.find("open_mp3_decoder()", first_pool)
        self.assertGreater(
            open_after_pool, first_pool,
            "arena path may allocate pools first only because the decoder "
            "sub-blocks go to the static tail")
        warm_idx = body.find('warm_decoder_with_scratch("re-acquire")')
        self.assertGreater(warm_idx, -1,
                           "fallback branch must keep warm_decoder_with_scratch")
        pool_after_warm = body.find("s_output_pool = static_cast", warm_idx)
        self.assertGreater(
            pool_after_warm, -1,
            "fallback branch must still allocate pool buffers")
        self.assertLess(
            warm_idx, pool_after_warm,
            "when the tail is unavailable the decoder blocks come from the "
            "heap, so warmup must run before the pool mallocs (ESP_MP3_DEC "
            "ret 10 regression guard)")

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
