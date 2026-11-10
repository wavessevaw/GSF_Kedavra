# Kedavra Detail Reconstruction — research candidate

**Version: 1.5.1-detail.3, based on the published GSF Kedavra 1.5.0.**
This is an opt-in test candidate, not a Stable release. No Steam Deck LCD/OLED was
available during development. Hardware GPU budget, frame pacing, latency and
perceptual acceptance remain open. Do not infer FSR 4 reconstruction quality.

## What changes

`detail=S` adds spatial sharpening to the existing `STAGE_SHARPEN`. Strength is
0–1; `detail=0` bypasses the effect with no dispatch and no pixel changes. The
default remains off. CAS and DLS remain available with their existing behavior.
Only the last sharpening token is selected, so Detail does not stack with CAS/DLS.

The filter uses nine neighboring samples, directional luminance gradients, a
conservative low-contrast gate and shared RGB correction bounded by local channel
extrema. It sharpens coherent soft transitions while limiting halos and noise
amplification. It cannot recover missing texture or fix frame-generation geometry.
CAS can retain more very fine texture; Detail is not a universal replacement.

Detail chains operate on each displayed real or generated frame, including both
intermediate phases of ×3. They use the existing post images and dispatch path.
There are no new motion fields, history buffers, runtime libraries or CPU readbacks.
Legacy presets retain the 1.5.0 source-prefilter path. If other effects are combined
with Detail, that entire existing chain runs after generation. This costs more
often than legacy source-prefiltering when FG is enabled; the hardware cost must
be measured at the display rate, not only the game's source rate.

**SDR only for the new effect.** HDR10/PQ bypasses Detail; explicitly selected
legacy effects retain their existing HDR policy. scRGB retains full post bypass,
including negative and above-one values. No new SDR clamp is applied to HDR.

## Install and use

1. Keep the official [1.5.0 ZIP](https://github.com/wavessevaw/GSF_Kedavra/releases/tag/v1.5.0)
   available for rollback. Preserve your current settings.
2. Download the candidate ZIP and its `.sha256` file from the linked CI run.
   Run `sha256sum -c GSF-Kedavra-1.5.1-detail.3.zip.sha256` in that directory.
3. In Decky Loader developer settings, install the ZIP. It contains the **KEDAVRA**
   plugin, complete bundled Gamescope runtime, shaders and standalone test tools.
4. Open KEDAVRA, use its existing runtime install/update action, then reboot.
   Check Diagnostics to confirm the installed runtime matches the bundled commit.
5. Open **Setup → Kedavra Focus**. The switch and **Processing strength** slider
   (0–100%, initially 50%) apply live without restarting the game. Strength is saved;
   the switch reflects the running compositor and starts off after Gamescope restarts.
   Focus works with frame generation off or on. HDR sources bypass the effect.
   During this plugin session, disabling Focus restores the previous CAS/DLS selection
   while preserving other effects. Changes made by another controller are not repeatedly
   overridden. Failed or unconfirmed commands are reported instead of showing success.
6. The existing command-line mechanism remains available, for example:

   ```sh
   /var/lib/kedavra/runtime/bin/gamescopectl kfg_post 'detail=0.5'
   ```

   Run from a terminal in that Gamescope session. If necessary, use the session's
   `XDG_RUNTIME_DIR` and `GAMESCOPE_WAYLAND_DISPLAY`; do not target a different compositor.
   A separate controller such as GFG may overwrite `kfg_post` when applying a preset.

Test with FG off, ×2 and ×3 using the existing FG controls. Start at 50%. For an already sharp external upscaler output,
leave Detail off. No attempt is made to detect FSR or OptiScaler from pixels.

Disable all post effects immediately with:

```sh
/var/lib/kedavra/runtime/bin/gamescopectl kfg_post ''
```

`detail=0` disables only the selected sharpener; other named effects in the same
string remain active. Switching between legacy and Detail placement invalidates
old source history once, preventing mixing filtered and unfiltered ring frames.

## Roll back

Use the KEDAVRA panel's existing remove/restore action and reboot to restore the
system Gamescope. Reinstall the official 1.5.0 plugin ZIP, install its runtime and
reboot again. Verify the version in Diagnostics. The installer uses a boot service
and bind mount; it does not replace the SteamOS system image. The existing automatic
boot rollback remains present, but is not a substitute for keeping the stable ZIP.

## Evidence and acceptance

See [architecture](ARCHITECTURE.md), [comparison and limitations](REPORT.md), and
[Steam Deck verification](DECK-VALIDATION.md). The PR is a draft until hardware
and visual acceptance are complete. CI builds a candidate artifact only; this
branch has no Stable release publishing job.

Implemented and software-checked: parsing, zero bypass, one sharpen pass, alpha
preservation, local bounds, SDR output, compile/build tests and comparison tooling.
Device checks still needed: OLED/LCD timing and P95/P99, HDR passthrough in the
actual compositor, game capture temporal behavior, external upscalers, installation
and rollback on SteamOS. An offline image metric is not a perceptual validation.

See EDGE-STATUS.md for the contour audit, current fixes and unverified acceptance gates.
