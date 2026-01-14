#!/usr/bin/env python3
"""
Analyze JV-880 performance structure in ROM2 and NVRAM.

This maps the 204-byte stored performance format to understand
how parameters are laid out, which helps us implement reading.
"""

import sys
import os
from pathlib import Path

# Constants from our research
PERF_SIZE = 0xCC  # 204 bytes
PERF_NAME_LEN = 12
PERFS_PER_BANK = 16

# ROM2 offsets
PERF_OFFSET_PRESET_A = 0x10020
PERF_OFFSET_PRESET_B = 0x18020

# NVRAM offsets
NVRAM_PERF_INTERNAL = 0x00b0

# Known SysEx parameter order for Performance Common
PERF_COMMON_PARAMS = [
    # Bytes 0-11: Name (12 bytes)
    "performancename1", "performancename2", "performancename3", "performancename4",
    "performancename5", "performancename6", "performancename7", "performancename8",
    "performancename9", "performancename10", "performancename11", "performancename12",
    # Byte 12: Key mode
    "keymode",
    # Bytes 13-16: Reverb
    "reverbtype", "reverblevel", "reverbtime", "reverbfeedback",
    # Bytes 17-22: Chorus
    "chorustype", "choruslevel", "chorusdepth", "chorusrate", "chorusfeedback", "chorusoutput",
    # Bytes 23-30: Voice reserve
    "voicereserve1", "voicereserve2", "voicereserve3", "voicereserve4",
    "voicereserve5", "voicereserve6", "voicereserve7", "voicereserve8",
]  # 31 params total

# Known SysEx parameter order for Part (35 params per part)
PART_PARAMS = [
    "transmitswitch", "transmitchannel", "transmitprogramchange", "-",
    "transmitvolume", "-", "transmitpan", "-",
    "transmitkeyrangelower", "transmitkeyrangeupper", "transmitkeytranspose",
    "transmitvelocitysense", "transmitvelocitymax", "transmitvelocitycurve",
    "internalswitch",
    "internalkeyrangelower", "internalkeyrangeupper", "internalkeytranspose",
    "internalvelocitysense", "internalvelocitymax", "internalvelocitycurve",
    "receiveswitch", "receivechannel", "patchnumber", "-",
    "partlevel", "partpan", "partcoarsetune", "partfinetune",
    "reverbswitch", "chorusswitch", "receiveprogramchange", "receivevolume",
    "receivehold1", "outputselect",
]  # 35 params


def load_file(path: str) -> bytes:
    """Load binary file."""
    with open(path, 'rb') as f:
        return f.read()


def print_hex_dump(data: bytes, offset: int = 0, width: int = 16, length: int = 64):
    """Print hex dump of data."""
    for i in range(0, min(len(data), length), width):
        hex_part = ' '.join(f'{b:02x}' for b in data[i:i+width])
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[i:i+width])
        print(f"  {offset+i:04x}: {hex_part:<{width*3}} |{ascii_part}|")


def analyze_performance(data: bytes, name: str, offset: int):
    """Analyze a single performance structure."""
    print(f"\n=== {name} (offset 0x{offset:04x}) ===")

    # Extract performance data
    perf = data[offset:offset + PERF_SIZE]
    if len(perf) < PERF_SIZE:
        print(f"  ERROR: Not enough data (got {len(perf)} bytes)")
        return

    # Name (first 12 bytes)
    name_bytes = perf[0:12]
    name_str = name_bytes.decode('ascii', errors='replace').strip()
    print(f"  Name: '{name_str}'")

    # Dump first 48 bytes
    print(f"\n  First 48 bytes (common params):")
    print_hex_dump(perf, 0, 16, 48)

    # Analyze common params after name
    print(f"\n  Common params (bytes 12-30):")
    common = perf[12:31]
    for i, param in enumerate(PERF_COMMON_PARAMS[12:]):  # Skip name bytes
        if 12 + i < 31:
            val = perf[12 + i]
            print(f"    [{12+i:2d}] {param}: {val} (0x{val:02x})")

    # Calculate remaining bytes for parts
    common_size = 31  # 12 name + 19 params
    remaining = PERF_SIZE - common_size
    bytes_per_part = remaining // 8
    print(f"\n  Remaining bytes for parts: {remaining} ({bytes_per_part} per part)")

    # Dump part data
    print(f"\n  Part data layout:")
    for part in range(8):
        part_offset = common_size + part * bytes_per_part
        part_data = perf[part_offset:part_offset + bytes_per_part]
        print(f"\n  Part {part + 1} (offset {part_offset}, {bytes_per_part} bytes):")
        print_hex_dump(part_data, part_offset, 16, bytes_per_part)


