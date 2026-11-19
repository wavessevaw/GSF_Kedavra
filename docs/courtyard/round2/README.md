# Courtyard 2 test build

Based on Courtyard 1 (836ea592), including Response90.2. The engine scales a candidate displacement from the previous source pair by the ratio of measured source intervals. It checks a denser coarse lattice where the existing predictions still fit poorly. At disputed motion boundaries it checks the projected forward and backward velocities before sampling the endpoints. A two-color stationary edge check protects small opaque overlays. The extra path uses existing flow images during irregular SDR delivery, for the affected pair and up to two subsequent regular pairs. It adds no image, dispatch or output slot.

At 1280×800 the native RGB inputs and analytic reference frames are identical across comparisons. severe4_pct is the fraction of 4×4 tiles with mean absolute luma error over 48/255. severe4_max is the highest such fraction in one generated frame. badblk_pct is the original neighborhood-relative 8×8 metric. The severe tile improvement does not eliminate all grid artifacts. On some routes the 8×8 metric worsens slightly. Reversal uses a separate camera trajectory which changes direction and a separate cadence. See the per-route JSON files. The data is from lavapipe. Deck GPU time, physical latency and game footage remain unverified.

Other trials included lower analysis resolutions, depth preference, wider spatial voting, per-phase field selection, guided flow upsampling, larger fine search windows and unrestricted endpoint verification. The unrestricted verification reduced some large errors but also increased local errors and shader work. Downsampling and depth preference did not consistently improve the control metrics.

## Reproduce

Build the candidate harness with make -C kedavra all. Run each of h_courtyard, h_courtyard_reverse and h_courtyard_reversal with the command below. Use the same candidate harness with SPIR-V from the Courtyard 1 bench directory for the baseline. The command prints per-pair analysis and per-phase warp Vulkan durations if timestamp queries are supported. Repeat on RADV after warm-up, with identical power, clock and resolution settings.

```sh
KFG_COURTYARD_TIMING=1 KFG_HARD_X2=1 kedavra/build/kfg_vktest --spv kedavra/build --hard --size 1280x800 --scene h_courtyard --json direct.json
```

For the video, set KFG_DETAIL_SEQUENCE_OUT to a separate existing directory for each run of the direct route. Run comparison.py with those directories and --left 'Courtyard 1' --right 'Courtyard 2'.

Install the inner GFG ZIP with Decky, reinstall Gamescope in GFG Setup, then reboot. To roll back, install the previous GFG ZIP, reinstall its Gamescope and reboot.
