import sys

disc_path = r'E:\Projects\PSX\roms\Ridge Racer (U).bin'
lba = 18
sector_size = 2352
data_offset = 24  # Skip sync + header for Mode1

with open(disc_path, 'rb') as f:
    f.seek(lba * sector_size + data_offset)
    data = f.read(8)

print(f"Sector {lba} first 8 bytes from disc:")
print(' '.join(f'{b:02X}' for b in data))

# Compare with log: FIFO LBA=18 [01001600 00000100]
# This is: 01 00 16 00 00 00 01 00 (little-endian u32 pairs)
expected = bytes([0x01, 0x00, 0x16, 0x00, 0x00, 0x00, 0x01, 0x00])
print(f"\nExpected (from log): {' '.join(f'{b:02X}' for b in expected)}")
print(f"Match: {data == expected}")
