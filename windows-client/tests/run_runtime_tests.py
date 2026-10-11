"""Run the project suite against the runtime shipped in the Windows package."""

from pathlib import Path
import sys
import unittest

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root))
sys.path.insert(0, str(root / "runtime"))

import codex_backend
import windows_codex_backend
import mac_bridge

sys.modules["tools.codex_backend"] = codex_backend
sys.modules["tools.windows_codex_backend"] = windows_codex_backend
sys.modules["tools.mac_bridge"] = mac_bridge

suite = unittest.defaultTestLoader.discover(str(root / "tests"))
result = unittest.TextTestRunner(verbosity=1).run(suite)
raise SystemExit(not result.wasSuccessful())
