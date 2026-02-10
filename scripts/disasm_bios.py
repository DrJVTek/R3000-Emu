import struct

bios_path = r'E:\Projects\PSX\duckstation\bios\Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin'

with open(bios_path, 'rb') as f:
    bios = f.read()

# BIOS is mapped at 0xBFC00000, mirrored to 0x8004xxxx via kseg0
# PC=0x8004AB00 in RAM corresponds to offset in BIOS
# Actually, 0x8004xxxx is RAM, not BIOS
# The code at 0x8004ABxx is loaded from CDROM into RAM

# Let me instead check the BIOS at 0xBFC0xxxx
# The debug dump showed code at 0x8004AB68, which is RAM
# This means the game executable is loaded there

print("Code at 0x8004ABxx is game code in RAM, not BIOS")
print("The BIOS ROM starts at 0xBFC00000")
print("Need to trace what game code is doing")
