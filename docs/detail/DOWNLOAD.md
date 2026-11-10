## Kedavra Focus 1.5.1-detail.3 — test candidate

This is a full installable candidate based on stable 1.5.0. It is **not a Stable release** and is not an accepted Edge Enhancement implementation.

### What changed
- Fixed a colour-channel defect: a constant channel could cancel useful contour sharpening in the other channels even at 100%.
- Bounded the Setup strength slider to 0–100% and reserved space for its handle.
- Added compositor-reported Focus status and a recorded game-pass counter, with HDR/source/allocation bypass explanations.
- Kept the Focus switch, saved strength, live control, CAS/DLS, default-off behavior and existing FG algorithms.

### Edge research outcome
Three profile-based Edge prototypes were built and tested on the same 170 inputs as Off, CAS, DLS and Detail .2. All three were rejected. The stricter anti-halo gate removed too much useful correction; weaker variants still expanded existing halos. None of those prototypes is installed.

The retained Detail colour fix improves a targeted colour-edge reference result (41.126 → 45.048 dB versus .2), but it is not a universal quality improvement. Some FG-frame errors worsen. Existing halos can still widen; keep Focus off on visibly oversharpened images. No FSR 4 equivalence or automatic upscaler detection is claimed.

### Download and install
Download **GSF-Kedavra-1.5.1-detail.3.zip** and its SHA256 file below. The ZIP includes the Decky plugin, complete Gamescope runtime and Deck benchmark tools. Install through Decky, then use Kedavra's runtime install/update action and reboot. Installing only the panel does not update the running compositor. In Setup, start at 50%; the game-pass counter helps distinguish accepted settings from actual recorded processing.

**Edge-Evidence.zip** contains lossless input/reference/comparison PNGs, seven labeled review videos and metrics, including rejected experiments. Videos use 30 fps review playback, not measured output pacing. The ringing control is procedural, not an OptiScaler capture.

### Validation and limits
The pinned source CI builds Gamescope and checks older-glibc startup, backend/UI, shaders, Vulkan correctness and engine/scheduler behavior. No Steam Deck LCD/OLED or physical GPU was available. Hardware GPU budget, compositor P95/P99, latency, actual Steam UI layout, Assassin's Creed behavior and named-upscaler compatibility remain unverified. Software timestamps are not hardware measurements. HDR enhancement remains bypassed.

See the attached English README for installation, controls and rollback. To return to stable, use the existing restore action, reboot, install the official 1.5.0 plugin/runtime and reboot again. Keep the stable ZIP available.

Source and technical review: https://github.com/wavessevaw/GSF_Kedavra/pull/90
