#pragma once

#include <cstdint>
#include <unordered_map>

namespace r3000
{

// Lightweight per-PC provenance analyzer for token propagation.
// It learns a rule once per PC and reuses it on subsequent executions.
class CpuProvenanceAnalyzer
{
  public:
    static constexpr uint32_t kNoToken = 0xFFFFFFFFu;

    // Infer destination token for the instruction at PC.
    // Returns kNoToken when no safe propagation rule applies.
    uint32_t infer_reg_token(
        uint32_t pc, uint32_t call_ctx, uint32_t instr, const uint32_t reg_tokens[32]);

    uint64_t cache_hits() const { return cache_hits_; }
    uint64_t cache_misses() const { return cache_misses_; }

  private:
    enum class RuleKind : uint8_t
    {
        none = 0,
        copy_rs,
        copy_rt,
        merge_rs_rt,
        copy_rs_if_rt_none,
    };

    struct Rule
    {
        RuleKind kind{RuleKind::none};
    };

    static uint32_t rs(uint32_t i) { return (i >> 21) & 0x1Fu; }
    static uint32_t rt(uint32_t i) { return (i >> 16) & 0x1Fu; }
    static uint32_t rd(uint32_t i) { return (i >> 11) & 0x1Fu; }
    static uint32_t op(uint32_t i) { return (i >> 26) & 0x3Fu; }
    static uint32_t funct(uint32_t i) { return i & 0x3Fu; }
    static uint16_t imm_u(uint32_t i) { return (uint16_t)(i & 0xFFFFu); }
    static uint32_t shamt(uint32_t i) { return (i >> 6) & 0x1Fu; }

    static Rule analyze_instruction(uint32_t instr);
    static uint32_t apply_rule(const Rule& r, uint32_t instr, const uint32_t reg_tokens[32]);

    std::unordered_map<uint64_t, Rule> pc_rules_{};
    uint64_t cache_hits_{0};
    uint64_t cache_misses_{0};
};

} // namespace r3000
