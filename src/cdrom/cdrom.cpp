#include "cdrom.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../log/emu_log.h"

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

namespace cdrom
{

static uint32_t rd_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ci_eq(char a, char b)
{
    if (a >= 'a' && a <= 'z')
        a = (char)(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z')
        b = (char)(b - 'a' + 'A');
    return a == b;
}

static int ci_starts_with(const char* s, const char* prefix)
{
    if (!s || !prefix)
        return 0;
    while (*prefix)
    {
        if (!*s)
            return 0;
        if (!ci_eq(*s, *prefix))
            return 0;
        ++s;
        ++prefix;
    }
    return 1;
}

static void normalize_iso_component(const char* in, char* out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!in)
        return;

    // Copy, upper-case, stop at '\0' or separator, strip ";<ver>".
    size_t n = 0;
    while (*in && *in != '\\' && *in != '/' && n + 1 < cap)
    {
        char c = *in++;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == ';')
            break;
        out[n++] = c;
    }
    out[n] = '\0';
}

static int iso_name_match(const char* want, const char* rec, size_t rec_len)
{
    // Compare case-insensitive, ignore version ";1" in record.
    if (!want || !rec)
        return 0;

    // Record is typically "NAME.EXT;1". We compare up to ';' or end.
    size_t r = 0;
    size_t w = 0;
    while (want[w] && r < rec_len)
    {
        const char rc = rec[r];
        if (rc == ';')
            break;
        if (!ci_eq(want[w], rc))
            return 0;
        ++w;
        ++r;
    }

    if (want[w] != '\0')
        return 0;

    // Ensure record doesn't have extra non-version chars.
    if (r < rec_len && rec[r] != ';')
        return 0;

    return 1;
}

static void cd_log(
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
    flog::vlogf(s, c, lvl, "CDROM", fmt, args);
    va_end(args);

    va_start(args, fmt);
    flog::vlogf(combined, c, lvl, "CDROM", fmt, args);
    va_end(args);
}

static void set_err(char* err, size_t cap, const char* msg)
{
    if (!err || cap == 0)
        return;
    std::snprintf(err, cap, "%s", msg ? msg : "error");
}

struct Cdrom::Disc
{
    struct File
    {
        std::FILE* f{nullptr};
        uint32_t sector_size{0}; // 2048 or 2352
        uint32_t num_sectors{0};
        uint32_t start_lba{0}; // start of this file in "disc LBA"
        char path[512]{};
    };

    struct Track
    {
        uint8_t number{0};      // 1..99, 0xAA = leadout (not stored as normal)
        uint8_t is_audio{0};    // 1 = audio, 0 = data
        uint32_t start_lba{0};  // LBA (0 == MSF 00:02:00)
        uint32_t file_index{0}; // index into files[]
    };

    File files[64]{};
    uint32_t file_count{0};

    Track tracks[100]{};
    uint32_t track_count{0};

    uint32_t disc_sectors{0};

    static int ends_with_ci(const char* s, const char* suffix)
    {
        if (!s || !suffix)
            return 0;
        const size_t ls = std::strlen(s);
        const size_t lf = std::strlen(suffix);
        if (lf > ls)
            return 0;
        const char* a = s + (ls - lf);
        for (size_t i = 0; i < lf; ++i)
        {
            const unsigned char ca = (unsigned char)a[i];
            const unsigned char cb = (unsigned char)suffix[i];
            const unsigned char la = (ca >= 'A' && ca <= 'Z') ? (unsigned char)(ca - 'A' + 'a') : ca;
            const unsigned char lb = (cb >= 'A' && cb <= 'Z') ? (unsigned char)(cb - 'A' + 'a') : cb;
            if (la != lb)
                return 0;
        }
        return 1;
    }

