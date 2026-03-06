#include "cpu_provenance_analyzer.h"

namespace r3000
{

CpuProvenanceAnalyzer::Rule CpuProvenanceAnalyzer::analyze_instruction(uint32_t instr)
{
    Rule r{};
    const uint32_t opcode = op(instr);

    if (opcode == 0x00u)
    {
        const uint32_t f = funct(instr);
        switch (f)
        {
            case 0x00u: // SLL rd, rt, shamt
                if ((rd(instr) & 31u) != 0u && (rt(instr) & 31u) != 0u)
                    r.kind = RuleKind::copy_rt;
                break;
            case 0x02u: // SRL rd, rt, shamt
            case 0x03u: // SRA rd, rt, shamt
            case 0x04u: // SLLV rd, rt, rs
            case 0x06u: // SRLV rd, rt, rs
            case 0x07u: // SRAV rd, rt, rs
                r.kind = RuleKind::copy_rt;
                break;
            case 0x20u: // ADD rd, rs, rt
            case 0x21u: // ADDU rd, rs, rt
                r.kind = RuleKind::merge_rs_rt;
                break;
            case 0x22u: // SUB rd, rs, rt
            case 0x23u: // SUBU rd, rs, rt
                r.kind = RuleKind::copy_rs_if_rt_none;
                break;
            case 0x24u: // AND rd, rs, rt
            case 0x25u: // OR rd, rs, rt
            case 0x26u: // XOR rd, rs, rt
            case 0x27u: // NOR rd, rs, rt
                r.kind = RuleKind::merge_rs_rt;
                break;
            default:
                break;
        }
        return r;
    }

    if (opcode == 0x08u) // ADDI rt, rs, imm
    {
        if ((rt(instr) & 31u) != 0u)
            r.kind = RuleKind::copy_rs;
        return r;
    }

    if (opcode == 0x09u) // ADDIU rt, rs, imm
    {
        if ((rt(instr) & 31u) != 0u)
            r.kind = RuleKind::copy_rs;
        return r;
    }

    if (opcode == 0x0Cu) // ANDI rt, rs, imm
    {
        if ((rt(instr) & 31u) != 0u)
            r.kind = RuleKind::copy_rs;
        return r;
    }

    if (opcode == 0x0Du) // ORI rt, rs, imm
    {
        if ((rt(instr) & 31u) != 0u)
            r.kind = RuleKind::copy_rs;
        return r;
    }

    if (opcode == 0x0Eu) // XORI rt, rs, imm
    {
        if ((rt(instr) & 31u) != 0u)
            r.kind = RuleKind::copy_rs;
        return r;
    }

    return r;
}

uint32_t CpuProvenanceAnalyzer::apply_rule(const Rule& r, uint32_t instr, const uint32_t reg_tokens[32])
{
    const uint32_t tok_rs = reg_tokens[rs(instr) & 31u];
    const uint32_t tok_rt = reg_tokens[rt(instr) & 31u];
    switch (r.kind)
    {
        case RuleKind::copy_rs:
            return tok_rs;
        case RuleKind::copy_rt:
            return tok_rt;
        case RuleKind::merge_rs_rt:
            if (tok_rs == kNoToken)
                return tok_rt;
            if (tok_rt == kNoToken)
                return tok_rs;
            if (tok_rs == tok_rt)
                return tok_rs;
            return kNoToken;
        case RuleKind::copy_rs_if_rt_none:
            return (tok_rt == kNoToken) ? tok_rs : kNoToken;
        default:
            return kNoToken;
    }
}

uint32_t CpuProvenanceAnalyzer::infer_reg_token(
    uint32_t pc, uint32_t call_ctx, uint32_t instr, const uint32_t reg_tokens[32])
{
    const uint64_t key = ((uint64_t)call_ctx << 32) | (uint64_t)pc;
    auto it = pc_rules_.find(key);
    if (it == pc_rules_.end())
    {
        const Rule r = analyze_instruction(instr);
        it = pc_rules_.emplace(key, r).first;
        ++cache_misses_;
    }
    else
    {
        ++cache_hits_;
    }

    return apply_rule(it->second, instr, reg_tokens);
}

} // namespace r3000
