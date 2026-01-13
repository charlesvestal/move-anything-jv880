/*
 * JV-880 Browser Mode Display
 *
 * Shows patch/performance info when not in menu mode.
 */

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* === State accessor === */
let stateAccessor = null;

export function setStateAccessor(accessor) {
    stateAccessor = accessor;
}

function getState() {
    return stateAccessor ? stateAccessor() : {};
}

/* === Browser Display === */
export function drawBrowser() {
    const state = getState();
    const mode = state.mode || 'patch';
    const patchName = state.patchName || '---';
    const bankName = state.bankName || 'JV-880';
    const currentPreset = state.currentPreset || 0;
    const totalPatches = state.totalPatches || 128;
    const selectedTone = state.selectedTone || 0;
    const selectedPart = state.selectedPart || 0;
    const toneEnabled = state.toneEnabled || [1, 1, 1, 1];
    const octaveTranspose = state.octaveTranspose || 0;

    clear_screen();

    /* Line 1: Mode and bank info */
    const modeLabel = mode === 'performance' ? 'PERF' : 'PATCH';
    const presetNum = currentPreset + 1;
    const headerLine = `${modeLabel}  ${bankName}`;
    print(2, 2, headerLine, 1);

    /* Right side: preset number */
    const presetStr = `#${presetNum}`;
    const presetX = SCREEN_WIDTH - (presetStr.length * 6) - 2;
    print(presetX, 2, presetStr, 1);

    /* Divider line */
    fill_rect(0, 14, SCREEN_WIDTH, 1, 1);

    /* Line 2: Patch name (large/prominent) */
    /* Truncate if too long for screen */
    const maxNameLen = 20;
    const displayName = patchName.length > maxNameLen
        ? patchName.substring(0, maxNameLen)
        : patchName;
    print(2, 18, displayName, 1);

    /* Line 3: Context info */
    let contextLine;
    if (mode === 'performance') {
        contextLine = `Part ${selectedPart + 1}`;
    } else {
        const toneStatus = toneEnabled.map((e, i) =>
            i === selectedTone ? (e ? `[${i + 1}]` : `(${i + 1})`) : (e ? `${i + 1}` : '-')
        ).join(' ');
        contextLine = `Tone: ${toneStatus}`;
    }
    print(2, 34, contextLine, 1);

    /* Line 4: Hints */
    const footerLine = `Clk:Browse  Menu:Edit`;
    print(2, 50, footerLine, 1);
}

/* === Loading Screen === */
export function drawLoadingScreen(status) {
    clear_screen();
    print(2, 2, 'JV-880', 1);
    fill_rect(0, 14, SCREEN_WIDTH, 1, 1);
    print(2, 24, 'Loading...', 1);
    print(2, 40, status || 'Initializing...', 1);
}

/* === Activity Overlay === */
export function drawActivityOverlay(text) {
    if (!text) return;

    /* Draw at bottom of screen */
    fill_rect(0, SCREEN_HEIGHT - 12, SCREEN_WIDTH, 12, 1);
    print(2, SCREEN_HEIGHT - 10, text, 0);
}
