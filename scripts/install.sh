#!/bin/bash
# Install Mini-JV module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/minijv" ]; then
    echo "Error: dist/minijv not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Mini-JV Module ==="

# Check for ROMs
if [ ! -f "dist/minijv/roms/jv880_rom1.bin" ]; then
    echo ""
    echo "WARNING: ROM files not found in dist/minijv/roms/"
    echo "You need to provide these files for the module to work:"
    echo "  - jv880_rom1.bin"
    echo "  - jv880_rom2.bin"
    echo "  - jv880_waverom1.bin"
    echo "  - jv880_waverom2.bin"
    echo "  - jv880_nvram.bin (optional)"
    echo ""
fi

# Deploy to Move - sound_generators subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/sound_generators/minijv"
scp -r dist/minijv/* ableton@move.local:/data/UserData/move-anything/modules/sound_generators/minijv/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/move-anything/patches/
fi

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/sound_generators/minijv"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/sound_generators/minijv/"
echo ""
echo "Restart Move Anything to load the new module."
