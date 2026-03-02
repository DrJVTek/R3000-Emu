#pragma once

#include <cstdint>

namespace gpu
{

struct DrawEnv;
struct FrameDrawList;

// Interface for all GPUs (primary rasterizer and shadow/3D reconstructor).
// Used by Bus, Core, and UE5 components — never the concrete type.
class IGpu
{
  public:
    virtual ~IGpu() = default;
    virtual void reset() = 0;
    virtual void gp0(uint32_t word) = 0;
    virtual void gp1(uint32_t word) = 0;
    virtual void on_vblank() = 0;

    // Thread-safe data access for UE5
    virtual void copy_ready_draw_list(FrameDrawList& out) const = 0;
    virtual uint32_t frame_count() const = 0;
};

} // namespace gpu
