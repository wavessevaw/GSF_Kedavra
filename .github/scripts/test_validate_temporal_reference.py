import json
import pathlib
import tempfile
import unittest

from validate_temporal_reference import METRICS, SCENES, validate


class TemporalReferenceValidation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        for scene, modes in SCENES.items():
            rows = [{"scene": scene, "mode": mode, **{k: 1.0 for k in METRICS}} for mode in modes]
            (self.root / f"{scene}.json").write_text(json.dumps({"device": "llvmpipe", "size": [1280, 800], "hard": rows}))

    def alter(self, scene, callback):
        path = self.root / f"{scene}.json"
        value = json.loads(path.read_text())
        callback(value)
        path.write_text(json.dumps(value))

    def test_valid_data_remains_unverified(self):
        self.assertEqual(validate(self.root)["quality_status"], "UNVERIFIED")

    def test_missing_mode_rejected(self):
        self.alter("h_lines_slow", lambda d: d["hard"].pop())
        with self.assertRaises(ValueError):
            validate(self.root)

    def test_nonfinite_metric_rejected(self):
        self.alter("h_turn64", lambda d: d["hard"][0].update(temporal=float("nan")))
        with self.assertRaises(ValueError):
            validate(self.root)

    def test_wrong_device_rejected(self):
        self.alter("h_parallax", lambda d: d.update(device="GPU"))
        with self.assertRaises(ValueError):
            validate(self.root)
