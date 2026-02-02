#include "core.h"

#include <new>
#include <cstdio>
#include <cstring>

// Explicit include to keep tooling in sync with Cpu API.
#include "../r3000/cpu.h"
#include "../log/emu_log.h"

namespace emu
{

static void set_errf(char* err, size_t cap, const char* fmt, const char* a = nullptr)
{
    if (!err || cap == 0)
        return;
    if (!fmt)
    {
        err[0] = '\0';
        return;
    }
    if (a)
        std::snprintf(err, cap, fmt, a);
    else
        std::snprintf(err, cap, "%s", fmt);
}

Core::Core(rlog::Logger* logger) : logger_(logger), cdrom_(logger), gpu_(logger)
{
}

Core::~Core() = default;

void Core::set_err(char* err, size_t err_cap, const char* msg) const
{
    set_errf(err, err_cap, msg);
}

bool Core::alloc_ram(uint32_t bytes, char* err, size_t err_cap)
{
    if (ram_)
    {
        set_err(err, err_cap, "RAM already allocated");
        return false;
    }
    if (bytes == 0)
    {
        set_err(err, err_cap, "invalid RAM size");
        return false;
    }
    ram_.reset(new (std::nothrow) uint8_t[bytes]{});
    if (!ram_)
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }
    ram_size_ = bytes;
    return true;
}

bool Core::set_bios_copy(const uint8_t* bios, uint32_t bios_size, char* err, size_t err_cap)
{
    bios_.reset();
    bios_size_ = 0;

    if (!bios || bios_size == 0)
        return true; // BIOS optional (load-only mode).

    bios_.reset(new (std::nothrow) uint8_t[bios_size]);
    if (!bios_)
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }
    std::memcpy(bios_.get(), bios, bios_size);
    bios_size_ = bios_size;
    return true;
}

void Core::set_log_sinks(const flog::Sink& cdlog, const flog::Sink& gpulog, const flog::Sink& syslog, const flog::Sink& iolog, const flog::Clock& clock)
{
    cdlog_ = cdlog;
    gpulog_ = gpulog;
    syslog_ = syslog;
    iolog_ = iolog;
    clock_ = clock;
    has_clock_ = 1;

    cdrom_.set_log_sinks(cdlog_, iolog_, clock_);
    gpu_.set_log_sinks(gpulog_, iolog_, clock_);
}

void Core::set_text_out(std::FILE* f)
{
    text_out_ = f;
    if (cpu_)
        cpu_->set_text_out(text_out_);
}

void Core::set_text_io_sink(const flog::Sink& s, const flog::Clock& c)
{
    text_io_ = s;
    text_clock_ = c;
    has_text_clock_ = 1;
    if (cpu_)
        cpu_->set_text_io_sink(text_io_, text_clock_);
}

bool Core::insert_disc(const char* path, char* err, size_t err_cap)
{
    emu::logf(emu::LogLevel::info, "CORE", "insert_disc: path=%s", path ? path : "(null)");
    if (!path || !*path)
    {
        set_err(err, err_cap, "invalid disc path");
        return false;
    }
    bool ok = cdrom_.insert_disc(path, err, err_cap);
    emu::logf(emu::LogLevel::info, "CORE", "insert_disc result: %d", ok);
    return ok;
}

void Core::set_gpu_dump_file(const char* path)
{
    if (path && *path)
        gpu_.set_dump_file(path);
}

void Core::set_compare_file(std::FILE* f)
{
    compare_file_ = f;
}

uint8_t* Core::ram()
{
    return ram_.get();
}

uint32_t Core::ram_size() const
{
    return ram_size_;
}

const uint8_t* Core::bios_data() const
{
    return bios_.get();
}

uint32_t Core::bios_size() const
{
    return bios_size_;
}

