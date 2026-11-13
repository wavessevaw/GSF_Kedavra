# Reproduce the first experiment

Build the ordinary Kedavra Vulkan harness and shaders with `make -C kedavra all`.
Copy its shader directory to two separate directories, baseline and pruned.
Compile only `research/4.0/prototypes/kfg_me_pruned.comp` into the pruned directory
as `kfg_me.spv`, with `glslangValidator -V --target-env vulkan1.1 -Ikedavra/shaders`.
Run `kedavra/build/kfg_vktest --spv DIR --size 320x200 --out OUTPUT --json RESULT`
for each directory, sequentially. Both small-size runs currently fail 30 existing
quality assertions; do not suppress or relabel these failures. Compare their
output directories with `python3 research/4.0/compare_images.py A B`.
For the full-size pan check, run each directory with
`--size 1280x800 --scene pan --bench 5 --json RESULT`.
The timing workload is the harness's fixed objects/HUD scene, independent of
`--scene pan`; the latter limits the quality check. The reported analysis figure
is a sum of per-pass medians, not a median of whole-frame durations.

Recorded runs used the existing local kfg_vktest binary and Mesa lavapipe 25.0.7.
Future clean builds should recheck equivalence. No physical Deck was available.
Do not run baseline and candidate timing processes concurrently.
