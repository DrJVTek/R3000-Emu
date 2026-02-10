#include "R3000GpuComponent.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Logging/LogMacros.h"

#include "gpu/gpu.h"
#include "log/emu_log.h"

DEFINE_LOG_CATEGORY_STATIC(LogR3000Gpu, Log, All);

// ===================================================================
// Constructor
// ===================================================================
UR3000GpuComponent::UR3000GpuComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;

    UE_LOG(LogR3000Gpu, Warning, TEXT("GpuComponent CONSTRUCTOR - tick enabled"));
}

// ===================================================================
// BeginPlay - ensure tick is enabled
// ===================================================================
void UR3000GpuComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(true);
    UE_LOG(LogR3000Gpu, Warning, TEXT("GpuComponent BeginPlay - SetComponentTickEnabled(true)"));
}

// ===================================================================
// Cleanup
// ===================================================================
void UR3000GpuComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Gpu_ = nullptr;
    delete[] PixelBuffer_;
    PixelBuffer_ = nullptr;
    delete[] VramCopyBuffer_;
    VramCopyBuffer_ = nullptr;
    Super::EndPlay(EndPlayReason);
}

// ===================================================================
// BindGpu - called by R3000EmuComponent after core init
// ===================================================================
void UR3000GpuComponent::BindGpu(gpu::Gpu* InGpu)
{
    UE_LOG(LogR3000Gpu, Warning, TEXT("BindGpu called. InGpu=%p (was Gpu_=%p)"), InGpu, Gpu_);
    emu::logf(emu::LogLevel::info, "GPU", "GpuComponent v7 (debug_texture_upload)");

    Gpu_ = InGpu;

    // ---- Geometry ProceduralMeshComponent ----
    if (!MeshComp_)
    {
        AActor* Owner = GetOwner();
        if (Owner)
        {
            MeshComp_ = NewObject<UProceduralMeshComponent>(Owner, TEXT("PSXMesh"));
            MeshComp_->bUseAsyncCooking = true;
            MeshComp_->SetCastShadow(false);
            MeshComp_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            if (Owner->GetRootComponent())
                MeshComp_->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
            MeshComp_->RegisterComponent();
            MeshComp_->SetVisibility(true);
            MeshComp_->SetHiddenInGame(false);
        }
    }

    // ---- VRAM texture ----
    if (!VramTexture_)
        CreateVramTexture();

    // ---- Geometry material instance ----
    if (BaseMaterial && !MatInst_)
    {
        MatInst_ = UMaterialInstanceDynamic::Create(BaseMaterial, this);
        if (MatInst_ && VramTexture_)
            MatInst_->SetTextureParameterValue(TEXT("VramTexture"), VramTexture_);
    }

    // Warn if no material assigned - mesh will be invisible!
    if (!BaseMaterial)
    {
        UE_LOG(LogR3000Gpu, Error, TEXT("WARNING: BaseMaterial is NULL! Assign a material in the Blueprint or mesh will be invisible."));
        emu::logf(emu::LogLevel::error, "GPU", "BaseMaterial is NULL - mesh will be invisible! Assign a material in Blueprint.");
    }

    // ---- VRAM debug viewer ----
    if (bShowVramViewer)
        CreateOrUpdateVramViewer();

    UE_LOG(LogR3000Gpu, Log, TEXT("GPU bound. MeshComp=%d VramTex=%d Mat=%d VramViewer=%d"),
        MeshComp_ != nullptr, VramTexture_ != nullptr, MatInst_ != nullptr, bShowVramViewer);
}

// ===================================================================
// VRAM Texture creation
// ===================================================================
void UR3000GpuComponent::CreateVramTexture()
{
    VramTexture_ = UTexture2D::CreateTransient(kVramW, kVramH, PF_B8G8R8A8);
    if (!VramTexture_)
    {
        UE_LOG(LogR3000Gpu, Error, TEXT("Failed to create VRAM texture"));
        return;
    }

    VramTexture_->Filter = TF_Nearest;  // PS1-style nearest-neighbor
    VramTexture_->SRGB = false;
    VramTexture_->NeverStream = true;
#if WITH_EDITORONLY_DATA
    VramTexture_->MipGenSettings = TMGS_NoMipmaps;
#endif
    VramTexture_->UpdateResource();

    PixelBuffer_ = new uint8[kVramW * kVramH * 4];
    FMemory::Memzero(PixelBuffer_, kVramW * kVramH * 4);

    // Thread-safe VRAM copy buffer (avoids race with emulator worker thread)
    VramCopyBuffer_ = new uint16[kVramW * kVramH];
    FMemory::Memzero(VramCopyBuffer_, kVramW * kVramH * sizeof(uint16));
}

