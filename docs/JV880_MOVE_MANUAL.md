# JV-880 on Move - User Manual

This module turns Move into a JV-880 synthesizer with menu-driven editing.

## Quick Start

1. Launch Move Anything
2. Select the **JV-880** module
3. Play pads to trigger sounds
4. Turn encoders to shape the sound (Cutoff, Resonance, Attack, etc.)
5. Use Jog wheel to browse patches
6. Press Menu button to access the edit menus

## Display Layout

The display shows 4 lines:

```
Line 1: Activity/status messages
Line 2: #PatchNum  PatchName     Oct:+0
Line 3: [Menu item or encoder labels]
Line 4: Clk:Browse  Menu:Edit
```

**Browser Mode**: Shows patch info and encoder macro labels
**Menu Mode**: Shows hierarchical menu navigation

## Controls Overview

### Browser Mode (Default)

| Control | Action |
|---------|--------|
| **Jog Wheel** | Browse patches/performances (with acceleration) |
| **Jog Click** | Open Browse menu (patches or performances) |
| **Menu Button** | Open Edit menu directly |
| **Left/Right** | Previous/next patch |
| **Shift + Left/Right** | Previous/next bank |
| **Capture Button** | Toggle Patch/Performance mode |
| **Encoders 1-8** | Macro controls (see below) |
| **Touch Encoder** | Show current value (3 sec minimum) |
| **Track Buttons 1-4** | Select Tone 1-4 |
| **Shift + Track** | Toggle tone mute |
| **Step Buttons 1-8** | Select Part 1-8 (Performance mode only) |
| **Shift + Step** | Toggle part mute (Performance mode only) |
| **Pads** | Play notes (velocity sensitive) |

### Menu Mode

| Control | Action |
|---------|--------|
| **Jog Wheel** | Scroll through menu / adjust value |
| **Jog Click** | Enter submenu / start editing / confirm |
| **Up/Down** | Scroll menu items |
| **Left/Right** | Quick adjust value (no edit mode needed) |
| **Back Button** | Cancel edit / go back one level / exit to browser |
| **Shift + Jog** | Fine adjust (when editing values) |

## Encoder Macros (Browser Mode)

Touch an encoder to see its current value. Turn to adjust.

### Patch Mode Macros (affect selected Tone)

| Encoder | Parameter | Scope |
|---------|-----------|-------|
| 1 | Cutoff | Tone filter |
| 2 | Resonance | Tone filter |
| 3 | Attack | Tone amp envelope |
| 4 | Release | Tone amp envelope |
| 5 | LFO Rate | Tone LFO1 |
| 6 | LFO Depth | Tone LFO1 filter depth |
| 7 | FX Send | Tone reverb send |
| 8 | Level | Tone amp |

### Performance Mode Macros (affect selected Part)

| Encoder | Parameter | Scope |
|---------|-----------|-------|
| 1 | Level | Part volume |
| 2 | Pan | Part pan |
| 3 | Coarse Tune | Part transpose |
| 4 | Fine Tune | Part detune |
| 5 | Key Range Low | Part key range |
| 6 | Key Range High | Part key range |
| 7 | Velocity Sense | Part velocity response |
| 8 | Velocity Max | Part max velocity |

- Encoders have acceleration: slow turn = fine control, fast turn = coarse
- Hold **Shift** while turning for fine adjustment (±1 steps)
- Track buttons 1-4 select tones in Patch mode (disabled in Performance mode)
- Step buttons 1-8 select parts in Performance mode

## Menu Structure

### Browse Menu (Jog Click)
- **Browse Patches** (in Patch mode) - Navigate by bank, select patches
- **Browse Performances** (in Performance mode) - Navigate performance banks

### Edit Menu - Patch Mode (Menu Button)
- **Common** - Patch-level settings (level, pan, portamento, bend range)
- **Tone 1-4** - Individual tone editing
- **Effects** - Reverb and chorus settings
- **Settings** - Octave, local control, MIDI monitor, SysEx RX

### Edit Menu - Performance Mode (Menu Button)
- **Expansion Card** - Select which expansion is loaded for Card patches (64-127)
- **Common** - Performance common settings (reverb, chorus, key mode)
- **Part X Edit** - Edit the currently selected part (see Part Submenu below)
- **All Parts** - Submenu to access any part directly
- **Save** - Save performance to Internal slots 1-16
- **Settings** - Octave, local control, MIDI monitor, SysEx RX

### Tone Submenu (Patch Mode)
- **Enable** - Tone on/off
- **Wave Group** - INT-A, INT-B, PCM, EXP-A/B/C/D
- **Wave Number** - 0-255
- **Level** - Tone volume
- **Filter (TVF)** - Cutoff, resonance, envelope, key follow
- **Amp (TVA)** - Level, velocity sense, envelope
- **Pitch** - Coarse, fine, pitch envelope
- **LFO 1/2** - Rate, delay, fade, modulation depths
- **Output/FX** - Dry level, reverb/chorus sends

### Part Submenu (Performance Mode)
- **Patch** - Select patch for this part by bank
- **Internal** - Toggle internal sound on/off
- **Level** - Part volume (0-127)
- **Pan** - Part pan (0-127, 64=center)
- **Coarse Tune** - Transpose in semitones (±48)
- **Fine Tune** - Detune in cents (±50)
- **Reverb** - Toggle reverb send on/off
- **Chorus** - Toggle chorus send on/off
- **Key Range** - Submenu with lower and upper key limits
- **Velocity** - Submenu with sensitivity and max velocity

## Patch Mode vs Performance Mode

Toggle between modes using the **Capture button** in browser mode.

