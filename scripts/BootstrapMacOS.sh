#!/usr/bin/env bash
# Build an isolated, pinned Vulkan toolchain; requires Xcode CLI tools, CMake,
# Ninja and Python 3. Does not install system packages or imply GPU support.
set -euo pipefail
asterRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "$(uname -s)" != Darwin ]]; then
  echo "BootstrapMacOS.sh requires a native macOS host." >&2
  exit 1
fi
for program in cmake ninja python3 curl shasum tar xcrun; do
  if ! command -v "$program" >/dev/null 2>&1; then
    echo "Missing prerequisite: $program" >&2
    exit 1
  fi
done
xcrun --find clang >/dev/null
asterBuild="$asterRoot/build/macos-vulkan"
asterInstall="$asterBuild/install"
asterJobs="${ASTER_BUILD_JOBS:-3}"
if [[ ! "$asterJobs" =~ ^[1-9][0-9]*$ ]] || (( asterJobs > 64 )); then
  echo "ASTER_BUILD_JOBS must be an integer in [1,64]." >&2
  exit 1
fi
mkdir -p "$asterBuild/sources" "$asterRoot/build/_archives"
python3 - "$asterRoot/cmake/MacOSVulkan.lock.json" > "$asterBuild/Archives.tsv" <<'PY'
import json, sys
for name, entry in json.load(open(sys.argv[1], encoding="utf-8")).items():
    print(name, entry["URL"], entry["SHA256"], sep="\t")
PY

# Verify cached archives too. A revision change gets a separate extraction tree,
# while generated sources and all downloaded binaries remain under build/.
while IFS=$'\t' read -r name url digest; do
  archive="$asterRoot/build/_archives/macos-$name-$digest.tar"
  if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 --output "$archive.partial" "$url"
    actual="$(shasum -a 256 "$archive.partial")"
    if [[ "${actual%% *}" != "$digest" ]]; then
      echo "SHA-256 mismatch for $name" >&2
      exit 1
    fi
    mv "$archive.partial" "$archive"
  fi
  actual="$(shasum -a 256 "$archive")"
  if [[ "${actual%% *}" != "$digest" ]]; then
    echo "SHA-256 mismatch for cached $name" >&2
    exit 1
  fi
  sourceDirectory="$asterBuild/sources/$name-$digest"
  if [[ ! -f "$sourceDirectory/.aster-complete" ]]; then
    mkdir -p "$sourceDirectory"
    tar -xf "$archive" -C "$sourceDirectory" --strip-components 1
    touch "$sourceDirectory/.aster-complete"
  fi
  # The link only names an extraction inside this script's private build tree.
  ln -sfn "$sourceDirectory" "$asterBuild/sources/$name"
done < "$asterBuild/Archives.tsv"

BuildDependency()
{
  local name="$1"
  shift
  cmake -S "$asterBuild/sources/$name" -B "$asterBuild/build/$name" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$asterInstall" \
    -DCMAKE_PREFIX_PATH="$asterInstall" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 "$@"
  cmake --build "$asterBuild/build/$name" --parallel "$asterJobs"
  cmake --install "$asterBuild/build/$name"
}

BuildDependency vulkan_headers -DVULKAN_HEADERS_ENABLE_TESTS=OFF
BuildDependency vulkan_loader -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF
BuildDependency vulkan_utility -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF
BuildDependency spirv_headers -DSPIRV_HEADERS_ENABLE_TESTS=OFF
BuildDependency spirv_tools -DSPIRV-Headers_SOURCE_DIR="$asterBuild/sources/spirv_headers" \
  -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF
BuildDependency vulkan_validation -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF
BuildDependency glslang -DGLSLANG_TESTS=OFF -DENABLE_OPT=OFF -DGLSLANG_ENABLE_INSTALL=ON
BuildDependency vulkan_tools -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF -DBUILD_CUBE=OFF -DBUILD_ICD=OFF

mkdir -p "$asterInstall/lib" "$asterInstall/share/vulkan/icd.d" "$asterInstall/share/licenses/MoltenVK"
cp "$asterBuild/sources/moltenvk/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$asterInstall/lib/"
cp "$asterBuild/sources/moltenvk/LICENSE" "$asterInstall/share/licenses/MoltenVK/"
python3 - "$asterBuild" "$asterInstall" <<'PY'
import json, pathlib, shlex, sys
build, install = map(pathlib.Path, sys.argv[1:])
manifest = json.loads((build / "sources/moltenvk/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json").read_text())
manifest["ICD"]["library_path"] = "../../../lib/libMoltenVK.dylib"
icd = install / "share/vulkan/icd.d/MoltenVK_icd.json"
icd.write_text(json.dumps(manifest, indent=2) + "\n")
environment = {
    "VULKAN_SDK": str(install),
    "VK_DRIVER_FILES": str(icd),
    "VK_ICD_FILENAMES": str(icd),
    "VK_LAYER_PATH": str(install / "share/vulkan/explicit_layer.d"),
}
with (build / "Environment.sh").open("w") as output:
    output.write("# Generated native development environment; do not commit.\n")
    for key, value in environment.items():
        output.write(f"export {key}={shlex.quote(value)}\n")
    output.write(f"export PATH={shlex.quote(str(install / 'bin'))}:\"$PATH\"\n")
    output.write(f"export CMAKE_PREFIX_PATH={shlex.quote(str(install))}\"${{CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}}\"\n")
    output.write(f"export DYLD_LIBRARY_PATH={shlex.quote(str(install / 'lib'))}\"${{DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}}\"\n")
PY
echo "Native toolchain installed. Source $asterBuild/Environment.sh before configuring or running Aster."
echo "Run vulkaninfo --summary to record the actual runner's Vulkan/Metal capabilities."
