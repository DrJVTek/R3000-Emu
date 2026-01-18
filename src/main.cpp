#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "loader/loader.h"
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
        "  r3000_emu --load=<file> [--format=auto|elf|psxexe] [--pretty] [--log-level=..] "
        "[--log-cats=..]\n"
    );
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

    const char* load_path = arg_value(argc, argv, "--load=");
    if (!load_path)
    {
        print_usage();
        return 1;
    }

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
            std::fprintf(stderr, "Unknown --format=%s (use auto|psxexe|elf)\n", fmt_s);
            return 1;
        }
    }

    const uint32_t kRamSize = 2u * 1024u * 1024u;
    uint8_t* ram = (uint8_t*)std::calloc(1, kRamSize);
    if (!ram)
    {
        std::fprintf(stderr, "Out of memory\n");
        return 1;
    }

    loader::LoadedImage img{};
    {
        char err[256];
        err[0] = '\0';
        if (!loader::load_file_into_ram(load_path, fmt, ram, kRamSize, &img, err, sizeof(err)))
        {
            std::fprintf(stderr, "Load failed: %s\n", err[0] ? err : "unknown error");
            std::free(ram);
            return 1;
        }
    }

    r3000::Bus bus(ram, kRamSize, &logger);
    r3000::Cpu cpu(bus, &logger);
    cpu.reset(img.entry_pc);
    cpu.set_pretty(has_flag(argc, argv, "--pretty"));

    // Initialisation minimale des registres utiles.
    // - gp = r28, sp = r29
    if (img.has_gp)
        cpu.set_gpr(28, img.gp);
    if (img.has_sp)
        cpu.set_gpr(29, img.sp);
    cpu.set_pc(img.entry_pc);

    rlog::logger_logf(
        &logger, rlog::Level::info, rlog::Category::exec, "R3000 run start (PC=0x%08X)", cpu.pc()
    );

    for (;;)
    {
        const auto res = cpu.step();
        if (res.kind == r3000::Cpu::StepResult::Kind::ok)
        {
            continue;
        }

        if (res.kind == r3000::Cpu::StepResult::Kind::halted)
        {
            rlog::logger_logf(
                &logger, rlog::Level::info, rlog::Category::exec, "HALT at PC=0x%08X", res.pc
            );
            break;
        }

        if (res.kind == r3000::Cpu::StepResult::Kind::illegal_instr)
        {
            std::fprintf(stderr, "Illegal instruction at PC=0x%08X: 0x%08X\n", res.pc, res.instr);
            break;
        }

        if (res.kind == r3000::Cpu::StepResult::Kind::mem_fault)
        {
            std::fprintf(
                stderr,
                "Mem fault at PC=0x%08X addr=0x%08X kind=%d\n",
                res.pc,
                res.mem_fault.addr,
                (int)res.mem_fault.kind
            );
            break;
        }
    }

    std::free(ram);
    return 0;
}
