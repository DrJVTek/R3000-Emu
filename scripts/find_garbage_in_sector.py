#!/usr/bin/env python3
"""Search for garbage pattern 0x01000016 in directory sectors."""

import struct

disc_path = r'E:\Projects\PSX\roms\Ridge Racer (U).bin'

pattern = struct.pack('<I', 0x01000016)
print(f"Searching for pattern: {pattern.hex()} (LE) = 0x01000016")

# Check directory sectors
for lba in [16, 17, 18, 22, 23, 24]:
    with open(disc_path, 'rb') as f:
        f.seek(lba * 2352 + 24)
        sector_data = f.read(2048)

    pos = 0
    while True:
        pos = sector_data.find(pattern, pos)
        if pos == -1:
            break
        print(f"  Found at LBA {lba} offset 0x{pos:03X}")
        # Show context
        start = max(0, pos - 8)
        end = min(len(sector_data), pos + 16)
        print(f"    Context: {sector_data[start:end].hex()}")
        pos += 1

# Also check raw sector including headers
print("\n=== Checking raw sectors (including headers) ===")
for lba in [22, 23]:
    with open(disc_path, 'rb') as f:
        f.seek(lba * 2352)
        raw = f.read(2352)

    pos = 0
    while True:
        pos = raw.find(pattern, pos)
        if pos == -1:
            break
        print(f"  Found at LBA {lba} raw offset 0x{pos:03X}")
        pos += 1

# Check what data exists at offset that would produce this pattern
# 0x01000016 as LE bytes: 16 00 00 01
pattern_bytes = bytes([0x16, 0x00, 0x00, 0x01])
print(f"\n=== Searching for byte pattern {pattern_bytes.hex()} ===")
for lba in [22]:
    with open(disc_path, 'rb') as f:
        f.seek(lba * 2352 + 24)
        sector_data = f.read(2048)

    pos = 0
    while True:
        pos = sector_data.find(pattern_bytes, pos)
        if pos == -1:
            break
        print(f"  Found at LBA {lba} offset 0x{pos:03X}")
        start = max(0, pos - 4)
        end = min(len(sector_data), pos + 12)
        context = sector_data[start:end]
        print(f"    Context: {context.hex()}")
        # Try to interpret as directory entry
        if pos >= 2:
            rec_start = pos - 2  # 2 bytes before is record length + ext attr
            rec_len = sector_data[rec_start]
            ext_attr = sector_data[rec_start + 1]
            lba_le = struct.unpack('<I', sector_data[rec_start+2:rec_start+6])[0]
            print(f"    As dir entry: rec_len={rec_len} ext_attr={ext_attr} LBA={lba_le}")
        pos += 1

# Dump first 256 bytes of root directory
print("\n=== Root directory (LBA 22) first 256 bytes ===")
with open(disc_path, 'rb') as f:
    f.seek(22 * 2352 + 24)
    data = f.read(256)

for i in range(0, len(data), 16):
    line = data[i:i+16]
    hex_part = ' '.join(f'{b:02X}' for b in line)
    ascii_part = ''.join(chr(b) if 0x20 <= b < 0x7F else '.' for b in line)
    print(f"  {i:03X}: {hex_part}  {ascii_part}")
