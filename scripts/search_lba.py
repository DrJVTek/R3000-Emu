import struct

disc_path = r'E:\Projects\PSX\roms\Ridge Racer (U).bin'
bios_path = r'E:\Projects\PSX\duckstation\bios\Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin'

# LBA 217238 = 0x35056
target_lba = 217238
target_bytes = struct.pack('<I', target_lba)  # 56 50 03 00 (little-endian)

print(f"Searching for LBA {target_lba} = 0x{target_lba:X}")
print(f"Target bytes (LE): {target_bytes.hex()}")

# Search in BIOS
with open(bios_path, 'rb') as f:
    bios = f.read()

print(f"\n=== BIOS ({len(bios)} bytes) ===")
pos = 0
count = 0
while True:
    pos = bios.find(target_bytes, pos)
    if pos == -1:
        break
    print(f"  Found at BIOS offset 0x{pos:X}")
    count += 1
    pos += 1
print(f"Total: {count} occurrences")

# Search in first 30 sectors of disc (directory area)
with open(disc_path, 'rb') as f:
    disc_start = f.read(30 * 2352)

print(f"\n=== Disc first 30 sectors ({len(disc_start)} bytes) ===")
pos = 0
count = 0
while True:
    pos = disc_start.find(target_bytes, pos)
    if pos == -1:
        break
    sector = pos // 2352
    offset_in_sector = pos % 2352
    print(f"  Found at disc offset 0x{pos:X} (sector {sector}, offset 0x{offset_in_sector:X})")
    count += 1
    pos += 1
print(f"Total: {count} occurrences")

# Also search for the MSF bytes 48 18 38 as consecutive bytes
msf_bytes = bytes([0x48, 0x18, 0x38])
print(f"\n=== Searching MSF bytes 48 18 38 in directory sectors ===")
for lba in range(16, 30):
    with open(disc_path, 'rb') as f:
        f.seek(lba * 2352 + 24)  # Skip sync + header
        sector_data = f.read(2048)

    pos = 0
    while True:
        pos = sector_data.find(msf_bytes, pos)
        if pos == -1:
            break
        context = sector_data[max(0,pos-4):pos+16]
        print(f"  Sector {lba} offset 0x{pos:X}: {context.hex()}")
        pos += 1
