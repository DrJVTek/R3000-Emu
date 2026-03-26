#include "gpu.h"
#include "gte_correlation.h"
#include "../log/emu_log.h"
#include "../util/file_util.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>

using util::fopen_utf8;

namespace gpu
{

static void gpu_log(
    const flog::Sink& s,
    const flog::Sink& combined,
    const flog::Clock& c,
    int has_clock,
    flog::Level lvl,
    const char* fmt,
    ...
)
{
    if (!has_clock || (!s.f && !combined.f) || !fmt)
        return;

    va_list args;
    va_start(args, fmt);
    flog::vlogf(s, c, lvl, "GPU", fmt, args);
    va_end(args);

    va_start(args, fmt);
    flog::vlogf(combined, c, lvl, "GPU", fmt, args);
    va_end(args);
}

// PS1 vertex X/Y use 11-bit signed (bits 0-10 and 16-26). Upper bits are
// "usually sign-extension" per spec; we must sign-extend for correctness.
static int16_t sign_extend_11(int32_t v)
{
    v &= 0xFFFF;
    int32_t lo = v & 0x7FF;
    if (lo & 0x400)
        lo |= ~0x7FF;  // sign extend 11 -> 32
    return (int16_t)lo;
}

// Pass through 8-bit color values directly
static uint8_t color8(uint8_t v)
{
    return v;
}

// ---------------------------------------------------------------------------
// GP0 parameter count lookup
// Returns number of ADDITIONAL words after the command word (0 = single word cmd)
// Returns -1 for polyline (variable length)
// Returns -2 for CPU→VRAM (A0h, needs special handling)
// ---------------------------------------------------------------------------
int Gpu::gp0_param_count(uint8_t cmd)
{
    // 00h-1Fh: Misc
    if (cmd == 0x00) return 0; // NOP
    if (cmd == 0x01) return 0; // Clear cache
    if (cmd == 0x02) return 2; // Fill rectangle (color, topleft+size)
    if (cmd == 0x1F) return 0; // IRQ request
    if (cmd <= 0x1F) return 0; // NOP mirrors

    // 20h-3Fh: Polygons
    if (cmd >= 0x20 && cmd <= 0x3F)
    {
        const bool gouraud  = (cmd & 0x10) != 0;
        const bool quad     = (cmd & 0x08) != 0;
        const bool textured = (cmd & 0x04) != 0;
        const int verts = quad ? 4 : 3;

        // Base: 1 cmd word (with color) + (verts-1) XY words for flat, or verts color+XY pairs for gouraud
        // Textured adds 1 UV+attr word per vertex
        int words = 0;
        if (gouraud)
            words = verts * 2; // color+XY per vert (first color is in cmd word, but XY follows)
        else
            words = 1 + verts; // cmd(color) + N XY coords

        // Actually let me be precise per psx-spx:
        // Flat triangle: cmd+color, v0, v1, v2 = 4 total (3 params)
        // Flat textured tri: cmd+color, v0, uv0+clut, v1, uv1+tpage, v2, uv2+pad = 7 total (6 params)
        // Gouraud tri: cmd+c0, v0, c1, v1, c2, v2 = 6 total (5 params)
        // Gouraud textured tri: cmd+c0, v0, uv0+clut, c1, v1, uv1+tpage, c2, v2, uv2+pad = 9 total (8 params)
        // Same pattern but +1 vertex for quads

        if (!gouraud && !textured)
            return verts;                          // flat: N vertex XYs
        if (!gouraud && textured)
            return verts * 2;                      // flat tex: N * (XY + UV)
        if (gouraud && !textured)
            return verts * 2 - 1;                  // gouraud: (N-1) * (color+XY) + 1 XY (first color in cmd)
        // gouraud + textured
        return verts * 3 - 1;                      // (N-1)*(color+XY+UV) + (XY+UV)
    }

    // 40h-5Fh: Lines
    if (cmd >= 0x40 && cmd <= 0x5F)
    {
        const bool gouraud  = (cmd & 0x10) != 0;
        const bool polyline = (cmd & 0x08) != 0;

        if (polyline)
            return -1; // Variable length, terminated by 0x5xxx5xxx

        // Single line
        if (!gouraud)
            return 2;  // XY0, XY1
        return 3;      // XY0, color1, XY1
    }

    // 60h-7Fh: Rectangles
    if (cmd >= 0x60 && cmd <= 0x7F)
    {
        const int size_code = (cmd >> 3) & 3;  // 0=variable, 1=1x1, 2=8x8, 3=16x16
        const bool textured = (cmd & 0x04) != 0;

        int params = 1; // XY always
        if (size_code == 0) params++; // variable size needs W+H word
        if (textured) params++;       // UV+CLUT word
        return params;
    }

    // 80h-9Fh: VRAM to VRAM copy
    if (cmd >= 0x80 && cmd <= 0x9F)
        return 3; // src XY, dst XY, size

    // A0h: CPU to VRAM
    if (cmd >= 0xA0 && cmd <= 0xBF)
        return -2; // Special: 2 params then pixel data

    // C0h: VRAM to CPU
    if (cmd >= 0xC0 && cmd <= 0xDF)
        return 2; // src XY, size

    // E0h-FFh: Environment / display
    if (cmd >= 0xE1 && cmd <= 0xE6)
        return 0; // Single word commands

    return 0; // Unknown / NOP
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
Gpu::Gpu(rlog::Logger* logger)
    : logger_(logger)
    , vram_(std::make_unique<uint16_t[]>(kVramPixels))
{
    // Version marker - update when making changes!
    emu::logf(emu::LogLevel::warn, "GPU", "GPU source v14 (session_2026_03_22)");
    status_ = 0x1490'2000u; // PAL default (bit 20 = 1) — matches SCPH-7502 hardware
    dma_dir_ = 0;
    vblank_div_ = 0;
    std::memset(vram_.get(), 0, kVramPixels * sizeof(uint16_t));
}

// ---------------------------------------------------------------------------
// Push a triangle draw command to the active frame list (for UE5 bridge)
// ---------------------------------------------------------------------------
void Gpu::push_triangle(
    int16_t x0, int16_t y0, uint8_t r0, uint8_t g0, uint8_t b0, uint8_t u0, uint8_t v0,
    int16_t x1, int16_t y1, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t u1, uint8_t v1,
    int16_t x2, int16_t y2, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t u2, uint8_t v2,
    uint16_t clut, uint16_t texpage, uint8_t flags, uint8_t semi_mode, uint8_t tex_depth)
{
    // Triangle bounding box (always computed for large-primitive rejection)
    const int16_t min_x = (std::min)({x0, x1, x2});
    const int16_t max_x = (std::max)({x0, x1, x2});
    const int16_t min_y = (std::min)({y0, y1, y2});
    const int16_t max_y = (std::max)({y0, y1, y2});

    // PS1 rejects "too large" primitives (>1023 pixels span in any axis).
    // Always active — prevents degenerate triangles from GTE overflow.
    if ((max_x - min_x) > 1023 || (max_y - min_y) > 1023)
        return;

    // Draw area clipping (GP0 E3h/E4h). Skipped in VR mode so that
    // off-screen polygons are available for full 3D reconstruction.
    if (clip_to_draw_area_)
    {
        const int16_t cx1 = (int16_t)draw_env_.clip_x1;
        const int16_t cy1 = (int16_t)draw_env_.clip_y1;
        const int16_t cx2 = (int16_t)draw_env_.clip_x2;
        const int16_t cy2 = (int16_t)draw_env_.clip_y2;
        if (max_x < cx1 || min_x > cx2 || max_y < cy1 || min_y > cy2)
            return;
    }

    // Subtract draw_offset: vertices arrive with VRAM-absolute coords (offset baked in).
    // We convert to screen-relative coords so both double-buffer halves
    // (Y=0 and Y=240) map to the same position. We render 3D, not VRAM.
    const int16_t ox = draw_env_.offset_x;
    const int16_t oy = draw_env_.offset_y;
    DrawCmd cmd{};
    cmd.v[0] = {(int16_t)(x0 - ox), (int16_t)(y0 - oy), r0, g0, b0, u0, v0};
    cmd.v[1] = {(int16_t)(x1 - ox), (int16_t)(y1 - oy), r1, g1, b1, u1, v1};
    cmd.v[2] = {(int16_t)(x2 - ox), (int16_t)(y2 - oy), r2, g2, b2, u2, v2};
    cmd.clut = clut;
    cmd.texpage = texpage;
    cmd.flags = flags;
    cmd.semi_mode = semi_mode;
    cmd.tex_depth = tex_depth;
    draw_lists_[draw_active_].push(cmd);

    // 3D reconstruction: correlate with GTE snapshot
    // GTE records SXY = raw screen coords (no draw offset).
    // gp0_polygon adds draw offset before calling push_triangle, then
    // push_triangle subtracts it again (cmd.v = vertex - offset).
    // So cmd.v == original GTE SXY == the key recorded by GTE. Match!
    if (gte_corr_)
    {
        GteSxyKey key{};
        key.sx[0] = cmd.v[0].x; key.sy[0] = cmd.v[0].y;
        key.sx[1] = cmd.v[1].x; key.sy[1] = cmd.v[1].y;
        key.sx[2] = cmd.v[2].x; key.sy[2] = cmd.v[2].y;

        // Diagnostic: log first lookups with table state to debug empty-table issue
        static int lookup_diag = 0;
        if (lookup_diag < 20 && frame_count_ > 30)
        {
            emu::logf(emu::LogLevel::warn, "GPU",
                "CORR_LOOKUP #%d f=%u tbl_size=%u rec=%u key=(%d,%d)(%d,%d)(%d,%d) ofs=(%d,%d)",
                lookup_diag, frame_count_,
                (unsigned)gte_corr_->size(), gte_corr_->frame_records(),
                key.sx[0], key.sy[0], key.sx[1], key.sy[1], key.sx[2], key.sy[2],
                (int)draw_env_.offset_x, (int)draw_env_.offset_y);
            if (lookup_diag < 3 && gte_corr_->size() > 0)
                gte_corr_->dump_first_entries(5);
            ++lookup_diag;
        }

        DrawCmd3D cmd3d{};
        auto* corr = gte_corr_->lookup_mut(key);
        bool swapped = false;

        // Quad second-triangle fix: PS1 quads split as (v0,v1,v2)+(v1,v3,v2).
        // The GTE FIFO after 4 RTPS calls holds (v1,v2,v3), but the GPU's
        // second triangle sends vertices in order (v1,v3,v2) — positions 1&2
        // are swapped vs the FIFO. Try swapped key as fallback.
        if (!corr)
        {
            GteSxyKey swapped_key{};
            swapped_key.sx[0] = key.sx[0]; swapped_key.sy[0] = key.sy[0];
            swapped_key.sx[1] = key.sx[2]; swapped_key.sy[1] = key.sy[2]; // swap 1↔2
            swapped_key.sx[2] = key.sx[1]; swapped_key.sy[2] = key.sy[1];
            corr = gte_corr_->lookup_mut(swapped_key);
            if (corr) swapped = true;
        }

        if (corr)
        {
            cmd3d.origin = PrimOrigin::origin_3d;
            const auto& snap = corr->snapshot;
            if (!swapped)
            {
                for (int i = 0; i < 3; ++i)
                {
                    cmd3d.verts_3d[i] = snap.vertices[i];
                    cmd3d.sz[i] = snap.sz[i];
                }
                frame_stats_.corr_hit++;
            }
            else
            {
                // Swapped match: GTE FIFO was (v1,v2,v3) but GPU wants (v1,v3,v2).
                // Vertex 0 stays, swap vertices 1↔2 to match GPU triangle order.
                cmd3d.verts_3d[0] = snap.vertices[0];
                cmd3d.verts_3d[1] = snap.vertices[2]; // GTE slot 2 → GPU slot 1
                cmd3d.verts_3d[2] = snap.vertices[1]; // GTE slot 1 → GPU slot 2
                cmd3d.sz[0] = snap.sz[0];
                cmd3d.sz[1] = snap.sz[2];
                cmd3d.sz[2] = snap.sz[1];
                frame_stats_.corr_hit_swap++;
            }
            cmd3d.transform = snap.transform;

            // Diagnostic: log first N hits with full 3D data
            static int hit_log = 0;
            if (hit_log < 10 && frame_count_ > 200)
            {
                const auto& v0 = cmd3d.verts_3d[0];
                const auto& v1 = cmd3d.verts_3d[1];
                const auto& v2 = cmd3d.verts_3d[2];
                const auto& t = snap.transform;
                int32_t cz0 = (t.rt[6]*v0.vx + t.rt[7]*v0.vy + t.rt[8]*v0.vz) / 4096 + t.tr[2];
                int32_t cz1 = (t.rt[6]*v1.vx + t.rt[7]*v1.vy + t.rt[8]*v1.vz) / 4096 + t.tr[2];
                int32_t cz2 = (t.rt[6]*v2.vx + t.rt[7]*v2.vy + t.rt[8]*v2.vz) / 4096 + t.tr[2];
                emu::logf(emu::LogLevel::warn, "GPU",
                    "CORR HIT #%d f=%u tex=%d swap=%d sxy=(%d,%d)(%d,%d)(%d,%d) "
                    "model=(%d,%d,%d)(%d,%d,%d)(%d,%d,%d) "
                    "TR=(%d,%d,%d) camZ=(%d,%d,%d)",
                    hit_log, frame_count_, (flags & 1) ? 1 : 0, swapped ? 1 : 0,
                    key.sx[0], key.sy[0], key.sx[1], key.sy[1], key.sx[2], key.sy[2],
                    v0.vx, v0.vy, v0.vz, v1.vx, v1.vy, v1.vz, v2.vx, v2.vy, v2.vz,
                    t.tr[0], t.tr[1], t.tr[2], cz0, cz1, cz2);
                ++hit_log;
            }
        }
        else
        {
            frame_stats_.corr_miss++;
            // Diagnostic: log missed lookups — early misses + gameplay misses
            static int miss_log = 0;
            static int miss_log_late = 0;
            const bool early = (miss_log < 10 && gte_corr_->size() > 10);
            const bool late  = (miss_log_late < 20 && frame_count_ > 500
                                && gte_corr_->size() > 100 && frame_stats_.corr_miss <= 3);
            if (early || late)
            {
                emu::logf(emu::LogLevel::warn, "GPU",
                    "CORR MISS #%d/%d f=%u tex=%d key=(%d,%d)(%d,%d)(%d,%d) tbl=%u rec=%u hit=%u ofs=(%d,%d)",
                    miss_log, miss_log_late, frame_count_, (flags & 1) ? 1 : 0,
                    key.sx[0], key.sy[0], key.sx[1], key.sy[1], key.sx[2], key.sy[2],
                    (unsigned)gte_corr_->size(), gte_corr_->frame_records(), gte_corr_->frame_hits(),
                    (int)draw_env_.offset_x, (int)draw_env_.offset_y);
                if ((early && miss_log == 0) || (late && miss_log_late == 0))
                    gte_corr_->dump_first_entries(5);
                if (early) ++miss_log;
                if (late) ++miss_log_late;
            }
        }
        draw_lists_[draw_active_].push_3d(cmd3d);
    }

    // Track semi-transparency stats
    if (flags & 2)
    {
        frame_stats_.semi_tris++;
        frame_stats_.semi_mode_count[semi_mode & 3]++;
    }
}

// ---------------------------------------------------------------------------
// VBlank
// ---------------------------------------------------------------------------
int Gpu::tick_vblank(uint32_t cycles)
{
    vblank_div_ += cycles;

    // Use correct timing based on video mode (PAL vs NTSC)
    const uint32_t period   = display_.is_pal ? kVblankPeriodCyclesPal  : kVblankPeriodCyclesNtsc;
    const uint32_t duration = display_.is_pal ? kVblankDurationPal      : kVblankDurationNtsc;

    uint32_t vblank_start = period - duration;
    in_vblank_ = (vblank_div_ >= vblank_start);

    if (vblank_div_ >= period)
    {
        vblank_div_ -= period; // Preserve remainder for accurate timing
        in_vblank_ = false;

        // Toggle even/odd field flag each VBlank.
        // GPUSTAT bit 31 = "Drawing even/odd lines in interlace mode"
        // Games poll this bit to detect VSync (wait for it to change).
        // Real hardware toggles this every frame regardless of interlace setting.
        even_odd_field_ = !even_odd_field_;

        // Toggle interlace field bit (GPUSTAT bit 13) each frame when interlace is enabled
        if (display_.interlace)
            status_ ^= (1u << 13);

        // Swap draw lists every VBlank. Simple and reliable.
        // Vertices are stored with draw_offset subtracted (screen-relative),
        // so both double-buffer halves overlap at the same coords.
        {
            std::lock_guard<std::mutex> lock(draw_list_mutex_);
            draw_lists_[draw_active_].frame_id = frame_count_;
            draw_lists_[draw_active_].draw_env = draw_env_;
            draw_lists_[draw_active_].display = display_;
            draw_active_ = 1 - draw_active_;
            draw_lists_[draw_active_].clear();
            // Diagnostic: log correlation stats before clearing
            if (gte_corr_)
            {
                const auto rec = gte_corr_->frame_records();
                const auto hit = gte_corr_->frame_hits();
                const auto sz  = gte_corr_->size();
                const auto& dl = draw_lists_[1 - draw_active_];
                const auto ncmds = dl.cmds.size();
                const auto n3d = dl.cmds_3d.size();
                // Count 3D vs 2D primitives in the draw list
                uint32_t cnt_3d = 0, cnt_2d_hud = 0, cnt_2d_rect = 0, cnt_2d_line = 0;
                for (size_t i = 0; i < n3d; ++i)
                {
                    switch (dl.cmds_3d[i].origin)
                    {
                    case PrimOrigin::origin_3d:      ++cnt_3d; break;
                    case PrimOrigin::origin_2d_hud:  ++cnt_2d_hud; break;
                    case PrimOrigin::origin_2d_rect: ++cnt_2d_rect; break;
                    case PrimOrigin::origin_2d_line: ++cnt_2d_line; break;
                    }
                }
                // Log ONGOING: every frame for first 30, then every 100 frames
                static int corr_log = 0;
                const bool do_log = (corr_log < 30 && (ncmds > 0 || rec > 0))
                    || (frame_count_ % 100 == 0 && ncmds > 0);
                if (do_log)
                {
                    const int pct = (ncmds > 0) ? (int)(cnt_3d * 100 / ncmds) : 0;
                    emu::logf(emu::LogLevel::warn, "GPU",
                        "CORR f=%u: %u cmds (3D:%u=%d%% hud:%u rect:%u line:%u) "
                        "GTE_rec=%u GTE_hit=%u tbl=%u",
                        frame_count_, (unsigned)ncmds, cnt_3d, pct, cnt_2d_hud,
                        cnt_2d_rect, cnt_2d_line, rec, hit, (unsigned)sz);
                    ++corr_log;
                }
                // Double-buffered swap: write→read, clear new write.
                // Games DMA previous frame's OT first, then GTE computes new frame.
                // GPU looks up in both tables so cross-frame matches work.
                gte_corr_->swap_frame();
            }
            vram_frame_++;
        }

        // Log frame stats
        frame_count_++;

        // Log every 50 VBlanks (~1 second) to confirm timing
        if ((frame_count_ % 50) == 1)
        {
            emu::logf(emu::LogLevel::info, "GPU", "VBlank #%u (%s, period=%u, field=%s)",
                frame_count_, display_.is_pal ? "PAL" : "NTSC", period,
                even_odd_field_ ? "odd" : "even");
        }

        const auto& s = frame_stats_;
        // Log at INFO level for frames 280-295 to debug the transition
        const auto log_level = (frame_count_ >= 280 && frame_count_ <= 295)
            ? emu::LogLevel::info : emu::LogLevel::debug;
        if (s.total_words > 0 || (frame_count_ >= 280 && frame_count_ <= 295))
        {
            emu::logf(log_level, "GPU", "FRAME #%u: %u tri, %u quad, %u rect, %u line, %u fill, "
                "%u v2v, %u c2v, %u v2c, %u env | %u words",
                frame_count_, s.triangles, s.quads, s.rects, s.lines, s.fills,
                s.vram_to_vram, s.cpu_to_vram, s.vram_to_cpu, s.env_cmds, s.total_words);
            emu::logf(log_level, "GPU", "  DRAWENV clip=(%u,%u)-(%u,%u) ofs=(%d,%d) | "
                "DISP start=(%u,%u) wh=(%u,%u) | draw_list=%zu tris",
                draw_env_.clip_x1, draw_env_.clip_y1, draw_env_.clip_x2, draw_env_.clip_y2,
                (int)draw_env_.offset_x, (int)draw_env_.offset_y,
                display_.display_x, display_.display_y, display_.width(), display_.height(),
                draw_lists_[1 - draw_active_].cmds.size());
            if (s.semi_tris > 0)
                emu::logf(emu::LogLevel::info, "GPU", "  SEMI: %u tris [mode0=%u mode1=%u mode2=%u mode3=%u] texpage_raw=0x%06X",
                    s.semi_tris, s.semi_mode_count[0], s.semi_mode_count[1],
                    s.semi_mode_count[2], s.semi_mode_count[3],
                    draw_env_.texpage_raw);

            // 3D reconstruction: per-frame correlation summary (uses counters from push_triangle)
            if (gte_corr_)
            {
                const uint32_t total_corr = s.corr_hit + s.corr_hit_swap + s.corr_miss;
                if (total_corr > 0)
                {
                    const int pct = (int)((s.corr_hit + s.corr_hit_swap) * 100 / total_corr);
                    emu::logf(emu::LogLevel::info, "GPU",
                        "  3D_CORR: hit=%u swap=%u miss=%u (%d%% matched) | GTE: %u projections",
                        s.corr_hit, s.corr_hit_swap, s.corr_miss, pct, gte_corr_->frame_records());
                }
            }
        }
        prev_frame_stats_ = frame_stats_;  // Save before reset for stuck detection
        frame_stats_.reset();

        return 1; // Signal VBlank IRQ
    }
    return 0;
}

uint32_t Gpu::current_scanline() const
{
    const uint32_t period = display_.is_pal ? kVblankPeriodCyclesPal : kVblankPeriodCyclesNtsc;
    const uint32_t total_lines = display_.is_pal ? 314u : 263u;
    if (period == 0u)
        return 0u;
    return (vblank_div_ * total_lines) / period;
}

uint32_t Gpu::total_scanlines() const
{
    return display_.is_pal ? 314u : 263u;
}

// ---------------------------------------------------------------------------
// Logging / dump
// ---------------------------------------------------------------------------
void Gpu::set_log_sinks(const flog::Sink& gpu_only, const flog::Sink& combined, const flog::Clock& clock)
{
    log_gpu_ = gpu_only;
    log_io_ = combined;
    clock_ = clock;
    has_clock_ = 1;

    gpu_log(log_gpu_, log_io_, clock_, has_clock_, flog::Level::info,
        "log start (gpu_level=%u io_level=%u)",
        (unsigned)log_gpu_.level, (unsigned)log_io_.level);
}

void Gpu::set_dump_file(const char* path)
{
    if (dump_) { std::fclose(dump_); dump_ = nullptr; }
    if (path && *path)
        dump_ = fopen_utf8(path, "wb");
}

void Gpu::dump_u32(uint32_t port, uint32_t v)
{
    if (!dump_) return;
    (void)std::fwrite(&port, 1, sizeof(port), dump_);
    (void)std::fwrite(&v, 1, sizeof(v), dump_);
    std::fflush(dump_);
}

// ---------------------------------------------------------------------------
// MMIO Read
// ---------------------------------------------------------------------------
uint32_t Gpu::mmio_read32(uint32_t addr)
{
    switch (addr)
    {
        case 0x1F80'1810u: // GPUREAD
        {
            uint32_t out = 0;
            if (vram_to_cpu_active_ && read_vram_w_ != 0 && read_vram_h_ != 0)
            {
                for (uint32_t i = 0; i < 2; ++i)
                {
                    const uint32_t x = (uint32_t)(read_vram_x_ + read_vram_col_) % kVramWidth;
                    const uint32_t y = (uint32_t)(read_vram_y_ + read_vram_row_) % kVramHeight;
                    const uint16_t px = vram_[y * kVramWidth + x];
                    out |= (uint32_t)px << (i * 16u);

                    read_vram_col_++;
                    if (read_vram_col_ >= read_vram_w_)
                    {
                        read_vram_col_ = 0;
                        read_vram_row_++;
                        if (read_vram_row_ >= read_vram_h_)
                        {
                            vram_to_cpu_active_ = false;
                            read_vram_x_ = read_vram_y_ = 0;
                            read_vram_w_ = read_vram_h_ = 0;
                            read_vram_col_ = read_vram_row_ = 0;
                            break;
                        }
                    }
                }
            }
            return out;
        }

        case 0x1F80'1814u: // GPUSTAT
        {
            uint32_t v = status_;
            const uint32_t ready_cmd = (gp0_state_ == Gp0State::idle) ? 1u : 0u;
            const uint32_t ready_dma = ready_cmd;
            const uint32_t ready_v2c = vram_to_cpu_active_ ? 1u : 0u;
            const uint32_t scanline = current_scanline();
            uint32_t display_line_lsb = 0u;
            if (display_.interlace && display_.v_res)
            {
                // 480i mode: bit31 = (display_y + field) & 1, no VBlank suppression.
                // field toggles at each VBlank. Games like Tekken read GPUSTAT during
                // VBlank (field=N), wait one VBlank, then read again (field=N^1) and
                // XOR bit31 to detect the frame transition. Suppressing field during
                // VBlank made the two reads identical (no change) -> init_slot loop.
                const uint32_t field = even_odd_field_ ? 1u : 0u;
                display_line_lsb =
                    (uint32_t)((display_.display_y + field) & 1u);
            }
            else
            {
                // Non-480i: DuckStation uses (display_y + current_scanline) & 1,
                // updated at each CRTC tick. We don't have per-scanline ticks,
                // so we use the per-frame toggle which works for VSync detection.
                // TODO: implement CRTC scanline tick events for full accuracy.
                display_line_lsb = even_odd_field_ ? 1u : 0u;
            }

            v &= ~((1u << 26) | (1u << 28) | (1u << 27));
            if (ready_cmd) v |= (1u << 26);
            if (ready_dma) v |= (1u << 28);
            if (ready_v2c) v |= (1u << 27);

            v &= ~(3u << 29);
            v |= (dma_dir_ & 3u) << 29;

            v &= ~(1u << 25);
            if ((dma_dir_ & 3u) == 1u && ready_dma) v |= (1u << 25);
            else if ((dma_dir_ & 3u) == 2u && ready_dma) v |= (1u << 25);
            else if ((dma_dir_ & 3u) == 3u && ready_v2c) v |= (1u << 25);

            // Bit 31: display line parity, not a simple once-per-frame toggle.
            // Some games poll GPUSTAT together with a timer/HBlank counter and
            // expect this bit to reflect current raster progression.
            v &= ~(1u << 31);
            if (display_line_lsb) v |= (1u << 31);

            return v;
        }
    }
    return 0;
}

Stage67GpuDebug Gpu::stage67_debug() const
{
    Stage67GpuDebug d{};
    const uint32_t scanline = current_scanline();
    uint32_t display_line_lsb = 0u;
    if (display_.interlace && display_.v_res)
    {
        const uint32_t active_field = in_vblank_ ? 0u : (even_odd_field_ ? 1u : 0u);
        display_line_lsb = (uint32_t)((display_.display_y + scanline + active_field) & 1u);
    }
    else
    {
        display_line_lsb = even_odd_field_ ? 1u : 0u;
    }

    uint32_t v = status_;
    const uint32_t ready_cmd = (gp0_state_ == Gp0State::idle) ? 1u : 0u;
    const uint32_t ready_dma = ready_cmd;
    const uint32_t ready_v2c = vram_to_cpu_active_ ? 1u : 0u;
    v &= ~((1u << 26) | (1u << 28) | (1u << 27));
    if (ready_cmd) v |= (1u << 26);
    if (ready_dma) v |= (1u << 28);
    if (ready_v2c) v |= (1u << 27);
    v &= ~(3u << 29);
    v |= (dma_dir_ & 3u) << 29;
    v &= ~(1u << 25);
    if ((dma_dir_ & 3u) == 1u && ready_dma) v |= (1u << 25);
    else if ((dma_dir_ & 3u) == 2u && ready_dma) v |= (1u << 25);
    else if ((dma_dir_ & 3u) == 3u && ready_v2c) v |= (1u << 25);
    v &= ~(1u << 31);
    if (display_line_lsb) v |= (1u << 31);

    d.gpustat = v;
    d.scanline = scanline;
    d.display_line_lsb = display_line_lsb;
    d.display_y = display_.display_y;
    d.h_res = display_.h_res;
    d.v_res = display_.v_res;
    d.is_pal = display_.is_pal ? 1u : 0u;
    d.interlace = display_.interlace ? 1u : 0u;
    d.in_vblank = in_vblank_ ? 1u : 0u;
    d.even_odd_field = even_odd_field_ ? 1u : 0u;
    d.frame_count = frame_count_;
    return d;
}

// ---------------------------------------------------------------------------
// MMIO Write
// ---------------------------------------------------------------------------
void Gpu::mmio_write32(uint32_t addr, uint32_t v)
{
    switch (addr)
    {
        case 0x1F80'1810u: // GP0
            dump_u32(0, v);
            gp0_write(v);
            break;

        case 0x1F80'1814u: // GP1
            dump_u32(1, v);
            gp1_write(v);
            break;
    }
}

// ---------------------------------------------------------------------------
// GP0 Write - Main state machine
// ---------------------------------------------------------------------------
void Gpu::gp0_write(uint32_t v)
{
    frame_stats_.total_words++;

    // If a VRAM→CPU transfer is active, any GP0 write cancels it
    // (the BIOS/game may start new commands before fully draining GPUREAD)
    if (vram_to_cpu_active_)
    {
        vram_to_cpu_active_ = false;
        read_vram_x_ = read_vram_y_ = 0;
        read_vram_w_ = read_vram_h_ = 0;
        read_vram_col_ = read_vram_row_ = 0;
    }

    // Special state: receiving pixel data for CPU→VRAM
    if (gp0_state_ == Gp0State::receiving_vram_data)
    {
        gp0_cpu_to_vram_data(v);
        return;
    }

    // Special state: polyline (variable-length, terminated by 0x5xxx5xxx pattern)
    if (gp0_state_ == Gp0State::polyline)
    {
        // Check terminator: bits 12-15 and 28-31 both = 5
        bool is_term = ((v & 0xF000F000u) == 0x50005000u);
        if (is_term)
        {
            gp0_state_ = Gp0State::idle;
            polyline_has_prev_ = false;
            return;
        }

        // Parse vertex data based on current phase
        uint8_t cur_r = polyline_prev_r_, cur_g = polyline_prev_g_, cur_b = polyline_prev_b_;
        int16_t cur_x = 0, cur_y = 0;

        if (polyline_gouraud_ && polyline_vertex_phase_ == 0 && polyline_has_prev_)
        {
            // Gouraud: expecting color word for next vertex
            cur_r = color8((uint8_t)(v & 0xFF));
            cur_g = color8((uint8_t)((v >> 8) & 0xFF));
            cur_b = color8((uint8_t)((v >> 16) & 0xFF));
            polyline_prev_r_ = cur_r;
            polyline_prev_g_ = cur_g;
            polyline_prev_b_ = cur_b;
            polyline_vertex_phase_ = 1; // Next is XY
            return;
        }

        // Expecting XY word
        cur_x = sign_extend_11(v);
        cur_y = sign_extend_11((int32_t)(v >> 16));

        // Apply drawing offset
        cur_x = (int16_t)(cur_x + draw_env_.offset_x);
        cur_y = (int16_t)(cur_y + draw_env_.offset_y);

        // If we have a previous vertex, draw a line segment
        if (polyline_has_prev_)
        {
            // Draw line as thin quad (same as gp0_line)
            int16_t x0 = polyline_prev_x_, y0 = polyline_prev_y_;
            int16_t x1 = cur_x, y1 = cur_y;
            uint8_t r0 = polyline_gouraud_ ? polyline_prev_r_ : cur_r;
            uint8_t g0 = polyline_gouraud_ ? polyline_prev_g_ : cur_g;
            uint8_t b0 = polyline_gouraud_ ? polyline_prev_b_ : cur_b;

            int16_t dx = (int16_t)(x1 - x0);
            int16_t dy = (int16_t)(y1 - y0);
            int16_t px, py;
            if (dx == 0 && dy == 0) { px = 1; py = 0; }
            else if (std::abs(dx) >= std::abs(dy)) { px = 0; py = 1; }
            else { px = 1; py = 0; }

            uint16_t tp = (uint16_t)(draw_env_.texpage_raw & 0xFFFF);
            uint8_t semi_mode = (uint8_t)((tp >> 5) & 3);
            uint8_t flags = polyline_semi_ ? (uint8_t)2 : (uint8_t)0;

            push_triangle(
                (int16_t)(x0 - px), (int16_t)(y0 - py), r0, g0, b0, 0, 0,
                (int16_t)(x0 + px), (int16_t)(y0 + py), r0, g0, b0, 0, 0,
                (int16_t)(x1 + px), (int16_t)(y1 + py), cur_r, cur_g, cur_b, 0, 0,
                0, tp, flags, semi_mode, 0);
            push_triangle(
                (int16_t)(x0 - px), (int16_t)(y0 - py), r0, g0, b0, 0, 0,
                (int16_t)(x1 + px), (int16_t)(y1 + py), cur_r, cur_g, cur_b, 0, 0,
                (int16_t)(x1 - px), (int16_t)(y1 - py), cur_r, cur_g, cur_b, 0, 0,
                0, tp, flags, semi_mode, 0);
        }

        // Store current vertex as previous for next segment
        polyline_prev_x_ = cur_x;
        polyline_prev_y_ = cur_y;
        if (!polyline_gouraud_)
        {
            // Flat shading keeps same color
        }
        polyline_has_prev_ = true;
        polyline_vertex_phase_ = 0; // Next is color (gouraud) or XY (flat)

        return;
    }

    // Collecting parameters for a command
    if (gp0_state_ == Gp0State::collecting_params)
    {
        if (cmd_buf_pos_ < 16)
            cmd_buf_[cmd_buf_pos_++] = v;

        if (cmd_buf_pos_ >= cmd_words_needed_)
        {
            gp0_execute();
            // gp0_execute may set a new state (e.g. receiving_vram_data for A0h)
            // Only reset to idle if still in collecting_params
            if (gp0_state_ == Gp0State::collecting_params)
                gp0_state_ = Gp0State::idle;
        }
        return;
    }

    // Idle: new command word
    gp0_start_command(v);
}

// ---------------------------------------------------------------------------
// GP0 Start Command
// ---------------------------------------------------------------------------
void Gpu::gp0_start_command(uint32_t cmd_word)
{
    const uint8_t cmd = (uint8_t)(cmd_word >> 24);
    const int params = gp0_param_count(cmd);

    cmd_buf_[0] = cmd_word;
    cmd_buf_pos_ = 1;

    if (params == 0)
    {
        // Single-word command: execute immediately
        cmd_words_needed_ = 1;
        gp0_execute();
        return;
    }

    if (params == -1)
    {
        // Polyline: variable length
        gp0_state_ = Gp0State::polyline;
        polyline_gouraud_ = (cmd & 0x10) != 0;
        polyline_semi_ = (cmd & 0x02) != 0;
        polyline_color_ = cmd_word & 0x00FFFFFFu;
        polyline_has_prev_ = false;
        polyline_vertex_phase_ = 0; // Next word is XY (flat) or color (gouraud after first)
        // Store initial color for flat shading
        polyline_prev_r_ = color8((uint8_t)(cmd_word & 0xFF));
        polyline_prev_g_ = color8((uint8_t)((cmd_word >> 8) & 0xFF));
        polyline_prev_b_ = color8((uint8_t)((cmd_word >> 16) & 0xFF));
        frame_stats_.lines++;

        emu::logf(emu::LogLevel::trace, "GPU", "GP0 POLYLINE%s%s color=%06X",
            polyline_gouraud_ ? "_GOURAUD" : "_FLAT",
            polyline_semi_ ? "_SEMI" : "",
            cmd_word & 0x00FFFFFFu);
        return;
    }

    if (params == -2)
    {
        // CPU→VRAM: collect 2 param words first, then pixel data
        gp0_state_ = Gp0State::collecting_params;
        cmd_words_needed_ = 3; // cmd + 2 params
        return;
    }

    // Normal multi-word command
    gp0_state_ = Gp0State::collecting_params;
    cmd_words_needed_ = 1 + params; // cmd word + params
}

// ---------------------------------------------------------------------------
// GP0 Execute - Command is fully buffered, dispatch by type
// ---------------------------------------------------------------------------
void Gpu::gp0_execute()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);

