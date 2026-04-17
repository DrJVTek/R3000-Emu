// ────────────────────────────────────────────────────────────────────────────
//  hle_bios.cpp — HLE BIOS bring-up extracted from Cpu::step() (Phase 1C-2)
// ────────────────────────────────────────────────────────────────────────────
//
// Emulates the PS1 kernel entry vectors A0 / B0 / C0 during bring-up, when
// the real BIOS stubs are not yet installed in RAM (or, for the
// text_hle_gate, to capture printf/putchar even after real stubs are up).
//
// Call protocol (from Cpu::step()):
//   const int is_bios_vector_pc = (pc_ == 0xA0 || 0xB0 || 0xC0);
//   const int text_hle_gate     = text_hle_ && is_bios_vector_pc;
//   int       hle_vec_gate      = 0;
//   if (hle_vectors_ && is_bios_vector_pc) { /* gate iff RAM[pc_]==0 */ }
//
//   if (hle_vec_gate || text_hle_gate) {
//       if (handle_hle_bios(r, hle_vec_gate, text_hle_gate))
//           return r;
//   }
//
// The method returns:
//   true  — HLE handled the call.  r.kind / r.instr set, gpr_[2]=v0,
//           pc_=ra.  Caller must return r immediately.
//   false — HLE did NOT handle the call (text_hle_gate-only path for a
//           function other than A(3Fh)/B(3Dh); the real BIOS stub must run).
//           No CPU state mutated by this method in the fallthrough case.
//
// Design note: nested lambdas, static locals, and state reads/writes are
// preserved verbatim from the original inline block in Cpu::step().  This
// is a pure move — zero semantic change.
// ────────────────────────────────────────────────────────────────────────────

#include "cpu.h"

#include <cinttypes>
#include <cstdio>

#include "../cdrom/cdrom.h"
#include "../log/emu_log.h"
#include "cpu_helpers.h" // text_push_char

namespace r3000
{

bool Cpu::handle_hle_bios(StepResult& r, int hle_vec_gate, int text_hle_gate)
{
    auto read_u8_guest = [&](uint32_t vaddr, uint8_t& out) -> int
    {
        Bus::MemFault f{};
        const uint32_t paddr = virt_to_phys(vaddr);
        return bus_.read_u8(paddr, out, f) ? 1 : 0;
    };
    auto write_u8_guest = [&](uint32_t vaddr, uint8_t v) -> int
    {
        Bus::MemFault f{};
        const uint32_t paddr = virt_to_phys(vaddr);

        return bus_.write_u8(paddr, v, f) ? 1 : 0;
    };
    auto write_u32_guest = [&](uint32_t vaddr, uint32_t v) -> int
    {
        // Little-endian write.
        return write_u8_guest(vaddr + 0, (uint8_t)(v & 0xFFu)) && write_u8_guest(vaddr + 1, (uint8_t)((v >> 8) & 0xFFu)) &&
               write_u8_guest(vaddr + 2, (uint8_t)((v >> 16) & 0xFFu)) && write_u8_guest(vaddr + 3, (uint8_t)((v >> 24) & 0xFFu));
    };
    auto read_u32_guest = [&](uint32_t vaddr, uint32_t& out) -> int
    {
        Bus::MemFault f{};
        const uint32_t paddr = virt_to_phys(vaddr);
        return bus_.read_u32(paddr, out, f) ? 1 : 0;
    };
    auto read_cstr_guest = [&](uint32_t vaddr, char* dst, uint32_t cap) -> uint32_t
    {
        if (!dst || cap == 0)
            return 0;
        uint32_t n = 0;
        while (n + 1 < cap)
        {
            uint8_t ch = 0;
            if (!read_u8_guest(vaddr + n, ch))
                break;
            dst[n++] = (char)ch;
            if (ch == 0)
                break;
        }
        dst[(n < cap) ? n : (cap - 1)] = 0;
        return n;
    };

    // MMIO helpers (phys addresses).
    auto mmio_read_u16 = [&](uint32_t phys_addr, uint16_t& out) -> int
    {
        Bus::MemFault f{};
        return bus_.read_u16(phys_addr, out, f) ? 1 : 0;
    };
    auto mmio_write_u16 = [&](uint32_t phys_addr, uint16_t v) -> int
    {
        Bus::MemFault f{};

        return bus_.write_u16(phys_addr, v, f) ? 1 : 0;
    };
    auto mmio_read_u32 = [&](uint32_t phys_addr, uint32_t& out) -> int
    {
        Bus::MemFault f{};
        return bus_.read_u32(phys_addr, out, f) ? 1 : 0;
    };
    auto mmio_write_u32 = [&](uint32_t phys_addr, uint32_t v) -> int
    {
        Bus::MemFault f{};

        return bus_.write_u32(phys_addr, v, f) ? 1 : 0;
    };

    auto hle_memcpy = [&](uint32_t dst, uint32_t src, uint32_t len) -> int
    {
        // Très lent (byte-by-byte), mais suffisant pour bring-up.
        for (uint32_t i = 0; i < len; ++i)
        {
            uint8_t b = 0;
            if (!read_u8_guest(src + i, b))
                return 0;
            if (!write_u8_guest(dst + i, b))
                return 0;
        }
        return 1;
    };
    auto hle_memset = [&](uint32_t dst, uint8_t fill, uint32_t len) -> int
    {
        for (uint32_t i = 0; i < len; ++i)
        {
            if (!write_u8_guest(dst + i, fill))
                return 0;
        }
        return 1;
    };
    auto hle_strlen = [&](uint32_t src, uint32_t max_scan) -> uint32_t
    {
        uint32_t n = 0;
        for (; n < max_scan; ++n)
        {
            uint8_t ch = 0;
            if (!read_u8_guest(src + n, ch))
                break;
            if (ch == 0)
                break;
        }
        return n;
    };

    // -----------------------------
    // HLE File I/O (cdrom:) minimal
    // -----------------------------
    auto hle_set_last_err = [&](uint32_t e) -> void
    {
        hle_last_error_ = e;
    };

    auto hle_alloc_fd = [&]() -> int
    {
        // PSX: 0..15, mais 0/1 sont souvent stdio réservés.
        for (int fd = 2; fd < 16; ++fd)
        {
            if (!hle_files_[fd].used)
            {
                hle_files_[fd].used = 1;
                hle_files_[fd].lba = 0;
                hle_files_[fd].size = 0;
                hle_files_[fd].pos = 0;
                return fd;
            }
        }
        return -1;
    };

    auto hle_free_fd = [&](int fd) -> void
    {
        if (fd < 0 || fd >= 16)
            return;
        hle_files_[fd].used = 0;
        hle_files_[fd].lba = 0;
        hle_files_[fd].size = 0;
        hle_files_[fd].pos = 0;
    };

    auto hle_write_guest = [&](uint32_t dst, const uint8_t* src, uint32_t n) -> int
    {
        if (!src || n == 0)
            return 1;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (!write_u8_guest(dst + i, src[i]))
                return 0;
        }
        return 1;
    };