// ===================================================================
// VRAM Texture upload (15-bit → BGRA8) - only when dirty
// ===================================================================
void UR3000GpuComponent::UpdateVramTexture()
{
    if (!Gpu_ || !VramTexture_ || !PixelBuffer_ || !VramCopyBuffer_)
    {
        static bool bWarnedOnce = false;
        if (!bWarnedOnce)
        {
            UE_LOG(LogR3000Gpu, Error, TEXT("UpdateVramTexture: NULL pointer! Gpu=%d Tex=%d Pix=%d Copy=%d"),
                Gpu_ != nullptr, VramTexture_ != nullptr, PixelBuffer_ != nullptr, VramCopyBuffer_ != nullptr);
            bWarnedOnce = true;
        }
        return;
    }

    // Check for stale pointer - disabled for debug
    // if (!Gpu_->is_valid())
    //     return;

    // Thread-safe: check if VRAM changed before doing the full copy
    const uint32 CurrentSeq = Gpu_->vram_write_seq_locked();
    if (CurrentSeq == LastVramWriteSeq_)
        return;

    // Log first few updates to confirm texture is being updated
    if (VramUploadCount_ < 5)
    {
        UE_LOG(LogR3000Gpu, Warning, TEXT("UpdateVramTexture: seq %u -> %u (upload #%d)"),
            LastVramWriteSeq_, CurrentSeq, VramUploadCount_ + 1);
    }

    // VRAM changed - do a thread-safe copy to avoid race with emulator worker
    uint32 CopySeq = 0;
    Gpu_->copy_vram(VramCopyBuffer_, CopySeq);
    LastVramWriteSeq_ = CopySeq;

    // Use our thread-safe copy instead of direct GPU VRAM access
    const uint16_t* Vram = VramCopyBuffer_;

    // Convert 15-bit PS1 pixels (0bbbbbgg_gggrrrrr) to BGRA8
    uint8* Dst = PixelBuffer_;
    const int32 NumPixels = kVramW * kVramH;
    for (int32 i = 0; i < NumPixels; ++i)
    {
        const uint16_t Px = Vram[i];
        const uint8_t R5 = (uint8_t)(Px & 0x1F);
        const uint8_t G5 = (uint8_t)((Px >> 5) & 0x1F);
        const uint8_t B5 = (uint8_t)((Px >> 10) & 0x1F);
        // 5-bit → 8-bit with top-bit replication for accuracy
        Dst[0] = (B5 << 3) | (B5 >> 2); // B
        Dst[1] = (G5 << 3) | (G5 >> 2); // G
        Dst[2] = (R5 << 3) | (R5 >> 2); // R
        Dst[3] = 0xFF;                   // A
        Dst += 4;
    }

    // Upload to GPU texture via BulkData
    FTexturePlatformData* PlatformData = VramTexture_->GetPlatformData();
    if (PlatformData && PlatformData->Mips.Num() > 0)
    {
        FTexture2DMipMap& Mip = PlatformData->Mips[0];
        void* RawData = Mip.BulkData.Lock(LOCK_READ_WRITE);
        if (RawData)
        {
            FMemory::Memcpy(RawData, PixelBuffer_, NumPixels * 4);
            Mip.BulkData.Unlock();
            VramTexture_->UpdateResource();
        }
        else
        {
            Mip.BulkData.Unlock();
        }
    }

    VramUploadCount_++;

    // Debug: log first pixel values to verify data
    if (VramUploadCount_ <= 3)
    {
        const uint16_t* Vram16 = VramCopyBuffer_;
        UE_LOG(LogR3000Gpu, Warning, TEXT("UpdateVramTexture #%d: First pixels raw: %04X %04X %04X %04X"),
            VramUploadCount_, Vram16[0], Vram16[1], Vram16[2], Vram16[3]);
        UE_LOG(LogR3000Gpu, Warning, TEXT("  Converted BGRA: %02X%02X%02X%02X %02X%02X%02X%02X"),
            PixelBuffer_[0], PixelBuffer_[1], PixelBuffer_[2], PixelBuffer_[3],
            PixelBuffer_[4], PixelBuffer_[5], PixelBuffer_[6], PixelBuffer_[7]);
    }
}

