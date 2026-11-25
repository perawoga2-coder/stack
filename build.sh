#!/usr/bin/env bash
# STACK's build for The Firmware Hub's shared workflow (zvenson/fm1-build): run in the repository's root.
#   ./build.sh --release X.Y[-suffix]      -> stack/app/build/stack-X.Y[-suffix].fwsc
# The workflow provides $JIELI_TOOLCHAIN and the AC79 SDK (~/fw-AC79_AIoT_SDK). fm1-base comes at the pinned commits
# in stack/BASE_COMMITS.txt, unchanged.
set -euo pipefail
cd "$(dirname "$0")/stack"
BASE=$(sed -n 1p BASE_COMMITS.txt)
BSP=$(sed -n 2p BASE_COMMITS.txt)
if [ ! -d base/.git ]; then
  git clone -q https://github.com/charlesvestal/fm1-base base
fi
git -C base checkout -q "$BASE"
git -C base submodule update --init -q
git -C base/bsp checkout -q "$BSP"
sh base/tools/setup_toolchain.sh --only uv,python
base/.venv/bin/python base/tools/build.py app "$@"
ls -la app/build/*.fwsc
