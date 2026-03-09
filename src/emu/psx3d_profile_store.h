#pragma once

#include <string>
#include <vector>

#include "provenance_hotspot_profiler.h"

namespace emu
{

struct Psx3dProfileData
{
    enum class ModeKind : uint8_t
    {
        unknown = 0,
        paired_edge_rtpt_gt4,
        subdivided_ft4_intpl_rtpt,
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

    std::string game_id{};
    std::vector<ProvenanceHotspotProfiler::PcSnapshot> hotspots{};
    std::vector<uint32_t> analyzed_pcs{};
    std::vector<CameraCandidate> camera_candidates{};
    std::vector<ModeRule> mode_rules{};
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
