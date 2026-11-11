# Kedavra 1.6.1 compatibility runtime

This build extends the accepted Kedavra 1.6.0 engine with GFG Benchmark HUD input
and native displayed-frame interval telemetry. Focus, optical flow, warp and the
Silk scheduler are unchanged.

The standalone panel participates in the shared controller lease: GFG has priority
while loaded; standalone commands and HUD pause, then resume after GFG releases
control. Existing preferences are retained. Both panels must use the compatible
controller implementation; the old 1.6.0 standalone panel cannot honor this lease.

Install the full Decky ZIP, reinstall Gamescope from Setup and reboot. Use this
companion if you keep standalone Kedavra installed alongside GFG 3.4.1. To roll
back, remove the custom runtime using Setup, reboot, then reinstall the previous
plugin and its matching runtime.

The HUD extension reserves contacts beginning inside an interactive HUD. Hold for
0.8 seconds to start a benchmark; moving more than 12 output pixels cancels the
hold. Normal game touches outside the HUD retain their existing handling.

Automated tests are software checks. No physical Steam Deck measurements or touch
validation are claimed by the build environment.
