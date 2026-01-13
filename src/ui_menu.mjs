/*
 * JV-880 Menu Definitions
 *
 * Defines all hierarchical menus using shared menu components.
 */

import {
    createSubmenu,
    createValue,
    createEnum,
    createToggle,
    createAction,
    createBack
} from '../../shared/menu_items.mjs';

/* === State accessors (set by ui.js) === */
let stateAccessor = null;

export function setStateAccessor(accessor) {
    stateAccessor = accessor;
}

function getState() {
    return stateAccessor ? stateAccessor() : {};
}

/* === Main Menu === */
export function getMainMenu() {
    return [
        createSubmenu('Browse Patches', () => getPatchBanksMenu()),
        createSubmenu('Browse Performances', () => getPerformancesMenu()),
        createSubmenu('Edit Current', () => getEditMenu()),
        createBack()
    ];
}

/* === Patch Browsing === */
export function getPatchBanksMenu() {
    const state = getState();
    const banks = state.getBanks ? state.getBanks() : [];

    return [
        ...banks.map(bank =>
            createSubmenu(bank.name, () => getPatchListMenu(bank.id, bank.name))
        ),
        createBack()
    ];
}

function getPatchListMenu(bankId, bankName) {
    const state = getState();
    const patches = state.getPatchesInBank ? state.getPatchesInBank(bankId) : [];

    return [
        ...patches.map((patch, index) =>
            createAction(`${index + 1}: ${patch.name}`, () => {
                if (state.loadPatch) {
                    state.loadPatch(bankId, patch.index);
                }
            })
        ),
        createBack()
    ];
}

/* === Performance Browsing === */
export function getPerformancesMenu() {
    const state = getState();
    /* 3 banks: Preset A (0-15), Preset B (16-31), Internal (32-47) */
    const banks = [
        { id: 0, name: 'Preset A', start: 0, count: 16 },
        { id: 1, name: 'Preset B', start: 16, count: 16 },
        { id: 2, name: 'Internal', start: 32, count: 16 }
    ];

    return [
        ...banks.map(bank =>
            createSubmenu(bank.name, () => getPerformanceListMenu(bank))
        ),
        createBack()
    ];
}

function getPerformanceListMenu(bank) {
    const state = getState();
    const items = [];

    for (let i = 0; i < bank.count; i++) {
        const perfIndex = bank.start + i;
        const name = state.getPerformanceName ? state.getPerformanceName(perfIndex) : `Perf ${i + 1}`;
        items.push(
            createAction(`${i + 1}: ${name}`, () => {
                if (state.loadPerformance) {
                    state.loadPerformance(perfIndex);
                }
            })
        );
    }

    items.push(createBack());
    return items;
}

/* === Edit Menu === */
export function getEditMenu() {
    const state = getState();
    const mode = state.mode || 'patch';

    if (mode === 'performance') {
        return getEditPerformanceMenu();
    }
    return getEditPatchMenu();
}

function getEditPatchMenu() {
    const state = getState();

    return [
        createSubmenu('Common', () => getPatchCommonMenu()),
        createSubmenu('Tone 1', () => getToneMenu(0)),
        createSubmenu('Tone 2', () => getToneMenu(1)),
        createSubmenu('Tone 3', () => getToneMenu(2)),
        createSubmenu('Tone 4', () => getToneMenu(3)),
        createSubmenu('Effects', () => getPatchFxMenu()),
        createSubmenu('Settings', () => getSettingsMenu()),
        createBack()
    ];
}

function getEditPerformanceMenu() {
    /* Performance editing not yet supported - NVRAM layout unknown */
    /* See docs/TODO-performance-editing.md for implementation plan */
    return [
        createAction('Performance edit not supported', () => {}),
        createAction('Use Patch mode for editing', () => {}),
        createSubmenu('Settings', () => getSettingsMenu()),
        createBack()
    ];
}

