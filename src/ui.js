/*
 * JV-880 Module UI - Menu-Driven Version
 *
 * Two main modes:
 * - Browser: Shows current patch, jog to browse, knobs for macros
 * - Menu: Hierarchical menu navigation for editing and settings
 */

import * as std from 'std';

import {
    BrightGreen, BrightRed, DarkGrey, LightGrey, White, ForestGreen,
    MoveMainKnob, MoveMainButton,
    MoveLeft, MoveRight, MoveUp, MoveDown,
    MoveShift, MoveMenu, MoveBack, MoveMute,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveRow1, MoveRow2, MoveRow3, MoveRow4,
    MoveSteps, MovePads
} from '../../shared/constants.mjs';

import { isCapacitiveTouchMessage, setLED, setButtonLED, decodeDelta, decodeAcceleratedDelta } from '../../shared/input_filter.mjs';

import { createMenuStack } from '../../shared/menu_stack.mjs';
import { createMenuState, handleMenuInput } from '../../shared/menu_nav.mjs';
import { drawHierarchicalMenu } from '../../shared/menu_render.mjs';
import { showOverlay, hideOverlay, tickOverlay, drawOverlay, isOverlayActive } from '../../shared/menu_layout.mjs';

import { getMainMenu, getPatchBanksMenu, getPerformancesMenu, getEditMenu, setStateAccessor as setMenuStateAccessor } from './ui_menu.mjs';
import {
    drawBrowser, drawLoadingScreen, drawActivityOverlay,
    setStateAccessor as setBrowserStateAccessor
} from './ui_browser.mjs';

import {
    buildSystemMode,
    buildPatchCommonParam,
    buildToneParam,
    buildPerformanceCommonParam,
    buildPartParam,
    buildDrumParam,
    clampValue,
    PATCH_COMMON_PARAMS,
    TONE_PARAMS
} from './jv880_sysex.mjs';

/* === Constants === */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* LED colors for 5-state system */
const LED_OFF = DarkGrey;
const LED_GREY = LightGrey;        /* Muted, not selected */
const LED_WHITE = White;           /* Muted, selected */
const LED_DIM_GREEN = ForestGreen; /* Enabled, not selected */
const LED_BRIGHT_GREEN = BrightGreen;  /* Enabled, selected */

const CC_JOG = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_UP = MoveUp;
const CC_DOWN = MoveDown;
const CC_SHIFT = MoveShift;
const CC_MENU = MoveMenu;
const CC_BACK = MoveBack;
const CC_MUTE = MoveMute;

const ENCODER_CCS = [
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8
];

const TRACK_NOTES = [MoveRow4, MoveRow3, MoveRow2, MoveRow1];

const SYSEX_DEVICE_IDS = [0x10];
const SYSEX_THROTTLE_MS = 30;

/* Play mode macros for knobs */
const PLAY_MACROS = [
    { label: 'Cutoff', key: 'cutofffrequency', scope: 'tone' },
    { label: 'Resonance', key: 'resonance', scope: 'tone' },
    { label: 'Attack', key: 'tvaenvtime1', scope: 'tone' },
    { label: 'Release', key: 'tvaenvtime4', scope: 'tone' },
    { label: 'LFO Rate', key: 'lfo1rate', scope: 'tone' },
    { label: 'LFO Depth', key: 'lfo1tvfdepth', scope: 'tone' },
    { label: 'FX Send', key: 'reverbsendlevel', scope: 'tone' },
    { label: 'Level', key: 'level', scope: 'tone' }
];

/* === State === */
let uiMode = 'browser';  /* 'browser' | 'menu' */
const menuStack = createMenuStack();
const menuState = createMenuState();

let currentPreset = 0;
let totalPatches = 128;
let bankName = 'JV-880';
let bankCount = 0;
let patchName = '---';
let lcdLine0 = '';
let lcdLine1 = '';
let loadingStatus = 'Initializing...';
let loadingComplete = false;
let shiftHeld = false;

