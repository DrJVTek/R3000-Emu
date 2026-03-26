#include "R3000VideoComponent.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Logging/LogMacros.h"

#include "gpu/gpu.h"
#include "log/emu_log.h"

DEFINE_LOG_CATEGORY_STATIC(LogR3000Video, Log, All);

// ===================================================================
// Constructor
// ===================================================================
UR3000VideoComponent::UR3000VideoComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    SetMobility(EComponentMobility::Movable);
}

// ===================================================================
// BeginPlay
// ===================================================================
void UR3000VideoComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(true);
}

// ===================================================================
// EndPlay — release all buffers
// ===================================================================
void UR3000VideoComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Gpu_ = nullptr;
    delete[] VramCopyBuffer_; VramCopyBuffer_ = nullptr;
    delete[] PixelBuffer_;    PixelBuffer_    = nullptr;
    delete UpdateRegion_;     UpdateRegion_   = nullptr;
    Super::EndPlay(EndPlayReason);
}

// ===================================================================
// BindGpu — called by R3000EmuComponent after core init
// ===================================================================
void UR3000VideoComponent::BindGpu(gpu::Gpu* InGpu)
{
    Gpu_ = InGpu;
    emu::logf(emu::LogLevel::info, "VIDEO", "R3000VideoComponent bound to GPU");

    // Allocate the persistent VRAM copy buffer once
    if (!VramCopyBuffer_)
        VramCopyBuffer_ = new uint16[kVramW * kVramH];
}

// ===================================================================
// Tick — detect video mode, capture frame, auto-show/hide plane
// ===================================================================
void UR3000VideoComponent::TickComponent(
    float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!Gpu_ || !VramCopyBuffer_)
        return;

    const gpu::DisplayConfig& disp = Gpu_->display_config();

    // Video mode = 24-bit color depth AND display enabled AND valid dimensions
    const bool bIsVideo = disp.color_24bit && disp.display_enabled
                          && disp.width() > 0 && disp.height() > 0;

    // Log first detection of video mode
    if (bIsVideo && !bVideoVisible_ && FramesSinceVideo_ > 0)
    {
        UE_LOG(LogR3000Video, Warning,
            TEXT("VideoComponent: VIDEO DETECTED disp_xy=(%u,%u) w=%u h=%u 24bit=%d enabled=%d"),
            disp.display_x, disp.display_y,
            disp.width(), disp.height(),
            disp.color_24bit ? 1 : 0,
            disp.display_enabled ? 1 : 0);
    }

    if (bIsVideo)
    {
        FramesSinceVideo_ = 0;

        const int32 W = disp.width();
        const int32 H = disp.height();

        // Resize texture + plane if the display area changed
        if (W != VideoTexW_ || H != VideoTexH_)
        {
            CreateOrResizeTexture(W, H);
            RebuildPlaneMesh(W, H);
        }

        // Upload new frame from VRAM (only when VRAM was actually written)
        UploadVideoFrame(W, H);

        if (!bVideoVisible_)
            SetVideoVisible(true);
    }
    else
    {
        ++FramesSinceVideo_;
        if (bVideoVisible_ && FramesSinceVideo_ > HideDelayFrames)
            SetVideoVisible(false);
    }
}

