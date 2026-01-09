/*
 * JV-880 Module UI
 *
 * Full-panel UI with Play/Edit/Utility states and SysEx parameter control.
 */

import * as std from 'std';

import {
    BrightGreen, BrightRed, DarkGrey, LightGrey, White,
    MoveMainKnob, MoveMainButton,
    MoveLeft, MoveRight, MoveUp, MoveDown,
    MoveShift, MoveMenu, MoveBack,
    MoveCapture, MovePlay, MoveRec, MoveLoop,
    MoveRecord, MoveMute, MoveCopy, MoveDelete, MoveUndo,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveRow1, MoveRow2, MoveRow3, MoveRow4,
    MoveSteps, MovePads
} from '../../shared/constants.mjs';

import { isCapacitiveTouchMessage, setLED, setButtonLED } from '../../shared/input_filter.mjs';

import {
    buildSystemMode,
    buildPatchCommonParam,
    buildToneParam,
    buildPerformanceCommonParam,
    buildPartParam,
    buildDrumParam,
    clampValue
} from './jv880_sysex.mjs';

/* === Constants === */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const LCD_LINE_HEIGHT = 14;
const LED_ACTIVE = BrightGreen;
const LED_ALERT = BrightRed;
const LED_DIM = LightGrey;
const LED_OFF = DarkGrey;

const CC_JOG = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_UP = MoveUp;
const CC_DOWN = MoveDown;
const CC_SHIFT = MoveShift;
const CC_MENU = MoveMenu;
const CC_BACK = MoveBack;

const CC_PLAY = MovePlay;
const CC_REC = MoveRec;
const CC_LOOP = MoveLoop;
const CC_CAPTURE = MoveCapture;
const CC_RECORD = MoveRecord;
const CC_MUTE = MoveMute;
const CC_COPY = MoveCopy;
const CC_DELETE = MoveDelete;
const CC_UNDO = MoveUndo;

const ENCODER_CCS = [
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8
];

const TRACK_NOTES = [MoveRow4, MoveRow3, MoveRow2, MoveRow1];

const PAGE_ORDER = [
    'home', 'common', 'tone', 'pitch', 'tvf', 'tva', 'lfo', 'outfx',
    'mod', 'ctrl', 'struct', 'mix', 'part', 'rhythm', 'fx', 'util'
];

const SYSEX_DEVICE_IDS = Array.from({ length: 16 }, (_, i) => 0x10 + i);

const PLAY_MACROS = [
    { label: 'Cutoff', short: 'Cut', key: 'cutofffrequency', scope: 'tone' },
    { label: 'Reso', short: 'Res', key: 'resonance', scope: 'tone' },
    { label: 'Attack', short: 'Atk', key: 'tvaenvtime1', scope: 'tone' },
    { label: 'Release', short: 'Rel', key: 'tvaenvtime4', scope: 'tone' },
    { label: 'LFO Rate', short: 'L1Rt', key: 'lfo1rate', scope: 'tone' },
    { label: 'LFO Depth', short: 'L1Dp', key: 'lfo1tvfdepth', scope: 'tone' },
    { label: 'FX Send', short: 'FX', key: 'reverbsendlevel', scope: 'tone' },
    { label: 'Level', short: 'Lvl', key: 'level', scope: 'tone' }
];

/* === State === */
let currentPreset = 0;
let totalPatches = 128;
let octaveTranspose = 0;
let semitoneTranspose = 0;
let bankName = 'JV-880';
let bankCount = 0;
let patchName = '---';
let lcdLine0 = '';
let lcdLine1 = '';
let loadingStatus = 'Initializing...';
let loadingComplete = false;
let shiftHeld = false;

let uiState = 'play';
let currentPage = 'home';
let selectedParamIndex = 0;
let lfoIndex = 1;

let mode = 'patch';
let selectedTone = 0;
let partBank = 0;
let selectedPart = 0;
let rhythmFocus = false;
let lastDrumNote = 60;
const toneEnabled = [1, 1, 1, 1];
const partEnabled = Array.from({ length: 8 }, () => 1);

let favorites = [];
let favoritesView = false;
let favoritesIndex = 0;

let localAudition = true;
let midiMonitor = false;
let sysExRx = true;
let velocityMode = false;
let sustainLatch = false;

let lastActivity = { text: '', until: 0 };
let helpLine = '';
let helpUntilMs = 0;
let pendingConfirm = null;

const paramValues = new Map();

let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;

/* === Utility === */
function nowMs() {
    return Date.now ? Date.now() : new Date().getTime();
}

function setActivity(text, durationMs = 2000) {
    lastActivity = { text, until: nowMs() + durationMs };
    needsRedraw = true;
}

function setHelp(text, durationMs = 2000) {
    helpLine = text;
    helpUntilMs = nowMs() + durationMs;
    needsRedraw = true;
}

function setState(next) {
    if (uiState === next) return;
    uiState = next;
    if (uiState !== 'play') {
        favoritesView = false;
    }
    setHelp(`${next.toUpperCase()} MODE`, 2000);
    updateStepLEDs();
    updateButtonLEDs();
}

function setMode(next, force = false) {
    if (!force && mode === next) return;
    mode = next;
    sendSysEx(buildSystemMode(next));
    updateTrackLEDs();
    updateStepLEDs();
    updateButtonLEDs();
    setHelp(next === 'performance' ? 'PERFORMANCE MODE' : 'PATCH MODE', 2000);
}

