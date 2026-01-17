#pragma once

#include <cstdint>

#include "../log/logger.h"

namespace r3000
{

// Bus minimal: RAM plate + une MMIO fictive pour démo live.
// - Little-endian (PS1).
// - Exceptions d'alignement pour LH/LW/SH/SW.
class Bus
{
  public:
    struct MemFault
    {
        enum class Kind
        {
            out_of_range,
            unaligned,
        };
        Kind kind{};
        uint32_t addr{};
    };

    Bus(uint8_t* ram, uint32_t ram_size, rlog::Logger* logger = nullptr);

    uint32_t ram_size() const;

    // Lecture/écriture RAM/MMIO. Retourne false en cas de fault (et remplit fault).
    bool read_u8(uint32_t addr, uint8_t& out, MemFault& fault);
    bool read_u16(uint32_t addr, uint16_t& out, MemFault& fault);
    bool read_u32(uint32_t addr, uint32_t& out, MemFault& fault);

    bool write_u8(uint32_t addr, uint8_t v, MemFault& fault);
    bool write_u16(uint32_t addr, uint16_t v, MemFault& fault);
    bool write_u32(uint32_t addr, uint32_t v, MemFault& fault);

    // MMIO "print" (demo): écrire un u32 à cette adresse => affiche la valeur (décimal + hex).
    static constexpr uint32_t kMmioPrintU32 = 0x1F00'0000u;

  private:
    bool is_in_ram(uint32_t addr, uint32_t size) const;
    void log_mem(const char* op, uint32_t addr, uint32_t v) const;

    uint8_t* ram_{nullptr};
    uint32_t ram_size_{0};
    rlog::Logger* logger_{nullptr};
};

} // namespace r3000
