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
} from '/data/UserData/move-anything/shared/menu_items.mjs';

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
    const state = getState();
    const selectedPart = state.selectedPart ?? 0;

    return [
        createSubmenu('Expansion Card', () => getExpansionCardMenu()),
        createSubmenu('Common', () => getPerformanceCommonMenu()),
        createSubmenu(`Part ${selectedPart + 1} Edit`, () => getPartMenu(selectedPart)),
        createSubmenu('All Parts', () => getAllPartsMenu()),
        createSubmenu('Save', () => getSavePerformanceMenu()),
        createSubmenu('Settings', () => getSettingsMenu()),
        createSubmenu('Debug', () => [
            createAction('Dump Part Values', () => {
                host_module_set_param('dump_part_values', '1');
            }),
            createAction('Dump Temp Perf', () => {
                host_module_set_param('dump_temp_perf', '1');
            }),
            createAction('Start Param Mapping', () => {
                host_module_set_param('start_param_mapping', '1');
            }),
            createAction('Stop Param Mapping', () => {
                host_module_set_param('stop_param_mapping', '1');
            }),
            createBack()
        ]),
        createBack()
    ];
}

/* === Expansion Card Menu === */
function getExpansionCardMenu() {
    const expansionCount = parseInt(host_module_get_param('expansion_count') || '0');
    const currentExpansion = parseInt(host_module_get_param('current_expansion') || '-1');
    const currentBankOffset = parseInt(host_module_get_param('expansion_bank_offset') || '0');
    const currentName = host_module_get_param('current_expansion_name') || 'None';

    const items = [];

    /* Show current card and bank */
    let currentLabel = currentName;
    if (currentExpansion >= 0) {
        const patchCount = parseInt(host_module_get_param(`expansion_${currentExpansion}_patch_count`) || '64');
        if (patchCount > 64) {
            currentLabel += ` (${currentBankOffset + 1}-${Math.min(currentBankOffset + 64, patchCount)})`;
        }
    }
    items.push(createAction(`Current: ${currentLabel}`, () => {}));

    /* Option to clear (no card) */
    items.push(createAction('(No Card)', () => {
        host_module_set_param('load_expansion', '-1');
    }));

    /* List available expansions with sub-banks for large ones */
    for (let i = 0; i < expansionCount; i++) {
        const name = host_module_get_param(`expansion_${i}_name`) || `Expansion ${i + 1}`;
        const patchCount = parseInt(host_module_get_param(`expansion_${i}_patch_count`) || '64');

        if (patchCount <= 64) {
            /* Single bank - show as action */
            const marker = (i === currentExpansion && currentBankOffset === 0) ? ' ✓' : '';
            items.push(createAction(`${name}${marker}`, () => {
                host_module_set_param('load_expansion', String(i));
            }));
        } else {
            /* Multiple banks - show as submenu */
            items.push(createSubmenu(name, () => {
                const bankItems = [];
                for (let bank = 0; bank < patchCount; bank += 64) {
                    const bankEnd = Math.min(bank + 64, patchCount);
                    const marker = (i === currentExpansion && currentBankOffset === bank) ? ' ✓' : '';
                    bankItems.push(createAction(`Patches ${bank + 1}-${bankEnd}${marker}`, () => {
                        host_module_set_param('load_expansion', `${i},${bank}`);
                    }));
                }
                bankItems.push(createBack());
                return bankItems;
            }));
        }
    }

    items.push(createBack());
    return items;
}

