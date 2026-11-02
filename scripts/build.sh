#!/usr/bin/env bash
# Build microQ module for Schwung (ARM64)
#
# Uses CMake to build the gearmulator Vavra library and plugin wrapper.
# Automatically uses Docker for cross-compilation if needed.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-vavra-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== microQ Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
cd "$REPO_ROOT"

echo "=== Building microQ Module ==="

# Create build directory
mkdir -p build

# Run CMake configure with cross-compilation toolchain
echo "Configuring CMake..."
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja \
    2>&1

# Build plugin
echo "Building plugin..."
cmake --build build --target dsp module_smoke runtime_test dump_presets -j"${BUILD_JOBS:-4}" 2>&1

# Package
echo "Packaging..."
mkdir -p dist/vavra

# Copy files to dist
cp src/module.json dist/vavra/module.json
cp build/dsp.so dist/vavra/dsp.so
chmod +x dist/vavra/dsp.so

# Licence texts. dsp.so is a GPL-3.0 binary built on gearmulator, and it links
# MIT/BSD/Zlib code whose terms require the notice to travel with the binary.
# Deliberately unconditional: a missing file must fail the build, not ship a
# tarball without its licence.
rm -rf dist/vavra/licenses
mkdir -p dist/vavra/licenses
cp LICENSE dist/vavra/LICENSE
cp licenses/*.txt dist/vavra/licenses/

# Asset directory placeholders (ROMs required, extra banks optional)
mkdir -p dist/vavra/roms dist/vavra/banks

# Create tarball for release
cd dist
tar -czvf vavra-module.tar.gz vavra/module.json vavra/dsp.so vavra/LICENSE vavra/licenses
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/vavra/"
echo "Tarball: dist/vavra-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
echo ""
echo "IMPORTANT: Place microQ OS 2.23 .bin or .mid in the roms/ directory on device:"
echo "  /data/UserData/schwung/modules/sound_generators/vavra/roms/"
