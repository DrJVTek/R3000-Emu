#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../r3000/bus.h"

namespace emu
{

// Generic hotspot profiler for "DMA2 words without token".
// Learns heavy writer PCs and asks for analysis refresh with cooldown.
class ProvenanceHotspotProfiler
{
  public:
    struct PcSnapshot
    {
        uint32_t pc{0};
        uint64_t total_words{0};
        uint32_t last_seen_vblank{0};
        uint32_t last_refresh_vblank{0};
    };

    struct RefreshDecision
    {
        bool request{false};
        uint32_t pc{0};
        uint32_t weight{0};
    };

    void reset();
    bool ingest(const r3000::Bus::Dma2NoHintSummary& s);
    RefreshDecision decide_refresh(uint32_t vblank_now);
    void ack_refresh(uint32_t pc, uint32_t vblank_now);
    std::vector<PcSnapshot> snapshot() const;
    void restore(const std::vector<PcSnapshot>& data);

  private:
    struct PcState
    {
        uint64_t total_words{0};
        uint32_t last_seen_vblank{0};
        uint32_t last_refresh_vblank{0};
    };

    std::unordered_map<uint32_t, PcState> pcs_{};
};

} // namespace emu
