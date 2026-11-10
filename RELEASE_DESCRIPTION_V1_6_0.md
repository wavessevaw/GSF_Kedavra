## What's New in 1.6.0

- **Kedavra Focus:** New adaptive edge enhancement for sharper contours, geometry, and fine details.
- **Intelligent Sharpening:** Adjusts processing to local contrast, edge direction, and existing sharpness.
- **Halo Protection:** Limits excessive bright and dark outlines around objects.
- **Upscaler Independence:** Processes the final image without modifying FSR 4, FSR 3, XeSS, DLSS, or other upscalers.
- **Simplified Controls:** One Focus ON/OFF switch, with processing strength fixed at 100% when enabled.
- **Frame Generation Support:** Enhances both real and generated frames in Off, Auto, ×2, and ×3 modes.
- **Efficient GPU Processing:** One existing shader pass, with no additional full-resolution buffers.
- **Existing Features Preserved:** Frame generation, Silk scheduling, CAS/DLS, HUD, diagnostics, and rollback.

**Note:** Kedavra Focus is SDR-only. HDR images bypass the enhancement.
