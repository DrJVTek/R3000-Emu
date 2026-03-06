#include "provenance_hotspot_profiler.h"

namespace emu
{

void ProvenanceHotspotProfiler::reset()
{
    pcs_.clear();
}

bool ProvenanceHotspotProfiler::ingest(const r3000::Bus::Dma2NoHintSummary& s)
{
    bool changed = false;
    for (const auto& kv : s.top_pcs)
    {
        const uint32_t pc = kv.first;
        const uint32_t w = kv.second;
        auto& st = pcs_[pc];
        const uint64_t before_words = st.total_words;
        const uint32_t before_seen = st.last_seen_vblank;
        st.total_words += w;
        st.last_seen_vblank = s.vblank;
        if (st.total_words != before_words || st.last_seen_vblank != before_seen)
            changed = true;
    }
    return changed;
}

ProvenanceHotspotProfiler::RefreshDecision
ProvenanceHotspotProfiler::decide_refresh(uint32_t vblank_now)
{
    // Generic thresholds (not game-specific):
    // - heavy frame contributor
    // - request at most every ~3 seconds (@60Hz => 180 vblanks) for same PC.
    static constexpr uint32_t kMinWords = 256;
    static constexpr uint32_t kCooldownVblank = 180;

    RefreshDecision d{};
    uint32_t best_pc = 0;
    uint32_t best_weight = 0;
    for (const auto& kv : pcs_)
    {
        const uint32_t pc = kv.first;
        const PcState& st = kv.second;
        if (st.last_seen_vblank != vblank_now)
            continue;
        const uint32_t w = static_cast<uint32_t>(st.total_words > 0xFFFFFFFFull ? 0xFFFFFFFFu : st.total_words);
        if (w < kMinWords)
            continue;
        if (vblank_now > st.last_refresh_vblank &&
            (vblank_now - st.last_refresh_vblank) < kCooldownVblank)
            continue;
        if (w > best_weight)
        {
            best_weight = w;
            best_pc = pc;
        }
    }

    if (best_pc != 0)
    {
        d.request = true;
        d.pc = best_pc;
        d.weight = best_weight;
        pcs_[best_pc].last_refresh_vblank = vblank_now;
    }
    return d;
}

void ProvenanceHotspotProfiler::ack_refresh(uint32_t pc, uint32_t vblank_now)
{
    if (pc == 0)
        return;
    auto it = pcs_.find(pc);
    if (it == pcs_.end())
        return;
    it->second.last_refresh_vblank = vblank_now;
}

std::vector<ProvenanceHotspotProfiler::PcSnapshot> ProvenanceHotspotProfiler::snapshot() const
{
    std::vector<PcSnapshot> out;
    out.reserve(pcs_.size());
    for (const auto& kv : pcs_)
    {
        PcSnapshot s{};
        s.pc = kv.first;
        s.total_words = kv.second.total_words;
        s.last_seen_vblank = kv.second.last_seen_vblank;
        s.last_refresh_vblank = kv.second.last_refresh_vblank;
        out.push_back(s);
    }
    return out;
}

void ProvenanceHotspotProfiler::restore(const std::vector<PcSnapshot>& data)
{
    pcs_.clear();
    for (const auto& s : data)
    {
        if (s.pc == 0)
            continue;
        PcState st{};
        st.total_words = s.total_words;
        st.last_seen_vblank = s.last_seen_vblank;
        st.last_refresh_vblank = s.last_refresh_vblank;
        pcs_[s.pc] = st;
    }
}

} // namespace emu
