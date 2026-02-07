#include "gpu.h"
#include "../log/emu_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <codecvt>
#include <locale>
#include <string>
#endif

static std::FILE* fopen_utf8(const char* path, const char* mode)
{
    if (!path || !mode)
        return nullptr;
#if defined(_WIN32)
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    const std::wstring wpath = conv.from_bytes(path);
    const std::wstring wmode = conv.from_bytes(mode);
    return _wfopen(wpath.c_str(), wmode.c_str());
#else
    return std::fopen(path, mode);
#endif
}

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
    status_ = 0x1490'2000u; // PAL default (bit 20 = 1) — matches SCPH-7502 hardware
    dma_dir_ = 0;
    vblank_div_ = 0;
    std::memset(vram_.get(), 0, kVramPixels * sizeof(uint16_t));
}

// ---------------------------------------------------------------------------
// VBlank
// ---------------------------------------------------------------------------
int Gpu::tick_vblank(uint32_t cycles)
{
    vblank_div_ += cycles;

    uint32_t vblank_start = kVblankPeriodCycles - kVblankDuration;
    in_vblank_ = (vblank_div_ >= vblank_start);

    if (vblank_div_ >= kVblankPeriodCycles)
    {
        vblank_div_ = 0;
        in_vblank_ = false;

        // Log frame stats
        frame_count_++;

        // Log every 50 VBlanks (~1 second) to confirm timing
        if ((frame_count_ % 50) == 1)
        {
            emu::logf(emu::LogLevel::info, "GPU", "VBlank #%u (every 50 = ~1sec at 50Hz)", frame_count_);
        }

        const auto& s = frame_stats_;
        if (s.total_words > 0)
        {
            emu::logf(emu::LogLevel::debug, "GPU", "FRAME #%u: %u tri, %u quad, %u rect, %u line, %u fill, "
                "%u v2v, %u c2v, %u v2c, %u env | %u words",
                frame_count_, s.triangles, s.quads, s.rects, s.lines, s.fills,
                s.vram_to_vram, s.cpu_to_vram, s.vram_to_cpu, s.env_cmds, s.total_words);
        }
        frame_stats_.reset();

        return 1; // Signal VBlank IRQ
    }
    return 0;
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
            if (in_vblank_) v |= (1u << 31);

            return v;
        }
    }
    return 0;
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
            // Polyline already counted on start
            return;
        }
        // Otherwise it's another vertex (or color+vertex) - just consume
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
    emu::logf(emu::LogLevel::trace, "GPU", "GP0 FILL (%u,%u) %ux%u color=%06X", x, y, w, h, color);
}

// ---------------------------------------------------------------------------
// GP0(20h-3Fh) Polygon
// ---------------------------------------------------------------------------
void Gpu::gp0_polygon()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const bool gouraud  = (cmd & 0x10) != 0;
    const bool quad     = (cmd & 0x08) != 0;
    const bool textured = (cmd & 0x04) != 0;
    const bool semi     = (cmd & 0x02) != 0;
    const int verts = quad ? 4 : 3;

    if (quad) frame_stats_.quads++;
    else      frame_stats_.triangles++;

    // Parse vertex positions for logging
    // Layout depends on flags - extract first vertex XY at minimum
    const uint32_t color0 = cmd_buf_[0] & 0x00FFFFFFu;

    // Build log string
    char buf[256];
    int pos = std::snprintf(buf, sizeof(buf), "GP0 %s%s%s%s c=%06X",
        quad ? "QUAD" : "TRI",
        gouraud ? "_GOURAUD" : "_FLAT",
        textured ? "_TEX" : "",
        semi ? "_SEMI" : "",
        color0);

    // Parse vertices for compact log
    int idx = 1; // Start after cmd word
    for (int v = 0; v < verts && idx < cmd_buf_pos_; ++v)
    {
        if (v > 0 && gouraud && idx < cmd_buf_pos_)
            idx++; // Skip color word for gouraud vertices > 0

        if (idx < cmd_buf_pos_)
        {
            int16_t vx = (int16_t)(cmd_buf_[idx] & 0xFFFF);
            int16_t vy = (int16_t)(cmd_buf_[idx] >> 16);
            pos += std::snprintf(buf + pos, sizeof(buf) - pos, " v%d=(%d,%d)", v, vx, vy);
            idx++;
        }

        if (textured && idx < cmd_buf_pos_)
            idx++; // Skip UV word
    }

    emu::logf(emu::LogLevel::trace, "GPU", "%s", buf);
}

