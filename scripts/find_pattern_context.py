import struct

disc_path = r'E:\Projects\PSX\roms\Ridge Racer (U).bin'

# Search for 48 18 38 pattern with more context
pattern = bytes([0x48, 0x18, 0x38])

with open(disc_path, 'rb') as f:
    # Read first 100 sectors (includes directory area)
    data = f.read(100 * 2352)

print(f"Searching for pattern {pattern.hex()} in first 100 sectors")
print()

pos = 0
count = 0
while True:
    pos = data.find(pattern, pos)
    if pos == -1:
        break

    sector = pos // 2352
    raw_offset = pos % 2352

    # Get context
    start = max(0, pos - 16)
    end = min(len(data), pos + 32)
    context = data[start:end]

    # Check if it looks like text
    text_chars = sum(1 for b in context if 0x20 <= b < 0x7F)
    is_text = text_chars > len(context) * 0.5

    print(f"Sector {sector} raw_offset 0x{raw_offset:03X}:")
    print(f"  Hex: {context.hex()}")
    if is_text:
        text = ''.join(chr(b) if 0x20 <= b < 0x7F else '.' for b in context)
        print(f"  Text: {text}")
    print()

    count += 1
    pos += 1

    if count >= 10:
        print(f"... and more (showing first 10)")
        break

print(f"\nTotal occurrences in first 100 sectors: {count}")

# Also check if the pattern appears in directory entries specifically
print("\n=== Checking directory sectors 22-25 ===")
for lba in range(22, 26):
    with open(disc_path, 'rb') as f:
        f.seek(lba * 2352 + 24)  # Skip sync + header
        sector_data = f.read(2048)

    pos = 0
    while True:
        pos = sector_data.find(pattern, pos)
        if pos == -1:
            break

        # Get directory entry context (ISO9660 format)
        print(f"  Sector {lba} offset 0x{pos:03X}")

        # Try to find start of directory entry (look backwards for length byte)
        entry_start = pos
        while entry_start > 0:
            entry_len = sector_data[entry_start]
            # Check if this could be a valid directory entry length
            if entry_len > 0 and entry_len < 128 and entry_start + entry_len <= len(sector_data):
                break
            entry_start -= 1

        if entry_start >= 0:
            entry_len = sector_data[entry_start]
            entry = sector_data[entry_start:entry_start+min(entry_len, 48)]
            print(f"    Entry: {entry.hex()}")

            # Parse ISO9660 directory entry
            if len(entry) >= 33:
                lba_le = struct.unpack('<I', entry[2:6])[0]
                size = struct.unpack('<I', entry[10:14])[0]
                name_len = entry[32]
                name = entry[33:33+name_len] if 33+name_len <= len(entry) else b''
                print(f"    LBA: {lba_le}, Size: {size}, Name: {name}")

        pos += 1
