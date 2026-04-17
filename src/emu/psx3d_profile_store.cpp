#include "psx3d_profile_store.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace emu
{

static Psx3dProfileData::ModeKind parse_mode_kind(const char* s)
{
    if (!s)
        return Psx3dProfileData::ModeKind::unknown;
    // Type A
    if (std::strcmp(s, "ot_classic_rtpt") == 0)
        return Psx3dProfileData::ModeKind::ot_classic_rtpt;
    if (std::strcmp(s, "paired_edge_rtpt_gt4") == 0)
        return Psx3dProfileData::ModeKind::paired_edge_rtpt_gt4;
    // Type B
    if (std::strcmp(s, "direct_dma_submission") == 0)
        return Psx3dProfileData::ModeKind::direct_dma_submission;
    // Type C
    if (std::strcmp(s, "chained_polygon_stream") == 0)
        return Psx3dProfileData::ModeKind::chained_polygon_stream;
    // Type D
    if (std::strcmp(s, "skinned_cpu_transform") == 0)
        return Psx3dProfileData::ModeKind::skinned_cpu_transform;
    // Type E
    if (std::strcmp(s, "tmd_compiled") == 0)
        return Psx3dProfileData::ModeKind::tmd_compiled;
    if (std::strcmp(s, "subdivided_ft4_intpl_rtpt") == 0)
        return Psx3dProfileData::ModeKind::subdivided_ft4_intpl_rtpt;
    // Type F
    if (std::strcmp(s, "billboard_radial") == 0)
        return Psx3dProfileData::ModeKind::billboard_radial;
    return Psx3dProfileData::ModeKind::unknown;
}

static const char* format_mode_kind(Psx3dProfileData::ModeKind mode)
{
    switch (mode)
    {
    case Psx3dProfileData::ModeKind::ot_classic_rtpt:
        return "ot_classic_rtpt";
    case Psx3dProfileData::ModeKind::paired_edge_rtpt_gt4:
        return "paired_edge_rtpt_gt4";
    case Psx3dProfileData::ModeKind::direct_dma_submission:
        return "direct_dma_submission";
    case Psx3dProfileData::ModeKind::chained_polygon_stream:
        return "chained_polygon_stream";
    case Psx3dProfileData::ModeKind::skinned_cpu_transform:
        return "skinned_cpu_transform";
    case Psx3dProfileData::ModeKind::tmd_compiled:
        return "tmd_compiled";
    case Psx3dProfileData::ModeKind::subdivided_ft4_intpl_rtpt:
        return "subdivided_ft4_intpl_rtpt";
    case Psx3dProfileData::ModeKind::billboard_radial:
        return "billboard_radial";
    default:
        return "unknown";
    }
}

static Psx3dProfileData::LinkRule parse_link_rule(const char* s)
{
    if (!s)
        return Psx3dProfileData::LinkRule::unknown;
    if (std::strcmp(s, "packet_edge_pairs") == 0)
        return Psx3dProfileData::LinkRule::packet_edge_pairs;
    if (std::strcmp(s, "packet_segments") == 0)
        return Psx3dProfileData::LinkRule::packet_edge_pairs;
    return Psx3dProfileData::LinkRule::unknown;
}

static const char* format_link_rule(Psx3dProfileData::LinkRule rule)
{
    switch (rule)
    {
    case Psx3dProfileData::LinkRule::packet_edge_pairs:
        return "packet_edge_pairs";
    default:
        return "unknown";
    }
}

static bool parse_pc_range_token(const char* token, Psx3dProfileData::PcRange& out)
{
    if (!token || !*token)
        return false;
    unsigned int start = 0;
    unsigned int end = 0;
    if (std::sscanf(token, "0x%X-0x%X", &start, &end) == 2)
    {
        out.start = start;
        out.end = end;
        return true;
    }
    if (std::sscanf(token, "0x%X", &start) == 1)
    {
        out.start = start;
        out.end = start;
        return true;
    }
    return false;
}

static std::string format_pc_range(const Psx3dProfileData::PcRange& r)
{
    char buf[64];
    if (r.start == r.end)
        std::snprintf(buf, sizeof(buf), "0x%08X", r.start);
    else
        std::snprintf(buf, sizeof(buf), "0x%08X-0x%08X", r.start, r.end);
    return std::string(buf);
}

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

std::string Psx3dProfileStore::default_profile_path(const std::string& root_dir, const std::string& game_id)
{
    const std::string id = sanitize_id(game_id);
    std::filesystem::path p = root_dir.empty() ? (std::filesystem::path("profiles") / "psx3d")
                                               : std::filesystem::path(root_dir);
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
            if (std::strncmp(line, "PSX3D_PROFILE_V1", 16) == 0 ||
                std::strncmp(line, "PSX3D_PROFILE_V2", 16) == 0)
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

        char mode_name[128]{};
        char link_rule[128]{};
        unsigned int priority = 0;
        if (std::sscanf(line, "MODE %127s %127s %u", mode_name, link_rule, &priority) == 3)
        {
            Psx3dProfileData::ModeRule rule{};
            rule.mode = parse_mode_kind(mode_name);
            rule.link_rule = parse_link_rule(link_rule);
            rule.priority = priority;
            out.mode_rules.push_back(rule);
            continue;
        }

        auto parse_mode_range_line =
            [&](const char* prefix, std::vector<Psx3dProfileData::PcRange> Psx3dProfileData::ModeRule::*member) -> bool
        {
            if (out.mode_rules.empty())
                return false;
            const size_t prefix_len = std::strlen(prefix);
            if (std::strncmp(line, prefix, prefix_len) != 0)
                return false;
            char* p = line + prefix_len;
            while (*p == ' ' || *p == '\t')
                ++p;
            while (*p)
            {
                while (*p == ' ' || *p == '\t')
                    ++p;
                if (*p == '\0' || *p == '\r' || *p == '\n')
                    break;
                char token[64]{};
                size_t ti = 0;
                while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
                       ti + 1 < sizeof(token))
                {
                    token[ti++] = *p++;
                }
                token[ti] = '\0';
                Psx3dProfileData::PcRange range{};
                if (parse_pc_range_token(token, range))
                    (out.mode_rules.back().*member).push_back(range);
            }
            return true;
        };

        if (parse_mode_range_line("MODE_PRODUCER_RANGES", &Psx3dProfileData::ModeRule::producer_pc_ranges))
            continue;
        if (parse_mode_range_line("MODE_GTE_RANGES", &Psx3dProfileData::ModeRule::gte_pc_ranges))
            continue;
        if (parse_mode_range_line("MODE_OT_RANGES", &Psx3dProfileData::ModeRule::ot_fill_pc_ranges))
            continue;

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

        // QUIRK <name> <int_value>
        // Game-specific render quirks. Unknown names are silently ignored
        // (forward compat: a profile from a newer build with extra quirks
        // can still be parsed by an older build).
        char quirk_name[64]{};
        int quirk_value = 0;
        if (std::sscanf(line, "QUIRK %63s %d", quirk_name, &quirk_value) == 2)
        {
            if (std::strcmp(quirk_name, "force_gte_geom_offset_zero") == 0)
                out.quirks.force_gte_geom_offset_zero = (quirk_value != 0);
            // Add new quirks here as they're introduced.
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

    std::fprintf(f, "PSX3D_PROFILE_V2\n");
    std::fprintf(f, "GAME %s\n", sanitize_id(data.game_id).c_str());

    // Quirks: only emit non-default values to keep files lean. The reader
    // ignores unknown quirk names, so adding new quirks here is forward-safe.
    if (data.quirks.force_gte_geom_offset_zero)
        std::fprintf(f, "QUIRK force_gte_geom_offset_zero 1\n");

    for (const auto& rule : data.mode_rules)
    {
        if (rule.mode == Psx3dProfileData::ModeKind::unknown)
            continue;
        std::fprintf(
            f,
            "MODE %s %s %u\n",
            format_mode_kind(rule.mode),
            format_link_rule(rule.link_rule),
            rule.priority);
        auto write_ranges = [&](const char* prefix, const std::vector<Psx3dProfileData::PcRange>& ranges)
        {
            if (ranges.empty())
                return;
            std::fprintf(f, "%s", prefix);
            for (const auto& range : ranges)
                std::fprintf(f, " %s", format_pc_range(range).c_str());
            std::fprintf(f, "\n");
        };
        write_ranges("MODE_PRODUCER_RANGES", rule.producer_pc_ranges);
        write_ranges("MODE_GTE_RANGES", rule.gte_pc_ranges);
        write_ranges("MODE_OT_RANGES", rule.ot_fill_pc_ranges);
    }
    // V2 runtime profiles stay lean: only game identity and validated mode rules.
    // Legacy hotspot / analyzed-PC / camera snapshots are still accepted by the
    // reader for backward compatibility, but are no longer emitted here.
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