/* === Performance Common Menu === */
function getPerformanceCommonMenu() {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});

    return [
        createEnum('Key Mode', {
            get: () => getParam('performanceCommon', 'keymode'),
            set: (v) => setParam('performanceCommon', 'keymode', v),
            options: ['Layer', 'Zone', 'Single']
        }),
        createSubmenu('Reverb', () => [
            createEnum('Type', {
                get: () => getParam('performanceCommon', 'reverbtype'),
                set: (v) => setParam('performanceCommon', 'reverbtype', v),
                options: ['Room1', 'Room2', 'Stage1', 'Stage2', 'Hall1', 'Hall2', 'Delay', 'Pan-Delay']
            }),
            createValue('Level', {
                get: () => getParam('performanceCommon', 'reverblevel'),
                set: (v) => setParam('performanceCommon', 'reverblevel', v),
                min: 0, max: 127
            }),
            createValue('Time', {
                get: () => getParam('performanceCommon', 'reverbtime'),
                set: (v) => setParam('performanceCommon', 'reverbtime', v),
                min: 0, max: 127
            }),
            createValue('Feedback', {
                get: () => getParam('performanceCommon', 'reverbfeedback'),
                set: (v) => setParam('performanceCommon', 'reverbfeedback', v),
                min: 0, max: 127
            }),
            createBack()
        ]),
        createSubmenu('Chorus', () => [
            createEnum('Type', {
                get: () => getParam('performanceCommon', 'chorustype'),
                set: (v) => setParam('performanceCommon', 'chorustype', v),
                options: ['Chorus1', 'Chorus2', 'Chorus3', 'Chorus4', 'Feedback', 'Flanger', 'Delay', 'Delay FB']
            }),
            createValue('Level', {
                get: () => getParam('performanceCommon', 'choruslevel'),
                set: (v) => setParam('performanceCommon', 'choruslevel', v),
                min: 0, max: 127
            }),
            createValue('Depth', {
                get: () => getParam('performanceCommon', 'chorusdepth'),
                set: (v) => setParam('performanceCommon', 'chorusdepth', v),
                min: 0, max: 127
            }),
            createValue('Rate', {
                get: () => getParam('performanceCommon', 'chorusrate'),
                set: (v) => setParam('performanceCommon', 'chorusrate', v),
                min: 0, max: 127
            }),
            createValue('Feedback', {
                get: () => getParam('performanceCommon', 'chorusfeedback'),
                set: (v) => setParam('performanceCommon', 'chorusfeedback', v),
                min: 0, max: 127
            }),
            createEnum('Output', {
                get: () => getParam('performanceCommon', 'chorusoutput'),
                set: (v) => setParam('performanceCommon', 'chorusoutput', v),
                options: ['Mix', 'Rev', 'Mix+Rev']
            }),
            createBack()
        ]),
        createBack()
    ];
}

/* === Part Menu === */
function getPartMenu(partIndex) {
    const state = getState();
    const getParam = state.getParam || (() => 0);
    const setParam = state.setParam || (() => {});

    return [
        createSubmenu('Patch', () => getPartPatchMenu(partIndex)),
        createToggle('Internal', {
            get: () => !!getParam('part', 'internalswitch', partIndex),
            set: (v) => setParam('part', 'internalswitch', v ? 1 : 0, partIndex)
        }),
        createValue('Level', {
            get: () => getParam('part', 'partlevel', partIndex),
            set: (v) => setParam('part', 'partlevel', v, partIndex),
            min: 0, max: 127
        }),
        createValue('Pan', {
            get: () => getParam('part', 'partpan', partIndex),
            set: (v) => setParam('part', 'partpan', v, partIndex),
            min: 0, max: 127,
            format: (v) => v === 64 ? 'C' : (v < 64 ? `L${64 - v}` : `R${v - 64}`)
        }),
        createValue('Coarse Tune', {
            get: () => getParam('part', 'partcoarsetune', partIndex),
            set: (v) => setParam('part', 'partcoarsetune', v, partIndex),
            min: 16, max: 112,
            format: (v) => `${v - 64 >= 0 ? '+' : ''}${v - 64}`
        }),
        createValue('Fine Tune', {
            get: () => getParam('part', 'partfinetune', partIndex),
            set: (v) => setParam('part', 'partfinetune', v, partIndex),
            min: 14, max: 114,
            format: (v) => `${v - 64 >= 0 ? '+' : ''}${v - 64}`
        }),
        createToggle('Reverb', {
            get: () => !!getParam('part', 'reverbswitch', partIndex),
            set: (v) => setParam('part', 'reverbswitch', v ? 1 : 0, partIndex)
        }),
        createToggle('Chorus', {
            get: () => !!getParam('part', 'chorusswitch', partIndex),
            set: (v) => setParam('part', 'chorusswitch', v ? 1 : 0, partIndex)
        }),
        createSubmenu('Key Range', () => [
            createValue('Lower', {
                get: () => getParam('part', 'internalkeyrangelower', partIndex),
                set: (v) => setParam('part', 'internalkeyrangelower', v, partIndex),
                min: 0, max: 127,
                format: formatNote
            }),
            createValue('Upper', {
                get: () => getParam('part', 'internalkeyrangeupper', partIndex),
                set: (v) => setParam('part', 'internalkeyrangeupper', v, partIndex),
                min: 0, max: 127,
                format: formatNote
            }),
            createBack()
        ]),
        createSubmenu('Velocity', () => [
            createValue('Sense', {
                get: () => getParam('part', 'internalvelocitysense', partIndex),
                set: (v) => setParam('part', 'internalvelocitysense', v, partIndex),
                min: 0, max: 127
            }),
            createValue('Max', {
                get: () => getParam('part', 'internalvelocitymax', partIndex),
                set: (v) => setParam('part', 'internalvelocitymax', v, partIndex),
                min: 1, max: 127
            }),
            createBack()
        ]),
        createBack()
    ];
}

