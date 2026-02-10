import struct

disc_path = r'E:\Projects\PSX\roms\Ridge Racer (U).bin'

# Check multiple directory sectors
for lba in [16, 17, 18, 22, 23, 24, 25]:
    with open(disc_path, 'rb') as f:
        f.seek(lba * 2352 + 24)  # Skip sync + header
        sector_data = f.read(2048)

    print(f"\n=== Sector {lba} ===")

    # Check offset 0x3E4 (996)
    offset = 0x3E4
    if offset + 8 <= len(sector_data):
        data = sector_data[offset:offset+16]
        print(f"Offset 0x{offset:03X} ({offset}): {data.hex()}")

        # Try to interpret as LBA (little-endian u32)
        if len(data) >= 4:
            lba_val = struct.unpack('<I', data[:4])[0]
            print(f"  As LE u32: {lba_val} (0x{lba_val:X})")

        # Check for 48 18 38 pattern
        for i in range(len(sector_data) - 2):
            if sector_data[i] == 0x48 and sector_data[i+1] == 0x18 and sector_data[i+2] == 0x38:
                print(f"  Found 48 18 38 at offset 0x{i:X}")