function clamp(val, min, max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

function decodeDelta(value) {
    if (value === 0) return 0;
    if (value >= 1 && value <= 63) return 1;
    if (value >= 65 && value <= 127) return -1;
    return 0;
}

function noteName(note) {
    const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    const n = clamp(note, 0, 127);
    const name = names[n % 12];
    const oct = Math.floor(n / 12) - 1;
    return `${name}${oct}`;
}

function formatParamValue(param, value) {
    if (!param) return '';
    if (param.min === 0 && param.max === 1) return value ? 'ON' : 'OFF';
    return String(value);
}

function getModuleDir() {
    const current = host_get_current_module();
    if (!current || !current.ui_script) return '';
    const path = current.ui_script;
    return path.slice(0, path.lastIndexOf('/'));
}

function getFavoritesPath() {
    const dir = getModuleDir();
    if (!dir) return '';
    return `${dir}/favorites.json`;
}

function normalizeFavorite(entry) {
    if (!entry) return null;
    if (typeof entry === 'string') {
        const parts = entry.split(':');
        const mode = parts[0] === 'performance' ? 'performance' : 'patch';
        const preset = parseInt(parts[1], 10);
        if (Number.isNaN(preset)) return null;
        return { mode, preset, name: '' };
    }
    if (typeof entry === 'object') {
        const mode = entry.mode === 'performance' ? 'performance' : 'patch';
        const preset = Number.isFinite(entry.preset) ? entry.preset : parseInt(entry.preset, 10);
        if (!Number.isFinite(preset)) return null;
        const name = typeof entry.name === 'string' ? entry.name : '';
        return { mode, preset, name };
    }
    return null;
}

function favoriteId(entry) {
    if (!entry) return '';
    return `${entry.mode}:${entry.preset}`;
}

function loadFavorites() {
    const path = getFavoritesPath();
    if (!path) return [];
    try {
        const raw = std.loadFile(path);
        if (!raw) return [];
        const parsed = JSON.parse(raw);
        if (!Array.isArray(parsed)) return [];
        return parsed.map(normalizeFavorite).filter((entry) => entry);
    } catch (e) {
        return [];
    }
}

function saveFavorites() {
    const path = getFavoritesPath();
    if (!path) return false;
    try {
        const f = std.open(path, 'w');
        if (!f) return false;
        f.puts(JSON.stringify(favorites));
        f.close();
        return true;
    } catch (e) {
        return false;
    }
}

function favoriteKey() {
    return `${mode}:${currentPreset}`;
}

function isFavorite(key) {
    return favorites.some((entry) => favoriteId(entry) === key);
}

function toggleFavorite() {
    const key = favoriteKey();
    if (isFavorite(key)) {
        favorites = favorites.filter((entry) => favoriteId(entry) !== key);
        setActivity('FAV REMOVED');
    } else {
        favorites = [...favorites, { mode, preset: currentPreset, name: patchName || '' }];
        setActivity('FAV ADDED');
    }
    if (favoritesIndex >= favorites.length) {
        favoritesIndex = Math.max(0, favorites.length - 1);
    }
    updateStepLEDs();
    if (!saveFavorites()) {
        setActivity('FAV SAVE ERR');
    }
}

function sendSysEx(msg) {
    if (!msg) return;
    if (msg[0] !== 0xF0 || msg.length < 6) {
        host_module_send_midi(msg, 'host');
        return;
    }
    for (const deviceId of SYSEX_DEVICE_IDS) {
        const out = msg.slice();
        out[2] = deviceId;
        host_module_send_midi(out, 'host');
    }
}

function sendCC(channel, cc, value) {
    host_module_send_midi([0xB0 | (channel & 0x0F), cc, value & 0x7F], 'host');
}

function setStepLED(index, color) {
    const note = MoveSteps[index];
    if (note === undefined) return;
    setLED(note, color, true);
}

function isPageAvailable(pageId) {
    if (pageId === 'mix' || pageId === 'part' || pageId === 'rhythm') {
        return mode === 'performance';
    }
    return true;
}

function updateStepLEDs() {
    if (uiState === 'play') {
        setStepLED(0, mode === 'patch' ? LED_ACTIVE : LED_DIM);
        setStepLED(1, mode === 'performance' ? LED_ACTIVE : LED_DIM);
        setStepLED(2, rhythmFocus ? LED_ACTIVE : LED_DIM);
        setStepLED(3, LED_DIM);

        const favActive = favoritesView;
        const favStored = isFavorite(favoriteKey());
        setStepLED(4, favActive ? LED_ACTIVE : (favStored ? White : LED_DIM));

        if (mode === 'performance') {
            setStepLED(5, partBank ? LED_ACTIVE : LED_DIM);
        } else {
            setStepLED(5, LED_OFF);
        }

        setStepLED(6, octaveTranspose <= -4 ? LED_OFF : LED_DIM);
        setStepLED(7, octaveTranspose >= 4 ? LED_OFF : LED_DIM);
        setStepLED(8, semitoneTranspose <= -12 ? LED_OFF : LED_DIM);
        setStepLED(9, semitoneTranspose >= 12 ? LED_OFF : LED_DIM);
        setStepLED(10, velocityMode ? LED_ACTIVE : LED_DIM);
        setStepLED(11, midiMonitor ? LED_ACTIVE : LED_DIM);
        setStepLED(12, LED_DIM);
        setStepLED(13, localAudition ? LED_ACTIVE : LED_DIM);
        setStepLED(14, sysExRx ? LED_ACTIVE : LED_OFF);
        setStepLED(15, uiState === 'utility' ? LED_ACTIVE : LED_DIM);
        return;
    }

    for (let i = 0; i < PAGE_ORDER.length; i++) {
        const pageId = PAGE_ORDER[i];
        if (!isPageAvailable(pageId)) {
            setStepLED(i, LED_OFF);
            continue;
        }
        setStepLED(i, pageId === currentPage ? LED_ACTIVE : LED_DIM);
    }
}

function updateButtonLEDs() {
    setButtonLED(CC_MENU, uiState === 'play' ? LED_DIM : LED_ACTIVE, true);
    setButtonLED(CC_BACK, LED_DIM, true);
    setButtonLED(CC_REC, midiMonitor ? LED_ACTIVE : LED_OFF, true);
    setButtonLED(CC_LOOP, sustainLatch ? LED_ACTIVE : LED_OFF, true);
    setButtonLED(CC_PLAY, LED_DIM, true);
    const muted = mode === 'patch' ? !toneEnabled[selectedTone] : !partEnabled[selectedPart];
    setButtonLED(CC_MUTE, muted ? LED_ALERT : LED_OFF, true);
    setButtonLED(CC_COPY, LED_DIM, true);
    setButtonLED(CC_DELETE, LED_DIM, true);
    setButtonLED(CC_UNDO, LED_DIM, true);
}

function updateTrackLEDs() {
    for (let i = 0; i < TRACK_NOTES.length; i++) {
        const note = TRACK_NOTES[i];
        if (mode === 'patch') {
            const enabled = !!toneEnabled[i];
            const selected = selectedTone === i;
            const color = enabled ? (selected ? LED_ACTIVE : LED_DIM) : LED_OFF;
            setLED(note, color, true);
        } else {
            const part = clamp(partBank * 4 + i, 0, 7);
            const enabled = !!partEnabled[part];
            const selected = selectedPart === part;
            const color = enabled ? (selected ? LED_ACTIVE : LED_DIM) : LED_OFF;
            setLED(note, color, true);
        }
    }
}

function sendAllNotesOff() {
    for (let ch = 0; ch < 16; ch++) {
        sendCC(ch, 123, 0);
    }
}

function sendResetControllers() {
    for (let ch = 0; ch < 16; ch++) {
        sendCC(ch, 121, 0);
    }
}

function sendSustain(on) {
    const value = on ? 127 : 0;
    for (let ch = 0; ch < 16; ch++) {
        sendCC(ch, 64, value);
    }
}

function setOctave(delta) {
    octaveTranspose = clamp(octaveTranspose + delta, -4, 4);
    host_module_set_param('octave_transpose', String(octaveTranspose));
    setActivity(`OCT ${octaveTranspose >= 0 ? '+' : ''}${octaveTranspose}`);
}

function setTranspose(delta) {
    semitoneTranspose = clamp(semitoneTranspose + delta, -12, 12);
    setActivity(`TRANS ${semitoneTranspose >= 0 ? '+' : ''}${semitoneTranspose}`);
}

function setPreset(index) {
    if (index < 0) index = totalPatches - 1;
    if (index >= totalPatches) index = 0;
    currentPreset = index;
    host_module_set_param('program_change', String(currentPreset));
    setActivity(`PATCH ${currentPreset + 1}`);
    updateStepLEDs();
    needsRedraw = true;
}

function jumpBank(direction) {
    host_module_set_param(direction > 0 ? 'next_bank' : 'prev_bank', '1');
    setActivity(direction > 0 ? 'BANK +' : 'BANK -');
    needsRedraw = true;
}

function getParamStoreKey(param, target) {
    const targetId = target !== undefined ? target : 0;
    return `${param.scope}:${param.key}:${targetId}`;
}

function getParamValue(param, target) {
    if (param.scope === 'ui' && param.key === 'toneSelect') {
        return selectedTone;
    }
    const key = getParamStoreKey(param, target);
    if (paramValues.has(key)) return paramValues.get(key);
    if (typeof param.default === 'number') return param.default;
    return 0;
}

function setParamValue(param, target, value) {
    const key = getParamStoreKey(param, target);
    paramValues.set(key, value);
}

function resolveParamTarget(param) {
    if (param.scope === 'tone') return selectedTone;
    if (param.scope === 'part') {
        if (typeof param.partOffset === 'number') {
            return clamp(partBank * 4 + param.partOffset, 0, 7);
        }
        return selectedPart;
    }
    if (param.scope === 'drum') return lastDrumNote;
    return 0;
}

function sendParamValue(param, value) {
    const v = clampValue(value, param.min ?? 0, param.max ?? (param.twoByte ? 2031 : 127));
    if (param.scope === 'ui') {
        if (param.key === 'toneSelect') {
            selectedTone = clamp(v, 0, 3);
            setParamValue(param, 0, selectedTone);
            setActivity(`TONE ${selectedTone + 1}`);
            return;
        }
        return;
    }

    if (param.scope === 'patchCommon') {
        sendSysEx(buildPatchCommonParam(param.key, v));
    } else if (param.scope === 'tone') {
        sendSysEx(buildToneParam(selectedTone, param.key, v));
    } else if (param.scope === 'performanceCommon') {
        sendSysEx(buildPerformanceCommonParam(param.key, v));
    } else if (param.scope === 'part') {
        const part = resolveParamTarget(param);
        sendSysEx(buildPartParam(part, param.key, v));
    } else if (param.scope === 'drum') {
        sendSysEx(buildDrumParam(resolveParamTarget(param), param.key, v));
    }

    setParamValue(param, resolveParamTarget(param), v);
}

function applyParamDelta(param, delta, fine, encoderIndex) {
    if (!param) return;
    const step = fine ? (param.fineStep ?? 1) : (param.step ?? 1);
    const target = resolveParamTarget(param);
    const current = getParamValue(param, target);
    const next = clamp(current + delta * step, param.min ?? 0, param.max ?? (param.twoByte ? 2031 : 127));
    sendParamValue(param, next);
    const displayValue = formatParamValue(param, next);
    setActivity(`E${encoderIndex + 1} ${param.label} ${displayValue}`);
}

function setPage(pageId) {
    if (!PAGE_ORDER.includes(pageId)) return;
    currentPage = pageId;
    selectedParamIndex = 0;
    updateStepLEDs();
    setHelp(`PAGE ${pageId.toUpperCase()}`);
}

function moveParamSelection(delta) {
    const params = getCurrentPageParams();
    if (params.length === 0) return;
    const next = clamp(selectedParamIndex + delta, 0, params.length - 1);
    selectedParamIndex = next;
    const param = params[next];
    if (param) setActivity(`SEL ${param.label}`);
}

function getHudLine1() {
    if (pendingConfirm) return `CONFIRM ${pendingConfirm.message}`;
    if (lastActivity.text && lastActivity.until > nowMs()) return lastActivity.text;
    return '';
}

function getHudLine2() {
    const modeLabel = mode === 'performance' ? 'PERF' : 'PATCH';
    const focus = mode === 'performance'
        ? (rhythmFocus || selectedPart === 7 ? 'Rhythm' : `Part ${selectedPart + 1}`)
        : `Tone ${selectedTone + 1}`;
    const rx = sysExRx ? 'RX ON' : 'RX OFF';
    const patch = patchName || '---';
    return `${modeLabel} ${patch} | ${focus} | ${rx}`;
}

function formatMacroLine(startIndex) {
    const parts = [];
    for (let i = 0; i < 4; i++) {
        const macro = PLAY_MACROS[startIndex + i];
        if (!macro) continue;
        const label = (macro.short || macro.label || '').slice(0, 4);
        parts.push(label.padEnd(4, ' '));
    }
    return parts.join(' ').trimEnd();
}

function getFavoritesLines() {
    if (favorites.length === 0) {
        return {
            line3: 'FAVORITES EMPTY',
            line4: 'SHIFT+FAV TO ADD'
        };
    }
    const fav = favorites[favoritesIndex];
    if (!fav) {
        return {
            line3: 'FAVORITES EMPTY',
            line4: 'SHIFT+FAV TO ADD'
        };
    }
    const modeLabel = fav.mode === 'performance' ? 'PERF' : 'PATCH';
    const preset = fav.preset + 1;
    const name = fav.name || '---';
    return {
        line3: `FAV ${favoritesIndex + 1}/${favorites.length} ${modeLabel} ${preset}`,
        line4: name
    };
}

/* === Page Definitions === */
const pages = {
    home: {
        label: 'HOME',
        params: [
            { label: 'Tone', key: 'toneSelect', scope: 'ui', min: 0, max: 3 },
            { label: 'Enable', key: 'toneswitch', scope: 'tone', min: 0, max: 1 },
            { label: 'Cutoff', key: 'cutofffrequency', scope: 'tone' },
            { label: 'Reso', key: 'resonance', scope: 'tone' },
            { label: 'Attack', key: 'tvaenvtime1', scope: 'tone' },
            { label: 'Release', key: 'tvaenvtime4', scope: 'tone' },
            { label: 'Level', key: 'level', scope: 'tone' },
            { label: 'Pan', key: 'pan', scope: 'tone', twoByte: true, max: 2031 }
        ]
    },
    common: {
        label: 'COMMON',
        params: [
            { label: 'Level', key: 'patchlevel', scope: 'patchCommon' },
            { label: 'Pan', key: 'patchpanning', scope: 'patchCommon' },
            { label: 'Poly', key: 'keyassign', scope: 'patchCommon', min: 0, max: 1 },
            { label: 'Porta', key: 'portamentoswitch', scope: 'patchCommon', min: 0, max: 1 },
            { label: 'Bend Up', key: 'bendrangeup', scope: 'patchCommon' },
            { label: 'Bend Dn', key: 'bendrangedown', scope: 'patchCommon' },
            { label: 'Porta T', key: 'portamentotime', scope: 'patchCommon' },
            { label: 'Tune', key: 'analogfeel', scope: 'patchCommon' }
        ]
    },
    tone: {
        label: 'TONE/WG',
        params: [
            { label: 'WaveGrp', key: 'wavegroup', scope: 'tone' },
            { label: 'Wave#', key: 'wavenumber', scope: 'tone', twoByte: true, max: 2031 },
            { label: 'Level', key: 'level', scope: 'tone' },
            { label: 'Pan', key: 'pan', scope: 'tone', twoByte: true, max: 2031 },
            { label: 'Coarse', key: 'pitchcoarse', scope: 'tone' },
            { label: 'Fine', key: 'pitchfine', scope: 'tone' },
            { label: 'KeyF', key: 'pitchkeyfollow', scope: 'tone' },
            { label: 'Tone Sw', key: 'toneswitch', scope: 'tone', min: 0, max: 1 }
        ]
    },
    pitch: {
        label: 'PITCH',
        params: [
            { label: 'Depth', key: 'penvdepth', scope: 'tone' },
            { label: 'A', key: 'penvtime1', scope: 'tone' },
            { label: 'D', key: 'penvtime2', scope: 'tone' },
            { label: 'S', key: 'penvlevel3', scope: 'tone' },
            { label: 'R', key: 'penvtime4', scope: 'tone' },
            { label: 'LFO1', key: 'lfo1pitchdepth', scope: 'tone' },
            { label: 'LFO2', key: 'lfo2pitchdepth', scope: 'tone' },
            { label: 'KeyF', key: 'penvtimekeyfollow', scope: 'tone' }
        ]
    },
    tvf: {
        label: 'TVF',
        params: [
            { label: 'Cutoff', key: 'cutofffrequency', scope: 'tone' },
            { label: 'Reso', key: 'resonance', scope: 'tone' },
            { label: 'Env', key: 'tvfenvdepth', scope: 'tone' },
            { label: 'A', key: 'tvfenvtime1', scope: 'tone' },
            { label: 'D', key: 'tvfenvtime2', scope: 'tone' },
            { label: 'S', key: 'tvfenvlevel3', scope: 'tone' },
            { label: 'R', key: 'tvfenvtime4', scope: 'tone' },
            { label: 'KeyF', key: 'cutoffkeyfollow', scope: 'tone' }
        ]
    },
    tva: {
        label: 'TVA',
        params: [
            { label: 'Level', key: 'level', scope: 'tone' },
            { label: 'Vel', key: 'tvaenvvelocitylevelsense', scope: 'tone' },
            { label: 'A', key: 'tvaenvtime1', scope: 'tone' },
            { label: 'D', key: 'tvaenvtime2', scope: 'tone' },
            { label: 'S', key: 'tvaenvlevel3', scope: 'tone' },
            { label: 'R', key: 'tvaenvtime4', scope: 'tone' },
            { label: 'Pan', key: 'pan', scope: 'tone', twoByte: true, max: 2031 },
            { label: 'Bias', key: 'levelkeyfollow', scope: 'tone' }
        ]
    },
    lfo: {
        label: 'LFO',
        params: [
            { label: 'Wave', key: () => `lfo${lfoIndex}form`, scope: 'tone' },
            { label: 'Rate', key: () => `lfo${lfoIndex}rate`, scope: 'tone' },
            { label: 'Delay', key: () => `lfo${lfoIndex}delay`, scope: 'tone', twoByte: true, max: 2031 },
            { label: 'Fade', key: () => `lfo${lfoIndex}fadetime`, scope: 'tone' },
            { label: 'Sync', key: () => `lfo${lfoIndex}synchro`, scope: 'tone' },
            { label: 'Pitch', key: () => `lfo${lfoIndex}pitchdepth`, scope: 'tone' },
            { label: 'Filter', key: () => `lfo${lfoIndex}tvfdepth`, scope: 'tone' },
            { label: 'Amp', key: () => `lfo${lfoIndex}tvadepth`, scope: 'tone' }
        ]
    },
    outfx: {
        label: 'OUT/FX',
        params: [
            { label: 'Output', key: 'outputselect', scope: 'tone' },
            { label: 'Dry', key: 'drylevel', scope: 'tone' },
            { label: 'Chorus', key: 'chorussendlevel', scope: 'tone' },
            { label: 'Reverb', key: 'reverbsendlevel', scope: 'tone' },
            { label: 'Tone Sw', key: 'toneswitch', scope: 'tone', min: 0, max: 1 },
            { label: 'Vol Sw', key: 'volumeswitch', scope: 'tone', min: 0, max: 1 },
            { label: 'FXM Sw', key: 'fxmswitch', scope: 'tone', min: 0, max: 1 },
            { label: 'FXM D', key: 'fxmdepth', scope: 'tone' }
        ]
    },
    mod: {
        label: 'MOD',
        params: [
            { label: 'Dst1', key: 'modulationdestination1', scope: 'tone' },
            { label: 'Amt1', key: 'modulationsense1', scope: 'tone' },
            { label: 'Dst2', key: 'modulationdestination2', scope: 'tone' },
            { label: 'Amt2', key: 'modulationsense2', scope: 'tone' },
            { label: 'Dst3', key: 'modulationdestination3', scope: 'tone' },
            { label: 'Amt3', key: 'modulationsense3', scope: 'tone' },
            { label: 'Dst4', key: 'modulationdestination4', scope: 'tone' },
            { label: 'Amt4', key: 'modulationsense4', scope: 'tone' }
        ]
    },
    ctrl: {
        label: 'CTRL',
        params: [
            { label: 'AT D1', key: 'aftertouchdestination1', scope: 'tone' },
            { label: 'AT A1', key: 'aftertouchsense1', scope: 'tone' },
            { label: 'AT D2', key: 'aftertouchdestination2', scope: 'tone' },
            { label: 'AT A2', key: 'aftertouchsense2', scope: 'tone' },
            { label: 'Ex D1', key: 'expressiondestination1', scope: 'tone' },
            { label: 'Ex A1', key: 'expressionsense1', scope: 'tone' },
            { label: 'Ex D2', key: 'expressiondestination2', scope: 'tone' },
            { label: 'Ex A2', key: 'expressionsense2', scope: 'tone' }
        ]
    },
    struct: {
        label: 'STRUCT',
        params: [
            { label: 'Vel Sw', key: 'velocityswitch', scope: 'patchCommon', min: 0, max: 1 },
            { label: 'Vel Lo', key: 'velocityrangelower', scope: 'tone' },
            { label: 'Vel Hi', key: 'velocityrangeupper', scope: 'tone' },
            { label: 'FXM Sw', key: 'fxmswitch', scope: 'tone', min: 0, max: 1 },
            { label: 'FXM D', key: 'fxmdepth', scope: 'tone' },
            { label: 'Tone Sw', key: 'toneswitch', scope: 'tone', min: 0, max: 1 },
            { label: 'Hold1', key: 'hold1switch', scope: 'tone', min: 0, max: 1 },
            { label: 'Vol Sw', key: 'volumeswitch', scope: 'tone', min: 0, max: 1 }
        ]
    },
    mix: {
        label: 'MIX',
        params: [
            { label: 'P1 Lv', key: 'partlevel', scope: 'part', partOffset: 0 },
            { label: 'P2 Lv', key: 'partlevel', scope: 'part', partOffset: 1 },
            { label: 'P3 Lv', key: 'partlevel', scope: 'part', partOffset: 2 },
            { label: 'P4 Lv', key: 'partlevel', scope: 'part', partOffset: 3 },
            { label: 'P1 Pan', key: 'partpan', scope: 'part', partOffset: 0 },
            { label: 'P2 Pan', key: 'partpan', scope: 'part', partOffset: 1 },
            { label: 'P3 Pan', key: 'partpan', scope: 'part', partOffset: 2 },
            { label: 'P4 Pan', key: 'partpan', scope: 'part', partOffset: 3 }
        ]
    },
    part: {
        label: 'PART',
        params: [
            { label: 'Patch', key: 'patchnumber', scope: 'part', twoByte: true, max: 2031 },
            { label: 'MIDI Ch', key: 'receivechannel', scope: 'part' },
            { label: 'Key Lo', key: 'internalkeyrangelower', scope: 'part' },
            { label: 'Key Hi', key: 'internalkeyrangeupper', scope: 'part' },
            { label: 'Vel Lo', key: 'internalvelocitysense', scope: 'part' },
            { label: 'Vel Hi', key: 'internalvelocitymax', scope: 'part' },
            { label: 'Trans', key: 'internalkeytranspose', scope: 'part' },
            { label: 'Detune', key: 'partfinetune', scope: 'part' }
        ]
    },
    rhythm: {
        label: 'RHYTHM',
        params: [
            { label: 'Wave#', key: 'wavenumber', scope: 'drum', twoByte: true, max: 2031 },
            { label: 'Level', key: 'level', scope: 'drum' },
            { label: 'Pan', key: 'pan', scope: 'drum', twoByte: true, max: 2031 },
            { label: 'Coarse', key: 'coarsetune', scope: 'drum' },
            { label: 'Fine', key: 'pitchfine', scope: 'drum' },
            { label: 'Cutoff', key: 'cutoff', scope: 'drum' },
            { label: 'Reso', key: 'resonance', scope: 'drum' },
            { label: 'Decay', key: 'tvaenvtime4', scope: 'drum' }
        ]
    },
    fx: {
        label: 'FX',
        params: []
    },
    util: {
        label: 'UTIL',
        params: []
    }
};

const mixSendParams = [
    { label: 'P1 Rev', key: 'reverbswitch', scope: 'part', partOffset: 0, min: 0, max: 1 },
    { label: 'P2 Rev', key: 'reverbswitch', scope: 'part', partOffset: 1, min: 0, max: 1 },
    { label: 'P3 Rev', key: 'reverbswitch', scope: 'part', partOffset: 2, min: 0, max: 1 },
    { label: 'P4 Rev', key: 'reverbswitch', scope: 'part', partOffset: 3, min: 0, max: 1 },
    { label: 'P1 Cho', key: 'chorusswitch', scope: 'part', partOffset: 0, min: 0, max: 1 },
    { label: 'P2 Cho', key: 'chorusswitch', scope: 'part', partOffset: 1, min: 0, max: 1 },
    { label: 'P3 Cho', key: 'chorusswitch', scope: 'part', partOffset: 2, min: 0, max: 1 },
    { label: 'P4 Cho', key: 'chorusswitch', scope: 'part', partOffset: 3, min: 0, max: 1 }
];

const patchFxParams = [
    { label: 'Rev Type', key: 'reverbtype', scope: 'patchCommon' },
    { label: 'Rev Lvl', key: 'reverblevel', scope: 'patchCommon' },
    { label: 'Rev Time', key: 'reverbtime', scope: 'patchCommon' },
    { label: 'Rev FB', key: 'reverbfeedback', scope: 'patchCommon' },
    { label: 'Cho Type', key: 'chorustype', scope: 'patchCommon' },
    { label: 'Cho Lvl', key: 'choruslevel', scope: 'patchCommon' },
    { label: 'Cho Dep', key: 'chorusdepth', scope: 'patchCommon' },
    { label: 'Cho Rate', key: 'chorusrate', scope: 'patchCommon' }
];

const perfFxParams = [
    { label: 'Rev Type', key: 'reverbtype', scope: 'performanceCommon' },
    { label: 'Rev Lvl', key: 'reverblevel', scope: 'performanceCommon' },
    { label: 'Rev Time', key: 'reverbtime', scope: 'performanceCommon' },
    { label: 'Rev FB', key: 'reverbfeedback', scope: 'performanceCommon' },
    { label: 'Cho Type', key: 'chorustype', scope: 'performanceCommon' },
    { label: 'Cho Lvl', key: 'choruslevel', scope: 'performanceCommon' },
    { label: 'Cho Dep', key: 'chorusdepth', scope: 'performanceCommon' },
    { label: 'Cho Rate', key: 'chorusrate', scope: 'performanceCommon' }
];

function getCurrentPageParams() {
    if (currentPage === 'fx') return mode === 'performance' ? perfFxParams : patchFxParams;
    const page = pages[currentPage];
    if (!page) return [];
    if (currentPage === 'mix') {
        if (mode !== 'performance') return [];
        if (shiftHeld) return mixSendParams;
    }
    if (currentPage === 'part' && mode !== 'performance') return [];
    if (currentPage === 'rhythm' && mode !== 'performance') return [];
    return page.params.map((param) => {
        if (typeof param.key === 'function') {
            return { ...param, key: param.key() };
        }
        return param;
    });
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

    const line1 = getHudLine1();
    const line2 = getHudLine2();
    const helpActive = helpLine && helpUntilMs > nowMs();
    let line3 = lcdLine0;
    let line4 = lcdLine1;

    if (uiState === 'play') {
        if (favoritesView) {
            const favLines = getFavoritesLines();
            line3 = favLines.line3;
            line4 = favLines.line4;
        } else {
            line3 = formatMacroLine(0);
            line4 = formatMacroLine(4);
        }
    }

    if (helpActive) {
        line4 = helpLine;
    }

    const y1 = 1;
    const y2 = 16;
    const y3 = 31;
    const y4 = 46;

    print(1, y1, line1, 1);
    print(1, y2, line2, 1);

    if (uiState !== 'play') {
        fill_rect(0, y3 - 2, SCREEN_WIDTH, LCD_LINE_HEIGHT, 1);
        print(1, y3, line3, 0);
        if (helpActive) {
            print(1, y4, line4, 1);
        } else {
            fill_rect(0, y4 - 2, SCREEN_WIDTH, LCD_LINE_HEIGHT, 1);
            print(1, y4, line4, 0);
        }
    } else {
        print(1, y3, line3, 1);
        print(1, y4, line4, 1);
    }

    needsRedraw = false;
}

/* === MIDI Handling === */
function handleEncoder(cc, value, encoderIndex) {
    const delta = decodeDelta(value);
    if (!delta) return false;

    if (uiState === 'play') {
        const param = PLAY_MACROS[encoderIndex];
        applyParamDelta(param, delta, shiftHeld, encoderIndex);
        return true;
    }

    const params = getCurrentPageParams();
    const param = params[encoderIndex];
    applyParamDelta(param, delta, shiftHeld, encoderIndex);
    return true;
}

function handleJog(value) {
    const delta = value === 1 ? 1 : (value === 127 || value === 65 ? -1 : 0);
    if (!delta) return false;

    if (uiState === 'play') {
        if (favoritesView) {
            if (favorites.length === 0) return true;
            favoritesIndex = clamp(favoritesIndex + delta, 0, favorites.length - 1);
            setActivity(`FAV ${favoritesIndex + 1}/${favorites.length}`);
            return true;
        }
        setPreset(currentPreset + delta);
        return true;
    }

    const params = getCurrentPageParams();
    const param = params[selectedParamIndex];
    applyParamDelta(param, delta, shiftHeld, selectedParamIndex);
    return true;
}

function handleStep(stepIndex) {
    if (uiState === 'play') {
        if (shiftHeld && stepIndex === 4) {
            toggleFavorite();
            return true;
        }
        switch (stepIndex) {
            case 0:
                setMode('patch', true);
                updateStepLEDs();
                return true;
            case 1:
                setMode('performance', true);
                updateStepLEDs();
                return true;
            case 2:
                if (mode !== 'performance') {
                    setMode('performance');
                }
                rhythmFocus = !rhythmFocus;
                if (rhythmFocus) {
                    selectedPart = 7;
                    partBank = 1;
                    setActivity('RHYTHM FOCUS');
                } else {
                    setActivity('RHYTHM OFF');
                }
                updateTrackLEDs();
                updateStepLEDs();
                return true;
            case 3:
                setState('edit');
                setPage('fx');
                return true;
            case 4:
                favoritesView = !favoritesView;
                favoritesIndex = 0;
                setActivity(favoritesView ? 'FAVORITES' : 'FAV EXIT');
                if (favoritesView) {
                    setHelp(favorites.length ? 'JOG=SELECT MENU=LOAD' : 'SHIFT+FAV TO ADD', 2000);
                }
                updateStepLEDs();
                return true;
            case 5:
                partBank = partBank === 0 ? 1 : 0;
                setActivity(`BANK ${partBank + 1}`);
                updateTrackLEDs();
                updateStepLEDs();
                return true;
            case 6:
                setOctave(-1);
                updateStepLEDs();
                return true;
            case 7:
                setOctave(1);
                updateStepLEDs();
                return true;
            case 8:
                setTranspose(-1);
                updateStepLEDs();
                return true;
            case 9:
                setTranspose(1);
                updateStepLEDs();
                return true;
            case 10:
                velocityMode = !velocityMode;
                setActivity(velocityMode ? 'VEL MODE ON' : 'VEL MODE OFF');
                updateStepLEDs();
                return true;
            case 11:
                midiMonitor = !midiMonitor;
                setActivity(midiMonitor ? 'MONITOR ON' : 'MONITOR OFF');
                updateStepLEDs();
                updateButtonLEDs();
                return true;
            case 12:
                setState('edit');
                setPage('outfx');
                return true;
            case 13:
                localAudition = !localAudition;
                setActivity(localAudition ? 'LOCAL ON' : 'LOCAL OFF');
                updateStepLEDs();
                return true;
            case 14:
                sysExRx = !sysExRx;
                setActivity(sysExRx ? 'SYSEX RX' : 'SYSEX THRU');
                updateStepLEDs();
                return true;
            case 15:
                setState('utility');
                setPage('util');
                return true;
            default:
                break;
        }
    }

    if (uiState === 'edit' || uiState === 'utility') {
        if (shiftHeld && stepIndex === 6) {
            lfoIndex = lfoIndex === 1 ? 2 : 1;
            setActivity(`LFO ${lfoIndex}`);
            return true;
        }
        const pageId = PAGE_ORDER[stepIndex];
        if (pageId) setPage(pageId);
        return true;
    }

    return false;
}

function handleTrack(trackIndex) {
    if (mode === 'patch') {
        selectedTone = clamp(trackIndex, 0, 3);
        updateTrackLEDs();
        updateButtonLEDs();
        setActivity(`TONE ${selectedTone + 1}`);
        return true;
    }

    const part = clamp(partBank * 4 + trackIndex, 0, 7);
    selectedPart = part;
    rhythmFocus = selectedPart === 7;
    updateTrackLEDs();
    updateButtonLEDs();
    setActivity(`PART ${selectedPart + 1}`);
    return true;
}

function handleTrackShift(trackIndex) {
    if (mode === 'patch') {
        const tone = clamp(trackIndex, 0, 3);
        selectedTone = tone;
        toneEnabled[tone] = toneEnabled[tone] ? 0 : 1;
        sendSysEx(buildToneParam(tone, 'toneswitch', toneEnabled[tone]));
        updateTrackLEDs();
        updateButtonLEDs();
        setActivity(`TONE ${tone + 1} ${toneEnabled[tone] ? 'ON' : 'MUTE'}`);
        return true;
    }

    const part = clamp(partBank * 4 + trackIndex, 0, 7);
    selectedPart = part;
    rhythmFocus = selectedPart === 7;
    partEnabled[part] = partEnabled[part] ? 0 : 1;
    sendSysEx(buildPartParam(part, 'internalswitch', partEnabled[part]));
    updateTrackLEDs();
    updateButtonLEDs();
    setActivity(`PART ${part + 1} ${partEnabled[part] ? 'ON' : 'MUTE'}`);
    return true;
}

function handleCC(cc, value) {
    if (cc === CC_SHIFT) {
        shiftHeld = value > 0;
        return false;
    }

    if (cc === CC_MENU && value > 0) {
        if (pendingConfirm) {
            pendingConfirm.onConfirm();
            pendingConfirm = null;
            setActivity('CONFIRMED');
            return true;
        }
        if (uiState === 'play' && favoritesView && favorites.length > 0) {
            const entry = favorites[favoritesIndex];
            const normalized = normalizeFavorite(entry);
            if (normalized) {
                setMode(normalized.mode);
                setPreset(normalized.preset);
                favoritesView = false;
                setActivity('FAV LOAD');
                return true;
            }
        }
        if (shiftHeld) {
            setState('utility');
            setPage('util');
        } else {
            setState('edit');
        }
        return true;
    }

    if (cc === CC_BACK && value > 0) {
        if (pendingConfirm) {
            pendingConfirm = null;
            setActivity('CANCELLED');
            return true;
        }
        if (uiState === 'play' && favoritesView) {
            favoritesView = false;
            setActivity('FAV EXIT');
            return true;
        }
        if (uiState !== 'play') {
            setState('play');
            return true;
        }
        return true;
    }

    if (cc === CC_LEFT && value > 0) {
        if (uiState === 'play') {
            if (favoritesView) {
                if (favorites.length === 0) return true;
                favoritesIndex = clamp(favoritesIndex - 1, 0, favorites.length - 1);
                setActivity(`FAV ${favoritesIndex + 1}/${favorites.length}`);
                return true;
            }
            if (shiftHeld) {
                jumpBank(-1);
                return true;
            }
            setPreset(currentPreset - 1);
            return true;
        }
        moveParamSelection(-1);
        return true;
    }

    if (cc === CC_RIGHT && value > 0) {
        if (uiState === 'play') {
            if (favoritesView) {
                if (favorites.length === 0) return true;
                favoritesIndex = clamp(favoritesIndex + 1, 0, favorites.length - 1);
                setActivity(`FAV ${favoritesIndex + 1}/${favorites.length}`);
                return true;
            }
            if (shiftHeld) {
                jumpBank(1);
                return true;
            }
            setPreset(currentPreset + 1);
            return true;
        }
        moveParamSelection(1);
        return true;
    }

    if (cc === CC_UP && value > 0) {
        if (uiState !== 'play') {
            moveParamSelection(-4);
            return true;
        }
        return true;
    }

    if (cc === CC_DOWN && value > 0) {
        if (uiState !== 'play') {
            moveParamSelection(4);
            return true;
        }
        return true;
    }

    if (cc === CC_JOG) {
        return handleJog(value);
    }

    const encIndex = ENCODER_CCS.indexOf(cc);
    if (encIndex >= 0) {
        return handleEncoder(cc, value, encIndex);
    }

    if (cc === CC_PLAY && value > 0) {
        setActivity('AUDITION');
        return true;
    }

    if (cc === CC_REC && value > 0) {
        midiMonitor = !midiMonitor;
        setActivity(midiMonitor ? 'MONITOR ON' : 'MONITOR OFF');
        updateStepLEDs();
        updateButtonLEDs();
        return true;
    }

    if (cc === CC_LOOP && value > 0) {
        sustainLatch = !sustainLatch;
        sendSustain(sustainLatch);
        setActivity(sustainLatch ? 'SUSTAIN ON' : 'SUSTAIN OFF');
        updateButtonLEDs();
        return true;
    }

    if (cc === CC_CAPTURE && value > 0) {
        setActivity('COMPARE');
        return true;
    }

    if (cc === CC_RECORD && value > 0) {
        if (shiftHeld) {
            sendResetControllers();
            setActivity('RESET CTRL');
        } else {
            sendAllNotesOff();
            setActivity('PANIC');
        }
        return true;
    }

    if (cc === CC_MUTE && value > 0) {
        if (mode === 'patch') {
            toneEnabled[selectedTone] = toneEnabled[selectedTone] ? 0 : 1;
            sendSysEx(buildToneParam(selectedTone, 'toneswitch', toneEnabled[selectedTone]));
            updateTrackLEDs();
            updateButtonLEDs();
            setActivity(toneEnabled[selectedTone] ? 'TONE ON' : 'TONE MUTE');
        } else {
            partEnabled[selectedPart] = partEnabled[selectedPart] ? 0 : 1;
            sendSysEx(buildPartParam(selectedPart, 'internalswitch', partEnabled[selectedPart]));
            updateTrackLEDs();
            updateButtonLEDs();
            setActivity(partEnabled[selectedPart] ? 'PART ON' : 'PART MUTE');
        }
        return true;
    }

    if (cc === CC_COPY && value > 0) {
        setActivity('COPY');
        return true;
    }

    if (cc === CC_DELETE && value > 0) {
        pendingConfirm = {
            message: 'INIT? MENU=YES',
            onConfirm: () => setActivity('INIT N/A')
        };
        return true;
    }

    if (cc === CC_UNDO && value > 0) {
        setActivity(shiftHeld ? 'REDO N/A' : 'UNDO N/A');
        return true;
    }

    return false;
}

function handleCapacitiveTouch(note, velocity) {
    if (velocity <= 0) return false;
    if (note >= 0 && note < ENCODER_CCS.length) {
        const params = uiState === 'play' ? PLAY_MACROS : getCurrentPageParams();
        const param = params[note];
        if (!param) return true;
        const target = resolveParamTarget(param);
        const current = getParamValue(param, target);
        const displayValue = formatParamValue(param, current);
        setActivity(`E${note + 1} ${param.label} ${displayValue}`);
        return true;
    }
    if (note === 8) {
        setActivity('JOG');
        return true;
    }
    return false;
}

function handleNoteOn(note, velocity, channel, source) {
    if (MoveSteps.includes(note)) {
        const stepIndex = note - MoveSteps[0];
        return handleStep(stepIndex);
    }

    if (TRACK_NOTES.includes(note)) {
        const trackIndex = TRACK_NOTES.indexOf(note);
        if (shiftHeld) return handleTrackShift(trackIndex);
        return handleTrack(trackIndex);
    }

    if (MovePads.includes(note)) {
        lastDrumNote = clamp(note, 36, 96);
    }

    if (velocity > 0 && (source === 'internal' || midiMonitor)) {
        const ch = clamp(channel + 1, 1, 16);
        setActivity(`IN Ch${ch} ${noteName(note)} V${velocity}`);
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

    const patch = host_module_get_param('current_patch');
    if (patch) {
        const p = parseInt(patch);
        if (p >= 0 && p !== currentPreset) {
            currentPreset = p;
            updateStepLEDs();
            needsRedraw = true;
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
    console.log('JV880 UI initializing...');
    favorites = loadFavorites();
    setMode(mode, true);
    updateTrackLEDs();
    updateStepLEDs();
    updateButtonLEDs();
    needsRedraw = true;
    updateFromDSP();
};

globalThis.tick = function() {
    updateFromDSP();

    tickCount++;
    if (needsRedraw || tickCount >= REDRAW_INTERVAL) {
        drawUI();
        tickCount = 0;
        needsRedraw = false;
    }
};

globalThis.onMidiMessageInternal = function(data) {
    if (isCapacitiveTouchMessage(data)) {
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