    // 00h-1Fh: Misc
    if (cmd <= 0x1F)
    {
        if (cmd == 0x02) gp0_fill_rect();
        else if (cmd == 0x01)
            emu::logf(emu::LogLevel::trace, "GPU", "GP0 CLEAR_CACHE");
        else if (cmd == 0x1F)
            emu::logf(emu::LogLevel::trace, "GPU", "GP0 IRQ_REQUEST");
        // Others are NOP
        return;
    }

    // 20h-3Fh: Polygons
    if (cmd >= 0x20 && cmd <= 0x3F)
    {
        gp0_polygon();
        return;
    }

    // 40h-5Fh: Lines (single, not polyline)
    if (cmd >= 0x40 && cmd <= 0x5F)
    {
        gp0_line();
        return;
    }

    // 60h-7Fh: Rectangles
    if (cmd >= 0x60 && cmd <= 0x7F)
    {
        gp0_rect();
        return;
    }

    // 80h-9Fh: VRAM→VRAM
    if (cmd >= 0x80 && cmd <= 0x9F)
    {
        gp0_vram_to_vram();
        return;
    }

    // A0h-BFh: CPU→VRAM
    if (cmd >= 0xA0 && cmd <= 0xBF)
    {
        gp0_cpu_to_vram_start();
        return;
    }

