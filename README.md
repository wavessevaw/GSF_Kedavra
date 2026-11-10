# GSF Kedavra 1.6.0

Frame generation and adaptive edge enhancement inside Gamescope for Steam Deck LCD/OLED.

[Download 1.6.0](https://github.com/wavessevaw/GSF_Kedavra/releases/tag/v1.6.0) · [GFG Extreme 3.4.0](https://github.com/wavessevaw/GFG-Extreme/releases/tag/v3.4.0)

## What is new

**Kedavra Focus** is now a single on/off switch in Setup. Enabling it applies the
accepted bounded CAS-derived edge enhancement at **100% strength**. There is no
Focus strength slider. It improves soft contours and texture definition using
local contrast and direction analysis, reduces gain on already sharp boundaries,
and limits excessive excursions. All of these mechanisms run automatically.

Focus uses one existing sharpen pass and nine image samples. No additional
full-resolution buffers, temporal history or CPU readbacks were added. It works
with Frame Generation off, x2, x3 and Auto, on the final displayed real or generated
SDR frame. Switching it off removes the Focus pass. Focus starts off after a
Gamescope restart; the processing strength is fixed at 100% whenever enabled.

The frame-generation algorithms, Silk scheduler, pacing controls, HUD, diagnostics
and rollback support are retained from the existing release lineage. This release
continues the official 1.5.0 / Silk lineage; cancelled 1.4.1–1.4.4 work is not its base.

## Image compatibility

Focus processes the finished image without detecting or modifying FSR, XeSS, DLSS,
TAA/TSR, OptiScaler, Gamescope scaling or game settings. It does not reconstruct
missing ML detail. CAS and DLS remain available through the existing controls/API;
Focus selects its own sharpening algorithm rather than stacking another CAS/DLS pass.

**Focus is SDR-only.** HDR10 and scRGB retain the safe bypass; existing HDR frame
generation and legacy filter policies remain. Setup reports processing and bypass
status. Noise and existing FG artifacts can become more visible on difficult input;
use the Focus switch to compare or disable it on already oversharpened/noisy images.

## Installation

1. Download the full ZIP and SHA256 file from the release page. Keep the previous
   official ZIP for rollback. Verify the checksum before installation.
2. Install the ZIP using Decky Loader's developer installation option.
3. Open Kedavra Setup, choose **Reinstall Gamescope KEDAVRA**, then reboot.
   Updating the plugin alone does not replace the currently running compositor.
4. Enable **Kedavra Focus** in Setup. It applies live at 100%; no game restart is
   needed for subsequent on/off changes. Diagnostics identifies the installed runtime.

Use one controlling plugin at a time. GFG Extreme 3.4.0 includes the same engine
and exposes Focus in Graphics alongside its power/performance management features.

## Rollback

Use the existing restore/remove action and reboot to restore system Gamescope.
To return to Kedavra 1.5.0, install its official plugin ZIP, install that bundled
runtime and reboot again. The automatic boot rollback mechanism remains present.
Kedavra's runtime is installed separately and bind-mounted; SteamOS's system image
is not replaced by the plugin package.

## Validation and scope

The preceding Focus candidate was tested and accepted by the user, who authorized
this official release. Shader/Vulkan, engine/scheduler, plugin/backend and runtime
build checks remain reproducible in CI. The full package contains the matching
Gamescope runtime and standalone benchmark tools.

No new hardware GPU-budget certification is claimed. The recorded development
measurements use software Vulkan; formal Steam Deck GPU time, compositor P95/P99
and upscaler-specific capture matrices remain unverified by the development
executor. User acceptance is not relabeled as those quantitative measurements.

The algorithm and comparison history are in [the technical report](docs/detail/BALANCED.md).
That report describes the research candidate and its former slider; 1.6.0 uses the
same shader with a fixed 100% UI setting. Sources for this release are pinned by
the **v1.6.0** tag; use that tag when rebuilding.

GPL-3.0. See LICENSE.md and the shader third-party notices included in the package.