    auto hle_file_open = [&](uint32_t filename_ptr, uint32_t accessmode) -> int
    {
        (void)accessmode;
        cdrom::Cdrom* cd = bus_.cdrom();
        if (!cd)
        {
            hle_set_last_err(0x13u); // unknown device
            return -1;
        }

        char name[256];
        read_cstr_guest(filename_ptr, name, (uint32_t)sizeof(name));

        uint32_t lba = 0;
        uint32_t size = 0;
        if (!cd->iso9660_find_file(name, &lba, &size))
        {
            hle_set_last_err(0x02u); // file not found
            return -1;
        }

        const int fd = hle_alloc_fd();
        if (fd < 0)
        {
            hle_set_last_err(0x18u); // not enough handles
            return -1;
        }

        hle_files_[fd].lba = lba;
        hle_files_[fd].size = size;
        hle_files_[fd].pos = 0;
        hle_set_last_err(0);
        return fd;
    };

    auto hle_file_seek = [&](int fd, int32_t offset, uint32_t seektype) -> int32_t
    {
        if (fd < 0 || fd >= 16 || !hle_files_[fd].used)
        {
            hle_set_last_err(0x09u); // invalid handle
            return -1;
        }

        int64_t base = 0;
        if (seektype == 0u)
            base = 0;
        else if (seektype == 1u)
            base = (int64_t)hle_files_[fd].pos;
        else
        {
            hle_set_last_err(0x16u); // bad seek type
            return -1;
        }

        int64_t np = base + (int64_t)offset;
        if (np < 0)
            np = 0;
        if (np > (int64_t)hle_files_[fd].size)
            np = (int64_t)hle_files_[fd].size;
        hle_files_[fd].pos = (uint32_t)np;
        hle_set_last_err(0);
        return (int32_t)hle_files_[fd].pos;
    };

    auto hle_file_read = [&](int fd, uint32_t dst, uint32_t len) -> int32_t
    {
        cdrom::Cdrom* cd = bus_.cdrom();
        if (!cd || fd < 0 || fd >= 16 || !hle_files_[fd].used)
        {
            hle_set_last_err(0x09u); // invalid handle
            return -1;
        }
        if (len == 0)
        {
            hle_set_last_err(0x16u); // invalid length
            return -1;
        }

        const uint32_t pos = hle_files_[fd].pos;
        const uint32_t size = hle_files_[fd].size;
        if (pos >= size)
        {
            hle_set_last_err(0);
            return 0;
        }

        uint32_t todo = len;
        if (todo > (size - pos))
            todo = (size - pos);

        uint32_t done = 0;
        uint8_t sec[2048];
        while (done < todo)
        {
            const uint32_t fpos = pos + done;
            const uint32_t sec_idx = fpos / 2048u;
            const uint32_t sec_off = fpos % 2048u;
            const uint32_t lba = hle_files_[fd].lba + sec_idx;

            if (!cd->read_sector_2048(lba, sec))
            {
                hle_set_last_err(0x10u); // general error
                break;
            }

            uint32_t n = todo - done;
            const uint32_t avail = 2048u - sec_off;
            if (n > avail)
                n = avail;

            if (!hle_write_guest(dst + done, sec + sec_off, n))
            {
                hle_set_last_err(0x10u);
                break;
            }

            done += n;
        }

        hle_files_[fd].pos = pos + done;
        if (done != 0)
            hle_set_last_err(0);
        return (int32_t)done;
    };

    auto hle_file_close = [&](int fd) -> int
    {
        if (fd < 0 || fd >= 16 || !hle_files_[fd].used)
        {
            hle_set_last_err(0x09u);
            return -1;
        }
        hle_free_fd(fd);
        hle_set_last_err(0);
        return fd;
    };

    const uint32_t fn = gpr_[9] & 0xFFu; // t1
    const uint32_t a0 = gpr_[4];
    const uint32_t a1 = gpr_[5];
    const uint32_t a2 = gpr_[6];
    const uint32_t a3 = gpr_[7];