    static uint32_t detect_sector_size(std::FILE* f)
    {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 0)
            return 0;
        if ((n % 2352) == 0)
            return 2352;
        if ((n % 2048) == 0)
            return 2048;
        // No fallback: unknown / inconsistent.
        return 0;
    }

    static uint32_t file_num_sectors(std::FILE* f, uint32_t sector_size)
    {
        if (!f || sector_size == 0)
            return 0;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 0)
            return 0;
        return (uint32_t)((uint32_t)n / sector_size);
    }

    static void path_dirname(const char* path, char* out_dir, size_t cap)
    {
        if (!out_dir || cap == 0)
            return;
        out_dir[0] = '\0';
        if (!path)
            return;
        const char* last_slash = std::strrchr(path, '/');
        const char* last_bslash = std::strrchr(path, '\\');
        const char* p = last_slash;
        if (!p || (last_bslash && last_bslash > p))
            p = last_bslash;
        if (!p)
            return;
        const size_t n = (size_t)(p - path + 1);
        const size_t m = (n < cap - 1) ? n : (cap - 1);
        std::memcpy(out_dir, path, m);
        out_dir[m] = '\0';
    }

    static void join_path(const char* dir, const char* rel, char* out, size_t cap)
    {
        if (!out || cap == 0)
            return;
        out[0] = '\0';
        if (!rel || !*rel)
            return;
        // absolute?
        if ((rel[0] == '\\' || rel[0] == '/') || (std::strlen(rel) >= 2 && rel[1] == ':'))
        {
            std::snprintf(out, cap, "%s", rel);
            return;
        }
        if (!dir || !*dir)
        {
            std::snprintf(out, cap, "%s", rel);
            return;
        }
        std::snprintf(out, cap, "%s%s", dir, rel);
    }

    static const char* skip_ws(const char* s)
    {
        while (s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
            ++s;
        return s;
    }

    static int parse_quoted_or_token(const char* s, char* out, size_t cap, const char** out_next)
    {
        if (!out || cap == 0)
            return 0;
        out[0] = '\0';
        s = skip_ws(s);
        if (!s || !*s)
            return 0;
        if (*s == '"')
        {
            ++s;
            size_t n = 0;
            while (*s && *s != '"' && n + 1 < cap)
                out[n++] = *s++;
            out[n] = '\0';
            if (*s == '"')
                ++s;
            if (out_next)
                *out_next = s;
            return 1;
        }
        // token
        size_t n = 0;
        while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && n + 1 < cap)
            out[n++] = *s++;
        out[n] = '\0';
        if (out_next)
            *out_next = s;
        return 1;
    }

    static int parse_msf(const char* s, uint8_t* out_m, uint8_t* out_s, uint8_t* out_f)
    {
        // format "MM:SS:FF" (decimal)
        if (!s || !out_m || !out_s || !out_f)
            return 0;
        int mm = 0, ss = 0, ff = 0;
        if (std::sscanf(s, "%d:%d:%d", &mm, &ss, &ff) != 3)
            return 0;
        if (mm < 0 || ss < 0 || ff < 0)
            return 0;
        *out_m = (uint8_t)mm;
        *out_s = (uint8_t)ss;
        *out_f = (uint8_t)ff;
        return 1;
    }

    static uint32_t msf_dec_to_lba0(uint8_t m, uint8_t s, uint8_t f)
    {
        // Convertit un MSF "décimal" en secteur offset (sans -150).
        // 00:00:00 => 0.
        return ((uint32_t)m * 60u + (uint32_t)s) * 75u + (uint32_t)f;
    }

    int add_file(const char* path, char* err, size_t err_cap)
    {
        emu::logf(emu::LogLevel::debug, "CD", "add_file: path=\"%s\"", path ? path : "(null)");
        if (file_count >= (uint32_t)(sizeof(files) / sizeof(files[0])))
        {
            set_err(err, err_cap, "too many files in cue");
            return 0;
        }
        std::FILE* f = fopen_utf8(path, "rb");
        if (!f)
        {
            emu::logf(emu::LogLevel::error, "CD", "add_file: fopen FAILED for \"%s\"", path);
            set_err(err, err_cap, "could not open track file");
            return 0;
        }
        File& fi = files[file_count];
        fi.f = f;
        fi.sector_size = detect_sector_size(f);
        if (fi.sector_size == 0)
        {
            std::fclose(f);
            fi.f = nullptr;
            set_err(err, err_cap, "could not detect sector size (file is not multiple of 2048 or 2352 bytes)");
            return 0;
        }
        fi.num_sectors = file_num_sectors(f, fi.sector_size);
        fi.start_lba = disc_sectors;
        std::snprintf(fi.path, sizeof(fi.path), "%s", path);
        disc_sectors += fi.num_sectors;
        ++file_count;
        return 1;
    }

    int add_track(uint8_t number, uint8_t is_audio, uint32_t start_lba, uint32_t file_index, char* err, size_t err_cap)
    {
        (void)err;
        (void)err_cap;
        if (track_count >= (uint32_t)(sizeof(tracks) / sizeof(tracks[0])))
            return 0;
        Track& t = tracks[track_count++];
        t.number = number;
        t.is_audio = is_audio;
        t.start_lba = start_lba;
        t.file_index = file_index;
        return 1;
    }

    static Disc* open_single(const char* path, char* err, size_t err_cap)
    {
        Disc* d = (Disc*)std::calloc(1, sizeof(Disc));
        if (!d)
        {
            set_err(err, err_cap, "out of memory");
            return nullptr;
        }
        if (!d->add_file(path, err, err_cap))
        {
            std::free(d);
            return nullptr;
        }
        // Single file => single data track 1 at LBA 0
        d->add_track(1, 0, 0, 0, err, err_cap);
        return d;
    }

    static Disc* open_cue(const char* cue_path, char* err, size_t err_cap)
    {
        std::FILE* fcue = fopen_utf8(cue_path, "rb");
        if (!fcue)
        {
            set_err(err, err_cap, "could not open cue");
            return nullptr;
        }

        Disc* d = (Disc*)std::calloc(1, sizeof(Disc));
        if (!d)
        {
            std::fclose(fcue);
            set_err(err, err_cap, "out of memory");
            return nullptr;
        }

        char cue_dir[512];
        path_dirname(cue_path, cue_dir, sizeof(cue_dir));
        emu::logf(emu::LogLevel::debug, "CD", "open_cue: cue_path=\"%s\" cue_dir=\"%s\"", cue_path, cue_dir);

        uint32_t current_file = 0xFFFF'FFFFu;
        uint8_t current_track = 0;
        uint8_t current_is_audio = 0;

        char line[1024];
        while (std::fgets(line, sizeof(line), fcue))
        {
            const char* p = skip_ws(line);
            if (!p || !*p)
                continue;

            if (std::strncmp(p, "FILE", 4) == 0 || std::strncmp(p, "file", 4) == 0)
            {
                p += 4;
                char rel[512];
                const char* next = nullptr;
                if (!parse_quoted_or_token(p, rel, sizeof(rel), &next))
                    continue;
                char full[512];
                join_path(cue_dir, rel, full, sizeof(full));
                if (!d->add_file(full, err, err_cap))
                {
                    d->close();
                    std::fclose(fcue);
                    return nullptr;
                }
                current_file = d->file_count - 1;
                current_track = 0;
                current_is_audio = 0;
                continue;
            }

            if (std::strncmp(p, "TRACK", 5) == 0 || std::strncmp(p, "track", 5) == 0)
            {
                if (current_file == 0xFFFF'FFFFu)
                    continue;
                p += 5;
                int tnum = 0;
                char ttype[64];
                ttype[0] = '\0';
                // TRACK NN TYPE
                if (std::sscanf(p, "%d %63s", &tnum, ttype) != 2)
                    continue;
                if (tnum < 1 || tnum > 99)
                    continue;
                current_track = (uint8_t)tnum;
                current_is_audio = (uint8_t)((std::strncmp(ttype, "AUDIO", 5) == 0 || std::strncmp(ttype, "audio", 5) == 0) ? 1 : 0);
                continue;
            }

            if (std::strncmp(p, "INDEX", 5) == 0 || std::strncmp(p, "index", 5) == 0)
            {
                if (current_file == 0xFFFF'FFFFu || current_track == 0)
                    continue;
                p += 5;
                int idx = 0;
                char msf[64];
                msf[0] = '\0';
                if (std::sscanf(p, "%d %63s", &idx, msf) != 2)
                    continue;
                if (idx != 1)
                    continue; // on ne prend que INDEX 01 pour la TOC
                uint8_t mm = 0, ss = 0, ff = 0;
                if (!parse_msf(msf, &mm, &ss, &ff))
                    continue;

                // Dans un cue multi-file, INDEX 01 est souvent 00:00:00 pour chaque track file.
                // On construit une LBA "disc" en concaténant les fichiers dans l’ordre.
                const uint32_t file_base = d->files[current_file].start_lba;
                const uint32_t off_lba = msf_dec_to_lba0(mm, ss, ff);
                const uint32_t start_lba = file_base + off_lba;
                d->add_track(current_track, current_is_audio, start_lba, current_file, err, err_cap);
                continue;
            }
        }

        std::fclose(fcue);

        // Si aucun INDEX n’a été trouvé, on fallback track 1 sur file 0.
        if (d->track_count == 0 && d->file_count != 0)
        {
            d->add_track(1, 0, 0, 0, err, err_cap);
        }

        return d;
    }

    static Disc* open_any(const char* path, char* err, size_t err_cap)
    {
        if (ends_with_ci(path, ".cue"))
            return open_cue(path, err, err_cap);
        return open_single(path, err, err_cap);
    }

    void close()
    {
        for (uint32_t i = 0; i < file_count; ++i)
        {
            if (files[i].f)
            {
                std::fclose(files[i].f);
                files[i].f = nullptr;
            }
        }
        std::free(this);
    }

    bool read_sector_raw(uint32_t lba, uint8_t* out, uint32_t out_cap, uint32_t* out_sector_size)
    {
        if (out_sector_size)
            *out_sector_size = 0;
        if (file_count == 0 || !out)
            return false;

        // Trouver le file qui contient ce LBA
        bool found_file = false;
        for (uint32_t i = 0; i < file_count; ++i)
        {
            const File& fi = files[i];
            if (lba < fi.start_lba)
                continue;
            const uint32_t rel = lba - fi.start_lba;
            if (rel >= fi.num_sectors)
            {
                emu::logf(emu::LogLevel::debug, "CD",
                    "read_sector_raw: LBA=%u file[%u] start=%u num_sectors=%u rel=%u OUT OF RANGE (ss=%u cap=%u)",
                    lba, i, fi.start_lba, fi.num_sectors, rel, fi.sector_size, out_cap);
                continue;
            }
            found_file = true;

            const uint32_t ss = fi.sector_size;
            if (ss == 0 || out_cap < ss)
                return false;
            const long off = (long)rel * (long)ss;
            if (std::fseek(fi.f, off, SEEK_SET) != 0)
                return false;
            const size_t got = std::fread(out, 1, ss, fi.f);
            if (got != ss)
                return false;
            if (out_sector_size)
                *out_sector_size = ss;
            return true;
        }

        return false;
    }
};

Cdrom::Cdrom(rlog::Logger* logger) : logger_(logger)
{
    // status_ très simplifié.
    status_ = 0x00;
    irq_enable_ = 0x1Fu; // PSX-SPX: defaults to 1Fh (all INT1-INT5 enabled)
    irq_flags_ = 0;
    shell_close_sent_ = 0;
}

void Cdrom::set_log_sinks(const flog::Sink& cd_only, const flog::Sink& combined, const flog::Clock& clock)
{
    log_cd_ = cd_only;
    log_io_ = combined;
    clock_ = clock;
    has_clock_ = 1;

    cd_log(
        log_cd_,
        log_io_,
        clock_,
        has_clock_,
        flog::Level::info,
        "log start (cd_level=%u io_level=%u)",
        (unsigned)log_cd_.level,
        (unsigned)log_io_.level
    );
}

void Cdrom::eject_disc()
{
    if (disc_)
    {
        disc_->close();
        disc_ = nullptr;
    }
}

bool Cdrom::insert_disc(const char* path, char* err, size_t err_cap)
{
    eject_disc();
    disc_ = Disc::open_any(path, err, err_cap);
    if (!disc_)
    {
        return false;
    }

    if (logger_)
    {
        uint32_t ss = 0;
        if (disc_->file_count != 0)
            ss = disc_->files[0].sector_size;
        rlog::logger_logf(
            logger_,
            rlog::Level::info,
            rlog::Category::exec,
            "CDROM: disc inserted (files=%u tracks=%u first_sector=%u)",
            disc_->file_count,
            disc_->track_count,
            ss
        );
    }

    cd_log(
        log_cd_,
        log_io_,
        clock_,
        has_clock_,
        flog::Level::info,
        "disc inserted (files=%u tracks=%u)",
        disc_->file_count,
        disc_->track_count
    );

    // Set motor spinning status when disc is inserted.
    // PSX-SPX: status bit 1 = motor on
    status_ = 0x02u; // Motor spinning

    // Queue unsolicited INT5 (shell close) for delivery after a short delay.
    // On real PS1, the CD controller sends INT5 automatically when the lid closes.
    // The BIOS polls I_STAT for this interrupt before sending any CD commands.
    shell_close_sent_ = 0;
    if (pending_irq_type_ == 0)
    {
        pending_irq_type_ = 0x05;
        pending_irq_resp_ = status_;
        pending_irq_reason_ = 0;
        pending_irq_delay_ = 100000; // ~3ms, enough for BIOS to set up I_MASK
        shell_close_sent_ = 1;
        emu::logf(emu::LogLevel::info, "CD", "INT5 (shell close) queued on disc insert (delay=%u)", pending_irq_delay_);
    }

    return true;
}

uint8_t Cdrom::bcd_to_u8(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0Fu) * 10u + (bcd & 0x0Fu));
}

uint8_t Cdrom::u8_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

void Cdrom::push_resp(uint8_t v)
{
    const uint8_t next = (uint8_t)(resp_w_ + 1);
    if (next == resp_r_)
        return; // drop (overflow)
    resp_fifo_[resp_w_] = v;
    resp_w_ = next;
}

uint8_t Cdrom::pop_resp()
{
    if (resp_r_ == resp_w_)
        return 0;
    const uint8_t v = resp_fifo_[resp_r_];
    resp_r_ = (uint8_t)(resp_r_ + 1);
    return v;
}

void Cdrom::clear_resp()
{
    resp_r_ = resp_w_ = 0;
}

void Cdrom::push_data(const uint8_t* p, size_t n)
{
    if (!p || n == 0)
        return;
    for (size_t i = 0; i < n; ++i)
    {
        const uint16_t next = (uint16_t)(data_w_ + 1);
        if (next == data_r_)
            return; // drop
        data_fifo_[data_w_] = p[i];
        data_w_ = next;
    }
}

uint8_t Cdrom::pop_data()
{
    if (data_r_ == data_w_)
        return 0;
    const uint8_t v = data_fifo_[data_r_];
    data_r_ = (uint16_t)(data_r_ + 1);
    return v;
}