// ===================================================================
// CreateOrResizeTexture — (re)allocate RGBA8 texture for display area
// ===================================================================
void UR3000VideoComponent::CreateOrResizeTexture(int32 W, int32 H)
{
    // Release old buffers if size changed
    delete[] PixelBuffer_;   PixelBuffer_ = nullptr;
    delete UpdateRegion_;     UpdateRegion_ = nullptr;

    VideoTexW_ = W;
    VideoTexH_ = H;

    // Create transient RGBA8 texture — unlit, no filtering (pixel-accurate FMV)
    VideoTexture_ = UTexture2D::CreateTransient(W, H, PF_R8G8B8A8, TEXT("PS1_Video"));
    if (!VideoTexture_)
    {
        UE_LOG(LogR3000Video, Error, TEXT("VideoComponent: failed to create texture %dx%d"), W, H);
        return;
    }
    VideoTexture_->Filter     = TF_Bilinear;   // bilinear looks better for FMV than nearest
    VideoTexture_->AddressX   = TA_Clamp;
    VideoTexture_->AddressY   = TA_Clamp;
    VideoTexture_->SRGB       = false;          // PS1 outputs linear (gamma already baked)
    VideoTexture_->NeverStream = true;
#if WITH_EDITORONLY_DATA
    VideoTexture_->MipGenSettings = TMGS_NoMipmaps;
#endif
    VideoTexture_->UpdateResource();

    PixelBuffer_ = new uint8[W * H * 4];
    FMemory::Memzero(PixelBuffer_, W * H * 4);
    UpdateRegion_ = new FUpdateTextureRegion2D(0, 0, 0, 0, (uint32)W, (uint32)H);

    // Update the material instance with the new texture
    if (VideoMaterial)
    {
        if (!VideoMatInst_ || VideoMatInst_->GetBaseMaterial() != VideoMaterial->GetBaseMaterial())
            VideoMatInst_ = UMaterialInstanceDynamic::Create(VideoMaterial, this);

        VideoMatInst_->SetTextureParameterValue(TEXT("VideoTexture"), VideoTexture_);

        if (VideoMesh_)
            VideoMesh_->SetMaterial(0, VideoMatInst_);
    }

    UE_LOG(LogR3000Video, Log, TEXT("VideoComponent: texture %dx%d created"), W, H);
    emu::logf(emu::LogLevel::info, "VIDEO", "video texture %dx%d (24-bit)", W, H);
}

// ===================================================================
// UploadVideoFrame — convert PS1 24-bit VRAM region to RGBA8 texture
// ===================================================================
//
// PS1 24-bit format: pixels stored as a byte stream (R,G,B per pixel) inside
// the 16-bit VRAM words. VRAM is treated as contiguous bytes:
//   row_stride_bytes = 1024 * 2 = 2048
//   display area starts at byte offset: display_y * 2048 + display_x * 2
//   pixel x: bytes [x*3, x*3+1, x*3+2] from row start = [R, G, B]
//
void UR3000VideoComponent::UploadVideoFrame(int32 W, int32 H)
{
    if (!VideoTexture_ || !PixelBuffer_ || !UpdateRegion_)
        return;

    // Thread-safe VRAM snapshot
    uint32 CopySeq = 0;
    Gpu_->copy_vram(VramCopyBuffer_, CopySeq);

    const gpu::DisplayConfig& disp = Gpu_->display_config();

    // VRAM as byte stream (each 16-bit word = 2 bytes: low byte first on little-endian host)
    const uint8* VramBytes = reinterpret_cast<const uint8*>(VramCopyBuffer_);
    constexpr uint32 kRowStrideBytes = kVramW * 2u; // 2048 bytes per VRAM row

    const uint32 BaseOff = (uint32)disp.display_y * kRowStrideBytes
                         + (uint32)disp.display_x * 2u;

    uint8* Dst = PixelBuffer_;
    for (int32 y = 0; y < H; ++y)
    {
        const uint8* Row = VramBytes + BaseOff + (uint32)y * kRowStrideBytes;
        for (int32 x = 0; x < W; ++x)
        {
            // 3 bytes per pixel: R at [x*3], G at [x*3+1], B at [x*3+2]
            const uint8* Px = Row + x * 3;
            Dst[0] = Px[0]; // R
            Dst[1] = Px[1]; // G
            Dst[2] = Px[2]; // B
            Dst[3] = 0xFF;  // A
            Dst += 4;
        }
    }

    VideoTexture_->UpdateTextureRegions(
        0,             // MipIndex
        1,             // NumRegions
        UpdateRegion_,
        (uint32)W * 4, // SrcPitch (bytes per row)
        4,             // SrcBpp (RGBA8)
        PixelBuffer_,
        [](uint8* /*Data*/, const FUpdateTextureRegion2D* /*Region*/) {});
}

