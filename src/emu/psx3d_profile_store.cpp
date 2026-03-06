#include "psx3d_profile_store.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace emu
{

static std::string sanitize_id(const std::string& in)
{
    if (in.empty())
        return "unknown";
    std::string out;
    out.reserve(in.size());
    for (char c : in)
    {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        out.push_back(ok ? c : '_');
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    if (out.empty())
        out = "unknown";
    return out;
}

std::string Psx3dProfileStore::default_profile_path(const std::string& game_id)
{
    const std::string id = sanitize_id(game_id);
    std::filesystem::path p("profiles");
    p /= "psx3d";
    p /= (id + ".psx3dprof");
    return p.string();
}

bool Psx3dProfileStore::load(const std::string& path, Psx3dProfileData& out)
{
    out = {};
    if (path.empty())
        return false;

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;

    char line[512];
    bool header_ok = false;
    while (std::fgets(line, sizeof(line), f))
    {
        if (!header_ok)
        {
            if (std::strncmp(line, "PSX3D_PROFILE_V1", 16) == 0)
            {
                header_ok = true;
                continue;
            }
            std::fclose(f);
            return false;
        }

        char gid[256]{};
        if (std::sscanf(line, "GAME %255s", gid) == 1)
        {
            out.game_id = gid;
            continue;
        }

        ProvenanceHotspotProfiler::PcSnapshot s{};
        unsigned int pc = 0;
        unsigned long long total_words = 0;
        unsigned int last_seen = 0;
        unsigned int last_refresh = 0;
        if (std::sscanf(
                line,
                "PC 0x%X %llu %u %u",
                &pc,
                &total_words,
                &last_seen,
                &last_refresh) == 4)
        {
            s.pc = (uint32_t)pc;
            s.total_words = (uint64_t)total_words;
            s.last_seen_vblank = (uint32_t)last_seen;
            s.last_refresh_vblank = (uint32_t)last_refresh;
            out.hotspots.push_back(s);
            continue;
        }

        unsigned int apc = 0;
        if (std::sscanf(line, "ANALYZED_PC 0x%X", &apc) == 1)
        {
            out.analyzed_pcs.push_back((uint32_t)apc);
            continue;
        }

        Psx3dProfileData::CameraCandidate cam{};
        unsigned int cam_addr = 0;
        unsigned int cam_hits = 0;
        unsigned int cam_frame_hits = 0;
        unsigned int cam_first = 0;
        unsigned int cam_last = 0;
        unsigned int cam_last_pc = 0;
        unsigned int cam_regs = 0;
        if (std::sscanf(
                line,
                "CAM 0x%X %u %u %u %u 0x%X 0x%X",
                &cam_addr,
                &cam_hits,
                &cam_frame_hits,
                &cam_first,
                &cam_last,
                &cam_last_pc,
                &cam_regs) == 7)
        {
            cam.addr = (uint32_t)cam_addr;
            cam.hits = (uint32_t)cam_hits;
            cam.frame_hits = (uint32_t)cam_frame_hits;
            cam.first_vblank = (uint32_t)cam_first;
            cam.last_vblank = (uint32_t)cam_last;
            cam.last_pc = (uint32_t)cam_last_pc;
            cam.gte_reg_mask = (uint32_t)cam_regs;
            out.camera_candidates.push_back(cam);
        }
    }
    std::fclose(f);
    return header_ok;
}

bool Psx3dProfileStore::save(const std::string& path, const Psx3dProfileData& data)
{
    if (path.empty())
        return false;

    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);

    const std::filesystem::path tmp = p.string() + ".tmp";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f)
        return false;

    std::fprintf(f, "PSX3D_PROFILE_V1\n");
    std::fprintf(f, "GAME %s\n", sanitize_id(data.game_id).c_str());
    std::vector<ProvenanceHotspotProfiler::PcSnapshot> sorted = data.hotspots;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.total_words != b.total_words)
            return a.total_words > b.total_words;
        return a.pc < b.pc;
    });
    for (const auto& s : sorted)
    {
        if (s.pc == 0 || s.total_words == 0)
            continue;
        std::fprintf(
            f,
            "PC 0x%08X %" PRIu64 " %u %u\n",
            s.pc,
            s.total_words,
            s.last_seen_vblank,
            s.last_refresh_vblank);
    }
    std::vector<uint32_t> analyzed = data.analyzed_pcs;
    std::sort(analyzed.begin(), analyzed.end());
    analyzed.erase(std::unique(analyzed.begin(), analyzed.end()), analyzed.end());
    for (uint32_t pc : analyzed)
    {
        if (pc == 0)
            continue;
        std::fprintf(f, "ANALYZED_PC 0x%08X\n", pc);
    }
    std::vector<Psx3dProfileData::CameraCandidate> cams = data.camera_candidates;
    std::sort(cams.begin(), cams.end(), [](const auto& a, const auto& b) {
        if (a.frame_hits != b.frame_hits)
            return a.frame_hits > b.frame_hits;
        if (a.hits != b.hits)
            return a.hits > b.hits;
        return a.addr < b.addr;
    });
    for (const auto& c : cams)
    {
        if (c.addr == 0 || c.hits == 0)
            continue;
        std::fprintf(
            f,
            "CAM 0x%08X %u %u %u %u 0x%08X 0x%08X\n",
            c.addr,
            c.hits,
            c.frame_hits,
            c.first_vblank,
            c.last_vblank,
            c.last_pc,
            c.gte_reg_mask);
    }
    std::fflush(f);
    std::fclose(f);

    std::filesystem::rename(tmp, p, ec);
    if (ec)
    {
        std::filesystem::remove(p, ec);
        ec.clear();
        std::filesystem::rename(tmp, p, ec);
        if (ec)
        {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

} // namespace emu
