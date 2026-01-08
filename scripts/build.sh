#!/usr/bin/env bash
# Build JV-880 module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== JV-880 Module Build (via Docker) ==="
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
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building JV-880 Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/jv880/roms

# Compile DSP plugin (with aggressive optimizations for CM4)
echo "Compiling DSP plugin..."
${CROSS_PREFIX}g++ -Ofast -shared -fPIC -std=c++11 \
    -march=armv8-a -mtune=cortex-a72 \
    -fno-exceptions -fno-rtti \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/jv880_plugin.cpp \
    src/dsp/mcu.cpp \
    src/dsp/mcu_opcodes.cpp \
    src/dsp/pcm.cpp \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm -lpthread

# Copy files to dist
echo "Packaging..."
cp src/module.json dist/jv880/
cp src/ui.js dist/jv880/
cp build/dsp.so dist/jv880/

echo ""
echo "=== Build Complete ==="
echo "Output: dist/jv880/"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
echo ""
echo "NOTE: You need to provide ROM files in dist/jv880/roms/:"
echo "  - jv880_rom1.bin"
echo "  - jv880_rom2.bin"
echo "  - jv880_waverom1.bin"
echo "  - jv880_waverom2.bin"
echo "  - jv880_nvram.bin (optional)"
