#!/usr/bin/env bash
# Install an isolated development toolchain without modifying system packages.
set -euo pipefail
asterRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "$(dpkg --print-architecture)" != amd64 ]]; then
  echo "This local bootstrap currently supports Ubuntu amd64. Use system development packages on other platforms." >&2
  exit 1
fi
mkdir -p "$asterRoot/.tools/debs" "$asterRoot/.tools/sysroot"
cd "$asterRoot/.tools/debs"
apt-get download cmake cmake-data ninja-build librhash0 libjsoncpp25 \
  libvulkan-dev libvulkan1 vulkan-tools vulkan-validationlayers \
  glslang-tools spirv-tools libx11-dev libx11-6 libxrandr-dev libxinerama-dev \
  libxcursor-dev libxi-dev libxrender-dev libxfixes-dev x11proto-dev \
  libxext-dev libxcb1-dev libxau-dev libxdmcp-dev libasound2-dev xdotool libxdo3
for package in ./*.deb; do
  dpkg-deb -x "$package" "$asterRoot/.tools/sysroot"
done
cd "$asterRoot"
bash scripts/LocalBuild.sh cmake --version
echo "Toolchain ready. Run: bash scripts/LocalBuild.sh cmake --preset debug"
