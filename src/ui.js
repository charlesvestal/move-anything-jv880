/*
 * JV-880 Module UI - LCD Native Mode
 *
 * Direct mapping of Move controls to JV-880 front panel buttons.
 * Display shows JV-880 LCD content directly.
 */

import * as std from 'std';

import {
    MoveMainKnob,
    MoveLeft, MoveRight,
    MoveUp, MoveDown,
    MoveShift, MoveMenu, MoveBack,
    MoveCapture,
    MoveSteps,
    MoveStep1, MoveStep2, MoveStep3, MoveStep4,
    MoveStep9, MoveStep10, MoveStep11, MoveStep12,
    WhiteLedBright, White, Bright
} from '../../shared/constants.mjs';

/* Bright = Bright Orange (color 3) */
const LedOn = Bright;
const LedOff = White;

/* === Constants === */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const LCD_LINE_HEIGHT = 14;

/* JV-880 MCU button constants (from mcu.h) */
const MCU_BUTTON_CURSOR_L = 0;
const MCU_BUTTON_CURSOR_R = 1;
const MCU_BUTTON_TONE_SELECT = 2;
const MCU_BUTTON_TONE_SW1 = 3;
const MCU_BUTTON_TONE_SW2 = 4;
const MCU_BUTTON_TONE_SW3 = 5;
const MCU_BUTTON_TONE_SW4 = 6;
const MCU_BUTTON_UTILITY = 7;
const MCU_BUTTON_PATCH_PERFORM = 8;
const MCU_BUTTON_EDIT = 9;
const MCU_BUTTON_SYSTEM = 10;
const MCU_BUTTON_RHYTHM = 11;

/* Move control mappings */
const CC_JOG = MoveMainKnob;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_SHIFT = MoveShift;
const CC_MENU = MoveMenu;
const CC_BACK = MoveBack;
const CC_CAPTURE = MoveCapture;

/* === State === */
let lcdLine0 = '';
let lcdLine1 = '';
let expansionName = 'Internal';
let expansionCount = 0;
let loadingStatus = 'Initializing...';
let loadingComplete = false;
let shiftHeld = false;

/* Track active buttons for display */
let activeButton = '';
let activeButtonUntil = 0;

/* Pending button releases */
let pendingReleases = [];

let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;

/* LED state cache (to avoid unnecessary updates) */
let lastLedState = '';
let debugLine1 = '';
let debugLine2 = '';

/* === LED Control === */
function setLed(control, brightness) {
    move_midi_internal_send([0x0b, 0xB0, control, brightness]);
}

function setStepLed(step, color) {
    /* Steps are RGB LEDs - use note message on channel 0 for solid */
    move_midi_internal_send([0x09, 0x90, step, color]);
}

function initLeds() {
    /* Light up navigation LEDs (white LEDs) */
    setLed(MoveMenu, WhiteLedBright);
    setLed(MoveCapture, WhiteLedBright);
    setLed(MoveLeft, WhiteLedBright);
    setLed(MoveRight, WhiteLedBright);
    setLed(MoveUp, WhiteLedBright);
    setLed(MoveDown, WhiteLedBright);

    /* Initialize step LEDs - will be updated by syncStepLeds */
    setStepLed(MoveStep1, White);
    setStepLed(MoveStep2, White);
    setStepLed(MoveStep3, White);
    setStepLed(MoveStep4, White);
    setStepLed(MoveStep9, White);
    setStepLed(MoveStep10, White);
    setStepLed(MoveStep11, White);
    setStepLed(MoveStep12, White);
}

