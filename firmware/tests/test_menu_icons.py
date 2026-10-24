from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main" / "app_registry.cc").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
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
    raise AssertionError(f"unterminated function: {signature}")


class MenuIconContractTest(unittest.TestCase):
    def test_menu_icons_are_drawn_instead_of_using_font_glyphs(self):
        body = function_body(SOURCE, "static void menu_icon_symbol")
        legacy_glyphs = ('sym = ">_"', 'sym = "+"', 'sym = "*"')
        for glyph in legacy_glyphs:
            self.assertNotIn(glyph, body, f"menu icon still uses font glyph: {glyph}")
        self.assertNotIn("lv_label_create(parent)", body,
                         "menu icons must be built from LVGL primitives")


if __name__ == "__main__":
    unittest.main()
