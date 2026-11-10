# Kedavra Focus 1.5.1-detail.4 — Balanced Edge experimental candidate

This release **replaces the installed Focus shader** with a bounded CAS-derived balanced edge response. It is opt-in and SDR-only, not Stable.

The one-pass filter uses contrast-adaptive per-channel correction, directional weighting, smooth gain reduction on high-contrast sharp boundaries, local ringing attenuation and bounded excursions. Nine samples, no new full-resolution buffers or history. The existing Focus switch, strength slider, diagnostics, CAS/DLS and HDR bypass remain. No FG algorithms or external upscaler settings are modified.

Conservative / balanced / strong were compared internally; balanced is installed. It preserves more useful photo/texture detail than the previous restrictive prototypes while keeping the procedural halo-control area at 146 (50% strength), versus 150 for CAS and Detail .3. It does not promise zero ringing. Noise amplification is higher than .3 but lower than CAS in the tested control. Motion metrics are mixed: photographic pan improves, some FG errors become more visible. See the full measured trade-offs in docs/detail/BALANCED.md and the evidence archive.

Download the full installation ZIP and SHA256 below. Install through Decky, run **Reinstall Gamescope KEDAVRA**, then reboot. Start with Focus at 50%; reduce strength on noisy or already sharpened images. Focus remains off by default. To roll back, use the restore action, reboot, then reinstall the official 1.5.0 plugin and matching runtime and reboot again.

Edge-Evidence.zip contains identical input/reference PNGs, CAS, DLS, frozen Detail .2/.3, the selected shader and internal configurations, plus seven labeled motion comparisons and metrics. Review videos are not frame-pacing measurements. The negative halo control is procedural, not OptiScaler footage.

No Steam Deck or physical GPU was available. Hardware <=0.3 ms / <=110% CAS, compositor P95/P99, latency, actual upscaler compatibility and perceptual motion acceptance remain unverified. CI software dispatch timings are not Deck measurements. The ZIP includes standalone Deck benchmark tools.

PR: https://github.com/wavessevaw/GSF_Kedavra/pull/90
