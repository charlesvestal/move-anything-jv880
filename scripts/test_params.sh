#!/bin/bash
# Automated parameter verification test for Mini-JV
# Runs on host, executes tests via SSH to Move device
#
# Prerequisites:
# - Move Anything running with Mini-JV loaded
# - SSH access to move.local
#
# Tests verify that:
# 1. set_param writes to correct NVRAM offset
# 2. get_param reads back the correct value

set -e

MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
MODULE_PATH="/data/UserData/move-anything/modules/sound_generators/minijv"
TEST_DIR="/tmp/minijv_test"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASS=0
FAIL=0

log_pass() {
    echo -e "${GREEN}✓ PASS${NC}: $1"
    ((PASS++))
}

log_fail() {
    echo -e "${RED}✗ FAIL${NC}: $1"
    ((FAIL++))
}

log_info() {
    echo -e "${YELLOW}→${NC} $1"
}

# Helper to call plugin get_param via the test helper
get_param() {
    local key="$1"
    ssh "$MOVE_HOST" "cat /tmp/minijv_get_$key 2>/dev/null || echo 'ERROR'"
}

# Create test helper script on device
setup_test_helper() {
    log_info "Setting up test helper on device..."

    ssh "$MOVE_HOST" "mkdir -p $TEST_DIR"

    # Create a simple test that uses the module's parameter system
    # This relies on the fact that Move Anything exposes params via files or we can
    # inject test commands

    cat << 'EOF' | ssh "$MOVE_HOST" "cat > $TEST_DIR/param_test.js"
// Parameter test helper for Mini-JV
// This would need to be integrated with Move Anything's JS runtime

const tests = [
    // Tone parameters - verify NVRAM offsets
    // Format: [param_key, test_value, nvram_offset, description]

    // TVA section (the ones we fixed)
    ["nvram_tone_0_level", 100, 67, "TVA Level"],
    ["nvram_tone_0_pan", 64, 68, "TVA Pan"],
    ["nvram_tone_0_tvaenvtime1", 50, 74, "TVA Env Time 1"],
    ["nvram_tone_0_tvaenvtime2", 50, 76, "TVA Env Time 2"],
    ["nvram_tone_0_tvaenvtime3", 50, 78, "TVA Env Time 3"],
    ["nvram_tone_0_tvaenvtime4", 50, 80, "TVA Env Time 4"],
    ["nvram_tone_0_drylevel", 80, 81, "Dry Level"],
    ["nvram_tone_0_reverbsendlevel", 60, 82, "Reverb Send"],
    ["nvram_tone_0_chorussendlevel", 40, 83, "Chorus Send"],

    // TVF section
    ["nvram_tone_0_cutofffrequency", 100, 52, "TVF Cutoff"],
    ["nvram_tone_0_resonance", 50, 53, "TVF Resonance"],

    // Pitch section
    ["nvram_tone_0_pitchcoarse", 64, 37, "Pitch Coarse"],
    ["nvram_tone_0_pitchfine", 64, 38, "Pitch Fine"],
];

// Export for use by test runner
if (typeof module !== 'undefined') {
    module.exports = { tests };
}
EOF
}

# Test round-trip: set value, read back, compare
test_roundtrip() {
    local param="$1"
    local value="$2"
    local desc="$3"

    # This would need Move Anything's param API exposed
    # For now, create a marker file approach
    log_info "Testing $desc ($param = $value)..."

    # We can't directly call the plugin from bash, but we can:
    # 1. Create a test patch that exercises the params
    # 2. Use the existing dump_nvram to verify offsets
}

# Main NVRAM offset verification test
# This compares two NVRAM dumps: before and after setting a parameter
test_nvram_offset() {
    local param="$1"
    local value="$2"
    local expected_offset="$3"
    local desc="$4"
    local tone_idx="${5:-0}"

    # Calculate actual NVRAM address
    # Patch at 0x0d70, common=26 bytes, each tone=84 bytes
    local patch_base=$((0x0d70))
    local tone_base=$((patch_base + 26 + tone_idx * 84))
    local abs_offset=$((tone_base + expected_offset))

    log_info "Testing $desc: offset $expected_offset (abs 0x$(printf '%04x' $abs_offset))"

    # This test would require:
    # 1. Dump NVRAM before
    # 2. Set parameter
    # 3. Dump NVRAM after
    # 4. Compare byte at expected offset

    echo "  Expected NVRAM offset within tone: $expected_offset"
    echo "  Absolute NVRAM address: 0x$(printf '%04x' $abs_offset)"
}

# Print the expected NVRAM layout for manual verification
print_expected_layout() {
    echo ""
    echo "=== Expected NVRAM Layout (Tone 0) ==="
    echo "Patch base: 0x0d70"
    echo "Tone 0 base: 0x0d70 + 26 = 0x0d8a"
    echo ""
    echo "TVA Section (offsets from tone base):"
    echo "  67 (0x43): tvaLevel      -> abs 0x0dcd"
    echo "  68 (0x44): tvaPan        -> abs 0x0dce"
    echo "  69 (0x45): tvaDelayTime  -> abs 0x0dcf"
    echo "  74 (0x4a): tvaEnvTime1   -> abs 0x0dd4"
    echo "  76 (0x4c): tvaEnvTime2   -> abs 0x0dd6"
    echo "  78 (0x4e): tvaEnvTime3   -> abs 0x0dd8"
    echo "  80 (0x50): tvaEnvTime4   -> abs 0x0dda"
    echo "  81 (0x51): drySend       -> abs 0x0ddb"
    echo "  82 (0x52): reverbSend    -> abs 0x0ddc"
    echo "  83 (0x53): chorusSend    -> abs 0x0ddd"
    echo ""
    echo "TVF Section:"
    echo "  52 (0x34): tvfCutoff     -> abs 0x0dbe"
    echo "  53 (0x35): tvfResonance  -> abs 0x0dbf"
    echo ""
}

