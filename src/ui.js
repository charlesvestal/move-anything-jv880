/*
 * JV-880 Module UI
 *
 * Provides UI for the Roland JV-880 emulator module.
 * Handles preset selection via MIDI program change and octave transpose.
 * Supports multiple expansion ROMs with unified patch list.
 */

import {
    MoveMainKnob, MoveMainButton,
    MoveLeft, MoveRight, MoveUp, MoveDown,
    MoveShift, MovePads
} from '../../shared/constants.mjs';

import { isCapacitiveTouchMessage } from '../../shared/input_filter.mjs';

/* State */
let currentPreset = 0;
let totalPatches = 128;
let octaveTranspose = 0;
let bankName = "JV-880";
let bankCount = 0;
let lcdLine0 = "";
let lcdLine1 = "";
let loadingStatus = "Initializing...";
let loadingComplete = false;
let shiftHeld = false;

/* Alias constants for clarity */
const CC_JOG_WHEEL = MoveMainKnob;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_PLUS = MoveUp;
const CC_MINUS = MoveDown;
const CC_SHIFT = MoveShift;

let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* Draw loading screen */
function drawLoadingScreen() {
    clear_screen();
    print(1, 1, "JV-880", 1);
    print(1, 20, "Loading...", 1);
    print(1, 35, loadingStatus, 1);
}

/* Draw the main UI */
function drawUI() {
    if (!loadingComplete) {
        drawLoadingScreen();
        return;
    }

    clear_screen();

    /* Bank name (top line) */
    print(1, 1, bankName, 1);

    /* LCD lines from emulator */
    print(1, 14, lcdLine0, 1);
    print(1, 26, lcdLine1, 1);

    /* Separator */
    fill_rect(0, 38, SCREEN_WIDTH, 1, 1);

    /* Status - show patch number and total */
    const octStr = octaveTranspose >= 0 ? `+${octaveTranspose}` : `${octaveTranspose}`;
    print(1, 42, `Oct:${octStr} ${currentPreset + 1}/${totalPatches}`, 1);

    /* Help */
    print(1, 54, "+/-:Oct Shft<>:Bank", 1);

    needsRedraw = false;
}

/* Send program change to JV-880 */
function setPreset(index) {
    /* Wrap around */
    if (index < 0) index = totalPatches - 1;
    if (index >= totalPatches) index = 0;

    currentPreset = index;

    /* Send program change to DSP */
    host_module_set_param("program_change", String(currentPreset));

    needsRedraw = true;
    console.log(`JV880: Preset changed to ${currentPreset}`);
}

/* Jump to next/previous bank */
function jumpBank(direction) {
    if (direction > 0) {
        host_module_set_param("next_bank", "1");
    } else {
        host_module_set_param("prev_bank", "1");
    }
    needsRedraw = true;
    console.log(`JV880: Jump bank ${direction > 0 ? 'next' : 'prev'}`);
}

/* Change octave */
function setOctave(delta) {
    octaveTranspose += delta;
    if (octaveTranspose < -4) octaveTranspose = -4;
    if (octaveTranspose > 4) octaveTranspose = 4;

    /* Sync with DSP */
    host_module_set_param("octave_transpose", String(octaveTranspose));

    needsRedraw = true;
    console.log(`JV880: Octave transpose: ${octaveTranspose}`);
}

/* Note: Tempo and clock are now host settings, controlled via host menu */

/* Handle CC messages */
function handleCC(cc, value) {
    /* Track shift state */
    if (cc === CC_SHIFT) {
        shiftHeld = value > 0;
        return false;  /* Don't consume - let other handlers see it */
    }

    /* Shift + Left/Right = Bank jump */
    if (shiftHeld) {
        if (cc === CC_LEFT && value > 0) {
            jumpBank(-1);
            return true;
        }
        if (cc === CC_RIGHT && value > 0) {
            jumpBank(1);
            return true;
        }
    }

    /* Preset navigation with left/right buttons */
    if (cc === CC_LEFT && value > 0) {
        setPreset(currentPreset - 1);
        return true;
    }
    if (cc === CC_RIGHT && value > 0) {
        setPreset(currentPreset + 1);
        return true;
    }

    /* Octave with up/down (plus/minus) */
    if (cc === CC_PLUS && value > 0) {
        setOctave(1);
        return true;
    }
    if (cc === CC_MINUS && value > 0) {
        setOctave(-1);
        return true;
    }

    /* Jog wheel for preset selection */
    if (cc === CC_JOG_WHEEL) {
        if (value === 1) {
            setPreset(currentPreset + 1);
        } else if (value === 127 || value === 65) {
            setPreset(currentPreset - 1);
        }
        return true;
    }

    return false;
}

/* Update state from DSP */
function updateFromDSP() {
    /* Check loading status */
    const complete = host_module_get_param("loading_complete");
    if (complete) {
        const wasLoading = !loadingComplete;
        loadingComplete = complete === "1";
        if (wasLoading && loadingComplete) {
            needsRedraw = true;
        }
    }

    /* Get loading status message */
    const status = host_module_get_param("loading_status");
    if (status && status !== loadingStatus) {
        loadingStatus = status;
        if (!loadingComplete) needsRedraw = true;
    }

    /* Get total patches */
    const total = host_module_get_param("total_patches");
    if (total) {
        const n = parseInt(total);
        if (n > 0 && n !== totalPatches) {
            totalPatches = n;
            needsRedraw = true;
        }
    }

    /* Get bank count */
    const banks = host_module_get_param("bank_count");
    if (banks) {
        const b = parseInt(banks);
        if (b > 0 && b !== bankCount) {
            bankCount = b;
            needsRedraw = true;
        }
    }

    /* Get current bank name */
    const bank = host_module_get_param("bank_name");
    if (bank && bank !== bankName) {
        bankName = bank;
        needsRedraw = true;
    }

    /* Get current patch index */
    const patch = host_module_get_param("current_patch");
    if (patch) {
        const p = parseInt(patch);
        if (p >= 0 && p !== currentPreset) {
            currentPreset = p;
            needsRedraw = true;
        }
    }

    /* Get LCD lines from emulator */
    const line0 = host_module_get_param("lcd_line0");
    const line1 = host_module_get_param("lcd_line1");
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
    console.log("JV880 UI initializing...");
    needsRedraw = true;

    /* Initial state from DSP */
    updateFromDSP();
};

globalThis.tick = function() {
    /* Update state from DSP */
    updateFromDSP();

    /* Rate-limited redraw */
    tickCount++;
    if (needsRedraw || tickCount >= REDRAW_INTERVAL) {
        drawUI();
        tickCount = 0;
        needsRedraw = false;
    }
};

globalThis.onMidiMessageInternal = function(data) {
    /* Filter capacitive touch from knobs */
    if (isCapacitiveTouchMessage(data)) return;

    const status = data[0] & 0xF0;

    if (status === 0xB0) {
        /* CC - handle UI controls */
        if (handleCC(data[1], data[2])) {
            return; /* Consumed by UI */
        }
    }

    /* All other MIDI is routed to DSP by host */
};

globalThis.onMidiMessageExternal = function(data) {
    /* External MIDI goes directly to DSP via host */
};
