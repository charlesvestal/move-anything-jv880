# Move Anything - JV-880 Module

Roland JV-880 synthesizer emulator module for [Move Anything](https://github.com/charlesvestal/move-anything).

Based on [mini-jv880](https://github.com/giulioz/mini-jv880) by giulioz (which is based on [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) by nukeykt).

## Requirements

- Move Anything installed on your Ableton Move
- JV-880 ROM files (v1.0.0):
  - `jv880_rom1.bin` (32KB)
  - `jv880_rom2.bin` (256KB)
  - `jv880_waverom1.bin` (2MB)
  - `jv880_waverom2.bin` (2MB)
  - `jv880_nvram.bin` (optional, 32KB)

**Note:** ROM version 1.0.0 is required. Version 1.0.1 ROMs do not work correctly.

## Expansion Cards

Multiple SR-JV80 expansion cards are supported simultaneously. Name your expansion ROMs using the format `expansion_XX.bin` where XX is the card number (01-99), and place them in the roms folder.

Examples:
- `expansion_01.bin` - SR-JV80-01 Pop
- `expansion_04.bin` - SR-JV80-04 Vintage Synth
- `expansion_10.bin` - SR-JV80-10 Bass & Drum
- `expansion_97.bin` - SR-JV80-97 Experience (2MB)

ROMs are automatically unscrambled on first load. A patch cache is created to speed up subsequent loads.

Supported expansion cards:
- **8MB cards**: SR-JV80-01 through SR-JV80-19 (Pop, Orchestral, Piano, Vintage Synth, World, Dance, etc.)
- **2MB cards**: SR-JV80-97, 98, 99 Experience series

## Building

```bash
./scripts/build.sh
```

This uses Docker for cross-compilation. The output will be in `dist/jv880/`.

## Installation

1. Place your ROM files in `dist/jv880/roms/`
2. Run:
```bash
./scripts/install.sh
```

Or manually copy `dist/jv880/` to `/data/UserData/move-anything/modules/` on your Move.

## Usage

- **Left/Right buttons or Jog wheel**: Change preset
- **Shift + Left/Right**: Jump to next/previous bank (Internal A, Internal B, Expansions)
- **Up/Down (+/-) buttons**: Octave transpose (±4 octaves)
- **Pads**: Play notes

The display shows the current bank name, LCD output from the emulator, and patch number.

## Signal Chain Integration

JV-880 works both as a standalone module and as a sound generator in Signal Chain patches. The install script adds chain presets for using JV-880 with arpeggiators and effects.

## Performance

- CPU usage: ~80% on Move's CM4
- Latency: ~46ms

## License

This module includes code from:
- mini-jv880 by giulioz
- Nuked-SC55 by nukeykt

See LICENSE file for details.
