# Move Anything - Mini-JV Module

ROM-based PCM synthesizer emulator module for [Move Anything](https://github.com/charlesvestal/move-anything).

Based on [mini-jv880](https://github.com/giulioz/mini-jv880) by giulioz (which is based on [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) by nukeykt).

## Quick Start

**Already have ROMs and Move Anything installed?**

1. Launch Move Anything → **Module Store** → **Sound Generators** → **Mini-JV** → **Install**
2. Copy your ROM files to Move (see Requirements below)
3. Select **Mini-JV** from the main menu
4. **Play pads** - you're making Mini-JV sounds!
5. **Jog wheel** to browse 192+ patches
6. **Encoders 1-8** for quick sound shaping (Cutoff, Resonance, Attack, etc.)
7. **Menu button** for deep editing

See the [full manual](docs/JV880_MOVE_MANUAL.md) for complete control documentation.

## Features

- Full Mini-JV emulation with 192 internal patches
- Multiple SR-JV80 expansion card support (sorted alphabetically for easy browsing)
- Menu-driven parameter editing with real-time value display
- Encoder macros for quick sound shaping:
  - Patch mode: Cutoff, Resonance, Attack, Release, LFO Rate, LFO Depth, FX Send, Level
  - Performance mode: Level, Pan, Coarse/Fine Tune, Key Range, Velocity
- Touch-sensitive encoders show current parameter values
- Full Patch mode editing (all 4 tones with complete parameter access)
- Full Performance mode editing:
  - Part parameters (level, pan, tune, key range, velocity, patch selection)
  - Save performances to 16 Internal slots
  - Select expansion card for Card patches (with bank pages for >64 patch expansions)

## Prerequisites

- [Move Anything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Requirements

ROM files (v1.0.0):
  - `jv880_rom1.bin` (32KB)
  - `jv880_rom2.bin` (256KB)
  - `jv880_waverom1.bin` (2MB)
  - `jv880_waverom2.bin` (2MB)
  - `jv880_nvram.bin` (32KB)

**Note:** ROM version 1.0.0 is required. Version 1.0.1 ROMs do not work correctly.

## Expansion Cards

Multiple SR-JV80 expansion cards are supported simultaneously. Place expansion ROMs in the `roms/expansions/` subfolder with filenames containing "SR-JV80".

Examples:
- `SR-JV80-01_Pop.bin`
- `SR-JV80-04_Vintage_Synth.bin`
- `SR-JV80-10_Bass_Drum.bin`
- `SR-JV80-97_Experience.bin`

ROMs are automatically unscrambled on first load. A patch cache is created to speed up subsequent loads.

Supported expansion cards:
- **8MB cards**: SR-JV80-01 through SR-JV80-19 (Pop, Orchestral, Piano, Vintage Synth, World, Dance, etc.)
- **2MB cards**: SR-JV80-97, 98, 99 Experience series

## Installation

### Quick Install (pre-built)

1. Download and install the module:
```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything-jv880/main/minijv-module.tar.gz | \
  ssh ableton@move.local 'tar -xz -C /data/UserData/move-anything/modules/'
```

2. Copy your ROM files to the device:
```bash
scp jv880_rom1.bin jv880_rom2.bin jv880_waverom1.bin jv880_waverom2.bin \
  ableton@move.local:/data/UserData/move-anything/modules/minijv/roms/
```

### Build from Source

Requires Docker (recommended) or ARM64 cross-compiler.

```bash
git clone https://github.com/charlesvestal/move-anything-jv880
cd move-anything-jv880
./scripts/build.sh
```

1. Place your ROM files in `dist/minijv/roms/`
2. Run:
```bash
./scripts/install.sh
```

## User Manual

See `docs/JV880_MOVE_MANUAL.md` for the full control map and editor workflow.

## Signal Chain Integration

Mini-JV works both as a standalone module and as a sound generator in Signal Chain patches. The install script adds chain presets for using Mini-JV with arpeggiators and effects.

## Current Limitations

- **Patch saving**: Patch edits affect the temporary working patch but cannot be saved to Internal bank yet.
- **Some packed parameters**: A few part parameters (transmit/receive switches, output select, MIDI channel routing) aren't exposed in the UI yet.

## Performance

- CPU usage: ~38% on Move's CM4 (varies with patch complexity)
- Latency: ~46ms (buffered emulation)

## License

This module includes code from:
- mini-jv880 by giulioz
- Nuked-SC55 by nukeykt

See LICENSE file for details.