    // C0h-DFh: VRAM→CPU
    if (cmd >= 0xC0 && cmd <= 0xDF)
    {
        gp0_vram_to_cpu_start();
        return;
    }

    // E0h-FFh: Environment
    if (cmd >= 0xE1 && cmd <= 0xE6)
    {
        gp0_env_command();
        return;
    }

    // E0h, E7h-EFh: NOP / unknown
}

// ---------------------------------------------------------------------------
// GP0(02h) Fill Rectangle
// ---------------------------------------------------------------------------
void Gpu::gp0_fill_rect()
{
    const uint32_t color = cmd_buf_[0] & 0x00FFFFFFu;
    const uint32_t xy = cmd_buf_[1];
    const uint32_t wh = cmd_buf_[2];

    const uint16_t x = (uint16_t)(xy & 0x3F0u);          // Rounded to 16-pixel
    const uint16_t y = (uint16_t)((xy >> 16) & 0x1FFu);
    const uint16_t w = (uint16_t)(((wh & 0x3FFu) + 0x0F) & ~0x0Fu); // Rounded up to 16
    const uint16_t h = (uint16_t)((wh >> 16) & 0x1FFu);

    // Convert 24-bit RGB to 15-bit PS1 pixel
    const uint16_t r5 = (uint16_t)((color >> 3) & 0x1F);
    const uint16_t g5 = (uint16_t)((color >> 11) & 0x1F);
    const uint16_t b5 = (uint16_t)((color >> 19) & 0x1F);
    const uint16_t pixel = r5 | (g5 << 5) | (b5 << 10);

    // Fill VRAM
    for (uint16_t row = 0; row < h; ++row)
    {
        const uint32_t vy = ((uint32_t)y + row) % kVramHeight;
        for (uint16_t col = 0; col < w; ++col)
        {
            const uint32_t vx = ((uint32_t)x + col) % kVramWidth;
            vram_[vy * kVramWidth + vx] = pixel;
        }
    }

    frame_stats_.fills++;
    vram_write_seq_++;
    emu::logf(emu::LogLevel::trace, "GPU", "GP0 FILL (%u,%u) %ux%u color=%06X", x, y, w, h, color);
}