// ===================================================================
// RebuildPlaneMesh — flat quad sized to match video aspect ratio
//
// Geometry: faces forward (+X). Width = PlaneWidth world units,
// height auto-computed from the pixel aspect ratio (W:H).
// UV (0,0) = top-left, (1,1) = bottom-right.
//
// VR path (bSphereMode): TODO — will be a sphere section facing inward
// so the video wraps around the viewer's field of view.
// ===================================================================
void UR3000VideoComponent::RebuildPlaneMesh(int32 W, int32 H)
{
    if (!VideoMesh_)
    {
        VideoMesh_ = NewObject<UProceduralMeshComponent>(GetOwner(), TEXT("VideoMesh"));
        VideoMesh_->bUseAsyncCooking = false;
        VideoMesh_->SetCastShadow(false);
        VideoMesh_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        VideoMesh_->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        VideoMesh_->RegisterComponent();
    }

    const float Pw = FMath::Max(PlaneWidth, 1.0f);
    const float Ph = (H > 0 && W > 0) ? Pw * (float)H / (float)W : Pw * 0.75f;
    const float HalfW = Pw * 0.5f;

    if (bSphereMode)
    {
        // TODO VR: generate a sphere section facing inward.
        // For now, fall through to flat plane.
        UE_LOG(LogR3000Video, Warning,
            TEXT("VideoComponent: bSphereMode not yet implemented, using flat plane."));
    }

    // Flat plane: quad at local origin, facing +X, top-left at (0, -HalfW, Ph)
    TArray<FVector>    Verts;
    TArray<int32>      Tris;
    TArray<FVector2D>  UVs;
    TArray<FVector>    Normals;
    TArray<FProcMeshTangent> Tangents;

    Verts.Add(FVector(0.f, -HalfW,  Ph)); // 0: top-left
    Verts.Add(FVector(0.f,  HalfW,  Ph)); // 1: top-right
    Verts.Add(FVector(0.f,  HalfW, 0.f)); // 2: bottom-right
    Verts.Add(FVector(0.f, -HalfW, 0.f)); // 3: bottom-left

    UVs.Add(FVector2D(0.f, 0.f)); // top-left
    UVs.Add(FVector2D(1.f, 0.f)); // top-right
    UVs.Add(FVector2D(1.f, 1.f)); // bottom-right
    UVs.Add(FVector2D(0.f, 1.f)); // bottom-left

    const FVector N(1.f, 0.f, 0.f); // normal: +X (facing forward)
    Normals.Add(N); Normals.Add(N); Normals.Add(N); Normals.Add(N);

    const FProcMeshTangent T(FVector(0.f, 1.f, 0.f), false);
    Tangents.Add(T); Tangents.Add(T); Tangents.Add(T); Tangents.Add(T);

    // Two triangles (CCW winding)
    Tris = {0, 2, 1,  0, 3, 2};

    VideoMesh_->CreateMeshSection(
        0, Verts, Tris, Normals, UVs,
        TArray<FVector2D>{}, TArray<FVector2D>{}, TArray<FVector2D>{},
        TArray<FColor>{}, Tangents,
        /*bCreateCollision=*/false);

    if (VideoMatInst_)
        VideoMesh_->SetMaterial(0, VideoMatInst_);

    UE_LOG(LogR3000Video, Log,
        TEXT("VideoComponent: plane rebuilt %.0fx%.0f UE units (%dx%d px)"), Pw, Ph, W, H);
}

// ===================================================================
// SetVideoVisible — show or hide the video plane
// ===================================================================
void UR3000VideoComponent::SetVideoVisible(bool bShow)
{
    bVideoVisible_ = bShow;

    if (VideoMesh_)
        VideoMesh_->SetVisibility(bShow, /*bPropagateToChildren=*/false);

    if (!bShow)
    {
        UE_LOG(LogR3000Video, Log, TEXT("VideoComponent: plane hidden (no video)"));
        emu::logf(emu::LogLevel::debug, "VIDEO", "video plane hidden");
    }
    else
    {
        UE_LOG(LogR3000Video, Log, TEXT("VideoComponent: plane shown (%dx%d 24-bit)"),
               VideoTexW_, VideoTexH_);
        emu::logf(emu::LogLevel::info, "VIDEO", "video plane shown %dx%d", VideoTexW_, VideoTexH_);
    }
}
