#include "gpu_3d.h"
#include "../gte/gte_3d.h"
#include "../log/emu_log.h"

#include <algorithm>
#include <cstring>

namespace gpu
{

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
        return words - 1;
    }

    // Lines (40h-5Fh)
    if (cmd >= 0x40 && cmd <= 0x5F)
    {
        const bool poly = (cmd & 0x08) != 0;
        if (poly) return -1; // polyline: variable length
        const bool gouraud = (cmd & 0x10) != 0;
        return gouraud ? 3 : 1;
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
    emu::logf(emu::LogLevel::warn, "GPU3D", "Gpu3D shadow GPU v1 (full parser)");
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
    frame_count_ = 0;
    draw_active_ = 0;
    draw_lists_[0].clear();
    draw_lists_[1].clear();
}

// ---------------------------------------------------------------------------
// GP0 entry point
// ---------------------------------------------------------------------------
void Gpu3D::gp0(uint32_t word)
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
                gp0_start_command(word);
            }
        }
        return;
    }

    // Collecting parameters
    if (gp0_state_ == Gp0State::collecting_params)
    {
        if (cmd_buf_pos_ < 16)
            cmd_buf_[cmd_buf_pos_++] = word;

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
    gp0_start_command(word);
}

// ---------------------------------------------------------------------------
// GP0 start command
// ---------------------------------------------------------------------------
void Gpu3D::gp0_start_command(uint32_t cmd_word)
{
    const uint8_t cmd = (uint8_t)(cmd_word >> 24);
    const int params = gp0_param_count(cmd);

    cmd_buf_[0] = cmd_word;
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
    const int nverts = quad ? 4 : 3;

    uint16_t raw_x[4], raw_y[4];
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

    if (textured)
        draw_env_.texpage_raw = (draw_env_.texpage_raw & ~0x7FFu) | (texpage_attr & 0x7FFu);

    // Decode face_idx from differential encoding BEFORE splitting into triangles.
    // For quads: use all 4 vertices (3 carriers + 1 reference) with majority vote.
    // For tris: use 3 vertices (2 carriers + 1 reference) with consistency check.
    uint32_t face_idx = 0xFFFFFFFFu;
    if (quad)
        face_idx = decode_face_quad(raw_x[0], raw_y[0], raw_x[1], raw_y[1],
                                    raw_x[2], raw_y[2], raw_x[3], raw_y[3]);
    else
        face_idx = decode_face_tri(raw_x[0], raw_y[0], raw_x[1], raw_y[1],
                                   raw_x[2], raw_y[2]);

    push_triangle(
        raw_x[0], raw_y[0], cr[0], cg[0], cb[0], tu[0], tv[0],
        raw_x[1], raw_y[1], cr[1], cg[1], cb[1], tu[1], tv[1],
        raw_x[2], raw_y[2], cr[2], cg[2], cb[2], tu[2], tv[2],
        clut, tp, flags, semi_mode, tex_depth, PrimOrigin::origin_2d_hud,
        face_idx, quad, 0);

    if (quad)
    {
        push_triangle(
            raw_x[1], raw_y[1], cr[1], cg[1], cb[1], tu[1], tv[1],
            raw_x[3], raw_y[3], cr[3], cg[3], cb[3], tu[3], tv[3],
            raw_x[2], raw_y[2], cr[2], cg[2], cb[2], tu[2], tv[2],
            clut, tp, flags, semi_mode, tex_depth, PrimOrigin::origin_2d_hud,
            face_idx, quad, 1);
    }
}

// ---------------------------------------------------------------------------
// GP0 2-vertex line — push 2 thin triangles as origin_2d_line
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

    // Degenerate triangle (line is zero-width, but keeps the draw list in sync)
    push_triangle(
        x0, y0, r0, g0, b0, 0, 0,
        x1, y1, r1, g1, b1, 0, 0,
        x0, y0, r0, g0, b0, 0, 0,
        0, tp, flags, semi_mode, 0,
        PrimOrigin::origin_2d_line,
        0xFFFFFFFFu, false);
}