def compare_performances(data: bytes, name1: str, off1: int, name2: str, off2: int):
    """Compare two performances to find common structure."""
    print(f"\n=== Comparing {name1} vs {name2} ===")

    perf1 = data[off1:off1 + PERF_SIZE]
    perf2 = data[off2:off2 + PERF_SIZE]

    print(f"  Name1: '{perf1[0:12].decode('ascii', errors='replace').strip()}'")
    print(f"  Name2: '{perf2[0:12].decode('ascii', errors='replace').strip()}'")

    # Find differences
    diffs = []
    for i in range(min(len(perf1), len(perf2))):
        if perf1[i] != perf2[i]:
            diffs.append((i, perf1[i], perf2[i]))

    print(f"\n  Differences ({len(diffs)} bytes differ):")
    for off, v1, v2 in diffs[:40]:  # Show first 40 diffs
        print(f"    [{off:3d}] 0x{off:02x}: {v1:3d} vs {v2:3d}")
    if len(diffs) > 40:
        print(f"    ... and {len(diffs) - 40} more")


def main():
    # Find ROMs
    roms_dir = Path(__file__).parent.parent / "roms"
    if not roms_dir.exists():
        print(f"Error: ROMs directory not found: {roms_dir}")
        sys.exit(1)

    rom2_path = roms_dir / "jv880_rom2.bin"
    nvram_path = roms_dir / "jv880_nvram.bin"

    print("=== JV-880 Performance Structure Analyzer ===\n")

    # Load ROM2
    if rom2_path.exists():
        print(f"Loading ROM2: {rom2_path}")
        rom2 = load_file(rom2_path)
        print(f"  Size: {len(rom2)} bytes (0x{len(rom2):x})")

        # Analyze Preset A performances
        print("\n\n========== PRESET A BANK ==========")
        for i in range(3):  # First 3 performances
            offset = PERF_OFFSET_PRESET_A + i * PERF_SIZE
            analyze_performance(rom2, f"Preset A:{i+1}", offset)

        # Compare first two to understand structure
        compare_performances(rom2, "Preset A:1", PERF_OFFSET_PRESET_A,
                           "Preset A:2", PERF_OFFSET_PRESET_A + PERF_SIZE)
    else:
        print(f"Warning: ROM2 not found: {rom2_path}")

    # Load NVRAM
    if nvram_path.exists():
        print(f"\n\nLoading NVRAM: {nvram_path}")
        nvram = load_file(nvram_path)
        print(f"  Size: {len(nvram)} bytes (0x{len(nvram):x})")

        print("\n\n========== INTERNAL BANK (NVRAM) ==========")
        for i in range(3):  # First 3 internal performances
            offset = NVRAM_PERF_INTERNAL + i * PERF_SIZE
            analyze_performance(nvram, f"Internal:{i+1}", offset)
    else:
        print(f"Warning: NVRAM not found: {nvram_path}")

    print("\n\n=== Summary ===")
    print(f"Performance size: {PERF_SIZE} bytes (0x{PERF_SIZE:02x})")
    print(f"Common params: ~31 bytes (12 name + 19 params)")
    print(f"Remaining for 8 parts: ~{PERF_SIZE - 31} bytes (~{(PERF_SIZE - 31) // 8} per part)")
    print(f"\nThe stored format appears to be more compact than the SysEx edit format.")
    print(f"SysEx has 35 params per part, but stored format has ~{(PERF_SIZE - 31) // 8} bytes/part.")


if __name__ == "__main__":
    main()
