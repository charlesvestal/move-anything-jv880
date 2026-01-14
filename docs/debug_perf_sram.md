# Debug: Finding Temp Performance in SRAM

This document describes how to find where the temporary performance data lives in SRAM.

## Debug Commands Available

These are now available via `host_module_set_param` and `host_module_get_param`:

### Dump Memory
```javascript
// Dump SRAM to file (32KB)
host_module_set_param('dump_sram', 'sram_before.bin');

// Dump NVRAM to file (32KB)
host_module_set_param('dump_nvram', 'nvram_before.bin');
```

### Search SRAM
```javascript
// Find a string in SRAM (returns hex offset or -1)
host_module_get_param('debug_find_sram_Jazz Split');  // Returns "0x1234" or "-1"
```

### Read SRAM Byte
```javascript
// Read byte at offset (decimal or hex)
host_module_get_param('debug_sram_0x1000');  // Read at offset 0x1000
host_module_get_param('debug_sram_4096');    // Same offset, decimal
```

## Procedure to Find Temp Performance Location

1. **Load a performance with known name**
   ```javascript
   host_module_set_param('mode', '1');        // Performance mode
   host_module_set_param('performance', '0'); // Load "Jazz Split" (Preset A:01)
   ```

2. **Search for the name in SRAM**
   ```javascript
   let offset = host_module_get_param('debug_find_sram_Jazz');
   console.log('Performance name found at:', offset);
   ```

3. **Dump SRAM before a change**
   ```javascript
   host_module_set_param('dump_sram', 'sram_before.bin');
   ```

4. **Send a SysEx to change a known parameter**
   For example, change reverb level (performance common param index 14):
   ```javascript
   // Build DT1 SysEx: F0 41 10 46 12 00 00 10 0E 7F <checksum> F7
   // Address 00 00 10 0E = temp perf common, reverb level
   // Value 7F = 127
   ```

5. **Dump SRAM after change**
   ```javascript
   host_module_set_param('dump_sram', 'sram_after.bin');
   ```

6. **Compare dumps on Mac**
   ```bash
   xxd sram_before.bin > before.txt
   xxd sram_after.bin > after.txt
   diff before.txt after.txt
   ```

## Expected Results

The temp performance should be somewhere in SRAM (0x0000-0x7FFF). The structure should mirror the SysEx parameter layout:
- Bytes 0-11: Performance name
- Byte 12: Key mode
- Bytes 13-16: Reverb (type, level, time, feedback)
- Bytes 17-22: Chorus (type, level, depth, rate, feedback, output)
- Bytes 23-30: Voice reserve 1-8

Then 8 parts of ~24 bytes each.

Once we find the offset, we can add proper read handlers for initial values.