function syncStepLeds() {
    /* Get LED state from DSP: edit,system,rhythm,utility,patch,unused,tone1,tone2,tone3,tone4 */
    const ledState = host_module_get_param('led_state');
    if (!ledState) return;

    /* Show raw LED debug on display for debugging */
    const ledDebug = host_module_get_param('led_debug');
    if (ledDebug && ledDebug !== lastLedState.split('|')[0]) {
        /* Store debug for display - will show on lines 1 and 2 */
        debugLine1 = ledDebug.substring(0, 20);
        debugLine2 = ledDebug.substring(20);
        activeButtonUntil = nowMs() + 5000;
        needsRedraw = true;
    }

    if (ledState === lastLedState.split('|')[1]) return;
    lastLedState = ledDebug + '|' + ledState;
    const leds = ledState.split(',').map(v => parseInt(v));

    /* Corrected mapping based on testing:
     * Step 1 (Edit) = 0x08 = leds[2] (DSP "rhythm")
     * Step 2 (System) = 0x04 = leds[1] (DSP "system")
     * Step 3 (Rhythm) = 0x02 = leds[0] (DSP "edit")
     * Step 4 (Utility) = 0x01 = leds[4] (DSP "patch") */
    setStepLed(MoveStep1, leds[2] ? LedOn : LedOff);  /* Edit */
    setStepLed(MoveStep2, leds[1] ? LedOn : LedOff);  /* System */
    setStepLed(MoveStep3, leds[0] ? LedOn : LedOff);  /* Rhythm */
    setStepLed(MoveStep4, leds[4] ? LedOn : LedOff);  /* Utility */

    /* Tones: reversed mapping (both buttons and LEDs reversed together) */
    setStepLed(MoveStep9, leds[9] ? LedOn : LedOff);   /* Tone 1 */
    setStepLed(MoveStep10, leds[8] ? LedOn : LedOff);  /* Tone 2 */
    setStepLed(MoveStep11, leds[7] ? LedOn : LedOff);  /* Tone 3 */
    setStepLed(MoveStep12, leds[6] ? LedOn : LedOff);  /* Tone 4 */

    /* Menu LED brightness based on patch vs performance mode */
    const perfMode = host_module_get_param('performance_mode');
    const inPatchMode = perfMode === '0';
    setLed(MoveMenu, inPatchMode ? WhiteLedBright : 0x40);
}

/* === Utility === */
function nowMs() {
    return Date.now ? Date.now() : new Date().getTime();
}

function setActivity(text) {
    activeButton = text;
    activeButtonUntil = nowMs() + 2000;
    needsRedraw = true;
}

function pressButton(button) {
    host_module_set_param('button_press', String(button));
}

function releaseButton(button) {
    host_module_set_param('button_release', String(button));
}

function tapButton(button, label) {
    pressButton(button);
    pendingReleases.push({ button, releaseAt: nowMs() + 80 });
    setActivity(label);
}

function processPendingReleases() {
    const now = nowMs();
    const stillPending = [];
    for (const item of pendingReleases) {
        if (now >= item.releaseAt) {
            releaseButton(item.button);
        } else {
            stillPending.push(item);
        }
    }
    pendingReleases = stillPending;
}

/* === Display === */
function drawLoadingScreen() {
    clear_screen();
    print(1, 1, 'JV-880', 1);
    print(1, 20, 'Loading...', 1);
    print(1, 35, loadingStatus, 1);
}

function drawUI() {
    if (!loadingComplete) {
        drawLoadingScreen();
        return;
    }

    clear_screen();

    const y1 = 1;
    const y2 = 16;
    const y3 = 31;
    const y4 = 46;

    /* Lines 1-2: Debug LED info or normal display */
    if (activeButtonUntil > nowMs() && debugLine1) {
        print(1, y1, debugLine1, 1);
        print(1, y2, debugLine2, 1);
    } else {
        print(1, y1, activeButton, 1);
        print(1, y2, `Card: ${expansionName}`, 1);
    }

    /* Lines 3-4: JV-880 LCD content (inverted like real LCD) */
    fill_rect(0, y3 - 2, SCREEN_WIDTH, LCD_LINE_HEIGHT, 1);
    print(1, y3, lcdLine0, 0);
    fill_rect(0, y4 - 2, SCREEN_WIDTH, LCD_LINE_HEIGHT, 1);
    print(1, y4, lcdLine1, 0);

    needsRedraw = false;
}

/* === Input Handling === */
function handleJog(value) {
    if (value >= 1 && value <= 63) {
        host_module_set_param('encoder', '1');
        setActivity('Data +');
    } else if (value >= 65 && value <= 127) {
        host_module_set_param('encoder', '0');
        setActivity('Data -');
    }
    return true;
}

