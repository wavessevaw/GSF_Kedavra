# Kedavra Focus — test candidate

This prerelease adds **Setup → Kedavra Focus**, a live on/off switch and a **Processing strength** slider from 0 to 100%, initially 50%. No game restart is needed to change these controls. Strength is saved; Focus remains off after Gamescope restarts.

Focus sharpens soft contours while limiting local halos and noise amplification. It works with Frame Generation off or on. CAS and DLS remain available; disabling Focus restores the previous sharpener within the plugin session, preserving other effects.

## Download and install

Download **GSF-Kedavra-1.5.1-detail.2.zip** below — not GitHub's automatic source-code archive. It includes the Decky plugin, Gamescope runtime, libraries and benchmark tools. Verify it with the attached SHA256 file.

Install the ZIP through Decky Loader. If the bundled runtime is not already installed, use Setup to install/update Gamescope KEDAVRA and reboot. Then enable Kedavra Focus in Setup and adjust Processing strength. The attached English README includes full installation, disabling and rollback instructions.

## Validation and limitations

This exact source revision passed all six KEDAVRA CI jobs, including backend tests, frontend compilation, engine regression, shader validation, Gamescope build, older-glibc compatibility and the comparison lab.

**This is not Stable and has not passed Steam Deck hardware validation.** GPU budget, frame pacing, latency and visual gameplay acceptance remain open. The new effect is SDR-only; HDR bypass is retained. CAS can restore more fine texture, and some temporal residual errors remain. No FSR 4 equivalence is claimed.

Source: [3553eb0](https://github.com/wavessevaw/GSF_Kedavra/commit/3553eb04dc8b29e6beda5b5c88d9970c412d128a).
[Technical report and PR](https://github.com/wavessevaw/GSF_Kedavra/pull/90).
[Successful build](https://github.com/wavessevaw/GSF_Kedavra/actions/runs/42704101776).

The download here is the CI-built package for this revision, with its own attached checksum. It supersedes the workspace-only download link, whose locally packaged ZIP used the earlier byte-identical engine source build.
