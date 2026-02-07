#include <cinttypes>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <direct.h>
#include <codecvt>
#include <locale>
#include <string>
#endif

#include "emu/core.h"
#include "loader/loader.h"
#include "log/emu_log.h"
#include "log/filelog.h"
#include "log/logger.h"
#include "r3000/bus.h"
#include "r3000/cpu.h"

static const char* arg_value(int argc, char** argv, const char* key_prefix)
{
    const size_t n = std::strlen(key_prefix);
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], key_prefix, n) == 0)
            return argv[i] + n;
    }
    return nullptr;
}

static int has_flag(int argc, char** argv, const char* flag)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], flag) == 0)
            return 1;
    }
    return 0;
}

static void print_usage(void)
{
    std::fprintf(
        stderr,
        "Usage:\n"
        "  r3000_emu [--bios=<bios.bin>] [--cd=<image>] [--gpu-dump=<file>] [--wav-output=<file.wav>]\n"
        "            [--max-steps=N] [--pretty] [--log-level=..] [--log-cats=..] [--emu-log-level=..]\n"
        "  r3000_emu --load=<file> [--format=auto|elf|psxexe] [--pretty] [--max-steps=N]\n"
        "\n"
        "Options:\n"
        "  --bios=<file>         Load BIOS ROM (default: bios/ps1_bios.bin)\n"
        "  --cd=<image>          Insert CD image (CUE/BIN)\n"
        "  --gpu-dump=<file>     Dump GPU commands to file\n"
        "  --wav-output=<file>   Save SPU audio to WAV file\n"
        "  --max-steps=N         Stop after N instructions\n"
        "  --max-time=N          Stop after N seconds wall clock (default: 300)\n"
        "  --load=<file>         Load ELF or PS-X EXE directly (skips BIOS)\n"
        "  --pretty              Pretty print instructions\n"
        "  --trace-io            Verbose MMIO logging\n"
        "  --pc-sample=N         Print PC every N steps\n"
        "  --bus-tick-batch=N    Tick HW every N CPU steps (1=accurate, 32=fast)\n"
        "  --stop-on-pc=ADDR     Stop when PC hits ADDR (hex ok)\n"
        "  --emu-log-level=LVL   Set emu log level (error|warn|info|debug|trace)\n"
        "  --reg-trace=START:END[:WATCH]  Trace registers in PC range, optionally watch for value\n"
        "                        Example: --reg-trace=0x8004AB00:0x8004AC00:0x35096\n"
    );
}

// CLI callback for emu::Log - writes to stderr
static void cli_log_callback(emu::LogLevel level, const char* tag, const char* msg, void* /*user*/)
{
    static const char* lvl_names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    const int idx = (int)level;
    const char* lvl_str = (idx >= 0 && idx <= 4) ? lvl_names[idx] : "???";
    std::fprintf(stderr, "[%s] [%s] %s\n", lvl_str, tag, msg);
    std::fflush(stderr);
}

static int read_file_malloc(const char* path, uint8_t** out_buf, uint32_t* out_size, char* err, size_t err_cap)
{
    if (err && err_cap)
        err[0] = '\0';
    if (!path || !out_buf || !out_size)
        return 0;

    auto fopen_utf8 = [](const char* p, const char* mode) -> std::FILE* {
        if (!p || !mode)
            return nullptr;
#if defined(_WIN32)
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        const std::wstring wpath = conv.from_bytes(p);
        const std::wstring wmode = conv.from_bytes(mode);
        return _wfopen(wpath.c_str(), wmode.c_str());
#else
        return std::fopen(p, mode);
#endif
    };

    std::FILE* f = fopen_utf8(path, "rb");
    if (!f)
    {
        if (err && err_cap)
            std::snprintf(err, err_cap, "could not open '%s'", path);
        return 0;
    }

    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0)
    {
        std::fclose(f);
        if (err && err_cap)
            std::snprintf(err, err_cap, "empty file '%s'", path);
        return 0;
    }

    uint8_t* buf = (uint8_t*)std::malloc((size_t)n);
    if (!buf)
    {
        std::fclose(f);
        if (err && err_cap)
            std::snprintf(err, err_cap, "out of memory");
        return 0;
    }

    const size_t got = std::fread(buf, 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n)
    {
        std::free(buf);
        if (err && err_cap)
            std::snprintf(err, err_cap, "failed to read '%s'", path);
        return 0;
    }

    *out_buf = buf;
    *out_size = (uint32_t)n;
    return 1;
}

static uint64_t parse_u64_or_zero(const char* s)
{
    if (!s || !*s)
        return 0;
    return std::strtoull(s, nullptr, 0);
}

static void ensure_dir_logs(void)
{
#if defined(_WIN32)
    _mkdir("logs");
#endif
}

