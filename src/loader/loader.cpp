#include "loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <codecvt>
#include <locale>
#include <string>
#endif

namespace loader
{
static std::FILE* fopen_utf8(const char* path, const char* mode)
{
    if (!path || !mode)
        return nullptr;
#if defined(_WIN32)
    // Allow non-ASCII paths on Windows (UTF-8 -> wide -> _wfopen).
    // Note: uses std::codecvt for compatibility with clangd setups where std::filesystem isn't available.
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    const std::wstring wpath = conv.from_bytes(path);
    const std::wstring wmode = conv.from_bytes(mode);
    return _wfopen(wpath.c_str(), wmode.c_str());
#else
    return std::fopen(path, mode);
#endif
}

static void set_err(char* err, size_t cap, const char* msg)
{
    if (!err || cap == 0)
        return;
    std::snprintf(err, cap, "%s", msg ? msg : "error");
}

static void set_errf(char* err, size_t cap, const char* fmt, const char* a, const char* b = nullptr)
{
    if (!err || cap == 0 || !fmt)
        return;
    if (b)
        std::snprintf(err, cap, fmt, a ? a : "", b ? b : "");
    else
        std::snprintf(err, cap, fmt, a ? a : "");
}

static uint32_t read_u32_le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16_le(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t virt_to_phys_ps1(uint32_t vaddr)
{
    // KSEG0/KSEG1 aliases on PS1.
    if ((vaddr & 0xE000'0000u) == 0x8000'0000u || (vaddr & 0xE000'0000u) == 0xA000'0000u)
    {
        return vaddr & 0x1FFF'FFFFu;
    }
    return vaddr;
}

static int load_psx_exe(
    const uint8_t* buf,
    size_t sz,
    uint8_t* ram,
    size_t ram_size,
    LoadedImage* out,
    char* err,
    size_t err_cap
)
{
    // PS-X EXE (aka "PSX EXE") : header typiquement 0x800 bytes, puis image binaire à t_addr.
    // NOTE: le format exact contient d'autres champs; on implémente le minimum utile pour le live.
    if (sz < 0x800)
    {
        set_err(err, err_cap, "PS-X EXE too small");
        return 0;
    }
    if (std::memcmp(buf, "PS-X EXE", 8) != 0)
    {
        set_err(err, err_cap, "PS-X EXE magic not found");
        return 0;
    }

    const uint32_t pc0 = read_u32_le(buf + 0x10);
    const uint32_t gp0 = read_u32_le(buf + 0x14);
    const uint32_t t_addr = read_u32_le(buf + 0x18);
    const uint32_t t_size = read_u32_le(buf + 0x1C);
    const uint32_t b_addr = read_u32_le(buf + 0x28);
    const uint32_t b_size = read_u32_le(buf + 0x2C);
    const uint32_t s_addr = read_u32_le(buf + 0x30);
    const uint32_t s_size = read_u32_le(buf + 0x34);

    if (t_size > sz - 0x800)
    {
        set_err(err, err_cap, "PS-X EXE truncated (t_size > file payload)");
        return 0;
    }

    const uint32_t t_paddr = virt_to_phys_ps1(t_addr);
    if ((size_t)t_paddr + (size_t)t_size > ram_size)
    {
        set_err(err, err_cap, "PS-X EXE load out of RAM bounds");
        return 0;
    }

    std::memcpy(ram + t_paddr, buf + 0x800, t_size);

    // BSS
    if (b_size != 0)
    {
        const uint32_t b_paddr = virt_to_phys_ps1(b_addr);
        if ((size_t)b_paddr + (size_t)b_size > ram_size)
        {
            set_err(err, err_cap, "PS-X EXE BSS out of RAM bounds");
            return 0;
        }
        std::memset(ram + b_paddr, 0, b_size);
    }

    out->entry_pc = pc0;
    out->gp = gp0;
    out->sp = (s_size != 0) ? (s_addr + s_size) : 0;
    out->has_gp = 1;
    out->has_sp = (s_size != 0) ? 1 : 0;
    return 1;
}

// ELF32 little-endian MIPS minimal loader
static int load_elf32(
    const uint8_t* buf,
    size_t sz,
    uint8_t* ram,
    size_t ram_size,
    LoadedImage* out,
    char* err,
    size_t err_cap
)
{
    if (sz < 0x34)
    {
        set_err(err, err_cap, "ELF too small");
        return 0;
    }
    if (!(buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F'))
    {
        set_err(err, err_cap, "ELF magic not found");
        return 0;
    }
    const uint8_t ei_class = buf[4];
    const uint8_t ei_data = buf[5];
    if (ei_class != 1)
    {
        set_err(err, err_cap, "ELF is not 32-bit");
        return 0;
    }
    if (ei_data != 1)
    {
        set_err(err, err_cap, "ELF is not little-endian");
        return 0;
    }

    const uint16_t e_type = read_u16_le(buf + 0x10);
    const uint16_t e_machine = read_u16_le(buf + 0x12);
    (void)e_type;
    if (e_machine != 8) // EM_MIPS
    {
        set_err(err, err_cap, "ELF is not EM_MIPS");
        return 0;
    }

    const uint32_t e_entry = read_u32_le(buf + 0x18);
    const uint32_t e_phoff = read_u32_le(buf + 0x1C);
    const uint16_t e_phentsize = read_u16_le(buf + 0x2A);
    const uint16_t e_phnum = read_u16_le(buf + 0x2C);

    if (e_phoff == 0 || e_phnum == 0)
    {
        set_err(err, err_cap, "ELF has no program headers");
        return 0;
    }
    if ((size_t)e_phoff + (size_t)e_phnum * (size_t)e_phentsize > sz)
    {
        set_err(err, err_cap, "ELF program headers out of file bounds");
        return 0;
    }

    for (uint16_t i = 0; i < e_phnum; ++i)
    {
        const uint8_t* ph = buf + e_phoff + (size_t)i * e_phentsize;
        const uint32_t p_type = read_u32_le(ph + 0x00);
        const uint32_t p_offset = read_u32_le(ph + 0x04);
        const uint32_t p_vaddr = read_u32_le(ph + 0x08);
        const uint32_t p_paddr = read_u32_le(ph + 0x0C);
        const uint32_t p_filesz = read_u32_le(ph + 0x10);
        const uint32_t p_memsz = read_u32_le(ph + 0x14);

        if (p_type != 1) // PT_LOAD
            continue;
        if ((size_t)p_offset + (size_t)p_filesz > sz)
        {
            set_err(err, err_cap, "ELF PT_LOAD outside file bounds");
            return 0;
        }

        const uint32_t vaddr = (p_paddr != 0) ? p_paddr : p_vaddr;
        const uint32_t dst = virt_to_phys_ps1(vaddr);
        if ((size_t)dst + (size_t)p_memsz > ram_size)
        {
            set_err(err, err_cap, "ELF PT_LOAD outside RAM");
            return 0;
        }

        if (p_filesz != 0)
        {
            std::memcpy(ram + dst, buf + p_offset, p_filesz);
        }
        if (p_memsz > p_filesz)
        {
            std::memset(ram + dst + p_filesz, 0, p_memsz - p_filesz);
        }
    }

    out->entry_pc = e_entry;
    out->gp = 0;
    out->sp = 0;
    out->has_gp = 0;
    out->has_sp = 0;
    return 1;
}

int load_file_into_ram(
    const char* path,
    Format fmt,
    uint8_t* ram,
    size_t ram_size,
    LoadedImage* out,
    char* err,
    size_t err_cap
)
{
    if (!path || !out || !ram || ram_size == 0)
    {
        set_err(err, err_cap, "invalid args");
        return 0;
    }

    std::FILE* f = fopen_utf8(path, "rb");
    if (!f)
    {
        set_errf(err, err_cap, "could not open '%s'", path);
        return 0;
    }

    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0)
    {
        std::fclose(f);
        set_err(err, err_cap, "empty file");
        return 0;
    }

    uint8_t* buf = (uint8_t*)std::malloc((size_t)n);
    if (!buf)
    {
        std::fclose(f);
        set_err(err, err_cap, "out of memory");
        return 0;
    }

    const size_t got = std::fread(buf, 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n)
    {
        std::free(buf);
        set_err(err, err_cap, "failed to read file");
        return 0;
    }

    LoadedImage img{};

    int ok = 0;
    if (fmt == Format::psxexe)
        ok = load_psx_exe(buf, (size_t)n, ram, ram_size, &img, err, err_cap);
    else if (fmt == Format::elf)
        ok = load_elf32(buf, (size_t)n, ram, ram_size, &img, err, err_cap);
    else
    {
        // auto detect
        if ((size_t)n >= 8 && std::memcmp(buf, "PS-X EXE", 8) == 0)
            ok = load_psx_exe(buf, (size_t)n, ram, ram_size, &img, err, err_cap);
        else if ((size_t)n >= 4 &&
                 (buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F'))
            ok = load_elf32(buf, (size_t)n, ram, ram_size, &img, err, err_cap);
        else
        {
            set_err(err, err_cap, "unknown file format (use --format=auto|psxexe|elf)");
            ok = 0;
        }
    }

    std::free(buf);
    if (!ok)
        return 0;

    *out = img;
    return 1;
}

} // namespace loader
