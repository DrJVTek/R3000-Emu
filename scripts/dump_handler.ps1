$b = [System.IO.File]::ReadAllBytes('E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin')
Write-Host "BIOS size: $($b.Length)"

# The BIOS copies kernel code to RAM at boot. The exception handler is at 0x0C80.
# But the BIOS ROM doesn't store kernel code at offset 0x0C80 directly.
# Instead, the BIOS copies from its ROM to RAM during init.
# Let's dump from the running emulator's RAM instead.
# For now, dump what the BIOS ROM has around the kernel copy routine.

# Actually, let's read from stderr_debug.txt - we already have the handler dump.
# Better: let's write a small tool to read RAM. But we can check BIOS ROM for the handler code.

# The exception handler installed at 0x80000080 jumps to 0x0C80.
# The code at 0x0C80 in RAM was copied from BIOS ROM during InstallExceptionHandlers.
# Let's find where in the BIOS ROM this code comes from by searching for the pattern.

# Search for the 0x80000080 handler pattern: LUI k0, 0 / ADDIU k0, 0x0C80 / JR k0
# 0x3C1A0000 0x275A0C80 0x03400008
$target = @(0x00, 0x00, 0x1A, 0x3C, 0x80, 0x0C, 0x5A, 0x27, 0x08, 0x00, 0x40, 0x03)
for ($i = 0; $i -lt ($b.Length - 12); $i += 4) {
    $match = $true
    for ($j = 0; $j -lt 12; $j++) {
        if ($b[$i + $j] -ne $target[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Host ("Found exception vector code at BIOS offset 0x{0:X}" -f $i)
    }
}

# Also dump around the kernel area where 0x0C80 handler code would be in BIOS
# The BIOS copies kernel from some ROM offset to RAM 0x500-0x1000+ area.
# Search for typical kernel handler code patterns
# The handler at 0x0C80 typically starts with saving k0/k1 to fixed RAM addresses
# Pattern: SW k0, addr / SW k1, addr → 0xAF... opcodes

Write-Host "`n--- Searching for kernel handler copy source ---"
# Look for code that saves k0 (register 26) to a fixed address
# SW $k0, imm($zero) = opcode 0xAF (101011 11 = SW, rs=0, rt=26=k0)
# Binary: 101011 00000 11010 iiiiiiiiiiiiiiii
# = 0xAC1A.... (SW k0, imm($zero))
for ($i = 0; $i -lt ($b.Length - 4); $i += 4) {
    $w = [BitConverter]::ToUInt32($b, $i)
    if (($w -band 0xFFFF0000) -eq 0xAC1A0000) {
        # Check if next instruction also saves k1
        if (($i + 4) -lt $b.Length) {
            $w2 = [BitConverter]::ToUInt32($b, $i + 4)
            if (($w2 -band 0xFFFF0000) -eq 0xAC1B0000) {
                Write-Host ("  SW k0/k1 pair at BIOS offset 0x{0:X}: {1:X8} {2:X8}" -f $i, $w, $w2)
                # Dump 32 words from here
                for ($k = 0; $k -lt 32; $k++) {
                    $off = $i + $k * 4
                    if ($off -lt $b.Length) {
                        $ww = [BitConverter]::ToUInt32($b, $off)
                        Write-Host ("    +{0:X4}: {1:X8}" -f ($k*4), $ww)
                    }
                }
                break
            }
        }
    }
}
