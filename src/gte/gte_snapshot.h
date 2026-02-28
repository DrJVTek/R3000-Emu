#pragma once

#include <cstdint>
#include <cstring>

namespace gte
{

// 3D vertex as received by the GTE (before projection).
struct GteVertex3D
{
    int32_t vx, vy, vz;
};

// Rotation + Translation transform active at projection time.
struct GteTransform
{
    int16_t rt[9];   // 3x3 rotation matrix (ctrl regs 0-4, packed int16 pairs)
    int32_t tr[3];   // Translation vector   (ctrl regs 5-7)
};

// Snapshot captured after each RTPS/RTPT command.
// Contains both input (3D vertices, transform) and output (2D screen coords, depth).
struct GteSnapshot
{
    GteVertex3D vertices[3];  // Input 3D vertices (V0, V1, V2 for RTPT; only [0] for RTPS)
    int16_t  sx[3], sy[3];   // Output 2D screen coords (SXY0, SXY1, SXY2)
    uint16_t sz[3];           // Output depth values       (SZ1, SZ2, SZ3)
    GteTransform transform;   // RT + TR at projection time
    uint8_t vertex_count;     // 1 (RTPS) or 3 (RTPT)
    uint8_t valid;            // 1 after RTPS/RTPT completes
    uint32_t sequence_id;     // Monotonic counter for ordering
};

} // namespace gte