let mode = 'patch';  /* 'patch' | 'performance' */
let selectedTone = 0;
let selectedPart = 0;
let partBank = 0;
const toneEnabled = [1, 1, 1, 1];
const partEnabled = Array.from({ length: 8 }, () => 1);

let octaveTranspose = 0;
let localAudition = true;
let midiMonitor = false;
let sysExRx = true;

let lastActivity = { text: '', until: 0 };
const paramValues = new Map();
const pendingSysex = new Map();

let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;

/* Knob touch tracking for overlay */
let touchedKnob = -1;
let touchStartTick = 0;
let globalTickCount = 0;
let overlayExtendUntil = 0; /* Keep overlay visible until this tick */
let lastOverlayMacro = null; /* Remember macro for extended display */
const MIN_OVERLAY_TICKS = 180; /* 3 seconds at 60fps */

/* === State Accessor for Menu/Browser modules === */
function getStateForModules() {
    return {
        mode,
        patchName,
        bankName,
        currentPreset,
        totalPatches,
        selectedTone,
        selectedPart,
        toneEnabled,
        partEnabled,
        octaveTranspose,
        localAudition,
        midiMonitor,
        sysExRx,

        /* Methods */
        getBanks: () => {
            /* Return available banks from DSP */
            const banks = [];
            for (let i = 0; i < bankCount; i++) {
                const name = host_module_get_param(`bank_${i}_name`) || `Bank ${i + 1}`;
                banks.push({ id: i, name });
            }
            if (banks.length === 0) {
                banks.push({ id: 0, name: 'Internal' });
            }
            return banks;
        },
        getPatchesInBank: (bankId) => {
            /* Get patches for a bank from DSP */
            const patches = [];
            const startStr = host_module_get_param(`bank_${bankId}_start`);
            const countStr = host_module_get_param(`bank_${bankId}_count`);
            const start = startStr ? parseInt(startStr) : bankId * 128;
            const count = countStr ? parseInt(countStr) : 128;

            for (let i = 0; i < count; i++) {
                const globalIdx = start + i;
                const name = host_module_get_param(`patch_${globalIdx}_name`) || `Patch ${i + 1}`;
                patches.push({ index: globalIdx, name });
            }
            return patches;
        },
        getPerformanceName: (index) => {
            return host_module_get_param(`perf_${index}_name`) || `Perf ${index + 1}`;
        },
        loadPatch: (bankId, patchIndex) => {
            console.log('loadPatch called:', bankId, patchIndex);
            setMode('patch');
            setPreset(patchIndex);
            /* Exit menu and return to browser */
            uiMode = 'browser';
            menuStack.reset();
            menuState.selectedIndex = 0;
            menuState.editing = false;
            updateButtonLEDs();
            needsRedraw = true;
            console.log('loadPatch done, uiMode:', uiMode, 'stack depth:', menuStack.depth());
        },
        loadPerformance: (perfIndex) => {
            console.log('loadPerformance called:', perfIndex);
            setMode('performance');
            setPreset(perfIndex);
            /* Exit menu and return to browser */
            uiMode = 'browser';
            menuStack.reset();
            menuState.selectedIndex = 0;
            menuState.editing = false;
            updateButtonLEDs();
            needsRedraw = true;
            console.log('loadPerformance done, uiMode:', uiMode, 'stack depth:', menuStack.depth());
        },
        getParam: (scope, key, target) => getParamValue(scope, key, target),
        setParam: (scope, key, value, target) => setParamValueAndSend(scope, key, value, target),
        setOctave: (v) => { octaveTranspose = clamp(v, -4, 4); setActivity(`Oct ${octaveTranspose >= 0 ? '+' : ''}${octaveTranspose}`); },
        setLocalAudition: (v) => { localAudition = v; setActivity(v ? 'Local ON' : 'Local OFF'); },
        setMidiMonitor: (v) => { midiMonitor = v; setActivity(v ? 'Monitor ON' : 'Monitor OFF'); },
        setSysExRx: (v) => { sysExRx = v; setActivity(v ? 'SysEx RX' : 'SysEx OFF'); }
    };
}

