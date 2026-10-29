# SPDX-License-Identifier: GPL-3.0-or-later
import asyncio
import time
import json
from pathlib import Path

import decky
from kedavra_ctl import Control, migrate_settings
from kedavra_hud import Hud
from kedavra_install import Installer, bench


class Plugin:
    async def _main(self):
        here = Path(__file__).resolve().parent
        self.ctl = Control(decky.DECKY_USER_HOME, here)
        self.inst = Installer(here, decky.DECKY_USER_HOME)
        self.out = Path(decky.DECKY_USER_HOME) / ".local/share/kedavra"
        self.lock = asyncio.Lock()
        self.settings_path = self.out / "settings.json"
        self.settings = self._load_settings()
        self.options_at = 0.0
        self.hud = Hud()
        loop = asyncio.get_event_loop()
        self.health = loop.create_task(self._health())
        self.hud_task = loop.create_task(self._hud())

    DEFAULTS = {"hud": True, "hdr": True, "mode": "off"}

    def _load_settings(self):
        try:
            saved = json.loads(self.settings_path.read_text())
        except (OSError, ValueError):
            return dict(self.DEFAULTS)
        # GSF 1.5: Custom ("cap") becomes X2; the Custom base and x3.75 are gone.
        settings, changed = migrate_settings({**self.DEFAULTS, **saved})
        if changed:
            self.settings = settings
            self._save_settings()
        return settings

    def _save_settings(self):
        try:
            self.out.mkdir(parents=True, exist_ok=True)
            self.settings_path.write_text(json.dumps(self.settings))
        except OSError as e:
            decky.logger.warning(f"settings: {e}")

    async def _hud(self):
        # The on-screen HUD: rendered here once a second, drawn by the KEDAVRA Gamescope.
        while True:
            try:
                st = await asyncio.to_thread(self.ctl.status)
                await asyncio.to_thread(self.hud.tick, bool(self.settings.get("hud")), st)
                # A restarted Gamescope comes back with its defaults: restore the engine options.
                # (The status file is rewritten once a second, so wait a few before checking again.)
                if st.get("fresh") and time.monotonic() - self.options_at > 3 and self.ctl.options_differ(st, self.settings):
                    self.options_at = time.monotonic()
                    async with self.lock:
                        await asyncio.to_thread(self.ctl.apply_options, self.settings)
                # An engine still in a Custom cap (set by an older plugin) or fixed x3: X2.
                if self.ctl.legacy_mode(st) and time.monotonic() - self.options_at > 3:
                    self.options_at = time.monotonic()
                    async with self.lock:
                        await asyncio.to_thread(self.ctl.set_mode, "x2")
                        self.settings["mode"] = "x2"
                        await asyncio.to_thread(self._save_settings)
            except Exception as e:
                decky.logger.warning(f"hud: {e}")
            await asyncio.sleep(1)

    async def _health(self):
        # A boot that mounted KEDAVRA is confirmed only once its Gamescope writes status;
        # an unconfirmed boot is rolled back on the next boot.
        while True:
            try:
                await asyncio.to_thread(self.inst.mark_healthy)
            except Exception as e:
                decky.logger.warning(f"health check: {e}")
            await asyncio.sleep(5)

    async def _unload(self):
        self.health.cancel()
        self.hud_task.cancel()
        await asyncio.to_thread(self.hud.tick, False, None)

    async def _uninstall(self):
        try:
            await asyncio.to_thread(self.ctl.set_multiplier, 0)
        except Exception:
            pass
        await asyncio.to_thread(self.inst.uninstall)

    async def get_hud(self):
        return {"hud": bool(self.settings.get("hud"))}

    async def set_hud(self, on: bool):
        self.settings["hud"] = bool(on)
        await asyncio.to_thread(self._save_settings)
        return {"hud": self.settings["hud"]}

    async def get_settings(self):
        return {"hud": bool(self.settings.get("hud")), "hdr": bool(self.settings.get("hdr")), "mode": self.settings.get("mode", "off")}

    async def set_options(self, hdr: bool):
        """Engine options: generate for HDR games."""
        async with self.lock:
            self.settings["hdr"] = bool(hdr)
            await asyncio.to_thread(self._save_settings)
            self.options_at = time.monotonic()
            await asyncio.to_thread(self.ctl.apply_options, self.settings)
            return {"hdr": self.settings["hdr"]}

    async def set_mode(self, mode: str):
        """off | x2 (strict x2, the game capped at refresh / 2) | auto (x2 or x3, at most two generated frames)."""
        if mode == "cap":
            mode = "x2"  # a frontend from before GSF 1.5
        async with self.lock:
            await asyncio.to_thread(self.ctl.set_mode, mode)
            self.settings["mode"] = mode
            await asyncio.to_thread(self._save_settings)
            return {"mode": mode}

    async def get_status(self):
        st = await asyncio.to_thread(self.ctl.status)
        st["live"] = self.hud.live  # rates from the once-a-second HUD loop
        return st

    async def set_multiplier(self, mult: int):
        async with self.lock:
            return await asyncio.to_thread(self.ctl.set_multiplier, mult)

    async def start_capture(self, label: str):
        async with self.lock:
            return await asyncio.to_thread(self.ctl.start_capture, label)

    async def stop_capture(self):
        async with self.lock:
            return await asyncio.to_thread(self.ctl.stop_capture)

    async def list_captures(self):
        return await asyncio.to_thread(self.ctl.list_captures)

    async def get_logs(self):
        return await asyncio.to_thread(self.ctl.logs)

    async def diagnose(self):
        return await asyncio.to_thread(self.inst.diagnose)

    async def save_report(self):
        return await asyncio.to_thread(self.inst.report, self.out)

    async def install(self):
        async with self.lock:
            return await asyncio.to_thread(self.inst.install)

    async def uninstall(self):
        async with self.lock:
            return await asyncio.to_thread(self.inst.uninstall)

    async def reboot(self):
        return await asyncio.to_thread(self.inst.reboot)

    async def run_bench(self):
        async with self.lock:
            return await asyncio.to_thread(bench, Path(__file__).resolve().parent, self.out)