// ===================================================================
// Rebuild geometry mesh from the GPU's ready draw list
// ===================================================================
void UR3000GpuComponent::RebuildMesh()
{
    if (!Gpu_ || !MeshComp_)
    {
        static bool bWarnedRebuild = false;
        if (!bWarnedRebuild)
        {
            UE_LOG(LogR3000Gpu, Error, TEXT("RebuildMesh: NULL pointer! Gpu=%d MeshComp=%d"), Gpu_ != nullptr, MeshComp_ != nullptr);
            bWarnedRebuild = true;
        }
        return;
    }

    // Check for stale pointer - disabled for debug
    // if (!Gpu_->is_valid())
    //     return;

    // Thread-safe copy of draw list (prevents race with emulator VBlank swap)
    gpu::FrameDrawList DrawListCopy;
    Gpu_->copy_ready_draw_list(DrawListCopy);
    const gpu::FrameDrawList& DrawList = DrawListCopy;
    const int32 NumCmds = static_cast<int32>(DrawList.cmds.size());

    // Debug: log draw list stats periodically
    static int32 sRebuildCount = 0;
    sRebuildCount++;
    if (sRebuildCount <= 10 || (sRebuildCount % 100) == 0)
    {
        UE_LOG(LogR3000Gpu, Warning, TEXT("RebuildMesh #%d: %d triangles in draw list, frame_id=%u"),
            sRebuildCount, NumCmds, DrawList.frame_id);
    }

    if (NumCmds == 0)
    {
        MeshComp_->ClearAllMeshSections();
        LastTriCount_ = 0;
        return;
    }

    // Log first time we receive primitives (confirms GPU bridge working)
    static bool bFirstPrimitives = true;
    if (bFirstPrimitives)
    {
        UE_LOG(LogR3000Gpu, Warning, TEXT("GPU: First primitives received! %d triangles. MatInst=%d"), NumCmds, MatInst_ != nullptr ? 1 : 0);
        emu::logf(emu::LogLevel::info, "GPU", "UE5 First primitives! %d tris, MatInst=%s", NumCmds, MatInst_ ? "OK" : "NULL (INVISIBLE!)");
        bFirstPrimitives = false;
    }

    const int32 NumVerts = NumCmds * 3;

    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FVector2D> UV0;
    TArray<FVector2D> UV1;
    TArray<FVector2D> UV2;
    TArray<FVector2D> UV3;
    TArray<FLinearColor> Colors;
    TArray<FProcMeshTangent> Tangents;

    Vertices.Reserve(NumVerts);
    Triangles.Reserve(NumVerts);
    Normals.Reserve(NumVerts);
    UV0.Reserve(NumVerts);
    UV1.Reserve(NumVerts);
    UV2.Reserve(NumVerts);
    UV3.Reserve(NumVerts);
    Colors.Reserve(NumVerts);
    Tangents.Reserve(NumVerts);

    // Normals face -X so the front of the mesh faces a camera looking down +X
    const FVector FaceNormal(-1.0f, 0.0f, 0.0f);
    const FProcMeshTangent FaceTangent(0.0f, 1.0f, 0.0f);

    // PS1→UE5 coordinate transform
    // Vertex coords are in drawing buffer space (draw offset already applied in GPU).
    // For centering: use DISPLAY rect (GP1(05) + width/height) - that's what the TV shows.
    // Display start (display_x, display_y) + half size = center of visible region.
    const gpu::DrawEnv& Env = Gpu_->draw_env();
    const gpu::DisplayConfig& Disp = Gpu_->display_config();
    const float DispCenterX = static_cast<float>(Disp.display_x) + 0.5f * static_cast<float>(Disp.width());
    const float DispCenterY = static_cast<float>(Disp.display_y) + 0.5f * static_cast<float>(Disp.height());
    const float OriginX = bCenterDisplay ? DispCenterX : static_cast<float>(Env.clip_x1);
    const float OriginY = bCenterDisplay ? DispCenterY : static_cast<float>(Env.clip_y1);

    if (bDebugMeshLog)
    {
        emu::logf(emu::LogLevel::info, "GPU", "MeshRebuild: %d tris | clip=(%u,%u)-(%u,%u) disp=(%u,%u)+(%u,%u) | Origin=(%.1f,%.1f) bCenter=%d Scale=%.3f DispOff=(%.1f,%.1f)",
            NumCmds, Env.clip_x1, Env.clip_y1, Env.clip_x2, Env.clip_y2,
            Disp.display_x, Disp.display_y, Disp.width(), Disp.height(),
            OriginX, OriginY, bCenterDisplay ? 1 : 0, PixelScale, DisplayOffset.X, DisplayOffset.Y);
    }

    float Ps1MinX = 1e9f, Ps1MaxX = -1e9f, Ps1MinY = 1e9f, Ps1MaxY = -1e9f;

    for (int32 i = 0; i < NumCmds; ++i)
    {
        const gpu::DrawCmd& Cmd = DrawList.cmds[i];
        // PS1 uses painter's algorithm: later triangles are drawn on top.
        // With depth buffer, we need REVERSE order: later triangles = smaller depth (closer to camera)
        const float Depth = static_cast<float>(NumCmds - 1 - i) * ZStep;

        for (int32 j = 0; j < 3; ++j)
        {
            const gpu::DrawVertex& V = Cmd.v[j];

            // GPU draw commands store absolute drawing buffer coords (draw offset is baked into vertices).
            // No adjustment needed here - the vertices are already in screen-space.
            float vx = static_cast<float>(V.x);
            float vy = static_cast<float>(V.y);

            // Drawing buffer coords → clip-relative (origin at display center or clip top-left) → UE5
            // +0.5: PS1 uses pixel-center convention (coords at pixel centers)
            float dx = (vx + 0.5f) - OriginX;
            float dy = (vy + 0.5f) - OriginY;
            float px = dx * PixelScale + DisplayOffset.X;
            float py = -dy * PixelScale + DisplayOffset.Y;
            Vertices.Add(FVector(Depth, px, py));

            if (bDebugMeshLog)
            {
                Ps1MinX = FMath::Min(Ps1MinX, static_cast<float>(V.x));
                Ps1MaxX = FMath::Max(Ps1MaxX, static_cast<float>(V.x));
                Ps1MinY = FMath::Min(Ps1MinY, static_cast<float>(V.y));
                Ps1MaxY = FMath::Max(Ps1MaxY, static_cast<float>(V.y));
            }

            Normals.Add(FaceNormal);
            Tangents.Add(FaceTangent);

            // Vertex color: PS1 RGB
            Colors.Add(FLinearColor(V.r / 255.0f, V.g / 255.0f, V.b / 255.0f, 1.0f));

            // UV0: texture coords within texture page (0-255 → 0.0-1.0)
            UV0.Add(FVector2D(V.u / 255.0f, V.v / 255.0f));

            // UV1: texture page base in VRAM (normalized)
            const float TpX = static_cast<float>((Cmd.texpage & 0xF) * 64) / 1024.0f;
            const float TpY = static_cast<float>(((Cmd.texpage >> 4) & 1) * 256) / 512.0f;
            UV1.Add(FVector2D(TpX, TpY));

            // UV2: CLUT base in VRAM (normalized)
            const float ClutX = static_cast<float>((Cmd.clut & 0x3F) * 16) / 1024.0f;
            const float ClutY = static_cast<float>((Cmd.clut >> 6) & 0x1FF) / 512.0f;
            UV2.Add(FVector2D(ClutX, ClutY));

            // UV3.x: tex depth mode (0=none, 1=4bit, 2=8bit, 3=15bit)
            // UV3.y: semi-transparency (0-3=semi mode, 4=opaque)
            const float TexMode = (Cmd.flags & 1) ? static_cast<float>(Cmd.tex_depth + 1) : 0.0f;
            const float SemiVal = (Cmd.flags & 2) ? static_cast<float>(Cmd.semi_mode) : 4.0f;
            UV3.Add(FVector2D(TexMode, SemiVal));

            // Winding order: PS1 has no backface culling, but UE5 does.
            // The Y inversion (-dy) flips winding, so we reverse vertex order:
            // Instead of 0,1,2 we add 0,2,1 to get correct CCW winding in UE5.
            static const int32 WindingRemap[3] = {0, 2, 1};
            Triangles.Add(i * 3 + WindingRemap[j]);
        }

        // Log first 3 tris with full vertex coords (PS1 -> UE5)
        if (bDebugMeshLog && i < 3)
        {
            const gpu::DrawVertex& Va = Cmd.v[0];
            const gpu::DrawVertex& Vb = Cmd.v[1];
            const gpu::DrawVertex& Vc = Cmd.v[2];
            float dxa = (static_cast<float>(Va.x) + 0.5f) - OriginX, dya = (static_cast<float>(Va.y) + 0.5f) - OriginY;
            float dxb = (static_cast<float>(Vb.x) + 0.5f) - OriginX, dyb = (static_cast<float>(Vb.y) + 0.5f) - OriginY;
            float dxc = (static_cast<float>(Vc.x) + 0.5f) - OriginX, dyc = (static_cast<float>(Vc.y) + 0.5f) - OriginY;
            emu::logf(emu::LogLevel::info, "GPU", "  Tri[%d]: PS1 v0=(%d,%d) v1=(%d,%d) v2=(%d,%d) -> UE5 (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) | tex=%d semi=%d",
                i, Va.x, Va.y, Vb.x, Vb.y, Vc.x, Vc.y,
                dxa * PixelScale + DisplayOffset.X, -dya * PixelScale + DisplayOffset.Y,
                dxb * PixelScale + DisplayOffset.X, -dyb * PixelScale + DisplayOffset.Y,
                dxc * PixelScale + DisplayOffset.X, -dyc * PixelScale + DisplayOffset.Y,
                (Cmd.flags & 1) ? 1 : 0, (Cmd.flags & 2) ? 1 : 0);
        }
    }

    if (bDebugMeshLog && NumCmds > 0)
    {
        emu::logf(emu::LogLevel::info, "GPU", "  Bounds PS1: X=[%.0f..%.0f] Y=[%.0f..%.0f] span=%.0fx%.0f",
            Ps1MinX, Ps1MaxX, Ps1MinY, Ps1MaxY, Ps1MaxX - Ps1MinX, Ps1MaxY - Ps1MinY);
    }

    MeshComp_->ClearAllMeshSections();
    MeshComp_->CreateMeshSection_LinearColor(
        0, Vertices, Triangles, Normals, UV0, UV1, UV2, UV3,
        Colors, Tangents, false /*bCreateCollision*/);

    // Force bounds update and log mesh info
    if (bDebugMeshLog)
    {
        FBoxSphereBounds Bounds = MeshComp_->Bounds;
        emu::logf(emu::LogLevel::info, "GPU", "MeshCreated: Verts=%d Tris=%d Bounds=Origin(%.1f,%.1f,%.1f) Extent(%.1f,%.1f,%.1f)",
            Vertices.Num(), Triangles.Num() / 3,
            Bounds.Origin.X, Bounds.Origin.Y, Bounds.Origin.Z,
            Bounds.BoxExtent.X, Bounds.BoxExtent.Y, Bounds.BoxExtent.Z);
    }

    if (MatInst_)
    {
        MeshComp_->SetMaterial(0, MatInst_);
        MeshComp_->MarkRenderStateDirty(); // Force render state update after material change
        if (bDebugMeshLog)
            emu::logf(emu::LogLevel::info, "GPU", "SetMaterial: MatInst_=%p BaseMaterial=%s MeshVisible=%d",
                (void*)MatInst_, BaseMaterial ? TCHAR_TO_UTF8(*BaseMaterial->GetName()) : "NULL",
                MeshComp_->IsVisible() ? 1 : 0);
    }
    else
    {
        // ALWAYS warn when no material - this makes the mesh invisible!
        static bool bWarnedNoMat = false;
        if (!bWarnedNoMat)
        {
            UE_LOG(LogR3000Gpu, Error, TEXT("GPU RebuildMesh: No material! %d triangles built but INVISIBLE. Set BaseMaterial in Blueprint!"), NumCmds);
            emu::logf(emu::LogLevel::error, "GPU", "RebuildMesh: No material! %d tris INVISIBLE. Set BaseMaterial in Blueprint!", NumCmds);
            bWarnedNoMat = true;
        }
    }

    LastTriCount_ = NumCmds;
}

