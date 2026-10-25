from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
APPS = (ROOT / "main" / "app_apps.cc").read_text(encoding="utf-8")
MAIN = (ROOT / "main" / "main.cc").read_text(encoding="utf-8")
UI = (ROOT / "main" / "radio_ui.cc").read_text(encoding="utf-8")
UI_H = (ROOT / "main" / "radio_ui.h").read_text(encoding="utf-8")


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


class CityLoadingTest(unittest.TestCase):
    """选城市(含自动定位)必须进入加载态, 完成前禁止返回。

    用户实测 (2026-10-25): 点自动定位立即回菜单, 快速进收音机时城市目录
    还没拿到, 表现为"拿不到地方的台"。修复: 选中后进入加载页, 按键全部
    忽略, 目录任务(app_radio_catalog_busy)完成后自动返回设置菜单。
    """

    def test_city_selection_enters_busy_state(self):
        body = function_body(APPS, "void settings_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {")
        city = body.find("RadioSettingsPage::City")
        window = body[city:city + 1200]
        self.assertIn("app_radio_request_city", window)
        self.assertIn("app_radio_catalog_busy()", window,
                      "the selection must check whether the catalog task is "
                      "actually running")
        self.assertIn("s_city_busy = true", window)
        self.assertIn("radio_ui_show_city_loading", window,
                      "the loading page must be shown during the search")

    def test_busy_state_falls_back_when_task_never_started(self):
        body = function_body(APPS, "void settings_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {")
        city = body.find("RadioSettingsPage::City")
        window = body[city:city + 1700]
        self.assertIn("show(RadioSettingsPage::Menu, 1)", window,
                      "when the catalog task failed to start, the old "
                      "return-to-menu flow must survive")

    def test_keys_swallowed_while_busy(self):
        back = function_body(APPS, "bool settings_app_back(void) {")
        self.assertIn("s_city_busy) return true", back,
                      "back must be ignored while locating/searching")
        key = function_body(APPS, "void settings_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {")
        self.assertIn("if (s_city_busy) return;", key,
                      "all keys must be ignored while locating/searching")

    def test_completion_returns_to_menu(self):
        tick = function_body(APPS, "void city_busy_tick(lv_timer_t *timer) {")
        self.assertIn("app_radio_catalog_busy()", tick)
        self.assertIn("lv_timer_del(timer)", tick,
                      "the poll timer must clean itself up")
        self.assertIn("show(RadioSettingsPage::Menu, 1)", tick,
                      "completion returns to the settings menu")

    def test_busy_query_reads_the_catalog_task(self):
        body = function_body(MAIN, "bool app_radio_catalog_busy(void) {")
        self.assertIn("s_catalog_task", body,
                      "the busy state must track the real catalog worker")

    def test_loading_screen_exists(self):
        self.assertIn("void radio_ui_show_city_loading", UI_H)
        body = function_body(UI, "void radio_ui_show_city_loading")
        self.assertIn("正在定位城市", body,
                      "the auto-locate loading text must be visible")
        self.assertIn("正在搜索", body,
                      "the named-city search text must be visible")


if __name__ == "__main__":
    unittest.main()
