#!/usr/bin/env python3
"""
Trace MIPS R3000 instructions at a specific PC address.
This script will be used to understand what code is writing garbage MSF values.
"""

import struct

# MIPS R3000 instruction decoder (simplified)
def decode_mips(instr, pc):
    """Decode a MIPS instruction and return a human-readable string."""
    op = (instr >> 26) & 0x3F
    rs = (instr >> 21) & 0x1F
    rt = (instr >> 16) & 0x1F
    rd = (instr >> 11) & 0x1F
    shamt = (instr >> 6) & 0x1F
    funct = instr & 0x3F
    imm = instr & 0xFFFF
    imm_signed = imm if imm < 0x8000 else imm - 0x10000
    target = instr & 0x3FFFFFF

    reg_names = [
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
        "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
    ]

    r = lambda x: reg_names[x]

    if op == 0:  # SPECIAL
        if funct == 0:
            if instr == 0:
                return "nop"
            return f"sll {r(rd)}, {r(rt)}, {shamt}"
        elif funct == 2:
            return f"srl {r(rd)}, {r(rt)}, {shamt}"
        elif funct == 3:
            return f"sra {r(rd)}, {r(rt)}, {shamt}"
        elif funct == 8:
            return f"jr {r(rs)}"
        elif funct == 9:
            return f"jalr {r(rd)}, {r(rs)}"
        elif funct == 0x18:
            return f"mult {r(rs)}, {r(rt)}"
        elif funct == 0x19:
            return f"multu {r(rs)}, {r(rt)}"
        elif funct == 0x1A:
            return f"div {r(rs)}, {r(rt)}"
        elif funct == 0x1B:
            return f"divu {r(rs)}, {r(rt)}"
        elif funct == 0x10:
            return f"mfhi {r(rd)}"
        elif funct == 0x12:
            return f"mflo {r(rd)}"
        elif funct == 0x20:
            return f"add {r(rd)}, {r(rs)}, {r(rt)}"
        elif funct == 0x21:
            return f"addu {r(rd)}, {r(rs)}, {r(rt)}"
        elif funct == 0x22:
            return f"sub {r(rd)}, {r(rs)}, {r(rt)}"
        elif funct == 0x23:
            return f"subu {r(rd)}, {r(rs)}, {r(rt)}"
        elif funct == 0x24:
            return f"and {r(rd)}, {r(rs)}, {r(rt)}"
        elif funct == 0x25:
            return f"or {r(rd)}, {r(rs)}, {r(rt)}"
        elif funct == 0x26:
            return f"xor {r(rd)}, {r(rs)}, {r(rt)}"
        elif funct == 0x2A:
            return f"slt {r(rd)}, {r(rs)}, {r(rt)}"
        elif funct == 0x2B:
            return f"sltu {r(rd)}, {r(rs)}, {r(rt)}"
        else:
            return f"special_{funct:02X} {r(rd)}, {r(rs)}, {r(rt)}"
    elif op == 1:  # REGIMM
        if rt == 0:
            return f"bltz {r(rs)}, 0x{pc + 4 + imm_signed*4:08X}"
        elif rt == 1:
            return f"bgez {r(rs)}, 0x{pc + 4 + imm_signed*4:08X}"
        elif rt == 0x10:
            return f"bltzal {r(rs)}, 0x{pc + 4 + imm_signed*4:08X}"
        elif rt == 0x11:
            return f"bgezal {r(rs)}, 0x{pc + 4 + imm_signed*4:08X}"
        else:
            return f"regimm_{rt:02X} {r(rs)}, 0x{imm:04X}"
    elif op == 2:
        return f"j 0x{((pc & 0xF0000000) | (target << 2)):08X}"
    elif op == 3:
        return f"jal 0x{((pc & 0xF0000000) | (target << 2)):08X}"
    elif op == 4:
        return f"beq {r(rs)}, {r(rt)}, 0x{pc + 4 + imm_signed*4:08X}"
    elif op == 5:
        return f"bne {r(rs)}, {r(rt)}, 0x{pc + 4 + imm_signed*4:08X}"
    elif op == 6:
        return f"blez {r(rs)}, 0x{pc + 4 + imm_signed*4:08X}"
    elif op == 7:
        return f"bgtz {r(rs)}, 0x{pc + 4 + imm_signed*4:08X}"
    elif op == 8:
        return f"addi {r(rt)}, {r(rs)}, {imm_signed}"
    elif op == 9:
        return f"addiu {r(rt)}, {r(rs)}, {imm_signed}"
    elif op == 0xA:
        return f"slti {r(rt)}, {r(rs)}, {imm_signed}"
    elif op == 0xB:
        return f"sltiu {r(rt)}, {r(rs)}, {imm_signed}"
    elif op == 0xC:
        return f"andi {r(rt)}, {r(rs)}, 0x{imm:04X}"
    elif op == 0xD:
        return f"ori {r(rt)}, {r(rs)}, 0x{imm:04X}"
    elif op == 0xE:
        return f"xori {r(rt)}, {r(rs)}, 0x{imm:04X}"
    elif op == 0xF:
        return f"lui {r(rt)}, 0x{imm:04X}"
    elif op == 0x20:
        return f"lb {r(rt)}, {imm_signed}({r(rs)})"
    elif op == 0x21:
        return f"lh {r(rt)}, {imm_signed}({r(rs)})"
    elif op == 0x23:
        return f"lw {r(rt)}, {imm_signed}({r(rs)})"
    elif op == 0x24:
        return f"lbu {r(rt)}, {imm_signed}({r(rs)})"
    elif op == 0x25:
        return f"lhu {r(rt)}, {imm_signed}({r(rs)})"
    elif op == 0x28:
        return f"sb {r(rt)}, {imm_signed}({r(rs)})"
    elif op == 0x29:
        return f"sh {r(rt)}, {imm_signed}({r(rs)})"
    elif op == 0x2B:
        return f"sw {r(rt)}, {imm_signed}({r(rs)})"
    elif op == 0x10:  # COP0
        if rs == 0:
            return f"mfc0 {r(rt)}, ${rd}"
        elif rs == 4:
            return f"mtc0 {r(rt)}, ${rd}"
        elif rs == 0x10:
            return "rfe"
        else:
            return f"cop0_{rs:02X} {r(rt)}, ${rd}"
    else:
        return f"op{op:02X} {r(rt)}, {r(rs)}, 0x{imm:04X}"

# Target PC from watchpoint: 0x8004AB68
target_pc = 0x8004AB68

print(f"Target PC: 0x{target_pc:08X}")
print("This PC is in RAM, so it's game code or BIOS code copied to RAM.")
print()
print("To analyze this code, we need to:")
print("1. Run the emulator until the watchpoint fires")
print("2. Dump the RAM region around this PC")
print("3. Disassemble the code")
print()
print("The watchpoint showed:")
print("  PC=0x8004AB68 writes 0x48 to addr 0x801FFEF8")
print("  This is the 'minutes' byte of MSF on the stack")
print()
print("Possible causes:")
print("1. Code reads corrupted LBA from FCB/memory")
print("2. Code has a bug in LBA→MSF conversion")
print("3. DMA overwrote the FCB with sector data")
print()
print("The value 0x48 = 72 decimal = 'H' ASCII")
print("MSF 48:18:38 = LBA 217238, which is beyond disc end (198481)")
