# Courtyard test candidate

Use the inner GFG plugin ZIP in Decky, reinstall Gamescope in GFG Setup, then reboot. For rollback repeat with the official GFG 3.5.0 ZIP.

The CPU ray renderer supplies only RGB to the real Vulkan interpolation pipeline. Its fence, moving sphere, ground, brick wall and fixed crosshair have independent depth and motion. Source times in milliseconds are 0, 33, 61, 108, 143, 199, 230 and 272. Each true intermediate is rendered at its exact time. The two camera routes have opposite translations and yaw. This short sequence tests disocclusions during a turn, not game compatibility or long-term stability.

For uneven SDR input the motion selection and repair passes use two compact half-windows. The lower-error half carries more weight near occlusions. Sampling stays at ten offsets per match. Timing decisions, display phases and the warp shader use Response90.2 behavior. Frame arrival time on Deck is the time of latching in Gamescope. The lab supplies the known source timestamps directly.

Build with make -C kedavra all. Run with KFG_HARD_X2=1 KFG_DETAIL_SEQUENCE_OUT=/existing/output/directory kedavra/build/kfg_vktest --spv kedavra/build --hard --size 1280x800 --scene h_courtyard --json result.json. Repeat with h_courtyard_reverse. For the baseline use the candidate harness and the original Response90.2 SPIR-V directory. Use comparison.py on the two output directories to reproduce the slowed video.

The JSON files contain software Vulkan measurements. badblk_pct counts interior 8x8 tiles with mean luma error over 6/255 and over 2.5 times the neighboring median. It is a proxy, not a count of visible artifacts. Mean error improves more consistently than the block count. Some worst frames and sharpness fluctuation scores regress. The candidate needs physical Deck frame pacing, GPU cost and gameplay checks.
