from pathlib import Path
import unittest

SOURCE = Path(__file__).parents[2] / 'leo-codex' / 'main' / 'app_coding.c'

class PagingActionLayoutTests(unittest.TestCase):
    def test_running_page_reapplies_actions_after_promoting_paging(self):
        source = SOURCE.read_text(encoding='utf-8')
        branch = source[source.index('} else if ((s_flow.stage == CODING_STAGE_RUNNING'):]
        branch = branch[:branch.index('\n    const char *title', branch.index('a3 = NULL;'))]
        self.assertIn('a0 = s_page + 1U >= estimated_pages ? "回第一页" : "下一页";', branch)
        self.assertIn('set_actions(a0, a1, a2, a3);', branch)
        self.assertLess(branch.index('a0 = s_page'), branch.index('set_actions(a0, a1, a2, a3);'))

if __name__ == '__main__':
    unittest.main()
