#pragma once

#include <string>
#include <vector>

#include "provenance_hotspot_profiler.h"

namespace emu
{

struct Psx3dProfileData
{
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

    std::string game_id{};
    std::vector<ProvenanceHotspotProfiler::PcSnapshot> hotspots{};
    std::vector<uint32_t> analyzed_pcs{};
    std::vector<CameraCandidate> camera_candidates{};
};

class Psx3dProfileStore
{
  public:
    static std::string default_profile_path(const std::string& game_id);
    static bool load(const std::string& path, Psx3dProfileData& out);
    static bool save(const std::string& path, const Psx3dProfileData& data);
};

} // namespace emu