/* Initialize state accessors */
setMenuStateAccessor(getStateForModules);
setBrowserStateAccessor(getStateForModules);

/* === Utility === */
function nowMs() {
    return Date.now ? Date.now() : new Date().getTime();
}

function clamp(val, min, max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

function setActivity(text, durationMs = 2000) {
    lastActivity = { text, until: nowMs() + durationMs };
    needsRedraw = true;
}

/* === Mode Management === */
function setMode(newMode, force = false) {
    if (!force && mode === newMode) return;
    mode = newMode;
    sendSysEx(buildSystemMode(newMode));
    host_module_set_param('mode', newMode === 'performance' ? '1' : '0');
    currentPreset = 0;
    updateLEDs();
    needsRedraw = true;
    setActivity(newMode === 'performance' ? 'PERFORMANCE' : 'PATCH');
}

function setPreset(index) {
    if (mode === 'performance') {
        const numPerfs = 48;
        if (index < 0) index = numPerfs - 1;
        if (index >= numPerfs) index = 0;
        currentPreset = index;
        host_module_set_param('performance', String(currentPreset));
    } else {
        if (index < 0) index = totalPatches - 1;
        if (index >= totalPatches) index = 0;
        currentPreset = index;
        host_module_set_param('program_change', String(currentPreset));
    }
    updateLEDs();
    needsRedraw = true;
}

/* === SysEx === */
function sendSysEx(msg) {
    if (!msg) return;
    if (msg[0] !== 0xF0 || msg.length < 6) {
        host_module_send_midi(msg, 'host');
        return;
    }
    const sendBytes = (bytes) => {
        const chunkSize = 3;
        if (bytes.length <= chunkSize) {
            host_module_send_midi(bytes, 'host');
            return;
        }
        for (let i = 0; i < bytes.length; i += chunkSize) {
            host_module_send_midi(bytes.slice(i, i + chunkSize), 'host');
        }
    };
    for (const deviceId of SYSEX_DEVICE_IDS) {
        const out = msg.slice();
        out[2] = deviceId;
        sendBytes(out);
    }
}

function queueSysEx(key, msg) {
    if (!msg) return;
    const now = nowMs();
    let entry = pendingSysex.get(key);
    if (!entry) {
        entry = { msg, lastSent: 0, dueAt: 0 };
        pendingSysex.set(key, entry);
    } else {
        entry.msg = msg;
    }

    if (now - entry.lastSent >= SYSEX_THROTTLE_MS) {
        sendSysEx(entry.msg);
        entry.lastSent = now;
        entry.dueAt = 0;
    } else {
        entry.dueAt = entry.lastSent + SYSEX_THROTTLE_MS;
    }
}

function flushPendingSysEx() {
    if (pendingSysex.size === 0) return;
    const now = nowMs();
    for (const entry of pendingSysex.values()) {
        if (entry.dueAt && now >= entry.dueAt) {
            sendSysEx(entry.msg);
            entry.lastSent = now;
            entry.dueAt = 0;
        }
    }
}

/* === Parameter Management === */
function getParamStoreKey(scope, key, target) {
    /* Handle object targets (e.g., { part: 0, tone: 1 }) */
    let targetId;
    if (target && typeof target === 'object') {
        targetId = `${target.part ?? 0}:${target.tone ?? 0}`;
    } else {
        targetId = target !== undefined ? target : 0;
    }
    return `${scope}:${key}:${targetId}`;
}

function getParamValue(scope, key, target) {
    const storeKey = getParamStoreKey(scope, key, target);
    if (paramValues.has(storeKey)) return paramValues.get(storeKey);

    /* Try to read from DSP/NVRAM using parameter names */
    let dspKey = null;
    if (scope === 'patchCommon') {
        /* Pass parameter name directly - DSP maps to correct NVRAM offset */
        dspKey = `nvram_patchCommon_${key}`;
    } else if (scope === 'tone') {
        const toneIdx = target !== undefined ? target : 0;
        dspKey = `nvram_tone_${toneIdx}_${key}`;
    } else if (scope === 'partTone') {
        const toneIdx = target?.tone ?? 0;
        dspKey = `nvram_tone_${toneIdx}_${key}`;
    }

    if (dspKey) {
        const val = host_module_get_param(dspKey);
        if (val !== null && val !== undefined) {
            const numVal = parseInt(val);
            if (!isNaN(numVal)) {
                paramValues.set(storeKey, numVal);
                return numVal;
            }
        }
    }

    return 0;
}

function setParamValueAndSend(scope, key, value, target) {
    const storeKey = getParamStoreKey(scope, key, target);
    const v = clampValue(value, 0, 127);
    paramValues.set(storeKey, v);

    if (scope === 'patchCommon') {
        queueSysEx(storeKey, buildPatchCommonParam(key, v));
    } else if (scope === 'tone') {
        queueSysEx(storeKey, buildToneParam(target || selectedTone, key, v));
    } else if (scope === 'performanceCommon') {
        queueSysEx(storeKey, buildPerformanceCommonParam(key, v));
    } else if (scope === 'part') {
        queueSysEx(storeKey, buildPartParam(target || selectedPart, key, v));
    } else if (scope === 'partTone') {
        /* Part tone editing - target is { part, tone } */
        const toneIdx = target?.tone ?? selectedTone;
        queueSysEx(storeKey, buildToneParam(toneIdx, key, v));
    } else if (scope === 'drum') {
        queueSysEx(storeKey, buildDrumParam(target || 60, key, v));
    }
}

/* === LED Management === */
function updateLEDs() {
    updateTrackLEDs();
    updateStepLEDs();
    updateButtonLEDs();
}

function updateTrackLEDs() {
    /* Track buttons always control tones (both patch and performance mode) */
    for (let i = 0; i < TRACK_NOTES.length; i++) {
        const note = TRACK_NOTES[i];
        const enabled = !!toneEnabled[i];
        const selected = selectedTone === i;
        let color;
        if (!enabled && !selected) color = LED_GREY;
        else if (!enabled && selected) color = LED_WHITE;
        else if (enabled && !selected) color = LED_DIM_GREEN;
        else color = LED_BRIGHT_GREEN;
        setLED(note, color, true);
        setButtonLED(note, color, true);
    }
}

function updateStepLEDs() {
    /* Steps 1-8: Select parts in performance mode */
    for (let i = 0; i < 8; i++) {
        if (mode === 'performance') {
            const enabled = !!partEnabled[i];
            const selected = selectedPart === i;
            let color;
            if (!enabled && !selected) color = LED_GREY;
            else if (!enabled && selected) color = LED_WHITE;
            else if (enabled && !selected) color = LED_DIM_GREEN;
            else color = LED_BRIGHT_GREEN;
            setStepLED(i, color);
        } else {
            setStepLED(i, LED_OFF);
        }
    }
    /* Steps 9-16: Various functions */
    for (let i = 8; i < 16; i++) {
        setStepLED(i, LED_OFF);
    }
}

function updateButtonLEDs() {
    setButtonLED(CC_MENU, uiMode === 'menu' ? LED_BRIGHT_GREEN : LED_GREY, true);
    setButtonLED(CC_BACK, LED_GREY, true);

    /* MUTE button: Red when selected tone/part is muted */
    let isMuted = false;
    if (mode === 'patch') {
        isMuted = !toneEnabled[selectedTone];
    } else {
        isMuted = !partEnabled[selectedPart];
    }
    setButtonLED(CC_MUTE, isMuted ? BrightRed : LED_GREY, true);
}

function setStepLED(index, color) {
    const note = MoveSteps[index];
    if (note === undefined) return;
    setLED(note, color, true);
}

/* === Display === */
function drawUI() {
    if (!loadingComplete) {
        drawLoadingScreen(loadingStatus);
        return;
    }

    clear_screen();

    if (uiMode === 'menu') {
        const current = menuStack.current();
        if (current) {
            const footer = menuState.editing
                ? 'Jog:Val Clk:OK Bck:Cancel'
                : 'Jog:Sel Bck:Back';
            drawHierarchicalMenu({
                title: current.title,
                items: current.items,
                state: menuState,
                footer
            });
        } else {
            /* Stack is empty, return to browser */
            uiMode = 'browser';
            updateButtonLEDs();
            drawBrowser();
        }
    } else {
        drawBrowser();
    }

    /* Parameter overlay (from knob changes) */
    drawOverlay();

    /* Activity overlay (from button presses etc) */
    if (lastActivity.text && lastActivity.until > nowMs() && !isOverlayActive()) {
        drawActivityOverlay(lastActivity.text);
    }

    needsRedraw = false;
}

/* === Input Handling === */
function handleCC(cc, value) {
    if (cc === CC_SHIFT) {
        shiftHeld = value > 0;
        return false;
    }

    /* Menu button - enter edit menu directly */
    if (cc === CC_MENU && value > 0) {
        if (uiMode === 'browser') {
            uiMode = 'menu';
            menuStack.reset();
            menuStack.push({ title: 'Edit', items: getEditMenu() });
            menuState.selectedIndex = 0;
            menuState.editing = false;
            updateButtonLEDs();
            needsRedraw = true;
            return true;
        }
        return false;
    }

    /* Menu mode input handling */
    if (uiMode === 'menu') {
        const current = menuStack.current();
        if (!current) {
            uiMode = 'browser';
            updateButtonLEDs();
            needsRedraw = true;
            return true;
        }

        const result = handleMenuInput({
            cc,
            value,
            items: current.items,
            state: menuState,
            stack: menuStack,
            onBack: () => {
                uiMode = 'browser';
                updateButtonLEDs();
            },
            shiftHeld
        });

        if (result.needsRedraw) {
            needsRedraw = true;
        }
        return true;
    }

    /* Browser mode controls */
    if (cc === CC_JOG) {
        const delta = decodeDelta(value);
        if (delta !== 0) {
            setPreset(currentPreset + delta);
        }
        return true;
    }

    if (cc === CC_JOG_CLICK && value > 0) {
        /* Enter browse menu on jog click in browser */
        uiMode = 'menu';
        menuStack.reset();
        if (mode === 'performance') {
            menuStack.push({ title: 'Performances', items: getPerformancesMenu() });
        } else {
            menuStack.push({ title: 'Patches', items: getPatchBanksMenu() });
        }
        menuState.selectedIndex = 0;
        menuState.editing = false;
        updateButtonLEDs();
        needsRedraw = true;
        return true;
    }

    if (cc === CC_LEFT && value > 0) {
        if (shiftHeld) {
            /* Bank jump */
            host_module_set_param('prev_bank', '1');
            setActivity('Bank -');
        } else {
            setPreset(currentPreset - 1);
        }
        return true;
    }

    if (cc === CC_RIGHT && value > 0) {
        if (shiftHeld) {
            host_module_set_param('next_bank', '1');
            setActivity('Bank +');
        } else {
            setPreset(currentPreset + 1);
        }
        return true;
    }

    if (cc === CC_BACK && value > 0) {
        /* Toggle mode in browser */
        setMode(mode === 'patch' ? 'performance' : 'patch');
        return true;
    }

    /* Knob macros in browser mode */
    const encIndex = ENCODER_CCS.indexOf(cc);
    if (encIndex >= 0) {
        /* Use acceleration for smooth control, shift for fine control */
        const delta = shiftHeld ? decodeDelta(value) : decodeAcceleratedDelta(value, encIndex);
        if (delta !== 0) {
            const macro = PLAY_MACROS[encIndex];
            if (macro) {
                const current = getParamValue(macro.scope, macro.key, selectedTone);
                const newVal = clamp(current + delta, 0, 127);
                setParamValueAndSend(macro.scope, macro.key, newVal, selectedTone);
                /* Use overlay for better parameter feedback */
                showOverlay(macro.label, String(newVal));
                needsRedraw = true;
            }
        }
        return true;
    }

    /* Track buttons - always select tone (even in performance mode) */
    if (TRACK_NOTES.includes(cc) && value > 0) {
        const trackIndex = TRACK_NOTES.indexOf(cc);
        if (shiftHeld) {
            /* Toggle tone enable */
            toneEnabled[trackIndex] = toneEnabled[trackIndex] ? 0 : 1;
            sendSysEx(buildToneParam(trackIndex, 'toneswitch', toneEnabled[trackIndex]));
            setActivity(`Tone ${trackIndex + 1} ${toneEnabled[trackIndex] ? 'ON' : 'MUTE'}`);
        } else {
            selectedTone = trackIndex;
            setActivity(`Tone ${selectedTone + 1}`);
        }
        updateLEDs();
        needsRedraw = true;
        return true;
    }

    return false;
}

function handleNoteOn(note, velocity, channel, source) {
    /* Step buttons - select part in performance mode */
    if (MoveSteps.includes(note)) {
        const stepIndex = note - MoveSteps[0];
        if (mode === 'performance' && stepIndex < 8) {
            if (shiftHeld) {
                /* Toggle part enable */
                partEnabled[stepIndex] = partEnabled[stepIndex] ? 0 : 1;
                sendSysEx(buildPartParam(stepIndex, 'internalswitch', partEnabled[stepIndex]));
                setActivity(`Part ${stepIndex + 1} ${partEnabled[stepIndex] ? 'ON' : 'MUTE'}`);
            } else {
                selectedPart = stepIndex;
                host_module_set_param('part', String(selectedPart));
                setActivity(`Part ${selectedPart + 1}`);
            }
            updateLEDs();
            needsRedraw = true;
            return true;
        }
    }

    /* Track buttons via note - always select tone (even in performance mode) */
    if (TRACK_NOTES.includes(note) && velocity > 0) {
        const trackIndex = TRACK_NOTES.indexOf(note);
        if (shiftHeld) {
            toneEnabled[trackIndex] = toneEnabled[trackIndex] ? 0 : 1;
            sendSysEx(buildToneParam(trackIndex, 'toneswitch', toneEnabled[trackIndex]));
            setActivity(`Tone ${trackIndex + 1} ${toneEnabled[trackIndex] ? 'ON' : 'MUTE'}`);
        } else {
            selectedTone = trackIndex;
            setActivity(`Tone ${selectedTone + 1}`);
        }
        updateLEDs();
        needsRedraw = true;
        return true;
    }

    /* MIDI monitor display */
    if (velocity > 0 && (source === 'internal' || midiMonitor)) {
        const ch = clamp(channel + 1, 1, 16);
        const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
        const noteName = names[note % 12] + (Math.floor(note / 12) - 1);
        setActivity(`Ch${ch} ${noteName} V${velocity}`);
    }

    return false;
}

function handleCapacitiveTouch(note, velocity) {
    console.log('handleCapacitiveTouch: note=', note, 'vel=', velocity);

    /* Touch end (note off or velocity 0) */
    if (velocity <= 0 || velocity < 64) {
        if (touchedKnob === note) {
            const elapsed = globalTickCount - touchStartTick;
            console.log('Touch end, elapsed=', elapsed, 'min=', MIN_OVERLAY_TICKS);
            if (elapsed >= MIN_OVERLAY_TICKS) {
                /* Minimum time passed, hide immediately */
                hideOverlay();
                overlayExtendUntil = 0;
                needsRedraw = true;
            } else {
                /* Set extension to ensure 3 second minimum */
                overlayExtendUntil = touchStartTick + MIN_OVERLAY_TICKS;
                console.log('Extending overlay until tick', overlayExtendUntil);
            }
            touchedKnob = -1;
        }
        return true;
    }

    /* Touch start */
    console.log('ENCODER_CCS.length=', ENCODER_CCS.length);
    if (note >= 0 && note < ENCODER_CCS.length) {
        const macro = PLAY_MACROS[note];
        console.log('macro for note', note, '=', macro ? macro.label : 'none');
        if (macro) {
            const current = getParamValue(macro.scope, macro.key, selectedTone);
            console.log('current value=', current, 'showing overlay');
            showOverlay(macro.label, String(current));
            touchedKnob = note;
            touchStartTick = globalTickCount;
            lastOverlayMacro = macro;
            overlayExtendUntil = 0; /* Clear any pending extension */
            needsRedraw = true;
        }
        return true;
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
        }
    }

    const status = host_module_get_param('loading_status');
    if (status && status !== loadingStatus) {
        loadingStatus = status;
        if (!loadingComplete) needsRedraw = true;
    }

    const total = host_module_get_param('total_patches');
    if (total) {
        const n = parseInt(total);
        if (n > 0 && n !== totalPatches) {
            totalPatches = n;
            needsRedraw = true;
        }
    }

    const banks = host_module_get_param('bank_count');
    if (banks) {
        const b = parseInt(banks);
        if (b > 0 && b !== bankCount) {
            bankCount = b;
            needsRedraw = true;
        }
    }

    const bank = host_module_get_param('bank_name');
    if (bank && bank !== bankName) {
        bankName = bank;
        needsRedraw = true;
    }

    if (mode === 'performance') {
        const perf = host_module_get_param('current_performance');
        if (perf) {
            const p = parseInt(perf);
            if (p >= 0 && p !== currentPreset) {
                currentPreset = p;
                updateLEDs();
                needsRedraw = true;
            }
        }
    } else {
        const patch = host_module_get_param('current_patch');
        if (patch) {
            const p = parseInt(patch);
            if (p >= 0 && p !== currentPreset) {
                currentPreset = p;
                updateLEDs();
                needsRedraw = true;
            }
        }
    }

    const name = host_module_get_param('patch_name');
    if (name && name !== patchName) {
        patchName = name;
        needsRedraw = true;
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
    console.log('JV880 UI initializing (menu-driven)...');
    setMode(mode, true);
    updateLEDs();
    needsRedraw = true;
    updateFromDSP();
};

globalThis.tick = function() {
    globalTickCount++;
    updateFromDSP();
    flushPendingSysEx();

    /* Keep overlay visible while knob is touched */
    if (touchedKnob >= 0) {
        const macro = PLAY_MACROS[touchedKnob];
        if (macro) {
            const current = getParamValue(macro.scope, macro.key, selectedTone);
            showOverlay(macro.label, String(current));
        }
    }
    /* Keep overlay visible during extension period (minimum 3 sec) */
    else if (overlayExtendUntil > 0 && globalTickCount < overlayExtendUntil) {
        if (lastOverlayMacro) {
            const current = getParamValue(lastOverlayMacro.scope, lastOverlayMacro.key, selectedTone);
            showOverlay(lastOverlayMacro.label, String(current));
        }
    }
    /* Extension period ended */
    else if (overlayExtendUntil > 0 && globalTickCount >= overlayExtendUntil) {
        hideOverlay();
        overlayExtendUntil = 0;
        lastOverlayMacro = null;
        needsRedraw = true;
    }

    /* Tick overlay timer (only auto-hides if not being refreshed) */
    if (tickOverlay()) {
        needsRedraw = true;
    }

    tickCount++;
    if (needsRedraw || tickCount >= REDRAW_INTERVAL) {
        drawUI();
        tickCount = 0;
        needsRedraw = false;
    }
};

globalThis.onMidiMessageInternal = function(data) {
    console.log('MIDI internal:', data[0].toString(16), data[1], data[2]);
    if (isCapacitiveTouchMessage(data)) {
        console.log('Capacitive touch detected: note=', data[1], 'vel=', data[2]);
        handleCapacitiveTouch(data[1], data[2]);
        return;
    }

    const status = data[0] & 0xF0;
    if (status === 0xB0) {
        if (handleCC(data[1], data[2])) return;
    }

    if (status === 0x90 && data[2] > 0) {
        handleNoteOn(data[1], data[2], data[0] & 0x0F, 'internal');
    }
};

globalThis.onMidiMessageExternal = function(data) {
    const status = data[0] & 0xF0;
    if (status === 0x90 && data[2] > 0) {
        handleNoteOn(data[1], data[2], data[0] & 0x0F, 'external');
    }
};
