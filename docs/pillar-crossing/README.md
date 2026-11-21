# Kedavra 1.8.1-pillar.1 test candidate

This SDR build targets a specific disocclusion artifact. A generated frame can retain the flat colour of a post on a previously hidden moving character. When the output still resembles one real endpoint at the same pixel, the warp searches both real frames for the edge of the same locally uniform material. It uses the interpolated edge position and the exposed source to correct stale pixels. The operation runs inside the existing warp dispatch. Static HUD protection, scene cuts and HDR retain their original paths.

## Install and compare

Install the inner Decky ZIP, open Setup, reinstall the bundled Gamescope and restart the Deck. Frame Generation activates the change automatically. Focus controls a separate post-processing stage. To roll back, install [GSF Kedavra 1.8.0](https://github.com/wavessevaw/GSF_Kedavra/releases/tag/v1.8.0) and repeat the Gamescope reinstall and restart. GFG users should use the matching GFG package and its Setup page instead.

The test scene contains a walking articulated figure, a near post and an oscillating camera. Render the same seven source intervals with each set of shaders. Rendered reference images use the true phase time. Use the candidate harness for both engines.

```sh
make -C kedavra all
mkdir -p before after baseline-spv
cp kedavra/build/*.spv baseline-spv/
git show v1.8.0:kedavra/shaders/kfg_warp.comp > baseline-spv/kfg_warp.comp
glslangValidator -V --target-env vulkan1.1 -Ikedavra/shaders baseline-spv/kfg_warp.comp -o baseline-spv/kfg_warp.spv
KFG_DETAIL_SEQUENCE_OUT=before KFG_HARD_X2=1 kedavra/build/kfg_vktest --spv baseline-spv --size 1280x800 --hard --scene h_pillar_crossing --json before/score.json
KFG_DETAIL_SEQUENCE_OUT=after KFG_HARD_X2=1 kedavra/build/kfg_vktest --spv kedavra/build --size 1280x800 --hard --scene h_pillar_crossing --json after/score.json
python3 kedavra/tools/courtyard/check_pillar.py before after
```

Repeat with `--reverse-courtyard` and `--regular-courtyard` applied to both invocations. These are synthetic reference sequences. Review the visible shape as well as regional error and whole-frame metrics. For Deck timing set `KFG_COURTYARD_TIMING=1`, warm up both builds, alternate their order with the same TDP and clocks, and compare warp and total frame times at both refresh rates. Hardware cost, input latency and real-game quality still require measurement.
