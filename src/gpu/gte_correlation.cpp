#include "gte_correlation.h"
#include "log/emu_log.h"

namespace gpu
{

void GteCorrelationTable::record(const gte::GteSnapshot& snap)
{
    if (!snap.valid) return;

    GteSxyKey key{};
    key.sx[0] = snap.sx[0]; key.sy[0] = snap.sy[0];
    key.sx[1] = snap.sx[1]; key.sy[1] = snap.sy[1];
    key.sx[2] = snap.sx[2]; key.sy[2] = snap.sy[2];

    // Diagnostic: log recordings — first 10 early + first 10 late
    const bool early_rec = (diag_record_log_ < 10 && frame_records_ > 5);
    const bool late_rec  = (diag_record_late_ < 10 && frame_records_ > 5
                            && diag_record_log_ >= 10);
    if (early_rec || late_rec)
    {
        emu::logf(emu::LogLevel::warn, "GTE",
            "CORR_REC #%d/%d: key=(%d,%d)(%d,%d)(%d,%d) 3D=(%d,%d,%d) w=%u r=%u frec=%u",
            diag_record_log_, diag_record_late_,
            key.sx[0], key.sy[0], key.sx[1], key.sy[1], key.sx[2], key.sy[2],
            snap.vertices[2].vx, snap.vertices[2].vy, snap.vertices[2].vz,
            (unsigned)write_.size(), (unsigned)read_.size(), frame_records_);
        if (early_rec) ++diag_record_log_;
        if (late_rec)  ++diag_record_late_;
    }

    write_[key] = {snap, 0};
    ++frame_records_;
}

GteCorrelation* GteCorrelationTable::lookup_mut(const GteSxyKey& key)
{
    // Search write table first (same-frame matches), then read (cross-frame)
    auto it = write_.find(key);
    if (it != write_.end())
    {
        ++frame_hits_;
        ++it->second.hits;
        return &it->second;
    }
    it = read_.find(key);
    if (it != read_.end())
    {
        ++frame_hits_;
        ++it->second.hits;
        return &it->second;
    }
    return nullptr;
}

void GteCorrelationTable::swap_frame()
{
    read_ = std::move(write_);
    write_.clear();
    frame_records_ = 0;
    frame_hits_ = 0;
}

void GteCorrelationTable::dump_first_entries(int n) const
{
    int i = 0;
    // Dump from read table (previous frame — what GPU looks up)
    for (auto it = read_.begin(); it != read_.end() && i < n; ++it, ++i)
    {
        const auto& k = it->first;
        const auto& s = it->second.snapshot;
        emu::logf(emu::LogLevel::warn, "GPU",
            "  TBL[%d] key=(%d,%d)(%d,%d)(%d,%d) 3D=(%d,%d,%d)(%d,%d,%d)(%d,%d,%d)",
            i, k.sx[0], k.sy[0], k.sx[1], k.sy[1], k.sx[2], k.sy[2],
            s.vertices[0].vx, s.vertices[0].vy, s.vertices[0].vz,
            s.vertices[1].vx, s.vertices[1].vy, s.vertices[1].vz,
            s.vertices[2].vx, s.vertices[2].vy, s.vertices[2].vz);
    }
}

} // namespace gpu
