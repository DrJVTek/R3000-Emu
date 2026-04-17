#pragma once

#include <string>
#include <vector>

#include "provenance_hotspot_profiler.h"

namespace emu
{

struct Psx3dProfileData
{
    // Render-loop pattern taxonomy.
    // See docs/PSX_RENDER_LOOP_PLAYBOOK.md for the full specification of each
    // value, its MIPS signature, observable signals, and which game(s) use it.
    // The 6 canonical types (A-F in the playbook) cover ~95% of commercial
    // PS1 games; the specific variants (paired_edge_rtpt_gt4, etc.) are
    // concrete sub-patterns observed on real games.
    //
    // When adding a new value here:
    //   1. Document it in the playbook first (§3 + §7)
    //   2. Wire parse/format in psx3d_profile_store.cpp (parse_mode_kind,
    //      format_mode_kind — both symmetrical)
    //   3. Propagate mode-specific behaviour in Gpu3D / PSX3DRenderComponent
    enum class ModeKind : uint8_t
    {
        unknown = 0,
        // Type A — OT classic RTPT (vanilla PSYQ AddPrim + RTPT)
        ot_classic_rtpt,
        // Type A variant — edge-reuse GT4 (Ridge Racer flag: DrawFlag@0x80026110)
        paired_edge_rtpt_gt4,
        // Type B — Direct DMA submission, no OT
        direct_dma_submission,
        // Type C — Chained polygon stream, next-pointer embedded in poly struct
        chained_polygon_stream,
        // Type D — Skinned / software CPU transform, GTE used only for projection
        skinned_cpu_transform,
        // Type E — TMD compiled / generic DivPolygon helpers
        tmd_compiled,
        // Type E variant — INTPL subdivision (Ridge Racer gameplay: DivPloyFT4@0x80047E38)
        subdivided_ft4_intpl_rtpt,
        // Type F — Billboard / radial (1 vertex RTPS + CPU quad construction)
        billboard_radial,
    };

    enum class LinkRule : uint8_t
    {
        unknown = 0,
        packet_edge_pairs,
    };

    struct PcRange
    {
        uint32_t start{0};
        uint32_t end{0};
    };

    struct CameraCandidate
    {
        uint32_t addr{0};
        uint32_t hits{0};
        uint32_t frame_hits{0};
        uint32_t first_vblank{0};
        uint32_t last_vblank{0};
        uint32_t last_pc{0};
        uint32_t gte_reg_mask{0};
    };

    struct ModeRule
    {
        ModeKind mode{ModeKind::unknown};
        LinkRule link_rule{LinkRule::unknown};
        uint32_t priority{0};
        std::vector<PcRange> producer_pc_ranges{};
        std::vector<PcRange> gte_pc_ranges{};
        std::vector<PcRange> ot_fill_pc_ranges{};
    };

    // Game-specific render quirks. These describe non-portable corner cases
    // that we cannot reliably auto-detect from the binary, and that need to be
    // toggled per-game. Each quirk has a stable string name used in the
    // .psx3dprof file format ("QUIRK <name> <value>") and a corresponding
    // bool field here. Default false → no behaviour change for existing games.
    //
    // Adding a new quirk:
    //   1. Add a bool field below
    //   2. Wire its parse/serialize in psx3d_profile_store.cpp (Quirks
    //      handling block in load() and save())
    //   3. Propagate it to the relevant subsystem in Core::try_load_psx3d_profile()
    struct Quirks
    {
        // GTE GeomOffset double-buffer compensation. Used by libgs games like
        // SCEE Demo One TREX that shift their back buffer via SetGeomOffset
        // (cop2 OFX/OFY) instead of GP0(0xE5) draw_offset. When set, the GTE
        // RTPS/RTPT projection math pretends OFX==OFY==0, so the SXY values
        // computed by the GTE land at the same logical screen position
        // regardless of which back buffer the game is currently writing to.
        // See gte::Gte::set_force_geom_offset_zero() for the full policy.
        bool force_gte_geom_offset_zero{false};
    };

    std::string game_id{};
    std::vector<ProvenanceHotspotProfiler::PcSnapshot> hotspots{};
    std::vector<uint32_t> analyzed_pcs{};
    std::vector<CameraCandidate> camera_candidates{};
    std::vector<ModeRule> mode_rules{};
    Quirks quirks{};
};

class Psx3dProfileStore
{
  public:
    static std::string default_profile_path(const std::string& game_id);
    static std::string default_profile_path(const std::string& root_dir, const std::string& game_id);
    static bool load(const std::string& path, Psx3dProfileData& out);
    static bool save(const std::string& path, const Psx3dProfileData& data);
};

} // namespace emu