// ===================================================================
// VRAM Debug Viewer - flat plane showing the 1024x512 VRAM
// ===================================================================
void UR3000GpuComponent::CreateOrUpdateVramViewer()
{
    AActor* Owner = GetOwner();
    if (!Owner || !VramTexture_)
        return;

    // Create the viewer mesh if needed
    if (!VramViewerMesh_)
    {
        VramViewerMesh_ = NewObject<UProceduralMeshComponent>(Owner, TEXT("VramViewerMesh"));
        VramViewerMesh_->bUseAsyncCooking = true;
        VramViewerMesh_->SetCastShadow(false);
        VramViewerMesh_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        if (Owner->GetRootComponent())
            VramViewerMesh_->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
        VramViewerMesh_->RegisterComponent();

        // Build a simple quad (2 triangles) showing the full VRAM.
        // The quad is built in the YZ plane, centered on Y, with normals facing -X
        // (so a player looking down +X sees it). Rotation property can adjust.
        const float W = kVramW * VramViewerScale;
        const float H = kVramH * VramViewerScale;
        const float HalfW = W * 0.5f;

        TArray<FVector> Verts;
        Verts.Add(FVector(0.0f, -HalfW, 0.0f));      // bottom-left
        Verts.Add(FVector(0.0f,  HalfW, 0.0f));       // bottom-right
        Verts.Add(FVector(0.0f,  HalfW, H));           // top-right
        Verts.Add(FVector(0.0f, -HalfW, H));           // top-left

        // Winding order: CCW when viewed from -X direction (facing the player)
        TArray<int32> Tris;
        Tris.Add(0); Tris.Add(2); Tris.Add(1);
        Tris.Add(0); Tris.Add(3); Tris.Add(2);

        TArray<FVector> Norms;
        Norms.Add(FVector(-1, 0, 0));
        Norms.Add(FVector(-1, 0, 0));
        Norms.Add(FVector(-1, 0, 0));
        Norms.Add(FVector(-1, 0, 0));

        // UV: map full texture. PS1 VRAM Y=0 is top, UE5 texture V=0 is top.
        TArray<FVector2D> UVs;
        UVs.Add(FVector2D(0.0f, 1.0f)); // bottom-left
        UVs.Add(FVector2D(1.0f, 1.0f)); // bottom-right
        UVs.Add(FVector2D(1.0f, 0.0f)); // top-right
        UVs.Add(FVector2D(0.0f, 0.0f)); // top-left

        TArray<FLinearColor> Colors;
        Colors.Add(FLinearColor::White);
        Colors.Add(FLinearColor::White);
        Colors.Add(FLinearColor::White);
        Colors.Add(FLinearColor::White);

        TArray<FProcMeshTangent> Tangents;
        Tangents.Add(FProcMeshTangent(0, 1, 0));
        Tangents.Add(FProcMeshTangent(0, 1, 0));
        Tangents.Add(FProcMeshTangent(0, 1, 0));
        Tangents.Add(FProcMeshTangent(0, 1, 0));

        VramViewerMesh_->CreateMeshSection_LinearColor(
            0, Verts, Tris, Norms, UVs, Colors, Tangents, false);

        bVramViewerCreated_ = true;
    }

    // Position and orient the viewer
    VramViewerMesh_->SetRelativeLocation(VramViewerOffset);
    VramViewerMesh_->SetRelativeRotation(VramViewerRotation);

    // Create/update material instance
    if (!VramViewerMatInst_)
    {
        UMaterialInterface* ViewerBase = VramViewerMaterial ? VramViewerMaterial : BaseMaterial;
        if (ViewerBase)
        {
            VramViewerMatInst_ = UMaterialInstanceDynamic::Create(ViewerBase, this);
            if (VramViewerMatInst_)
                VramViewerMatInst_->SetTextureParameterValue(TEXT("VramTexture"), VramTexture_);
        }
    }

    if (VramViewerMatInst_)
        VramViewerMesh_->SetMaterial(0, VramViewerMatInst_);

    VramViewerMesh_->SetVisibility(true);
    UE_LOG(LogR3000Gpu, Log, TEXT("VRAM Viewer created/updated. Scale=%.2f Offset=(%.0f,%.0f,%.0f)"),
        VramViewerScale, VramViewerOffset.X, VramViewerOffset.Y, VramViewerOffset.Z);
}

