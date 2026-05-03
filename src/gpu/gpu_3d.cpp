#include "gpu_3d.h"
#include "../gte/gte_3d.h"
#include "../r3000/bus.h"
#include "../log/emu_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gpu
{

// Forward declaration (defined later with decode functions)
static bool in_dead_zone(uint16_t v);

// Controlled recovery path: only used when no token hint is available.
#ifndef R3000_GPU3D_MISS_ONLY_DECODE_FALLBACK
#define R3000_GPU3D_MISS_ONLY_DECODE_FALLBACK 1
#endif

// Intra-packet token carry: if some coord words in a GP0 packet have no token,
// reuse a valid token observed in the same packet (bounded, deterministic).
#ifndef R3000_GPU3D_PACKET_TOKEN_CARRY
#define R3000_GPU3D_PACKET_TOKEN_CARRY 1
#endif

// TEMP DEBUG ONLY.
// Rebuild 3D using SXY->vertex lookup when the normal GTE->GPU correlation path
// did not produce a usable face/quad mode. This must stay disabled for normal
// runs because it hides producer-side bugs instead of fixing them.
#ifndef R3000_GPU3D_VTX_LOOKUP_FALLBACK
#define R3000_GPU3D_VTX_LOOKUP_FALLBACK 0
#endif

// ---------------------------------------------------------------------------
// GP0 parameter count (handles ALL commands to prevent desync)
// Returns -1 for polyline (variable length), -2 for CPU→VRAM (special)
// ---------------------------------------------------------------------------
int Gpu3D::gp0_param_count(uint8_t cmd)
{
    // Polygons (20h-3Fh)
    if (cmd >= 0x20 && cmd <= 0x3F)
    {
        const bool gouraud  = (cmd & 0x10) != 0;
        const bool quad     = (cmd & 0x08) != 0;
        const bool textured = (cmd & 0x04) != 0;
        int n = quad ? 4 : 3;
        int words = 0;
        for (int i = 0; i < n; ++i)
        {
            if (i > 0 && gouraud) ++words;
            ++words;
            if (textured) ++words;
        }
        return words;
    }

    // Lines (40h-5Fh)
    if (cmd >= 0x40 && cmd <= 0x5F)
    {
        const bool poly = (cmd & 0x08) != 0;
        if (poly) return -1; // polyline: variable length
        const bool gouraud = (cmd & 0x10) != 0;
        // Mono line: cmd + vertex0 + vertex1 = 3 words → params=2
        // Gouraud line: cmd + vertex0 + color1 + vertex1 = 4 words → params=3
        return gouraud ? 3 : 2;
    }

    // Rects (60h-7Fh)
    if (cmd >= 0x60 && cmd <= 0x7F)
    {
        const bool textured = (cmd & 0x04) != 0;
        const int size_code = (cmd >> 3) & 3;
        int p = 1; // vertex
        if (textured) p += 1; // UV+CLUT
        if (size_code == 0) p += 1; // variable size word
        return p;
    }

    // Fill rect (02h)
    if (cmd == 0x02) return 2;

    // VRAM-to-VRAM copy (80h-9Fh)
    if (cmd >= 0x80 && cmd <= 0x9F) return 3;

    // CPU-to-VRAM (A0h-BFh)
    if (cmd >= 0xA0 && cmd <= 0xBF) return 2;

    // VRAM-to-CPU (C0h-DFh)
    if (cmd >= 0xC0 && cmd <= 0xDF) return 2;

    // Environment (E1h-E6h)
    if (cmd >= 0xE1 && cmd <= 0xE6) return 0;

    return 0;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
Gpu3D::Gpu3D()
{
    emu::logf(emu::LogLevel::info, "GPU3D", "Gpu3D shadow GPU v1 (full parser)");
}

void Gpu3D::clear_runtime_mode_rules()
{
    runtime_mode_rules_.clear();
}

void Gpu3D::set_runtime_mode_rules(const std::vector<RuntimeModeRule>& rules)
{
    runtime_mode_rules_ = rules;
}

bool Gpu3D::pc_in_ranges(uint32_t pc, const std::vector<PcRange>& ranges)
{
    for (const auto& r : ranges)
    {
        if (pc >= r.start && pc <= r.end)
            return true;
    }
    return false;
}

bool Gpu3D::rule_matches_writer_pc(const RuntimeModeRule& rule, uint32_t producer_pc)
{
    if (producer_pc == 0)
        return false;
    if (pc_in_ranges(producer_pc, rule.producer_pc_ranges))
        return true;
    // The DMA2 writer PC we observe at packet build time is often the OT fill/builder
    // PC, so OT ranges are also valid selectors for the active mode.
    if (pc_in_ranges(producer_pc, rule.ot_fill_pc_ranges))
        return true;
    return false;
}

const Gpu3D::RuntimeModeRule* Gpu3D::find_runtime_mode_rule(uint32_t producer_pc) const
{
    if (producer_pc == 0)
        return nullptr;
    const RuntimeModeRule* best = nullptr;
    for (const auto& rule : runtime_mode_rules_)
    {
        if (!rule_matches_writer_pc(rule, producer_pc))
            continue;
        if (!best || rule.priority > best->priority)
            best = &rule;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------
void Gpu3D::reset()
{
    gp0_state_ = Gp0State::idle;
    cmd_buf_pos_ = 0;
    cmd_words_needed_ = 0;
    vram_words_remaining_ = 0;
    polyline_gouraud_ = false;
    polyline_phase_ = 0;
    draw_env_ = {};
    display_ = {};
    frame_count_ = 0;
    draw_active_ = 0;
    draw_lists_[0].clear();
    draw_lists_[1].clear();
    for (int i = 0; i < 16; ++i)
        cmd_face_hint_[i] = kNoFaceHint;
}

// ---------------------------------------------------------------------------
// GP0 entry point
// ---------------------------------------------------------------------------
void Gpu3D::gp0(uint32_t word)
{
    gp0_with_face_hint(word, kNoFaceHint, 0);
}

void Gpu3D::gp0_with_face_hint(uint32_t word, uint32_t face_hint, uint32_t writer_pc)
{
    ++gp0_words_accum_;

    // Skipping VRAM transfer data
    if (gp0_state_ == Gp0State::skipping_vram_data)
    {
        if (vram_words_remaining_ > 0)
            --vram_words_remaining_;
        if (vram_words_remaining_ == 0)
            gp0_state_ = Gp0State::idle;
        return;
    }

    // Skipping polyline data — consume until terminator
    if (gp0_state_ == Gp0State::skipping_polyline)
    {
        // PS1 polyline terminator detection:
        // - Non-gouraud polyline: each word is a vertex. Terminator if top nibble
        //   of upper halfword >= 0x5 (bit 28 set) while not being a valid vertex.
        // - Gouraud polyline: alternating color/vertex words. Check the color word.
        //
        // DuckStation approach: for gouraud, check every other word (the "color" word)
        // for (word & 0xF000F000) == 0x55555555 pattern.
        // Mednafen: checks (word & 0xF000F000) == 0x50005000.
        //
        // Robust approach: accept several common terminator patterns AND reset at
        // GP1(01h) (reset command buffer) which games send before new frame rendering.
        const bool term_a = (word & 0xF000'F000u) == 0x5000'5000u;
        const bool term_b = (word & 0xF000'F000u) == 0x5555'5555u;
        // Also check: top byte matches common command range (new command starting)
        // If the word looks like a new GP0 command (0x00-0x1F, 0x20-0x3F, 0x60-0x7F, 0xA0-0xBF, 0xE0-0xEF)
        // it's very unlikely to be polyline data — treat as terminator.
        const uint8_t top = (uint8_t)(word >> 24);
        const bool term_c = (top >= 0x01 && top <= 0x3F) || (top >= 0x60 && top <= 0x7F) ||
                            (top >= 0x80 && top <= 0xBF) || (top >= 0xE0 && top <= 0xEF);
        if (term_a || term_b || term_c)
        {
            gp0_state_ = Gp0State::idle;
            // If it looks like a real command, re-process it
            if (term_c && !term_a && !term_b)
            {
                ++gp0_cmds_accum_;
                gp0_start_command(word, kNoFaceHint, 0);
            }
        }
        return;
    }

    // Collecting parameters
    if (gp0_state_ == Gp0State::collecting_params)
    {
        if (cmd_buf_pos_ < 16)
        {
            cmd_buf_[cmd_buf_pos_++] = word;
            cmd_face_hint_[cmd_buf_pos_ - 1] = face_hint;
            cmd_writer_pc_[cmd_buf_pos_ - 1] = writer_pc;
        }

        if (cmd_buf_pos_ >= cmd_words_needed_)
        {
            gp0_execute();
            if (gp0_state_ == Gp0State::collecting_params)
                gp0_state_ = Gp0State::idle;
        }
        return;
    }

    // Idle: new command
    ++gp0_cmds_accum_;
    gp0_start_command(word, face_hint, writer_pc);
}

// ---------------------------------------------------------------------------
// GP0 start command
// ---------------------------------------------------------------------------
void Gpu3D::gp0_start_command(uint32_t cmd_word, uint32_t face_hint, uint32_t writer_pc)
{
    const uint8_t cmd = (uint8_t)(cmd_word >> 24);
    const int params = gp0_param_count(cmd);

    cmd_buf_[0] = cmd_word;
    cmd_face_hint_[0] = face_hint;
    cmd_writer_pc_[0] = writer_pc;
    cmd_buf_pos_ = 1;

    if (params == 0)
    {
        cmd_words_needed_ = 1;
        gp0_execute();
        return;
    }

    if (params == -1)
    {
        // Polyline: enter skip state. We consume words until terminator.
        polyline_gouraud_ = (cmd & 0x10) != 0;
        polyline_phase_ = 0;
        gp0_state_ = Gp0State::skipping_polyline;
        return;
    }

    cmd_words_needed_ = 1 + params;
    gp0_state_ = Gp0State::collecting_params;
}

// ---------------------------------------------------------------------------
// GP0 execute collected command
// ---------------------------------------------------------------------------
void Gpu3D::gp0_execute()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);

    // Fill rect (02h)
    if (cmd == 0x02)
    {
        gp0_fill_rect();
        return;
    }

    // Polygons (20h-3Fh)
    if (cmd >= 0x20 && cmd <= 0x3F)
    {
        gp0_polygon();
        return;
    }

    // 2-vertex lines (40h-47h, 50h-57h — non-polyline)
    if (cmd >= 0x40 && cmd <= 0x5F)
    {
        gp0_line();
        return;
    }

    // Rects/sprites (60h-7Fh)
    if (cmd >= 0x60 && cmd <= 0x7F)
    {
        gp0_rect();
        return;
    }

    // CPU→VRAM transfer start (A0h-BFh): compute words to skip
    if (cmd >= 0xA0 && cmd <= 0xBF)
    {
        const uint32_t wh = cmd_buf_[2];
        const uint32_t xs = wh & 0xFFFFu;
        const uint32_t ys = (wh >> 16) & 0xFFFFu;
        const uint16_t w = (uint16_t)((xs == 0) ? 0x400u : (((xs - 1) & 0x3FF) + 1));
        const uint16_t h = (uint16_t)((ys == 0) ? 0x200u : (((ys - 1) & 0x1FF) + 1));
        const uint32_t total_pixels = (uint32_t)w * h;
        vram_words_remaining_ = (total_pixels + 1) / 2;
        ++gp0_vram_skips_accum_;
        gp0_state_ = Gp0State::skipping_vram_data;
        return;
    }

    // Environment commands (E1h-E6h): track draw offset
    if (cmd >= 0xE1 && cmd <= 0xE6)
    {
        gp0_env_command();
        return;
    }

    // Everything else (VRAM-to-VRAM, VRAM-to-CPU, NOP): silently ignore
}

// ---------------------------------------------------------------------------
// GP0 fill rect (02h) — push 2 triangles as origin_2d_rect
// ---------------------------------------------------------------------------
void Gpu3D::gp0_fill_rect()
{
    // Shadow GPU: no VRAM — skip fill rects entirely
    (void)cmd_buf_;
}

// Forward declarations for differential decode (defined after push_triangle)
static uint32_t decode_face_tri(uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1,
                                uint16_t x2, uint16_t y2);
static uint32_t decode_face_quad(uint16_t x0, uint16_t y0,
                                 uint16_t x1, uint16_t y1,
                                 uint16_t x2, uint16_t y2,
                                 uint16_t x3, uint16_t y3);

// ---------------------------------------------------------------------------
// GP0 polygon — parse vertices, push triangles with RAW coordinates
// ---------------------------------------------------------------------------
void Gpu3D::gp0_polygon()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const bool gouraud  = (cmd & 0x10) != 0;
    const bool quad     = (cmd & 0x08) != 0;
    const bool textured = (cmd & 0x04) != 0;
    const bool semi     = (cmd & 0x02) != 0;
    const bool raw      = (cmd & 0x01) != 0; // raw texture (no color modulation)
    const int nverts = quad ? 4 : 3;

    uint16_t raw_x[4], raw_y[4];
    uint32_t face_hints[4] = {kNoFaceHint, kNoFaceHint, kNoFaceHint, kNoFaceHint};
    uint32_t coord_writer_pc[4] = {0, 0, 0, 0};
    uint8_t cr[4], cg[4], cb[4];
    uint8_t tu[4] = {}, tv[4] = {};
    uint16_t clut = 0, texpage_attr = 0;

    cr[0] = (uint8_t)(cmd_buf_[0] & 0xFF);
    cg[0] = (uint8_t)((cmd_buf_[0] >> 8) & 0xFF);
    cb[0] = (uint8_t)((cmd_buf_[0] >> 16) & 0xFF);

    int idx = 1;
    for (int i = 0; i < nverts; ++i)
    {
        if (i > 0 && gouraud)
        {
            cr[i] = (uint8_t)(cmd_buf_[idx] & 0xFF);
            cg[i] = (uint8_t)((cmd_buf_[idx] >> 8) & 0xFF);
            cb[i] = (uint8_t)((cmd_buf_[idx] >> 16) & 0xFF);
            idx++;
        }
        else if (i > 0)
        {
            cr[i] = cr[0]; cg[i] = cg[0]; cb[i] = cb[0];
        }

        raw_x[i] = (uint16_t)(cmd_buf_[idx] & 0xFFFFu);
        raw_y[i] = (uint16_t)(cmd_buf_[idx] >> 16);
        face_hints[i] = cmd_face_hint_[idx];
        coord_writer_pc[i] = cmd_writer_pc_[idx];
        idx++;

        if (textured)
        {
            tu[i] = (uint8_t)(cmd_buf_[idx] & 0xFF);
            tv[i] = (uint8_t)((cmd_buf_[idx] >> 8) & 0xFF);
            if (i == 0) clut = (uint16_t)((cmd_buf_[idx] >> 16) & 0xFFFF);
            if (i == 1) texpage_attr = (uint16_t)((cmd_buf_[idx] >> 16) & 0xFFFF);
            idx++;
        }
    }

    uint16_t tp = textured ? texpage_attr : (uint16_t)(draw_env_.texpage_raw & 0xFFFF);
    uint8_t semi_mode = (uint8_t)((tp >> 5) & 3);
    uint8_t tex_depth = (uint8_t)((tp >> 7) & 3);
    uint8_t flags = 0;
    if (textured) flags |= 1;
    if (semi)     flags |= 2;
    if (raw)      flags |= 4;

    if (textured)
        draw_env_.texpage_raw = (draw_env_.texpage_raw & ~0x7FFu) | (texpage_attr & 0x7FFu);

    uint32_t producer_pc = 0;
    int producer_count = 0;
    for (int i = 0; i < nverts; ++i)
    {
        const uint32_t pc = coord_writer_pc[i];
        if (pc == 0)
            continue;
        int c = 0;
        for (int j = 0; j < nverts; ++j)
            if (coord_writer_pc[j] == pc)
                ++c;
        if (c > producer_count)
        {
            producer_count = c;
            producer_pc = pc;
        }
    }

#if R3000_GPU3D_PACKET_TOKEN_CARRY
    // Recover missing coord hints from any valid hint in the same command packet.
    uint32_t packet_hint = kNoFaceHint;
    for (int k = 0; k < cmd_buf_pos_; ++k)
    {
        const uint32_t h = cmd_face_hint_[k];
        if (h == kNoFaceHint || h == 0u)
            continue;
        if (gte_3d_ && gte_3d_->face_by_index(h) == nullptr)
            continue;
        packet_hint = h;
        break;
    }
    if (packet_hint != kNoFaceHint)
    {
        for (int i = 0; i < nverts; ++i)
        {
            if (face_hints[i] == kNoFaceHint || face_hints[i] == 0u)
                face_hints[i] = packet_hint;
        }
    }
#endif

    // Token-first path. If no token is available, optional miss-only decode fallback.
    // Select a hint that is both frequent and present in face cache.
    uint32_t face_idx = 0xFFFFFFFFu;
    uint32_t face_idx_secondary = kNoFaceHint;
    {
        uint32_t best_cached = kNoFaceHint;
        int best_cached_count = 0;
        uint32_t second_cached = kNoFaceHint;
        int second_cached_count = 0;
        uint32_t best_any = kNoFaceHint;
        int best_any_count = 0;

        for (int i = 0; i < nverts; ++i)
        {
            const uint32_t h = face_hints[i];
            if (h == kNoFaceHint || h == 0)
                continue;

            int c = 0;
            for (int j = 0; j < nverts; ++j)
                if (face_hints[j] == h)
                    ++c;

            if (c > best_any_count)
            {
                best_any_count = c;
                best_any = h;
            }

            const bool in_cache = (gte_3d_ && gte_3d_->face_by_index(h) != nullptr);
            if (in_cache && c > best_cached_count)
            {
                second_cached_count = best_cached_count;
                second_cached = best_cached;
                best_cached_count = c;
                best_cached = h;
            }
            else if (in_cache && h != best_cached && c > second_cached_count)
            {
                second_cached_count = c;
                second_cached = h;
            }
        }

        const bool had_any_hint = (best_any != kNoFaceHint);
        const bool had_cached_hint = (best_cached != kNoFaceHint);
        if (had_any_hint)
            ++token_poly_hinted_;
        else
            ++token_poly_missing_;
        if (had_cached_hint)
            ++token_poly_cached_;
        if (!had_any_hint)
            ++tok_miss_no_hint_;
        else if (!had_cached_hint)
            ++tok_miss_hint_not_cached_;

        // Prefer cache-valid token to avoid dropping to 2D when majority token is stale.
        face_idx = (best_cached != kNoFaceHint) ? best_cached : best_any;
        face_idx_secondary = second_cached;
    }

#if R3000_GPU3D_MISS_ONLY_DECODE_FALLBACK
    if (face_idx == kNoFaceHint || face_idx == 0u)
    {
        if (quad)
            face_idx = decode_face_quad(raw_x[0], raw_y[0], raw_x[1], raw_y[1], raw_x[2], raw_y[2], raw_x[3], raw_y[3]);
        else
            face_idx = decode_face_tri(raw_x[0], raw_y[0], raw_x[1], raw_y[1], raw_x[2], raw_y[2]);
    }
#endif
    if (face_idx == kNoFaceHint || face_idx == 0u)
        ++tok_miss_decode_fail_;

    // --- 3D decode diagnostic (log first 5 polygons per frame + transitions) ---
    static uint32_t diag_frame = 0xFFFFFFFFu;
    static uint32_t diag_poly_in_frame = 0;
    static uint32_t diag_3d_hits = 0;
    static uint32_t diag_2d_misses = 0;
    if (frame_count_ != diag_frame)
    {
        if (diag_frame != 0xFFFFFFFFu && (diag_frame < 5 || (diag_frame % 300) == 0))
        {
            emu::logf(emu::LogLevel::info, "GPU3D_DIAG",
                "frame=%u polygons=%u 3d_hits=%u 2d_misses=%u gte_3d=%p face_cache=%u",
                diag_frame, diag_poly_in_frame, diag_3d_hits, diag_2d_misses,
                (void*)gte_3d_,
                gte_3d_ ? gte_3d_->face_count() : 0);
        }
        diag_frame = frame_count_;
        diag_poly_in_frame = 0;
        diag_3d_hits = 0;
        diag_2d_misses = 0;
    }

    if (quad)
    {
        // Diagnostic: log raw coords + selected face token for first few polys
        if (diag_poly_in_frame < 5 && (frame_count_ < 5 || (frame_count_ % 300) == 0))
        {
            emu::logf(emu::LogLevel::info, "GPU3D_DIAG",
                "  QUAD[%u] raw=(%u,%u)(%u,%u)(%u,%u)(%u,%u) hints=(0x%X,0x%X,0x%X,0x%X) fi=0x%X",
                diag_poly_in_frame,
                raw_x[0], raw_y[0], raw_x[1], raw_y[1],
                raw_x[2], raw_y[2], raw_x[3], raw_y[3],
                face_hints[0], face_hints[1], face_hints[2], face_hints[3],
                face_idx);
        }

        push_quad(
            raw_x[0], raw_y[0], cr[0], cg[0], cb[0], tu[0], tv[0],
            raw_x[1], raw_y[1], cr[1], cg[1], cb[1], tu[1], tv[1],
            raw_x[2], raw_y[2], cr[2], cg[2], cb[2], tu[2], tv[2],
            raw_x[3], raw_y[3], cr[3], cg[3], cb[3], tu[3], tv[3],
            clut, tp, flags, semi_mode, tex_depth, PrimOrigin::origin_2d_hud, face_idx, face_idx_secondary, producer_pc);
    }
    else
    {
        if (diag_poly_in_frame < 5 && (frame_count_ < 5 || (frame_count_ % 300) == 0))
        {
            emu::logf(emu::LogLevel::info, "GPU3D_DIAG",
                "  TRI[%u] raw=(%u,%u)(%u,%u)(%u,%u) hints=(0x%X,0x%X,0x%X) fi=0x%X",
                diag_poly_in_frame,
                raw_x[0], raw_y[0], raw_x[1], raw_y[1], raw_x[2], raw_y[2],
                face_hints[0], face_hints[1], face_hints[2],
                face_idx);
        }

        push_triangle(
            raw_x[0], raw_y[0], cr[0], cg[0], cb[0], tu[0], tv[0],
            raw_x[1], raw_y[1], cr[1], cg[1], cb[1], tu[1], tv[1],
            raw_x[2], raw_y[2], cr[2], cg[2], cb[2], tu[2], tv[2],
            clut, tp, flags, semi_mode, tex_depth, PrimOrigin::origin_2d_hud, face_idx);
    }

    if (face_idx != 0xFFFFFFFFu && face_idx != 0)
        ++diag_3d_hits;
    else
        ++diag_2d_misses;
    ++diag_poly_in_frame;
}

// ---------------------------------------------------------------------------
// GP0 2-vertex line — push as a 1px-wide quad (2 triangles)
// PS1 lines are rasterized as thin rectangles perpendicular to the line direction.
// ---------------------------------------------------------------------------
void Gpu3D::gp0_line()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const bool gouraud = (cmd & 0x10) != 0;
    const bool semi    = (cmd & 0x02) != 0;

    uint8_t r0 = (uint8_t)(cmd_buf_[0] & 0xFF);
    uint8_t g0 = (uint8_t)((cmd_buf_[0] >> 8) & 0xFF);
    uint8_t b0 = (uint8_t)((cmd_buf_[0] >> 16) & 0xFF);

    uint16_t x0 = (uint16_t)(cmd_buf_[1] & 0xFFFFu);
    uint16_t y0 = (uint16_t)(cmd_buf_[1] >> 16);

    uint8_t r1 = r0, g1 = g0, b1 = b0;
    int vi = 2;
    if (gouraud)
    {
        r1 = (uint8_t)(cmd_buf_[vi] & 0xFF);
        g1 = (uint8_t)((cmd_buf_[vi] >> 8) & 0xFF);
        b1 = (uint8_t)((cmd_buf_[vi] >> 16) & 0xFF);
        vi++;
    }
    uint16_t x1 = (uint16_t)(cmd_buf_[vi] & 0xFFFFu);
    uint16_t y1 = (uint16_t)(cmd_buf_[vi] >> 16);

    uint8_t flags = semi ? 2 : 0;
    uint16_t tp = (uint16_t)(draw_env_.texpage_raw & 0xFFFF);
    uint8_t semi_mode = (uint8_t)((tp >> 5) & 3);

    // Compute 1px perpendicular offset to form a visible quad.
    // For horizontal lines: offset Y by 1. For vertical: offset X by 1.
    // For diagonal: offset along the shorter axis.
    const int16_t dx = (int16_t)x1 - (int16_t)x0;
    const int16_t dy = (int16_t)y1 - (int16_t)y0;
    int16_t px, py;
    if (dx == 0 && dy == 0)
    {
        // Zero-length line: 1×1 pixel
        px = 1; py = 0;
    }
    else if (std::abs(dx) >= std::abs(dy))
    {
        // More horizontal: offset in Y
        px = 0; py = 1;
    }
    else
    {
        // More vertical: offset in X
        px = 1; py = 0;
    }

    // Quad: (x0,y0)-(x1,y1)-(x1+px,y1+py)-(x0+px,y0+py)
    push_quad(
        x0, y0, r0, g0, b0, 0, 0,
        x1, y1, r1, g1, b1, 0, 0,
        (uint16_t)(x0 + px), (uint16_t)(y0 + py), r0, g0, b0, 0, 0,
        (uint16_t)(x1 + px), (uint16_t)(y1 + py), r1, g1, b1, 0, 0,
        0, tp, flags, semi_mode, 0,
        PrimOrigin::origin_2d_line, 0xFFFFFFFFu, kNoFaceHint);
}

// ---------------------------------------------------------------------------
// GP0 rect/sprite (60h-7Fh) — push 2 triangles as origin_2d_rect
// ---------------------------------------------------------------------------
void Gpu3D::gp0_rect()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const bool textured = (cmd & 0x04) != 0;
    const bool semi     = (cmd & 0x02) != 0;
    const bool raw      = (cmd & 0x01) != 0; // raw texture (no color modulation)
    const int size_code = (cmd >> 3) & 3;

    uint8_t r = (uint8_t)(cmd_buf_[0] & 0xFF);
    uint8_t g = (uint8_t)((cmd_buf_[0] >> 8) & 0xFF);
    uint8_t b = (uint8_t)((cmd_buf_[0] >> 16) & 0xFF);

    uint16_t x0 = (uint16_t)(cmd_buf_[1] & 0xFFFFu);
    uint16_t y0 = (uint16_t)(cmd_buf_[1] >> 16);

    uint8_t u0 = 0, v0 = 0;
    uint16_t clut = 0;
    int wi = 2;
    if (textured)
    {
        u0 = (uint8_t)(cmd_buf_[wi] & 0xFF);
        v0 = (uint8_t)((cmd_buf_[wi] >> 8) & 0xFF);
        clut = (uint16_t)((cmd_buf_[wi] >> 16) & 0xFFFF);
        wi++;
    }

    int32_t w = 0, h = 0;
    switch (size_code)
    {
        case 0: // variable
            w = (int32_t)(cmd_buf_[wi] & 0xFFFFu);
            h = (int32_t)(cmd_buf_[wi] >> 16);
            break;
        case 1: w = 1;  h = 1;  break; // 1x1
        case 2: w = 8;  h = 8;  break; // 8x8
        case 3: w = 16; h = 16; break; // 16x16
    }

    if (w <= 0 || h <= 0)
        return;

    uint16_t x1 = (uint16_t)(x0 + (uint16_t)w);
    uint16_t y1 = (uint16_t)(y0 + (uint16_t)h);

    uint16_t tp = (uint16_t)(draw_env_.texpage_raw & 0xFFFF);
    uint8_t semi_mode = (uint8_t)((tp >> 5) & 3);
    uint8_t tex_depth = (uint8_t)((tp >> 7) & 3);
    uint8_t flags = 0;
    if (textured) flags |= 1;
    if (semi)     flags |= 2;
    if (raw)      flags |= 4;

    // Sprite UV are 8-bit and wrap naturally on PS1.
    const uint8_t u1 = static_cast<uint8_t>(u0 + (uint8_t)w);
    const uint8_t v1 = static_cast<uint8_t>(v0 + (uint8_t)h);

    push_triangle(
        x0, y0, r, g, b, u0, v0,
        x1, y0, r, g, b, u1, v0,
        x0, y1, r, g, b, u0, v1,
        clut, tp, flags, semi_mode, tex_depth,
        PrimOrigin::origin_2d_rect, 0xFFFFFFFFu);
    push_triangle(
        x1, y0, r, g, b, u1, v0,
        x1, y1, r, g, b, u1, v1,
        x0, y1, r, g, b, u0, v1,
        clut, tp, flags, semi_mode, tex_depth,
        PrimOrigin::origin_2d_rect, 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// Environment commands — track draw state
// ---------------------------------------------------------------------------
void Gpu3D::gp0_env_command()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const uint32_t val = cmd_buf_[0] & 0x00FFFFFFu;

    switch (cmd)
    {
        case 0xE1:
            draw_env_.texpage_raw = val;
            break;
        case 0xE2:
            draw_env_.tex_window = val;
            break;
        case 0xE3:
            draw_env_.clip_x1 = (uint16_t)(val & 0x3FF);
            draw_env_.clip_y1 = (uint16_t)((val >> 10) & 0x1FF);
            break;
        case 0xE4:
            draw_env_.clip_x2 = (uint16_t)(val & 0x3FF);
            draw_env_.clip_y2 = (uint16_t)((val >> 10) & 0x1FF);
            break;
        case 0xE5:
        {
            int32_t ox = (int32_t)(val & 0x7FF);
            int32_t oy = (int32_t)((val >> 11) & 0x7FF);
            if (ox & 0x400) ox |= ~0x7FF;
            if (oy & 0x400) oy |= ~0x7FF;
            draw_env_.offset_x = (int16_t)ox;
            draw_env_.offset_y = (int16_t)oy;
            break;
        }
        case 0xE6:
            draw_env_.mask_bits = (uint16_t)(val & 3);
            break;
    }
}

// ---------------------------------------------------------------------------
// GP1 — only handle reset (00h). Shadow doesn't need display config.
// ---------------------------------------------------------------------------
void Gpu3D::gp1(uint32_t word)
{
    const uint8_t cmd = (uint8_t)(word >> 24);
    switch (cmd)
    {
        case 0x00: // Reset GPU
            reset();
            break;

        case 0x01: // Reset command buffer
            gp0_state_ = Gp0State::idle;
            cmd_buf_pos_ = 0;
            cmd_words_needed_ = 0;
            vram_words_remaining_ = 0;
            break;

        case 0x03: // Display enable/disable
            display_.display_enabled = ((word & 1u) == 0);
            break;

        case 0x05: // Start of display area in VRAM
            display_.display_x = (uint16_t)(word & 0x3FFu);
            display_.display_y = (uint16_t)((word >> 10) & 0x1FFu);
            break;

        case 0x06: // Horizontal display range
            display_.h_range_x1 = (uint16_t)(word & 0xFFFu);
            display_.h_range_x2 = (uint16_t)((word >> 12) & 0xFFFu);
            break;

        case 0x07: // Vertical display range
            display_.v_range_y1 = (uint16_t)(word & 0x3FFu);
            display_.v_range_y2 = (uint16_t)((word >> 10) & 0x3FFu);
            break;

        case 0x08: // Display mode
            display_.h_res = (uint8_t)(word & 3u);
            if (word & 0x40u) display_.h_res = 4; // 368 mode
            display_.v_res = (uint8_t)((word >> 2) & 1u);
            display_.is_pal = (word & 8u) != 0;
            display_.color_24bit = (word & 0x10u) != 0;
            display_.interlace = (word & 0x20u) != 0;
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Push triangle to active draw list — NO rejection, NO clipping
// ---------------------------------------------------------------------------
// Differential lattice decoder: extract face_idx from vertex coordinate differences.
//
// Tagged coords live in dead zone [2048, 32767], safely outside real PS1 screen
// coordinates ([-1024,1023] = uint16 [0,2047]∪[64512,65535]).
//
// RTPT encoding:  V0 = carrier, V1 = reference, V2 = carrier (redundant)
// Carrier.X = REF_BASE + face_idx_lo * SPACING
// Reference  = REF_BASE
// Decode: face_idx_lo = round((carrier.x - ref.x) / SPACING)
//
// Immune to uniform add/sub: (V0+C) - (V1+C) = V0 - V1 → offset cancels.

static bool in_dead_zone(uint16_t v)
{
    return v >= gte::Gte3D::DEAD_MIN && v <= gte::Gte3D::DEAD_MAX;
}

// Decode face_idx lo (from X) and hi (from Y) from a carrier-reference pair.
// Returns -1 on failure. Both axes must be in dead zone.
static int32_t decode_pair(uint16_t carrier_x, uint16_t carrier_y,
                           uint16_t ref_x, uint16_t ref_y)
{
    const int dx = (int)carrier_x - (int)ref_x;
    const int dy = (int)carrier_y - (int)ref_y;

    const int sp = gte::Gte3D::SPACING;
    const int lo = (dx >= 0) ? (dx + sp / 2) / sp : -((-dx + sp / 2) / sp);
    const int hi = (dy >= 0) ? (dy + sp / 2) / sp : -((-dy + sp / 2) / sp);

    if (lo < 0 || hi < 0 || lo > 255 || hi > 255)
        return -1;

    return (hi << 8) | lo;
}

// Decode a single axis (lo from X, or hi from Y) from carrier-reference pair.
// Returns -1 if either value is outside dead zone.
static int32_t decode_axis(uint16_t carrier, uint16_t ref)
{
    if (!in_dead_zone(carrier) || !in_dead_zone(ref))
        return -1;
    const int d = (int)carrier - (int)ref;
    const int sp = gte::Gte3D::SPACING;
    const int val = (d >= 0) ? (d + sp / 2) / sp : -((-d + sp / 2) / sp);
    if (val < 0 || val > 255) return -1;
    return val;
}

// Decode a face index directly from an absolute tagged carrier coordinate.
// This is the correct path for builders that emit carrier pairs without any
// in-packet reference vertex, such as Ridge Racer's flag GT4 builder.
static uint32_t decode_face_absolute_xy(uint16_t x, uint16_t y)
{
    if (!in_dead_zone(x) || !in_dead_zone(y))
        return 0xFFFFFFFFu;

    constexpr uint16_t RB = gte::Gte3D::REF_BASE;
    constexpr int sp = gte::Gte3D::SPACING;
    const int dx = (int)x - (int)RB;
    const int dy = (int)y - (int)RB;
    const int lo = (dx >= 0) ? (dx + sp / 2) / sp : -((-dx + sp / 2) / sp);
    const int hi = (dy >= 0) ? (dy + sp / 2) / sp : -((-dy + sp / 2) / sp);
    if (lo < 0 || hi < 0 || lo > 255 || hi > 255)
        return 0xFFFFFFFFu;
    return (uint32_t)((hi << 8) | lo);
}

// Decode face_idx from N vertices using differential scheme.
// Supports per-axis partial decode: X and Y are decoded independently.
// For each reference candidate R: gather lo from X-axis and hi from Y-axis
// of all other vertices. Use majority vote for each axis separately.
//
// This handles the PS1 fan topology where anchor vertices have one real coord
// (e.g., X=0) but the other tagged (Y=8192), allowing partial extraction.
static uint32_t decode_face_nv(const uint16_t* xs, const uint16_t* ys,
                               const bool* dz_x, const bool* dz_y, int n)
{
    // --- Carrier-only detection for quads ---
    // Ridge Racer pattern: game reads SXY2 twice from each of 2 RTPTs.
    // Result: V0==V1 (carrier1) and V2==V3 (carrier2), no reference vertex.
    // Differential passes all fail or give wrong results (delta, not absolute).
    // Skip directly to the absolute REF_BASE fallback.
    if (n == 4 &&
        xs[0] == xs[1] && ys[0] == ys[1] &&
        xs[2] == xs[3] && ys[2] == ys[3] &&
        (xs[0] != xs[2] || ys[0] != ys[2]))
    {
        goto absolute_fallback;
    }

    // Multi-pass decode: try strongest evidence first across ALL ref candidates
    // before falling back to weaker methods. This prevents a wrong ref's partial
    // decode from shadowing the correct ref's full-pair majority.

    // --- Pass 1: Full-pair UNANIMOUS (ALL valid carriers must agree) ---
    // Requires unanimity to prevent false positives when the GP0 packet
    // contains only carrier vertices (no reference). In that case, picking
    // any carrier as "reference" gives decode_pair differences of ~1 SPACING
    // between adjacent carriers, producing a bogus face_idx. Unanimity
    // rejects this because the carrier chosen as "reference" decodes to 0
    // against itself-like vertices, breaking agreement.
    for (int ref = 0; ref < n; ++ref)
    {
        if (!dz_x[ref] || !dz_y[ref]) continue;

        int32_t agreed = -1;
        bool unanimous = true;
        int valid_count = 0;

        for (int j = 0; j < n; ++j)
        {
            if (j == ref) continue;
            const int32_t fi = (dz_x[j] && dz_y[j])
                ? decode_pair(xs[j], ys[j], xs[ref], ys[ref])
                : -1;
            if (fi < 0) continue;
            ++valid_count;
            if (agreed < 0) agreed = fi;
            else if (fi != agreed) { unanimous = false; break; }
        }

        if (unanimous && valid_count >= 2 && agreed >= 0)
            return (uint32_t)agreed;
    }

    // --- Pass 2: Single full pair with all others being anchors ---
    for (int ref = 0; ref < n; ++ref)
    {
        if (!dz_x[ref] || !dz_y[ref]) continue;

        int32_t full_faces[4];
        int full_valid = 0;
        for (int j = 0; j < n; ++j)
        {
            if (j == ref) { full_faces[j] = -1; continue; }
            full_faces[j] = (dz_x[j] && dz_y[j])
                ? decode_pair(xs[j], ys[j], xs[ref], ys[ref])
                : -1;
            if (full_faces[j] >= 0) ++full_valid;
        }

        if (full_valid == 1)
        {
            int anchors = 0;
            for (int j = 0; j < n; ++j)
                if (j != ref && !(dz_x[j] && dz_y[j])) ++anchors;
            if (anchors == n - 2)
            {
                for (int j = 0; j < n; ++j)
                    if (full_faces[j] >= 0) return (uint32_t)full_faces[j];
            }
        }
    }

    // --- Pass 3: Per-axis partial decode with UNANIMOUS agreement ---
    // Same unanimity principle as Pass 1: all valid axis values must agree.
    for (int ref = 0; ref < n; ++ref)
    {
        if (!dz_x[ref] || !dz_y[ref]) continue;

        int32_t los[4], his[4];
        int lo_valid = 0, hi_valid = 0;
        for (int j = 0; j < n; ++j)
        {
            if (j == ref) { los[j] = his[j] = -1; continue; }
            los[j] = decode_axis(xs[j], xs[ref]);
            his[j] = decode_axis(ys[j], ys[ref]);
            if (los[j] >= 0) ++lo_valid;
            if (his[j] >= 0) ++hi_valid;
        }

        if (lo_valid < 2 || hi_valid < 2) continue;

        // Require ALL valid values to agree on each axis
        int32_t best_lo = -1;
        bool lo_unanimous = true;
        for (int j = 0; j < n; ++j)
        {
            if (los[j] < 0) continue;
            if (best_lo < 0) best_lo = los[j];
            else if (los[j] != best_lo) { lo_unanimous = false; break; }
        }

        int32_t best_hi = -1;
        bool hi_unanimous = true;
        for (int j = 0; j < n; ++j)
        {
            if (his[j] < 0) continue;
            if (best_hi < 0) best_hi = his[j];
            else if (his[j] != best_hi) { hi_unanimous = false; break; }
        }

        if (lo_unanimous && hi_unanimous && best_lo >= 0 && best_hi >= 0)
            return ((uint32_t)best_hi << 8) | (uint32_t)best_lo;
    }

    // --- Pass 4: Per-axis with single-value fallback (weakest) ---
    // Still requires unanimity among all valid values to avoid false positives.
    for (int ref = 0; ref < n; ++ref)
    {
        if (!dz_x[ref] || !dz_y[ref]) continue;

        int32_t los[4], his[4];
        int lo_valid = 0, hi_valid = 0;
        for (int j = 0; j < n; ++j)
        {
            if (j == ref) { los[j] = his[j] = -1; continue; }
            los[j] = decode_axis(xs[j], xs[ref]);
            his[j] = decode_axis(ys[j], ys[ref]);
            if (los[j] >= 0) ++lo_valid;
            if (his[j] >= 0) ++hi_valid;
        }

        if (lo_valid == 0 || hi_valid == 0) continue;

        // Accept if all valid values agree (even with just 1 valid per axis)
        int32_t best_lo = -1;
        bool lo_ok = true;
        for (int j = 0; j < n; ++j)
        {
            if (los[j] < 0) continue;
            if (best_lo < 0) best_lo = los[j];
            else if (los[j] != best_lo) { lo_ok = false; break; }
        }

        int32_t best_hi = -1;
        bool hi_ok = true;
        for (int j = 0; j < n; ++j)
        {
            if (his[j] < 0) continue;
            if (best_hi < 0) best_hi = his[j];
            else if (his[j] != best_hi) { hi_ok = false; break; }
        }

        if (lo_ok && hi_ok && best_lo >= 0 && best_hi >= 0)
            return ((uint32_t)best_hi << 8) | (uint32_t)best_lo;
    }

    // LEGACY TEMPORARY decode path.
    // This absolute decode keeps older tagged-coordinate experiments working
    // while the explicit per-mode GTE/GPU reconstruction is being cleaned up.
    // Do not treat this as a final reconstruction mode.
absolute_fallback:
    {
        constexpr uint16_t RB = gte::Gte3D::REF_BASE;
        constexpr int sp = gte::Gte3D::SPACING;

        // Gather all valid lo/hi from absolute decode against REF_BASE
        int32_t abs_los[4], abs_his[4];
        int abs_lo_valid = 0, abs_hi_valid = 0;
        for (int j = 0; j < n; ++j)
        {
            abs_los[j] = -1;
            abs_his[j] = -1;
            if (dz_x[j])
            {
                int d = (int)xs[j] - (int)RB;
                int val = (d >= 0) ? (d + sp / 2) / sp : -((-d + sp / 2) / sp);
                if (val >= 0 && val <= 255) { abs_los[j] = val; ++abs_lo_valid; }
            }
            if (dz_y[j])
            {
                int d = (int)ys[j] - (int)RB;
                int val = (d >= 0) ? (d + sp / 2) / sp : -((-d + sp / 2) / sp);
                if (val >= 0 && val <= 255) { abs_his[j] = val; ++abs_hi_valid; }
            }
        }

        if (abs_lo_valid > 0 && abs_hi_valid > 0)
        {
            // Separate carrier values (non-zero) from reference values (zero)
            // Reference encodes lo=0,hi=0. Carriers encode the actual face_idx.
            int32_t best_lo = -1, best_hi = -1;

            // For lo: pick the majority non-zero value (carriers), ignoring zeros (reference)
            for (int a = 0; a < n; ++a)
            {
                if (abs_los[a] <= 0) continue;
                int count = 0;
                for (int b = 0; b < n; ++b)
                    if (abs_los[b] == abs_los[a]) ++count;
                if (count >= 2) { best_lo = abs_los[a]; break; }
            }
            // If no majority, take single non-zero carrier value
            if (best_lo < 0)
            {
                for (int j = 0; j < n; ++j)
                    if (abs_los[j] > 0) { best_lo = abs_los[j]; break; }
            }
            // If all are 0 (face_idx lo actually is 0), accept 0
            if (best_lo < 0)
            {
                bool all_zero = true;
                for (int j = 0; j < n; ++j)
                    if (abs_los[j] != 0 && abs_los[j] != -1) { all_zero = false; break; }
                if (all_zero && abs_lo_valid >= 2) best_lo = 0;
            }

            // Same for hi
            for (int a = 0; a < n; ++a)
            {
                if (abs_his[a] <= 0) continue;
                int count = 0;
                for (int b = 0; b < n; ++b)
                    if (abs_his[b] == abs_his[a]) ++count;
                if (count >= 2) { best_hi = abs_his[a]; break; }
            }
            if (best_hi < 0)
            {
                for (int j = 0; j < n; ++j)
                    if (abs_his[j] > 0) { best_hi = abs_his[j]; break; }
            }
            if (best_hi < 0)
            {
                bool all_zero = true;
                for (int j = 0; j < n; ++j)
                    if (abs_his[j] != 0 && abs_his[j] != -1) { all_zero = false; break; }
                if (all_zero && abs_hi_valid >= 2) best_hi = 0;
            }

            if (best_lo >= 0 && best_hi >= 0)
                return ((uint32_t)best_hi << 8) | (uint32_t)best_lo;
        }
    }

    return 0xFFFFFFFFu;
}

// Decode face_idx from 3 triangle vertices using differential scheme.
// Original encoding: V0=carrier, V1=reference, V2=carrier.
// Tries all reference assignments + per-axis partial decode for mixed anchors.
static uint32_t decode_face_tri(uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1,
                                uint16_t x2, uint16_t y2)
{
    const uint16_t xs[3] = {x0, x1, x2};
    const uint16_t ys[3] = {y0, y1, y2};
    const bool dz_x[3] = { in_dead_zone(x0), in_dead_zone(x1), in_dead_zone(x2) };
    const bool dz_y[3] = { in_dead_zone(y0), in_dead_zone(y1), in_dead_zone(y2) };
    return decode_face_nv(xs, ys, dz_x, dz_y, 3);
}

// Decode face_idx from 4 quad vertices using differential scheme.
// Tries all 4 reference assignments + per-axis partial decode.
static uint32_t decode_face_quad(uint16_t x0, uint16_t y0,
                                 uint16_t x1, uint16_t y1,
                                 uint16_t x2, uint16_t y2,
                                 uint16_t x3, uint16_t y3)
{
    const uint16_t xs[4] = {x0, x1, x2, x3};
    const uint16_t ys[4] = {y0, y1, y2, y3};
    const bool dz_x[4] = { in_dead_zone(x0), in_dead_zone(x1), in_dead_zone(x2), in_dead_zone(x3) };
    const bool dz_y[4] = { in_dead_zone(y0), in_dead_zone(y1), in_dead_zone(y2), in_dead_zone(y3) };
    return decode_face_nv(xs, ys, dz_x, dz_y, 4);
}

// ---------------------------------------------------------------------------
// Helper: sign-extend 11-bit coord (GPU hardware behavior for 2D primitives)
// ---------------------------------------------------------------------------
static int16_t se11(uint16_t v)
{
    return (int16_t)((int32_t)(v << 21) >> 21);
}

// ---------------------------------------------------------------------------
// Helper: prepare DrawCmd vertex for 2D.
// Keep raw screen-space coordinates (se11 only), without draw offset.
// Draw offset is a VRAM page targeting detail (double-buffering), not a stable
// world-space transform, and applying it here causes visible Y flicker.
//
// For 3D tagged coords, returns raw values (UE5 uses verts_3d from DrawCmd3D).
// ---------------------------------------------------------------------------
static DrawVertex make_vertex(uint16_t raw_x, uint16_t raw_y,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t u, uint8_t v,
                              bool is_2d, int16_t ox, int16_t oy)
{
    if (is_2d)
    {
        (void)ox;
        (void)oy;
        const int16_t sx = se11(raw_x);
        const int16_t sy = se11(raw_y);
        return {sx, sy, r, g, b, u, v};
    }
    return {(int16_t)raw_x, (int16_t)raw_y, r, g, b, u, v};
}

// ---------------------------------------------------------------------------
// Helper: look up face cache and populate DrawCmd3D for a triangle
// ---------------------------------------------------------------------------
static void fill_cmd3d_from_face(DrawCmd3D& cmd3d, const gte::GteCacheFace* face,
                                 int i0, int i1, int i2)
{
    const int map[3] = {i0, i1, i2};
    for (int j = 0; j < 3; ++j)
    {
        const int k = map[j];
        cmd3d.verts_3d[j] = {face->vx[k], face->vy[k], face->vz[k]};
        cmd3d.nx[j] = face->nx[k];
        cmd3d.ny[j] = face->ny[k];
        cmd3d.nz[j] = face->nz[k];
        cmd3d.sz[j] = face->sz[k];
    }
    cmd3d.transform = face->transform;
    cmd3d.source_pc = face->source_pc;
}

static void fill_cmd3d_from_quad(DrawCmd3D& cmd3d, const gte::GteCacheQuad* qc,
                                 int i0, int i1, int i2)
{
    const int map[3] = {i0, i1, i2};
    for (int j = 0; j < 3; ++j)
    {
        const int k = map[j];
        cmd3d.verts_3d[j] = {qc->vx[k], qc->vy[k], qc->vz[k]};
        cmd3d.nx[j] = qc->nx[k];
        cmd3d.ny[j] = qc->ny[k];
        cmd3d.nz[j] = qc->nz[k];
        cmd3d.sz[j] = qc->sz[k];
    }
    cmd3d.transform = qc->transform;
    cmd3d.source_pc = qc->source_pc;
}

static bool same_transform(const gte::GteTransform& a, const gte::GteTransform& b)
{
    return std::memcmp(a.rt, b.rt, sizeof(a.rt)) == 0 &&
           std::memcmp(a.tr, b.tr, sizeof(a.tr)) == 0;
}

struct VertexLookupHit
{
    const gte::GteCacheVertex* v{nullptr};
    uint32_t idx{0xFFFFFFFFu};
};

static VertexLookupHit lookup_vertex_by_sxy(
    gte::Gte3D* gte_3d, uint16_t x, uint16_t y)
{
    VertexLookupHit out{};
    if (!gte_3d)
        return out;
    const uint32_t sxy = ((uint32_t)y << 16) | (uint32_t)x;
    const uint32_t vi = gte_3d->lookup_by_sxy(sxy);
    if (vi == 0xFFFFFFFFu || vi == 0u)
        return out;
    out.idx = vi;
    out.v = gte_3d->vertex_by_index(vi);
    return out;
}

static bool indices_are_local(const uint32_t* idx, int n, uint32_t max_span)
{
    uint32_t mn = 0xFFFFFFFFu;
    uint32_t mx = 0;
    for (int i = 0; i < n; ++i)
    {
        if (idx[i] == 0xFFFFFFFFu || idx[i] == 0u)
            return false;
        if (idx[i] < mn) mn = idx[i];
        if (idx[i] > mx) mx = idx[i];
    }
    return (mx - mn) <= max_span;
}

// ---------------------------------------------------------------------------
// push_triangle — a real triangle (3 vertices)
// ---------------------------------------------------------------------------
void Gpu3D::push_triangle(
    uint16_t x0, uint16_t y0, uint8_t r0, uint8_t g0, uint8_t b0, uint8_t u0, uint8_t v0,
    uint16_t x1, uint16_t y1, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t u1, uint8_t v1,
    uint16_t x2, uint16_t y2, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t u2, uint8_t v2,
    uint16_t clut, uint16_t texpage, uint8_t flags, uint8_t semi_mode, uint8_t tex_depth,
    PrimOrigin origin, uint32_t face_idx)
{
    // The external VBlank thread swaps/clears draw lists asynchronously.
    // Keep the command and its parallel 3D metadata in the same frame.
    std::lock_guard<std::mutex> lock(draw_list_mutex_);

    DrawCmd3D cmd3d{};
    cmd3d.is_quad = false;
    cmd3d.quad_half = 0;

    // Face cache lookup: resolve face_idx → 3D data or fallback to 2D
    // face_idx=0 is never valid (face_index_ starts at 1; index 0 is zeroed)
    bool is_3d = false;
    if (face_idx != 0xFFFFFFFFu && face_idx != 0 && gte_3d_)
    {
        const gte::GteCacheFace* face = gte_3d_->face_by_index(face_idx);
        if (face)
        {
            is_3d = true;
            origin = PrimOrigin::origin_3d;
            fill_cmd3d_from_face(cmd3d, face, 0, 1, 2);
        }
        else
        {
            // Diagnostic: face decoded but not in cache
            static uint32_t tri_miss_count = 0;
            if (tri_miss_count < 20)
                emu::logf(emu::LogLevel::info, "GPU3D_DIAG",
                    "TRI face_idx=%u NOT IN CACHE (face_count=%u) frame=%u",
                    face_idx, gte_3d_->face_count(), frame_count_);
            ++tri_miss_count;
            face_idx = 0xFFFFFFFFu; // decoded but not in cache
        }
    }
    else
        face_idx = 0xFFFFFFFFu; // no gte_3d_ or no tag decoded

    // Fallback: direct per-vertex SXY lookup (no face token required).
    if (!is_3d && gte_3d_
#if R3000_GPU3D_VTX_LOOKUP_FALLBACK
        )
#else
        && false)
#endif
    {
        const VertexLookupHit h0 = lookup_vertex_by_sxy(gte_3d_, x0, y0);
        const VertexLookupHit h1 = lookup_vertex_by_sxy(gte_3d_, x1, y1);
        const VertexLookupHit h2 = lookup_vertex_by_sxy(gte_3d_, x2, y2);
        const gte::GteCacheVertex* v0 = h0.v;
        const gte::GteCacheVertex* v1 = h1.v;
        const gte::GteCacheVertex* v2 = h2.v;
        const uint32_t idxs[3] = {h0.idx, h1.idx, h2.idx};
        if (v0 && v1 && v2 &&
            indices_are_local(idxs, 3, 24u) &&
            same_transform(v0->transform, v1->transform) &&
            same_transform(v0->transform, v2->transform))
        {
            is_3d = true;
            origin = PrimOrigin::origin_3d;
            cmd3d.verts_3d[0] = {v0->vx, v0->vy, v0->vz};
            cmd3d.verts_3d[1] = {v1->vx, v1->vy, v1->vz};
            cmd3d.verts_3d[2] = {v2->vx, v2->vy, v2->vz};
            cmd3d.nx[0] = v0->nx; cmd3d.ny[0] = v0->ny; cmd3d.nz[0] = v0->nz; cmd3d.sz[0] = v0->sz;
            cmd3d.nx[1] = v1->nx; cmd3d.ny[1] = v1->ny; cmd3d.nz[1] = v1->nz; cmd3d.sz[1] = v1->sz;
            cmd3d.nx[2] = v2->nx; cmd3d.ny[2] = v2->ny; cmd3d.nz[2] = v2->nz; cmd3d.sz[2] = v2->sz;
            cmd3d.transform = v0->transform;
            cmd3d.source_pc = v0->source_pc;
            ++vtx_lookup_hits_;
        }
        else
        {
            ++vtx_lookup_misses_;
        }
    }

    cmd3d.face_idx = face_idx;
    cmd3d.origin = origin;
    cmd3d.ot_z = current_ot_z_;

    const bool is_2d = !is_3d;
    const int16_t ox = is_2d ? draw_env_.offset_x : 0;
    const int16_t oy = is_2d ? draw_env_.offset_y : 0;

    DrawCmd cmd{};
    cmd.v[0] = make_vertex(x0, y0, r0, g0, b0, u0, v0, is_2d, ox, oy);
    cmd.v[1] = make_vertex(x1, y1, r1, g1, b1, u1, v1, is_2d, ox, oy);
    cmd.v[2] = make_vertex(x2, y2, r2, g2, b2, u2, v2, is_2d, ox, oy);
    cmd.clut = clut;
    cmd.texpage = texpage;
    cmd.flags = flags;
    cmd.semi_mode = semi_mode;
    cmd.tex_depth = tex_depth;

    draw_lists_[draw_active_].push(cmd);
    draw_lists_[draw_active_].push_3d(cmd3d);
}

// ---------------------------------------------------------------------------
// push_quad — a real quad (4 vertices), split into 2 triangles internally
// GP0 quad: V0,V1,V2,V3 → tri1(V0,V1,V2) + tri2(V1,V3,V2)
// ---------------------------------------------------------------------------
void Gpu3D::push_quad(
    uint16_t x0, uint16_t y0, uint8_t r0, uint8_t g0, uint8_t b0, uint8_t u0, uint8_t v0,
    uint16_t x1, uint16_t y1, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t u1, uint8_t v1,
    uint16_t x2, uint16_t y2, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t u2, uint8_t v2,
    uint16_t x3, uint16_t y3, uint8_t r3, uint8_t g3, uint8_t b3, uint8_t u3, uint8_t v3,
    uint16_t clut, uint16_t texpage, uint8_t flags, uint8_t semi_mode, uint8_t tex_depth,
    PrimOrigin origin, uint32_t face_idx, uint32_t face_idx_secondary_hint, uint32_t producer_pc)
{
    // A quad is emitted as two triangles; a VBlank swap between halves would
    // produce exactly the kind of partial vector frame that flickers in MCP/UE.
    std::lock_guard<std::mutex> lock(draw_list_mutex_);

    // Face/quad cache lookup — done once for both triangles
    bool is_3d = false;
    const gte::GteCacheFace* face = nullptr;
    const gte::GteCacheQuad* qc = nullptr;

    if (face_idx != 0xFFFFFFFFu && face_idx != 0 && gte_3d_)
    {
        face = gte_3d_->face_by_index(face_idx);
        if (face)
        {
            is_3d = true;
            origin = PrimOrigin::origin_3d;
            qc = gte_3d_->quad_by_index(face_idx);
        }
        else
            face_idx = 0xFFFFFFFFu; // decoded but not in cache
    }
    else
        face_idx = 0xFFFFFFFFu; // no gte_3d_ or no tag decoded

    // TEMP DEBUG ONLY: direct per-vertex SXY lookup (no face token required).
    const gte::GteCacheVertex* vtx[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t vtx_idx[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    bool vtx_lookup_3d = false;
    if (!is_3d && gte_3d_
#if R3000_GPU3D_VTX_LOOKUP_FALLBACK
        )
#else
        && false)
#endif
    {
        const VertexLookupHit h0 = lookup_vertex_by_sxy(gte_3d_, x0, y0);
        const VertexLookupHit h1 = lookup_vertex_by_sxy(gte_3d_, x1, y1);
        const VertexLookupHit h2 = lookup_vertex_by_sxy(gte_3d_, x2, y2);
        const VertexLookupHit h3 = lookup_vertex_by_sxy(gte_3d_, x3, y3);
        vtx[0] = h0.v; vtx[1] = h1.v; vtx[2] = h2.v; vtx[3] = h3.v;
        vtx_idx[0] = h0.idx; vtx_idx[1] = h1.idx; vtx_idx[2] = h2.idx; vtx_idx[3] = h3.idx;
        if (vtx[0] && vtx[1] && vtx[2] && vtx[3] &&
            indices_are_local(vtx_idx, 4, 32u) &&
            same_transform(vtx[0]->transform, vtx[1]->transform) &&
            same_transform(vtx[0]->transform, vtx[2]->transform) &&
            same_transform(vtx[0]->transform, vtx[3]->transform))
        {
            vtx_lookup_3d = true;
            is_3d = true;
            origin = PrimOrigin::origin_3d;
            ++vtx_lookup_hits_;
        }
        else
        {
            ++vtx_lookup_misses_;
        }
    }

    enum class QuadBuildMode : uint8_t
    {
        none = 0,
        cache,
        paired_edge_rtpt_gt4,
        edge_strip,
        face_pair,
        debug_vtx_lookup,
        face_partial,
    };

    // Shared 2D coord processing params
    const bool is_2d = !is_3d;
    const int16_t ox = is_2d ? draw_env_.offset_x : 0;
    const int16_t oy = is_2d ? draw_env_.offset_y : 0;

    // Build vertices once, reuse for both triangles
    const DrawVertex dv0 = make_vertex(x0, y0, r0, g0, b0, u0, v0, is_2d, ox, oy);
    const DrawVertex dv1 = make_vertex(x1, y1, r1, g1, b1, u1, v1, is_2d, ox, oy);
    const DrawVertex dv2 = make_vertex(x2, y2, r2, g2, b2, u2, v2, is_2d, ox, oy);
    const DrawVertex dv3 = make_vertex(x3, y3, r3, g3, b3, u3, v3, is_2d, ox, oy);

    // Detect edge-strip pattern: if face has V1==V2, only 2 unique vertices
    // (V0 and V1). Ridge Racer terrain uses RTPT with duplicate V1=V2 to
    // process edges. The quad is assembled from 2 faces (face_A + face_B),
    // each contributing 2 unique vertices → 4 total for the real quad.
    const bool edge_strip = face && (face->vx[1] == face->vx[2] &&
                                     face->vy[1] == face->vy[2] &&
                                     face->vz[1] == face->vz[2]);

    auto is_edge_face = [](const gte::GteCacheFace* f) -> bool
    {
        return f && (f->vx[1] == f->vx[2] &&
                     f->vy[1] == f->vy[2] &&
                     f->vz[1] == f->vz[2]);
    };

    const RuntimeModeRule* active_mode_rule = find_runtime_mode_rule(producer_pc);
    if (!active_mode_rule)
    {
        auto rule_matches_gte_source = [&](const RuntimeModeRule& rule) -> bool
        {
            if (rule.gte_pc_ranges.empty())
                return false;
            if (qc && qc->valid && pc_in_ranges(qc->source_pc, rule.gte_pc_ranges))
                return true;
            if (face && pc_in_ranges(face->source_pc, rule.gte_pc_ranges))
                return true;
            return false;
        };
        for (const auto& rule : runtime_mode_rules_)
        {
            if (!rule_matches_gte_source(rule))
                continue;
            if (!active_mode_rule || rule.priority > active_mode_rule->priority)
                active_mode_rule = &rule;
        }
    }

    const bool rule_packet_edge_pairs =
        active_mode_rule && active_mode_rule->link_rule == RuntimeLinkRule::packet_edge_pairs;
    const bool rule_paired_edge =
        active_mode_rule && active_mode_rule->mode == RuntimeModeKind::paired_edge_rtpt_gt4;
    const bool rule_subdivided_ft4 =
        active_mode_rule && active_mode_rule->mode == RuntimeModeKind::subdivided_ft4_intpl_rtpt;

    // Explicit paired-edge mode:
    // some builders (e.g. Ridge Racer DrawFlag) produce a GT4 packet from two
    // RTPT-projected edge faces. The packet carries two visible screen-space
    // edge segments; recover each edge directly from the packet, then rebuild
    // the logical quad from the two projected edges.
    const gte::GteCacheFace* packet_edge_face_a = nullptr;
    const gte::GteCacheFace* packet_edge_face_b = nullptr;
    int packet_edge_draw_idx[4] = {0, 1, 2, 3};
    const char* packet_edge_layout = "none";
    if (is_3d && gte_3d_ && rule_packet_edge_pairs && (rule_paired_edge || rule_subdivided_ft4))
    {
        if (rule_paired_edge)
        {
            const uint32_t fi_a = gte_3d_->lookup_edge_face_by_segment(
                static_cast<int16_t>(x0), static_cast<int16_t>(y0),
                static_cast<int16_t>(x1), static_cast<int16_t>(y1));
            const uint32_t fi_b = gte_3d_->lookup_edge_face_by_segment(
                static_cast<int16_t>(x2), static_cast<int16_t>(y2),
                static_cast<int16_t>(x3), static_cast<int16_t>(y3));
            if (fi_a != 0xFFFFFFFFu && fi_b != 0xFFFFFFFFu &&
                fi_a != 0u && fi_b != 0u && fi_a != fi_b)
            {
                const gte::GteCacheFace* fa = gte_3d_->face_by_index(fi_a);
                const gte::GteCacheFace* fb = gte_3d_->face_by_index(fi_b);
                if (is_edge_face(fa) && is_edge_face(fb))
                {
                    packet_edge_face_a = fa;
                    packet_edge_face_b = fb;
                    packet_edge_layout = "01_23";
                }
            }
        }

        if (!packet_edge_face_a || !packet_edge_face_b)
        {
            struct PacketEdgeCandidate
            {
                int idx[4];
                const char* label;
            };
            static const PacketEdgeCandidate kCandidates[] = {
                {{0, 1, 2, 3}, "01_23"},
                {{0, 2, 1, 3}, "02_13"},
                {{0, 3, 1, 2}, "03_12"},
            };

            int best_score = -1;
            for (const auto& cand : kCandidates)
            {
                const int a0 = cand.idx[0], a1 = cand.idx[1];
                const int b0 = cand.idx[2], b1 = cand.idx[3];
                const uint16_t px[4] = {x0, x1, x2, x3};
                const uint16_t py[4] = {y0, y1, y2, y3};
                const uint32_t fi_a = gte_3d_->lookup_edge_face_by_segment(
                    static_cast<int16_t>(px[a0]), static_cast<int16_t>(py[a0]),
                    static_cast<int16_t>(px[a1]), static_cast<int16_t>(py[a1]));
                const uint32_t fi_b = gte_3d_->lookup_edge_face_by_segment(
                    static_cast<int16_t>(px[b0]), static_cast<int16_t>(py[b0]),
                    static_cast<int16_t>(px[b1]), static_cast<int16_t>(py[b1]));
                if (fi_a == 0xFFFFFFFFu || fi_b == 0xFFFFFFFFu ||
                    fi_a == 0u || fi_b == 0u || fi_a == fi_b)
                    continue;

                const gte::GteCacheFace* fa = gte_3d_->face_by_index(fi_a);
                const gte::GteCacheFace* fb = gte_3d_->face_by_index(fi_b);
                if (!is_edge_face(fa) || !is_edge_face(fb))
                    continue;

                int score = 0;
                if (fi_a == face_idx || fi_b == face_idx)
                    score += 8;
                if (face_idx_secondary_hint != kNoFaceHint &&
                    (fi_a == face_idx_secondary_hint || fi_b == face_idx_secondary_hint))
                    score += 4;
                if (fa->source_pc != 0 && fb->source_pc != 0 && fa->source_pc == fb->source_pc)
                    score += 2;

                if (score > best_score)
                {
                    best_score = score;
                    packet_edge_face_a = fa;
                    packet_edge_face_b = fb;
                    packet_edge_draw_idx[0] = a0;
                    packet_edge_draw_idx[1] = a1;
                    packet_edge_draw_idx[2] = b0;
                    packet_edge_draw_idx[3] = b1;
                    packet_edge_layout = cand.label;
                }
            }
        }
    }

    // For edge strips, try to find face_B from V3's tagged coords
    const gte::GteCacheFace* face_b = nullptr;
    const char* face_b_source = "none";
    if (is_3d && edge_strip)
    {
        // Preferred in token-flow mode: explicit hint from V3 coord word.
        if (face_idx_secondary_hint != kNoFaceHint && face_idx_secondary_hint != 0 && face_idx_secondary_hint != face_idx)
        {
            face_b = gte_3d_->face_by_index(face_idx_secondary_hint);
            if (face_b)
            {
                face_b_source = "secondary_hint";
                ++quad_v3_hint_used_;
            }
        }

        if (face_b)
        {
            // Already resolved via token hint; skip legacy tagged-coordinate decode.
        }
        else
        {
        const auto matches_segment = [](const gte::GteCacheFace* f, uint16_t ax, uint16_t ay, uint16_t bx, uint16_t by) -> bool
        {
            const uint16_t f0x = static_cast<uint16_t>(f->sx[0]);
            const uint16_t f0y = static_cast<uint16_t>(f->sy[0]);
            const uint16_t f1x = static_cast<uint16_t>(f->sx[1]);
            const uint16_t f1y = static_cast<uint16_t>(f->sy[1]);
            return ((f0x == ax && f0y == ay && f1x == bx && f1y == by) ||
                    (f0x == bx && f0y == by && f1x == ax && f1y == ay));
        };

        // Explicit paired-edge mode: some GT4 builders only preserve one face token
        // even though the packet clearly contains two edge segments from two RTPT calls.
        // When the dominant face matches one packet segment, resolve the other segment
        // through the edge-face segment table built by Gte3D.
        if (gte_3d_)
        {
            uint32_t other_face_idx = 0xFFFFFFFFu;
            if (matches_segment(face, x0, y0, x1, y1))
                other_face_idx = gte_3d_->lookup_edge_face_by_segment(static_cast<int16_t>(x2), static_cast<int16_t>(y2),
                                                                      static_cast<int16_t>(x3), static_cast<int16_t>(y3));
            else if (matches_segment(face, x2, y2, x3, y3))
                other_face_idx = gte_3d_->lookup_edge_face_by_segment(static_cast<int16_t>(x0), static_cast<int16_t>(y0),
                                                                      static_cast<int16_t>(x1), static_cast<int16_t>(y1));

            if (other_face_idx != 0xFFFFFFFFu && other_face_idx != 0u && other_face_idx != face_idx)
            {
                face_b = gte_3d_->face_by_index(other_face_idx);
                if (face_b)
                    face_b_source = "segment_link";
            }
        }

        if (!face_b)
        {
        const uint16_t v3x = x3;
        const uint16_t v3y = y3;

        if (in_dead_zone(v3x) && in_dead_zone(v3y) &&
            v3x != gte::Gte3D::REF_BASE && v3y != gte::Gte3D::REF_BASE)
        {
            // V3 is a carrier: decode face_idx_B
            const int lo = (int)std::round((double)(v3x - gte::Gte3D::REF_BASE) / gte::Gte3D::SPACING);
            const int hi = (int)std::round((double)(v3y - gte::Gte3D::REF_BASE) / gte::Gte3D::SPACING);
            if (lo >= 0 && lo <= 255 && hi >= 0 && hi <= 255)
            {
                const uint32_t fi_b = (uint32_t)((hi << 8) | lo);
                face_b = gte_3d_->face_by_index(fi_b);
                if (face_b)
                    face_b_source = "v3_carrier";
            }
        }
        else if (in_dead_zone(v3x) && in_dead_zone(v3y))
        {
            // V3 is a reference: try face_idx ± 1
            if (face_idx + 1 < 0xFFFFu)
            {
                face_b = gte_3d_->face_by_index(face_idx + 1);
                if (face_b)
                    face_b_source = "v3_ref_plus1";
            }
            if (!face_b && face_idx > 1)
            {
                face_b = gte_3d_->face_by_index(face_idx - 1);
                if (face_b)
                    face_b_source = "v3_ref_minus1";
            }
        }
        }
        }
    }

    struct EdgeQuadCorner
    {
        int32_t vx, vy, vz;
        int16_t nx, ny, nz;
        uint16_t sz;
        int16_t sx, sy;
    };

    auto make_corner = [](const gte::GteCacheFace* f, int idx) -> EdgeQuadCorner
    {
        return EdgeQuadCorner{
            f->vx[idx], f->vy[idx], f->vz[idx],
            f->nx[idx], f->ny[idx], f->nz[idx],
            f->sz[idx],
            f->sx[idx], f->sy[idx]
        };
    };

    auto dist2_2d = [](const EdgeQuadCorner& a, const EdgeQuadCorner& b) -> double
    {
        const double dx = static_cast<double>(a.sx) - static_cast<double>(b.sx);
        const double dy = static_cast<double>(a.sy) - static_cast<double>(b.sy);
        return dx * dx + dy * dy;
    };

    auto tri_normal = [](const EdgeQuadCorner& a, const EdgeQuadCorner& b,
                         const EdgeQuadCorner& c, double (&out)[3]) -> double
    {
        const double abx = static_cast<double>(b.vx) - static_cast<double>(a.vx);
        const double aby = static_cast<double>(b.vy) - static_cast<double>(a.vy);
        const double abz = static_cast<double>(b.vz) - static_cast<double>(a.vz);
        const double acx = static_cast<double>(c.vx) - static_cast<double>(a.vx);
        const double acy = static_cast<double>(c.vy) - static_cast<double>(a.vy);
        const double acz = static_cast<double>(c.vz) - static_cast<double>(a.vz);
        out[0] = aby * acz - abz * acy;
        out[1] = abz * acx - abx * acz;
        out[2] = abx * acy - aby * acx;
        return std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    };

    auto diag_score = [&](const EdgeQuadCorner (&corners)[4], bool use_alt_diag) -> double
    {
        const int t0[3] = {0, 1, use_alt_diag ? 3 : 2};
        const int t1[3] = {use_alt_diag ? 0 : 1, 3, 2};
        double n0[3] = {};
        double n1[3] = {};
        const double a0 = tri_normal(corners[t0[0]], corners[t0[1]], corners[t0[2]], n0);
        const double a1 = tri_normal(corners[t1[0]], corners[t1[1]], corners[t1[2]], n1);
        if (a0 < 1e-6 || a1 < 1e-6)
            return -1e30;
        const double dot = (n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2]) / (a0 * a1);
        const double align = std::fabs(dot);
        const double balance = (a0 < a1) ? (a0 / a1) : (a1 / a0);
        return align * 4.0 + balance * 2.0;
    };

    EdgeQuadCorner edge_quad[4] = {};
    int edge_draw_idx[4] = {0, 1, 2, 3};
    int edge_tri0[3] = {0, 1, 2};
    int edge_tri1[3] = {1, 3, 2};
    const char* edge_pairing = "fixed";
    const char* edge_diag = "12";

    // Edge strip with face_B found: reconstruct real quad from 4 unique vertices.
    // The old fixed ordering A0,A1,B0,B1 can twist valid grids. Choose:
    // 1) the best endpoint pairing in screen space
    // 2) the least twisted diagonal in 3D
    const bool paired_edge_packet_ok =
        rule_packet_edge_pairs && (rule_paired_edge || rule_subdivided_ft4) &&
        packet_edge_face_a && packet_edge_face_b;
    const gte::GteTransform* edge_transform = nullptr;
    if (paired_edge_packet_ok)
        edge_transform = &packet_edge_face_a->transform;
    else if (face)
        edge_transform = &face->transform;
    const bool edge_strip_ok = edge_strip && face_b;
    // Also detect if face_B is edge-strip too (V1==V2)
    const bool face_b_edge = face_b && (face_b->vx[1] == face_b->vx[2] &&
                                         face_b->vy[1] == face_b->vy[2] &&
                                         face_b->vz[1] == face_b->vz[2]);

    if (paired_edge_packet_ok)
    {
        auto orient_edge_to_segment = [&](const gte::GteCacheFace* f,
                                          int16_t sx0, int16_t sy0,
                                          int16_t sx1, int16_t sy1,
                                          EdgeQuadCorner& out0,
                                          EdgeQuadCorner& out1)
        {
            const bool forward =
                f->sx[0] == sx0 && f->sy[0] == sy0 &&
                f->sx[1] == sx1 && f->sy[1] == sy1;
            const bool reverse =
                f->sx[0] == sx1 && f->sy[0] == sy1 &&
                f->sx[1] == sx0 && f->sy[1] == sy0;
            if (reverse && !forward)
            {
                out0 = make_corner(f, 1);
                out1 = make_corner(f, 0);
            }
            else
            {
                out0 = make_corner(f, 0);
                out1 = make_corner(f, 1);
            }
        };

        EdgeQuadCorner a0{};
        EdgeQuadCorner a1{};
        EdgeQuadCorner b0{};
        EdgeQuadCorner b1{};

        if (rule_paired_edge)
        {
            edge_pairing = "packet_fixed";
            edge_diag = "03";
            edge_draw_idx[0] = 0;
            edge_draw_idx[1] = 1;
            edge_draw_idx[2] = 2;
            edge_draw_idx[3] = 3;
            edge_tri0[0] = 0; edge_tri0[1] = 1; edge_tri0[2] = 3;
            edge_tri1[0] = 0; edge_tri1[1] = 3; edge_tri1[2] = 2;

            orient_edge_to_segment(packet_edge_face_a,
                                   static_cast<int16_t>(x0), static_cast<int16_t>(y0),
                                   static_cast<int16_t>(x1), static_cast<int16_t>(y1),
                                   a0, a1);
            orient_edge_to_segment(packet_edge_face_b,
                                   static_cast<int16_t>(x2), static_cast<int16_t>(y2),
                                   static_cast<int16_t>(x3), static_cast<int16_t>(y3),
                                   b0, b1);

            edge_quad[0] = a0;
            edge_quad[1] = a1;
            edge_quad[2] = b0;
            edge_quad[3] = b1;
        }
        else
        {
            edge_draw_idx[0] = packet_edge_draw_idx[0];
            edge_draw_idx[1] = packet_edge_draw_idx[1];
            edge_draw_idx[2] = packet_edge_draw_idx[2];
            edge_draw_idx[3] = packet_edge_draw_idx[3];

            a0 = make_corner(packet_edge_face_a, 0);
            a1 = make_corner(packet_edge_face_a, 1);
            b0 = make_corner(packet_edge_face_b, 0);
            b1 = make_corner(packet_edge_face_b, 1);

            EdgeQuadCorner pairing_direct[4] = {a0, a1, b0, b1};
            EdgeQuadCorner pairing_swapped[4] = {a0, a1, b1, b0};

            const double direct_span = dist2_2d(a0, b0) + dist2_2d(a1, b1);
            const double swapped_span = dist2_2d(a0, b1) + dist2_2d(a1, b0);

            const EdgeQuadCorner* chosen_pairing = pairing_direct;
            if (swapped_span + 1e-6 < direct_span)
            {
                chosen_pairing = pairing_swapped;
                edge_pairing = "packet_swapped";
                edge_draw_idx[2] = 3;
                edge_draw_idx[3] = 2;
            }
            else
            {
                edge_pairing = "packet_direct";
            }

            for (int i = 0; i < 4; ++i)
                edge_quad[i] = chosen_pairing[i];

            const double diag12_score = diag_score(edge_quad, false);
            const double diag03_score = diag_score(edge_quad, true);
            if (diag03_score > diag12_score)
            {
                edge_tri0[0] = 0; edge_tri0[1] = 1; edge_tri0[2] = 3;
                edge_tri1[0] = 0; edge_tri1[1] = 3; edge_tri1[2] = 2;
                edge_diag = "03";
            }
            else
            {
                edge_diag = "12";
            }
        }

        static uint32_t paired_edge_diag_logs = 0;
        if (paired_edge_diag_logs < 96)
        {
            emu::logf(
                emu::LogLevel::debug, "GPU3D_PAIRED_EDGE",
                "face=0x%X layout=%s pair=%s diag=%s "
                "A=((%d,%d,%d),(%d,%d,%d),(%d,%d,%d)) "
                "B=((%d,%d,%d),(%d,%d,%d),(%d,%d,%d)) "
                "packet=((%d,%d),(%d,%d),(%d,%d),(%d,%d))",
                face_idx, packet_edge_layout, edge_pairing, edge_diag,
                packet_edge_face_a->vx[0], packet_edge_face_a->vy[0], packet_edge_face_a->vz[0],
                packet_edge_face_a->vx[1], packet_edge_face_a->vy[1], packet_edge_face_a->vz[1],
                packet_edge_face_a->vx[2], packet_edge_face_a->vy[2], packet_edge_face_a->vz[2],
                packet_edge_face_b->vx[0], packet_edge_face_b->vy[0], packet_edge_face_b->vz[0],
                packet_edge_face_b->vx[1], packet_edge_face_b->vy[1], packet_edge_face_b->vz[1],
                packet_edge_face_b->vx[2], packet_edge_face_b->vy[2], packet_edge_face_b->vz[2],
                (int)x0, (int)y0, (int)x1, (int)y1, (int)x2, (int)y2, (int)x3, (int)y3);
            ++paired_edge_diag_logs;
        }
    }
    else if (edge_strip_ok)
    {
        const EdgeQuadCorner a0 = make_corner(face, 0);
        const EdgeQuadCorner a1 = make_corner(face, 1);
        const EdgeQuadCorner b0 = make_corner(face_b, 0);
        const EdgeQuadCorner b1 = make_corner(face_b, face_b_edge ? 1 : 0);

        EdgeQuadCorner pairing_direct[4] = {a0, a1, b0, b1};
        EdgeQuadCorner pairing_swapped[4] = {a0, a1, b1, b0};

        const double direct_span = dist2_2d(a0, b0) + dist2_2d(a1, b1);
        const double swapped_span = dist2_2d(a0, b1) + dist2_2d(a1, b0);

        const EdgeQuadCorner* chosen_pairing = pairing_direct;
        if (face_b_edge && swapped_span + 1e-6 < direct_span)
        {
            chosen_pairing = pairing_swapped;
            edge_pairing = "swapped";
            edge_draw_idx[2] = 3;
            edge_draw_idx[3] = 2;
        }
        else
        {
            edge_pairing = "direct";
        }

        for (int i = 0; i < 4; ++i)
            edge_quad[i] = chosen_pairing[i];

        const double diag12_score = diag_score(edge_quad, false);
        const double diag03_score = diag_score(edge_quad, true);
        if (diag03_score > diag12_score)
        {
            edge_tri0[0] = 0; edge_tri0[1] = 1; edge_tri0[2] = 3;
            edge_tri1[0] = 0; edge_tri1[1] = 3; edge_tri1[2] = 2;
            edge_diag = "03";
        }
        else
        {
            edge_diag = "12";
        }

        static uint32_t edge_strip_diag_logs = 0;
        if (edge_strip_diag_logs < 96)
        {
            emu::logf(
                emu::LogLevel::debug, "GPU3D_EDGE",
                "face=0x%X face_b_src=%s secondary_hint=0x%X pair=%s diag=%s "
                "A=((%d,%d,%d),(%d,%d,%d),(%d,%d,%d)) "
                "B=((%d,%d,%d),(%d,%d,%d),(%d,%d,%d)) "
                "tri0=((%d,%d,%d),(%d,%d,%d),(%d,%d,%d)) "
                "tri1=((%d,%d,%d),(%d,%d,%d),(%d,%d,%d)) "
                "trA=(%d,%d,%d) trB=(%d,%d,%d) face_b_edge=%d",
                face_idx, face_b_source, face_idx_secondary_hint, edge_pairing, edge_diag,
                face->vx[0], face->vy[0], face->vz[0],
                face->vx[1], face->vy[1], face->vz[1],
                face->vx[2], face->vy[2], face->vz[2],
                face_b->vx[0], face_b->vy[0], face_b->vz[0],
                face_b->vx[1], face_b->vy[1], face_b->vz[1],
                face_b->vx[2], face_b->vy[2], face_b->vz[2],
                edge_quad[edge_tri0[0]].vx, edge_quad[edge_tri0[0]].vy, edge_quad[edge_tri0[0]].vz,
                edge_quad[edge_tri0[1]].vx, edge_quad[edge_tri0[1]].vy, edge_quad[edge_tri0[1]].vz,
                edge_quad[edge_tri0[2]].vx, edge_quad[edge_tri0[2]].vy, edge_quad[edge_tri0[2]].vz,
                edge_quad[edge_tri1[0]].vx, edge_quad[edge_tri1[0]].vy, edge_quad[edge_tri1[0]].vz,
                edge_quad[edge_tri1[1]].vx, edge_quad[edge_tri1[1]].vy, edge_quad[edge_tri1[1]].vz,
                edge_quad[edge_tri1[2]].vx, edge_quad[edge_tri1[2]].vy, edge_quad[edge_tri1[2]].vz,
                face->transform.tr[0], face->transform.tr[1], face->transform.tr[2],
                face_b->transform.tr[0], face_b->transform.tr[1], face_b->transform.tr[2],
                face_b_edge ? 1 : 0);
            ++edge_strip_diag_logs;
        }
    }

    // Explicit non-edge quad mode: use a second tagged face as V3 source.
    const gte::GteCacheFace* face_pair_b = nullptr;
    int face_pair_v3_vert_idx = 0;
    if (is_3d && !vtx_lookup_3d && !qc && !edge_strip_ok)
    {
        // Preferred in token-flow mode: explicit hint from V3 coord word.
        if (face_idx_secondary_hint != kNoFaceHint && face_idx_secondary_hint != 0 && face_idx_secondary_hint != face_idx)
        {
            face_pair_b = gte_3d_->face_by_index(face_idx_secondary_hint);
            face_pair_v3_vert_idx = 0;
            if (face_pair_b)
                ++quad_v3_hint_used_;
        }

        const uint16_t v3x = x3;
        const uint16_t v3y = y3;
        if (!face_pair_b && in_dead_zone(v3x) && in_dead_zone(v3y) &&
            v3x != gte::Gte3D::REF_BASE && v3y != gte::Gte3D::REF_BASE)
        {
            const int lo = (int)std::round((double)(v3x - gte::Gte3D::REF_BASE) / gte::Gte3D::SPACING);
            const int hi = (int)std::round((double)(v3y - gte::Gte3D::REF_BASE) / gte::Gte3D::SPACING);
            if (lo >= 0 && lo <= 255 && hi >= 0 && hi <= 255)
            {
                const uint32_t fi_b = (uint32_t)((hi << 8) | lo);
                face_pair_b = gte_3d_->face_by_index(fi_b);
                face_pair_v3_vert_idx = 0;
            }
        }
        else if (!face_pair_b && in_dead_zone(v3x) && in_dead_zone(v3y))
        {
            face_pair_v3_vert_idx = 1;
            if (face_idx + 1 < 0xFFFFu)
                face_pair_b = gte_3d_->face_by_index(face_idx + 1);
            if (!face_pair_b && face_idx > 1)
                face_pair_b = gte_3d_->face_by_index(face_idx - 1);
        }
    }

    QuadBuildMode quad_mode = QuadBuildMode::none;
    if (is_3d)
    {
        if (vtx_lookup_3d)
            quad_mode = QuadBuildMode::debug_vtx_lookup;
        else if (paired_edge_packet_ok && rule_paired_edge)
            // Packet-derived paired-edge mode when no explicit quad cache exists.
            quad_mode = QuadBuildMode::paired_edge_rtpt_gt4;
        else if (qc)
            quad_mode = QuadBuildMode::cache;
        else if (paired_edge_packet_ok)
            quad_mode = QuadBuildMode::paired_edge_rtpt_gt4;
        else if (edge_strip_ok)
            quad_mode = QuadBuildMode::edge_strip;
        else if (face_pair_b)
            quad_mode = QuadBuildMode::face_pair;
        else
            quad_mode = QuadBuildMode::face_partial;
    }

    switch (quad_mode)
    {
    case QuadBuildMode::cache: ++quad_mode_cache_hits_; break;
    case QuadBuildMode::paired_edge_rtpt_gt4:
        ++quad_mode_paired_edge_hits_;
        if (producer_pc != 0)
            ++quad_paired_edge_pc_hist_[producer_pc];
        break;
    case QuadBuildMode::edge_strip:
        ++quad_mode_edge_strip_hits_;
        if (producer_pc != 0)
            ++quad_edge_pc_hist_[producer_pc];
        break;
    case QuadBuildMode::face_pair: ++quad_mode_face_pair_hits_; break;
    case QuadBuildMode::debug_vtx_lookup: ++quad_mode_debug_vtx_hits_; break;
    case QuadBuildMode::face_partial:
        ++quad_mode_face_partial_hits_;
        if (producer_pc != 0)
            ++quad_partial_pc_hist_[producer_pc];
        break;
    default: break;
    }

    static uint32_t quad_partial_diag_logs = 0;
    if (quad_mode == QuadBuildMode::face_partial &&
        producer_pc == 0x800264B0u &&
        face &&
        quad_partial_diag_logs < 64)
    {
        emu::logf(
            emu::LogLevel::debug, "GPU3D_PARTIAL",
            "face=0x%X secondary_hint=0x%X edge_strip=%d "
            "A=((%d,%d,%d),(%d,%d,%d),(%d,%d,%d)) "
            "screen=((%d,%d),(%d,%d),(%d,%d),(%d,%d))",
            face_idx, face_idx_secondary_hint, edge_strip ? 1 : 0,
            face->vx[0], face->vy[0], face->vz[0],
            face->vx[1], face->vy[1], face->vz[1],
            face->vx[2], face->vy[2], face->vz[2],
            (int)x0, (int)y0, (int)x1, (int)y1, (int)x2, (int)y2, (int)x3, (int)y3);
        ++quad_partial_diag_logs;
    }

    // --- Triangle 1: V0, V1, V2 (quad_half=0) ---
    {
        DrawCmd3D cmd3d{};
        cmd3d.is_quad = true;
        cmd3d.quad_half = 0;
        cmd3d.face_idx = face_idx;
        cmd3d.origin = origin;
        cmd3d.ot_z = current_ot_z_;
        cmd3d.source_pc = producer_pc;

        if (is_3d)
        {
            if (quad_mode == QuadBuildMode::debug_vtx_lookup)
            {
                cmd3d.verts_3d[0] = {vtx[0]->vx, vtx[0]->vy, vtx[0]->vz};
                cmd3d.verts_3d[1] = {vtx[1]->vx, vtx[1]->vy, vtx[1]->vz};
                cmd3d.verts_3d[2] = {vtx[2]->vx, vtx[2]->vy, vtx[2]->vz};
                cmd3d.nx[0] = vtx[0]->nx; cmd3d.ny[0] = vtx[0]->ny; cmd3d.nz[0] = vtx[0]->nz; cmd3d.sz[0] = vtx[0]->sz;
                cmd3d.nx[1] = vtx[1]->nx; cmd3d.ny[1] = vtx[1]->ny; cmd3d.nz[1] = vtx[1]->nz; cmd3d.sz[1] = vtx[1]->sz;
                cmd3d.nx[2] = vtx[2]->nx; cmd3d.ny[2] = vtx[2]->ny; cmd3d.nz[2] = vtx[2]->nz; cmd3d.sz[2] = vtx[2]->sz;
                cmd3d.transform = vtx[0]->transform;
                cmd3d.source_pc = vtx[0]->source_pc;
            }
            else if (quad_mode == QuadBuildMode::cache)
            {
                fill_cmd3d_from_quad(cmd3d, qc, 0, 1, 2);
            }
            else if (quad_mode == QuadBuildMode::paired_edge_rtpt_gt4 ||
                     quad_mode == QuadBuildMode::edge_strip)
            {
                for (int k = 0; k < 3; ++k)
                {
                    const EdgeQuadCorner& c = edge_quad[edge_tri0[k]];
                    cmd3d.verts_3d[k] = {c.vx, c.vy, c.vz};
                    cmd3d.nx[k] = c.nx; cmd3d.ny[k] = c.ny; cmd3d.nz[k] = c.nz;
                    cmd3d.sz[k] = c.sz;
                }
                if (edge_transform)
                    cmd3d.transform = *edge_transform;
                if (packet_edge_face_a)
                    cmd3d.source_pc = packet_edge_face_a->source_pc;
                else if (face)
                    cmd3d.source_pc = face->source_pc;
            }
            else
                fill_cmd3d_from_face(cmd3d, face, 0, 1, 2);
        }

        DrawCmd cmd{};
        if (quad_mode == QuadBuildMode::paired_edge_rtpt_gt4 ||
            quad_mode == QuadBuildMode::edge_strip)
        {
            const DrawVertex edge_dv[4] = {dv0, dv1, dv2, dv3};
            cmd.v[0] = edge_dv[edge_draw_idx[edge_tri0[0]]];
            cmd.v[1] = edge_dv[edge_draw_idx[edge_tri0[1]]];
            cmd.v[2] = edge_dv[edge_draw_idx[edge_tri0[2]]];
        }
        else
        {
            cmd.v[0] = dv0; cmd.v[1] = dv1; cmd.v[2] = dv2;
        }
        cmd.clut = clut; cmd.texpage = texpage;
        cmd.flags = flags; cmd.semi_mode = semi_mode; cmd.tex_depth = tex_depth;

        draw_lists_[draw_active_].push(cmd);
        draw_lists_[draw_active_].push_3d(cmd3d);
    }

    // --- Triangle 2: V1, V3, V2 (quad_half=1) ---
    {
        DrawCmd3D cmd3d{};
        cmd3d.is_quad = true;
        cmd3d.quad_half = 1;
        cmd3d.face_idx = face_idx;
        cmd3d.origin = origin;
        cmd3d.ot_z = current_ot_z_;
        cmd3d.source_pc = producer_pc;

        if (is_3d)
        {
            if (quad_mode == QuadBuildMode::debug_vtx_lookup)
            {
                cmd3d.verts_3d[0] = {vtx[1]->vx, vtx[1]->vy, vtx[1]->vz};
                cmd3d.verts_3d[1] = {vtx[3]->vx, vtx[3]->vy, vtx[3]->vz};
                cmd3d.verts_3d[2] = {vtx[2]->vx, vtx[2]->vy, vtx[2]->vz};
                cmd3d.nx[0] = vtx[1]->nx; cmd3d.ny[0] = vtx[1]->ny; cmd3d.nz[0] = vtx[1]->nz; cmd3d.sz[0] = vtx[1]->sz;
                cmd3d.nx[1] = vtx[3]->nx; cmd3d.ny[1] = vtx[3]->ny; cmd3d.nz[1] = vtx[3]->nz; cmd3d.sz[1] = vtx[3]->sz;
                cmd3d.nx[2] = vtx[2]->nx; cmd3d.ny[2] = vtx[2]->ny; cmd3d.nz[2] = vtx[2]->nz; cmd3d.sz[2] = vtx[2]->sz;
                cmd3d.transform = vtx[0]->transform;
                cmd3d.source_pc = vtx[0]->source_pc;
            }
            else if (quad_mode == QuadBuildMode::cache)
            {
                fill_cmd3d_from_quad(cmd3d, qc, 1, 3, 2);
            }
            else if (quad_mode == QuadBuildMode::paired_edge_rtpt_gt4 ||
                     quad_mode == QuadBuildMode::edge_strip)
            {
                for (int k = 0; k < 3; ++k)
                {
                    const EdgeQuadCorner& c = edge_quad[edge_tri1[k]];
                    cmd3d.verts_3d[k] = {c.vx, c.vy, c.vz};
                    cmd3d.nx[k] = c.nx; cmd3d.ny[k] = c.ny; cmd3d.nz[k] = c.nz;
                    cmd3d.sz[k] = c.sz;
                }
                if (edge_transform)
                    cmd3d.transform = *edge_transform;
                if (packet_edge_face_a)
                    cmd3d.source_pc = packet_edge_face_a->source_pc;
                else if (face)
                    cmd3d.source_pc = face->source_pc;
            }
            else if (quad_mode == QuadBuildMode::face_pair)
            {
                cmd3d.verts_3d[0] = {face->vx[1], face->vy[1], face->vz[1]};
                cmd3d.nx[0] = face->nx[1]; cmd3d.ny[0] = face->ny[1]; cmd3d.nz[0] = face->nz[1];
                cmd3d.verts_3d[1] = {face_pair_b->vx[face_pair_v3_vert_idx], face_pair_b->vy[face_pair_v3_vert_idx], face_pair_b->vz[face_pair_v3_vert_idx]};
                cmd3d.nx[1] = face_pair_b->nx[face_pair_v3_vert_idx]; cmd3d.ny[1] = face_pair_b->ny[face_pair_v3_vert_idx]; cmd3d.nz[1] = face_pair_b->nz[face_pair_v3_vert_idx];
                cmd3d.verts_3d[2] = {face->vx[2], face->vy[2], face->vz[2]};
                cmd3d.nx[2] = face->nx[2]; cmd3d.ny[2] = face->ny[2]; cmd3d.nz[2] = face->nz[2];
                cmd3d.transform = face->transform;
                cmd3d.source_pc = face->source_pc;
            }
            else
            {
                // Partial-face mode.
                // This is intentionally explicit in the stats because it means the
                // producer did not provide a complete quad mode for a GP0 GT4 packet.
                fill_cmd3d_from_face(cmd3d, face, 1, 1, 2);
            }
        }

        DrawCmd cmd{};
        if (quad_mode == QuadBuildMode::paired_edge_rtpt_gt4 ||
            quad_mode == QuadBuildMode::edge_strip)
        {
            const DrawVertex edge_dv[4] = {dv0, dv1, dv2, dv3};
            cmd.v[0] = edge_dv[edge_draw_idx[edge_tri1[0]]];
            cmd.v[1] = edge_dv[edge_draw_idx[edge_tri1[1]]];
            cmd.v[2] = edge_dv[edge_draw_idx[edge_tri1[2]]];
        }
        else
        {
            cmd.v[0] = dv1; cmd.v[1] = dv3; cmd.v[2] = dv2;
        }
        cmd.clut = clut; cmd.texpage = texpage;
        cmd.flags = flags; cmd.semi_mode = semi_mode; cmd.tex_depth = tex_depth;

        draw_lists_[draw_active_].push(cmd);
        draw_lists_[draw_active_].push_3d(cmd3d);
    }
}

// ---------------------------------------------------------------------------
// VBlank: swap draw lists
// ---------------------------------------------------------------------------
void Gpu3D::on_vblank()
{
    std::lock_guard<std::mutex> lock(draw_list_mutex_);

    draw_lists_[draw_active_].draw_env = draw_env_;
    draw_lists_[draw_active_].display = display_;
    draw_lists_[draw_active_].frame_id = frame_count_;

    // Save debug state BEFORE reset (read by CLI diagnostic hook AFTER VBlank)
    dbg_gp0_words_ = gp0_words_accum_;
    dbg_gp0_cmds_ = gp0_cmds_accum_;
    dbg_vram_skips_ = gp0_vram_skips_accum_;
    dbg_state_ = gp0_state_;
    dbg_vram_remaining_ = vram_words_remaining_;
    dbg_last_tris_ = static_cast<uint32_t>(draw_lists_[draw_active_].cmds_3d.size());

    const auto& dl = draw_lists_[draw_active_];
    uint32_t n3d = 0, n2d = 0;
    for (size_t i = 0; i < dl.cmds_3d.size(); ++i)
    {
        if (dl.cmds_3d[i].origin == PrimOrigin::origin_3d) ++n3d;
        else ++n2d;
    }
    dbg_last_3d_ = n3d;
    dbg_last_2d_ = n2d;
    dbg_last_miss_no_hint_ = tok_miss_no_hint_;
    dbg_last_miss_hint_stale_ = tok_miss_hint_not_cached_;
    dbg_last_miss_decode_fail_ = tok_miss_decode_fail_;

    // Diagnostic: log GP0 activity per frame (first 10 + every 60).
    // 60 keeps logs readable while still capturing transient issues (e.g. flag/menu).
    if (frame_count_ < 10 || (frame_count_ % 60) == 0)
    {
        const uint32_t cpu_pc = bus_ ? bus_->cpu_pc() : 0;
        const uint32_t vblank = bus_ ? bus_->vblank_count() : 0;
        emu::logf(emu::LogLevel::debug, "GPU3D_VBLANK",
            "frame=%u vblank=%u pc=0x%08X words=%u cmds=%u vram_skips=%u tris=%zu 3d=%u 2d=%u state=%d quad_cache=%u quad_paired_edge=%u quad_edge=%u quad_facepair=%u quad_debug_vtx=%u quad_partial=%u tok_hint=%u tok_miss=%u tok_cached=%u v3_hint=%u vtx_hit=%u vtx_miss=%u miss_no_hint=%u miss_hint_stale=%u miss_decode_fail=%u",
            frame_count_, vblank, cpu_pc, gp0_words_accum_, gp0_cmds_accum_, gp0_vram_skips_accum_,
            dl.cmds_3d.size(), n3d, n2d, (int)gp0_state_,
            quad_mode_cache_hits_, quad_mode_paired_edge_hits_, quad_mode_edge_strip_hits_, quad_mode_face_pair_hits_,
            quad_mode_debug_vtx_hits_, quad_mode_face_partial_hits_,
            token_poly_hinted_, token_poly_missing_, token_poly_cached_, quad_v3_hint_used_,
            vtx_lookup_hits_, vtx_lookup_misses_,
            tok_miss_no_hint_, tok_miss_hint_not_cached_, tok_miss_decode_fail_);

        auto log_top_hist = [](const char* tag, uint32_t frame, const std::unordered_map<uint32_t, uint32_t>& hist)
        {
            if (hist.empty())
                return;
            std::vector<std::pair<uint32_t, uint32_t>> items(hist.begin(), hist.end());
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
            const size_t topn = std::min<size_t>(items.size(), 4);
            for (size_t i = 0; i < topn; ++i)
            {
                emu::logf(emu::LogLevel::debug, tag,
                    "frame=%u top[%zu] pc=0x%08X count=%u",
                    frame, i, items[i].first, items[i].second);
            }
        };
        log_top_hist("GPU3D_PAIRED_EDGE_PC", frame_count_, quad_paired_edge_pc_hist_);
        log_top_hist("GPU3D_EDGE_PC", frame_count_, quad_edge_pc_hist_);
        log_top_hist("GPU3D_PARTIAL_PC", frame_count_, quad_partial_pc_hist_);
    }

    // Reset quad mode stats
    quad_mode_cache_hits_ = 0;
    quad_mode_paired_edge_hits_ = 0;
    quad_mode_edge_strip_hits_ = 0;
    quad_mode_face_pair_hits_ = 0;
    quad_mode_debug_vtx_hits_ = 0;
    quad_mode_face_partial_hits_ = 0;
    quad_paired_edge_pc_hist_.clear();
    quad_edge_pc_hist_.clear();
    quad_partial_pc_hist_.clear();
    token_poly_hinted_ = 0;
    token_poly_missing_ = 0;
    token_poly_cached_ = 0;
    quad_v3_hint_used_ = 0;
    vtx_lookup_hits_ = 0;
    vtx_lookup_misses_ = 0;
    tok_miss_no_hint_ = 0;
    tok_miss_hint_not_cached_ = 0;
    tok_miss_decode_fail_ = 0;

    draw_active_ = 1 - draw_active_;
    draw_lists_[draw_active_].clear();
    ++frame_count_;

    // Reset per-frame counters
    gp0_words_accum_ = 0;
    gp0_cmds_accum_ = 0;
    gp0_vram_skips_accum_ = 0;
    current_ot_z_ = 0;
}

// ---------------------------------------------------------------------------
// Thread-safe copy of the ready (previous frame) draw list
// ---------------------------------------------------------------------------
void Gpu3D::copy_ready_draw_list(FrameDrawList& out) const
{
    std::lock_guard<std::mutex> lock(draw_list_mutex_);
    out = draw_lists_[1 - draw_active_];
}

} // namespace gpu