void Cdrom::clear_data()
{
    data_r_ = data_w_ = 0;
}

void Cdrom::clear_params()
{
    param_count_ = 0;
}

void Cdrom::queue_cmd_irq(uint8_t flags)
{
    // Queue IRQ for delivery after a delay matching DuckStation's
    // GetAckDelayForCommand(). Response data is already in the FIFO.
    cmd_irq_pending_ = flags;

    // Command response IRQ delay in CPU cycles.
    // Real PS1: ~25000 cycles (DuckStation reference). Since our bus ticks
    // once per instruction (not cycle-accurate), the effective delay must
    // be large enough that the response doesn't arrive while the kernel
    // exception handler is still dispatching the previous IRQ.
    // Per-command delays (DuckStation: 25000 with disc, 15000 without, 80000 for Init).
    uint32_t delay = disc_ ? 25000u : 15000u;
    if (last_cmd_ == 0x0A) // Init
        delay = 80000u;
    cmd_irq_delay_ = delay;
}

void Cdrom::set_irq(uint8_t flags)
{
    // no$psx / PSX-SPX:
    // 1F801803h.Index1 bits0-2 contain response IRQ type (INT1..INT7 as value 1..7).
    // Upper bits 5..7 read as 1.
    const uint8_t old = irq_flags_;
    irq_flags_ &= ~0x07u;
    irq_flags_ |= (flags & 0x07u);
    // Route to BUS tag so it appears in system.log
    emu::logf(emu::LogLevel::info, "BUS", "CD set_irq(%u): old=0x%02X new=0x%02X shell_sent=%d pending=%u last_cmd=0x%02X",
        (unsigned)flags, (unsigned)old, (unsigned)irq_flags_, (int)shell_close_sent_, (unsigned)pending_irq_type_, (unsigned)last_cmd_);
}

uint8_t Cdrom::status_reg() const
{
    const uint8_t idx = (uint8_t)(index_ & 3u);

    // 1F801800h:
    // 2 ADPBUSY (0=Empty)      : XA ADPCM playing (not implemented here) => 0
    // 3 PRMEMPT (1=Empty)      : Parameter FIFO empty
    // 4 PRMWRDY (0=Full)       : Parameter FIFO full
    // 5 RSLRRDY (0=Empty)      : Response FIFO empty
    // 6 DRQSTS (0=Empty)       : Data FIFO empty
    // 7 BUSYSTS (1=Busy)       : Command/param transmission busy
    const uint8_t prm_empty = (param_count_ == 0) ? (1u << 3) : 0u;
    const uint8_t prm_wrd = (param_count_ < (uint8_t)sizeof(param_fifo_)) ? (1u << 4) : 0u;
    const uint8_t resp_not_empty = (resp_r_ != resp_w_) ? (1u << 5) : 0u;
    const uint8_t data_not_empty = (data_r_ != data_w_) ? (1u << 6) : 0u;
    const uint8_t busy = (busy_ || queued_cmd_valid_) ? (1u << 7) : 0u;

    return (uint8_t)(idx | prm_empty | prm_wrd | resp_not_empty | data_not_empty | busy);
}

int Cdrom::irq_line() const
{
    // PSX-SPX: The CDROM controller's /IRQ line is active when
    // (IRQ_Flag AND IRQ_Enable) is non-zero. The line feeds into
    // the interrupt controller's I_STAT bit 2 via edge detection.
    return ((irq_flags_ & irq_enable_ & 0x1Fu) != 0) ? 1 : 0;
}

void Cdrom::try_fill_data_fifo()
{
    // PSX-SPX:
    // After ReadN/ReadS has generated INT1, software must set Want Data (Index0.Bit7),
    // then wait until DRQSTS indicates data fifo not empty.
    if (!disc_)
        return;
    if (!data_ready_pending_)
        return;
    if (!want_data_)
        return;
    if (data_r_ != data_w_)
        return; // already loaded

    uint8_t data[2048];
    emu::logf(emu::LogLevel::info, "CD", "try_fill: LBA=%u disc=%p want=%d drp=%d fifo_r=%u fifo_w=%u",
        (unsigned)loc_lba_, (void*)disc_, (int)want_data_, (int)data_ready_pending_, data_r_, data_w_);

    // Debug: dump first 64 bytes of sector data for directory reads
    if (loc_lba_ <= 20)
    {
        uint8_t raw[2352];
        uint32_t raw_ss = 0;
        if (disc_->read_sector_raw(loc_lba_, raw, sizeof(raw), &raw_ss))
        {
            emu::logf(emu::LogLevel::info, "CD",
                "RAW sector %u: ss=%u mode=%02X hdr=%02X%02X%02X%02X sub=%02X%02X%02X%02X%02X%02X%02X%02X",
                loc_lba_, raw_ss, raw[15],
                raw[12], raw[13], raw[14], raw[15],
                raw[16], raw[17], raw[18], raw[19], raw[20], raw[21], raw[22], raw[23]);
            // Dump first 32 bytes of user data (offset 24 for Mode2)
            const uint8_t* ud = raw + 24;
            emu::logf(emu::LogLevel::info, "CD",
                "User[0..31]: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
                "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                ud[0],ud[1],ud[2],ud[3],ud[4],ud[5],ud[6],ud[7],
                ud[8],ud[9],ud[10],ud[11],ud[12],ud[13],ud[14],ud[15],
                ud[16],ud[17],ud[18],ud[19],ud[20],ud[21],ud[22],ud[23],
                ud[24],ud[25],ud[26],ud[27],ud[28],ud[29],ud[30],ud[31]);
        }
    }

    if (read_user_data_2048(loc_lba_, data))
    {
        push_data(data, sizeof(data));
        emu::logf(emu::LogLevel::info, "CD", "FIFO FILLED: LBA=%u 2048 bytes", (unsigned)loc_lba_);
    }
    else
    {
        emu::logf(emu::LogLevel::warn, "CD", "FIFO FILL FAILED: LBA=%u (disc=%p)", (unsigned)loc_lba_, (void*)disc_);
    }
}

uint32_t Cdrom::msf_to_lba(uint8_t m, uint8_t s, uint8_t f) const
{
    // CD-ROM MSF => LBA.
    // LBA = (m*60 + s)*75 + f - 150 (lead-in)
    const uint32_t mm = (uint32_t)bcd_to_u8(m);
    const uint32_t ss = (uint32_t)bcd_to_u8(s);
    const uint32_t ff = (uint32_t)bcd_to_u8(f);
    const uint32_t lba = (mm * 60u + ss) * 75u + ff;
    if (lba < 150u)
        return 0;
    return lba - 150u;
}

bool Cdrom::read_user_data_2048(uint32_t lba, uint8_t out[2048])
{
    if (!disc_)
        return false;

    uint32_t ss = 0;
    // Read raw sector from disc
    uint8_t sec2352[2352];
    uint8_t sec2048[2048];

    // Try 2048 first
    if (disc_->read_sector_raw(lba, sec2048, sizeof(sec2048), &ss) && ss == 2048)
    {
        std::memcpy(out, sec2048, 2048);
        return true;
    }

    if (disc_->read_sector_raw(lba, sec2352, sizeof(sec2352), &ss) && ss == 2352)
    {
        const uint8_t mode = sec2352[15];
        if (mode == 1)
        {
            std::memcpy(out, sec2352 + 16, 2048);
            return true;
        }
        if (mode == 2)
        {
            // CD-XA Mode 2: user data at offset 24 (after sync+header+subheader*2)
            std::memcpy(out, sec2352 + 24, 2048);
            return true;
        }

        emu::logf(emu::LogLevel::warn, "CD",
            "read_user_data_2048: LBA=%u unknown mode=%u (hdr: %02X%02X%02X%02X)",
            lba, mode, sec2352[12], sec2352[13], sec2352[14], sec2352[15]);
        return false;
    }

    emu::logf(emu::LogLevel::warn, "CD", "read_user_data_2048: LBA=%u read_sector_raw failed (ss=%u)", lba, ss);
    return false;
}

bool Cdrom::read_sector_2048(uint32_t lba, uint8_t out[2048])
{
    return read_user_data_2048(lba, out);
}

static int iso_read_pvd(Cdrom* cd, uint32_t* out_root_lba, uint32_t* out_root_size)
{
    if (!cd || !out_root_lba || !out_root_size)
        return 0;
    uint8_t sec[2048];
    if (!cd->read_sector_2048(16, sec))
    {
        std::fprintf(stderr, "[WARN] [ISO] PVD: read_sector_2048(16) failed\n");
        return 0;
    }

    std::fprintf(stderr, "[INFO] [ISO] PVD sector 16: type=%02X magic='%c%c%c%c%c' ver=%02X\n",
        sec[0], sec[1], sec[2], sec[3], sec[4], sec[5], sec[6]);

    // Primary Volume Descriptor:
    // 00 type=1, 01..05 "CD001", 06 version=1
    if (sec[0] != 0x01)
        return 0;
    if (!(sec[1] == 'C' && sec[2] == 'D' && sec[3] == '0' && sec[4] == '0' && sec[5] == '1'))
        return 0;
    if (sec[6] != 0x01)
        return 0;

    // Root Directory Record at offset 156.
    const uint8_t* rr = sec + 156;
    const uint8_t rr_len = rr[0];
    if (rr_len < 34)
        return 0;

    const uint32_t extent_lba = rd_le32(rr + 2);
    const uint32_t data_len = rd_le32(rr + 10);
    *out_root_lba = extent_lba;
    *out_root_size = data_len;
    return 1;
}

