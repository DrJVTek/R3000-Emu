#pragma once

#include <cstdint>
#include <cstring>
#include <map>

#include "../gte/gte_snapshot.h"

namespace gpu
{

// Key for correlating GPU polygon vertices with GTE RTPS/RTPT output.
// Holds 3 projected screen coords (before draw offset).
// operator< uses memcmp for fast STL map lookup (user-specified design).
struct GteSxyKey
{
    int16_t sx[3], sy[3];

    bool operator<(const GteSxyKey& o) const
    {
        return std::memcmp(this, &o, sizeof(GteSxyKey)) < 0;
    }
    bool operator==(const GteSxyKey& o) const
    {
        return std::memcmp(this, &o, sizeof(GteSxyKey)) == 0;
    }
};

// Result of a successful correlation lookup.
struct GteCorrelation
{
    gte::GteSnapshot snapshot;
    uint32_t hits;  // Number of times this entry was matched by GPU
};

// Double-buffered correlation table. Populated by GTE (RTPS/RTPT), queried by GPU.
// GTE writes to the "write" table. GPU reads from BOTH write + read tables.
// At VBlank: write→read, clear new write table.
// This handles PS1 double-buffered rendering where DMA sends frame N's OT
// while GTE computes frame N+1's geometry.
class GteCorrelationTable
{
  public:
    // Record a GTE snapshot (called after each RTPS/RTPT).
    void record(const gte::GteSnapshot& snap);

    // Lookup by screen coords (searches both tables). Returns nullptr if no match.
    GteCorrelation* lookup_mut(const GteSxyKey& key);

    // Swap tables at VBlank: write→read, clear new write.
    void swap_frame();

    // Per-frame stats
    size_t size() const { return write_.size() + read_.size(); }
    uint32_t frame_records() const { return frame_records_; }
    uint32_t frame_hits() const { return frame_hits_; }

    // Debug: dump first N table entries to log
    void dump_first_entries(int n) const;

  private:
    using Table = std::map<GteSxyKey, GteCorrelation>;
    Table write_;   // Current frame: GTE writes here
    Table read_;    // Previous frame: GPU reads here (+ write_)
    uint32_t frame_records_{0};
    uint32_t frame_hits_{0};
    int diag_record_log_{0};
    int diag_record_late_{0};
};

} // namespace gpu
