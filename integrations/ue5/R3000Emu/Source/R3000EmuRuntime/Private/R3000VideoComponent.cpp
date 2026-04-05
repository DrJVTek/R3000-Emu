#include "R3000VideoComponent.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Logging/LogMacros.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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
    if (!disp.display_enabled || disp.width() == 0 || disp.height() == 0)
    {
        ++FramesSinceVideo_;
        if (bVideoVisible_ && FramesSinceVideo_ > HideDelayFrames)
            SetVideoVisible(false);
        return;
    }

    // Simple detection: MDEC has decoded AND a write touched the display area
    const bool bMdecDisplay = Gpu_->has_mdec_display_content();

    if (bMdecDisplay)
    {
        FramesSinceVideo_ = 0;
        bIsVideo15Bit_ = !disp.color_24bit;

        const int32 W = disp.width();
        const int32 H = disp.height();
        SrcX_ = disp.display_x;
        SrcY_ = disp.display_y;

        // Resize texture + plane if dimensions changed
        if (W != VideoTexW_ || H != VideoTexH_)
        {
            CreateOrResizeTexture(W, H);
            RebuildPlaneMesh(W, H);
        }

        // Upload new frame from VRAM
        UploadVideoFrame(W, H);

        if (bHasRealContent_ && !bVideoVisible_)
        {
            UE_LOG(LogR3000Video, Warning,
                TEXT("VideoComponent: MDEC DISPLAY detected disp=(%u,%u %ux%u 24=%d)"),
                disp.display_x, disp.display_y, W, H, disp.color_24bit ? 1 : 0);
            SetVideoVisible(true);
        }
        else if (!bHasRealContent_ && bVideoVisible_)
        {
            SetVideoVisible(false);
        }

        // Reset flags so we detect NEXT frame's MDEC + write
        Gpu_->reset_mdec_display_flags();
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
// UploadVideoFrame — convert PS1 VRAM region to RGBA8 texture
// ===================================================================
// Supports both 24-bit (3 bytes/pixel in VRAM byte stream) and
// 15-bit (BGR555 in 16-bit VRAM words) display modes.
//
void UR3000VideoComponent::UploadVideoFrame(int32 W, int32 H)
{
    if (!VideoTexture_ || !PixelBuffer_ || !UpdateRegion_)
        return;

    // Thread-safe VRAM snapshot
    uint32 CopySeq = 0;
    Gpu_->copy_vram(VramCopyBuffer_, CopySeq);

    uint8* Dst = PixelBuffer_;

    if (bIsVideo15Bit_)
    {
        // 15-bit BGR555: read from LoadImage DMA area (SrcX_/SrcY_)
        for (int32 y = 0; y < H; ++y)
        {
            const uint32 Vy = ((uint32)SrcY_ + (uint32)y) % kVramH;
            for (int32 x = 0; x < W; ++x)
            {
                const uint32 Vx = ((uint32)SrcX_ + (uint32)x) % kVramW;
                const uint16 Pixel = VramCopyBuffer_[Vy * kVramW + Vx];
                Dst[0] = static_cast<uint8>(((Pixel      ) & 0x1F) << 3 | ((Pixel      ) & 0x1F) >> 2);
                Dst[1] = static_cast<uint8>(((Pixel >>  5) & 0x1F) << 3 | ((Pixel >>  5) & 0x1F) >> 2);
                Dst[2] = static_cast<uint8>(((Pixel >> 10) & 0x1F) << 3 | ((Pixel >> 10) & 0x1F) >> 2);
                Dst[3] = 0xFF;
                Dst += 4;
            }
        }
    }
    else
    {
        // 24-bit: read from display area as byte stream (R,G,B)
        const uint8* VramBytes = reinterpret_cast<const uint8*>(VramCopyBuffer_);
        constexpr uint32 kRowStrideBytes = kVramW * 2u;
        const uint32 BaseOff = (uint32)SrcY_ * kRowStrideBytes
                             + (uint32)SrcX_ * 2u;

        for (int32 y = 0; y < H; ++y)
        {
            const uint8* Row = VramBytes + BaseOff + (uint32)y * kRowStrideBytes;
            for (int32 x = 0; x < W; ++x)
            {
                const uint8* Px = Row + x * 3;
                Dst[0] = Px[0];
                Dst[1] = Px[1];
                Dst[2] = Px[2];
                Dst[3] = 0xFF;
                Dst += 4;
            }
        }
    }

    // Count non-black pixels — don't upload/show if frame is empty (MDEC init, no real data)
    uint32 NonBlack = 0;
    const uint8* Check = PixelBuffer_;
    const int32 Total = W * H;
    for (int32 i = 0; i < Total; ++i)
    {
        if (Check[0] | Check[1] | Check[2])
            ++NonBlack;
        Check += 4;
    }

    bHasRealContent_ = (NonBlack > static_cast<uint32>(Total / 20)); // >5% non-black

    // Dump first video frames as PPM for debugging
    if (DumpedFrames_ < 8)
    {
        ++DumpedFrames_;
        TArray<uint8> Ppm;
        const FString Header = FString::Printf(TEXT("P6\n%d %d\n255\n"), W, H);
        FTCHARToUTF8 Hdr(*Header);
        Ppm.Append(reinterpret_cast<const uint8*>(Hdr.Get()), Hdr.Length());
        Ppm.Reserve(Ppm.Num() + W * H * 3);
        for (int32 i = 0; i < W * H; ++i)
        {
            Ppm.Add(PixelBuffer_[i * 4]);
            Ppm.Add(PixelBuffer_[i * 4 + 1]);
            Ppm.Add(PixelBuffer_[i * 4 + 2]);
        }
        const FString Path = FPaths::Combine(
            FPaths::ProjectLogDir(),
            FString::Printf(TEXT("video_dump_%02u_%dx%d_src%u_%u_%s_nb%u.ppm"),
                DumpedFrames_, W, H, SrcX_, SrcY_,
                bIsVideo15Bit_ ? TEXT("15bit") : TEXT("24bit"), NonBlack));
        FFileHelper::SaveArrayToFile(Ppm, *Path);
        UE_LOG(LogR3000Video, Warning, TEXT("VideoComponent: dumped frame #%u to %s (nonblack=%u/%d real=%d)"),
            DumpedFrames_, *Path, NonBlack, Total, bHasRealContent_ ? 1 : 0);
    }

    if (!bHasRealContent_)
        return;

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