// ---------------------------------------------------------------------------
// GP0(20h-3Fh) Polygon → parse vertices and push draw commands for UE5
// ---------------------------------------------------------------------------
void Gpu::gp0_polygon()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const bool gouraud  = (cmd & 0x10) != 0;
    const bool quad     = (cmd & 0x08) != 0;
    const bool textured = (cmd & 0x04) != 0;
    const bool semi     = (cmd & 0x02) != 0;
    const bool raw      = (cmd & 0x01) != 0; // raw texture (no color modulation)
    const int nverts = quad ? 4 : 3;

    if (quad) frame_stats_.quads++;
    else      frame_stats_.triangles++;

    // Parse all vertex data from command buffer
    int16_t vx[4], vy[4];
    uint8_t cr[4], cg[4], cb[4];
    uint8_t tu[4] = {}, tv[4] = {};
    uint16_t clut = 0, texpage_attr = 0;

    // First vertex color from command word (PS1 uses 5-bit, expand to 8-bit)
    cr[0] = color8((uint8_t)(cmd_buf_[0] & 0xFF));
    cg[0] = color8((uint8_t)((cmd_buf_[0] >> 8) & 0xFF));
    cb[0] = color8((uint8_t)((cmd_buf_[0] >> 16) & 0xFF));

    const int expected_words = 1 + gp0_param_count(cmd);
    if (cmd_buf_pos_ < expected_words)
    {
        emu::logf(emu::LogLevel::warn, "GPU", "GP0 polygon cmd=0x%02X: insufficient words %d < %d",
            cmd, cmd_buf_pos_, expected_words);
        return;
    }

    int idx = 1;
    for (int i = 0; i < nverts; ++i)
    {
        // Gouraud: each vertex after the first has its own color word
        if (i > 0 && gouraud)
        {
            cr[i] = color8((uint8_t)(cmd_buf_[idx] & 0xFF));
            cg[i] = color8((uint8_t)((cmd_buf_[idx] >> 8) & 0xFF));
            cb[i] = color8((uint8_t)((cmd_buf_[idx] >> 16) & 0xFF));
            idx++;
        }
        else if (i > 0)
        {
            cr[i] = cr[0]; cg[i] = cg[0]; cb[i] = cb[0];
        }

        // Vertex position (PS1: 11-bit signed X/Y; sign-extend for correctness)
        vx[i] = sign_extend_11(cmd_buf_[idx]);
        vy[i] = sign_extend_11((int32_t)(cmd_buf_[idx] >> 16));

        // Log when GPU receives clamped boundary values (indicates GTE overflow)
        if (vx[i] == -1024 || vx[i] == 1023 || vy[i] == -1024 || vy[i] == 1023)
        {
            static int gpu_clamp_log = 0;
            if (gpu_clamp_log++ < 50)
            {
                emu::logf(emu::LogLevel::warn, "GPU",
                    "GP0 vertex %d raw=0x%08X -> (%d,%d) BOUNDARY", i, cmd_buf_[idx], vx[i], vy[i]);
            }
        }
        idx++;

        // Textured: UV + palette/texpage word per vertex
        if (textured)
        {
            tu[i] = (uint8_t)(cmd_buf_[idx] & 0xFF);
            tv[i] = (uint8_t)((cmd_buf_[idx] >> 8) & 0xFF);
            if (i == 0) clut = (uint16_t)((cmd_buf_[idx] >> 16) & 0xFFFF);
            if (i == 1) texpage_attr = (uint16_t)((cmd_buf_[idx] >> 16) & 0xFFFF);
            idx++;
        }
    }

    // Apply drawing offset
    for (int i = 0; i < nverts; ++i)
    {
        vx[i] = (int16_t)(vx[i] + draw_env_.offset_x);
        vy[i] = (int16_t)(vy[i] + draw_env_.offset_y);
    }

    // Determine texpage (from polygon attribute if textured, else from draw env)
    uint16_t tp = textured ? texpage_attr : (uint16_t)(draw_env_.texpage_raw & 0xFFFF);
    uint8_t semi_mode = (uint8_t)((tp >> 5) & 3);
    uint8_t tex_depth = (uint8_t)((tp >> 7) & 3);
    uint8_t flags = 0;
    if (textured) flags |= 1;
    if (semi)     flags |= 2;
    if (raw)      flags |= 4;

    // Debug: log first semi-transparent primitives with full texpage info
    if (semi)
    {
        static int semi_log_count = 0;
        if (semi_log_count++ < 20)
        {
            emu::logf(emu::LogLevel::warn, "GPU",
                "SEMI_POLY cmd=0x%02X textured=%d tp=0x%04X texpage_attr=0x%04X texpage_raw=0x%06X → semi_mode=%u",
                cmd, textured ? 1 : 0, tp, texpage_attr, draw_env_.texpage_raw, semi_mode);
        }
    }

    // Textured polygons update GPUSTAT bits 0-10 from their texpage attribute
    // (real hardware mirrors this — affects subsequent non-textured/rect primitives)
    if (textured)
    {
        draw_env_.texpage_raw = (draw_env_.texpage_raw & ~0x7FFu) | (texpage_attr & 0x7FFu);
        status_ = (status_ & ~0x7FFu) | (texpage_attr & 0x7FFu);
    }

    // Push first triangle (v0, v1, v2)
    push_triangle(
        vx[0], vy[0], cr[0], cg[0], cb[0], tu[0], tv[0],
        vx[1], vy[1], cr[1], cg[1], cb[1], tu[1], tv[1],
        vx[2], vy[2], cr[2], cg[2], cb[2], tu[2], tv[2],
        clut, tp, flags, semi_mode, tex_depth);

    // For quads: second triangle (v1, v3, v2)
    // PS1 quad vertex order: v0=top-left, v1=top-right, v2=bottom-left, v3=bottom-right
    // Triangulation: (v0,v1,v2) + (v1,v3,v2) forms the quad correctly
    if (quad)
    {
        push_triangle(
            vx[1], vy[1], cr[1], cg[1], cb[1], tu[1], tv[1],
            vx[3], vy[3], cr[3], cg[3], cb[3], tu[3], tv[3],
            vx[2], vy[2], cr[2], cg[2], cb[2], tu[2], tv[2],
            clut, tp, flags, semi_mode, tex_depth);
    }

    if (quad)
        emu::logf(emu::LogLevel::trace, "GPU", "GP0 QUAD%s%s%s v0=(%d,%d)#%02X%02X%02X v1=(%d,%d)#%02X%02X%02X v2=(%d,%d)#%02X%02X%02X v3=(%d,%d)#%02X%02X%02X ofs=(%d,%d)",
            gouraud ? "_GOURAUD" : "_FLAT", textured ? "_TEX" : "", semi ? "_SEMI" : "",
            (int)vx[0], (int)vy[0], cr[0], cg[0], cb[0],
            (int)vx[1], (int)vy[1], cr[1], cg[1], cb[1],
            (int)vx[2], (int)vy[2], cr[2], cg[2], cb[2],
            (int)vx[3], (int)vy[3], cr[3], cg[3], cb[3],
            (int)draw_env_.offset_x, (int)draw_env_.offset_y);
    else
        emu::logf(emu::LogLevel::trace, "GPU", "GP0 TRI%s%s%s v0=(%d,%d)#%02X%02X%02X v1=(%d,%d)#%02X%02X%02X v2=(%d,%d)#%02X%02X%02X ofs=(%d,%d)",
            gouraud ? "_GOURAUD" : "_FLAT", textured ? "_TEX" : "", semi ? "_SEMI" : "",
            (int)vx[0], (int)vy[0], cr[0], cg[0], cb[0],
            (int)vx[1], (int)vy[1], cr[1], cg[1], cb[1],
            (int)vx[2], (int)vy[2], cr[2], cg[2], cb[2],
            (int)draw_env_.offset_x, (int)draw_env_.offset_y);
}