/* === Patch Common Menu === */
function getPatchCommonMenu() {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});

    return [
        createValue('Level', {
            get: () => getParam('patchCommon', 'patchlevel'),
            set: (v) => setParam('patchCommon', 'patchlevel', v),
            min: 0, max: 127
        }),
        createValue('Pan', {
            get: () => getParam('patchCommon', 'patchpanning'),
            set: (v) => setParam('patchCommon', 'patchpanning', v),
            min: 0, max: 127,
            format: (v) => v === 64 ? 'C' : (v < 64 ? `L${64 - v}` : `R${v - 64}`)
        }),
        createToggle('Portamento', {
            get: () => !!getParam('patchCommon', 'portamentoswitch'),
            set: (v) => setParam('patchCommon', 'portamentoswitch', v ? 1 : 0)
        }),
        createValue('Porta Time', {
            get: () => getParam('patchCommon', 'portamentotime'),
            set: (v) => setParam('patchCommon', 'portamentotime', v),
            min: 0, max: 127
        }),
        createValue('Bend Up', {
            get: () => getParam('patchCommon', 'bendrangeup'),
            set: (v) => setParam('patchCommon', 'bendrangeup', v),
            min: 0, max: 12
        }),
        createValue('Bend Down', {
            get: () => getParam('patchCommon', 'bendrangedown'),
            set: (v) => setParam('patchCommon', 'bendrangedown', v),
            min: 0, max: 48
        }),
        createBack()
    ];
}

/* Wave group names for display */
const WAVE_GROUPS = ['INT-A', 'INT-B', 'PCM', 'EXP-A', 'EXP-B', 'EXP-C', 'EXP-D'];

/* === Tone Menu === */
function getToneMenu(toneIndex) {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});
    const getToneParam = (key) => getParam('tone', key, toneIndex);
    const setToneParam = (key, v) => setParam('tone', key, v, toneIndex);

    return [
        createToggle('Enable', {
            get: () => !!getToneParam('toneswitch'),
            set: (v) => setToneParam('toneswitch', v ? 1 : 0)
        }),
        createEnum('Wave Group', {
            get: () => WAVE_GROUPS[getToneParam('wavegroup')] || 'INT-A',
            set: (v) => setToneParam('wavegroup', WAVE_GROUPS.indexOf(v)),
            options: WAVE_GROUPS
        }),
        createValue('Wave Number', {
            get: () => getToneParam('wavenumber'),
            set: (v) => setToneParam('wavenumber', v),
            min: 0, max: 255,
            step: 1,
            fineStep: 1
        }),
        createValue('Level', {
            get: () => getToneParam('level'),
            set: (v) => setToneParam('level', v),
            min: 0, max: 127
        }),
        createSubmenu('Filter (TVF)', () => getToneFilterMenu(toneIndex)),
        createSubmenu('Amp (TVA)', () => getToneAmpMenu(toneIndex)),
        createSubmenu('Pitch', () => getTonePitchMenu(toneIndex)),
        createSubmenu('LFO 1', () => getToneLFOMenu(toneIndex, 1)),
        createSubmenu('LFO 2', () => getToneLFOMenu(toneIndex, 2)),
        createSubmenu('Output/FX', () => getToneOutputMenu(toneIndex)),
        createBack()
    ];
}

/* === Tone Filter Menu === */
function getToneFilterMenu(toneIndex) {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});
    const getToneParam = (key) => getParam('tone', key, toneIndex);
    const setToneParam = (key, v) => setParam('tone', key, v, toneIndex);

    return [
        createValue('Cutoff', {
            get: () => getToneParam('cutofffrequency'),
            set: (v) => setToneParam('cutofffrequency', v),
            min: 0, max: 127
        }),
        createValue('Resonance', {
            get: () => getToneParam('resonance'),
            set: (v) => setToneParam('resonance', v),
            min: 0, max: 127
        }),
        createValue('Env Depth', {
            get: () => getToneParam('tvfenvdepth'),
            set: (v) => setToneParam('tvfenvdepth', v),
            min: 0, max: 127
        }),
        createValue('Attack', {
            get: () => getToneParam('tvfenvtime1'),
            set: (v) => setToneParam('tvfenvtime1', v),
            min: 0, max: 127
        }),
        createValue('Decay', {
            get: () => getToneParam('tvfenvtime2'),
            set: (v) => setToneParam('tvfenvtime2', v),
            min: 0, max: 127
        }),
        createValue('Sustain', {
            get: () => getToneParam('tvfenvlevel3'),
            set: (v) => setToneParam('tvfenvlevel3', v),
            min: 0, max: 127
        }),
        createValue('Release', {
            get: () => getToneParam('tvfenvtime4'),
            set: (v) => setToneParam('tvfenvtime4', v),
            min: 0, max: 127
        }),
        createValue('Key Follow', {
            get: () => getToneParam('cutoffkeyfollow'),
            set: (v) => setToneParam('cutoffkeyfollow', v),
            min: 0, max: 127
        }),
        createBack()
    ];
}

