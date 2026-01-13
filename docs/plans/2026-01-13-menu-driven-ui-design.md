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

## Shared Menu Components (move-anything repo)

Before implementing the JV-880 UI, we'll build shared menu components in move-anything that can be reused across modules.

### MenuStack (`src/shared/menu_stack.mjs`)

Manages hierarchical navigation history:

```javascript
import { createMenuStack } from '../../shared/menu_stack.mjs';

const stack = createMenuStack();
stack.push({ id: 'part1', title: 'Part 1', items: [...] });
stack.pop();        // returns to previous menu
stack.current();    // get current menu state
stack.depth();      // how deep in hierarchy
stack.reset();      // clear to root
```

### MenuItem Types (`src/shared/menu_items.mjs`)

Standardized menu item definitions:

```javascript
// Submenu - click enters child menu
{ type: 'submenu', label: 'Filter', getMenu: () => filterMenuItems }

// Value - click to edit, jog/arrows change, click confirms
{ type: 'value', label: 'Cutoff', get: () => value, set: (v) => {}, min: 0, max: 127 }

// Enum - click to edit, jog/arrows cycle options
{ type: 'enum', label: 'Wave', get: () => value, set: (v) => {}, options: ['saw', 'square'] }

// Toggle - click toggles, or arrows change
{ type: 'toggle', label: 'Enable', get: () => bool, set: (v) => {} }

// Action - click executes callback
{ type: 'action', label: 'Initialize', onAction: () => {} }

// Back - returns to parent menu
{ type: 'back', label: '[Back]' }
```

### Menu Navigation (`src/shared/menu_nav.mjs`)

Handles input for hierarchical menus:

```javascript
import { handleMenuInput } from '../../shared/menu_nav.mjs';

// Returns: { needsRedraw, exitMenu, valueChanged }
const result = handleMenuInput({
    cc,
    value,
    stack,           // MenuStack instance
    selectedIndex,
    editingValue,    // true if in value-edit mode
});
```

**Input behavior:**
- **Jog wheel**: Scroll list (navigate mode) or change value (edit mode)
- **Jog click**: Enter submenu, start editing value, confirm edit, or execute action
- **Left/Right arrows**: Change value without entering edit mode (quick adjust)
- **Back button**: Exit edit mode (cancel), or pop menu stack

**Edit mode visual:**
- Selected item shows inverted highlight when navigating
- Editing item shows different indicator (e.g., blinking or bracket around value)

### Menu Renderer (`src/shared/menu_render.mjs`)

Extended drawing functions:

```javascript
import { drawHierarchicalMenu } from '../../shared/menu_render.mjs';

drawHierarchicalMenu({
    stack,
    selectedIndex,
    editingValue,
    editingIndex,
});
```

Shows breadcrumb or title from stack, renders items with appropriate value display.

## Implementation Order

### Phase 1: Shared Components (move-anything repo)

1. Create `menu_stack.mjs` - navigation history management
2. Create `menu_items.mjs` - item type definitions and helpers
3. Create `menu_nav.mjs` - input handling with edit mode
4. Extend `menu_layout.mjs` or create `menu_render.mjs` - hierarchical rendering

### Phase 2: Settings Migration (move-anything repo)

1. Refactor `menu_settings.mjs` to use new shared components
2. Convert settings items to MenuItem types
3. Add click-to-edit behavior (replacing arrow-only editing)
4. Test navigation and value editing

### Phase 3: JV-880 UI (this repo)

1. Import shared components
2. Build menu structure (Browse, Edit, Settings)
3. Implement browser mode (large patch display)
4. Wire up physical controls (tracks, steps, knobs)
5. Implement LED feedback

## Implementation Notes

- Menu state persists when exiting to browser (re-entering returns to same position)
- Knob changes apply immediately (no confirmation needed)
- Menu value changes require click to confirm, BACK to cancel
- Left/Right arrows provide quick value adjustment without entering edit mode

## Migration

This is a complete UI rewrite. The current step-button paradigm will be replaced entirely.
