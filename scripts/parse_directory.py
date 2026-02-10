#!/usr/bin/env python3
"""Parse ISO9660 directory entries from Ridge Racer disc."""

import struct

disc_path = r'E:\Projects\PSX\roms\Ridge Racer (U).bin'

def read_sector_2048(lba):
    """Read user data (2048 bytes) from a sector."""
    with open(disc_path, 'rb') as f:
        f.seek(lba * 2352 + 24)  # Skip sync + header (Mode2 Form1)
        return f.read(2048)

def parse_dir_entry(data, offset):
    """Parse a single ISO9660 directory entry."""
    if offset >= len(data):
        return None

    rec_len = data[offset]
    if rec_len == 0:
        return None

    if offset + rec_len > len(data):
        return None

    entry = data[offset:offset + rec_len]
    if len(entry) < 33:
        return None

    # ISO9660 directory record format:
    # 0: record length
    # 1: extended attribute length
    # 2-5: LBA of extent (little-endian)
    # 6-9: LBA of extent (big-endian)
    # 10-13: data length (little-endian)
    # 14-17: data length (big-endian)
    # 18-24: recording date/time
    # 25: file flags
    # 26: interleave unit size
    # 27: interleave gap size
    # 28-29: volume sequence number (little-endian)
    # 30-31: volume sequence number (big-endian)
    # 32: file identifier length
    # 33+: file identifier

    lba = struct.unpack('<I', entry[2:6])[0]
    size = struct.unpack('<I', entry[10:14])[0]
    flags = entry[25]
    name_len = entry[32]
    name = entry[33:33 + name_len].decode('ascii', errors='replace')

    # Clean up filename
    if ';' in name:
        name = name.split(';')[0]

    return {
        'rec_len': rec_len,
        'lba': lba,
        'size': size,
        'flags': flags,
        'name': name,
        'is_dir': (flags & 2) != 0
    }

# Read PVD (sector 16) to get root directory location
pvd = read_sector_2048(16)
print("=== Primary Volume Descriptor (Sector 16) ===")
print(f"Type: {pvd[0]}")
print(f"Magic: {pvd[1:6]}")

# Root directory record starts at offset 156 in PVD
root_rec = pvd[156:156+34]
root_lba = struct.unpack('<I', root_rec[2:6])[0]
root_size = struct.unpack('<I', root_rec[10:14])[0]
print(f"Root directory: LBA={root_lba} size={root_size}")

# Read root directory
print(f"\n=== Root Directory (Sector {root_lba}) ===")
dir_data = read_sector_2048(root_lba)

offset = 0
entries = []
while offset < len(dir_data):
    entry = parse_dir_entry(dir_data, offset)
    if entry is None:
        break
    entries.append(entry)
    offset += entry['rec_len']

for e in entries:
    flag = 'D' if e['is_dir'] else 'F'
    print(f"  [{flag}] {e['name']:20s} LBA={e['lba']:6d} size={e['size']:10d}")

# Check for suspicious LBAs
print("\n=== LBA Analysis ===")
disc_sectors = 198481
for e in entries:
    if e['lba'] > disc_sectors:
        print(f"  ERROR: {e['name']} has LBA {e['lba']} beyond disc end ({disc_sectors})!")
    elif e['lba'] + (e['size'] // 2048) > disc_sectors:
        end_lba = e['lba'] + (e['size'] // 2048)
        print(f"  WARNING: {e['name']} extends beyond disc (LBA {e['lba']}-{end_lba})")

# Calculate what LBA 217238 could represent
print("\n=== Garbage LBA Analysis ===")
garbage_lba = 217238
print(f"Garbage LBA: {garbage_lba}")

# If file_start_lba + (position / 2048) = garbage_lba
# Then position = (garbage_lba - file_start_lba) * 2048
for e in entries:
    if e['lba'] > 0:
        position = (garbage_lba - e['lba']) * 2048
        if position > 0 and position < 1024 * 1024 * 1024:  # < 1GB
            print(f"  If reading {e['name']} (LBA {e['lba']}): position = {position} bytes ({position/1024/1024:.1f} MB)")
