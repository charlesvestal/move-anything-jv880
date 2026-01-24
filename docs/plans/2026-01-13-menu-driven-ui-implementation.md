# Implementation Plan: Menu-Driven UI

Based on design: `2026-01-13-menu-driven-ui-design.md`

## Phase 1: Shared Components (move-anything repo)

Work in `move-anything/src/shared/`.

### 1.1 Create MenuStack (`menu_stack.mjs`)

```javascript
export function createMenuStack() {
    const stack = [];
    return {
        push(menu) { stack.push(menu); },
        pop() { return stack.pop(); },
        current() { return stack[stack.length - 1] || null; },
        depth() { return stack.length; },
        reset() { stack.length = 0; },
        getPath() { return stack.map(m => m.title); }
    };
}
```

**Tests:**
- Push/pop maintains correct order
- `current()` returns top of stack
- `reset()` clears all

### 1.2 Create MenuItem helpers (`menu_items.mjs`)

```javascript
export const MenuItemType = {
    SUBMENU: 'submenu',
    VALUE: 'value',
    ENUM: 'enum',
    TOGGLE: 'toggle',
    ACTION: 'action',
    BACK: 'back'
};

export function createSubmenu(label, getMenu) {
    return { type: MenuItemType.SUBMENU, label, getMenu };
}

export function createValue(label, { get, set, min = 0, max = 127, step = 1, format }) {
    return { type: MenuItemType.VALUE, label, get, set, min, max, step, format };
}

export function createEnum(label, { get, set, options }) {
    return { type: MenuItemType.ENUM, label, get, set, options };
}

export function createToggle(label, { get, set }) {
    return { type: MenuItemType.TOGGLE, label, get, set };
}

export function createAction(label, onAction) {
    return { type: MenuItemType.ACTION, label, onAction };
}

export function createBack(label = '[Back]') {
    return { type: MenuItemType.BACK, label };
}

export function formatItemValue(item) {
    if (item.type === MenuItemType.VALUE) {
        const val = item.get();
        return item.format ? item.format(val) : String(val);
    }
    if (item.type === MenuItemType.ENUM) {
        return item.get();
    }
    if (item.type === MenuItemType.TOGGLE) {
        return item.get() ? 'On' : 'Off';
    }
    if (item.type === MenuItemType.SUBMENU) {
        return '>';
    }
    return '';
}
```

### 1.3 Create Menu Navigation (`menu_nav.mjs`)

```javascript
import { MenuItemType } from './menu_items.mjs';

export function createMenuState() {
    return {
        selectedIndex: 0,
        editing: false,
        editValue: null,  // temp value while editing
    };
}

export function handleMenuInput({ cc, value, items, state, stack, onBack }) {
    const isDown = value > 0;
    const item = items[state.selectedIndex];
    let needsRedraw = false;

    // Jog wheel scroll/edit
    if (cc === 14) {
        const delta = value < 64 ? 1 : -1;
        if (state.editing) {
            needsRedraw = adjustValue(item, state, delta);
        } else {
            state.selectedIndex = clamp(state.selectedIndex + delta, 0, items.length - 1);
            needsRedraw = true;
        }
    }

    // Jog click - enter/confirm
    if (cc === 3 && isDown) {
        needsRedraw = handleClick(item, state, stack);
    }

    // Left/right arrows - quick adjust
    if ((cc === 62 || cc === 63) && isDown) {
        const delta = cc === 63 ? 1 : -1;
        needsRedraw = adjustValue(item, state, delta);
    }

    // Back button
    if (cc === 52 && isDown) {
        if (state.editing) {
            // Cancel edit
            state.editing = false;
            state.editValue = null;
            needsRedraw = true;
        } else if (stack.depth() > 0) {
            stack.pop();
            state.selectedIndex = 0;
            needsRedraw = true;
        } else if (onBack) {
            onBack();
        }
    }

    return { needsRedraw };
}

function handleClick(item, state, stack) {
    if (!item) return false;

    switch (item.type) {
        case MenuItemType.SUBMENU:
            stack.push({ title: item.label, items: item.getMenu() });
            state.selectedIndex = 0;
            return true;

        case MenuItemType.VALUE:
        case MenuItemType.ENUM:
            if (state.editing) {
                // Confirm edit
                item.set(state.editValue);
                state.editing = false;
                state.editValue = null;
            } else {
                // Start edit
                state.editing = true;
                state.editValue = item.get();
            }
            return true;

        case MenuItemType.TOGGLE:
            item.set(!item.get());
            return true;

        case MenuItemType.ACTION:
            item.onAction();
            return true;

        case MenuItemType.BACK:
            if (stack.depth() > 0) {
                stack.pop();
                state.selectedIndex = 0;
            }
            return true;
    }
    return false;
}

function adjustValue(item, state, delta) {
    if (!item) return false;

    const currentVal = state.editing ? state.editValue : item.get();
    let newVal;

    if (item.type === MenuItemType.VALUE) {
        const step = item.step || 1;
        newVal = clamp(currentVal + delta * step, item.min, item.max);
    } else if (item.type === MenuItemType.ENUM) {
        const opts = item.options;
        const idx = opts.indexOf(currentVal);
        const newIdx = (idx + delta + opts.length) % opts.length;
        newVal = opts[newIdx];
    } else if (item.type === MenuItemType.TOGGLE) {
        newVal = !currentVal;
    } else {
        return false;
    }

    if (state.editing) {
        state.editValue = newVal;
    } else {
        item.set(newVal);
    }
    return true;
}

function clamp(v, min, max) {
    return Math.max(min, Math.min(max, v));
}
```