bool Core::init_from_image(const loader::LoadedImage& img, const InitOptions& opt, char* err, size_t err_cap)
{
    if (!ram_ || ram_size_ == 0)
    {
        set_err(err, err_cap, "RAM not allocated");
        return false;
    }

    // (Re)create bus/cpu.
    bus_.reset(new (std::nothrow) r3000::Bus(ram_.get(), ram_size_, bios_.get(), bios_size_, &cdrom_, &gpu_, logger_));
    if (!bus_)
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }
    cpu_.reset(new (std::nothrow) r3000::Cpu(*bus_, logger_));
    if (!cpu_)
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }

    // Bus tracing options (diagnostic only).
    if (has_clock_)
        bus_->set_trace_vector_sink(iolog_, clock_);
    bus_->set_trace_vectors(opt.trace_vectors ? 1 : 0);
    if (opt.watch_u32_enabled)
        bus_->set_watch_ram_u32(opt.watch_u32_phys, 1);

    cpu_->reset(img.entry_pc);

    cpu_->set_pretty(opt.pretty ? 1 : 0);
    cpu_->set_trace_io(opt.trace_io ? 1 : 0);
    cpu_->set_hle_vectors(opt.hle_vectors ? 1 : 0);

    cpu_->set_loop_detectors(opt.loop_detectors ? 1 : 0);
    cpu_->set_bus_tick_batch(opt.bus_tick_batch);
    cpu_->set_stop_on_high_ram(opt.stop_on_high_ram ? 1 : 0);
    cpu_->set_stop_on_bios_to_ram_nop(opt.stop_on_bios_to_ram_nop ? 1 : 0);
    cpu_->set_stop_on_ram_nop(opt.stop_on_ram_nop ? 1 : 0);
    if (opt.stop_on_pc_enabled)
        cpu_->set_stop_on_pc(opt.stop_on_pc, 1);

    // System/io sinks for higher-signal events.
    if (has_clock_)
        cpu_->set_sys_log_sinks(syslog_, iolog_, clock_);

    if (compare_file_)
        cpu_->set_compare_file(compare_file_);

    // Apply pending text sinks.
    if (text_out_)
        cpu_->set_text_out(text_out_);
    if (has_text_clock_ && text_io_.f)
        cpu_->set_text_io_sink(text_io_, text_clock_);

    // Apply initial registers (loader-provided).
    if (img.has_gp)
        cpu_->set_gpr(28, img.gp);
    if (img.has_sp)
        cpu_->set_gpr(29, img.sp);
    cpu_->set_pc(img.entry_pc);

    return true;
}

r3000::Cpu::StepResult Core::step()
{
    if (!cpu_)
    {
        r3000::Cpu::StepResult r{};
        r.kind = r3000::Cpu::StepResult::Kind::halted;
        return r;
    }
    return cpu_->step();
}

uint32_t Core::pc() const
{
    return cpu_ ? cpu_->pc() : 0;
}

void Core::set_gpr(uint32_t idx, uint32_t v)
{
    if (cpu_)
        cpu_->set_gpr(idx, v);
}

void Core::set_pc(uint32_t v)
{
    if (cpu_)
        cpu_->set_pc(v);
}

r3000::Bus* Core::bus()
{
    return bus_.get();
}

r3000::Cpu* Core::cpu()
{
    return cpu_.get();
}