// ---------------------------------------------------------------------------
// GP0(40h-5Fh) Line (single) → push as thin quad (2 triangles) for UE5
// ---------------------------------------------------------------------------
void Gpu::gp0_line()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const bool gouraud = (cmd & 0x10) != 0;
    const bool semi    = (cmd & 0x02) != 0;

    frame_stats_.lines++;

    uint8_t r0 = color8((uint8_t)(cmd_buf_[0] & 0xFF));
    uint8_t g0 = color8((uint8_t)((cmd_buf_[0] >> 8) & 0xFF));
    uint8_t b0 = color8((uint8_t)((cmd_buf_[0] >> 16) & 0xFF));

    int16_t x0 = sign_extend_11(cmd_buf_[1]);
    int16_t y0 = sign_extend_11((int32_t)(cmd_buf_[1] >> 16));

    uint8_t r1 = r0, g1 = g0, b1 = b0;
    int idx = 2;
    if (gouraud)
    {
        r1 = color8((uint8_t)(cmd_buf_[idx] & 0xFF));
        g1 = color8((uint8_t)((cmd_buf_[idx] >> 8) & 0xFF));
        b1 = color8((uint8_t)((cmd_buf_[idx] >> 16) & 0xFF));
        idx++;
    }
    int16_t x1 = (idx < cmd_buf_pos_) ? sign_extend_11(cmd_buf_[idx]) : x0;
    int16_t y1 = (idx < cmd_buf_pos_) ? sign_extend_11((int32_t)(cmd_buf_[idx] >> 16)) : y0;

    // Apply drawing offset
    x0 = (int16_t)(x0 + draw_env_.offset_x);
    y0 = (int16_t)(y0 + draw_env_.offset_y);
    x1 = (int16_t)(x1 + draw_env_.offset_x);
    y1 = (int16_t)(y1 + draw_env_.offset_y);

    // Expand line to a thin quad (1px wide) for mesh rendering
    // Compute perpendicular offset
    int16_t dx = (int16_t)(x1 - x0);
    int16_t dy = (int16_t)(y1 - y0);
    int16_t px, py;
    if (dx == 0 && dy == 0) { px = 1; py = 0; }
    else if (std::abs(dx) >= std::abs(dy)) { px = 0; py = 1; }
    else { px = 1; py = 0; }

    uint16_t tp = (uint16_t)(draw_env_.texpage_raw & 0xFFFF);
    uint8_t semi_mode = (uint8_t)((tp >> 5) & 3);
    uint8_t flags = semi ? (uint8_t)2 : (uint8_t)0;

    // Triangle 1: (x0-px, y0-py) (x0+px, y0+py) (x1+px, y1+py)
    push_triangle(
        (int16_t)(x0 - px), (int16_t)(y0 - py), r0, g0, b0, 0, 0,
        (int16_t)(x0 + px), (int16_t)(y0 + py), r0, g0, b0, 0, 0,
        (int16_t)(x1 + px), (int16_t)(y1 + py), r1, g1, b1, 0, 0,
        0, tp, flags, semi_mode, 0);
    // Triangle 2: (x0-px, y0-py) (x1+px, y1+py) (x1-px, y1-py)
    push_triangle(
        (int16_t)(x0 - px), (int16_t)(y0 - py), r0, g0, b0, 0, 0,
        (int16_t)(x1 + px), (int16_t)(y1 + py), r1, g1, b1, 0, 0,
        (int16_t)(x1 - px), (int16_t)(y1 - py), r1, g1, b1, 0, 0,
        0, tp, flags, semi_mode, 0);

    emu::logf(emu::LogLevel::trace, "GPU", "GP0 LINE%s%s (%d,%d)-(%d,%d) c=%02X%02X%02X ofs=(%d,%d)",
        gouraud ? "_GOURAUD" : "_FLAT", semi ? "_SEMI" : "",
        x0, y0, x1, y1, r0, g0, b0, (int)draw_env_.offset_x, (int)draw_env_.offset_y);
}