### 1.4 Extend Menu Rendering (`menu_layout.mjs`)

Add to existing file or create `menu_render.mjs`:

```javascript
export function drawHierarchicalMenu({ title, items, state }) {
    drawMenuHeader(title);

    drawMenuList({
        items,
        selectedIndex: state.selectedIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (item) => item.label,
        getValue: (item, index) => {
            const val = formatItemValue(item);
            if (state.editing && index === state.selectedIndex) {
                return `[${state.editValue}]`;  // Show editing indicator
            }
            return val;
        },
        valueAlignRight: true
    });
}
```

---

## Phase 2: Settings Migration (move-anything repo)

### 2.1 Refactor menu_settings.mjs

Convert settings to use new MenuItem types:

```javascript
import { createMenuStack, createMenuState } from '../shared/menu_stack.mjs';
import { createValue, createEnum, createToggle, createBack } from '../shared/menu_items.mjs';
import { handleMenuInput } from '../shared/menu_nav.mjs';
import { drawHierarchicalMenu } from '../shared/menu_layout.mjs';

const VELOCITY_CURVES = ['linear', 'soft', 'hard', 'full'];
const PAD_LAYOUTS = ['chromatic', 'fourth'];
const CLOCK_MODES = ['off', 'internal', 'external'];

function getSettingsItems() {
    return [
        createEnum('Velocity', {
            get: () => host_get_setting('velocity_curve') || 'linear',
            set: (v) => { host_set_setting('velocity_curve', v); host_save_settings(); },
            options: VELOCITY_CURVES
        }),
        createToggle('Aftertouch', {
            get: () => host_get_setting('aftertouch_enabled') ?? 1,
            set: (v) => { host_set_setting('aftertouch_enabled', v ? 1 : 0); host_save_settings(); }
        }),
        createValue('AT Deadzone', {
            get: () => host_get_setting('aftertouch_deadzone') ?? 0,
            set: (v) => { host_set_setting('aftertouch_deadzone', v); host_save_settings(); },
            min: 0, max: 50, step: 5
        }),
        createEnum('Pad Layout', {
            get: () => host_get_setting('pad_layout') || 'chromatic',
            set: (v) => { host_set_setting('pad_layout', v); host_save_settings(); },
            options: PAD_LAYOUTS
        }),
        createEnum('MIDI Clock', {
            get: () => host_get_setting('clock_mode') || 'internal',
            set: (v) => { host_set_setting('clock_mode', v); host_save_settings(); },
            options: CLOCK_MODES
        }),
        createValue('Tempo BPM', {
            get: () => host_get_setting('tempo_bpm') ?? 120,
            set: (v) => { host_set_setting('tempo_bpm', v); host_save_settings(); },
            min: 20, max: 300, step: 5
        }),
        createBack()
    ];
}
```

### 2.2 Update menu_ui.js

Integrate new settings rendering and input handling.

### 2.3 Test

- Jog scrolls settings
- Click enters edit mode (value shows brackets)
- Jog/arrows change value while editing
- Click confirms, Back cancels
- Back from settings returns to main menu

---

## Phase 3: Mini-JV UI (this repo)

### 3.1 Create menu structure (`src/ui_menu.mjs`)

Define all menus using shared components:

```javascript
// Top-level menu
function getMainMenu() {
    return [
        createSubmenu('Browse Patches', () => getPatchBanksMenu()),
        createSubmenu('Browse Performances', () => getPerfBanksMenu()),
        createSubmenu('Settings', () => getSettingsMenu()),
    ];
}

// Patch banks
function getPatchBanksMenu() {
    const banks = getBankList();
    return [
        ...banks.map(bank => createSubmenu(bank.name, () => getPatchListMenu(bank.id))),
        createBack()
    ];
}

// Patches in a bank
function getPatchListMenu(bankId) {
    const patches = getPatchesInBank(bankId);
    return [
        ...patches.map(patch => createAction(patch.name, () => loadPatch(patch.id))),
        createBack()
    ];
}

// Edit patch menu
function getEditPatchMenu() {
    return [
        createSubmenu('Common', () => getPatchCommonMenu()),
        ...getTones().map((tone, i) =>
            createSubmenu(`Tone ${i+1}: ${tone.name}`, () => getToneMenu(i))
        ),
        createBack()
    ];
}

// Tone submenu
function getToneMenu(toneIndex) {
    return [
        createValue('Wave', { get: () => getToneWave(toneIndex), set: (v) => setToneWave(toneIndex, v), ... }),
        createValue('Level', { ... }),
        createSubmenu('Filter', () => getToneFilterMenu(toneIndex)),
        createSubmenu('Amp', () => getToneAmpMenu(toneIndex)),
        createSubmenu('Pitch', () => getTonePitchMenu(toneIndex)),
        createSubmenu('LFO', () => getToneLFOMenu(toneIndex)),
        createBack()
    ];
}
```

### 3.2 Create browser mode (`src/ui_browser.mjs`)

Large display for playing:

```javascript
function drawBrowser() {
    clear_screen();

    // Mode
    print(2, 2, mode === 'performance' ? 'PERFORMANCE' : 'PATCH', 1);

    // Large patch name (could use bigger font or multiple lines)
    const name = getCurrentPatchName();
    print(2, 18, name, 1);

    // Context
    const context = mode === 'performance'
        ? `Part ${selectedPart + 1}, Tone ${selectedTone + 1}`
        : `Tone ${selectedTone + 1}`;
    print(2, 36, context, 1);

    // Location
    print(2, 50, `${bankName}  ${currentPreset + 1}/${totalPatches}`, 1);
}
```

### 3.3 Wire up controls (`src/ui.js`)

Main UI state machine:

```javascript
let uiMode = 'browser';  // 'browser' | 'menu'
const menuStack = createMenuStack();
const menuState = createMenuState();

function handleCC(cc, value) {
    if (cc === CC_MENU && value > 0) {
        if (uiMode === 'browser') {
            uiMode = 'menu';
            menuStack.reset();
            menuStack.push({ title: 'Mini-JV', items: getMainMenu() });
        }
        return;
    }

    if (uiMode === 'menu') {
        const current = menuStack.current();
        handleMenuInput({
            cc, value,
            items: current.items,
            state: menuState,
            stack: menuStack,
            onBack: () => { uiMode = 'browser'; }
        });
        return;
    }

    // Browser mode controls
    if (cc === CC_JOG) {
        scrollPatch(decodeDelta(value));
    }
    // ... track buttons, step buttons, knobs
}
```

### 3.4 Implement LED feedback

```javascript
function updateLEDs() {
    // Tracks - tones
    for (let i = 0; i < 4; i++) {
        const enabled = toneEnabled[i];
        const selected = selectedTone === i;
        let color;
        if (!enabled && !selected) color = LED_GREY;
        else if (!enabled && selected) color = LED_WHITE;
        else if (enabled && !selected) color = LED_DIM_GREEN;
        else color = LED_BRIGHT_GREEN;
        setTrackLED(i, color);
    }

    // Steps - parts (performance mode only)
    for (let i = 0; i < 8; i++) {
        if (mode !== 'performance') {
            setStepLED(i, LED_OFF);
            continue;
        }
        const enabled = partEnabled[i];
        const selected = selectedPart === i;
        // ... same logic
    }
}
```

### 3.5 Connect to DSP

Wire menu actions to SysEx commands:

```javascript
function setToneCutoff(toneIndex, value) {
    sendSysEx(buildToneParam(toneIndex, 'cutofffrequency', value));
}
```

---

## Testing Checklist

### Phase 1
- [ ] MenuStack push/pop/current/reset work correctly
- [ ] MenuItem creators produce correct structures
- [ ] handleMenuInput navigates correctly
- [ ] Value editing: click enters, jog changes, click confirms, back cancels
- [ ] Arrow keys adjust values without entering edit mode

### Phase 2
- [ ] Settings displays with new renderer
- [ ] All settings editable via click-to-edit
- [ ] Back returns to main menu
- [ ] Values persist after editing

### Phase 3
- [ ] Top menu shows Browse Patches/Performances/Settings
- [ ] Can navigate to patch, load it, return to browser
- [ ] Edit menu shows correct hierarchy for patch/performance
- [ ] Track buttons select tones, update LEDs
- [ ] Step buttons select parts (performance mode), update LEDs
- [ ] Knobs adjust selected tone parameters
- [ ] SHIFT+Track mutes/unmutes
- [ ] LED colors match 5-state scheme