/* === Tone Amp Menu === */
function getToneAmpMenu(toneIndex) {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});
    const getToneParam = (key) => getParam('tone', key, toneIndex);
    const setToneParam = (key, v) => setParam('tone', key, v, toneIndex);

    return [
        createValue('Level', {
            get: () => getToneParam('level'),
            set: (v) => setToneParam('level', v),
            min: 0, max: 127
        }),
        createValue('Vel Sense', {
            get: () => getToneParam('tvaenvvelocitylevelsense'),
            set: (v) => setToneParam('tvaenvvelocitylevelsense', v),
            min: 0, max: 127
        }),
        createValue('Attack', {
            get: () => getToneParam('tvaenvtime1'),
            set: (v) => setToneParam('tvaenvtime1', v),
            min: 0, max: 127
        }),
        createValue('Decay', {
            get: () => getToneParam('tvaenvtime2'),
            set: (v) => setToneParam('tvaenvtime2', v),
            min: 0, max: 127
        }),
        createValue('Sustain', {
            get: () => getToneParam('tvaenvlevel3'),
            set: (v) => setToneParam('tvaenvlevel3', v),
            min: 0, max: 127
        }),
        createValue('Release', {
            get: () => getToneParam('tvaenvtime4'),
            set: (v) => setToneParam('tvaenvtime4', v),
            min: 0, max: 127
        }),
        createBack()
    ];
}

/* === Tone Pitch Menu === */
function getTonePitchMenu(toneIndex) {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});
    const getToneParam = (key) => getParam('tone', key, toneIndex);
    const setToneParam = (key, v) => setParam('tone', key, v, toneIndex);

    return [
        createValue('Coarse', {
            get: () => getToneParam('pitchcoarse'),
            set: (v) => setToneParam('pitchcoarse', v),
            min: 0, max: 127,
            format: (v) => v >= 64 ? `+${v - 64}` : `${v - 64}`
        }),
        createValue('Fine', {
            get: () => getToneParam('pitchfine'),
            set: (v) => setToneParam('pitchfine', v),
            min: 0, max: 127,
            format: (v) => v >= 64 ? `+${v - 64}` : `${v - 64}`
        }),
        createValue('Env Depth', {
            get: () => getToneParam('penvdepth'),
            set: (v) => setToneParam('penvdepth', v),
            min: 0, max: 127
        }),
        createValue('Attack', {
            get: () => getToneParam('penvtime1'),
            set: (v) => setToneParam('penvtime1', v),
            min: 0, max: 127
        }),
        createValue('Decay', {
            get: () => getToneParam('penvtime2'),
            set: (v) => setToneParam('penvtime2', v),
            min: 0, max: 127
        }),
        createValue('Release', {
            get: () => getToneParam('penvtime4'),
            set: (v) => setToneParam('penvtime4', v),
            min: 0, max: 127
        }),
        createBack()
    ];
}