void UR3000GpuComponent::DestroyVramViewer()
{
    if (VramViewerMesh_)
    {
        VramViewerMesh_->SetVisibility(false);
        VramViewerMesh_->ClearAllMeshSections();
    }
    bVramViewerCreated_ = false;
}

void UR3000GpuComponent::SetVramViewerVisible(bool bVisible)
{
    bShowVramViewer = bVisible;
    if (bVisible && Gpu_)
        CreateOrUpdateVramViewer();
    else
        DestroyVramViewer();
}

// ===================================================================
// Tick - update VRAM texture + rebuild geometry + manage viewer
// ===================================================================
void UR3000GpuComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    // DEBUG: Log first few ticks to verify TickComponent is running
    static int sTickCount = 0;
    sTickCount++;
    if (sTickCount <= 5 || (sTickCount % 300) == 0)
    {
        UE_LOG(LogR3000Gpu, Warning, TEXT("TickComponent #%d: Gpu_=%p"), sTickCount, Gpu_);
    }

    // Debug: check if Gpu_ is null or stale (freed memory from Hot Reload)
    if (!Gpu_)
    {
        static bool bWarnedGpuNull = false;
        if (!bWarnedGpuNull)
        {
            UE_LOG(LogR3000Gpu, Error, TEXT("TickComponent: Gpu_ is NULL! BindGpu was not called?"));
            bWarnedGpuNull = true;
        }
        return;
    }

    // Check for stale pointer (Hot Reload issue) - DISABLED for debug, just log
    if (!Gpu_->is_valid())
    {
        static int sStaleLogCount = 0;
        if (sStaleLogCount < 5)
        {
            UE_LOG(LogR3000Gpu, Error, TEXT("TickComponent: Gpu_ magic=0x%08X (expected 0x%08X) - continuing anyway for debug"),
                Gpu_->magic_, gpu::Gpu::kMagicValid);
            sStaleLogCount++;
        }
        // DON'T return - continue for debugging
    }

    // Upload VRAM texture if it changed (dirty tracking via write sequence)
    UpdateVramTexture();

    // Rebuild geometry mesh when a new frame is ready
    const uint32 CurrentFrame = Gpu_->vram_frame_count();
    if (CurrentFrame != LastVramFrame_)
    {
        // Debug: log frame count transitions
        static uint32 sLoggedFrames = 0;
        if (sLoggedFrames < 10)
        {
            UE_LOG(LogR3000Gpu, Warning, TEXT("RebuildMesh: frame %u -> %u (LastTriCount=%d)"),
                LastVramFrame_, CurrentFrame, LastTriCount_);
            sLoggedFrames++;
        }
        RebuildMesh();
        LastVramFrame_ = CurrentFrame;
    }

    // VRAM viewer: create/destroy + live-update transform
    if (bShowVramViewer && !bVramViewerCreated_)
        CreateOrUpdateVramViewer();
    else if (!bShowVramViewer && bVramViewerCreated_)
        DestroyVramViewer();

    if (bVramViewerCreated_ && VramViewerMesh_)
    {
        VramViewerMesh_->SetRelativeLocation(VramViewerOffset);
        VramViewerMesh_->SetRelativeRotation(VramViewerRotation);
    }
}

// ===================================================================
// Display info accessors
// ===================================================================
int32 UR3000GpuComponent::GetDisplayWidth() const
{
    return Gpu_ ? static_cast<int32>(Gpu_->display_config().width()) : 320;
}

int32 UR3000GpuComponent::GetDisplayHeight() const
{
    return Gpu_ ? static_cast<int32>(Gpu_->display_config().height()) : 240;
}

bool UR3000GpuComponent::IsDisplayEnabled() const
{
    return Gpu_ ? Gpu_->display_config().display_enabled : false;
}
