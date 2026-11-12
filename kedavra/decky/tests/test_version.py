"""The installed panel must identify the same release as its package."""
import json
import re
import unittest
from pathlib import Path

class VersionTest(unittest.TestCase):
    def test_ui_and_package_version_match(self):
        root = Path(__file__).resolve().parents[1]
        version = json.loads((root / 'package.json').read_text())['version']
        source = (root / 'src/index.tsx').read_text()
        self.assertEqual(re.search(r"const VERSION = '([^']+)'", source).group(1), version)
        lock = json.loads((root / 'package-lock.json').read_text())
        self.assertEqual(lock['version'], version)
        self.assertEqual(lock['packages']['']['version'], version)
