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
const LCD_LINE_HEIGHT = 8;  /* 6 pixels + 2 spacing */
const LCD_CHAR_WIDTH = 5;   /* 4 pixels + 1 spacing (24*5=120, fits in 128) */
const LCD_START_X = 4;      /* Left margin: (128-120)/2 = 4 */

/* 4x6 pixel font - font4x6 by luizbills (Unlicense/Public Domain)
 * https://github.com/luizbills/font4x6
 * Each character is 3 bytes encoding 6 rows (high nibble, then low nibble per byte)
 * Characters 32-126 (printable ASCII) */
const FONT_4X6 = [
    [0x00, 0x00, 0x00], /* 32 space */
    [0x22, 0x20, 0x20], /* 33 ! */
    [0x55, 0x00, 0x00], /* 34 " */
    [0x57, 0x57, 0x50], /* 35 # */
    [0x63, 0x67, 0x20], /* 36 $ */
    [0x54, 0x21, 0x50], /* 37 % */
    [0x63, 0x65, 0x60], /* 38 & */
    [0x22, 0x00, 0x00], /* 39 ' */
    [0x21, 0x11, 0x20], /* 40 ( */
    [0x24, 0x44, 0x20], /* 41 ) */
    [0x52, 0x50, 0x00], /* 42 * */
    [0x02, 0x72, 0x00], /* 43 + */
    [0x00, 0x00, 0x21], /* 44 , */
    [0x00, 0x70, 0x00], /* 45 - */
    [0x00, 0x00, 0x10], /* 46 . */
    [0x44, 0x21, 0x10], /* 47 / */
    [0x75, 0x55, 0x70], /* 48 0 */
    [0x32, 0x22, 0x70], /* 49 1 */
    [0x74, 0x71, 0x70], /* 50 2 */
    [0x74, 0x74, 0x70], /* 51 3 */
    [0x55, 0x74, 0x40], /* 52 4 */
    [0x71, 0x74, 0x70], /* 53 5 */
    [0x71, 0x75, 0x70], /* 54 6 */
    [0x74, 0x44, 0x40], /* 55 7 */
    [0x75, 0x75, 0x70], /* 56 8 */
    [0x75, 0x74, 0x70], /* 57 9 */
    [0x00, 0x20, 0x20], /* 58 : */
    [0x00, 0x20, 0x21], /* 59 ; */
    [0x42, 0x12, 0x40], /* 60 < */
    [0x07, 0x07, 0x00], /* 61 = */
    [0x12, 0x42, 0x10], /* 62 > */
    [0x74, 0x60, 0x20], /* 63 ? */
    [0x25, 0x51, 0x60], /* 64 @ */
    [0x25, 0x75, 0x50], /* 65 A */
    [0x35, 0x35, 0x30], /* 66 B */
    [0x61, 0x11, 0x60], /* 67 C */
    [0x35, 0x55, 0x30], /* 68 D */
    [0x71, 0x31, 0x70], /* 69 E */
    [0x71, 0x31, 0x10], /* 70 F */
    [0x61, 0x55, 0x60], /* 71 G */
    [0x55, 0x75, 0x50], /* 72 H */
    [0x72, 0x22, 0x70], /* 73 I */
    [0x44, 0x45, 0x20], /* 74 J */
    [0x55, 0x35, 0x50], /* 75 K */
    [0x11, 0x11, 0x70], /* 76 L */
    [0x57, 0x75, 0x50], /* 77 M */
    [0x75, 0x55, 0x50], /* 78 N */
    [0x25, 0x55, 0x20], /* 79 O */
    [0x75, 0x71, 0x10], /* 80 P */
    [0x65, 0x53, 0x60], /* 81 Q */
    [0x35, 0x35, 0x50], /* 82 R */
    [0x61, 0x74, 0x30], /* 83 S */
    [0x72, 0x22, 0x20], /* 84 T */
    [0x55, 0x55, 0x70], /* 85 U */
    [0x55, 0x52, 0x20], /* 86 V */
    [0x55, 0x77, 0x50], /* 87 W */
    [0x55, 0x25, 0x50], /* 88 X */
    [0x55, 0x22, 0x20], /* 89 Y */
    [0x74, 0x21, 0x70], /* 90 Z */
    [0x62, 0x22, 0x60], /* 91 [ */
    [0x11, 0x24, 0x40], /* 92 \ */
    [0x32, 0x22, 0x30], /* 93 ] */
    [0x25, 0x00, 0x00], /* 94 ^ */
    [0x00, 0x00, 0x70], /* 95 _ */
    [0x12, 0x00, 0x00], /* 96 ` */
    [0x06, 0x55, 0x60], /* 97 a */
    [0x13, 0x55, 0x30], /* 98 b */
    [0x06, 0x11, 0x60], /* 99 c */
    [0x46, 0x55, 0x60], /* 100 d */
    [0x02, 0x53, 0x60], /* 101 e */
    [0x42, 0x72, 0x20], /* 102 f */
    [0x02, 0x56, 0x42], /* 103 g */
    [0x11, 0x35, 0x50], /* 104 h */
    [0x02, 0x02, 0x20], /* 105 i */
    [0x02, 0x02, 0x21], /* 106 j */
    [0x15, 0x35, 0x50], /* 107 k */
    [0x22, 0x22, 0x40], /* 108 l */
    [0x05, 0x75, 0x50], /* 109 m */
    [0x03, 0x55, 0x50], /* 110 n */
    [0x02, 0x55, 0x20], /* 111 o */
    [0x03, 0x55, 0x31], /* 112 p */
    [0x06, 0x55, 0x64], /* 113 q */
    [0x02, 0x51, 0x10], /* 114 r */
    [0x06, 0x14, 0x30], /* 115 s */
    [0x27, 0x22, 0x40], /* 116 t */
    [0x05, 0x55, 0x60], /* 117 u */
    [0x05, 0x52, 0x20], /* 118 v */
    [0x05, 0x57, 0x50], /* 119 w */
    [0x05, 0x22, 0x50], /* 120 x */
    [0x05, 0x56, 0x42], /* 121 y */
    [0x07, 0x41, 0x70], /* 122 z */
    [0x62, 0x12, 0x60], /* 123 { */
    [0x22, 0x22, 0x20], /* 124 | */
    [0x32, 0x42, 0x30], /* 125 } */
    [0x03, 0x60, 0x00], /* 126 ~ */
];

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
let cursorVisible = false;
let cursorRow = 0;
let cursorCol = 0;
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
    if (!ledState || ledState === lastLedState) return;
    lastLedState = ledState;
    const leds = ledState.split(',').map(v => parseInt(v));

    needsRedraw = true;

    /* Corrected mapping based on testing:
     * Step 1 (Edit) = 0x08 = leds[2] (DSP "rhythm")
     * Step 2 (System) = 0x04 = leds[1] (DSP "system")
     * Step 3 (Rhythm) = 0x02 = leds[0] (DSP "edit")
     * Step 4 (Utility) = 0x01 = leds[4] (DSP "patch") */
    setStepLed(MoveStep1, leds[2] ? LedOn : LedOff);  /* Edit */
    setStepLed(MoveStep2, leds[1] ? LedOn : LedOff);  /* System */
    setStepLed(MoveStep3, leds[0] ? LedOn : LedOff);  /* Rhythm */
    setStepLed(MoveStep4, leds[4] ? LedOn : LedOff);  /* Utility */

    /* Tones: reversed LED mapping, inverted colors (1=active=white, 0=muted=orange) */
    setStepLed(MoveStep9, leds[9] ? LedOff : LedOn);   /* Tone 1 */
    setStepLed(MoveStep10, leds[8] ? LedOff : LedOn);  /* Tone 2 */
    setStepLed(MoveStep11, leds[7] ? LedOff : LedOn);  /* Tone 3 */
    setStepLed(MoveStep12, leds[6] ? LedOff : LedOn);  /* Tone 4 */

    /* Menu LED: bright in Patch mode, dim in Performance mode */
    const perfMode = host_module_get_param('performance_mode') || '0';
    const inPatchMode = perfMode === '0';
    setLed(MoveMenu, inPatchMode ? WhiteLedBright : 0x10);
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

