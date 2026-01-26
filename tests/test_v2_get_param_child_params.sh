#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FILE="${REPO_ROOT}/src/dsp/jv880_plugin.cpp"

start=$(rg -n "static int v2_get_param" "$FILE" | head -n 1 | cut -d: -f1 || true)
end=$(rg -n "static int v2_get_error" "$FILE" | head -n 1 | cut -d: -f1 || true)

if [ -z "$start" ] || [ -z "$end" ]; then
    echo "FAIL: could not locate v2_get_param/v2_get_error boundaries"
    exit 1
fi

section=$(sed -n "${start},${end}p" "$FILE")

echo "$section" | rg "nvram_tone_"
echo "$section" | rg "nvram_patchCommon_"
echo "$section" | rg "sram_part_"

echo "PASS: v2_get_param handles child parameter prefixes"
