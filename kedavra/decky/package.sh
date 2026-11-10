#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the GSF Kedavra Decky plugin zip (top folder stays KEDAVRA, so a new zip
# installs over the old one in place):
#   package.sh OUT.zip [RUNTIME_DIR [BENCH_DIR]]
# RUNTIME_DIR: bundled Gamescope runtime (bin/, lib/, VERSION; see kedavra/gamescope/bundle.sh).
# BENCH_DIR: kfg_vktest, kfg_posttest and the SPIR-V shaders. Without them the zip holds the panel only.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/build/KEDAVRA.zip}
RUNTIME=${2:-}
BENCH=${3:-}
cd "$HERE"
npm ci --no-audit --no-fund
npm run -s build
STAGE=$(mktemp -d)
P=$STAGE/KEDAVRA
mkdir -p "$P/bin" "$P/dist"
cp main.py plugin.json package.json README.md "$P/"
cp -r py_modules "$P/"
rm -rf "$P/py_modules/__pycache__"
cp dist/index.js "$P/dist/"
cp ../tools/kfg_pacing.py "$P/bin/"
cp ../../LICENSE.md ../shaders/THIRD_PARTY.md "$P/"
if [ -n "$RUNTIME" ]; then
	cp -a "$RUNTIME" "$P/runtime"
	chmod +x "$P/runtime/bin/"*
fi
if [ -n "$BENCH" ]; then
	mkdir -p "$P/bench"
	cp "$BENCH"/kfg_vktest "$BENCH"/*.spv "$P/bench/"
	chmod +x "$P/bench/kfg_vktest"
	if [ -f "$BENCH/kfg_posttest" ]; then
		cp "$BENCH/kfg_posttest" "$P/bench/"
		chmod +x "$P/bench/kfg_posttest"
	fi
	if [ -f "$BENCH/kfg_detailtest" ]; then
		cp "$BENCH/kfg_detailtest" "$P/bench/"
		cp -r "$BENCH/research" "$P/bench/"
		cp ../tools/detail/bench_detail.py "$P/bench/"
		chmod +x "$P/bench/kfg_detailtest"
	fi
fi
if [ -f ../../docs/detail/README.md ]; then
	mkdir -p "$P/docs/detail"
	cp ../../docs/detail/*.md ../../docs/detail/*.json "$P/docs/detail/"
fi
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
# Decky extracts with Python zipfile (no modes, no symlinks): the backend restores +x.
(cd "$STAGE" && zip -qr "$OUT" KEDAVRA)
rm -rf "$STAGE"
echo "$OUT"
