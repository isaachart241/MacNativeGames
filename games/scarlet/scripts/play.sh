#!/bin/zsh
# Plays Scarlet in a window, set up the way the test runs in CHANGELOG.md are (GPU submission on, the default GPU memory
# budget, 40 % of RAM; app/launcher.m still sets Bayonetta's 5000 MB and TOTK's MetalFX paths, untested here).
# Needs `make runtime shader-tools` done and the work folder prepared (README "Stage 1"). The log goes to
# SCARLET_build/logs/play.log; `echo "shot $PWD/shot.png" >> SCARLET_build/logs/play-input.txt` takes a screenshot.
set -eu
cd "${0:A:h}/../SCARLET_build"
[ -x build/scarlet-native ] || { echo "build/scarlet-native is missing: run make runtime shader-tools first" >&2; exit 1; }
mkdir -p logs; : > logs/play-input.txt
exec env -u TOTK_HEADLESS TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 \
  DM_PRESENT_LINK_FALLBACK=1 TOTK_SLOT_OWNER=user TOTK_SLOT_PURPOSE=player TOTK_INPUT_FILE="$PWD/logs/play-input.txt" \
  caffeinate -d -i ./build/scarlet-native rewritten/v400 boot 2> logs/play.log