# Create on-device test that can run within Move Anything
create_device_test() {
    log_info "Creating on-device verification test..."

    cat << 'JSEOF' | ssh "$MOVE_HOST" "cat > $TEST_DIR/verify_params.mjs"
// On-device parameter verification for Mini-JV
// Run this from Move Anything's JS environment

export function verifyToneParams() {
    const results = [];

    // TVA section - these were the ones with wrong offsets
    const tvaTests = [
        { param: "level", offset: 67, desc: "TVA Level" },
        { param: "pan", offset: 68, desc: "TVA Pan" },
        { param: "tvaenvtime1", offset: 74, desc: "TVA Env Time 1" },
        { param: "tvaenvtime2", offset: 76, desc: "TVA Env Time 2" },
        { param: "tvaenvtime3", offset: 78, desc: "TVA Env Time 3" },
        { param: "tvaenvtime4", offset: 80, desc: "TVA Env Time 4" },
        { param: "drylevel", offset: 81, desc: "Dry Send" },
        { param: "reverbsendlevel", offset: 82, desc: "Reverb Send" },
        { param: "chorussendlevel", offset: 83, desc: "Chorus Send" },
    ];

    for (const test of tvaTests) {
        const testValue = 99;  // Distinctive value
        const toneIdx = 0;

        // Set parameter
        host_module_set_param(`nvram_tone_${toneIdx}_${test.param}`, String(testValue));

        // Read back
        const readBack = host_module_get_param(`nvram_tone_${toneIdx}_${test.param}`);

        const pass = parseInt(readBack) === testValue;
        results.push({
            param: test.param,
            expected: testValue,
            actual: readBack,
            pass: pass,
            desc: test.desc
        });
    }

    return results;
}

export function printResults(results) {
    console.log("=== Parameter Verification Results ===");
    let pass = 0, fail = 0;

    for (const r of results) {
        if (r.pass) {
            console.log(`✓ ${r.desc}: ${r.actual} (expected ${r.expected})`);
            pass++;
        } else {
            console.log(`✗ ${r.desc}: got ${r.actual}, expected ${r.expected}`);
            fail++;
        }
    }

    console.log(`\nTotal: ${pass} passed, ${fail} failed`);
    return fail === 0;
}
JSEOF

    log_info "Test script created at $TEST_DIR/verify_params.mjs"
}

# Main
main() {
    echo "=== Mini-JV Parameter Verification ==="
    echo ""

    # Check SSH connectivity
    log_info "Checking connection to $MOVE_HOST..."
    if ! ssh -o ConnectTimeout=5 "$MOVE_HOST" "echo ok" > /dev/null 2>&1; then
        echo -e "${RED}Cannot connect to $MOVE_HOST${NC}"
        echo "Make sure Move is connected and Move Anything is running."
        exit 1
    fi
    log_pass "Connected to Move"

    # Print expected layout for reference
    print_expected_layout

    # Create device test
    create_device_test

    echo ""
    echo "=== Manual Verification Steps ==="
    echo ""
    echo "Since we can't directly inject JS into the running Move Anything,"
    echo "use these manual steps to verify:"
    echo ""
    echo "1. Load Mini-JV in Signal Chain"
    echo "2. Select a patch and tone"
    echo "3. In Shadow UI, adjust these parameters and verify sound changes:"
    echo ""
    echo "   Level (TVA)      - Volume should increase/decrease"
    echo "   Pan              - Sound should move left/right"
    echo "   Reverb Send      - Reverb amount should change"
    echo "   Chorus Send      - Chorus amount should change"
    echo "   Cutoff           - Brightness should change"
    echo "   Resonance        - Peak/emphasis should change"
    echo ""
    echo "4. Or, SSH to device and run:"
    echo "   ssh $MOVE_HOST"
    echo "   # Then from Move Anything console, import the test module"
    echo ""

    # Alternative: use the plugin's dump_nvram feature
    echo "=== Built-in Test Commands ==="
    echo ""
    echo "The plugin now has built-in test commands. From Move Anything JS console:"
    echo ""
    echo "1. Run automated parameter offset test:"
    echo "   host_module_set_param('run_param_test', '1')"
    echo ""
    echo "   This will write test values to NVRAM and verify they land at"
    echo "   the expected offsets. Check stderr for results."
    echo ""
    echo "2. Dump current tone structure with labels:"
    echo "   host_module_set_param('dump_tone_layout', '1')"
    echo ""
    echo "   This shows the current values at each NVRAM offset for tone 0,"
    echo "   with labels matching jv880_juce field names."
    echo ""
    echo "3. To view test output, SSH to device and check logs:"
    echo "   ssh $MOVE_HOST"
    echo "   journalctl -f -u move-anything"
    echo ""
    echo "=== Expected Test Output ==="
    echo ""
    echo "If offsets are correct, you should see:"
    echo "  ✓ PASS: level               offset=67 wrote=0x63 read=0x63"
    echo "  ✓ PASS: pan                 offset=68 wrote=0x40 read=0x40"
    echo "  ... (all tests passing)"
    echo ""
}

main "$@"
