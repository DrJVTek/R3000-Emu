#include "core.h"

#include <new>
#include <cstdio>
#include <cstring>

// Explicit include to keep tooling in sync with Cpu API.
#include "../r3000/cpu.h"

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
    std::fprintf(stderr, "[CORE] insert_disc called: path=%s\n", path ? path : "(null)");
    std::fflush(stderr);
    if (!path || !*path)
    {
        set_err(err, err_cap, "invalid disc path");
        return false;
    }
    bool ok = cdrom_.insert_disc(path, err, err_cap);
    std::fprintf(stderr, "[CORE] insert_disc result: %d\n", ok);
    std::fflush(stderr);
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

} // namespace emu

