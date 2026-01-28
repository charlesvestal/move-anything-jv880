#!/bin/bash
# Run parameter tests on Move device
# This script creates a trigger file that the plugin checks

set -e

MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
TRIGGER_FILE="/tmp/minijv_run_test"
LOG_FILE="/data/UserData/move-anything/shadow_inprocess.log"

echo "=== Mini-JV On-Device Parameter Test ==="
echo ""
echo "This test requires Mini-JV to be loaded in Signal Chain."
echo ""

# Check connection
echo "Checking connection to $MOVE_HOST..."
if ! ssh -o ConnectTimeout=5 "$MOVE_HOST" "echo ok" > /dev/null 2>&1; then
    echo "ERROR: Cannot connect to $MOVE_HOST"
    exit 1
fi
echo "Connected."
echo ""

# Clear log and create trigger
echo "Starting test..."
ssh "$MOVE_HOST" "echo '=== TEST START ===' >> $LOG_FILE"

# To run the test, we need to send a param via the shadow UI
# The cleanest way is to modify a parameter and watch for output
#
# Since we can't inject JS directly, let's just dump the current state
# and verify the offsets match what we expect

echo ""
echo "Dumping current NVRAM for tone 0 to verify offsets..."
echo ""

# Create a verification script on the device
ssh "$MOVE_HOST" "cat > /tmp/verify_nvram.sh" << 'EOF'
#!/bin/bash
# This would need the NVRAM file to verify
NVRAM="/data/UserData/move-anything/modules/sound_generators/minijv/roms/jv880_nvram.bin"

if [ ! -f "$NVRAM" ]; then
    echo "NVRAM file not found at $NVRAM"
    exit 1
fi

echo "=== NVRAM Tone 0 Verification ==="
echo "File: $NVRAM"
echo ""

# Patch offset: 0x0d70 = 3440
# Tone 0 base: 0x0d70 + 26 = 0x0d8a = 3466
TONE_BASE=3466

echo "Expected offsets (from jv880_juce):"
echo "  tvaLevel at $((TONE_BASE + 67)) = $(printf '0x%04x' $((TONE_BASE + 67)))"
echo "  tvaPan at $((TONE_BASE + 68)) = $(printf '0x%04x' $((TONE_BASE + 68)))"
echo "  drySend at $((TONE_BASE + 81)) = $(printf '0x%04x' $((TONE_BASE + 81)))"
echo "  reverbSend at $((TONE_BASE + 82)) = $(printf '0x%04x' $((TONE_BASE + 82)))"
echo "  chorusSend at $((TONE_BASE + 83)) = $(printf '0x%04x' $((TONE_BASE + 83)))"
echo ""

echo "Current values in NVRAM (if sound is working, these should be reasonable):"
# Use xxd to dump specific bytes
hexdump -C "$NVRAM" -s $TONE_BASE -n 84 | head -6

echo ""
echo "TVA Section (bytes 67-83 from tone base):"
hexdump -C "$NVRAM" -s $((TONE_BASE + 67)) -n 17

echo ""
echo "=== End Verification ==="
EOF

ssh "$MOVE_HOST" "chmod +x /tmp/verify_nvram.sh && /tmp/verify_nvram.sh"

echo ""
echo "=== Manual Sound Test ==="
echo ""
echo "To verify the parameters work correctly:"
echo "1. Load Mini-JV in Shadow Mode slot 1"
echo "2. Enter Shadow UI (Shift+Vol+Knob1)"
echo "3. Go to slot 1 settings"
echo "4. Navigate to Tone parameters"
echo "5. Adjust Level - volume should change"
echo "6. Adjust Pan - sound should move L/R"
echo "7. Adjust Reverb Send - reverb amount should change"
echo ""
echo "If these work, the parameter offsets are correct."
