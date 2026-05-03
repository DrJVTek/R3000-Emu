#pragma once

#include <cstdint>

namespace emu
{

// Maximum hooks per type. Static array, no heap allocation.
static constexpr int kMaxHooks = 8;

// Hook function signatures (C-style for ABI stability + zero overhead)
using VBlankHook = void(*)(uint32_t vblank_count, void* user);
using WriteHook  = void(*)(uint32_t phys_addr, uint32_t value, uint32_t size, void* user);
using StepHook   = void(*)(uint32_t pc, void* user);

struct HookEntry
{
    void* fn{nullptr};
    void* user{nullptr};
};

struct WriteHookEntry
{
    WriteHook fn{nullptr};
    void*     user{nullptr};
    uint32_t  watch_addr{0}; // 0 = all writes, nonzero = specific physical address
};

// Lightweight hook manager. Owned by emu::Core, pointer shared with Bus.
// Zero-cost when no hooks registered (single int compare per invocation site).
struct Hooks
{
    // --- VBlank hooks (called once per VBlank in Bus::tick) ---
    HookEntry on_vblank[kMaxHooks]{};
    int       on_vblank_count{0};

    // --- Write hooks (called on RAM writes in Bus::write_*) ---
    WriteHookEntry on_write[kMaxHooks]{};
    int            on_write_count{0};
    int            on_write_has_wildcard{0};

    // --- Step hooks (called after each instruction — EXPENSIVE) ---
    HookEntry on_step[kMaxHooks]{};
    int       on_step_count{0};

    // --- Registration ---

    int add_vblank(VBlankHook fn, void* user)
    {
        if (on_vblank_count >= kMaxHooks) return -1;
        on_vblank[on_vblank_count] = {reinterpret_cast<void*>(fn), user};
        return on_vblank_count++;
    }

    int add_write(WriteHook fn, void* user, uint32_t watch_addr = 0)
    {
        if (on_write_count >= kMaxHooks) return -1;
        on_write[on_write_count] = {fn, user, watch_addr};
        if (watch_addr == 0) on_write_has_wildcard = 1;
        return on_write_count++;
    }

    int add_step(StepHook fn, void* user)
    {
        if (on_step_count >= kMaxHooks) return -1;
        on_step[on_step_count] = {reinterpret_cast<void*>(fn), user};
        return on_step_count++;
    }

    bool remove_write(int handle)
    {
        if (handle < 0 || handle >= on_write_count) return false;
        for (int i = handle; i + 1 < on_write_count; ++i)
            on_write[i] = on_write[i + 1];
        on_write[--on_write_count] = {};
        on_write_has_wildcard = 0;
        for (int i = 0; i < on_write_count; ++i)
            if (on_write[i].watch_addr == 0) on_write_has_wildcard = 1;
        return true;
    }

    bool remove_write(WriteHook fn, void* user)
    {
        for (int i = 0; i < on_write_count; ++i)
            if (on_write[i].fn == fn && on_write[i].user == user)
                return remove_write(i);
        return false;
    }

    bool remove_step(int handle)
    {
        if (handle < 0 || handle >= on_step_count) return false;
        for (int i = handle; i + 1 < on_step_count; ++i)
            on_step[i] = on_step[i + 1];
        on_step[--on_step_count] = {};
        return true;
    }

    bool remove_step(StepHook fn, void* user)
    {
        void* raw_fn = reinterpret_cast<void*>(fn);
        for (int i = 0; i < on_step_count; ++i)
            if (on_step[i].fn == raw_fn && on_step[i].user == user)
                return remove_step(i);
        return false;
    }

    // --- Fast-path guards ---

    bool has_vblank() const { return on_vblank_count > 0; }
    bool has_write()  const { return on_write_count > 0; }
    bool has_step()   const { return on_step_count > 0; }

    // --- Dispatch ---

    void fire_vblank(uint32_t vblank_count) const
    {
        for (int i = 0; i < on_vblank_count; ++i)
            reinterpret_cast<VBlankHook>(on_vblank[i].fn)(vblank_count, on_vblank[i].user);
    }

    void fire_write(uint32_t phys_addr, uint32_t value, uint32_t size) const
    {
        for (int i = 0; i < on_write_count; ++i)
        {
            const auto& w = on_write[i];
            if (w.watch_addr == 0 || w.watch_addr == phys_addr)
                w.fn(phys_addr, value, size, w.user);
        }
    }

    void fire_step(uint32_t pc) const
    {
        for (int i = 0; i < on_step_count; ++i)
            reinterpret_cast<StepHook>(on_step[i].fn)(pc, on_step[i].user);
    }

    // Check if a specific physical address is watched by any write hook
    bool watches_addr(uint32_t phys_addr) const
    {
        if (on_write_has_wildcard) return true;
        for (int i = 0; i < on_write_count; ++i)
            if (on_write[i].watch_addr == phys_addr) return true;
        return false;
    }
};

} // namespace emu