static int iso_find_in_dir(
    Cdrom* cd,
    uint32_t dir_lba,
    uint32_t dir_size,
    const char* want_comp,
    uint32_t* out_lba,
    uint32_t* out_size,
    int* out_is_dir
)
{
    if (!cd || !want_comp || !out_lba || !out_size || !out_is_dir)
        return 0;

    const uint32_t sectors = (dir_size + 2047u) / 2048u;
    uint8_t sec[2048];

    uint32_t bytes_left = dir_size;
    for (uint32_t si = 0; si < sectors; ++si)
    {
        if (!cd->read_sector_2048(dir_lba + si, sec))
            return 0;

        uint32_t off = 0;
        const uint32_t lim = (bytes_left < 2048u) ? bytes_left : 2048u;
        while (off < lim)
        {
            const uint8_t len = sec[off + 0];
            if (len == 0)
            {
                // End of records in this sector.
                break;
            }
            if (off + len > lim)
            {
                // Corrupt/short record; stop scanning this sector.
                break;
            }

            const uint8_t* dr = sec + off;
            const uint32_t extent = rd_le32(dr + 2);
            const uint32_t dlen = rd_le32(dr + 10);
            const uint8_t flags = dr[25];
            const uint8_t name_len = dr[32];
            const char* name = (const char*)(dr + 33);

            // Skip special 0x00/0x01 names ('.' and '..').
            if (name_len == 1 && ((uint8_t)name[0] == 0x00 || (uint8_t)name[0] == 0x01))
            {
                off += len;
                continue;
            }

            if (iso_name_match(want_comp, name, name_len))
            {
                *out_lba = extent;
                *out_size = dlen;
                *out_is_dir = (flags & 0x02) ? 1 : 0;
                return 1;
            }

            off += len;
        }

        if (bytes_left > 2048u)
            bytes_left -= 2048u;
        else
            bytes_left = 0;
    }

    return 0;
}

bool Cdrom::iso9660_find_file(const char* path, uint32_t* out_lba, uint32_t* out_size)
{
    if (out_lba)
        *out_lba = 0;
    if (out_size)
        *out_size = 0;
    if (!path || !out_lba || !out_size)
        return false;
    if (!disc_)
        return false;

    // Strip device prefix (cdrom:).
    const char* p = path;
    if (ci_starts_with(p, "cdrom:"))
        p += 6;
    while (*p == '\\' || *p == '/')
        ++p;

    uint32_t cur_lba = 0;
    uint32_t cur_size = 0;
    if (!iso_read_pvd(this, &cur_lba, &cur_size))
    {
        cd_log(log_cd_, log_io_, clock_, has_clock_, flog::Level::warn, "ISO9660: missing/invalid PVD");
        return false;
    }

    // Traverse components.
    char comp[128];
    char want[128];
    while (*p)
    {
        // Extract component up to next separator.
        size_t n = 0;
        while (p[n] && p[n] != '\\' && p[n] != '/' && n + 1 < sizeof(comp))
        {
            comp[n] = p[n];
            ++n;
        }
        comp[n] = '\0';
        p += n;
        while (*p == '\\' || *p == '/')
            ++p;

        normalize_iso_component(comp, want, sizeof(want));
        if (!want[0])
            continue;

        uint32_t next_lba = 0;
        uint32_t next_size = 0;
        int is_dir = 0;
        if (!iso_find_in_dir(this, cur_lba, cur_size, want, &next_lba, &next_size, &is_dir))
        {
            cd_log(log_cd_, log_io_, clock_, has_clock_, flog::Level::info, "ISO9660: not found '%s' (in '%s')", want, path);
            return false;
        }

        cur_lba = next_lba;
        cur_size = next_size;

        if (*p)
        {
            if (!is_dir)
            {
                cd_log(log_cd_, log_io_, clock_, has_clock_, flog::Level::info, "ISO9660: '%s' is not a directory", want);
                return false;
            }
        }
        else
        {
            // Last component.
            if (is_dir)
            {
                cd_log(log_cd_, log_io_, clock_, has_clock_, flog::Level::info, "ISO9660: '%s' is a directory (need file)", want);
                return false;
            }
            *out_lba = cur_lba;
            *out_size = cur_size;
            cd_log(log_cd_, log_io_, clock_, has_clock_, flog::Level::info, "ISO9660: '%s' -> LBA=%u size=%u", want, (unsigned)cur_lba, (unsigned)cur_size);
            return true;
        }
    }

    return false;
}

const char* Cdrom::cmd_name(uint8_t cmd) const
{
    // Noms classiques (No$PSX). Objectif: logs lisibles.
    switch (cmd)
    {
        case 0x00:
            return "Sync";
        case 0x01:
            return "GetStat";
        case 0x02:
            return "SetLoc";
        case 0x03:
            return "Play";
        case 0x04:
            return "Forward";
        case 0x05:
            return "Backward";
        case 0x06:
            return "ReadN";
        case 0x07:
            return "MotorOn";
        case 0x08:
            return "Stop";
        case 0x09:
            return "Pause";
        case 0x0A:
            return "Init";
        case 0x0B:
            return "Mute";
        case 0x0C:
            return "Demute";
        case 0x0D:
            return "SetFilter";
        case 0x0E:
            return "SetMode";
        case 0x0F:
            return "GetParam";
        case 0x10:
            return "GetLocL";
        case 0x11:
            return "GetLocP";
        case 0x12:
            return "SetSession";
        case 0x13:
            return "GetTN";
        case 0x14:
            return "GetTD";
        case 0x15:
            return "SeekL";
        case 0x16:
            return "SeekP";
        case 0x17:
            return "SetClock";
        case 0x18:
            return "GetClock";
        case 0x19:
            return "Test";
        case 0x1A:
            return "GetID";
        case 0x1B:
            return "ReadS";
        case 0x1C:
            return "Reset";
        case 0x1D:
            return "GetQ";
        case 0x1E:
            return "ReadTOC";
        default:
            return "Unknown";
    }
}

uint8_t Cdrom::cmd_expected_params(uint8_t cmd) const
{
    // Param counts (approximatif, suffisant pour refuser les mauvais calls/diagnostiquer).
    switch (cmd)
    {
        case 0x02: // SetLoc mm ss ff
            return 3;
        case 0x0D: // SetFilter file chan
            return 2;
        case 0x0E: // SetMode mode
            return 1;
        case 0x12: // SetSession session
            return 1;
        case 0x14: // GetTD track
            return 1;
        case 0x17: // SetClock (7 bytes: year,mon,day,hour,min,sec,cent?)
            return 7;
        case 0x19: // Test subcmd + params
            return 1; // minimum
        default:
            return 0;
    }
}