// ---------------------------------------------------------------------------
// GP0(40h-5Fh) Line (single)
// ---------------------------------------------------------------------------
void Gpu::gp0_line()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const bool gouraud = (cmd & 0x10) != 0;
    const bool semi    = (cmd & 0x02) != 0;
    const uint32_t color = cmd_buf_[0] & 0x00FFFFFFu;

    frame_stats_.lines++;

    int16_t x0 = (int16_t)(cmd_buf_[1] & 0xFFFF);
    int16_t y0 = (int16_t)(cmd_buf_[1] >> 16);
    int idx = gouraud ? 3 : 2;
    int16_t x1 = (idx < cmd_buf_pos_) ? (int16_t)(cmd_buf_[idx] & 0xFFFF) : 0;
    int16_t y1 = (idx < cmd_buf_pos_) ? (int16_t)(cmd_buf_[idx] >> 16) : 0;

    emu::logf(emu::LogLevel::trace, "GPU", "GP0 LINE%s%s (%d,%d)-(%d,%d) c=%06X",
        gouraud ? "_GOURAUD" : "_FLAT", semi ? "_SEMI" : "",
        x0, y0, x1, y1, color);
}

// ---------------------------------------------------------------------------
// GP0(60h-7Fh) Rectangle
// ---------------------------------------------------------------------------
void Gpu::gp0_rect()
{
    const uint8_t cmd = (uint8_t)(cmd_buf_[0] >> 24);
    const int size_code = (cmd >> 3) & 3;
    const bool textured = (cmd & 0x04) != 0;
    const bool semi     = (cmd & 0x02) != 0;
    const uint32_t color = cmd_buf_[0] & 0x00FFFFFFu;

    frame_stats_.rects++;

    int16_t x = (int16_t)(cmd_buf_[1] & 0xFFFF);
    int16_t y = (int16_t)(cmd_buf_[1] >> 16);

    int w = 0, h = 0;
    const char* size_name = "";
    switch (size_code)
    {
        case 0: // Variable
        {
            int wh_idx = textured ? 3 : 2;
            if (wh_idx < cmd_buf_pos_)
            {
                w = cmd_buf_[wh_idx] & 0xFFFF;
                h = cmd_buf_[wh_idx] >> 16;
            }
            size_name = "VAR";
            break;
        }
        case 1: w = 1;  h = 1;  size_name = "1x1";   break;
        case 2: w = 8;  h = 8;  size_name = "8x8";   break;
        case 3: w = 16; h = 16; size_name = "16x16"; break;
    }

    emu::logf(emu::LogLevel::trace, "GPU", "GP0 RECT_%s%s%s (%d,%d) %dx%d c=%06X",
        size_name, textured ? "_TEX" : "", semi ? "_SEMI" : "",
        x, y, w, h, color);
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
            emu::logf(emu::LogLevel::info, "GPU", "GP1 DISPLAY %s", off ? "OFF" : "ON");
            break;
        }

        case 0x04: // DMA direction
            dma_dir_ = v & 3u;
            emu::logf(emu::LogLevel::trace, "GPU", "GP1 DMA_DIR %u", dma_dir_);
            break;

        case 0x05: // Start of display area
        {
            uint32_t dx = v & 0x3FF;
            uint32_t dy = (v >> 10) & 0x1FF;
            emu::logf(emu::LogLevel::trace, "GPU", "GP1 DISPLAY_START (%u,%u)", dx, dy);
            break;
        }

        case 0x06: // Horizontal display range
        {
            uint32_t x1 = v & 0xFFF;
            uint32_t x2 = (v >> 12) & 0xFFF;
            emu::logf(emu::LogLevel::trace, "GPU", "GP1 H_RANGE %u-%u", x1, x2);
            break;
        }

        case 0x07: // Vertical display range
        {
            uint32_t y1 = v & 0x3FF;
            uint32_t y2 = (v >> 10) & 0x3FF;
            emu::logf(emu::LogLevel::trace, "GPU", "GP1 V_RANGE %u-%u", y1, y2);
            break;
        }

        case 0x08: // Display mode
        {
            // Bits: 0-1=H.res, 2=V.res, 3=video mode, 4=color depth, 5=interlace, 6=H.res2
            status_ = (status_ & ~0x7F4000u) | ((v & 0x3F) << 17) | ((v & 0x40) << 10);
            emu::logf(emu::LogLevel::debug, "GPU", "GP1 DISPLAY_MODE hres=%u vres=%u video=%s depth=%u interlace=%u",
                v & 3, (v >> 2) & 1, (v & 8) ? "PAL" : "NTSC",
                (v >> 4) & 1, (v >> 5) & 1);
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