    auto emit_text_chars = [&](const char* s) -> void
    {
        if (!s)
            return;
        // Accumulate chars into a static line buffer and emit one emu::logf
        // call per completed line — preserves stderr visibility of the
        // guest printf on CLI (emu::logf falls back to fprintf(stderr)
        // when no sink is installed) without violating the "no printf"
        // project rule.  text_out_ (a dedicated diagnostic FILE*) and
        // text_push_char (flog::Sink path) keep receiving every byte.
        // Single-threaded: HLE runs on the emu thread only.
        static char   hle_line_buf[512];
        static uint32_t hle_line_pos = 0;
        for (uint32_t i = 0; s[i] != 0; ++i)
        {
            const uint8_t ch = (uint8_t)s[i];
            if (ch == '\n' || hle_line_pos + 1 >= sizeof(hle_line_buf))
            {
                hle_line_buf[hle_line_pos] = '\0';
                if (hle_line_pos > 0)
                    emu::logf(emu::LogLevel::info, "HLE", "%s", hle_line_buf);
                hle_line_pos = 0;
            }
            if (ch != '\n')
                hle_line_buf[hle_line_pos++] = (char)ch;
            if (text_out_)
                std::fputc((int)ch, text_out_);
            text_push_char(text_io_, text_clock_, text_has_clock_, text_line_, (uint32_t)sizeof(text_line_), text_pos_, ch);
        }
        if (text_out_)
            std::fflush(text_out_);
    };
    auto format_guest_printf = [&](uint32_t fmt_ptr, char* dst, uint32_t cap) -> void
    {
        if (!dst || cap == 0)
            return;

        char fmt[512];
        read_cstr_guest(fmt_ptr, fmt, (uint32_t)sizeof(fmt));

        uint32_t reg_arg_idx = 0;
        uint32_t stack_arg_idx = 0;
        const uint32_t stack_base = gpr_[29] + 16u;

        auto next_arg_u32 = [&]() -> uint32_t
        {
            if (reg_arg_idx == 0u) { reg_arg_idx++; return a1; }
            if (reg_arg_idx == 1u) { reg_arg_idx++; return a2; }
            if (reg_arg_idx == 2u) { reg_arg_idx++; return a3; }
            uint32_t v = 0;
            (void)read_u32_guest(stack_base + stack_arg_idx * 4u, v);
            stack_arg_idx++;
            return v;
        };

        uint32_t out = 0;
        auto push_ch = [&](char ch) -> void
        {
            if (out + 1u < cap)
                dst[out++] = ch;
        };
        auto push_str = [&](const char* s) -> void
        {
            if (!s)
                return;
            for (uint32_t i = 0; s[i] != 0; ++i)
                push_ch(s[i]);
        };

        for (uint32_t i = 0; fmt[i] != 0 && out + 1u < cap; ++i)
        {
            if (fmt[i] != '%')
            {
                push_ch(fmt[i]);
                continue;
            }

            ++i;
            if (fmt[i] == 0)
                break;
            if (fmt[i] == '%')
            {
                push_ch('%');
                continue;
            }

            while (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' ||
                   fmt[i] == '#' || fmt[i] == '0' || (fmt[i] >= '0' && fmt[i] <= '9') || fmt[i] == '.')
            {
                ++i;
                if (fmt[i] == 0)
                    break;
            }
            if (fmt[i] == 0)
                break;

            char tmp[128];
            tmp[0] = 0;
            switch (fmt[i])
            {
            case 's':
            {
                char str[256];
                read_cstr_guest(next_arg_u32(), str, (uint32_t)sizeof(str));
                push_str(str);
                break;
            }
            case 'd':
            case 'i':
                std::snprintf(tmp, sizeof(tmp), "%d", (int32_t)next_arg_u32());
                push_str(tmp);
                break;
            case 'u':
                std::snprintf(tmp, sizeof(tmp), "%u", next_arg_u32());
                push_str(tmp);
                break;
            case 'x':
                std::snprintf(tmp, sizeof(tmp), "%x", next_arg_u32());
                push_str(tmp);
                break;
            case 'X':
                std::snprintf(tmp, sizeof(tmp), "%X", next_arg_u32());
                push_str(tmp);
                break;
            case 'c':
                push_ch((char)(next_arg_u32() & 0xFFu));
                break;
            default:
                push_ch('%');
                push_ch(fmt[i]);
                break;
            }
        }

        dst[out] = 0;
    };

    // Default: succès "neutre".
    uint32_t ret_v0 = 0;
    int handled = 1;

