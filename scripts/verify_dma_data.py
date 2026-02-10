#!/usr/bin/env python3
"""
Analyze the DMA3 target address to see if it overlaps with FCB.

Key addresses:
- FCB base: 0x800793E4 = physical 0x793E4
- Common DMA target: 0x80079000 = physical 0x79000

If DMA writes a 2048-byte sector to 0x79000, it covers:
  0x79000 to 0x79800 (2048 bytes)

FCB at 0x793E4 is at offset 0x3E4 into this region - IT WILL BE OVERWRITTEN!
"""

fcb_addr = 0x793E4
dma_target = 0x79000
sector_size = 2048

print("=== DMA3 FCB Overlap Analysis ===")
print(f"FCB address: 0x{fcb_addr:05X}")
print(f"DMA target:  0x{dma_target:05X}")
print(f"DMA end:     0x{dma_target + sector_size:05X}")
print()

if dma_target <= fcb_addr < dma_target + sector_size:
    offset = fcb_addr - dma_target
    print(f"OVERLAP! FCB is at offset 0x{offset:03X} ({offset}) in DMA buffer")
    print(f"This means sector data will OVERWRITE the FCB!")
    print()
    print("When the BIOS reads sector data into 0x79000:")
    print("  - Bytes 0x000-0x3E3: sector data (beginning of sector)")
    print("  - Bytes 0x3E4-0x3E7: FCB fields OVERWRITTEN by sector data")
    print("  - Bytes 0x3E8-0x7FF: more sector data")
    print()
    print("The garbage MSF 48:18:38 could be:")
    print("  - Data from the sector at offset 0x3E4")
    print("  - ASCII or binary data from a file being read")
else:
    print("No overlap")

# Let's check what data is at offset 0x3E4 in various sectors
print()
print("=== Checking sector data at offset 0x3E4 ===")

disc_path = r'E:\Projects\PSX\roms\Ridge Racer (U).bin'

import struct

# Check system area and first game sectors
for lba in [0, 16, 17, 18, 22, 23, 24, 25, 26, 27, 28, 29, 30]:
    try:
        with open(disc_path, 'rb') as f:
            f.seek(lba * 2352 + 24)  # Skip sync + header
            sector_data = f.read(2048)

        offset = 0x3E4
        data = sector_data[offset:offset+16]

        # Check if this looks like LBA 217238 related
        if len(data) >= 4:
            lba_val = struct.unpack('<I', data[:4])[0]

            # Check for 48 18 38 pattern
            has_pattern = b'\x48\x18\x38' in data

            if has_pattern or lba_val > 200000:
                print(f"Sector {lba:3d} offset 0x{offset:03X}: {data.hex()} | LBA={lba_val}")
    except:
        pass

# Also check game data sectors
print()
print("=== Checking game sectors 100-150 ===")
for lba in range(100, 150):
    try:
        with open(disc_path, 'rb') as f:
            f.seek(lba * 2352 + 24)
            sector_data = f.read(2048)

        offset = 0x3E4
        data = sector_data[offset:offset+4]

        if b'\x48\x18\x38' in data or (len(data) >= 3 and data[0] == 0x48 and data[1] == 0x18):
            print(f"Sector {lba}: offset 0x3E4 = {data.hex()}")
    except:
        pass
