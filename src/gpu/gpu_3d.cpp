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
    display_ = {};
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
    {
        face_idx = decode_face_quad(raw_x[0], raw_y[0], raw_x[1], raw_y[1],
            raw_x[2], raw_y[2], raw_x[3], raw_y[3]);
        push_quad(
            raw_x[0], raw_y[0], cr[0], cg[0], cb[0], tu[0], tv[0],
            raw_x[1], raw_y[1], cr[1], cg[1], cb[1], tu[1], tv[1],
            raw_x[2], raw_y[2], cr[2], cg[2], cb[2], tu[2], tv[2],
            raw_x[3], raw_y[3], cr[3], cg[3], cb[3], tu[3], tv[3],
            clut, tp, flags, semi_mode, tex_depth, PrimOrigin::origin_2d_hud, face_idx);
    }
    else
    {
        face_idx = decode_face_tri(raw_x[0], raw_y[0], raw_x[1], raw_y[1],
            raw_x[2], raw_y[2]);
        push_triangle(
            raw_x[0], raw_y[0], cr[0], cg[0], cb[0], tu[0], tv[0],
            raw_x[1], raw_y[1], cr[1], cg[1], cb[1], tu[1], tv[1],
            raw_x[2], raw_y[2], cr[2], cg[2], cb[2], tu[2], tv[2],
            clut, tp, flags, semi_mode, tex_depth, PrimOrigin::origin_2d_hud, face_idx);
    }
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
        PrimOrigin::origin_2d_line, 0xFFFFFFFFu);
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
        PrimOrigin::origin_2d_rect, 0xFFFFFFFFu);
    push_triangle(
        x1, y0, r, g, b, (uint8_t)(u0 + w), v0,
        x1, y1, r, g, b, (uint8_t)(u0 + w), (uint8_t)(v0 + h),
        x0, y1, r, g, b, u0, (uint8_t)(v0 + h),
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

    // --- Absolute REF_BASE fallback ---
    // When no reference vertex exists in the GP0 packet (game reads only SXY2
    // carriers from each RTPT, never SXY1 reference), or when per-axis
    // differential fails, decode each axis against the known REF_BASE.
    // This is the primary path for "carrier-only" quad patterns.
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
// Helper: prepare DrawCmd vertex for 2D (sign_extend_11 + offset ADDITION)
// Matches the primary GPU pipeline: se11(raw) + draw_offset = VRAM position.
// UE5 centers the result using display.width()/height() (same as 2D component).
//
// For 3D tagged coords, returns raw values (UE5 uses verts_3d from DrawCmd3D).
// ---------------------------------------------------------------------------
static DrawVertex make_vertex(uint16_t raw_x, uint16_t raw_y,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t u, uint8_t v,
                              bool is_2d, int16_t ox, int16_t oy)
{
    if (is_2d)
    {
        const int16_t sx = (int16_t)(se11(raw_x) + ox);
        const int16_t sy = (int16_t)(se11(raw_y) + oy);
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
            face_idx = 0xFFFFFFFFu; // decoded but not in cache
    }
    else
        face_idx = 0xFFFFFFFFu; // no gte_3d_ or no tag decoded

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
    PrimOrigin origin, uint32_t face_idx)
{
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

    // Shared 2D coord processing params
    const bool is_2d = !is_3d;
    const int16_t ox = is_2d ? draw_env_.offset_x : 0;
    const int16_t oy = is_2d ? draw_env_.offset_y : 0;

    // Build vertices once, reuse for both triangles
    const DrawVertex dv0 = make_vertex(x0, y0, r0, g0, b0, u0, v0, is_2d, ox, oy);
    const DrawVertex dv1 = make_vertex(x1, y1, r1, g1, b1, u1, v1, is_2d, ox, oy);
    const DrawVertex dv2 = make_vertex(x2, y2, r2, g2, b2, u2, v2, is_2d, ox, oy);
    const DrawVertex dv3 = make_vertex(x3, y3, r3, g3, b3, u3, v3, is_2d, ox, oy);

    // --- Triangle 1: V0, V1, V2 (quad_half=0) ---
    {
        DrawCmd3D cmd3d{};
        cmd3d.is_quad = true;
        cmd3d.quad_half = 0;
        cmd3d.face_idx = face_idx;
        cmd3d.origin = origin;
        cmd3d.ot_z = current_ot_z_;

        if (is_3d)
            fill_cmd3d_from_face(cmd3d, face, 0, 1, 2);

        DrawCmd cmd{};
        cmd.v[0] = dv0; cmd.v[1] = dv1; cmd.v[2] = dv2;
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

        if (is_3d)
        {
            if (qc)
                fill_cmd3d_from_quad(cmd3d, qc, 1, 3, 2);
            else
                fill_cmd3d_from_face(cmd3d, face, 1, 1, 2); // V3 ≈ V1 fallback
        }

        DrawCmd cmd{};
        cmd.v[0] = dv1; cmd.v[1] = dv3; cmd.v[2] = dv2;
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
