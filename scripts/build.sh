#!/usr/bin/env bash
# Build Mini-JV module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Mini-JV Module Build (via Docker) ==="
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

echo "=== Building Mini-JV Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/minijv/roms/expansions

# Compile resample library (C code)
echo "Compiling resample library..."
for f in resample resamplesubs filterkit; do
    ${CROSS_PREFIX}gcc -Ofast -c -fPIC \
        -march=armv8-a -mtune=cortex-a72 \
        -fomit-frame-pointer -fno-stack-protector \
        -DNDEBUG \
        src/dsp/resample/${f}.c \
        -Isrc/dsp/resample \
        -o build/${f}.o
done

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
    build/resample.o build/resamplesubs.o build/filterkit.o \
    -o build/dsp.so \
    -Isrc/dsp \
    -Isrc/dsp/resample \
    -lm -lpthread

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat src/module.json > dist/minijv/module.json
cat src/ui.js > dist/minijv/ui.js
cat src/ui_menu.mjs > dist/minijv/ui_menu.mjs
cat src/ui_browser.mjs > dist/minijv/ui_browser.mjs
cat src/jv880_sysex.mjs > dist/minijv/jv880_sysex.mjs
cat build/dsp.so > dist/minijv/dsp.so
chmod +x dist/minijv/dsp.so

# Create tarball for release
cd dist
tar -czvf minijv-module.tar.gz minijv/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/minijv/"
echo "Tarball: dist/minijv-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
echo ""
echo "NOTE: You need to provide ROM files in dist/minijv/roms/:"
echo "  - jv880_rom1.bin"
echo "  - jv880_rom2.bin"
echo "  - jv880_waverom1.bin"
echo "  - jv880_waverom2.bin"
echo "  - jv880_nvram.bin (optional)"