// ---------------------------------------------------------------------------
// GP0(60h-7Fh) Rectangle → push as 2 triangles for UE5
// ---------------------------------------------------------------------------
void Gpu::gp0_rect()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const int size_code = (cmd >> 3) & 3;
    const bool textured = (cmd & 0x04) != 0;
    const bool semi     = (cmd & 0x02) != 0;
    const bool raw      = (cmd & 0x01) != 0; // raw texture (no color modulation)

    frame_stats_.rects++;

    uint8_t r = color8((uint8_t)(cmd_buf_[0] & 0xFF));
    uint8_t g = color8((uint8_t)((cmd_buf_[0] >> 8) & 0xFF));
    uint8_t b = color8((uint8_t)((cmd_buf_[0] >> 16) & 0xFF));

    int16_t x = sign_extend_11(cmd_buf_[1]);
    int16_t y = sign_extend_11((int32_t)(cmd_buf_[1] >> 16));

    uint8_t u0 = 0, v0 = 0;
    uint16_t clut = 0;
    int idx = 2;
    if (textured)
    {
        u0 = (uint8_t)(cmd_buf_[idx] & 0xFF);
        v0 = (uint8_t)((cmd_buf_[idx] >> 8) & 0xFF);
        clut = (uint16_t)((cmd_buf_[idx] >> 16) & 0xFFFF);
        idx++;
    }

    int16_t w = 0, h = 0;
    const char* size_name = "";
    switch (size_code)
    {
        case 0:
        {
            if (idx < cmd_buf_pos_)
            {
                w = (int16_t)(cmd_buf_[idx] & 0xFFFF);
                h = (int16_t)(cmd_buf_[idx] >> 16);
            }
            size_name = "VAR";
            break;
        }
        case 1: w = 1;  h = 1;  size_name = "1x1";   break;
        case 2: w = 8;  h = 8;  size_name = "8x8";   break;
        case 3: w = 16; h = 16; size_name = "16x16"; break;
    }

    if (w <= 0 || h <= 0)
        return; // degenerate rect

    // Apply drawing offset
    x = (int16_t)(x + draw_env_.offset_x);
    y = (int16_t)(y + draw_env_.offset_y);

    uint16_t tp = (uint16_t)(draw_env_.texpage_raw & 0xFFFF);
    uint8_t semi_mode = (uint8_t)((tp >> 5) & 3);
    uint8_t tex_depth = (uint8_t)((tp >> 7) & 3);
    uint8_t flags = 0;
    if (textured) flags |= 1;
    if (semi)     flags |= 2;
    if (raw)      flags |= 4;

    // Debug: log first semi-transparent rects
    if (semi)
    {
        static int semi_rect_log = 0;
        if (semi_rect_log++ < 20)
        {
            emu::logf(emu::LogLevel::warn, "GPU",
                "SEMI_RECT cmd=0x%02X textured=%d tp=0x%04X texpage_raw=0x%06X → semi_mode=%u",
                cmd, textured ? 1 : 0, tp, draw_env_.texpage_raw, semi_mode);
        }
    }

    // Rectangle corners
    int16_t x1 = (int16_t)(x + w);
    int16_t y1 = (int16_t)(y + h);
    // UV: Don't wrap here! Let the shader do fmod(uv, 256) to avoid
    // interpolation artifacts when UV crosses 255/0 boundary.
    // Store as full range values (can exceed 255).
    uint8_t u1 = static_cast<uint8_t>(std::min((int32_t)u0 + w, 255));
    uint8_t v1 = static_cast<uint8_t>(std::min((int32_t)v0 + h, 255));

    // Triangle 1: top-left, top-right, bottom-left
    push_triangle(
        x,  y,  r, g, b, u0, v0,
        x1, y,  r, g, b, u1, v0,
        x,  y1, r, g, b, u0, v1,
        clut, tp, flags, semi_mode, tex_depth);
    // Triangle 2: top-right, bottom-right, bottom-left
    push_triangle(
        x1, y,  r, g, b, u1, v0,
        x1, y1, r, g, b, u1, v1,
        x,  y1, r, g, b, u0, v1,
        clut, tp, flags, semi_mode, tex_depth);

    emu::logf(emu::LogLevel::trace, "GPU", "GP0 RECT_%s%s%s TL=(%d,%d) %dx%d c=%02X%02X%02X ofs=(%d,%d)",
        size_name, textured ? "_TEX" : "", semi ? "_SEMI" : "",
        (int)x, (int)y, (int)w, (int)h, r, g, b, (int)draw_env_.offset_x, (int)draw_env_.offset_y);
}