function handleCC(cc, value) {
    if (cc === CC_SHIFT) {
        shiftHeld = value > 0;
        return false;
    }

    if (cc === CC_MENU && value > 0) {
        if (shiftHeld) {
            if (expansionCount > 0) {
                host_module_set_param('next_expansion', '1');
                setActivity('Next Card');
            } else {
                setActivity('No Cards');
            }
        } else {
            tapButton(MCU_BUTTON_PATCH_PERFORM, 'Patch/Perform');
        }
        return true;
    }

    if (cc === CC_BACK && value > 0) {
        setActivity('Back');
        return true;
    }

    if (cc === CC_LEFT && value > 0) {
        tapButton(MCU_BUTTON_CURSOR_L, 'Cursor L');
        return true;
    }

    if (cc === CC_RIGHT && value > 0) {
        tapButton(MCU_BUTTON_CURSOR_R, 'Cursor R');
        return true;
    }

    if (cc === CC_CAPTURE && value > 0) {
        tapButton(MCU_BUTTON_TONE_SELECT, 'Tone Select');
        return true;
    }

    if (cc === CC_JOG) {
        return handleJog(value);
    }

    return false;
}

function handleStep(stepIndex) {
    switch (stepIndex) {
        /* Steps 1-4: EDIT, SYSTEM, RHYTHM, UTILITY */
        case 0:
            tapButton(MCU_BUTTON_EDIT, 'Edit');
            return true;
        case 1:
            tapButton(MCU_BUTTON_SYSTEM, 'System');
            return true;
        case 2:
            tapButton(MCU_BUTTON_RHYTHM, 'Rhythm');
            return true;
        case 3:
            tapButton(MCU_BUTTON_UTILITY, 'Utility');
            return true;
        /* Steps 9-12: TONE 1-4 */
        case 8:
            tapButton(MCU_BUTTON_TONE_SW1, 'Tone 1');
            return true;
        case 9:
            tapButton(MCU_BUTTON_TONE_SW2, 'Tone 2');
            return true;
        case 10:
            tapButton(MCU_BUTTON_TONE_SW3, 'Tone 3');
            return true;
        case 11:
            tapButton(MCU_BUTTON_TONE_SW4, 'Tone 4');
            return true;
        default:
            return false;
    }
}

function handleNoteOn(note, velocity, channel, source) {
    if (MoveSteps.includes(note)) {
        const stepIndex = note - MoveSteps[0];
        return handleStep(stepIndex);
    }
    return false;
}

/* === DSP Sync === */
function updateFromDSP() {
    const complete = host_module_get_param('loading_complete');
    if (complete) {
        const wasLoading = !loadingComplete;
        loadingComplete = complete === '1';
        if (wasLoading && loadingComplete) {
            needsRedraw = true;
            initLeds();
        }
    }

    const status = host_module_get_param('loading_status');
    if (status && status !== loadingStatus) {
        loadingStatus = status;
        if (!loadingComplete) needsRedraw = true;
    }

    const expName = host_module_get_param('expansion_name');
    if (expName && expName !== expansionName) {
        expansionName = expName;
        needsRedraw = true;
    }

    const expCount = host_module_get_param('expansion_count');
    if (expCount) {
        const c = parseInt(expCount);
        if (c !== expansionCount) {
            expansionCount = c;
        }
    }

    const line0 = host_module_get_param('lcd_line0');
    const line1 = host_module_get_param('lcd_line1');
    if (line0 !== null && line0 !== lcdLine0) {
        lcdLine0 = line0;
        needsRedraw = true;
    }
    if (line1 !== null && line1 !== lcdLine1) {
        lcdLine1 = line1;
        needsRedraw = true;
    }

}

/* === Required module UI callbacks === */

globalThis.init = function() {
    console.log('JV880 UI initializing...');
    needsRedraw = true;
    updateFromDSP();
    initLeds();
};

globalThis.tick = function() {
    processPendingReleases();
    updateFromDSP();
    syncStepLeds();

    tickCount++;
    if (needsRedraw || tickCount >= REDRAW_INTERVAL) {
        drawUI();
        tickCount = 0;
        needsRedraw = false;
    }
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    if (status === 0xB0) {
        if (handleCC(data[1], data[2])) return;
    }

    if (status === 0x90 && data[2] > 0) {
        handleNoteOn(data[1], data[2], data[0] & 0x0F, 'internal');
    }
};

globalThis.onMidiMessageExternal = function(data) {
    /* Pass through external MIDI to the engine */
};
