#include "bus.h"

#include <cstdio>

namespace r3000
{

Bus::Bus(uint8_t* ram, uint32_t ram_size, rlog::Logger* logger)
    : ram_(ram), ram_size_(ram_size), logger_(logger)
{
}

uint32_t Bus::ram_size() const
{
    return ram_size_;
}

bool Bus::is_in_ram(uint32_t addr, uint32_t size) const
{
    if (addr > ram_size())
        return false;
    return (ram_size() - addr) >= size;
}

void Bus::log_mem(const char* op, uint32_t addr, uint32_t v) const
{
    if (!logger_)
        return;
    rlog::logger_logf(
        logger_, rlog::Level::trace, rlog::Category::mem, "%s addr=0x%08X v=0x%08X", op, addr, v
    );
}

bool Bus::read_u8(uint32_t addr, uint8_t& out, MemFault& fault)
{
    if (!is_in_ram(addr, 1))
    {
        fault = {MemFault::Kind::out_of_range, addr};
        return false;
    }
    out = ram_[addr];
    log_mem("RB", addr, out);
    return true;
}

bool Bus::read_u16(uint32_t addr, uint16_t& out, MemFault& fault)
{
    if ((addr & 1u) != 0u)
    {
        fault = {MemFault::Kind::unaligned, addr};
        return false;
    }
    if (!is_in_ram(addr, 2))
    {
        fault = {MemFault::Kind::out_of_range, addr};
        return false;
    }
    out = (uint16_t)ram_[addr] | (uint16_t)(ram_[addr + 1] << 8);
    log_mem("RH", addr, out);
    return true;
}

bool Bus::read_u32(uint32_t addr, uint32_t& out, MemFault& fault)
{
    if ((addr & 3u) != 0u)
    {
        fault = {MemFault::Kind::unaligned, addr};
        return false;
    }
    if (!is_in_ram(addr, 4))
    {
        fault = {MemFault::Kind::out_of_range, addr};
        return false;
    }
    out = (uint32_t)ram_[addr] | ((uint32_t)ram_[addr + 1] << 8) |
          ((uint32_t)ram_[addr + 2] << 16) | ((uint32_t)ram_[addr + 3] << 24);
    log_mem("RW", addr, out);
    return true;
}

bool Bus::write_u8(uint32_t addr, uint8_t v, MemFault& fault)
{
    if (!is_in_ram(addr, 1))
    {
        fault = {MemFault::Kind::out_of_range, addr};
        return false;
    }
    ram_[addr] = v;
    log_mem("WB", addr, v);
    return true;
}

bool Bus::write_u16(uint32_t addr, uint16_t v, MemFault& fault)
{
    if ((addr & 1u) != 0u)
    {
        fault = {MemFault::Kind::unaligned, addr};
        return false;
    }
    if (!is_in_ram(addr, 2))
    {
        fault = {MemFault::Kind::out_of_range, addr};
        return false;
    }
    ram_[addr] = (uint8_t)(v & 0xFFu);
    ram_[addr + 1] = (uint8_t)((v >> 8) & 0xFFu);
    log_mem("WH", addr, v);
    return true;
}

bool Bus::write_u32(uint32_t addr, uint32_t v, MemFault& fault)
{
    if (addr == kMmioPrintU32)
    {
        // Démo live: on "printf" côté hôte.
        // stderr pour rester visible même si stdout est spam par --pretty/logs.
        std::fprintf(stderr, "[GUEST:MMIO] %u (0x%08X)\n", v, v);
        std::fflush(stderr);
        log_mem("WMMIO", addr, v);
        return true;
    }
    if ((addr & 3u) != 0u)
    {
        fault = {MemFault::Kind::unaligned, addr};
        return false;
    }
    if (!is_in_ram(addr, 4))
    {
        fault = {MemFault::Kind::out_of_range, addr};
        return false;
    }
    ram_[addr] = (uint8_t)(v & 0xFFu);
    ram_[addr + 1] = (uint8_t)((v >> 8) & 0xFFu);
    ram_[addr + 2] = (uint8_t)((v >> 16) & 0xFFu);
    ram_[addr + 3] = (uint8_t)((v >> 24) & 0xFFu);
    log_mem("WW", addr, v);
    return true;
}

} // namespace r3000
