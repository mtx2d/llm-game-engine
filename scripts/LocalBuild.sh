#!/usr/bin/env bash
set -euo pipefail
asterRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
asterTools="$asterRoot/.tools/sysroot/usr"
export PATH="$asterTools/bin:$PATH"
export LD_LIBRARY_PATH="$asterTools/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export VK_LAYER_PATH="$asterTools/share/vulkan/explicit_layer.d"
export CMAKE_PREFIX_PATH="$asterTools${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
cd "$asterRoot"
exec "$@"