void Cdrom::exec_command(uint8_t cmd)
{
    last_cmd_ = cmd;
    // Command execution (minimal, but aligned to PSX-SPX register semantics).
    //
    // IMPORTANT:
    // - Response IRQs are queued: BIOS expects INT3 (first response) then sometimes INT1/INT2.
    // - Data FIFO must be loaded only after INT1 and after Want Data (Index0.Bit7).
    clear_resp();
    // Note: do not clear data fifo here; Read commands will manage it.

    if (logger_)
    {
        rlog::logger_logf(
            logger_,
            rlog::Level::debug,
            rlog::Category::exec,
            "CDROM cmd 0x%02X (%s) params=%u",
            cmd,
            cmd_name(cmd),
            (unsigned)param_count_
        );
    }

    cd_log(
        log_cd_,
        log_io_,
        clock_,
        has_clock_,
        // Important au bring-up: on veut voir les commandes CD-ROM passer même en niveau "info".
        // Les accès MMIO bruts (polling) restent en "trace" pour éviter le spam.
        flog::Level::info,
        "CMD 0x%02X (%s) params=%u",
        cmd,
        cmd_name(cmd),
        (unsigned)param_count_
    );

    emu::logf(emu::LogLevel::info, "CD", "CMD 0x%02X (%s) params=%u", cmd, cmd_name(cmd), (unsigned)param_count_);

    const uint8_t expected = cmd_expected_params(cmd);
    if (expected != 0 && param_count_ < expected)
    {
        // Parameter count error: return error response with INT5.
        // This is real hardware behavior - commands with insufficient parameters fail.
        push_resp(status_ | 0x01u); // Error flag in status
        push_resp(0x20);            // Error code: wrong number of parameters
        queue_cmd_irq(0x05);        // INT5: error
        cd_log(
            log_cd_,
            log_io_,
            clock_,
            has_clock_,
            flog::Level::warn,
            "CMD 0x%02X parameter error: got %u, expected %u",
            cmd,
            (unsigned)param_count_,
            (unsigned)expected
        );
        clear_params();
        return;
    }

    switch (cmd)
    {
        case 0x00: // Sync
        {
            push_resp(status_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x01: // GetStat
        {
            // If shell_close not yet sent and disc is present, queue INT5
            // AFTER this GetStat response. The BIOS expects INT5 to arrive
            // after its CDROM event handlers are installed, which happens
            // after the initial Test/GetStat probing sequence.
            if (disc_ && !shell_close_sent_ && pending_irq_type_ == 0)
            {
                push_resp(status_);
                queue_cmd_irq(0x03);
                // Queue INT5 to fire after BIOS ACKs this GetStat INT3
                pending_irq_type_ = 0x05;
                pending_irq_resp_ = status_ | 0x01u; // stat with error bit
                pending_irq_reason_ = 0x08;           // shell open reason
                pending_irq_delay_ = 50000;            // ~1.5ms after ACK
                shell_close_sent_ = 1;
                emu::logf(emu::LogLevel::info, "BUS",
                    "CD INT5 (shell close) queued after GetStat (delay=%u)", pending_irq_delay_);
            }
            else
            {
                push_resp(status_);
                queue_cmd_irq(0x03);
            }
            break;
        }
        case 0x02: // SetLoc (mm ss ff)
        {
            loc_msf_[0] = param_fifo_[0];
            loc_msf_[1] = param_fifo_[1];
            loc_msf_[2] = param_fifo_[2];
            loc_lba_ = msf_to_lba(loc_msf_[0], loc_msf_[1], loc_msf_[2]);
            emu::logf(emu::LogLevel::info, "CD", "SetLoc: MSF=%02X:%02X:%02X -> LBA=%u",
                loc_msf_[0], loc_msf_[1], loc_msf_[2], loc_lba_);
            push_resp(status_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x03: // Play
        {
            push_resp(status_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x04: // Forward
        case 0x05: // Backward
        {
            push_resp(status_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x06: // ReadN
        case 0x1B: // ReadS
        {
            // Read command:
            // - First response: INT3
            // - Second response: INT1 (data ready)
            // - Continuous: after INT1 ack, auto-advance to next sector and send another INT1
            // - Data FIFO is loaded only after Want Data (Index0.Bit7).
            clear_data();
            want_data_ = 0;
            data_ready_pending_ = 0;
            read_pending_irq1_ = 1;
            reading_active_ = 1;

            // Set status: motor on (bit1) + reading (bit5)
            status_ |= 0x22u;
            push_resp(status_);
            queue_cmd_irq(0x03); // INT3 (first response)
            break;
        }
        case 0x07: // MotorOn
        {
            push_resp(status_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x08: // Stop
        {
            reading_active_ = 0;
            read_pending_irq1_ = 0;
            status_ &= ~0x20u; // clear Reading
            status_ &= ~0x02u; // clear Motor on
            push_resp(status_);
            queue_cmd_irq(0x03);
            // Stop has INT2 second response
            pending_irq_type_ = 0x02;
            pending_irq_resp_ = status_;
            pending_irq_reason_ = 0;
            pending_irq_delay_ = 80000;
            break;
        }
        case 0x09: // Pause
        {
            // First response uses status with Reading still set
            push_resp(status_);
            queue_cmd_irq(0x03);
            // Clear reading and active flags
            reading_active_ = 0;
            read_pending_irq1_ = 0;
            status_ &= ~0x20u;
            // Pause has INT2 second response
            pending_irq_type_ = 0x02;
            pending_irq_resp_ = status_;
            pending_irq_reason_ = 0;
            pending_irq_delay_ = 50000;
            break;
        }
        case 0x0A: // Init
        {
            status_ = 0x02u; // Motor on after init
            mode_ = 0x20; // default mode: double speed
            push_resp(status_);
            queue_cmd_irq(0x03);
            // Queue second response INT2 (Init complete) after first is acked
            pending_irq_type_ = 0x02;
            pending_irq_resp_ = status_;
            pending_irq_reason_ = 0;
            pending_irq_delay_ = 80000;
            break;
        }
        case 0x0B: // Mute
        case 0x0C: // Demute
        {
            push_resp(status_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x0D: // SetFilter (file, chan)
        {
            filter_file_ = param_fifo_[0];
            filter_chan_ = param_fifo_[1];
            push_resp(status_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x0E: // SetMode
        {
            mode_ = param_fifo_[0];
            emu::logf(emu::LogLevel::info, "CD", "SetMode: 0x%02X (ss=%s xa=%d speed=%s)",
                mode_,
                (mode_ & 0x20) ? "2340" : "2048",
                (mode_ >> 3) & 1,
                (mode_ & 0x80) ? "2x" : "1x");
            push_resp(status_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x0F: // GetParam
        {
            push_resp(status_);
            push_resp(mode_);
            push_resp(filter_file_);
            push_resp(filter_chan_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x10: // GetLocL
        {
            // Très simplifié: renvoyer LBA en BCD (mm ss ff) basé sur loc_msf_
            push_resp(status_);
            push_resp(loc_msf_[0]);
            push_resp(loc_msf_[1]);
            push_resp(loc_msf_[2]);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x11: // GetLocP
        {
            // GetLocP returns position from Q subchannel:
            // - Track number (BCD)
            // - Index (BCD, usually 01)
            // - Relative MM:SS:FF within track (BCD)
            // - Absolute MM:SS:FF (BCD)
            //
            // We compute relative position by finding the current track and subtracting its start.
            uint8_t track_bcd = 0x01;
            uint8_t index_bcd = 0x01;
            uint8_t rel_mm = 0x00;
            uint8_t rel_ss = 0x00;
            uint8_t rel_ff = 0x00;

            if (disc_ && disc_->track_count != 0)
            {
                // Find which track contains loc_lba_
                uint32_t track_start_lba = 0;
                uint8_t track_num = 1;
                for (uint32_t i = 0; i < disc_->track_count; ++i)
                {
                    if (disc_->tracks[i].start_lba <= loc_lba_)
                    {
                        // Check if this is the best match (highest start <= loc_lba_)
                        if (disc_->tracks[i].start_lba >= track_start_lba)
                        {
                            track_start_lba = disc_->tracks[i].start_lba;
                            track_num = disc_->tracks[i].number;
                        }
                    }
                }
                track_bcd = u8_to_bcd(track_num);

                // Relative position = current LBA - track start LBA
                const uint32_t rel_lba = (loc_lba_ >= track_start_lba) ? (loc_lba_ - track_start_lba) : 0;
                const uint32_t rel_frames = rel_lba;
                const uint32_t mm = rel_frames / (60u * 75u);
                const uint32_t rem = rel_frames % (60u * 75u);
                const uint32_t ss = rem / 75u;
                const uint32_t ff = rem % 75u;
                rel_mm = u8_to_bcd((uint8_t)mm);
                rel_ss = u8_to_bcd((uint8_t)ss);
                rel_ff = u8_to_bcd((uint8_t)ff);
            }

            push_resp(track_bcd);   // Track
            push_resp(index_bcd);   // Index
            push_resp(rel_mm);      // Relative MM
            push_resp(rel_ss);      // Relative SS
            push_resp(rel_ff);      // Relative FF
            push_resp(loc_msf_[0]); // Absolute MM
            push_resp(loc_msf_[1]); // Absolute SS
            push_resp(loc_msf_[2]); // Absolute FF
            queue_cmd_irq(0x03);
            break;
        }
        case 0x12: // SetSession
        {
            // SetSession(session) - PS1 retail discs are single-session.
            // Multi-session discs (e.g., Photo CD) are rare and not supported by most games.
            //
            // Behavior:
            // - Session 1: accepted (standard data session)
            // - Session > 1: return error INT5 (unsupported, not emulated)
            const uint8_t session = param_fifo_[0];
            if (session == 0x01)
            {
                // Session 1 is always valid
                push_resp(status_);
                queue_cmd_irq(0x03); // INT3: command accepted
                // Note: real hardware would send INT2 after seek completes.
                // For simplicity, we treat it as instant.
            }
            else
            {
                // Multi-session not supported - return explicit error
                push_resp(status_ | 0x01u); // Error flag in status
                push_resp(0x10);            // Error code: invalid parameter
                queue_cmd_irq(0x05);              // INT5: error
                cd_log(
                    log_cd_,
                    log_io_,
                    clock_,
                    has_clock_,
                    flog::Level::warn,
                    "SetSession(%u) rejected: multi-session not supported",
                    (unsigned)session
                );
            }
            break;
        }
        case 0x13: // GetTN
        {
            // Return first/last track numbers (1..99)
            uint8_t first = 1;
            uint8_t last = 1;
            if (disc_ && disc_->track_count != 0)
            {
                first = 99;
                last = 1;
                for (uint32_t i = 0; i < disc_->track_count; ++i)
                {
                    const uint8_t n = disc_->tracks[i].number;
                    if (n >= 1 && n <= 99)
                    {
                        if (n < first)
                            first = n;
                        if (n > last)
                            last = n;
                    }
                }
                if (first == 99)
                    first = 1;
            }
            push_resp(status_);
            push_resp(u8_to_bcd(first));
            push_resp(u8_to_bcd(last));
            queue_cmd_irq(0x03);
            break;
        }
        case 0x14: // GetTD
        {
            // Track start time MSF (BCD). Param: track number (BCD usually).
            uint8_t trk = param_fifo_[0];
            trk = bcd_to_u8(trk);

            uint32_t start_lba = 0;
            uint32_t end_lba = 0;

            if (disc_ && disc_->disc_sectors != 0)
            {
                end_lba = disc_->disc_sectors;
            }

            if (disc_ && disc_->track_count != 0)
            {
                // track 0xAA = leadout
                if (trk == 0xAA)
                {
                    start_lba = end_lba;
                }
                else
                {
                    for (uint32_t i = 0; i < disc_->track_count; ++i)
                    {
                        if (disc_->tracks[i].number == trk)
                        {
                            start_lba = disc_->tracks[i].start_lba;
                            break;
                        }
                    }
                }
            }

            // Convert LBA -> MSF (add 150 sectors) then BCD
            const uint32_t abs = start_lba + 150u;
            const uint32_t mm = abs / (60u * 75u);
            const uint32_t rem = abs % (60u * 75u);
            const uint32_t ss = rem / 75u;
            const uint32_t ff = rem % 75u;

            push_resp(status_);
            push_resp(u8_to_bcd((uint8_t)mm));
            push_resp(u8_to_bcd((uint8_t)ss));
            push_resp(u8_to_bcd((uint8_t)ff));
            queue_cmd_irq(0x03);
            break;
        }
        case 0x15: // SeekL
        case 0x16: // SeekP
        {
            // SeekL: Seek to location set by SetLoc (data mode, uses sector header)
            // SeekP: Seek to location set by SetLoc (audio mode, uses Q subchannel)
            //
            // Real hardware behavior:
            // - INT3 (command accepted)
            // - Motor seeks to target position (takes time)
            // - INT2 (seek complete)
            //
            // Emulator simplification: instant seek.
            // This is acceptable because:
            // - We correctly update the read head position (loc_lba_/loc_msf_)
            // - Games poll for INT2 completion, which we signal immediately
            // - Cycle-accurate seek timing is not required for correct emulation
            //
            // The position was already set by SetLoc, so we just signal completion.
            push_resp(status_);
            queue_cmd_irq(0x02); // INT2: seek complete (not INT3)
            break;
        }
        case 0x17: // SetClock
        {
            // SetClock: Set the real-time clock (7 params: year, month, day, hour, min, sec, 1/100s)
            //
            // The PS1 CD-ROM drive has an internal RTC used by some games/apps.
            // We do not emulate the RTC - return explicit error.
            push_resp(status_ | 0x01u); // Error flag
            push_resp(0x40);            // Error code: command not available
            queue_cmd_irq(0x05);              // INT5: error
            cd_log(
                log_cd_,
                log_io_,
                clock_,
                has_clock_,
                flog::Level::warn,
                "SetClock rejected: RTC not emulated"
            );
            break;
        }
        case 0x18: // GetClock
        {
            // GetClock: Read the real-time clock
            //
            // Returns 8 bytes: stat, year, month, day, hour, min, sec, 1/100s
            // We do not emulate the RTC - return explicit error.
            push_resp(status_ | 0x01u); // Error flag
            push_resp(0x40);            // Error code: command not available
            queue_cmd_irq(0x05);              // INT5: error
            cd_log(
                log_cd_,
                log_io_,
                clock_,
                has_clock_,
                flog::Level::warn,
                "GetClock rejected: RTC not emulated"
            );
            break;
        }
        case 0x19: // Test
        {
            // Test command with subcodes (PSX-SPX documentation):
            // - 0x04: Get SCEx counters (stat,counters)
            // - 0x05: Get SCEx counters (stat,counters)
            // - 0x20: Get CDROM BIOS date (yy,mm,dd,ver) e.g. "94/09/19" version C
            // - 0x22: Get region char (stat,region)
            // - 0x03: Force Motor Off
            const uint8_t subcmd = param_fifo_[0];

            switch (subcmd)
            {
                case 0x03: // Force Motor Off
                    push_resp(status_);
                    queue_cmd_irq(0x03);
                    break;
                case 0x04: // Get SCEx counters
                case 0x05:
                    // Return stat + 2 counter bytes (simplified: zeros)
                    push_resp(status_);
                    push_resp(0x00);
                    push_resp(0x00);
                    queue_cmd_irq(0x03);
                    break;
                case 0x20: // Get CDROM BIOS date
                    // Return: yy, mm, dd, version (4 bytes, no stat)
                    // Per PSX-SPX and DuckStation: 10 Jan 1997 version C2 for PU-18
                    push_resp(0x97);  // year (97 = 1997)
                    push_resp(0x01);  // month (01 = January)
                    push_resp(0x10);  // day (10)
                    push_resp(0xC2);  // version C2 (PU-18)
                    queue_cmd_irq(0x03);
                    break;
                case 0x22: // Get region char
                    // Return stat + region letter: 'I'=Japan, 'A'=America, 'E'=Europe
                    push_resp(status_);
                    push_resp('E'); // Europe
                    queue_cmd_irq(0x03);
                    break;
                default:
                    // Unknown subcmd: return stat only
                    push_resp(status_);
                    queue_cmd_irq(0x03);
                    break;
            }
            // NOTE: Test command has no async second response.
            // After INT3 is ACKed, BIOS should continue immediately.
            break;
        }
        case 0x1A: // GetID
        {
            // GetID: Identify disc type and region.
            //
            // Two-part response:
            // 1. INT3: stat (command accepted)
            // 2. INT2: stat, flags, type, atip, "SCEx" (disc ID string)
            //
            // Response format (8 bytes total):
            // [0] stat
            // [1] flags: bit3=ings data mode, bit4=ings audio mode, bit7=unlicensed
            // [2] disc type: 0x00=CD-DA, 0x20=CD-ROM (Mode1 or Mode2/XA)
            // [3] ATIP: 0x00 (not used on PS1)
            // [4..7] Region string: "SCEI" (Japan), "SCEA" (US), "SCEE" (Europe), or 0x00 (unlicensed)
            //
            // For a licensed disc with valid data track, we return SCEE (Europe) by default.
            // If no disc is inserted, return error.
            if (!disc_)
            {
                // No disc - return error
                push_resp(status_ | 0x01u); // Error flag
                push_resp(0x80);            // Error: no disc
                queue_cmd_irq(0x05);              // INT5: error
                break;
            }

            // Check if disc has a data track (track 1 is data)
            uint8_t has_data_track = 0;
            if (disc_->track_count > 0 && disc_->tracks[0].is_audio == 0)
            {
                has_data_track = 1;
            }

            // First response: INT3 (acknowledge)
            push_resp(status_);
            queue_cmd_irq(0x03);

            // Second response: INT2 (result) delivered async after delay.
            // 8 bytes: stat, flags, type, atip, region[4]
            pending_irq_type_ = 0x02;  // INT2
            pending_irq_resp_ = status_;
            pending_irq_reason_ = 0;
            pending_irq_delay_ = 50000; // ~1.5ms
            pending_irq_extra_len_ = 0;
            if (has_data_track)
            {
                pending_irq_extra_[pending_irq_extra_len_++] = 0x02;  // flags: licensed data
                pending_irq_extra_[pending_irq_extra_len_++] = 0x00;  // disc type
                pending_irq_extra_[pending_irq_extra_len_++] = 0x00;  // ATIP
                pending_irq_extra_[pending_irq_extra_len_++] = 'S';   // Region "SCEE"
                pending_irq_extra_[pending_irq_extra_len_++] = 'C';
                pending_irq_extra_[pending_irq_extra_len_++] = 'E';
                pending_irq_extra_[pending_irq_extra_len_++] = 'E';
            }
            else
            {
                pending_irq_extra_[pending_irq_extra_len_++] = 0x90;  // flags: unlicensed
                pending_irq_extra_[pending_irq_extra_len_++] = 0x00;
                pending_irq_extra_[pending_irq_extra_len_++] = 0x00;
                pending_irq_extra_[pending_irq_extra_len_++] = 0x00;
                pending_irq_extra_[pending_irq_extra_len_++] = 0x00;
                pending_irq_extra_[pending_irq_extra_len_++] = 0x00;
                pending_irq_extra_[pending_irq_extra_len_++] = 0x00;
            }
            break;
        }
        case 0x1C: // Reset
        {
            status_ = 0x00;
            mode_ = 0;
            filter_file_ = 0;
            filter_chan_ = 0;
            clear_data();
            clear_resp();
            push_resp(status_);
            queue_cmd_irq(0x03);
            break;
        }
        case 0x1D: // GetQ
        {
            // GetQ: Read Q subchannel data from current position.
            //
            // Q subchannel format (Mode 1, standard audio/data):
            // [0] Control/ADR: upper 4 bits = control, lower 4 bits = ADR (usually 1)
            //     Control: bit2 = data track (1) or audio (0)
            // [1] Track number (BCD)
            // [2] Index (BCD, usually 01)
            // [3] Relative minute (BCD)
            // [4] Relative second (BCD)
            // [5] Relative frame (BCD)
            // [6] Zero
            // [7] Absolute minute (BCD)
            // [8] Absolute second (BCD)
            // [9] Absolute frame (BCD)
            //
            // We construct this from our current position and track info.
            uint8_t ctrl_adr = 0x01; // ADR=1 (standard position)
            uint8_t track_bcd = 0x01;
            uint8_t index_bcd = 0x01;
            uint8_t rel_mm = 0x00;
            uint8_t rel_ss = 0x00;
            uint8_t rel_ff = 0x00;
            uint8_t abs_mm = loc_msf_[0];
            uint8_t abs_ss = loc_msf_[1];
            uint8_t abs_ff = loc_msf_[2];

            if (disc_ && disc_->track_count != 0)
            {
                // Find current track
                uint32_t track_start_lba = 0;
                uint8_t track_num = 1;
                uint8_t is_audio = 0;

                for (uint32_t i = 0; i < disc_->track_count; ++i)
                {
                    if (disc_->tracks[i].start_lba <= loc_lba_)
                    {
                        if (disc_->tracks[i].start_lba >= track_start_lba)
                        {
                            track_start_lba = disc_->tracks[i].start_lba;
                            track_num = disc_->tracks[i].number;
                            is_audio = disc_->tracks[i].is_audio;
                        }
                    }
                }

                // Control nibble: bit2 = data track
                ctrl_adr = (uint8_t)(0x01u | (is_audio ? 0x00u : 0x40u));
                track_bcd = u8_to_bcd(track_num);

                // Relative position within track
                const uint32_t rel_lba = (loc_lba_ >= track_start_lba) ? (loc_lba_ - track_start_lba) : 0;
                const uint32_t mm = rel_lba / (60u * 75u);
                const uint32_t rem = rel_lba % (60u * 75u);
                const uint32_t ss = rem / 75u;
                const uint32_t ff = rem % 75u;
                rel_mm = u8_to_bcd((uint8_t)mm);
                rel_ss = u8_to_bcd((uint8_t)ss);
                rel_ff = u8_to_bcd((uint8_t)ff);
            }

            push_resp(ctrl_adr);   // [0] Control/ADR
            push_resp(track_bcd);  // [1] Track
            push_resp(index_bcd);  // [2] Index
            push_resp(rel_mm);     // [3] Relative MM
            push_resp(rel_ss);     // [4] Relative SS
            push_resp(rel_ff);     // [5] Relative FF
            push_resp(0x00);       // [6] Zero
            push_resp(abs_mm);     // [7] Absolute MM
            push_resp(abs_ss);     // [8] Absolute SS
            push_resp(abs_ff);     // [9] Absolute FF
            queue_cmd_irq(0x03);
            break;
        }
        case 0x1E: // ReadTOC
        {
            // ReadTOC: Re-read the Table of Contents from disc.
            //
            // This command causes the drive to seek to the lead-in area and re-read the TOC.
            // It's used after disc swap or to verify disc integrity.
            //
            // Real hardware behavior:
            // - INT3 (command accepted)
            // - Motor seeks to lead-in (takes time)
            // - TOC is read from Q subchannel in lead-in
            // - INT2 (TOC read complete)
            //
            // Since we already have the TOC loaded from the CUE/ISO file during insert_disc(),
            // we don't need to re-read anything. We just signal completion.
            //
            // If no disc is inserted, return error.
            if (!disc_)
            {
                push_resp(status_ | 0x01u); // Error flag
                push_resp(0x80);            // Error: no disc
                queue_cmd_irq(0x05);              // INT5: error
                break;
            }

            // TOC is already loaded - signal completion.
            // Real hardware sends INT3 then INT2; we simplify to just INT2.
            push_resp(status_);
            queue_cmd_irq(0x02); // INT2: command complete (not INT3)
            cd_log(
                log_cd_,
                log_io_,
                clock_,
                has_clock_,
                flog::Level::info,
                "ReadTOC complete (tracks=%u)",
                (disc_ ? disc_->track_count : 0u)
            );
            break;
        }
        default:
        {
            push_resp(status_);
            // Unknown/unimplemented command: return error (INT5).
            // This avoids "pretending success" for missing hardware behavior.
            queue_cmd_irq(0x05);
            break;
        }
    }

    clear_params();

    cd_log(
        log_cd_,
        log_io_,
        clock_,
        has_clock_,
        flog::Level::info,
        "DONE irq_flags=0x%02X irq_en=0x%02X status=0x%02X resp_r=%u resp_w=%u data_r=%u data_w=%u",
        (unsigned)irq_flags_,
        (unsigned)irq_enable_,
        (unsigned)status_reg(),
        (unsigned)resp_r_,
        (unsigned)resp_w_,
        (unsigned)data_r_,
        (unsigned)data_w_
    );
}

uint8_t Cdrom::mmio_read8(uint32_t addr)
{
    const uint32_t off = addr - 0x1F80'1800u;
    uint8_t out = 0;
    switch (off & 3u)
    {
        case 0: // Status
            out = status_reg();
            emu::logf(emu::LogLevel::trace, "CD", "STATUS read: 0x%02X (idx=%u irq=0x%02X busy=%d queued=%d resp_r=%u resp_w=%u)",
                out, index_, irq_flags_, busy_, queued_cmd_valid_, resp_r_, resp_w_);
            break;
        case 1: // Response FIFO (R) (mirrors for Index0,2,3)
            out = pop_resp();
            emu::logf(emu::LogLevel::trace, "CD", "RESP read: 0x%02X (r=%u w=%u irq=0x%02X)",
                out, resp_r_, resp_w_, irq_flags_);
            break;
        case 2: // Data FIFO (R) (Index0..3) 8-bit
            out = pop_data();
            {
                static uint32_t data_read_count = 0;
                ++data_read_count;
                if (data_read_count <= 5 || (data_read_count & 0x7FF) == 0)
                    emu::logf(emu::LogLevel::info, "CD", "DATA_READ #%u = 0x%02X (fifo_r=%u fifo_w=%u)",
                        data_read_count, out, data_r_, data_w_);
            }
            break;
        case 3:
            // 1F801803h banked read:
            // - Index0/2: Interrupt Enable Register (R)
            // - Index1/3: Interrupt Flag Register (R/W)
            if ((index_ & 1u) == 0u)
            {
                // bits5-7 usually read as 1
                out = (uint8_t)(irq_enable_ | 0xE0u);
            }
            else
            {
                // bits5-7 read as 1, bit4 = Command Ready (1 when not busy)
                const uint8_t cmd_ready = (busy_ || queued_cmd_valid_) ? 0u : (1u << 4);
                out = (uint8_t)((irq_flags_ & 0x1Fu) | cmd_ready | 0xE0u);
                emu::logf(emu::LogLevel::trace, "CD", "IRQ_FLAG read: 0x%02X (irq_flags=0x%02X)", out, irq_flags_);
            }
            break;
    }
    cd_log(
        log_cd_,
        log_io_,
        clock_,
        has_clock_,
        flog::Level::trace,
        "MMIO R8 addr=0x%08X -> 0x%02X (idx=%u)",
        addr,
        (unsigned)out,
        (unsigned)index_
    );
    return out;
}

void Cdrom::mmio_write8(uint32_t addr, uint8_t v)
{
    cd_log(
        log_cd_,
        log_io_,
        clock_,
        has_clock_,
        flog::Level::trace,
        "MMIO W8 addr=0x%08X val=0x%02X (idx=%u)",
        addr,
        (unsigned)v,
        (unsigned)index_
    );
    const uint32_t off = addr - 0x1F80'1800u;
    switch (off & 3u)
    {
        case 0: // Index/Status write
            index_ = v & 3u;
            break;
        case 1:
            // 1F801801h:
            // - Index0: Command (W)
            // - Index3: Audio volume RR (W)
            // - Index1/2: unused/other
            if (index_ == 0)
            {
                emu::logf(emu::LogLevel::info, "CD", "CMD_WRITE: 0x%02X (%s) irq=0x%02X busy=%d queued=%d param_count=%d",
                    v, cmd_name(v), irq_flags_, busy_, queued_cmd_valid_, param_count_);
                // If there are pending cdrom interrupts, they must be acknowledged before sending a command.
                // Otherwise, BUSYSTS may stay set (PSX-SPX).
                if ((irq_flags_ & 0x1Fu) != 0u || busy_)
                {
                    queued_cmd_ = v;
                    queued_cmd_valid_ = 1;
                    queued_param_count_ = param_count_;
                    if (queued_param_count_ > (uint8_t)sizeof(queued_params_))
                        queued_param_count_ = (uint8_t)sizeof(queued_params_);
                    for (uint8_t i = 0; i < queued_param_count_; ++i)
                        queued_params_[i] = param_fifo_[i];
                    busy_ = 1;
                }
                else
                {
                    busy_ = 1;
                    exec_command(v);
                    busy_ = 0;
                }
            }
            else if (index_ == 3)
            {
                vol_rr_ = v;
            }
            break;
        case 2:
            // 1F801802h:
            // - Index0: Parameter FIFO (W)
            // - Index1: Interrupt Enable (W)
            // - Index2: Audio volume LL (W)
            // - Index3: Audio volume RL (W)
            if (index_ == 0)
            {
                if (param_count_ < (uint8_t)sizeof(param_fifo_))
                {
                    param_fifo_[param_count_++] = v;
                }
            }
            else if (index_ == 1)
            {
                const uint8_t old_enable = irq_enable_;
                irq_enable_ = v & 0x1Fu;
                emu::logf(emu::LogLevel::info, "CD", "IRQ_ENABLE: old=0x%02X new=0x%02X", old_enable, irq_enable_);
                emu::logf(emu::LogLevel::trace, "CD", "IRQ check: disc=%p irq_en=0x%02X old_en=0x%02X irq_flags=0x%02X shell_close_sent=%d",
                    (void*)disc_, irq_enable_, old_enable, irq_flags_, (int)shell_close_sent_);

                // INT5 (Shell Close / Disc Change):
                // When BIOS enables INT5 (bit 4) and a disc is present, send INT5.
                // This signals that the shell/lid is closed and the disc is ready.
                // The BIOS waits for this interrupt before proceeding with disc access.
                const uint8_t int5_bit = (1u << 4); // INT5 enable is bit 4
                const bool int5_newly_enabled = ((irq_enable_ & int5_bit) != 0) && ((old_enable & int5_bit) == 0);
                // INT5 (Shell Close): when BIOS first enables INT5 and disc is present,
                // send it immediately. This is early in boot, before CDROM is in I_MASK,
                // so the BIOS will poll it directly (no IRQ handler needed).
                // INT5 is now queued from GetStat (cmd 0x01) instead of IRQ_ENABLE.
                // The BIOS installs its CDROM event handlers between the Test probe
                // and GetStat, so INT5 must arrive after GetStat to be dispatched.
            }
            else if (index_ == 2)
            {
                vol_ll_ = v;
            }
            else if (index_ == 3)
            {
                vol_rl_ = v;
            }
            break;
        case 3:
            // 1F801803h:
            // - Index0: Request Register (W) (SMEN/BFRD)
            // - Index1/3: Interrupt Flag Register (R/W) (ack/reset)
            // - Index2: Audio volume LR (W) ; Index3: apply changes via bit5
            if (index_ == 0)
            {
                request_ = v;
                // Want Data bit7:
                const uint8_t new_want = (v & 0x80u) ? 1u : 0u;
                want_data_ = new_want;
                emu::logf(emu::LogLevel::info, "CD",
                    "Request reg write=0x%02X want_data=%d data_ready_pending=%d fifo_r=%u fifo_w=%u",
                    v, (int)want_data_, (int)data_ready_pending_, (unsigned)data_r_, (unsigned)data_w_);
                if (!want_data_)
                {
                    // Reset Data FIFO
                    clear_data();
                }
                else
                {
                    try_fill_data_fifo();
                }
            }
            else if (index_ == 1 || index_ == 3)
            {
                // Acknowledge/reset IRQ flags:
                // - write 07h to reset response bits
                // - write 1Fh to reset all IRQ bits (0..4)
                const uint8_t old_flags = irq_flags_ & 0x1Fu;
                const uint8_t m = v & 0x1Fu;
                irq_flags_ &= (uint8_t)~m;
                cd_log(log_cd_, log_io_, clock_, has_clock_, flog::Level::info,
                    "IRQ_ACK: write=0x%02X old=0x%02X new=0x%02X status=0x%02X shell_sent=%d disc=%d",
                    v, old_flags, (irq_flags_ & 0x1Fu), status_reg(), (int)shell_close_sent_, disc_ ? 1 : 0);
                emu::logf(emu::LogLevel::info, "CD",
                    "IRQ_ACK: write=0x%02X old=0x%02X new=0x%02X read_pend=%d queued=%d",
                    v, old_flags, (irq_flags_ & 0x1Fu), (int)read_pending_irq1_, (int)queued_cmd_valid_);

                // Special bits:
                if (v & 0x40u)
                {
                    // Reset Parameter FIFO
                    clear_params();
                }

                // NOTE: Do NOT clear response FIFO on IRQ acknowledge.
                // The response bytes must remain available for reading after ACK.
                // Response FIFO is cleared only when a new command starts (in execute_cmd).

                // If we just acknowledged the first response (INT3) of a read command,
                // defer INT1 (data ready) to a future tick so the IRQ line has a
                // proper low→high edge that the bus can detect.
                if (read_pending_irq1_ && ((old_flags & 0x07u) != 0u) && ((irq_flags_ & 0x07u) == 0u))
                {
                    read_pending_irq1_ = 0;
                    data_ready_pending_ = 1;
                    pending_irq_type_ = 0x01; // INT1 (data ready)
                    pending_irq_resp_ = status_;
                    pending_irq_reason_ = 0;
                    pending_irq_delay_ = 5000; // small delay for edge detection
                    try_fill_data_fifo();
                }
                // ReadN/ReadS continuous: after INT1 is acked, queue next sector read.
                // Don't advance loc_lba_ yet - the current sector data must remain
                // available for DMA3. The advance happens when pending_irq fires in tick().
                else if (reading_active_ && ((old_flags & 0x07u) == 0x01u) && ((irq_flags_ & 0x07u) == 0u))
                {
                    // Queue next INT1 after read delay. loc_lba_ will be advanced
                    // when this pending IRQ fires in tick().
                    // Use reason=0xFF as a marker for "continuous read advance needed"
                    pending_irq_type_ = 0x01; // INT1 (next sector ready)
                    pending_irq_resp_ = status_;
                    pending_irq_reason_ = 0xFFu; // marker: advance sector on delivery
                    // Real PS1 read timing: ~6.7ms per sector (single speed) or ~3.3ms (double speed)
                    // At ~33MHz CPU: single=~220000 cycles, double=~110000 cycles
                    // mode_ bit 7 = double speed
                    pending_irq_delay_ = (mode_ & 0x80u) ? 110000u : 220000u;
                    emu::logf(emu::LogLevel::info, "CD",
                        "ReadN continuous: queued next INT1, current LBA=%u delay=%u", loc_lba_, pending_irq_delay_);
                }

                // If async status is pending and INT3 was just acknowledged,
                // defer INT1 delivery for proper edge detection.
                if (async_stat_pending_ && ((old_flags & 0x07u) != 0u) && ((irq_flags_ & 0x07u) == 0u))
                {
                    async_stat_pending_ = 0;
                    pending_irq_type_ = 0x01; // INT1 (status update)
                    pending_irq_resp_ = status_;
                    pending_irq_reason_ = 0;
                    pending_irq_delay_ = 5000;
                    emu::logf(emu::LogLevel::debug, "CD", "Deferred async INT1 (status=0x%02X)", status_);
                }

                // If IRQ flags are now clear and shell close INT5 hasn't been sent yet,
                // queue it for async delivery. On real hardware the drive sends INT5
                // asynchronously; we delay it so the CPU returns to its polling loop
                // with interrupts enabled and can take the exception properly.
                // INT5 shell close: do NOT send here (IRQ_ACK path).
                // The BIOS enables CDROM in I_MASK later, and a late INT5 would crash
                // because the BIOS event handler isn't installed for unsolicited IRQs.
                // INT5 is sent on disc insert (via IRQ_ENABLE path) instead.

                // If there is a queued command and no pending IRQ flags (and no
                // deferred IRQ waiting), start it now. The queued command will
                // produce its own IRQ via queue_cmd_irq, which has a delay.
                if (queued_cmd_valid_ && ((irq_flags_ & 0x1Fu) == 0u) && pending_irq_type_ == 0)
                {
                    // Restore queued params into the parameter fifo (as if they were written before cmd).
                    clear_params();
                    param_count_ = queued_param_count_;
                    for (uint8_t i = 0; i < param_count_; ++i)
                        param_fifo_[i] = queued_params_[i];
                    queued_cmd_valid_ = 0;
                    busy_ = 1;
                    exec_command(queued_cmd_);
                    busy_ = 0;
                }
                else if (!queued_cmd_valid_)
                {
                    busy_ = 0;
                }
            }
            else if (index_ == 2)
            {
                vol_lr_ = v;
            }
            else if (index_ == 3)
            {
                // Apply audio volume changes (bit5=1).
                // Not needed for boot; kept for completeness.
            }
            break;
    }
}

void Cdrom::tick(uint32_t cycles)
{
    // Deliver command response IRQ after delay (like DuckStation MINIMUM_INTERRUPT_DELAY).
    // Response data is already in the FIFO; this just sets irq_flags.
    if (cmd_irq_pending_ != 0)
    {
        if (cmd_irq_delay_ > 0)
        {
            if (cycles >= cmd_irq_delay_)
                cmd_irq_delay_ = 0;
            else
                cmd_irq_delay_ -= cycles;
        }
        if (cmd_irq_delay_ == 0)
        {
            set_irq(cmd_irq_pending_);
            cmd_irq_pending_ = 0;
        }
    }

    // Deliver pending async IRQs after delay expires.
    if (pending_irq_type_ != 0)
    {
        // Count down delay (if any remaining).
        if (pending_irq_delay_ > 0)
        {
            if (cycles >= pending_irq_delay_)
                pending_irq_delay_ = 0;
            else
                pending_irq_delay_ -= cycles;
        }

        // Once delay has elapsed, deliver when irq_flags are clear.
        if (pending_irq_delay_ == 0 && (irq_flags_ & 0x1Fu) == 0u)
        {
            // For continuous ReadN/ReadS: advance sector before delivering INT1
            const uint8_t is_read_advance = (pending_irq_reason_ == 0xFFu) ? 1u : 0u;
            if (is_read_advance)
            {
                loc_lba_++;
                clear_data();
                want_data_ = 0;
                data_ready_pending_ = 1;
                pending_irq_reason_ = 0; // clear marker before pushing resp
                emu::logf(emu::LogLevel::info, "CD",
                    "ReadN auto-advance: LBA now %u", loc_lba_);
            }

            clear_resp();
            push_resp(pending_irq_resp_);
            if (pending_irq_reason_ != 0)
                push_resp(pending_irq_reason_);
            for (uint8_t i = 0; i < pending_irq_extra_len_; ++i)
                push_resp(pending_irq_extra_[i]);
            pending_irq_extra_len_ = 0;
            set_irq(pending_irq_type_);
            emu::logf(emu::LogLevel::info, "CD", "Async IRQ%u delivered (resp=0x%02X reason=0x%02X)",
                (unsigned)pending_irq_type_, (unsigned)pending_irq_resp_, (unsigned)pending_irq_reason_);
            cd_log(log_cd_, log_io_, clock_, has_clock_, flog::Level::info,
                "Async IRQ%u delivered (resp=0x%02X reason=0x%02X)",
                (unsigned)pending_irq_type_, (unsigned)pending_irq_resp_, (unsigned)pending_irq_reason_);
            pending_irq_type_ = 0;
            pending_irq_reason_ = 0;
        }
    }

    // Deliver pending read INT1 after delay.
    // (ReadN/ReadS second response is also async on real hardware.)
}

} // namespace cdrom

