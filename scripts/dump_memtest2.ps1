$b = [System.IO.File]::ReadAllBytes('E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin')

# The memory test code at RAM 0x801C0D5C
# The kernel is likely copied from ROM to RAM. We need to find the ROM offset.
# Let's search for the byte pattern at RAM 0x69240 = 0x99,0x79,0x77,0x77
# which corresponds to a SB instruction writing these values.
# SB rt, offset(base) = opcode 0xA0

# Let's instead look at what's around the memory test address in ROM.
# The BIOS typically copies starting from ROM offset 0x10000 to RAM 0x80010000
# So RAM 0x801C0D5C = 0x1C0D5C. If copied from ROM 0x10000 -> RAM 0x10000:
# ROM offset = 0x1C0D5C (but ROM is only 512KB = 0x80000, so this is out of bounds)
#
# Alternative: maybe the kernel maps differently.
# Let's search for the instruction that does the SB to a test pattern.
# The SB instruction at 0x801C0D5C stores a byte from some register.
# Let's search for the instruction at that PC.
#
# Actually, let's just dump the BIOS ROM looking for sequences that write
# byte patterns 0x99, 0x79, 0x77, 0x77 (the memory test pattern we see)

# First let's check: maybe the kernel copies from ROM lower area
# PS1 BIOS v4.1: kernel starts at ROM offset ~0x6000 or so
# Kernel entry point is typically at ROM 0xBFC10000 → offset 0x10000

# Let's try to find the instruction pattern by searching for
# addresses that reference 0x69240 in any way
# Actually, let's look at the kernel copy mechanism.
# During boot, BIOS copies from a ROM table to specific RAM addresses.

# Let's search ROM for the SB instruction that would be at the test PC
# by looking for the 4-byte pattern byte values 99,79,77,77 as LI instructions

# Actually simpler: search for any instruction that loads 0x99 or similar test patterns
# Let's look for ADDIU/LI instructions loading specific values

# Let me try a different approach: let's find where in ROM the kernel at 0x60000+ comes from
# by comparing ROM content with expected RAM content

Write-Host "=== Checking ROM content at offset 0x69240 ==="
for ($i = 0; $i -lt 32; $i += 4) {
    $w = [BitConverter]::ToUInt32($b, 0x69240 + $i)
    Write-Host ("  ROM[0x{0:X5}]: 0x{1:X8}" -f (0x69240 + $i), $w)
}

Write-Host "`n=== Looking for kernel copy: comparing ROM regions with expected RAM 0x60000+ ==="
# The kernel code area in RAM starts around 0x60000 (based on handler addresses)
# Let's check if ROM offset 0x60000 matches what we'd expect

Write-Host "`nROM[0x60000..0x6001F]:"
for ($i = 0; $i -lt 32; $i += 4) {
    $w = [BitConverter]::ToUInt32($b, 0x60000 + $i)
    Write-Host ("  ROM[0x{0:X5}]: 0x{1:X8}" -f (0x60000 + $i), $w)
}

# Let's check a few known RAM addresses against ROM
# Handler at 0x800685BC had: NOP(00000000), LI_t2_0xA0(240A00A0), JR_t2(01400008), LI_t1_0x15(24090015)
$pattern = @(0x00000000, 0x240A00A0, 0x01400008, 0x24090015)
Write-Host "`n=== Searching ROM for handler pattern (NOP,LI t2 0xA0,JR t2,LI t1 0x15) ==="
for ($off = 0; $off -lt $b.Length - 15; $off += 4) {
    $w0 = [BitConverter]::ToUInt32($b, $off)
    if ($w0 -eq $pattern[0]) {
        $w1 = [BitConverter]::ToUInt32($b, $off + 4)
        if ($w1 -eq $pattern[1]) {
            $w2 = [BitConverter]::ToUInt32($b, $off + 8)
            $w3 = [BitConverter]::ToUInt32($b, $off + 12)
            if ($w2 -eq $pattern[2] -and $w3 -eq $pattern[3]) {
                Write-Host ("  FOUND at ROM offset 0x{0:X5}" -f $off)
                # This tells us the ROM-to-RAM mapping
                $ramAddr = 0x800685BC
                $romOff = $off
                $delta = $ramAddr - 0x80000000 - $romOff
                Write-Host ("  RAM addr would be 0x{0:X8}, ROM offset 0x{1:X5}, delta=0x{2:X}" -f $ramAddr, $romOff, $delta)
            }
        }
    }
}

# Now let's search for the memory test instruction pattern
# The code at PC=0x801C0D5C does SB to 0x80069240
# This would be part of a loop. Let's find it in ROM.
Write-Host "`n=== Searching for memory test code (SB instructions in loops) ==="
# The PC is 0x801C0D5C. If kernel base in RAM is different from ROM offset,
# we need the delta to find it.
