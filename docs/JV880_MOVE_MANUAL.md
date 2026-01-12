# JV-880 on Move (Module) Manual

This module emulates a Roland JV-880 synthesizer with a direct LCD-native interface. Move controls map directly to JV-880 front panel buttons.

For full JV-880 operation details, see the [Roland JV-880 Owner's Manual (PDF)](https://cdn.roland.com/assets/media/pdf/JV-880_OM.pdf).

## Quick Start

1. Launch Move Anything.
2. Select the **JV-880** module.
3. Play pads to trigger sounds.
4. Use Jog wheel to change patches/parameters.
5. Press MENU to toggle Patch/Performance mode.

## Display (4 Lines)

```
Line 1: Last pressed button (e.g., "Edit", "Data +")
Line 2: Loaded expansion card (e.g., "Card: 01 Pop")
Line 3: JV-880 LCD line 1 (inverted)
Line 4: JV-880 LCD line 2 (inverted)
```

Lines 3-4 show the actual JV-880 LCD content, just like the hardware display.

## Control Mapping

Move controls map directly to JV-880 front panel buttons:

| Move Control | JV-880 Function |
|--------------|-----------------|
| **Jog Wheel** | DATA dial (+/-) |
| **Left** | CURSOR - |
| **Right** | CURSOR + |
| **Menu** | PATCH/PERFORM |
| **SHIFT + Menu** | Cycle expansion cards |
| **Capture** | TONE SELECT / PARAM SHIFT |

## Step Buttons

```
Steps 1-4:   [EDIT] [SYSTEM] [RHYTHM] [UTILITY]
Steps 5-8:   (unused)
Steps 9-12:  [TONE 1] [TONE 2] [TONE 3] [TONE 4]
Steps 13-16: (unused)
```

### Steps 1-4: Mode Buttons

| Step | Function | LED |
|------|----------|-----|
| 1 | EDIT - Enter Patch/Performance Edit mode | Lit when in Edit mode |
| 2 | SYSTEM - Enter System Edit mode | Lit when in System mode |
| 3 | RHYTHM - Enter Rhythm Edit mode | Lit when in Rhythm mode |
| 4 | UTILITY - Enter Utility mode | Lit when in Utility mode |

### Steps 9-12: Tone Switch (Mode-Dependent)

These buttons have different functions depending on the current mode:

**Patch Mode:**
| Step | Function |
|------|----------|
| 9 | Tone 1 |
| 10 | Tone 2 |
| 11 | Tone 3 |
| 12 | Tone 4 |

**Performance Mode:**
| Step | Function |
|------|----------|
| 9 | MUTE |
| 10 | MONITOR |
| 11 | INFO |
| 12 | Tone 4 |

**Utility Mode:**
| Step | Function |
|------|----------|
| 9 | Tone 1 |
| 10 | Tone 2 |
| 11 | COMPARE |
| 12 | ENTER |

## Pads

- Always play the JV-880 sound engine.
- Velocity affects note volume.
- Aftertouch follows JV modulation routing.

## Expansion Cards

The JV-880 supports SR-JV80 expansion cards. This module can load multiple expansion ROMs simultaneously.

### Setup

Place expansion ROM files in `roms/expansions/` with "SR-JV80" in the filename:
```
roms/expansions/SR-JV80-01_Pop.bin
roms/expansions/SR-JV80-04_Vintage_Synth.bin
```

ROMs are automatically unscrambled on first load. A patch cache speeds up subsequent loads.

### Accessing Expansion Patches

**Method 1: Unified Patch List (Recommended)**
- All patches (internal + all expansions) appear in one continuous list
- Use the **DATA dial** (jog wheel) to scroll through patches
- The expansion loads automatically when you select one of its patches
- Line 2 shows the current expansion name

**Method 2: Manual Expansion Selection**
- **SHIFT + MENU** cycles through available expansion cards
- This filters the patch list to show only that expansion's patches
- "Internal" shows only the built-in JV-880 patches

### Display

- Line 2 shows the currently active expansion (e.g., "01 Pop", "04 Vintage Synth")
- "Internal" means using built-in JV-880 patches only

### Notes

- Only one expansion can be active at a time (like real hardware)
- Switching expansions resets the synth engine briefly
- Expansion patches are organized by bank (A, B, C, D per expansion)

## LED Indicators

LEDs reflect the actual JV-880 state:

| Control | LED Behavior |
|---------|--------------|
| Step 1 (Edit) | Orange when in Edit mode |
| Step 2 (System) | Orange when in System mode |
| Step 3 (Rhythm) | Orange when in Rhythm mode |
| Step 4 (Utility) | Orange when in Utility mode |
| Steps 9-12 (Tones) | **Orange** = tone active (playing), **White** = tone muted |
| Menu | Bright when in Patch mode, dim when in Performance mode |

## Patch vs Performance Mode

Press **MENU** (PATCH/PERFORM) to toggle between modes:

**Patch Mode** (Menu LED bright):
- Single patch plays on MIDI channel 1.
- 4 tones per patch, each with independent parameters.
- Use DATA dial to select patches.
- Steps 9-12 toggle individual tones on/off.

**Performance Mode** (Menu LED dim):
- 8 parts, each on its own MIDI channel.
- Part 8 is the Rhythm part.
- Use DATA dial to select performances.
- Steps 9-12 have different functions (see above).

## Navigation Tips

The JV-880 interface uses CURSOR and DATA for navigation:

- **CURSOR +/-** (Left/Right): Move between parameters or menu items.
- **DATA +/-** (Jog wheel): Change values or select patches.
- **EDIT**: Enter edit mode for current patch/performance.
- **TONE SELECT** (Capture) + **TONE SWITCH**: Select tone to edit.

## ROM Requirements

Place these files in the `roms/` folder:
- `jv880_rom1.bin` (required)
- `jv880_rom2.bin` (required)
- `jv880_waverom1.bin` (required)
- `jv880_waverom2.bin` (required)
- `jv880_nvram.bin` (optional - saves settings)

**Note:** ROMs must be version 1.0.0. Version 1.0.1 causes CPU traps.