**Patch Mode**:
- Single patch on MIDI channel 1
- 192 internal patches + expansion patches
- Full editing of all 4 tones
- Browse by bank: Preset A, Preset B, Internal, Expansions
- Track buttons 1-4 select Tone 1-4 (LEDs show enabled/selected state)
- Encoder macros control tone parameters (Cutoff, Resonance, etc.)

**Performance Mode**:
- 8 parts on MIDI channels 1-8
- 48 performances (3 banks × 16)
- Step buttons 1-8 select Part 1-8
- Shift + Step toggles part mute
- Track buttons disabled (LEDs off)
- Encoder macros control part parameters (Level, Pan, Tune, etc.)
- Part 8 is the Rhythm part
- Full part editing via Edit menu (level, pan, tune, key range, velocity, patch selection)
- Save performances to 16 Internal slots
- Select expansion card for Card patches (patchnumber 64-127)

## Parameter Editing

When editing a value:
1. **Jog Click** to enter edit mode (value highlights)
2. **Jog Wheel** to adjust value
3. **Jog Click** again to confirm
4. **Back** to cancel and restore original

Quick adjust (no edit mode):
- **Left/Right arrows** adjust value immediately
- Works on VALUE and ENUM type parameters

## Supported Parameters (NVRAM Reading)

All parameters below can be read directly from the emulator and display their actual values:

### Patch Common
| Parameter | Range | Notes |
|-----------|-------|-------|
| Level | 0-127 | Patch output level |
| Pan | 0-127 | 64=Center, <64=Left, >64=Right |
| Portamento Switch | On/Off | |
| Portamento Time | 0-127 | |
| Bend Range Up | 0-12 | Semitones |
| Bend Range Down | 0-48 | Semitones |
| Reverb Level | 0-127 | |
| Reverb Time | 0-127 | |
| Chorus Level | 0-127 | |
| Chorus Rate | 0-127 | |
| Chorus Depth | 0-127 | |

### Tone Parameters (per tone)
| Parameter | Range | Notes |
|-----------|-------|-------|
| Enable (toneswitch) | On/Off | |
| Wave Group | INT-A/B, PCM, EXP-A/B/C/D | |
| Wave Number | 0-255 | |
| Level | 0-127 | Tone volume |
| Pan | 0-127 | |
| Cutoff | 0-127 | Filter frequency |
| Resonance | 0-127 | Filter resonance |
| Cutoff Key Follow | 0-127 | |
| TVF Env Depth | 0-127 | Filter envelope amount |
| TVF Env Time 1-4 | 0-127 | ADSR times |
| TVF Env Level 1-4 | 0-127 | ADSR levels |
| TVA Velocity Sense | 0-127 | |
| TVA Env Time 1-4 | 0-127 | ADSR times |
| TVA Env Level 1-3 | 0-127 | ADR levels |
| Pitch Coarse | 0-127 | 64=0, display as ±semitones |
| Pitch Fine | 0-127 | 64=0, display as ±cents |
| Pitch Env Depth | 0-127 | |
| Pitch Env Time 1-4 | 0-127 | |
| LFO 1/2 Rate | 0-127 | |
| LFO 1/2 Delay | 0-127 | |
| LFO 1/2 Fade | 0-127 | |
| LFO 1/2 Pitch Depth | 0-127 | |
| LFO 1/2 TVF Depth | 0-127 | |
| LFO 1/2 TVA Depth | 0-127 | |
| Dry Level | 0-127 | |
| Reverb Send | 0-127 | |
| Chorus Send | 0-127 | |

## Known Limitations

### Saving Patches
Patch saving to Internal bank is not yet implemented. Edits affect the temporary working patch but are not persisted to NVRAM.

### Performance Saving
Performances CAN be saved to the 16 Internal slots. Use Edit → Save in Performance mode.

### Expansion Cards in Performances
Each performance can use one expansion card at a time for Card patches (patchnumber 64-127). This matches real hardware behavior. Use Edit → Expansion Card to select which expansion is loaded. Expansions with more than 64 patches show sub-bank options (patches 1-64, 65-128, etc.).

### Parameter Reading
Parameters are read from NVRAM/SRAM when a patch or performance is loaded. If you edit via external SysEx, the display may not update until you reload.

## Troubleshooting

**No sound**:
- Check that ROM files are in the correct location
- Verify ROM version is 1.0.0 (not 1.0.1)
- Check MIDI routing if using external controller

**Parameters show 0**:
- Reload the patch (select it again)
- Some parameters may not be mapped yet

**Slow patch loading**:
- First load builds patch cache
- Subsequent loads are faster
- Expansions load on-demand when selected

## Technical Reference

### NVRAM Layout (Patch Data at 0x0d70)
```
Offset  Size  Content
0-11    12    Patch name
12      1     Reverb/Chorus config
13-15   3     Reverb params
16-19   4     Chorus params
20      1     Analog feel
21      1     Level
22      1     Pan
23      1     Bend range (down)
24      1     Flags (bend up, porta, key assign)
25      1     Portamento time
26-109  84    Tone 1
110-193 84    Tone 2
194-277 84    Tone 3
278-361 84    Tone 4
```

### Tone Structure (84 bytes each)
```
Offset  Content
0       Flags (wave group bits 0-1, tone switch bit 7)
1       Wave number
2       FXM config
3-4     Velocity range
5-22    Modulation matrix
23-30   LFO 1/2 config
31-36   LFO depths
37-38   Pitch coarse/fine
39-51   Pitch envelope
52-66   TVF (filter) params
67-80   TVA (amp) params
81-83   Dry/Reverb/Chorus sends
```