/* === Tone LFO Menu === */
function getToneLFOMenu(toneIndex, lfoNum) {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});
    const getToneParam = (key) => getParam('tone', key, toneIndex);
    const setToneParam = (key, v) => setParam('tone', key, v, toneIndex);
    const prefix = `lfo${lfoNum}`;

    return [
        createValue('Rate', {
            get: () => getToneParam(`${prefix}rate`),
            set: (v) => setToneParam(`${prefix}rate`, v),
            min: 0, max: 127
        }),
        createValue('Delay', {
            get: () => getToneParam(`${prefix}delay`),
            set: (v) => setToneParam(`${prefix}delay`, v),
            min: 0, max: 127
        }),
        createValue('Fade', {
            get: () => getToneParam(`${prefix}fadetime`),
            set: (v) => setToneParam(`${prefix}fadetime`, v),
            min: 0, max: 127
        }),
        createValue('Pitch Depth', {
            get: () => getToneParam(`${prefix}pitchdepth`),
            set: (v) => setToneParam(`${prefix}pitchdepth`, v),
            min: 0, max: 127
        }),
        createValue('Filter Depth', {
            get: () => getToneParam(`${prefix}tvfdepth`),
            set: (v) => setToneParam(`${prefix}tvfdepth`, v),
            min: 0, max: 127
        }),
        createValue('Amp Depth', {
            get: () => getToneParam(`${prefix}tvadepth`),
            set: (v) => setToneParam(`${prefix}tvadepth`, v),
            min: 0, max: 127
        }),
        createBack()
    ];
}

/* === Tone Output Menu === */
function getToneOutputMenu(toneIndex) {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});
    const getToneParam = (key) => getParam('tone', key, toneIndex);
    const setToneParam = (key, v) => setParam('tone', key, v, toneIndex);

    return [
        createValue('Dry Level', {
            get: () => getToneParam('drylevel'),
            set: (v) => setToneParam('drylevel', v),
            min: 0, max: 127
        }),
        createValue('Chorus Send', {
            get: () => getToneParam('chorussendlevel'),
            set: (v) => setToneParam('chorussendlevel', v),
            min: 0, max: 127
        }),
        createValue('Reverb Send', {
            get: () => getToneParam('reverbsendlevel'),
            set: (v) => setToneParam('reverbsendlevel', v),
            min: 0, max: 127
        }),
        createBack()
    ];
}

/* === Patch FX Menu === */
function getPatchFxMenu() {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});

    return [
        createValue('Reverb Level', {
            get: () => getParam('patchCommon', 'reverblevel'),
            set: (v) => setParam('patchCommon', 'reverblevel', v),
            min: 0, max: 127
        }),
        createValue('Reverb Time', {
            get: () => getParam('patchCommon', 'reverbtime'),
            set: (v) => setParam('patchCommon', 'reverbtime', v),
            min: 0, max: 127
        }),
        createValue('Chorus Level', {
            get: () => getParam('patchCommon', 'choruslevel'),
            set: (v) => setParam('patchCommon', 'choruslevel', v),
            min: 0, max: 127
        }),
        createValue('Chorus Rate', {
            get: () => getParam('patchCommon', 'chorusrate'),
            set: (v) => setParam('patchCommon', 'chorusrate', v),
            min: 0, max: 127
        }),
        createValue('Chorus Depth', {
            get: () => getParam('patchCommon', 'chorusdepth'),
            set: (v) => setParam('patchCommon', 'chorusdepth', v),
            min: 0, max: 127
        }),
        createBack()
    ];
}

/* === Performance Common Menu === */
/* === Settings Menu === */
function getSettingsMenu() {
    const state = getState();

    return [
        createValue('Octave', {
            get: () => state.octaveTranspose || 0,
            set: (v) => { if (state.setOctave) state.setOctave(v); },
            min: -4, max: 4,
            format: (v) => v >= 0 ? `+${v}` : String(v)
        }),
        createToggle('Local Audition', {
            get: () => state.localAudition !== false,
            set: (v) => { if (state.setLocalAudition) state.setLocalAudition(v); }
        }),
        createToggle('MIDI Monitor', {
            get: () => !!state.midiMonitor,
            set: (v) => { if (state.setMidiMonitor) state.setMidiMonitor(v); }
        }),
        createToggle('SysEx Receive', {
            get: () => state.sysExRx !== false,
            set: (v) => { if (state.setSysExRx) state.setSysExRx(v); }
        }),
        createBack()
    ];
}
