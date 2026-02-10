import sys

def search_pattern(filepath, pattern):
    with open(filepath, 'rb') as f:
        data = f.read()

    pattern_bytes = bytes(pattern)
    found = []
    pos = 0
    while True:
        pos = data.find(pattern_bytes, pos)
        if pos == -1:
            break
        found.append(pos)
        pos += 1
    return found

# Search for 48 18 38 in BIOS
bios_path = r'E:\Projects\PSX\duckstation\bios\Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin'
pattern = [0x48, 0x18, 0x38]
results = search_pattern(bios_path, pattern)
if results:
    print(f"Found in BIOS at offsets: {[hex(x) for x in results]}")
else:
    print("Not found in BIOS")

# Search in disc
disc_path = r'E:\Projects\PSX\roms\Ridge Racer (U).bin'
results = search_pattern(disc_path, pattern)
if results:
    print(f"Found in disc at {len(results)} locations")
    for r in results[:10]:
        lba = r // 2352
        sector_off = r % 2352
        print(f"  offset 0x{r:X} = LBA {lba}, sector offset {sector_off}")
else:
    print("Not found in disc")