// ---------------------------------------------------------------------------
// GP0(80h) VRAM to VRAM copy
// ---------------------------------------------------------------------------
void Gpu::gp0_vram_to_vram()
{
    const uint32_t src_xy = cmd_buf_[1];
    const uint32_t dst_xy = cmd_buf_[2];
    const uint32_t wh = cmd_buf_[3];

    const uint16_t sx = (uint16_t)(src_xy & 0x3FFu);
    const uint16_t sy = (uint16_t)((src_xy >> 16) & 0x1FFu);
    const uint16_t dx = (uint16_t)(dst_xy & 0x3FFu);
    const uint16_t dy = (uint16_t)((dst_xy >> 16) & 0x1FFu);
    const uint32_t xs = wh & 0xFFFFu;
    const uint32_t ys = (wh >> 16) & 0xFFFFu;
    const uint16_t w = (uint16_t)((xs == 0) ? 0x400u : (((xs - 1) & 0x3FF) + 1));
    const uint16_t h = (uint16_t)((ys == 0) ? 0x200u : (((ys - 1) & 0x1FF) + 1));

    // Perform copy in VRAM
    for (uint16_t row = 0; row < h; ++row)
    {
        for (uint16_t col = 0; col < w; ++col)
        {
            const uint32_t src_addr = (((uint32_t)sy + row) % kVramHeight) * kVramWidth +
                                      (((uint32_t)sx + col) % kVramWidth);
            const uint32_t dst_addr = (((uint32_t)dy + row) % kVramHeight) * kVramWidth +
                                      (((uint32_t)dx + col) % kVramWidth);
            vram_[dst_addr] = vram_[src_addr];
        }
    }

    frame_stats_.vram_to_vram++;
    vram_write_seq_++;
    emu::logf(emu::LogLevel::debug, "GPU", "GP0 VRAM->VRAM (%u,%u)->(%u,%u) %ux%u",
        sx, sy, dx, dy, w, h);
}

// ---------------------------------------------------------------------------
// GP0(A0h) CPU to VRAM - Start (params collected)
// ---------------------------------------------------------------------------
void Gpu::gp0_cpu_to_vram_start()
{
    const uint32_t xy = cmd_buf_[1];
    const uint32_t wh = cmd_buf_[2];

    cpu_vram_x_ = (uint16_t)(xy & 0x3FFu);
    cpu_vram_y_ = (uint16_t)((xy >> 16) & 0x1FFu);
    const uint32_t xs = wh & 0xFFFFu;
    const uint32_t ys = (wh >> 16) & 0xFFFFu;
    cpu_vram_w_ = (uint16_t)((xs == 0) ? 0x400u : (((xs - 1) & 0x3FF) + 1));
    cpu_vram_h_ = (uint16_t)((ys == 0) ? 0x200u : (((ys - 1) & 0x1FF) + 1));
    cpu_vram_col_ = 0;
    cpu_vram_row_ = 0;

    const uint32_t total_pixels = (uint32_t)cpu_vram_w_ * cpu_vram_h_;
    cpu_vram_words_remaining_ = (total_pixels + 1) / 2; // 2 pixels per word

    gp0_state_ = Gp0State::receiving_vram_data;
    frame_stats_.cpu_to_vram++;

    emu::logf(emu::LogLevel::debug, "GPU", "GP0 CPU->VRAM (%u,%u) %ux%u [%u pixels, %u words]",
        cpu_vram_x_, cpu_vram_y_, cpu_vram_w_, cpu_vram_h_,
        total_pixels, cpu_vram_words_remaining_);
}

// ---------------------------------------------------------------------------
// GP0(A0h) CPU to VRAM - Pixel data word
// ---------------------------------------------------------------------------
void Gpu::gp0_cpu_to_vram_data(uint32_t v)
{
    // Each word contains 2 pixels (16-bit each)
    for (int i = 0; i < 2; ++i)
    {
        if (cpu_vram_row_ >= cpu_vram_h_)
            break;

        const uint16_t pixel = (uint16_t)(v >> (i * 16));
        const uint32_t x = ((uint32_t)cpu_vram_x_ + cpu_vram_col_) % kVramWidth;
        const uint32_t y = ((uint32_t)cpu_vram_y_ + cpu_vram_row_) % kVramHeight;
        vram_[y * kVramWidth + x] = pixel;

        cpu_vram_col_++;
        if (cpu_vram_col_ >= cpu_vram_w_)
        {
            cpu_vram_col_ = 0;
            cpu_vram_row_++;
        }
    }

    if (cpu_vram_words_remaining_ > 0)
        cpu_vram_words_remaining_--;

    vram_write_seq_++;

    if (cpu_vram_words_remaining_ == 0 || cpu_vram_row_ >= cpu_vram_h_)
        gp0_state_ = Gp0State::idle;
}