    if (!hle_vec_gate && text_hle_gate)
    {
        if (pc_ == 0x0000'00A0u && fn == 0x3Fu)
        {
            char buf[512];
            format_guest_printf(a0, buf, (uint32_t)sizeof(buf));
            for (uint32_t i = 0; buf[i] != 0; ++i)
            {
                if (putchar_cb_)
                    putchar_cb_(buf[i], true, putchar_cb_user_);
            }
            emit_text_chars(buf);
        }
        else if (pc_ == 0x0000'00B0u && fn == 0x3Du)
        {
            const uint8_t ch = (uint8_t)(a0 & 0xFFu);
            char buf[2] = {(char)ch, 0};
            if (putchar_cb_)
                putchar_cb_((char)ch, false, putchar_cb_user_);
            emit_text_chars(buf);
            ret_v0 = 1;
        }
        else
        {
            handled = 0;
        }

        if (handled)
        {
            gpr_[2] = ret_v0;
            pc_ = gpr_[31]; // ra
            r.kind = StepResult::Kind::ok;
            r.instr = 0;
            return true;
        }
    }

    if (hle_vec_gate)
    {
        if (pc_ == 0x0000'00A0u)
        {
            switch (fn)
            {
            case 0x00u: // A(00h) FileOpen(filename, accessmode)
                ret_v0 = (uint32_t)hle_file_open(a0, a1);
                break;
            case 0x01u: // A(01h) FileSeek(fd, offset, seektype)
                ret_v0 = (uint32_t)hle_file_seek((int)a0, (int32_t)a1, a2);
                break;
            case 0x02u: // A(02h) FileRead(fd, dst, length)
                ret_v0 = (uint32_t)hle_file_read((int)a0, a1, a2);
                break;
            case 0x03u: // A(03h) FileWrite(fd, src, length) (CDROM: returns 0)
                ret_v0 = 0;
                break;
            case 0x04u: // A(04h) FileClose(fd)
                ret_v0 = (uint32_t)hle_file_close((int)a0);
                break;
            case 0x1Bu: // A(1Bh) strlen(src)
                ret_v0 = hle_strlen(a0, 1024u * 1024u);
                break;
            case 0x28u: // A(28h) bzero(dst,len)
                (void)hle_memset(a0, 0, a1);
                ret_v0 = a0;
                break;
            case 0x2Au: // A(2Ah) memcpy(dst,src,len)
                (void)hle_memcpy(a0, a1, a2);
                ret_v0 = a0;
                break;
            case 0x2Bu: // A(2Bh) memset(dst,fillbyte,len)
                (void)hle_memset(a0, (uint8_t)(a1 & 0xFFu), a2);
                ret_v0 = a0;
                break;
            case 0x3Fu: // A(3Fh) printf(txt, ...)
                {
                    char buf[512];
                    format_guest_printf(a0, buf, (uint32_t)sizeof(buf));
                    for (uint32_t i = 0; buf[i] != 0; ++i)
                    {
                        if (putchar_cb_)
                            putchar_cb_(buf[i], true, putchar_cb_user_);
                    }
                    emit_text_chars(buf);
                    ret_v0 = 0;
                }
                break;
            case 0x44u: // A(44h) FlushCache()
                ret_v0 = 0;
                break;
            case 0x45u: // A(45h) init_a0_b0_c0_vectors
                // On laisse le BIOS continuer; ici, on dépend surtout du HLE pour ne pas tomber sur NOP.
                ret_v0 = 0;
                break;
            case 0x13u: // A(13h) setjmp(buf)
            {
                // Save RA, SP, FP, S0-S7, GP to the jmpbuf at a0.
                // Returns 0 on initial call.
                auto wj = [&](uint32_t addr, uint32_t val) {
                    Bus::MemFault wf{};
                    (void)bus_.write_u32(virt_to_phys(addr), val, wf);
                };
                wj(a0 + 0x00u, gpr_[31]); // RA
                wj(a0 + 0x04u, gpr_[29]); // SP
                wj(a0 + 0x08u, gpr_[30]); // FP
                for (uint32_t i = 0; i < 8; ++i)
                    wj(a0 + 0x0Cu + i * 4u, gpr_[16 + i]); // S0-S7
                wj(a0 + 0x2Cu, gpr_[28]); // GP
                ret_v0 = 0;
                break;
            }
            case 0x14u: // A(14h) longjmp(buf, retval)
            {
                // Restore registers from jmpbuf at a0, return a1.
                auto rj = [&](uint32_t addr) -> uint32_t {
                    Bus::MemFault mf{};
                    uint32_t v = 0;
                    (void)bus_.read_u32(virt_to_phys(addr), v, mf);
                    return v;
                };
                gpr_[31] = rj(a0 + 0x00u); // RA
                gpr_[29] = rj(a0 + 0x04u); // SP
                gpr_[30] = rj(a0 + 0x08u); // FP
                for (uint32_t i = 0; i < 8; ++i)
                    gpr_[16 + i] = rj(a0 + 0x0Cu + i * 4u); // S0-S7
                gpr_[28] = rj(a0 + 0x2Cu); // GP
                ret_v0 = (a1 != 0) ? a1 : 1u;
                break;
            }
            case 0x17u: // A(17h) strcmp(s1, s2)
            {
                uint32_t i = 0;
                for (;; ++i)
                {
                    uint8_t c1 = 0, c2 = 0;
                    read_u8_guest(a0 + i, c1);
                    read_u8_guest(a1 + i, c2);
                    if (c1 != c2 || c1 == 0)
                    {
                        ret_v0 = (uint32_t)(int32_t)((int)c1 - (int)c2);
                        break;
                    }
                }
                break;
            }
            case 0x18u: // A(18h) strncmp(s1, s2, maxlen)
            {
                ret_v0 = 0;
                for (uint32_t i = 0; i < a2; ++i)
                {
                    uint8_t c1 = 0, c2 = 0;
                    read_u8_guest(a0 + i, c1);
                    read_u8_guest(a1 + i, c2);
                    if (c1 != c2 || c1 == 0)
                    {
                        ret_v0 = (uint32_t)(int32_t)((int)c1 - (int)c2);
                        break;
                    }
                }
                break;
            }
            case 0x19u: // A(19h) strcpy(dst, src)
            {
                uint32_t i = 0;
                for (;; ++i)
                {
                    uint8_t ch = 0;
                    read_u8_guest(a1 + i, ch);
                    write_u8_guest(a0 + i, ch);
                    if (ch == 0) break;
                }
                ret_v0 = a0;
                break;
            }
            case 0x1Au: // A(1Ah) strncpy(dst, src, maxlen)
            {
                uint32_t i = 0;
                bool ended = false;
                for (; i < a2; ++i)
                {
                    if (!ended)
                    {
                        uint8_t ch = 0;
                        read_u8_guest(a1 + i, ch);
                        write_u8_guest(a0 + i, ch);
                        if (ch == 0) ended = true;
                    }
                    else
                    {
                        write_u8_guest(a0 + i, 0);
                    }
                }
                ret_v0 = a0;
                break;
            }
            case 0x1Cu: // A(1Ch) strcat(dst, src)
            {
                uint32_t dlen = hle_strlen(a0, 1024u * 1024u);
                uint32_t i = 0;
                for (;; ++i)
                {
                    uint8_t ch = 0;
                    read_u8_guest(a1 + i, ch);
                    write_u8_guest(a0 + dlen + i, ch);
                    if (ch == 0) break;
                }
                ret_v0 = a0;
                break;
            }
            case 0x25u: // A(25h) toupper(c)
            {
                uint32_t c = a0 & 0xFFu;
                if (c >= 'a' && c <= 'z') c -= 0x20u;
                ret_v0 = c;
                break;
            }
            case 0x26u: // A(26h) tolower(c)
            {
                uint32_t c = a0 & 0xFFu;
                if (c >= 'A' && c <= 'Z') c += 0x20u;
                ret_v0 = c;
                break;
            }
            case 0x34u: // A(34h) malloc(size)
            {
                // Simple bump allocator
                uint32_t aligned = (kalloc_ptr_ + 3u) & ~3u;
                if (aligned + a0 <= kalloc_end_)
                {
                    ret_v0 = aligned;
                    kalloc_ptr_ = aligned + a0;
                }
                else
                {
                    ret_v0 = 0; // out of memory
                }
                break;
            }
            case 0x35u: // A(35h) free(ptr)
                // No-op for bump allocator
                ret_v0 = 0;
                break;
            case 0x39u: // A(39h) InitHeap(base, size)
            {
                // Simple heap init: store base+size for malloc
                kalloc_ptr_ = a0;
                kalloc_end_ = a0 + a1;
                ret_v0 = 0;
                break;
            }
            case 0x49u: // A(49h) GPU_cw(gp0cmd) — send GP0 command word
            {
                Bus::MemFault wf{};
                (void)bus_.write_u32(0x1F80'1810u, a0, wf);
                ret_v0 = 0;
                break;
            }
            case 0x70u: // A(70h) _bu_init()
                ret_v0 = 0;
                break;
            case 0x71u: // A(71h) _96_init() — CD driver initialization
            {
                // The BIOS calls this before starting the boot executable.
                // It sets up CDROM IRQ handlers via SysEnqIntRP and reads the TOC.
                // For HLE, we just return success. The IRQ handlers are set up
                // separately via the HLE event system.
                emu::logf(emu::LogLevel::debug, "HLE", "A(0x71) _96_init() called");
                ret_v0 = 1; // success
                break;
            }
            case 0x72u: // A(72h) _96_remove() — CD driver cleanup
            {
                // Removes the CD driver IRQ handlers. Due to a SysDeqIntRP bug
                // in the original BIOS, this function doesn't actually work.
                // For HLE, we just return success.
                emu::logf(emu::LogLevel::debug, "HLE", "A(0x72) _96_remove() a0=0x%08X a1=0x%08X", a0, a1);
                ret_v0 = 1;
                break;
            }
            case 0x9Fu: // A(9Fh) SetMem(megabytes)
            {
                // Set available RAM size. We ignore this (always 2MB).
                ret_v0 = 0;
                break;
            }
            default:
                handled = 0;
                break;
            }
        }
        else if (pc_ == 0x0000'00B0u)
        {
        // B0:0x3D = putchar(char) (souvent utilisé pendant le boot)
        if (fn == 0x3Du)
        {
            const uint8_t ch = (uint8_t)(a0 & 0xFFu);
            if (putchar_cb_)
                putchar_cb_((char)ch, false, putchar_cb_user_);
            char buf[2] = {(char)ch, 0};
            emit_text_chars(buf);
            ret_v0 = 1;
        }
        else
        {
            switch (fn)
            {
                case 0x32u: // B(32h) FileOpen(filename, accessmode)
                    ret_v0 = (uint32_t)hle_file_open(a0, a1);
                    break;
                case 0x33u: // B(33h) FileSeek(fd, offset, seektype)
                    ret_v0 = (uint32_t)hle_file_seek((int)a0, (int32_t)a1, a2);
                    break;
                case 0x34u: // B(34h) FileRead(fd, dst, length)
                    ret_v0 = (uint32_t)hle_file_read((int)a0, a1, a2);
                    break;
                case 0x35u: // B(35h) FileWrite(fd, src, length)
                    ret_v0 = 0;
                    break;
                case 0x36u: // B(36h) FileClose(fd)
                    ret_v0 = (uint32_t)hle_file_close((int)a0);
                    break;
                case 0x00u: // B(00h) alloc_kernel_memory(size)
                    {
                        const uint32_t size = a0;
                        uint32_t p = kalloc_ptr_;
                        p = (p + 3u) & ~3u;
                        if (size == 0 || p > kalloc_end_ || (kalloc_end_ - p) < size)
                        {
                            ret_v0 = 0;
                        }
                        else
                        {
                            ret_v0 = p;
                            kalloc_ptr_ = p + ((size + 3u) & ~3u);
                        }
                    }
                    break;
                case 0x01u: // B(01h) free_kernel_memory(buf)
                    ret_v0 = 1;
                    break;
                case 0x02u: // B(02h) init_timer(t,reload,flags)
                    {
                        const uint32_t t = a0;
                        const uint32_t reload = a1;
                        const uint32_t flags = a2;
                        if (t <= 2u)
                        {
                            const uint32_t base = 0x1F80'1100u + t * 0x10u;

                            // Reset old mode, set target (reload), set new mode.
                            (void)mmio_write_u16(base + 0x04u, 0);
                            (void)mmio_write_u16(base + 0x08u, (uint16_t)(reload & 0xFFFFu));

                            uint16_t mode = (flags & (1u << 4)) ? (uint16_t)0x0049u : (uint16_t)0x0048u;
                            if ((flags & 1u) == 0u)
                                mode = (uint16_t)(mode | 0x0100u);
                            if (flags & (1u << 12))
                                mode = (uint16_t)(mode | 0x0010u);

                            (void)mmio_write_u16(base + 0x04u, mode);
                            ret_v0 = 1;
                        }
                        else
                        {
                            ret_v0 = 0;
                        }
                    }
                    break;
                case 0x03u: // B(03h) get_timer(t)
                    {
                        const uint32_t t = a0;
                        if (t <= 2u)
                        {
                            const uint32_t base = 0x1F80'1100u + t * 0x10u;
                            uint16_t cur = 0;
                            (void)mmio_read_u16(base + 0x00u, cur);
                            ret_v0 = (uint32_t)cur;
                        }
                        else
                        {
                            ret_v0 = 0;
                        }
                    }
                    break;
                case 0x04u: // B(04h) enable_timer_irq(t)
                    {
                        const uint32_t t = a0;
                        if (t <= 2u)
                        {
                            uint32_t mask = 0;
                            (void)mmio_read_u32(0x1F80'1074u, mask);
                            mask |= (1u << (4u + t));
                            (void)mmio_write_u32(0x1F80'1074u, mask);
                            ret_v0 = 1;
                        }
                        else
                        {
                            // t=3 (vblank) => return 0 per docs; other => garbage.
                            ret_v0 = 0;
                        }
                    }
                    break;
                case 0x05u: // B(05h) disable_timer_irq(t)
                    {
                        const uint32_t t = a0;
                        if (t <= 2u)
                        {
                            uint32_t mask = 0;
                            (void)mmio_read_u32(0x1F80'1074u, mask);
                            mask &= ~(1u << (4u + t));
                            (void)mmio_write_u32(0x1F80'1074u, mask);
                        }
                        ret_v0 = 1;
                    }
                    break;
                case 0x06u: // B(06h) restart_timer(t)
                    {
                        const uint32_t t = a0;
                        if (t <= 2u)
                        {
                            const uint32_t base = 0x1F80'1100u + t * 0x10u;
                            (void)mmio_write_u16(base + 0x00u, 0);
                            ret_v0 = 1;
                        }
                        else
                        {
                            ret_v0 = 0;
                        }
                    }
                    break;
                case 0x07u: // B(07h) DeliverEvent(class,spec)
                    {
                        const uint32_t cls = a0;
                        const uint32_t spec = a1;
                        for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])); ++i)
                        {
                            HleEvent& e = hle_events_[i];
                            if ((e.status & 0x2000u) && e.cls == cls && e.spec == spec)
                            {
                                // mode=2000h => mark ready.
                                // mode=1000h => callback (non implémenté ici), on marque ready aussi pour le bring-up.
                                e.status &= ~0x2000u;
                                e.status |= 0x4000u;
                            }
                        }
                        ret_v0 = 1;
                    }
                    break;
                case 0x08u: // B(08h) OpenEvent(class,spec,mode,func)
                    {
                        uint32_t idx = 0xFFFFFFFFu;
                        for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])); ++i)
                        {
                            if (hle_events_[i].status == 0)
                            {
                                idx = i;
                                break;
                            }
                        }
                        if (idx == 0xFFFFFFFFu)
                        {
                            ret_v0 = 0xFFFFFFFFu;
                        }
                        else
                        {
                            HleEvent& e = hle_events_[idx];
                            e.cls = a0;
                            e.spec = a1;
                            e.mode = a2;
                            e.func = a3;
                            // mode (très simplifié):
                            // - 0x2000: enabled/busy
                            // - 0x1000: callback (non implémenté ici)
                            // Beaucoup de BIOS passent 0x2000 et s'attendent à ce que l'event soit actif.
                            e.status = (a2 & 0x2000u) ? 0x2000u : 0x1000u;
                            ret_v0 = 0xF100'0000u | (idx & 0xFFFFu);

                            emu::logf(emu::LogLevel::debug, "HLE", "OpenEvent[%u] cls=0x%08X spec=0x%04X mode=0x%04X",
                                idx, a0, a1, a2);

                            if (sys_has_clock_)
                            {
                                flog::logf(
                                    sys_io_,
                                    sys_clock_,
                                    flog::Level::info,
                                    "CPU",
                                    "HLE OpenEvent cls=0x%08X spec=0x%08X mode=0x%08X func=0x%08X -> handle=0x%08X",
                                    a0,
                                    a1,
                                    a2,
                                    a3,
                                    ret_v0
                                );
                            }
                        }
                    }
                    break;
                case 0x09u: // B(09h) CloseEvent(event)
                    {
                        const uint32_t h = a0;
                        if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                        {
                            const uint32_t idx = h & 0xFFFFu;
                            if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                            {
                                hle_events_[idx] = {};
                            }
                        }
                        ret_v0 = 1;
                    }
                    break;
                case 0x0Au: // B(0Ah) WaitEvent(event)
                    {
                        const uint32_t h = a0;
                        ret_v0 = 0;
                        if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                        {
                            const uint32_t idx = h & 0xFFFFu;
                            if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                            {
                                HleEvent& e = hle_events_[idx];
                                if (e.status & 0x4000u)
                                {
                                    e.status &= ~0x4000u;
                                    e.status |= 0x2000u;
                                    ret_v0 = 1;
                                }

                                // Log minimal (throttled) pour diagnostiquer les boucles de WaitEvent.
                                hle_wait_event_calls_++;
                                const uint64_t n = hle_wait_event_calls_;
                                const int log_it = (n <= 8u) || ((n & (n - 1u)) == 0u);
                                if (log_it && sys_has_clock_)
                                {
                                    flog::logf(
                                        sys_io_,
                                        sys_clock_,
                                        flog::Level::info,
                                        "CPU",
                                        "HLE WaitEvent handle=0x%08X idx=%u status=0x%08X -> v0=%u (call=%" PRIu64 ")",
                                        h,
                                        idx,
                                        e.status,
                                        ret_v0,
                                        n
                                    );
                                }
                            }
                        }
                    }
                    break;
                case 0x0Bu: // B(0Bh) TestEvent(event)
                    {
                        const uint32_t h = a0;
                        ret_v0 = 0;
                        if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                        {
                            const uint32_t idx = h & 0xFFFFu;
                            if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                            {
                                HleEvent& e = hle_events_[idx];
                                if (e.status & 0x4000u)
                                {
                                    e.status &= ~0x4000u;
                                    e.status |= 0x2000u;
                                    ret_v0 = 1;
                                }
                            }
                        }
                    }
                    break;
                case 0x0Cu: // B(0Ch) EnableEvent(event)
                    {
                        const uint32_t h = a0;
                        ret_v0 = 0;
                        if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                        {
                            const uint32_t idx = h & 0xFFFFu;
                            if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                            {
                                HleEvent& e = hle_events_[idx];
                                if (e.status != 0)
                                {
                                    // enabled/busy
                                    e.status &= ~0x4000u;
                                    e.status |= 0x2000u;
                                    ret_v0 = 1;
                                }
                            }
                        }
                    }
                    break;
                case 0x0Du: // B(0Dh) DisableEvent(event)
                    {
                        const uint32_t h = a0;
                        if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                        {
                            const uint32_t idx = h & 0xFFFFu;
                            if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                            {
                                HleEvent& e = hle_events_[idx];
                                if (e.status != 0)
                                    e.status = 0x1000u;
                            }
                        }
                        ret_v0 = 1;
                    }
                    break;
                case 0x18u: // B(18h) ResetEntryInt()
                    {
                        // Retourne un pointeur vers une structure "savestate" type setjmp (30h bytes).
                        // On met une structure minimale dans une zone réservée.
                        const uint32_t base = entryint_struct_addr_;
                        // ra/pc, sp, fp, r16..r23, gp
                        (void)write_u32_guest(base + 0x00u, 0);
                        (void)write_u32_guest(base + 0x04u, 0x801F'FFF0u);
                        (void)write_u32_guest(base + 0x08u, 0);
                        for (uint32_t i = 0; i < 8; ++i)
                        {
                            (void)write_u32_guest(base + 0x0Cu + i * 4u, 0);
                        }
                        (void)write_u32_guest(base + 0x2Cu, 0);
                        ret_v0 = base;
                        entryint_hook_addr_ = 0;
                    }
                    break;
                case 0x19u: // B(19h) SetCustomExitFromException(addr)
                {
                    // The custom exit handler replaces ReturnFromException.
                    // On the real PS1, the BIOS performs a longjmp to the
                    // setjmp buffer at addr when returning from an exception.
                    hle_custom_exit_handler_ = a0;
                    ret_v0 = 0;
                    break;
                }
                case 0x17u: // B(17h) ReturnFromException()
                {
                    // PS1 BIOS ReturnFromException:
                    // 1. Read PCB pointer from RAM[0x108]
                    // 2. Read current TCB from PCB[0]
                    // 3. Restore all GPRs, HI, LO, Status, EPC from TCB
                    // 4. Apply RFE (shift Status >> 2)
                    // 5. Jump to saved EPC
                    //
                    // TCB layout (standard PS1 kernel):
                    //   +0x08: r26 (k0)       +0x0C: r27 (k1)
                    //   +0x10: cop0r13 (Cause) +0x14: cop0r14 (EPC)
                    //   +0x18..(+0x18+24*4)=+0x78: r1..r25
                    //   +0x7C: r28 (gp)  +0x80: r29 (sp)
                    //   +0x84: r30 (fp)  +0x88: r31 (ra)
                    //   +0x8C: cop0r12 (SR)
                    //   +0x90: hi        +0x94: lo

                    uint32_t pcb_ptr = 0;
                    if (!read_u32_guest(0x108u, pcb_ptr))
                    {
                        if (sys_has_clock_)
                            flog::logf(sys_io_, sys_clock_, flog::Level::error, "CPU",
                                "ReturnFromException: failed to read PCB at 0x108");
                        ret_v0 = 0;
                        break;
                    }

                    uint32_t tcb_ptr = 0;
                    if (!read_u32_guest(pcb_ptr, tcb_ptr))
                    {
                        if (sys_has_clock_)
                            flog::logf(sys_io_, sys_clock_, flog::Level::error, "CPU",
                                "ReturnFromException: failed to read TCB from PCB=0x%08X", pcb_ptr);
                        ret_v0 = 0;
                        break;
                    }

                    // Restore r1..r25 from TCB+0x18 + (n-1)*4
                    for (uint32_t n = 1; n <= 25; ++n)
                    {
                        uint32_t val = 0;
                        (void)read_u32_guest(tcb_ptr + 0x18u + (n - 1u) * 4u, val);
                        gpr_[n] = val;
                    }
                    // r26 (k0) from TCB+0x08, r27 (k1) from TCB+0x0C
                    { uint32_t v = 0; (void)read_u32_guest(tcb_ptr + 0x08u, v); gpr_[26] = v; }
                    { uint32_t v = 0; (void)read_u32_guest(tcb_ptr + 0x0Cu, v); gpr_[27] = v; }
                    // r28 (gp) from +0x7C, r29 (sp) from +0x80
                    { uint32_t v = 0; (void)read_u32_guest(tcb_ptr + 0x7Cu, v); gpr_[28] = v; }
                    { uint32_t v = 0; (void)read_u32_guest(tcb_ptr + 0x80u, v); gpr_[29] = v; }
                    // r30 (fp) from +0x84, r31 (ra) from +0x88
                    { uint32_t v = 0; (void)read_u32_guest(tcb_ptr + 0x84u, v); gpr_[30] = v; }
                    { uint32_t v = 0; (void)read_u32_guest(tcb_ptr + 0x88u, v); gpr_[31] = v; }

                    // Restore COP0 Status from +0x8C
                    uint32_t saved_sr = 0;
                    (void)read_u32_guest(tcb_ptr + 0x8Cu, saved_sr);

                    // Restore HI/LO from +0x90/+0x94
                    { uint32_t v = 0; (void)read_u32_guest(tcb_ptr + 0x90u, v); hi_ = v; }
                    { uint32_t v = 0; (void)read_u32_guest(tcb_ptr + 0x94u, v); lo_ = v; }

                    // Read saved EPC from +0x14
                    uint32_t saved_epc = 0;
                    (void)read_u32_guest(tcb_ptr + 0x14u, saved_epc);

                    // If using custom_exit (game handles IRQs itself) and CDROM IRQ is
                    // still pending, ack it before RFE to prevent infinite re-trigger.
                    // Some games (e.g., Ridge Racer) don't properly ack CDROM in their
                    // exception handler, relying on the BIOS to do it.
                    if (hle_custom_exit_handler_ != 0)
                    {
                        Bus::MemFault mf_ack{};
                        uint32_t cur_i_stat = 0;
                        (void)bus_.read_u32(0x1F80'1070u, cur_i_stat, mf_ack);
                        if (cur_i_stat & 0x0004u)  // CDROM pending
                        {
                            // Ack CDROM register first (write 0x07 to Index1.IRQ_Flags)
                            bus_.write_u8(0x1F80'1800u, 0x01u, mf_ack);  // Set Index 1
                            bus_.write_u8(0x1F80'1803u, 0x07u, mf_ack);  // Ack IRQ flags
                            bus_.write_u8(0x1F80'1800u, 0x00u, mf_ack);  // Restore Index 0
                            // Ack I_STAT bit 2
                            (void)bus_.write_u32(0x1F80'1070u, cur_i_stat & ~0x0004u, mf_ack);
                        }
                    }

                    // Apply RFE: shift Status[5:0] right by 2
                    saved_sr = (saved_sr & ~0x3Fu) | ((saved_sr >> 2) & 0x0Fu);
                    cop0_[COP0_STATUS] = saved_sr;

                    if (sys_has_clock_)
                    {
                        flog::logf(sys_io_, sys_clock_, flog::Level::debug, "CPU",
                            "ReturnFromException: TCB=0x%08X EPC=0x%08X SR=0x%08X",
                            tcb_ptr, saved_epc, saved_sr);
                    }

                    // Jump directly to saved EPC (bypass normal gpr_[31] return)
                    pc_ = saved_epc;
                    r.kind = StepResult::Kind::ok;
                    r.instr = 0;
                    return true;
                }
                case 0x20u: // B(20h) UnDeliverEvent(class,spec)
                    {
                        const uint32_t cls = a0;
                        const uint32_t spec = a1;
                        for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])); ++i)
                        {
                            HleEvent& e = hle_events_[i];
                            if ((e.status & 0x4000u) && e.cls == cls && e.spec == spec)
                            {
                                // enabled/ready -> enabled/busy
                                e.status &= ~0x4000u;
                                e.status |= 0x2000u;
                            }
                        }
                        ret_v0 = 1;
                    }
                    break;
                case 0x12u: // B(12h) InitPad(buf1, sz1, buf2, sz2)
                    // Joypad init. Store buffers but no actual pad emulation yet.
                    ret_v0 = 1;
                    break;
                case 0x13u: // B(13h) StartPad()
                    // Start joypad polling. No-op.
                    ret_v0 = 1;
                    break;
                case 0x14u: // B(14h) StopPad()
                    ret_v0 = 1;
                    break;
                case 0x38u: // B(38h) chdir(name)
                    // Change CD directory. No-op for our flat ISO view.
                    ret_v0 = 0;
                    break;
                case 0x3Bu: // B(3Bh) FileRead(fd, dst, len)
                    ret_v0 = 0;
                    break;
                case 0x4Au: // B(4Ah) InitCard(pad_enable)
                    // Memory card init. No-op.
                    ret_v0 = 1;
                    break;
                case 0x4Bu: // B(4Bh) StartCard()
                    // Start memory card communication. No-op.
                    ret_v0 = 1;
                    break;
                case 0x56u: // B(56h) GetC0Table
                    ret_v0 = 0;
                    break;
                case 0x57u: // B(57h) GetB0Table
                    ret_v0 = 0;
                    break;
                case 0x5Bu: // B(5Bh) ChangeClearPad(int)
                    // Controller init. No-op for now.
                    ret_v0 = 0;
                    break;
                default:
                    handled = 0;
                    break;
            }
        }
        }
        else
        {
        // C0
        switch (fn)
        {
            case 0x00u: // C(00h) EnqueueTimerAndVblankIrqs(priority)
                // Active un "tick" minimal pour débloquer WaitEvent sur la vblank software timer.
                hle_pseudo_vblank_ = 1;
                if (sys_has_clock_)
                {
                    flog::logf(sys_io_, sys_clock_, flog::Level::info, "CPU", "HLE EnqueueTimerAndVblankIrqs (pseudo vblank ON)");
                }
                ret_v0 = 0;
                break;
            case 0x01u: // C(01h) EnqueueSyscallHandler(priority)
                ret_v0 = 0;
                break;
            case 0x02u: // C(02h) SysEnqIntRP(priority, struc)
            {
                // Insert handler struct at HEAD of priority chain.
                // Chain heads: RAM[0x100 + priority*4]
                // Struct layout: +0=next, +4=func2, +8=func1, +C=pad
                const uint32_t prio = a0 & 3u;
                const uint32_t head_off = (0x100u + prio * 4u) & (bus_.ram_size() - 1u);
                uint8_t* ram = bus_.ram_ptr();
                // Read current head
                uint32_t old_head = (uint32_t)ram[head_off]
                                  | ((uint32_t)ram[head_off+1] << 8)
                                  | ((uint32_t)ram[head_off+2] << 16)
                                  | ((uint32_t)ram[head_off+3] << 24);
                // Write old head into new_struct->next (+0)
                uint32_t struc_phys = a1 & (bus_.ram_size() - 1u);
                ram[struc_phys+0] = (uint8_t)(old_head);
                ram[struc_phys+1] = (uint8_t)(old_head >> 8);
                ram[struc_phys+2] = (uint8_t)(old_head >> 16);
                ram[struc_phys+3] = (uint8_t)(old_head >> 24);
                // Update head to point to new struct
                ram[head_off+0] = (uint8_t)(a1);
                ram[head_off+1] = (uint8_t)(a1 >> 8);
                ram[head_off+2] = (uint8_t)(a1 >> 16);
                ram[head_off+3] = (uint8_t)(a1 >> 24);
                ret_v0 = 0;
                break;
            }
            case 0x03u: // C(03h) SysDeqIntRP(priority, struc)
            {
                // Remove handler struct from priority chain.
                const uint32_t prio = a0 & 3u;
                const uint32_t head_off = (0x100u + prio * 4u) & (bus_.ram_size() - 1u);
                uint8_t* ram = bus_.ram_ptr();
                uint32_t cur_ptr = (uint32_t)ram[head_off]
                                 | ((uint32_t)ram[head_off+1] << 8)
                                 | ((uint32_t)ram[head_off+2] << 16)
                                 | ((uint32_t)ram[head_off+3] << 24);
                uint32_t prev_off = head_off; // points to the "next" field to patch
                bool is_head = true;
                while (cur_ptr != 0)
                {
                    if (cur_ptr == a1)
                    {
                        // Found it — read its next pointer and patch previous
                        uint32_t cp = cur_ptr & (bus_.ram_size() - 1u);
                        uint32_t nxt = (uint32_t)ram[cp]
                                     | ((uint32_t)ram[cp+1] << 8)
                                     | ((uint32_t)ram[cp+2] << 16)
                                     | ((uint32_t)ram[cp+3] << 24);
                        ram[prev_off+0] = (uint8_t)(nxt);
                        ram[prev_off+1] = (uint8_t)(nxt >> 8);
                        ram[prev_off+2] = (uint8_t)(nxt >> 16);
                        ram[prev_off+3] = (uint8_t)(nxt >> 24);
                        break;
                    }
                    // Advance: prev_off = &cur->next, cur = cur->next
                    uint32_t cp = cur_ptr & (bus_.ram_size() - 1u);
                    prev_off = cp; // next field is at offset 0
                    cur_ptr = (uint32_t)ram[cp]
                            | ((uint32_t)ram[cp+1] << 8)
                            | ((uint32_t)ram[cp+2] << 16)
                            | ((uint32_t)ram[cp+3] << 24);
                    is_head = false;
                }
                ret_v0 = 0;
                break;
            }
            case 0x07u: // C(07h) InstallExceptionHandlers()
                ret_v0 = 0;
                break;
            case 0x08u: // C(08h) SysInitMemory(addr,size)
                kalloc_ptr_ = a0;
                kalloc_end_ = a0 + a1;
                ret_v0 = 0;
                break;
            case 0x0Au: // C(0Ah) ChangeClearRCnt(t,flag)
                // Non implémenté: pour bring-up, on accepte et retourne 0.
                ret_v0 = 0;
                break;
            case 0x0Cu: // C(0Ch) InitDefInt(priority)
                ret_v0 = 0;
                break;
            case 0x12u: // C(12h) InstallDevices(ttyflag)
                ret_v0 = 0;
                break;
            case 0x1Cu: // C(1Ch) AdjustA0Table()
                ret_v0 = 0;
                break;
            default:
                handled = 0;
                break;
        }
    }
    }

    if (hle_vec_gate)
    {
        if (!handled)
        {
            const char* vec_name = (pc_ == 0xA0u) ? "A" : (pc_ == 0xB0u) ? "B" : "C";
            emu::logf(emu::LogLevel::debug, "HLE", "Unhandled %s(0x%02X) a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X ra=0x%08X",
                vec_name, fn, a0, a1, a2, a3, gpr_[31]);
            if (logger_ && rlog::logger_enabled(logger_, rlog::Level::debug, rlog::Category::exc))
            {
                rlog::logger_logf(
                    logger_,
                    rlog::Level::debug,
                    rlog::Category::exc,
                    "HLE BIOS vector PC=0x%08X fn=0x%02X a0=0x%08X a1=0x%08X a2=0x%08X (unhandled, fallback v0=0)",
                    pc_,
                    fn,
                    a0,
                    a1,
                    a2
                );
            }
            if (sys_has_clock_)
            {
                flog::logf(sys_io_, sys_clock_, flog::Level::warn, "CPU",
                    "HLE UNHANDLED %s(0x%02X) a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X ra=0x%08X",
                    vec_name, fn, a0, a1, a2, a3, gpr_[31]);
            }
            ret_v0 = 0;
        }
        else
        {
            if (logger_ && rlog::logger_enabled(logger_, rlog::Level::debug, rlog::Category::exc))
            {
                rlog::logger_logf(
                    logger_,
                    rlog::Level::debug,
                    rlog::Category::exc,
                    "HLE BIOS vector PC=0x%08X fn=0x%02X -> v0=0x%08X",
                    pc_,
                    fn,
                    ret_v0
                );
            }
        }

        gpr_[2] = ret_v0;
        pc_ = gpr_[31]; // ra
        r.kind = StepResult::Kind::ok;
        r.instr = 0;
        return true;
    }

    // Fallthrough: text_hle_gate was on but function wasn't A(3Fh)/B(3Dh) —
    // real BIOS stub should run.  Caller (step) continues past the gate.
    return false;
}

} // namespace r3000
