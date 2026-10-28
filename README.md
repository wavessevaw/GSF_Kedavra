<div align="center">

**English** · [Русский](README.ru.md)

<img src="docs/img/logo.png" alt="GSF Kedavra" width="144">

# GSF Kedavra

**Independent x2 / x3 frame generation for Steam Deck and Steam Machine in Gaming Mode. No Lossless Scaling. No launch command.**

GSF (GPU Synthetic Frames) generates the in-between frames inside a patched Gamescope, the compositor that already owns every vblank and every page flip in SteamOS Gaming Mode. A Decky plugin installs it, switches it on and off and measures the result. Since 1.3.1 it also targets the **Steam Machine** on a TV (60 or 120 Hz, games at 1080p or 4K).

[**Download 1.4.2**](https://github.com/wavessevaw/GSF/releases/tag/v1.4.2) · [Install](#installation) · [Modes](#modes) · [Measuring pacing](#measuring-pacing) · [How it works](#how-it-works)

[![Release](https://img.shields.io/github/v/release/wavessevaw/GSF?style=flat-square&color=52d64a&label=release)](https://github.com/wavessevaw/GSF/releases/latest)
[![Steam Deck](https://img.shields.io/badge/Steam%20Deck-OLED-0d1210?style=flat-square)](https://www.steamdeck.com/)
[![Steam Machine](https://img.shields.io/badge/Steam%20Machine-TV%2060%2F120%20Hz-0d1210?style=flat-square)](#steam-machine)
[![Decky](https://img.shields.io/badge/Decky%20Loader-plugin-0d1210?style=flat-square)](https://decky.xyz/)
[![SDR and HDR](https://img.shields.io/badge/output-SDR%20%7C%20HDR-0d1210?style=flat-square)](#known-limits)
[![License](https://img.shields.io/badge/license-GPL--3.0-0d1210?style=flat-square)](LICENSE.md)

<img src="docs/img/interface-1.1.1.png" alt="GSF Kedavra interface: the Decky panel in Custom mode at a 40 FPS base with mode tiles and ring cards, the in-game HUD, the Pace and Setup views" width="900">

<sub>The plugin's own interface code and HUD renderer, rendered with sample values. Not a measurement.</sub>

</div>

> **Status: experimental.** The engine, scheduler and plugin pass their software tests in CI, and the build runs on a Steam Deck OLED.

## Why GSF Kedavra

- **No Lossless Scaling.** No paid app, no DLL from another program, no Vulkan layer inside the game. GSF has its own interpolation engine (GPL-3.0).
- **Works on every game Gamescope shows.** Frames are taken from the compositor, so there is nothing to inject and no per-game launch option.
- **One zip, nothing by hand.** The Decky plugin installs and updates its own Gamescope build, checks the system and rolls back by itself. No Desktop Mode scripts.
- **The system image stays untouched.** The KEDAVRA Gamescope lives in `/var/lib/kedavra` and is mounted over the stock one only while the service runs. A boot that does not reach Gaming Mode returns to the stock Gamescope on the next boot.
- **Frame pacing comes first.** A vblank-indexed scheduler decides what every refresh shows, and the game is held on a fixed cadence that generation can fill evenly.
- **No FPS limit to set by hand.** In **Auto** the engine caps the game itself and picks the cap from the median of what the game really delivers (30, 35, 40 or 45 at 90 Hz, so x3 to x2, and 24 for x3.75 if you allow it). Or set your own base FPS with one slider.
- **Honest numbers.** The ring counts new frames on screen and the game's own frames, and the pacing record is taken from KMS page-flip timestamps, not from an FPS counter.

## Installation

Requires [Decky Loader](https://decky.xyz/). Tested on SteamOS 3.8.

1. Download [**GSF-Kedavra-1.4.2.zip**](https://github.com/wavessevaw/GSF/releases/download/v1.4.2/GSF-Kedavra-1.4.2.zip).
2. In Decky: **Settings → Developer → Install plugin from zip**, pick the file. Upgrading over an older KEDAVRA plugin keeps your settings.
3. Open the plugin, go to **Setup** and press **Install Gamescope KEDAVRA**. Reboot when asked.
4. After the reboot every check in **Setup** is green, with no line saying the installed Gamescope differs from the one in the plugin.

To remove it, press **Remove and restore stock Gamescope** in **Setup**, then reboot.

## Modes

| Mode | What it does |
| --- | --- |
| **Off** | The game runs untouched. The ring shows the game's own frame rate. |
| **Auto** | The engine limits the game to the highest of 30 / 35 / 40 / 45 FPS (90 Hz: x3, x2.6, x2.25, x2) that it keeps steady. It starts at 30, steps up one cap after each clean second, and when the game falls behind drops to the cap under the median of its frame rate over the last seconds. By default it never goes above x3: a game below 30 stays at 30 and frames repeat instead of being stretched further. With **Allow ×3.75** on, Auto may hold such a game at 24 instead (x3.75): smoother than repeats, but every real frame is stretched over almost four refreshes, so artifacts are more visible. |
| **Custom** | The engine holds the game at the base FPS you set (20 to 90) and generation fills the rest of the refresh. Rates that divide the refresh (30, 45 at 90 Hz) are the smoothest. Others, like 40, work with a 2-2-3 cadence. |

**For the most stable result, lock the frame rate in the game's own settings** (30 or 45) and choose the same value in **Custom**. A game that paces its own frames gives the generator evenly spaced inputs. No Steam FPS limit is needed: GSF sets the limit itself while generation is on and gives Steam's back when it is off.

Generation needs at least a few milliseconds of GPU time per game frame for motion analysis. If the game already uses all of the GPU, lower the base FPS.

## In-game HUD

<img src="docs/img/hud.png" alt="In-game HUD: 90 FPS on screen, 30 real, x3, pace 100" width="320">

The optional HUD draws three rings over the game, in the same style as [GFG Extreme](https://github.com/wavessevaw/GFG-Extreme):

- **FPS:** new frames that reach the screen each second (real and generated). Below it, **REAL** is the game's own frame rate. Repeated frames and Steam menu repaints are not counted.
- **x2 / x3:** the current multiplier. The arc is the share of generated frames. The label says **AUTO** or **CAP** with the base FPS.
- **PACE:** the share of refreshes that showed a new frame. Green above 99.5%.

## Measuring pacing

**Pace → Record pacing trace** writes the KMS flip time of every refresh while you play. Stop it after one or two minutes and the plugin analyses the trace on the device. The 1.4.2 targets are:

| Metric | Target |
| --- | --- |
| P95 interval error | ≤ 1 ms |
| P99 interval error | ≤ 2 ms |
| Missed refreshes | < 0.5% |
| Oscillation, latency drift | none |

Press **Setup → Save report to Desktop** when you report a problem: it saves one zip on the Desktop (`~/Desktop/GSF-report-….zip`) with the diagnostics, the latest pacing traces and the benchmark.

## Steam Machine

Since 1.3.1 the same zip installs on a Steam Machine (SteamOS Gaming Mode with Decky Loader), the same way as on the Deck. **Not yet run on a Steam Machine:** everything below comes from software tests.

- **Multipliers for a TV.** Auto never goes above ×3, and ×2 to ×2.5 is the sweet spot. At 60 Hz the game is held at 30 and doubled; **Allow ×2.5** lets it drop to 24 in heavy scenes. At 120 Hz the cap runs from 40 (×3) to 60 (×2). ×4 at 120 Hz is not recommended: it is off and only switched on in **Setup → Allow ×4 in Auto**.
- **Resolution costs little.** Motion is analysed at no more than 1536×960 pixels (1.44× the Deck's screen); a 1080p or 4K game is analysed scaled down, and only the final frame is built at full resolution. A pair of game frames costs 1.37× the Deck's work at 1080p and 1.74× at 4K (lavapipe), on a GPU several times faster than the Deck's.
- **VRR.** Generated frames need a fixed refresh, so adaptive sync (FreeSync) is paused while Auto or Custom is selected. **Off** gives it back.
- **HUD** keeps its share of the screen on a 1080p or 4K TV.

## How it works

1. Gamescope copies every new game frame into a small GPU ring.
2. Right after the frame is composited, a separate submission runs hierarchical motion estimation between it and the previous frame, split over two refreshes so it never delays a flip.
3. On every vblank the scheduler (`kedavra/sched/kfg_sched.h`) picks a real frame or an intermediate at the right phase. Frames from a game whose presents jitter around the cap keep their place on the cap grid, so motion stays even. Since 1.4.0 the scheduler moves content in eighths of a refresh: after a late game frame the picture runs about 1.25× fast for a few refreshes instead of skipping one, and when the next frame is late it slows down evenly instead of stopping. Since 1.4.1, when the ready frames would still run out, it briefly drops to quarter speed instead of freezing. Since 1.4.2 a slow-down is not paid back with a speed-up: the picture keeps its delay and gives a refresh back after a calm second, so it no longer changes speed at every late frame. A fractional cap (40 or 35 at 90 Hz) is followed on its real 2-2-2-3 release pattern, not its mean, and the motion analysis runs in one pass when it fits, so a pair is usable one refresh after its frame arrives. Each refresh still costs one warp or one copy.
4. The intermediate is synthesised at full resolution with occlusion-aware blending, and Gamescope flips it like any other frame.
5. The plugin reads the engine status once a second, draws the HUD and records traces.

More detail: [architecture](docs/kedavra/PHASE_B_ARCHITECTURE.md), [Deck test protocol](docs/kedavra/DECK_PROTOCOL.md), [engine and scheduler sources](kedavra/README.md).

## Known limits

- HDR games (HDR10 PQ and scRGB) get generation since 1.2.0; turn **HDR games** off to pass them through untouched. HDR generation has not been checked on a Deck yet.
- Built and tested for Steam Deck OLED at 90 Hz. Other refresh rates work in the model and tests but have not been measured on hardware.
- **Steam Machine (since 1.3.1, unconfirmed on hardware):** on a TV, Auto stays at ×3 at most (60 Hz: base 30, ×2; 120 Hz: 40 to 60), ×4 at 120 Hz is a not-recommended toggle in Setup, and VRR is paused while generation is on. Games above 1536×960 (1080p, 4K) are analysed at that size and only the final warp runs at their own resolution: lavapipe puts a 1080p pair at 1.37× the Deck's work and 4K at 1.74×.
- Fine repeating detail (stairs, fences, grilles) is the hardest case for any motion estimator. 1.0.0 reduced the shimmer, 1.1.0 follows walking forward better and 1.1.1 checks motion over a taller window so step edges are no longer erased, but steps can still break up at low base FPS.
- Stopping a Pace recording has restarted Gaming Mode on the Deck. 1.1.2 moves the trace file off the compositor, but the cause is not confirmed. If it happens, press **Setup → Save report to Desktop** right after: the report now includes the crash record.

## Building from source

```sh
make -C kedavra test                 # scheduler tests, model stress matrix, engine on a Vulkan device (lavapipe works)
make -C kedavra bench                # engine GPU timing (meaningful on the Deck)
sh kedavra/decky/package.sh out.zip  # plugin zip without the Gamescope runtime
```

The full plugin with the Gamescope runtime is built by [CI](.github/workflows/kedavra.yml) against Gamescope `aa289ea`.

## Repository

- [`kedavra/`](kedavra/): the current engine, scheduler, Gamescope integration and Decky plugin.
- [`engine/`](engine/), [`decky/`](decky/), [`docs/fg3/`](docs/fg3/), [`docs/archive/`](docs/archive/README.md): earlier GSF research (Vulkan layer prototypes, Temporal Engine, audits), kept for history.
- GSF is part of the same family as [GFG Extreme](https://github.com/wavessevaw/GFG-Extreme). See [MIGRATION.md](MIGRATION.md) for how the code was separated.

## License

**GPL-3.0-or-later**, with upstream copyright and third-party notices preserved: [license](LICENSE.md), [copyright](COPYRIGHT.md), [third-party notices](NOTICE.md), [licensing guidance in Russian](docs/LICENSING.ru.md). The GPL does not grant rights to present modified builds as official GSF releases; see [brand-use guidelines](TRADEMARKS.md). Contributions: [CONTRIBUTING.md](CONTRIBUTING.md).
