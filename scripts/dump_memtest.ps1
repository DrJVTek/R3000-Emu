$b = [System.IO.File]::ReadAllBytes('E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin')
Write-Host ("BIOS size: {0} bytes (0x{0:X})" -f $b.Length)

# The memory test runs from PC=0x801C0D5C (physical 0x1C0D5C)
# This was copied from ROM. The kernel copy typically maps:
# ROM 0xBFC10000+ -> RAM 0x80010000+ or similar
# But we need to find WHERE in ROM the code at 0x1C0D5C originates.
# The BIOS typically copies itself starting from a known ROM offset.

# Let's just search for a distinctive instruction pattern near the memory test.
# From the watchpoint, PC=0x801C0D5C writes bytes to 0x69240.
# SB instruction encoding: opcode 0x28 (bits 31-26 = 101000)
# Let's search ROM for code that references 0x69240 or nearby addresses.

# Actually, let's dump ROM content that would map to the memtest area.
# The BIOS kernel is at the END of ROM usually.
# PS1 BIOS is 512KB. Kernel at ROM offset ~0x10000-0x70000 typically.

# Search for the instruction sequence that would be at 0x801C0D5C
# We know from the trace that the code at 0x800685BC is:
# NOP, LI $t2,0xA0, JR $t2, LI $t1,0x15
# Let's find that pattern in ROM
$target = @(0x00000000, 0x240A00A0, 0x01400008, 0x24090015)
for ($off = 0; $off -lt $b.Length - 15; $off += 4) {
    $match = $true
    for ($j = 0; $j -lt 4; $j++) {
        $w = [BitConverter]::ToUInt32($b, $off + $j * 4)
        if ($w -ne $target[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Host ("Found handler pattern at ROM offset 0x{0:X} (ROM addr 0xBFC{0:X5})" -f $off)
        # Dump surrounding context
        $start = [Math]::Max(0, $off - 32)
        $end = [Math]::Min($b.Length, $off + 48)
        for ($i = $start; $i -lt $end; $i += 4) {
            $w = [BitConverter]::ToUInt32($b, $i)
            $marker = if ($i -eq $off) { " <-- handler" } else { "" }
            Write-Host ("  ROM 0x{0:X5}: 0x{1:X8}{2}" -f $i, $w, $marker)
        }
    }
}

# Also dump what the BIOS has at the area that maps to 0x69240 range
# To understand if the kernel code should be there
Write-Host "`n--- Searching for kernel copy routine (memcpy-like patterns) ---"

# Dump ROM around offset 0x69240 to see if there's valid code there
# (If kernel is copied 1:1 from ROM to RAM starting at some offset)
Write-Host "`n--- ROM at various offsets that could map to 0x69240 ---"
foreach ($base in @(0x00000, 0x10000, 0x18000, 0x50000, 0x60000)) {
    $romOff = $base + 0x9240  # 0x69240 - 0x60000 = 0x9240
    if ($romOff -ge 0 -and $romOff -lt $b.Length - 3) {
        $w = [BitConverter]::ToUInt32($b, $romOff)
        Write-Host ("  ROM base=0x{0:X5} + 0x9240 = offset 0x{1:X5}: 0x{2:X8}" -f $base, $romOff, $w)
    }
    $romOff2 = $base + 0x69240
    if ($romOff2 -ge 0 -and $romOff2 -lt $b.Length - 3) {
        $w = [BitConverter]::ToUInt32($b, $romOff2)
        Write-Host ("  ROM base=0x{0:X5} + 0x69240 = offset 0x{1:X5}: 0x{2:X8}" -f $base, $romOff2, $w)
    }
}

# Check what ROM has at the straight offset 0x69240
if (0x69240 -lt $b.Length) {
    Write-Host "`n--- ROM straight offset 0x69240 (8 words) ---"
    for ($i = 0; $i -lt 32; $i += 4) {
        $w = [BitConverter]::ToUInt32($b, 0x69240 + $i)
        Write-Host ("  ROM[0x{0:X5}]: 0x{1:X8}" -f (0x69240 + $i), $w)
    }
}
