#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prepares a KEDAVRA Gamescope source tree:
#   apply.sh <gamescope checkout>
# The checkout must be ValveSoftware/gamescope at GAMESCOPE_COMMIT (or a tree the
# hooks still apply to). The KEDAVRA sources are copied, not symlinked, so the
# tree builds on its own; rerun after changing anything under kedavra/.
set -eu

GAMESCOPE_COMMIT=aa289ea5977d8b21b92b0f246f0e8f0c225d180f
HERE=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$HERE/.." && pwd)
G=${1:?usage: apply.sh <gamescope checkout>}

if [ -d "$G/.git" ]; then
	have=$(git -C "$G" rev-parse HEAD)
	[ "$have" = "$GAMESCOPE_COMMIT" ] || echo "warning: $G is at $have, hooks were made against $GAMESCOPE_COMMIT" >&2
fi

if git -C "$G" apply --check "$HERE/hooks.patch" 2>/dev/null; then
	git -C "$G" apply "$HERE/hooks.patch"
elif git -C "$G" apply --check --reverse "$HERE/hooks.patch" 2>/dev/null; then
	echo "hooks already applied" >&2
else
	echo "error: hooks.patch does not apply to $G" >&2
	exit 1
fi

mkdir -p "$G/src/kfg/smaa" "$G/src/shaders/smaa"
cp "$HERE/src/kfg/kfg_hud_input.hpp" "$G/src/kfg/"
cp "$HERE/src/kfg/kfg.cpp" "$HERE/src/kfg/kfg.hpp" "$G/src/kfg/"
cp "$K/engine/kfg_plan.h" "$K/engine/kfg_post.h" "$K/engine/kfg_smaa_tex.h" "$K/sched/kfg_sched.h" "$K/sched/kfg_auto.h" "$G/src/kfg/"
cp "$K/engine/smaa/AreaTex.h" "$K/engine/smaa/SearchTex.h" "$G/src/kfg/smaa/"
cp "$K/shaders/kfg_common.glsl" "$K/shaders/kfg_smaa.glsl" "$G/src/shaders/"
cp "$K/shaders/smaa/SMAA.hlsl" "$G/src/shaders/smaa/"
for s in copy reduce me stats select warp post smaa_edge smaa_weight smaa_blend; do
	cp "$K/shaders/kfg_$s.comp" "$G/src/shaders/cs_kfg_$s.comp"
done
echo "KEDAVRA sources copied into $G"