/* Draw a single character using 4x6 pixel font
 * Format: 3 bytes, each byte = 2 rows (high nibble first), LSB = leftmost pixel
 * color: 0=black, 1=white */
function drawChar4x6(x, y, ch, color) {
    const code = ch.charCodeAt(0);
    if (code < 32 || code > 126) return;
    const glyph = FONT_4X6[code - 32];
    for (let byteIdx = 0; byteIdx < 3; byteIdx++) {
        const byte = glyph[byteIdx];
        /* High nibble = first row, low nibble = second row */
        const row0 = (byte >> 4) & 0x0F;
        const row1 = byte & 0x0F;
        const rowY0 = y + byteIdx * 2;
        const rowY1 = rowY0 + 1;
        for (let bit = 0; bit < 4; bit++) {
            if (row0 & (1 << bit)) {
                set_pixel(x + bit, rowY0, color);
            }
            if (row1 & (1 << bit)) {
                set_pixel(x + bit, rowY1, color);
            }
        }
    }
}

/* Draw text using 4x6 pixel font with 1px spacing */
function drawLcdText(x, y, text, color) {
    for (let i = 0; i < text.length; i++) {
        drawChar4x6(x + i * LCD_CHAR_WIDTH, y, text[i], color);
    }
}

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

    /* Lines 1-2: Button activity and expansion */
    print(1, y1, `${activeButton}`, 1);
    print(1, y2, `${expansionName}`, 1);

    /* JV-880 LCD area - using 4x6 pixel font */
    /* LCD takes bottom portion: 2 lines of 8 pixels each + padding */
    const lcdY = 46;  /* Start Y for LCD area */
    const lcdLine0Y = lcdY;
    const lcdLine1Y = lcdY + LCD_LINE_HEIGHT;

    /* White background for LCD (inverted display) */
    fill_rect(0, lcdY - 2, SCREEN_WIDTH, LCD_LINE_HEIGHT * 2 + 4, 1);

    /* Draw LCD text in black (color 0) on white background */
    drawLcdText(LCD_START_X, lcdLine0Y, lcdLine0, 0);
    drawLcdText(LCD_START_X, lcdLine1Y, lcdLine1, 0);

    /* Draw cursor underline if visible */
    if (cursorVisible && cursorCol >= 0 && cursorCol < 24) {
        const cursorY = (cursorRow === 0) ? lcdLine0Y : lcdLine1Y;
        const cursorX = LCD_START_X + cursorCol * LCD_CHAR_WIDTH;
        /* Draw black underline below the character (font is 6px tall) */
        fill_rect(cursorX, cursorY + 6, LCD_CHAR_WIDTH - 1, 1, 0);
    }

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
        /* Steps 9-12: TONE 1-4 (direct: SW1 controls leds[9]→Step9, etc.) */
        case 8:
            tapButton(MCU_BUTTON_TONE_SW1, `T1`);
            return true;
        case 9:
            tapButton(MCU_BUTTON_TONE_SW2, `T2`);
            return true;
        case 10:
            tapButton(MCU_BUTTON_TONE_SW3, `T3`);
            return true;
        case 11:
            tapButton(MCU_BUTTON_TONE_SW4, `T4`);
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

    const cursor = host_module_get_param('lcd_cursor');
    if (cursor) {
        const parts = cursor.split(',');
        const newVisible = parts[0] === '1';
        const newRow = parseInt(parts[1]);
        const newCol = parseInt(parts[2]);
        if (newVisible !== cursorVisible || newRow !== cursorRow || newCol !== cursorCol) {
            cursorVisible = newVisible;
            cursorRow = newRow;
            cursorCol = newCol;
            needsRedraw = true;
        }
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