// ---------------------------------------------------------------------------
// GP0(C0h) VRAM to CPU - Start
// ---------------------------------------------------------------------------
void Gpu::gp0_vram_to_cpu_start()
{
    const uint32_t xy = cmd_buf_[1];
    const uint32_t wh = cmd_buf_[2];

    read_vram_x_ = (uint16_t)(xy & 0x3FFu);
    read_vram_y_ = (uint16_t)((xy >> 16) & 0x1FFu);
    const uint32_t xs = wh & 0xFFFFu;
    const uint32_t ys = (wh >> 16) & 0xFFFFu;
    read_vram_w_ = (uint16_t)((xs == 0) ? 0x400u : (((xs - 1) & 0x3FF) + 1));
    read_vram_h_ = (uint16_t)((ys == 0) ? 0x200u : (((ys - 1) & 0x1FF) + 1));
    read_vram_col_ = 0;
    read_vram_row_ = 0;
    vram_to_cpu_active_ = true;

    frame_stats_.vram_to_cpu++;
    emu::logf(emu::LogLevel::debug, "GPU", "GP0 VRAM->CPU (%u,%u) %ux%u",
        read_vram_x_, read_vram_y_, read_vram_w_, read_vram_h_);
}

// ---------------------------------------------------------------------------
// GP0(E1h-E6h) Environment commands
// ---------------------------------------------------------------------------
void Gpu::gp0_env_command()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const uint32_t val = cmd_buf_[0] & 0x00FFFFFFu;

    frame_stats_.env_cmds++;

    switch (cmd)
    {
        case 0xE1: // Draw mode / Texpage
        {
            draw_env_.texpage_raw = val;
            // Also update GPUSTAT bits 0-10 (draw mode)
            status_ = (status_ & ~0x7FFu) | (val & 0x7FFu);
            // Bit 15 (texture disable) from bit 11 of val
            if (val & (1u << 11))
                status_ |= (1u << 15);
            else
                status_ &= ~(1u << 15);

            emu::logf(emu::LogLevel::trace, "GPU", "GP0 ENV TEXPAGE raw=0x%06X tpx=%u tpy=%u semi=%u depth=%u dither=%u texdis=%u",
                val, val & 0xF, (val >> 4) & 1, (val >> 5) & 3, (val >> 7) & 3,
                (val >> 9) & 1, (val >> 11) & 1);
            break;
        }
        case 0xE2: // Texture window
            draw_env_.tex_window = val;
            emu::logf(emu::LogLevel::trace, "GPU", "GP0 ENV TEX_WINDOW mask=(%u,%u) off=(%u,%u)",
                val & 0x1F, (val >> 5) & 0x1F, (val >> 10) & 0x1F, (val >> 15) & 0x1F);
            break;

        case 0xE3: // Drawing area top-left
            draw_env_.clip_x1 = (uint16_t)(val & 0x3FF);
            draw_env_.clip_y1 = (uint16_t)((val >> 10) & 0x1FF);
            emu::logf(emu::LogLevel::trace, "GPU", "GP0 ENV CLIP_TL (%u,%u)",
                draw_env_.clip_x1, draw_env_.clip_y1);
            break;

        case 0xE4: // Drawing area bottom-right
            draw_env_.clip_x2 = (uint16_t)(val & 0x3FF);
            draw_env_.clip_y2 = (uint16_t)((val >> 10) & 0x1FF);
            emu::logf(emu::LogLevel::trace, "GPU", "GP0 ENV CLIP_BR (%u,%u)",
                draw_env_.clip_x2, draw_env_.clip_y2);
            break;

        case 0xE5: // Drawing offset
        {
            // 11-bit signed values
            int32_t ox = (int32_t)(val & 0x7FF);
            int32_t oy = (int32_t)((val >> 11) & 0x7FF);
            if (ox & 0x400) ox |= ~0x7FF; // Sign extend
            if (oy & 0x400) oy |= ~0x7FF;
            draw_env_.offset_x = (int16_t)ox;
            draw_env_.offset_y = (int16_t)oy;
            emu::logf(emu::LogLevel::trace, "GPU", "GP0 ENV DRAW_OFFSET (%d,%d)", ox, oy);
            break;
        }

        case 0xE6: // Mask bit setting
            draw_env_.mask_bits = (uint16_t)(val & 3);
            // Update GPUSTAT bits 11-12
            status_ = (status_ & ~(3u << 11)) | ((val & 3u) << 11);
            emu::logf(emu::LogLevel::trace, "GPU", "GP0 ENV MASK set=%u check=%u",
                val & 1, (val >> 1) & 1);
            break;
    }
}

// ---------------------------------------------------------------------------
// GP1 Write
// ---------------------------------------------------------------------------
void Gpu::gp1_write(uint32_t v)
{
    const uint8_t cmd = (uint8_t)(v >> 24);

    switch (cmd)
    {
        case 0x00: // Reset GPU
            status_ = 0x1490'2000u; // PAL default (bit 20 = 1)
            dma_dir_ = 0;
            gp0_state_ = Gp0State::idle;
            cmd_buf_pos_ = 0;
            cmd_words_needed_ = 0;
            vram_to_cpu_active_ = false;
            read_vram_x_ = read_vram_y_ = 0;
            read_vram_w_ = read_vram_h_ = 0;
            read_vram_col_ = read_vram_row_ = 0;
            cpu_vram_words_remaining_ = 0;
            draw_env_ = DrawEnv{};
            display_ = DisplayConfig{};
            emu::logf(emu::LogLevel::info, "GPU", "GP1 RESET");
            break;

        case 0x01: // Clear FIFO
            gp0_state_ = Gp0State::idle;
            cmd_buf_pos_ = 0;
            cpu_vram_words_remaining_ = 0;
            emu::logf(emu::LogLevel::debug, "GPU", "GP1 CLEAR_FIFO");
            break;

        case 0x02: // Ack IRQ1
            status_ &= ~(1u << 24);
            break;

        case 0x03: // Display enable/disable
        {
            const uint32_t off = v & 1u;
            if (off) status_ |= (1u << 23);
            else     status_ &= ~(1u << 23);
            display_.display_enabled = (off == 0);
            emu::logf(emu::LogLevel::info, "GPU", "GP1 DISPLAY %s", off ? "OFF" : "ON");
            break;
        }

        case 0x04: // DMA direction
            dma_dir_ = v & 3u;
            emu::logf(emu::LogLevel::trace, "GPU", "GP1 DMA_DIR %u", dma_dir_);
            break;

        case 0x05: // Start of display area
        {
            display_.display_x = (uint16_t)(v & 0x3FF);
            display_.display_y = (uint16_t)((v >> 10) & 0x1FF);
            emu::logf(emu::LogLevel::trace, "GPU", "GP1 DISPLAY_START (%u,%u)",
                display_.display_x, display_.display_y);
            break;
        }

        case 0x06: // Horizontal display range
        {
            display_.h_range_x1 = (uint16_t)(v & 0xFFF);
            display_.h_range_x2 = (uint16_t)((v >> 12) & 0xFFF);
            emu::logf(emu::LogLevel::trace, "GPU", "GP1 H_RANGE %u-%u",
                display_.h_range_x1, display_.h_range_x2);
            break;
        }

        case 0x07: // Vertical display range
        {
            display_.v_range_y1 = (uint16_t)(v & 0x3FF);
            display_.v_range_y2 = (uint16_t)((v >> 10) & 0x3FF);
            emu::logf(emu::LogLevel::trace, "GPU", "GP1 V_RANGE %u-%u",
                display_.v_range_y1, display_.v_range_y2);
            break;
        }

        case 0x08: // Display mode
        {
            // Bits: 0-1=H.res, 2=V.res, 3=video mode, 4=color depth, 5=interlace, 6=H.res2
            status_ = (status_ & ~0x7F4000u) | ((v & 0x3F) << 17) | ((v & 0x40) << 10);
            display_.h_res = (uint8_t)(v & 3);
            if (v & 0x40) display_.h_res = 4; // 368 mode
            display_.v_res = (uint8_t)((v >> 2) & 1);
            display_.is_pal = (v & 8) != 0;
            display_.color_24bit = (v & 0x10) != 0;
            display_.interlace = (v & 0x20) != 0;
            emu::logf(emu::LogLevel::debug, "GPU", "GP1 DISPLAY_MODE hres=%u(%u) vres=%u video=%s depth=%u interlace=%u",
                display_.h_res, display_.width(), display_.v_res,
                display_.is_pal ? "PAL" : "NTSC",
                display_.color_24bit ? 24 : 15,
                display_.interlace ? 1 : 0);
            break;
        }

        case 0x10: // Get GPU info
            // Various sub-commands; GPUREAD returns data
            break;

        default:
            emu::logf(emu::LogLevel::debug, "GPU", "GP1 cmd=0x%02X val=0x%06X", cmd, v & 0x00FFFFFFu);
            break;
    }
}

} // namespace gpu