// ---------------------------------------------------------------------------
// GP0 rect/sprite (60h-7Fh) — push 2 triangles as origin_2d_rect
// ---------------------------------------------------------------------------
void Gpu3D::gp0_rect()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const bool textured = (cmd & 0x04) != 0;
    const bool semi     = (cmd & 0x02) != 0;
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

    uint16_t w = 0, h = 0;
    switch (size_code)
    {
        case 0: // variable
            w = (uint16_t)(cmd_buf_[wi] & 0xFFFFu);
            h = (uint16_t)(cmd_buf_[wi] >> 16);
            break;
        case 1: w = 1;  h = 1;  break; // 1x1
        case 2: w = 8;  h = 8;  break; // 8x8
        case 3: w = 16; h = 16; break; // 16x16
    }

    uint16_t x1 = (uint16_t)(x0 + w);
    uint16_t y1 = (uint16_t)(y0 + h);

    uint16_t tp = (uint16_t)(draw_env_.texpage_raw & 0xFFFF);
    uint8_t semi_mode = (uint8_t)((tp >> 5) & 3);
    uint8_t tex_depth = (uint8_t)((tp >> 7) & 3);
    uint8_t flags = 0;
    if (textured) flags |= 1;
    if (semi)     flags |= 2;

    push_triangle(
        x0, y0, r, g, b, u0, v0,
        x1, y0, r, g, b, (uint8_t)(u0 + w), v0,
        x0, y1, r, g, b, u0, (uint8_t)(v0 + h),
        clut, tp, flags, semi_mode, tex_depth,
        PrimOrigin::origin_2d_rect,
        0xFFFFFFFFu, false);
    push_triangle(
        x1, y0, r, g, b, (uint8_t)(u0 + w), v0,
        x1, y1, r, g, b, (uint8_t)(u0 + w), (uint8_t)(v0 + h),
        x0, y1, r, g, b, u0, (uint8_t)(v0 + h),
        clut, tp, flags, semi_mode, tex_depth,
        PrimOrigin::origin_2d_rect,
        0xFFFFFFFFu, false);
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
    if (cmd == 0x00)
        reset();
    else if (cmd == 0x01)
    {
        // GP1(01h) = Reset command buffer: abort any in-progress GP0 command
        gp0_state_ = Gp0State::idle;
        cmd_buf_pos_ = 0;
        cmd_words_needed_ = 0;
        vram_words_remaining_ = 0;
    }
    else if (cmd == 0x08)
    {
        // GP1(08h) Display Mode: bits 0-1 = h_res (0=256,1=320,2=512,3=640), bit 6 = h_res2 (368)
        const uint8_t hr = (uint8_t)(word & 3u);
        h_res_ = (word & 0x40u) ? 4 : hr; // bit6 → 368px mode
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
    for (int ref = 0; ref < n; ++ref)
    {
        // Reference must have BOTH axes in dead zone
        if (!dz_x[ref] || !dz_y[ref]) continue;

        // --- Full-pair decode first (fast path) ---
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

        // 2+ full pairs agree → done
        for (int a = 0; a < n; ++a)
            for (int b = a + 1; b < n; ++b)
                if (full_faces[a] >= 0 && full_faces[a] == full_faces[b])
                    return (uint32_t)full_faces[a];

        // Single full pair with all others being anchors (not in dz) → accept
        if (full_valid == 1)
        {
            // Count how many non-ref vertices are NOT fully in dead zone (anchors)
            int anchors = 0;
            for (int j = 0; j < n; ++j)
                if (j != ref && !(dz_x[j] && dz_y[j])) ++anchors;
            if (anchors == n - 2) // all others are anchors
            {
                for (int j = 0; j < n; ++j)
                    if (full_faces[j] >= 0) return (uint32_t)full_faces[j];
            }
        }

        // --- Per-axis partial decode (handles mixed anchor vertices) ---
        // Gather all valid lo values (from X-axis) and hi values (from Y-axis)
        int32_t los[4], his[4];
        int lo_valid = 0, hi_valid = 0;
        for (int j = 0; j < n; ++j)
        {
            if (j == ref) { los[j] = his[j] = -1; continue; }
            los[j] = decode_axis(xs[j], xs[ref]);  // lo from X
            his[j] = decode_axis(ys[j], ys[ref]);  // hi from Y
            if (los[j] >= 0) ++lo_valid;
            if (his[j] >= 0) ++hi_valid;
        }

        if (lo_valid == 0 || hi_valid == 0) continue;

        // Pick lo: majority vote among valid lo values
        int32_t best_lo = -1;
        for (int a = 0; a < n; ++a)
        {
            if (los[a] < 0) continue;
            int count = 0;
            for (int b = 0; b < n; ++b)
                if (los[b] == los[a]) ++count;
            if (count >= 2) { best_lo = los[a]; break; }
        }
        if (best_lo < 0 && lo_valid >= 1)
        {
            // No majority: take first valid
            for (int j = 0; j < n; ++j)
                if (los[j] >= 0) { best_lo = los[j]; break; }
        }

        // Pick hi: majority vote among valid hi values
        int32_t best_hi = -1;
        for (int a = 0; a < n; ++a)
        {
            if (his[a] < 0) continue;
            int count = 0;
            for (int b = 0; b < n; ++b)
                if (his[b] == his[a]) ++count;
            if (count >= 2) { best_hi = his[a]; break; }
        }
        if (best_hi < 0 && hi_valid >= 1)
        {
            for (int j = 0; j < n; ++j)
                if (his[j] >= 0) { best_hi = his[j]; break; }
        }

        if (best_lo >= 0 && best_hi >= 0)
            return ((uint32_t)best_hi << 8) | (uint32_t)best_lo;
    }

    // --- Absolute REF_BASE fallback ---
    // When no reference vertex is fully in dead zone, or when per-axis differential
    // fails (only 1 carrier axis valid), decode each axis against the known REF_BASE.
    // This is less robust to add/sub but handles games that overwrite one coordinate
    // of the reference vertex (e.g., triangle fan center replaces X with 0).
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

void Gpu3D::push_triangle(
    uint16_t raw_x0, uint16_t raw_y0, uint8_t r0, uint8_t g0, uint8_t b0, uint8_t u0, uint8_t v0,
    uint16_t raw_x1, uint16_t raw_y1, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t u1, uint8_t v1,
    uint16_t raw_x2, uint16_t raw_y2, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t u2, uint8_t v2,
    uint16_t clut, uint16_t texpage, uint8_t flags, uint8_t semi_mode, uint8_t tex_depth,
    PrimOrigin origin,
    uint32_t face_idx, bool is_quad, uint8_t quad_half)
{
    // Shadow GPU: no VRAM, no offset subtraction.
    // Tags decoded from dead-zone coordinates → face cache → inline 3D data.

    DrawCmd3D cmd3d{};
    cmd3d.is_quad = is_quad;
    cmd3d.quad_half = quad_half;

    // Validate face_idx against face cache and copy 3D data inline
    bool is_3d = false;
    if (face_idx != 0xFFFFFFFFu && gte_3d_)
    {
        const gte::GteCacheFace* face = gte_3d_->face_by_index(face_idx);
        if (face)
        {
            is_3d = true;

            // For quad second half (V1,V3,V2): remap to correct face vertices.
            // GP0 quad vertex order: V0,V1,V2,V3 → tri1(V0,V1,V2) + tri2(V1,V3,V2)
            // Face cache stores the RTPT order: [0],[1],[2] = V0,V1,V2
            // Quad cache (if available) adds [3] = V3 (unique vertex from 2nd RTPT)
            if (is_quad && quad_half == 1)
            {
                const gte::GteCacheQuad* qc = gte_3d_->quad_by_index(face_idx);
                if (qc)
                {
                    // tri2 = (V1, V3, V2)
                    cmd3d.verts_3d[0] = {qc->vx[1], qc->vy[1], qc->vz[1]};
                    cmd3d.nx[0] = qc->nx[1]; cmd3d.ny[0] = qc->ny[1]; cmd3d.nz[0] = qc->nz[1];
                    cmd3d.verts_3d[1] = {qc->vx[3], qc->vy[3], qc->vz[3]};
                    cmd3d.nx[1] = qc->nx[3]; cmd3d.ny[1] = qc->ny[3]; cmd3d.nz[1] = qc->nz[3];
                    cmd3d.verts_3d[2] = {qc->vx[2], qc->vy[2], qc->vz[2]};
                    cmd3d.nx[2] = qc->nx[2]; cmd3d.ny[2] = qc->ny[2]; cmd3d.nz[2] = qc->nz[2];
                    cmd3d.transform = qc->transform;
                    cmd3d.sz[0] = qc->sz[1]; cmd3d.sz[1] = qc->sz[3]; cmd3d.sz[2] = qc->sz[2];
                }
                else
                {
                    // No quad cache — fallback: V3 ≈ V1
                    cmd3d.verts_3d[0] = {face->vx[1], face->vy[1], face->vz[1]};
                    cmd3d.nx[0] = face->nx[1]; cmd3d.ny[0] = face->ny[1]; cmd3d.nz[0] = face->nz[1];
                    cmd3d.verts_3d[1] = {face->vx[1], face->vy[1], face->vz[1]};
                    cmd3d.nx[1] = face->nx[1]; cmd3d.ny[1] = face->ny[1]; cmd3d.nz[1] = face->nz[1];
                    cmd3d.verts_3d[2] = {face->vx[2], face->vy[2], face->vz[2]};
                    cmd3d.nx[2] = face->nx[2]; cmd3d.ny[2] = face->ny[2]; cmd3d.nz[2] = face->nz[2];
                    cmd3d.transform = face->transform;
                    cmd3d.sz[0] = face->sz[1]; cmd3d.sz[1] = face->sz[1]; cmd3d.sz[2] = face->sz[2];
                }
            }
            else
            {
                // tri1 = (V0, V1, V2) — direct mapping from face cache
                for (int i = 0; i < 3; ++i)
                {
                    cmd3d.verts_3d[i] = {face->vx[i], face->vy[i], face->vz[i]};
                    cmd3d.nx[i] = face->nx[i];
                    cmd3d.ny[i] = face->ny[i];
                    cmd3d.nz[i] = face->nz[i];
                    cmd3d.sz[i] = face->sz[i];
                }
                cmd3d.transform = face->transform;
            }
        }
        else
        {
            face_idx = 0xFFFFFFFFu; // not in cache → reject
        }
    }
    else if (face_idx != 0xFFFFFFFFu)
    {
        face_idx = 0xFFFFFFFFu; // no shadow GTE bound
    }
    cmd3d.face_idx = face_idx;

    if (is_3d)
        origin = PrimOrigin::origin_3d;
    cmd3d.origin = origin;

    // For 2D primitives: apply sign_extend_11 (like the real GPU hardware).
    // GP0 vertex coords are 16-bit but only bits 0-10 are significant.
    // Games write GTE SXY outputs where upper bits may be garbage.
    // For 3D tagged coords: do NOT se11 — tags use the full 16-bit range.
    auto se11 = [](uint16_t v) -> int16_t {
        return (int16_t)((int32_t)(v << 21) >> 21);
    };

    if (!is_3d)
    {
        raw_x0 = (uint16_t)se11(raw_x0);
        raw_y0 = (uint16_t)se11(raw_y0);
        raw_x1 = (uint16_t)se11(raw_x1);
        raw_y1 = (uint16_t)se11(raw_y1);
        raw_x2 = (uint16_t)se11(raw_x2);
        raw_y2 = (uint16_t)se11(raw_y2);

        // HD normalization: /2 for 512/640
        static constexpr uint16_t res_table[] = {256, 320, 512, 640, 368};
        const uint16_t disp_w = (h_res_ < 5) ? res_table[h_res_] : 320;
        if (disp_w > 320)
        {
            raw_x0 = (uint16_t)((int16_t)raw_x0 >> 1);
            raw_y0 = (uint16_t)((int16_t)raw_y0 >> 1);
            raw_x1 = (uint16_t)((int16_t)raw_x1 >> 1);
            raw_y1 = (uint16_t)((int16_t)raw_y1 >> 1);
            raw_x2 = (uint16_t)((int16_t)raw_x2 >> 1);
            raw_y2 = (uint16_t)((int16_t)raw_y2 >> 1);
        }
    }

    DrawCmd cmd{};
    cmd.v[0] = {(int16_t)raw_x0, (int16_t)raw_y0, r0, g0, b0, u0, v0};
    cmd.v[1] = {(int16_t)raw_x1, (int16_t)raw_y1, r1, g1, b1, u1, v1};
    cmd.v[2] = {(int16_t)raw_x2, (int16_t)raw_y2, r2, g2, b2, u2, v2};
    cmd.clut = clut;
    cmd.texpage = texpage;
    cmd.flags = flags;
    cmd.semi_mode = semi_mode;
    cmd.tex_depth = tex_depth;

    draw_lists_[draw_active_].push(cmd);
    draw_lists_[draw_active_].push_3d(cmd3d);
}

// ---------------------------------------------------------------------------
// VBlank: swap draw lists
// ---------------------------------------------------------------------------
void Gpu3D::on_vblank()
{
    std::lock_guard<std::mutex> lock(draw_list_mutex_);

    draw_lists_[draw_active_].draw_env = draw_env_;
    draw_lists_[draw_active_].display.h_res = h_res_;
    draw_lists_[draw_active_].frame_id = frame_count_;

    // Save debug state BEFORE reset (read by CLI diagnostic hook AFTER VBlank)
    dbg_gp0_words_ = gp0_words_accum_;
    dbg_gp0_cmds_ = gp0_cmds_accum_;
    dbg_vram_skips_ = gp0_vram_skips_accum_;
    dbg_state_ = gp0_state_;
    dbg_vram_remaining_ = vram_words_remaining_;

    draw_active_ = 1 - draw_active_;
    draw_lists_[draw_active_].clear();
    ++frame_count_;

    // Reset per-frame counters
    gp0_words_accum_ = 0;
    gp0_cmds_accum_ = 0;
    gp0_vram_skips_accum_ = 0;
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