/* Note name formatter */
function formatNote(n) {
    const notes = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    const octave = Math.floor(n / 12) - 1;
    return `${notes[n % 12]}${octave}`;
}

/* === Part Patch Selection Menu === */
function getPartPatchMenu(partIndex) {
    const state = getState();
    const banks = state.getBanks ? state.getBanks() : [];
    const setParam = state.setParam || (() => {});

    return [
        ...banks.map(bank =>
            createSubmenu(bank.name, () => getPartPatchListMenu(partIndex, bank.id))
        ),
        createBack()
    ];
}

function getPartPatchListMenu(partIndex, bankId) {
    const state = getState();
    const patches = state.getPatchesInBank ? state.getPatchesInBank(bankId) : [];
    const setParam = state.setParam || (() => {});
    const getPatchNumBase = state.getPatchNumBase || (() => 0);

    return [
        ...patches.map((patch, index) =>
            createAction(`${index + 1}: ${patch.name}`, () => {
                /* JV-880 patchnumber encoding:
                 * 0-63 = Internal, 64-127 = Card, 128-191 = Preset A, 192-255 = Preset B
                 * Get the base for this bank and add the patch position */
                const base = getPatchNumBase(bankId);
                const patchNumber = base + index;
                setParam('part', 'patchnumber', patchNumber, partIndex);
            })
        ),
        createBack()
    ];
}

/* === All Parts Menu === */
function getAllPartsMenu() {
    return [
        createSubmenu('Part 1', () => getPartMenu(0)),
        createSubmenu('Part 2', () => getPartMenu(1)),
        createSubmenu('Part 3', () => getPartMenu(2)),
        createSubmenu('Part 4', () => getPartMenu(3)),
        createSubmenu('Part 5', () => getPartMenu(4)),
        createSubmenu('Part 6', () => getPartMenu(5)),
        createSubmenu('Part 7', () => getPartMenu(6)),
        createSubmenu('Part 8', () => getPartMenu(7)),
        createBack()
    ];
}

/* === Save Performance Menu === */
function getSavePerformanceMenu() {
    const slots = [];
    for (let i = 0; i < 16; i++) {
        slots.push(
            createAction(`Internal ${i + 1}`, () => {
                host_module_set_param(`write_performance_${i}`, '1');
                host_module_set_param('save_nvram', '1');
            })
        );
    }
    return [
        ...slots,
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