bool Core::fast_boot_from_cd(char* err, size_t err_cap)
{
    if (!cpu_ || !bus_ || !ram_)
    {
        set_err(err, err_cap, "core not initialized");
        return false;
    }

    // 1. Read SYSTEM.CNF from the CD
    // Diagnostic: try reading PVD sector (16) directly
    {
        uint8_t pvd[2048]{};
        bool pvd_ok = cdrom_.read_sector_2048(16, pvd);
        emu::logf(emu::LogLevel::info, "CORE", "PVD sector 16 read: %s, magic=%c%c%c%c%c type=%d",
            pvd_ok ? "ok" : "FAIL",
            pvd[1], pvd[2], pvd[3], pvd[4], pvd[5], pvd[0]);
    }

    uint32_t cnf_lba = 0, cnf_size = 0;
    if (!cdrom_.iso9660_find_file("\\SYSTEM.CNF;1", &cnf_lba, &cnf_size))
    {
        set_err(err, err_cap, "SYSTEM.CNF not found on disc");
        return false;
    }

    // Read SYSTEM.CNF (usually < 2048 bytes)
    uint8_t cnf_buf[2048]{};
    if (!cdrom_.read_sector_2048(cnf_lba, cnf_buf))
    {
        set_err(err, err_cap, "failed to read SYSTEM.CNF sector");
        return false;
    }

    // Parse "BOOT = cdrom:\\<filename>;1" line
    char boot_file[128]{};
    {
        const char* cnf = reinterpret_cast<const char*>(cnf_buf);
        const char* p = std::strstr(cnf, "BOOT");
        if (!p)
        {
            set_err(err, err_cap, "BOOT entry not found in SYSTEM.CNF");
            return false;
        }
        // Skip "BOOT" and any spaces/= characters
        p += 4;
        while (*p == ' ' || *p == '\t' || *p == '=') ++p;
        // Skip "cdrom:" or "cdrom:\\" prefix
        if (std::strncmp(p, "cdrom:", 6) == 0) p += 6;
        while (*p == '\\') ++p;

        size_t i = 0;
        while (*p && *p != '\r' && *p != '\n' && *p != ';' && i < sizeof(boot_file) - 2)
            boot_file[i++] = *p++;
        boot_file[i] = '\0';
    }

    if (!boot_file[0])
    {
        set_err(err, err_cap, "empty BOOT filename in SYSTEM.CNF");
        return false;
    }

    emu::logf(emu::LogLevel::info, "CORE", "Fast boot: loading %s from CD", boot_file);

    // 2. Find the EXE file on disc
    char iso_path[140]{};
    std::snprintf(iso_path, sizeof(iso_path), "\\%s;1", boot_file);
    uint32_t exe_lba = 0, exe_size = 0;
    if (!cdrom_.iso9660_find_file(iso_path, &exe_lba, &exe_size))
    {
        set_err(err, err_cap, "boot EXE not found on disc");
        return false;
    }

    emu::logf(emu::LogLevel::info, "CORE", "EXE found: LBA=%u size=%u", exe_lba, exe_size);

    // 3. Read the full EXE into a temp buffer
    const uint32_t sector_count = (exe_size + 2047) / 2048;
    std::unique_ptr<uint8_t[]> exe_buf(new (std::nothrow) uint8_t[sector_count * 2048]{});
    if (!exe_buf)
    {
        set_err(err, err_cap, "out of memory for EXE");
        return false;
    }
    for (uint32_t s = 0; s < sector_count; ++s)
    {
        if (!cdrom_.read_sector_2048(exe_lba + s, exe_buf.get() + s * 2048))
        {
            set_err(err, err_cap, "failed to read EXE sector from disc");
            return false;
        }
    }

    // 4. Parse PS-X EXE header
    if (exe_size < 0x800 || std::memcmp(exe_buf.get(), "PS-X EXE", 8) != 0)
    {
        set_err(err, err_cap, "boot file is not a valid PS-X EXE");
        return false;
    }

    auto read_u32_le = [](const uint8_t* p) -> uint32_t {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    };

    const uint32_t pc0 = read_u32_le(exe_buf.get() + 0x10);
    const uint32_t gp0 = read_u32_le(exe_buf.get() + 0x14);
    const uint32_t t_addr = read_u32_le(exe_buf.get() + 0x18);
    const uint32_t t_size = read_u32_le(exe_buf.get() + 0x1C);
    const uint32_t b_addr = read_u32_le(exe_buf.get() + 0x28);
    const uint32_t b_size = read_u32_le(exe_buf.get() + 0x2C);
    const uint32_t s_addr = read_u32_le(exe_buf.get() + 0x30);
    const uint32_t s_size = read_u32_le(exe_buf.get() + 0x34);

    // Convert KSEG0/KSEG1 to physical
    auto virt_to_phys = [](uint32_t v) -> uint32_t {
        return v & 0x1FFF'FFFFu;
    };

    const uint32_t t_phys = virt_to_phys(t_addr);
    if ((uint64_t)t_phys + t_size > ram_size_)
    {
        set_err(err, err_cap, "EXE text segment exceeds RAM");
        return false;
    }

    // 5. Copy text segment to RAM
    std::memcpy(ram_.get() + t_phys, exe_buf.get() + 0x800, t_size);
    emu::logf(emu::LogLevel::info, "CORE", "Loaded text: 0x%08X -> phys 0x%08X (%u bytes)", t_addr, t_phys, t_size);
    emu::logf(emu::LogLevel::info, "CORE", "EXE header: BSS=0x%08X size=%u, SP=0x%08X size=%u",
        b_addr, b_size, s_addr, s_size);

    // Zero BSS segment (fast_boot was missing this!)
    if (b_size != 0)
    {
        const uint32_t b_phys = virt_to_phys(b_addr);
        if ((uint64_t)b_phys + b_size <= ram_size_)
        {
            std::memset(ram_.get() + b_phys, 0, b_size);
            emu::logf(emu::LogLevel::info, "CORE", "Zeroed BSS: 0x%08X -> phys 0x%08X (%u bytes)", b_addr, b_phys, b_size);
        }
    }

    // Diagnostic: check if 0x8007BCF4 is in loaded range
    {
        const uint32_t probe = 0x0007'BCF4u; // physical addr of 0x8007BCF4
        bool in_text = (probe >= t_phys && probe < t_phys + t_size);
        bool in_bss = (b_size != 0 && probe >= virt_to_phys(b_addr) && probe < virt_to_phys(b_addr) + b_size);
        emu::logf(emu::LogLevel::info, "CORE", "Probe 0x8007BCF4: in_text=%d in_bss=%d byte=0x%02X%02X%02X%02X",
            in_text, in_bss,
            ram_.get()[probe], ram_.get()[probe+1], ram_.get()[probe+2], ram_.get()[probe+3]);
    }

    // 6. Set CPU state
    cpu_->set_pc(pc0);
    cpu_->set_gpr(28, gp0); // GP
    if (s_size != 0)
        cpu_->set_gpr(29, s_addr + s_size); // SP = stack base + size
    else if (s_addr != 0)
        cpu_->set_gpr(29, s_addr); // SP = s_addr when size=0
    else
        cpu_->set_gpr(29, 0x801F'FF00u); // default SP

    // 7. Initialize minimal hardware state for game code

    // Enable HLE vectors so A0/B0/C0 calls + exception vector are intercepted
    cpu_->set_hle_vectors(1);

    // Set I_MASK for VBLANK + CDROM + DMA
    {
        r3000::Bus::MemFault mf{};
        bus_->write_u32(0x1F80'1074u, 0x000Du, mf); // VBLANK(0) + CDROM(2) + DMA(3)
    }

    // Set COP0 Status: IEc=1, IM2=1, IM0=1 (enable hardware + software interrupts)
    cpu_->set_cop0(12, (1u << 0) | (1u << 8) | (1u << 10)); // SR = 0x00000501

    // Initialize kernel data structures (PCB/TCB) so BIOS calls work
    {
        constexpr uint32_t kPcbAddr = 0x0200u;
        constexpr uint32_t kTcbAddr = 0x0300u;
        constexpr uint32_t kTcbSize = 0xC0u;

        std::memset(ram_.get() + kPcbAddr, 0, 0x10);
        std::memset(ram_.get() + kTcbAddr, 0, kTcbSize);

        // TCB[0x00] = 0x4000 (active flag)
        auto w32 = [&](uint32_t addr, uint32_t val) {
            std::memcpy(ram_.get() + addr, &val, 4);
        };
        w32(kTcbAddr, 0x4000u);

        // TCB[0x94] = saved Status with IEp=1 (bit 2) + IM2=1 (bit 10)
        w32(kTcbAddr + 0x94, (1u << 2) | (1u << 10));

        // PCB[0] = pointer to TCB (KSEG0)
        w32(kPcbAddr, 0x80000000u | kTcbAddr);

        // [0x108] = PCB address (KSEG0)
        w32(0x108, 0x80000000u | kPcbAddr);

        cpu_->set_hle_tcb_addr(kTcbAddr);

        emu::logf(emu::LogLevel::info, "CORE", "Kernel data: PCB=0x%X TCB=0x%X [0x108]=0x%08X",
            kPcbAddr, kTcbAddr, 0x80000000u | kPcbAddr);
    }

    // Debug: watch writes to 0x8007BCF4 (filename buffer)
    bus_->set_watch_ram_u32(0x0007'BCF4u, 1);

    emu::logf(emu::LogLevel::info, "CORE", "Fast boot: PC=0x%08X GP=0x%08X SP=0x%08X", pc0, gp0, cpu_->gpr(29));
    return true;
}

} // namespace emu

