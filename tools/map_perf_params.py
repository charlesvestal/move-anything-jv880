#!/usr/bin/env python3
"""
Map stored performance bytes to SysEx parameter values.

Based on analysis of ROM2 performances, the stored format appears to be:
- Bytes 0-11: Name (12 ASCII chars)
- Byte 12: Key mode (or part of a packed field?)
- Bytes 13+: Common params then part data

The SysEx temp performance format expands this for editing.
"""

import sys
from pathlib import Path

# Performance common params from SysEx (after 12 name bytes)
PERF_COMMON_SYSEX = [
    (12, "keymode"),
    (13, "reverbtype"),
    (14, "reverblevel"),
    (15, "reverbtime"),
    (16, "reverbfeedback"),
    (17, "chorustype"),
    (18, "choruslevel"),
    (19, "chorusdepth"),
    (20, "chorusrate"),
    (21, "chorusfeedback"),
    (22, "chorusoutput"),
    (23, "voicereserve1"),
    (24, "voicereserve2"),
    (25, "voicereserve3"),
    (26, "voicereserve4"),
    (27, "voicereserve5"),
    (28, "voicereserve6"),
    (29, "voicereserve7"),
    (30, "voicereserve8"),
]

# Part params from SysEx (35 per part, but stored as ~21 bytes)
# The stored format is more compact - some params may be combined or omitted
PART_STORED_OFFSETS = {
    # Based on pattern analysis, parts start at byte 31
    # Each part is 21-22 bytes in stored format
    # Key observations from hex dumps:
    # - Pattern "24 60 00 20 7f 00" appears frequently - likely key range defaults
    # - e0, e1, e2... appear to be patch bank select
    # - Values like 80, 7f appear to be levels/pans at default
}


def load_file(path: str) -> bytes:
    with open(path, 'rb') as f:
        return f.read()


def analyze_part_patterns(data: bytes, offset: int, perf_name: str):
    """Try to decode part structure from hex patterns."""
    perf = data[offset:offset + 204]

    print(f"\n=== Analyzing parts for '{perf_name}' ===")

    # Look for patterns in part data
    part_start = 31  # After common params
    bytes_per_part = 21

    for part in range(8):
        part_offset = part_start + part * bytes_per_part
        part_data = perf[part_offset:part_offset + bytes_per_part]

        # Look for key patterns
        # "24 60" might be key range (C-1 to C6 = 36-96 or similar)
        # "00 20 7f" might be velocity range / level

        # Find potential patch number (high byte with e0-e6 pattern)
        patch_bank = None
        patch_num = None
        for i, b in enumerate(part_data):
            if b >= 0xe0 and b <= 0xe7:
                patch_bank = b - 0xe0
                # Next bytes might be patch number
                if i + 1 < len(part_data):
                    # Could be packed or next byte
                    pass

        print(f"  Part {part + 1}: {' '.join(f'{b:02x}' for b in part_data)}")


def decode_performance(data: bytes, offset: int, name: str):
    """Attempt to decode a performance's parameters."""
    perf = data[offset:offset + 204]

    print(f"\n=== Decoding '{name}' ===\n")

    # Name
    perf_name = perf[0:12].decode('ascii', errors='replace').strip()
    print(f"Name: '{perf_name}'")

    # Common params (educated guesses based on values)
    print("\nCommon params (stored offset -> SysEx param):")
    for stored_off, param in PERF_COMMON_SYSEX:
        val = perf[stored_off]
        # Try to interpret the value
        interpretation = ""
        if param == "keymode" and val <= 2:
            interpretation = ["Layer", "Zone", "Single"][val]
        elif "level" in param:
            interpretation = f"{val}/127"
        elif "type" in param:
            interpretation = f"type {val}"
        elif "reserve" in param:
            interpretation = f"voices: {val}"

        print(f"  [{stored_off:2d}] {param:20s} = {val:3d} (0x{val:02x}) {interpretation}")

    # Analyze parts
    analyze_part_patterns(data, offset, name)


def main():
    roms_dir = Path(__file__).parent.parent / "roms"

    rom2_path = roms_dir / "jv880_rom2.bin"
    if rom2_path.exists():
        rom2 = load_file(rom2_path)

        # Decode first performance from Preset A
        decode_performance(rom2, 0x10020, "Preset A:01 - Jazz Split")

        # Decode a different one to compare
        decode_performance(rom2, 0x100ec, "Preset A:02 - Softly......")

    nvram_path = roms_dir / "jv880_nvram.bin"
    if nvram_path.exists():
        nvram = load_file(nvram_path)
        decode_performance(nvram, 0x00b0, "Internal:01 - Syn Lead")


if __name__ == "__main__":
    main()
