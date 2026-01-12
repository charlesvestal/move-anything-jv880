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
const LCD_LINE_HEIGHT = 9;  /* 7 pixels + 2 spacing */
const LCD_CHAR_WIDTH = 5;   /* 5 pixels, no spacing (24*5=120, fits in 128) */
const LCD_START_X = 4;      /* Left margin: (128-120)/2 = 4 */

/* 5x7 pixel font - classic HD44780/embedded LCD font (public domain)
 * Each character is 5 bytes, one per column, LSB at top
 * Characters 32-126 (printable ASCII) */
const FONT_5X7 = [
    [0x00,0x00,0x00,0x00,0x00], /* 32 space */
    [0x00,0x00,0x5F,0x00,0x00], /* 33 ! */
    [0x00,0x07,0x00,0x07,0x00], /* 34 " */
    [0x14,0x7F,0x14,0x7F,0x14], /* 35 # */
    [0x24,0x2A,0x7F,0x2A,0x12], /* 36 $ */
    [0x23,0x13,0x08,0x64,0x62], /* 37 % */
    [0x36,0x49,0x55,0x22,0x50], /* 38 & */
    [0x00,0x05,0x03,0x00,0x00], /* 39 ' */
    [0x00,0x1C,0x22,0x41,0x00], /* 40 ( */
    [0x00,0x41,0x22,0x1C,0x00], /* 41 ) */
    [0x08,0x2A,0x1C,0x2A,0x08], /* 42 * */
    [0x08,0x08,0x3E,0x08,0x08], /* 43 + */
    [0x00,0x50,0x30,0x00,0x00], /* 44 , */
    [0x08,0x08,0x08,0x08,0x08], /* 45 - */
    [0x00,0x60,0x60,0x00,0x00], /* 46 . */
    [0x20,0x10,0x08,0x04,0x02], /* 47 / */
    [0x3E,0x51,0x49,0x45,0x3E], /* 48 0 */
    [0x00,0x42,0x7F,0x40,0x00], /* 49 1 */
    [0x42,0x61,0x51,0x49,0x46], /* 50 2 */
    [0x21,0x41,0x45,0x4B,0x31], /* 51 3 */
    [0x18,0x14,0x12,0x7F,0x10], /* 52 4 */
    [0x27,0x45,0x45,0x45,0x39], /* 53 5 */
    [0x3C,0x4A,0x49,0x49,0x30], /* 54 6 */
    [0x01,0x71,0x09,0x05,0x03], /* 55 7 */
    [0x36,0x49,0x49,0x49,0x36], /* 56 8 */
    [0x06,0x49,0x49,0x29,0x1E], /* 57 9 */
    [0x00,0x36,0x36,0x00,0x00], /* 58 : */
    [0x00,0x56,0x36,0x00,0x00], /* 59 ; */
    [0x00,0x08,0x14,0x22,0x41], /* 60 < */
    [0x14,0x14,0x14,0x14,0x14], /* 61 = */
    [0x41,0x22,0x14,0x08,0x00], /* 62 > */
    [0x02,0x01,0x51,0x09,0x06], /* 63 ? */
    [0x32,0x49,0x79,0x41,0x3E], /* 64 @ */
    [0x7E,0x11,0x11,0x11,0x7E], /* 65 A */
    [0x7F,0x49,0x49,0x49,0x36], /* 66 B */
    [0x3E,0x41,0x41,0x41,0x22], /* 67 C */
    [0x7F,0x41,0x41,0x22,0x1C], /* 68 D */
    [0x7F,0x49,0x49,0x49,0x41], /* 69 E */
    [0x7F,0x09,0x09,0x01,0x01], /* 70 F */
    [0x3E,0x41,0x41,0x51,0x32], /* 71 G */
    [0x7F,0x08,0x08,0x08,0x7F], /* 72 H */
    [0x00,0x41,0x7F,0x41,0x00], /* 73 I */
    [0x20,0x40,0x41,0x3F,0x01], /* 74 J */
    [0x7F,0x08,0x14,0x22,0x41], /* 75 K */
    [0x7F,0x40,0x40,0x40,0x40], /* 76 L */
    [0x7F,0x02,0x04,0x02,0x7F], /* 77 M */
    [0x7F,0x04,0x08,0x10,0x7F], /* 78 N */
    [0x3E,0x41,0x41,0x41,0x3E], /* 79 O */
    [0x7F,0x09,0x09,0x09,0x06], /* 80 P */
    [0x3E,0x41,0x51,0x21,0x5E], /* 81 Q */
    [0x7F,0x09,0x19,0x29,0x46], /* 82 R */
    [0x46,0x49,0x49,0x49,0x31], /* 83 S */
    [0x01,0x01,0x7F,0x01,0x01], /* 84 T */
    [0x3F,0x40,0x40,0x40,0x3F], /* 85 U */
    [0x1F,0x20,0x40,0x20,0x1F], /* 86 V */
    [0x7F,0x20,0x18,0x20,0x7F], /* 87 W */
    [0x63,0x14,0x08,0x14,0x63], /* 88 X */
    [0x03,0x04,0x78,0x04,0x03], /* 89 Y */
    [0x61,0x51,0x49,0x45,0x43], /* 90 Z */
    [0x00,0x00,0x7F,0x41,0x41], /* 91 [ */
    [0x02,0x04,0x08,0x10,0x20], /* 92 \ */
    [0x41,0x41,0x7F,0x00,0x00], /* 93 ] */
    [0x04,0x02,0x01,0x02,0x04], /* 94 ^ */
    [0x40,0x40,0x40,0x40,0x40], /* 95 _ */
    [0x00,0x01,0x02,0x04,0x00], /* 96 ` */
    [0x20,0x54,0x54,0x54,0x78], /* 97 a */
    [0x7F,0x48,0x44,0x44,0x38], /* 98 b */
    [0x38,0x44,0x44,0x44,0x20], /* 99 c */
    [0x38,0x44,0x44,0x48,0x7F], /* 100 d */
    [0x38,0x54,0x54,0x54,0x18], /* 101 e */
    [0x08,0x7E,0x09,0x01,0x02], /* 102 f */
    [0x08,0x14,0x54,0x54,0x3C], /* 103 g */
    [0x7F,0x08,0x04,0x04,0x78], /* 104 h */
    [0x00,0x44,0x7D,0x40,0x00], /* 105 i */
    [0x20,0x40,0x44,0x3D,0x00], /* 106 j */
    [0x00,0x7F,0x10,0x28,0x44], /* 107 k */
    [0x00,0x41,0x7F,0x40,0x00], /* 108 l */
    [0x7C,0x04,0x18,0x04,0x78], /* 109 m */
    [0x7C,0x08,0x04,0x04,0x78], /* 110 n */
    [0x38,0x44,0x44,0x44,0x38], /* 111 o */
    [0x7C,0x14,0x14,0x14,0x08], /* 112 p */
    [0x08,0x14,0x14,0x18,0x7C], /* 113 q */
    [0x7C,0x08,0x04,0x04,0x08], /* 114 r */
    [0x48,0x54,0x54,0x54,0x20], /* 115 s */
    [0x04,0x3F,0x44,0x40,0x20], /* 116 t */
    [0x3C,0x40,0x40,0x20,0x7C], /* 117 u */
    [0x1C,0x20,0x40,0x20,0x1C], /* 118 v */
    [0x3C,0x40,0x30,0x40,0x3C], /* 119 w */
    [0x44,0x28,0x10,0x28,0x44], /* 120 x */
    [0x0C,0x50,0x50,0x50,0x3C], /* 121 y */
    [0x44,0x64,0x54,0x4C,0x44], /* 122 z */
    [0x00,0x08,0x36,0x41,0x00], /* 123 { */
    [0x00,0x00,0x7F,0x00,0x00], /* 124 | */
    [0x00,0x41,0x36,0x08,0x00], /* 125 } */
    [0x08,0x08,0x2A,0x1C,0x08], /* 126 ~ */
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

/* Draw a single character using 5x7 pixel font
 * color: 0=black, 1=white */
function drawChar5x7(x, y, ch, color) {
    const code = ch.charCodeAt(0);
    if (code < 32 || code > 126) return;
    const glyph = FONT_5X7[code - 32];
    for (let col = 0; col < 5; col++) {
        const colData = glyph[col];
        for (let row = 0; row < 7; row++) {
            if (colData & (1 << row)) {
                set_pixel(x + col, y + row, color);
            }
        }
    }
}

/* Draw text using 5x7 pixel font */
function drawLcdText(x, y, text, color) {
    for (let i = 0; i < text.length; i++) {
        drawChar5x7(x + i * LCD_CHAR_WIDTH, y, text[i], color);
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

    /* Lines 1-2: Debug LED info or normal display */
    if (activeButtonUntil > nowMs() && debugLine1) {
        print(1, y1, debugLine1, 1);
        print(1, y2, debugLine2, 1);
    } else {
        print(1, y1, activeButton, 1);
        print(1, y2, `Card: ${expansionName}`, 1);
    }

    /* JV-880 LCD area - using 5x7 pixel font */
    /* LCD takes bottom portion: 2 lines of 9 pixels each + padding */
    const lcdY = 44;  /* Start Y for LCD area */
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
        /* Draw black underline below the character */
        fill_rect(cursorX, cursorY + 7, LCD_CHAR_WIDTH, 1, 0);
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
