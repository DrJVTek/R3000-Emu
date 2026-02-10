#!/usr/bin/env python3
"""Show TOC from CUE file to find track LBAs."""

cue_path = r'E:\Projects\PSX\roms\Ridge Racer (U).cue'

with open(cue_path, 'r') as f:
    content = f.read()

print("CUE file content:")
print(content)
print()

# Parse MSF from INDEX lines
import re
for match in re.finditer(r'TRACK (\d+) (\w+)', content):
    track_num = int(match.group(1))
    track_type = match.group(2)
    print(f"Track {track_num}: {track_type}")

# Check bin file size
import os
disc_path = r'E:\Projects\PSX\roms\Ridge Racer (U).bin'
size = os.path.getsize(disc_path)
sectors = size // 2352
print(f"\nDisc: {size} bytes = {sectors} sectors")
print(f"Max LBA: {sectors - 1}")
print(f"Garbage LBA: 217238 (18757 sectors beyond disc end!)")