static flog::Level parse_flog_level_or(const char* s, flog::Level fallback)
{
    if (!s || !*s)
        return fallback;
    if (std::strcmp(s, "error") == 0)
        return flog::Level::error;
    if (std::strcmp(s, "warn") == 0)
        return flog::Level::warn;
    if (std::strcmp(s, "info") == 0)
        return flog::Level::info;
    if (std::strcmp(s, "debug") == 0)
        return flog::Level::debug;
    if (std::strcmp(s, "trace") == 0)
        return flog::Level::trace;
    return fallback;
}

int main(int argc, char** argv)
{
    rlog::Logger logger{};
    rlog::logger_init(&logger, stdout);

    const char* lvl = arg_value(argc, argv, "--log-level=");
    if (lvl)
    {
        rlog::logger_set_level(&logger, rlog::parse_level(lvl));
    }

    const char* cats = arg_value(argc, argv, "--log-cats=");
    if (cats)
    {
        rlog::logger_set_cats(&logger, rlog::parse_categories_csv(cats));
    }

    // Initialize emu::Log (callback-based, UE5-ready)
    const char* emu_lvl = arg_value(argc, argv, "--emu-log-level=");
    emu::Log emu_log{};
    emu_log.cb = cli_log_callback;
    emu_log.max_level = emu::log_parse_level(emu_lvl); // defaults to info
    emu::log_init(&emu_log);

    const char* bios_path = arg_value(argc, argv, "--bios=");
    const char* load_path = arg_value(argc, argv, "--load=");
    const char* cd_path = arg_value(argc, argv, "--cd=");
    const char* gpu_dump = arg_value(argc, argv, "--gpu-dump=");
    const char* wav_output = arg_value(argc, argv, "--wav-output=");
    const int trace_io = has_flag(argc, argv, "--trace-io");

    const char* fmt_s = arg_value(argc, argv, "--format=");
    loader::Format fmt = loader::Format::auto_detect;
    if (fmt_s)
    {
        if (std::strcmp(fmt_s, "auto") == 0)
            fmt = loader::Format::auto_detect;
        else if (std::strcmp(fmt_s, "psxexe") == 0)
            fmt = loader::Format::psxexe;
        else if (std::strcmp(fmt_s, "elf") == 0)
            fmt = loader::Format::elf;
        else
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Unknown --format=%s (use auto|psxexe|elf)", fmt_s);
            return 1;
        }
    }

    const uint64_t max_steps = parse_u64_or_zero(arg_value(argc, argv, "--max-steps="));
    const uint64_t max_time_raw = parse_u64_or_zero(arg_value(argc, argv, "--max-time="));
    const uint64_t max_time_s = (max_time_raw != 0) ? max_time_raw : 300; // default 5 min
    const uint64_t pc_sample = parse_u64_or_zero(arg_value(argc, argv, "--pc-sample="));
    const uint64_t stop_on_pc = parse_u64_or_zero(arg_value(argc, argv, "--stop-on-pc="));
    const char* bus_tick_batch_s = arg_value(argc, argv, "--bus-tick-batch=");
    uint32_t bus_tick_batch = 0;
    if (bus_tick_batch_s)
    {
        uint64_t v = parse_u64_or_zero(bus_tick_batch_s);
        if (v < 1u) v = 1u;
        if (v > 128u) v = 128u;
        bus_tick_batch = (uint32_t)v;
    }

    const flog::Level hw_lvl = parse_flog_level_or(arg_value(argc, argv, "--hw-log-level="), flog::Level::info);
    const flog::Level cd_lvl = parse_flog_level_or(arg_value(argc, argv, "--cd-log-level="), hw_lvl);
    const flog::Level gpu_lvl = parse_flog_level_or(arg_value(argc, argv, "--gpu-log-level="), hw_lvl);
    const flog::Level io_lvl = parse_flog_level_or(arg_value(argc, argv, "--io-log-level="), hw_lvl);
    const flog::Level sys_lvl = parse_flog_level_or(arg_value(argc, argv, "--system-log-level="), hw_lvl);

    // Parse --reg-trace=START:END[:WATCH]
    // Example: --reg-trace=0x8004AB00:0x8004AC00:0x35096
    uint32_t reg_trace_start = 0, reg_trace_end = 0, reg_trace_watch = 0;
    const char* reg_trace_s = arg_value(argc, argv, "--reg-trace=");
    if (reg_trace_s)
    {
        // Parse format: START:END or START:END:WATCH
        char* endp = nullptr;
        reg_trace_start = (uint32_t)std::strtoul(reg_trace_s, &endp, 0);
        if (endp && *endp == ':')
        {
            reg_trace_end = (uint32_t)std::strtoul(endp + 1, &endp, 0);
            if (endp && *endp == ':')
            {
                reg_trace_watch = (uint32_t)std::strtoul(endp + 1, &endp, 0);
            }
        }
        emu::logf(emu::LogLevel::info, "MAIN", "Register trace: PC=0x%08X-0x%08X watch=0x%08X",
            reg_trace_start, reg_trace_end, reg_trace_watch);
    }

    const uint32_t kRamSize = 2u * 1024u * 1024u;
    emu::Core core(&logger);
    {
        char err[256];
        err[0] = '\0';
        if (!core.alloc_ram(kRamSize, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "RAM alloc failed: %s", err[0] ? err : "unknown error");
            return 1;
        }
    }

    // Setup log files
    ensure_dir_logs();
    const flog::Clock clock = flog::clock_start();
    std::FILE* outtext = std::fopen("logs/outtext.log", "wb");
    std::FILE* cdlog_f = std::fopen("logs/cdrom.log", "wb");
    std::FILE* gpulog_f = std::fopen("logs/gpu.log", "wb");
    std::FILE* syslog_f = std::fopen("logs/system.log", "wb");
    std::FILE* iolog_f = std::fopen("logs/io.log", "wb");

    const flog::Sink cdlog{cdlog_f, cd_lvl};
    const flog::Sink gpulog{gpulog_f, gpu_lvl};
    const flog::Sink syslog{syslog_f, sys_lvl};
    const flog::Sink iolog{iolog_f, io_lvl};

    uint8_t* bios = nullptr;
    uint32_t bios_size = 0;

    loader::LoadedImage img{};
    bool boot_bios = false;

    if (!load_path)
    {
        boot_bios = true;
        if (!bios_path)
        {
            // Default: try to find BIOS in bios/ directory
            const char* candidates[] = {
                "bios/ps1_bios.bin",
                "bios/bios.bin",
                "bios/scph1001.bin",
            };
            char err[256];
            bool ok = false;
            for (size_t i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); ++i)
            {
                bios_path = candidates[i];
                if (read_file_malloc(bios_path, &bios, &bios_size, err, sizeof(err)))
                {
                    emu::logf(emu::LogLevel::info, "MAIN", "BIOS loaded: %s (%u bytes)", bios_path, bios_size);
                    ok = true;
                    break;
                }
            }
            if (!ok)
            {
                emu::logf(emu::LogLevel::error, "MAIN", "No BIOS found. Put a BIOS in 'bios/ps1_bios.bin' or use --bios=...");
                print_usage();
                return 1;
            }
        }
        else
        {
            char err[256];
            if (!read_file_malloc(bios_path, &bios, &bios_size, err, sizeof(err)))
            {
                emu::logf(emu::LogLevel::error, "MAIN", "BIOS load failed: %s", err[0] ? err : "unknown error");
                return 1;
            }
            emu::logf(emu::LogLevel::info, "MAIN", "BIOS loaded: %s (%u bytes)", bios_path, bios_size);
        }
    }
    else
    {
        char err[256];
        err[0] = '\0';
        if (!loader::load_file_into_ram(load_path, fmt, core.ram(), core.ram_size(), &img, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Load failed: %s", err[0] ? err : "unknown error");
            return 1;
        }
    }

    if (boot_bios)
    {
        img.entry_pc = 0xBFC0'0000u;  // BIOS reset vector
        img.has_gp = 0;
        img.has_sp = 1;
        img.sp = 0x801F'FFF0u;
    }

    core.set_log_sinks(cdlog, gpulog, syslog, iolog, clock);

    if (gpu_dump)
    {
        core.set_gpu_dump_file(gpu_dump);
    }

    if (cd_path)
    {
        char err[256];
        err[0] = '\0';
        if (!core.insert_disc(cd_path, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "CD image load failed: %s", err[0] ? err : "unknown error");
        }
        else
        {
            emu::logf(emu::LogLevel::info, "MAIN", "CD inserted: %s", cd_path);
        }
    }

    if (bios)
    {
        char err[256];
        err[0] = '\0';
        if (!core.set_bios_copy(bios, bios_size, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "BIOS setup failed: %s", err[0] ? err : "unknown error");
            return 1;
        }
        std::free(bios);
        bios = nullptr;
    }

    core.set_text_out(outtext);
    core.set_text_io_sink(iolog, clock);

    emu::Core::InitOptions core_opt{};
    core_opt.pretty = has_flag(argc, argv, "--pretty") ? 1 : 0;
    core_opt.trace_io = trace_io ? 1 : 0;
    core_opt.hle_vectors = has_flag(argc, argv, "--hle") ? 1 : 0;
    if (bus_tick_batch != 0)
        core_opt.bus_tick_batch = bus_tick_batch;
    if (stop_on_pc != 0)
    {
        core_opt.stop_on_pc_enabled = 1;
        core_opt.stop_on_pc = (uint32_t)stop_on_pc;
    }

    {
        char err[256];
        err[0] = '\0';
        if (!core.init_from_image(img, core_opt, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Core init failed: %s", err[0] ? err : "unknown error");
            return 1;
        }
    }

    // Enable register trace mode if requested
    if (reg_trace_start != 0 || reg_trace_end != 0)
    {
        if (core.cpu())
        {
            core.cpu()->set_reg_trace(reg_trace_start, reg_trace_end, reg_trace_watch);
            emu::logf(emu::LogLevel::info, "MAIN", "Register trace enabled");
        }
    }

    // Enable WAV audio output if requested
    if (wav_output && core.bus())
    {
        core.bus()->enable_wav_output(wav_output);
        emu::logf(emu::LogLevel::info, "MAIN", "WAV output: %s", wav_output);
    }

    // Fast boot: skip BIOS, load game EXE directly from CD
    if (has_flag(argc, argv, "--fast-boot") && cd_path)
    {
        char err[256]{};
        if (!core.fast_boot_from_cd(err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Fast boot failed: %s", err[0] ? err : "unknown");
            return 1;
        }
    }

    emu::logf(emu::LogLevel::info, "MAIN", "Run start PC=0x%08X", core.pc());

    rlog::logger_logf(
        &logger, rlog::Level::info, rlog::Category::exec, "R3000 run start (PC=0x%08X)", core.pc()
    );

    uint64_t steps = 0;
    const auto run_start = std::chrono::steady_clock::now();
    for (;;)
    {
        const auto res = core.step();
        if (res.kind == r3000::Cpu::StepResult::Kind::ok)
        {
            ++steps;
            if (pc_sample != 0 && (steps % pc_sample) == 0)
            {
                const r3000::Cpu* cpu = core.cpu();
                const r3000::Bus* bus = core.bus();
                const uint32_t cause = cpu ? cpu->cop0(13) : 0u;
                const uint32_t status = cpu ? cpu->cop0(12) : 0u;
                const uint32_t epc = cpu ? cpu->cop0(14) : 0u;
                const uint32_t exc = (cause >> 2) & 0x1Fu;
                const uint32_t i_stat = bus ? bus->irq_stat_raw() : 0u;
                const uint32_t i_mask = bus ? bus->irq_mask_raw() : 0u;
                const uint32_t ipend = bus ? bus->irq_pending_masked() : 0u;
                emu::logf(emu::LogLevel::info, "MAIN",
                    "SAMPLE step=%" PRIu64 " PC=0x%08X INSTR=0x%08X exc=%u epc=0x%08X cause=0x%08X status=0x%08X i_stat=0x%08X i_mask=0x%08X ipend=0x%08X",
                    steps, res.pc, res.instr, exc, epc, cause, status, i_stat, i_mask, ipend);
            }
            if (max_steps != 0 && steps >= max_steps)
            {
                emu::logf(emu::LogLevel::info, "MAIN", "Stop: reached --max-steps=%" PRIu64, max_steps);
                break;
            }
            if (max_time_s != 0 && (steps & 0xFFFF) == 0)
            {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - run_start).count();
                if ((uint64_t)elapsed >= max_time_s)
                {
                    emu::logf(emu::LogLevel::info, "MAIN", "Stop: reached --max-time=%" PRIu64 "s (steps=%" PRIu64 ")", max_time_s, steps);
                    break;
                }
            }
            continue;
        }

        if (res.kind == r3000::Cpu::StepResult::Kind::halted)
        {
            emu::logf(emu::LogLevel::info, "MAIN", "HALT at PC=0x%08X", res.pc);
            break;
        }

        if (res.kind == r3000::Cpu::StepResult::Kind::illegal_instr)
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Illegal instruction at PC=0x%08X: 0x%08X (steps=%" PRIu64 ")",
                res.pc, res.instr, steps);
            break;
        }

        if (res.kind == r3000::Cpu::StepResult::Kind::mem_fault)
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Mem fault at PC=0x%08X addr=0x%08X kind=%d (steps=%" PRIu64 ")",
                res.pc, res.mem_fault.addr, (int)res.mem_fault.kind, steps);
            break;
        }
    }

    if (outtext)
        std::fclose(outtext);
    if (cdlog_f)
        std::fclose(cdlog_f);
    if (gpulog_f)
        std::fclose(gpulog_f);
    if (syslog_f)
        std::fclose(syslog_f);
    if (iolog_f)
        std::fclose(iolog_f);

    return 0;
}
