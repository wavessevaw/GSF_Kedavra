# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed-strength Focus migration and compatibility with older Decky clients."""
import asyncio
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py_modules"))
spec = importlib.util.spec_from_file_location("focus_plugin", ROOT / "main.py")
plugin = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {"decky": types.SimpleNamespace(logger=Mock())}):
    spec.loader.exec_module(plugin)


class FocusPluginTest(unittest.IsolatedAsyncioTestCase):
    async def test_old_saved_strength_migrates_without_losing_other_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = plugin.Plugin()
            p.settings_path = Path(tmp) / "settings.json"
            p.settings_path.write_text(json.dumps({"detail_strength": 0.25, "base_fps": 40, "hud": False}))
            settings = p._load_settings()
            self.assertEqual(settings["detail_strength"], 1.0)
            self.assertEqual(settings["base_fps"], 40)
            self.assertFalse(settings["hud"])

    async def test_legacy_client_strength_is_fixed_and_off_reaches_controller(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = plugin.Plugin()
            p.out = Path(tmp)
            p.settings_path = p.out / "settings.json"
            p.settings = dict(p.DEFAULTS)
            p.lock = asyncio.Lock()
            p.ctl = Mock()
            for enabled in (True, False):
                p.ctl.set_detail.return_value = {"detail": enabled, "detail_strength": 1.0}
                result = await p.set_detail(enabled, 0.25)
                p.ctl.set_detail.assert_called_with(enabled, 1.0)
                self.assertEqual(result["detail"], enabled)
                self.assertEqual(json.loads(p.settings_path.read_text())["detail_strength"], 1.0)
