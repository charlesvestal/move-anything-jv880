# JV-880 Menu-Driven UI Redesign

## Overview

Replace the current step-button-driven UI with a hierarchical menu system that matches Move's navigation paradigm. Knobs remain dedicated to performance controls rather than menu editing.

## Goals

- Reduce button presses needed to access part/tone editing
- Clear visual feedback for what's selected and muted
- Consistent navigation via jog wheel and menu button
- Use shared menu components from move-anything repo

## Modes

Mode is determined by what you load:

- **Patch mode** - Single patch with 4 tones
- **Performance mode** - 8 parts, each containing a patch with 4 tones

## Screen Layout

### Browser Screen (Playing)

```
┌────────────────────────┐
│ PATCH                  │  ← mode
│ A-21 Acoustic Piano    │  ← large name (what you're playing)
│ Tone 2 selected        │  ← current selection context
│ Bank: Internal  124/256│  ← location info
└────────────────────────┘
```

Jog scrolls patches/performances. Screen updates live.

### Menu Screen

Standard hierarchical menu. Jog scrolls, click enters/edits, BACK exits.

## Menu Structure

### Top Level

```
JV-880
> Browse Patches
  Browse Performances
  Settings
```

### Browse Patches

```
Browse Patches
> Internal
  Expansion: Keyboards
  Expansion: Orchestra
  [Back]
```

Inside a bank:
```
Internal
> 001 Acoustic Piano      ← click loads & exits to browser
  002 Bright Piano
  003 ...
  [Back]
```

### Browse Performances

Same structure as patches, organized by bank.

### Settings

```
Settings
> Local Sound: On
  SysEx Rx: On
  [Back]
```

Minimal - only global settings that aren't patch/performance parameters.

## Edit Menu (Patch Mode)

Press MENU from browser when patch is loaded:

```
Edit Patch: Acoustic Piano
> Common
  Tone 1: Piano Layer
  Tone 2: Strings
  Tone 3: [Off]
  Tone 4: [Off]
  [Back]
```

### Common Submenu

```
Common
> Level: 100
  Pan: Center
  Reverb Type: Hall
  Reverb Level: 80
  Chorus Type: Chorus 1
  Chorus Level: 40
  [Back]
```

### Tone Submenu

```
Tone 1: Piano Layer
> Wave: A-Piano 1
  Level: 100
  Filter
  Amp
  Pitch
  LFO
  [Back]
```

### Parameter Submenus (e.g., Filter)

```
Filter
> Cutoff: 64
  Resonance: 20
  Env Depth: 30
  Key Follow: +50
  [Back]
```

## Edit Menu (Performance Mode)

Press MENU from browser when performance is loaded:

```
Edit Performance: Jazz Quartet
> Common
  Part 1: Piano
  Part 2: Bass
  Part 3: Drums
  Part 4: Strings
  Part 5: [Off]
  Part 6: [Off]
  Part 7: [Off]
  Part 8: [Off]
  [Back]
```

### Part Submenu

```
Part 1: Piano
> Patch: A-21 Piano       ← jog to change patch assignment
  Level: 100
  Pan: Center
  MIDI Channel: 1
  Key Range: C-1 to G9
  Tone 1: Piano Layer     ← drill into patch's tones
  Tone 2: Strings
  Tone 3: [Off]
  Tone 4: [Off]
  [Back]
```

### Part's Tone Submenu

Same structure as Patch mode tones. Full hierarchy:
Performance → Part → Tone → Filter → Cutoff

## Physical Controls

### Jog Wheel

- **Browser mode**: Scroll patches/performances
- **Menu mode**: Navigate up/down, click to enter/edit, click to confirm

### Track Buttons (4 buttons, left side)

- **Patch mode**: Select Tone 1-4
- **Performance mode**: Select Tone 1-4 of currently selected part

### Step Buttons 1-8

- **Patch mode**: Unused (off)
- **Performance mode**: Select Part 1-8

### Step Buttons 9-16

Unused (off) - reserved for future features.

### Knobs 1-8

Always control the selected tone's parameters:

| Knob | Parameter |
|------|-----------|
| E1 | Cutoff |
| E2 | Resonance |
| E3 | Attack |
| E4 | Release |
| E5 | LFO Rate |
| E6 | LFO Depth |
| E7 | FX Send |
| E8 | Level |

### Navigation Buttons

- **MENU**: Enter edit menu from browser
- **BACK**: Go up one level in menu, or exit to browser

### Modifier Combinations

- **SHIFT + Track**: Mute/unmute that tone

## LED Feedback

Five-state LED scheme:

| State | Color | Meaning |
|-------|-------|---------|
| Off | — | Not applicable |
| Grey | ⚫ | Muted, not selected |
| White | ⚪ | Muted, selected |
| Dim green | 🟢 | Enabled, not selected |
| Bright green | 🟢 | Enabled, selected |

### Examples

**Patch mode, Tone 2 selected, Tone 3 muted:**
```
Tracks: [dim] [BRIGHT] [grey] [dim]
          T1     T2      T3    T4
Steps 1-8: [off] [off] [off] [off] [off] [off] [off] [off]
```

**Performance mode, Part 1 selected, Part 3 muted, Parts 5-8 unused:**
```
Steps 1-8: [BRIGHT] [dim] [grey] [dim] [off] [off] [off] [off]
              P1      P2    P3    P4    P5    P6    P7    P8
```

### Button LEDs

- **MENU**: Bright when in edit menu
- **MUTE**: Red when selected tone/part is muted

## Implementation Notes

- Use shared menu components from move-anything repo
- Menu state persists when exiting to browser (re-entering returns to same position)
- Knob changes apply immediately (no confirmation needed)
- Menu value changes require click to confirm, BACK to cancel

## Migration

This is a complete UI rewrite. The current step-button paradigm will be replaced entirely.
