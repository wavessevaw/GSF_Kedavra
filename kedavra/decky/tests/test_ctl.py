# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "py_modules"))
import kedavra_ctl  # noqa: E402

KEDAVRA = HERE.parent.parent


class Recorder:
    def __init__(self):
        self.calls = []

    def __call__(self, argv, **kw):
        self.calls.append(argv)
        self.envs = getattr(self, "envs", []) + [kw.get("env")]
        if argv[0].endswith("python3") or argv[0] == sys.executable:
            return subprocess.run(argv, **kw)
        return subprocess.CompletedProcess(argv, 0, "", "")


class ControlTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.now = 1000.0
        self.rec = Recorder()
        plugin = self.tmp / "plugin"
        (plugin / "bin").mkdir(parents=True)
        shutil.copy(KEDAVRA / "tools" / "kfg_pacing.py", plugin / "bin")
        self.ctl = kedavra_ctl.Control(self.tmp / "home", plugin, runner=self.rec, clock=lambda: self.now)
        self.ctl.runtime = self.tmp / "run"
        self.ctl.runtime.mkdir()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_absent_engine_is_not_reported_running(self):
        s = self.ctl.status()
        self.assertEqual(s["engine"], "absent")
        self.assertFalse(s["fresh"])

    def test_stale_status_is_not_running(self):
        p = self.ctl.runtime / "kedavra-status.json"
        p.write_text(json.dumps({"multiplier": 3, "state": "active"}))
        os.utime(p, (self.now - 10, self.now - 10))
        s = self.ctl.status()
        self.assertEqual(s["engine"], "stale")
        os.utime(p, (self.now - 1, self.now - 1))
        s = self.ctl.status()
        self.assertEqual(s["engine"], "running")
        self.assertEqual(s["multiplier"], 3)

    def test_gamescopectl_does_not_inherit_decky_libraries(self):
        os.environ["LD_LIBRARY_PATH"] = "/tmp/_MEI1234"
        try:
            self.ctl.set_multiplier(2)
        except Exception:
            pass
        finally:
            del os.environ["LD_LIBRARY_PATH"]
        env = self.rec.envs[0]
        self.assertIsNotNone(env)
        self.assertNotIn("LD_LIBRARY_PATH", env)

    def test_multiplier_goes_through_gamescopectl(self):
        self.ctl.set_multiplier(3)
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_multiplier", "3"])
        with self.assertRaises(ValueError):
            self.ctl.set_multiplier(4)

    def test_base_fps_goes_through_gamescopectl(self):
        self.ctl.set_base_fps(40)
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_base_fps", "40"])
        self.ctl.set_base_fps(0)
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_base_fps", "0"])
        with self.assertRaises(ValueError):
            self.ctl.set_base_fps(10)

    def test_base_fps_refused_by_old_engine(self):
        (self.ctl.runtime / "kedavra-status.json").write_text(json.dumps({"engine": "kedavra-gamescope", "auto": False}))
        with self.assertRaises(RuntimeError):
            self.ctl.set_base_fps(30)

    def test_capture_runs_analyzer(self):
        c = self.ctl.start_capture("x3 test")
        self.assertEqual(self.rec.calls[-1][-2], "kfg_trace")
        # Fake a perfectly paced x3 trace at 90 Hz as the engine would write it.
        period = 11111111
        rows = ["vblank,flip_ns,kind,src_a,src_b,phase_num,phase_den,content,delay,reason,state,refresh_ns,mult,compose_late,skipped"]
        for k in range(90 * 20):
            src = k // 3
            ph = k % 3
            kind = "real" if ph == 0 else "gen"
            rows.append(f"{k},{k * period},{kind},{src},{src + 1 if ph else 0},{ph},{3 if ph else 0},{k},3,none,active,{period},3,0,0")
        Path(c["path"]).write_text("\n".join(rows) + "\n")
        # The engine still reports the file open for a moment after the stop.
        status = self.ctl.runtime / "kedavra-status.json"
        status.write_text(json.dumps({"engine": "kedavra-gamescope", "trace": c["path"]}))
        naps = []

        def nap(s):
            naps.append(s)
            status.write_text(json.dumps({"engine": "kedavra-gamescope", "trace": ""}))
        self.ctl.sleep = nap
        r = self.ctl.stop_capture()
        self.assertEqual(len(naps), 1)
        self.assertEqual(self.rec.calls[-2][-2:], ["kfg_trace", ""])
        self.assertIn("summary", r, r["log"])
        self.assertLess(r["summary"]["p95_ms"], 0.01)
        self.assertIsNone(self.ctl.capture)

    def test_options_hdr_and_floor(self):
        status = self.ctl.runtime / "kedavra-status.json"
        status.write_text(json.dumps({"engine": "kedavra-gamescope", "req_auto_floor": 0, "hdr_enabled": True}))
        self.assertFalse(self.ctl.options_differ(json.loads(status.read_text()), {"hdr": True}))
        self.assertTrue(self.ctl.options_differ(json.loads(status.read_text()), {"hdr": False}))
        # A floor left by the old x3.75 option is put back to refresh / 3.
        self.assertTrue(self.ctl.options_differ({"req_auto_floor": 20, "hdr_enabled": True}, {"hdr": True}))
        self.ctl.apply_options({"hdr": False})
        self.assertEqual(self.rec.calls[-2][-2:], ["kfg_auto_floor", "0"])
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_hdr", "0"])

    def test_options_refused_by_old_engine(self):
        (self.ctl.runtime / "kedavra-status.json").write_text(json.dumps({"engine": "kedavra-gamescope", "auto": True}))
        self.assertFalse(self.ctl.options_differ({"auto": True}, {"hdr": True}))
        with self.assertRaises(RuntimeError):
            self.ctl.apply_options({"hdr": True})

    def _engine(self, **kw):
        st = {"engine": "kedavra-gamescope", "auto": False, "base_fps": 0, "req_auto_floor": 0, **kw}
        (self.ctl.runtime / "kedavra-status.json").write_text(json.dumps(st))

    def _convars(self):
        return [c[-2:] for c in self.rec.calls]

    def test_mode_x2_is_strict_x2_without_a_base(self):
        self._engine(strict=1)
        self.ctl.set_mode("x2")
        self.assertEqual(self._convars(), [["kfg_base_fps", "0"], ["kfg_strict", "1"], ["kfg_multiplier", "2"]])

    def test_mode_auto_is_x2_to_x3(self):
        self._engine(strict=1)
        self.ctl.set_mode("auto")
        self.assertEqual(self._convars(), [["kfg_base_fps", "0"], ["kfg_strict", "1"], ["kfg_auto_floor", "0"], ["kfg_multiplier", "1"]])

    def test_mode_without_strict_engine_and_off(self):
        self._engine()
        self.ctl.set_mode("x2")
        self.assertNotIn(["kfg_strict", "1"], self._convars())
        self.ctl.set_mode("off")
        self.assertEqual(self._convars()[-1], ["kfg_multiplier", "0"])
        for gone in ("cap", "x375", "x3"):
            with self.assertRaises(ValueError):
                self.ctl.set_mode(gone)

    def test_legacy_engine_modes_move_to_x2(self):
        legacy = kedavra_ctl.Control.legacy_mode
        self.assertTrue(legacy({"fresh": True, "req_multiplier": 1, "req_base_fps": 40}))
        self.assertTrue(legacy({"fresh": True, "req_multiplier": 3, "req_base_fps": 0}))
        self.assertFalse(legacy({"fresh": True, "req_multiplier": 1, "req_base_fps": 0}))
        self.assertFalse(legacy({"fresh": True, "req_multiplier": 2, "req_base_fps": 0}))
        self.assertFalse(legacy({"fresh": True, "req_multiplier": 0, "req_base_fps": 40}))
        self.assertFalse(legacy({"fresh": False, "req_multiplier": 1, "req_base_fps": 40}))
        self.assertFalse(legacy({"fresh": True}))


class MigrateTest(unittest.TestCase):
    def test_custom_becomes_x2_and_old_options_go(self):
        s, changed = kedavra_ctl.migrate_settings({"hud": True, "base_fps": 40, "x375": True, "hdr": False, "mode": "cap"})
        self.assertTrue(changed)
        self.assertEqual(s, {"hud": True, "hdr": False, "mode": "x2"})

    def test_current_settings_unchanged(self):
        for mode in ("off", "x2", "auto"):
            s, changed = kedavra_ctl.migrate_settings({"hud": False, "hdr": True, "mode": mode})
            self.assertFalse(changed)
            self.assertEqual(s["mode"], mode)

    def test_unknown_mode_is_off(self):
        s, changed = kedavra_ctl.migrate_settings({"mode": "x375"})
        self.assertTrue(changed)
        self.assertEqual(s["mode"], "off")

if __name__ == "__main__":
    unittest.main()
