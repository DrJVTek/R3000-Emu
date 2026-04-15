#include "PSXVideoSurfaceComponent.h"
#include "PSXSurfaceComponent.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Logging/LogMacros.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "gpu/gpu.h"
#include "log/emu_log.h"

DEFINE_LOG_CATEGORY_STATIC(LogPSXVideoSurface, Log, All);

namespace
{
static uint16 VideoDisplayRowWords(const gpu::DisplayConfig& Disp)
{
    const uint16 DisplayW = Disp.width();
    if (!Disp.color_24bit)
        return DisplayW;

    return static_cast<uint16>(((static_cast<uint32>(DisplayW) * 3u) + 1u) / 2u);
}

static bool IsVideoMultiBufferedMode(EPSXSurfaceBufferingMode Mode)
{
    return Mode == EPSXSurfaceBufferingMode::DoubleBuffer ||
           Mode == EPSXSurfaceBufferingMode::TripleBuffer;
}

static bool IsRecentDisplaySizedWrite(const gpu::CpuVramWriteInfo& Write, const gpu::DisplayConfig& Disp, uint32 CurrentFrame)
{
    if (Write.seq == 0 || !Disp.display_enabled)
        return false;

    const uint32 FrameDelta = (CurrentFrame >= Write.frame_count) ? (CurrentFrame - Write.frame_count) : 0u;
    if (FrameDelta > 12u)
        return false;

    const uint16 ExpectedW = VideoDisplayRowWords(Disp);
    const uint16 ExpectedH = Disp.height();
    const uint16 MinW = static_cast<uint16>(FMath::Max<int32>(64, (static_cast<int32>(ExpectedW) * 3) / 4));
    const uint16 MinH = static_cast<uint16>(FMath::Max<int32>(64, (static_cast<int32>(ExpectedH) * 3) / 4));
    return Write.w >= MinW && Write.h >= MinH;
}

static uint32 CountRecentDisplaySizedWrites(
    const std::vector<gpu::CpuVramWriteInfo>& Writes,
    const gpu::DisplayConfig& Disp,
    uint32 CurrentFrame)
{
    uint32 Count = 0;
    for (const gpu::CpuVramWriteInfo& Write : Writes)
    {
        if (IsRecentDisplaySizedWrite(Write, Disp, CurrentFrame))
            ++Count;
    }
    return Count;
}

static bool SelectVideoSourceWrite(
    const std::vector<gpu::CpuVramWriteInfo>& Writes,
    const gpu::DisplayConfig& Disp,
    uint32 CurrentFrame,
    EPSXSurfaceBufferingMode BufferingMode,
    gpu::CpuVramWriteInfo& OutWrite)
{
    bool bFound = false;
    uint32 BestScore = 0;

    for (const gpu::CpuVramWriteInfo& Write : Writes)
    {
        if (!IsRecentDisplaySizedWrite(Write, Disp, CurrentFrame))
            continue;

        const uint32 FrameDelta = (CurrentFrame >= Write.frame_count) ? (CurrentFrame - Write.frame_count) : 0u;
        const uint32 AreaScore = static_cast<uint32>(Write.w) * static_cast<uint32>(Write.h);
        const bool bExactDisplayOrigin =
            (Write.x == Disp.display_x) && (Write.y == Disp.display_y);
        const uint32 Score =
            ((bExactDisplayOrigin && !IsVideoMultiBufferedMode(BufferingMode)) ? 1u << 30 : 0u) +
            ((4u - FMath::Min(FrameDelta, 4u)) << 24) +
            FMath::Min(AreaScore, 0x00FFFFFFu);

        if (!bFound || Score >= BestScore)
        {
            bFound = true;
            BestScore = Score;
            OutWrite = Write;
        }
    }

    return bFound;
}

static EPSXSurfaceBufferingMode InferBufferingMode(
    const std::vector<gpu::CpuVramWriteInfo>& Writes,
    const gpu::DisplayConfig& Disp,
    uint32 CurrentFrame)
{
    struct FOrigin
    {
        uint16 X;
        uint16 Y;
        bool operator==(const FOrigin& Other) const { return X == Other.X && Y == Other.Y; }
    };

    TArray<FOrigin, TInlineAllocator<8>> UniqueOrigins;
    for (const gpu::CpuVramWriteInfo& Write : Writes)
    {
        if (!IsRecentDisplaySizedWrite(Write, Disp, CurrentFrame))
            continue;

        const FOrigin Origin{Write.x, Write.y};
        bool bExists = false;
        for (const FOrigin& Existing : UniqueOrigins)
        {
            if (Existing == Origin)
            {
                bExists = true;
                break;
            }
        }

        if (!bExists)
            UniqueOrigins.Add(Origin);
    }

    if (UniqueOrigins.Num() >= 3)
        return EPSXSurfaceBufferingMode::TripleBuffer;
    if (UniqueOrigins.Num() == 2)
        return EPSXSurfaceBufferingMode::DoubleBuffer;
    if (UniqueOrigins.Num() == 1)
        return EPSXSurfaceBufferingMode::Mono;
    return EPSXSurfaceBufferingMode::AutoDetect;
}
}

UPSXVideoSurfaceComponent::UPSXVideoSurfaceComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    SetMobility(EComponentMobility::Movable);
}

void UPSXVideoSurfaceComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(true);
}

void UPSXVideoSurfaceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Gpu_ = nullptr;
    delete[] VramCopyBuffer_; VramCopyBuffer_ = nullptr;
    delete[] PixelBuffer_;    PixelBuffer_    = nullptr;
    delete UpdateRegion_;     UpdateRegion_   = nullptr;
    Super::EndPlay(EndPlayReason);
}

void UPSXVideoSurfaceComponent::BindGpu(gpu::Gpu* InGpu)
{
    Gpu_ = InGpu;
    SurfaceComp_ = GetOwner() ? GetOwner()->FindComponentByClass<UPSXSurfaceComponent>() : nullptr;
    emu::logf(emu::LogLevel::warn, "VIDEO", "PSXVideoSurfaceComponent bound to GPU");

    // Allocate the persistent VRAM copy buffer once
    if (!VramCopyBuffer_)
        VramCopyBuffer_ = new uint16[kVramW * kVramH];
}

void UPSXVideoSurfaceComponent::TickComponent(
    float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!Gpu_ || !VramCopyBuffer_)
        return;

    const gpu::DisplayConfig& disp = Gpu_->display_config();
    std::vector<gpu::CpuVramWriteInfo> RecentWrites;
    Gpu_->copy_recent_cpu_vram_writes(RecentWrites);
    gpu::CpuVramWriteInfo SelectedWrite{};
    const uint32 CurrentFrame = Gpu_->vram_frame_count();
    const EPSXSurfaceBufferingMode BufferingMode = InferBufferingMode(RecentWrites, disp, CurrentFrame);
    const bool bDisplaySizedWrite = SelectVideoSourceWrite(RecentWrites, disp, CurrentFrame, BufferingMode, SelectedWrite);
    const uint32 RecentDisplayWriteCount = CountRecentDisplaySizedWrites(RecentWrites, disp, CurrentFrame);

    if (!disp.display_enabled || disp.width() == 0 || disp.height() == 0)
    {
        ++FramesSinceVideo_;
        if (bVideoVisible_ && FramesSinceVideo_ > HideDelayFrames)
            SetVideoVisible(false);
        return;
    }

    // Video candidate:
    // - classic case: MDEC output touched the current display area
    // - buffered case: MDEC is active and a recent display-sized VRAM write exists
    const bool bMdecDisplay = Gpu_->has_mdec_display_content();
    const bool bBufferedStreamingCandidate =
        bDisplaySizedWrite &&
        (RecentDisplayWriteCount >= 2u || BufferingMode != EPSXSurfaceBufferingMode::AutoDetect);
    const bool bLatchedBufferedCandidate = bVideoVisible_ && bDisplaySizedWrite;
    const bool bVideoCandidate =
        bMdecDisplay ||
        (Gpu_->has_mdec_activity() && bDisplaySizedWrite) ||
        bBufferedStreamingCandidate ||
        bLatchedBufferedCandidate;
    static uint32 LastRejectedLogFrame = 0;

    if (!bVideoCandidate && Gpu_->has_mdec_activity())
    {
        const uint32 RejectFrame = Gpu_->vram_frame_count();
        if (RejectFrame != LastRejectedLogFrame)
        {
            LastRejectedLogFrame = RejectFrame;
            emu::logf(
                emu::LogLevel::warn,
                "VIDEO",
                "candidate rejected frame=%u mdec_display=%d display_sized_write=%d eligible_writes=%u recent_writes=%u disp=(%u,%u %ux%u 24=%d)",
                RejectFrame,
                bMdecDisplay ? 1 : 0,
                bDisplaySizedWrite ? 1 : 0,
                RecentDisplayWriteCount,
                static_cast<unsigned>(RecentWrites.size()),
                disp.display_x, disp.display_y, disp.width(), disp.height(),
                disp.color_24bit ? 1 : 0);
        }
    }

    if (bVideoCandidate)
    {
        FramesSinceVideo_ = 0;
        bIsVideo15Bit_ = !disp.color_24bit;

        const int32 W = disp.width();
        const int32 H = disp.height();
        SrcX_ = bDisplaySizedWrite ? SelectedWrite.x : disp.display_x;
        SrcY_ = bDisplaySizedWrite ? SelectedWrite.y : disp.display_y;

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
            UE_LOG(LogPSXVideoSurface, Log,
                TEXT("Video surface detected mode=%s buffering=%d src=(%u,%u) disp=(%u,%u %ux%u 24=%d)"),
                bMdecDisplay ? TEXT("display-hit") : TEXT("buffered-write"),
                static_cast<int32>(BufferingMode),
                SrcX_, SrcY_,
                disp.display_x, disp.display_y, W, H, disp.color_24bit ? 1 : 0);
            emu::logf(
                emu::LogLevel::warn,
                "VIDEO",
                "detected mode=%s buffering=%d writes=%u src=(%u,%u) disp=(%u,%u %ux%u 24=%d)",
                bMdecDisplay ? "display-hit" : (bBufferedStreamingCandidate ? "buffered-stream" : "buffered-latched"),
                static_cast<int32>(BufferingMode),
                RecentDisplayWriteCount,
                SrcX_, SrcY_,
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

void UPSXVideoSurfaceComponent::CreateOrResizeTexture(int32 W, int32 H)
{
    delete[] PixelBuffer_;   PixelBuffer_ = nullptr;
    delete UpdateRegion_;     UpdateRegion_ = nullptr;

    VideoTexW_ = W;
    VideoTexH_ = H;

    VideoTexture_ = UTexture2D::CreateTransient(W, H, PF_R8G8B8A8, TEXT("PS1_Video"));
    if (!VideoTexture_)
    {
        UE_LOG(LogPSXVideoSurface, Error, TEXT("Video surface failed to create texture %dx%d"), W, H);
        return;
    }
    VideoTexture_->Filter     = TF_Bilinear;
    VideoTexture_->AddressX   = TA_Clamp;
    VideoTexture_->AddressY   = TA_Clamp;
    VideoTexture_->SRGB       = false;
    VideoTexture_->NeverStream = true;
#if WITH_EDITORONLY_DATA
    VideoTexture_->MipGenSettings = TMGS_NoMipmaps;
#endif
    VideoTexture_->UpdateResource();

    PixelBuffer_ = new uint8[W * H * 4];
    FMemory::Memzero(PixelBuffer_, W * H * 4);
    UpdateRegion_ = new FUpdateTextureRegion2D(0, 0, 0, 0, (uint32)W, (uint32)H);

    if (VideoMaterial)
    {
        if (!VideoMatInst_ || VideoMatInst_->GetBaseMaterial() != VideoMaterial->GetBaseMaterial())
            VideoMatInst_ = UMaterialInstanceDynamic::Create(VideoMaterial, this);

        VideoMatInst_->SetTextureParameterValue(TEXT("VideoTexture"), VideoTexture_);

        if (VideoMesh_)
            VideoMesh_->SetMaterial(0, VideoMatInst_);
    }

    UE_LOG(LogPSXVideoSurface, Log, TEXT("Video surface texture %dx%d created"), W, H);
    emu::logf(emu::LogLevel::info, "VIDEO", "video texture %dx%d (24-bit)", W, H);

    if (bVideoVisible_)
        SyncUnifiedSurface(true);
}

void UPSXVideoSurfaceComponent::UploadVideoFrame(int32 W, int32 H)
{
    if (!VideoTexture_ || !PixelBuffer_ || !UpdateRegion_)
        return;

    uint32 CopySeq = 0;
    Gpu_->copy_vram(VramCopyBuffer_, CopySeq);

    uint8* Dst = PixelBuffer_;

    if (bIsVideo15Bit_)
    {
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
        UE_LOG(LogPSXVideoSurface, Verbose, TEXT("Video surface dumped frame #%u to %s (nonblack=%u/%d real=%d)"),
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

void UPSXVideoSurfaceComponent::RebuildPlaneMesh(int32 W, int32 H)
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
        UE_LOG(LogPSXVideoSurface, Verbose,
            TEXT("Video surface sphere mode is not implemented yet; using a flat plane."));
    }

    TArray<FVector>    Verts;
    TArray<int32>      Tris;
    TArray<FVector2D>  UVs;
    TArray<FVector>    Normals;
    TArray<FProcMeshTangent> Tangents;

    Verts.Add(FVector(0.f, -HalfW,  Ph));
    Verts.Add(FVector(0.f,  HalfW,  Ph));
    Verts.Add(FVector(0.f,  HalfW, 0.f));
    Verts.Add(FVector(0.f, -HalfW, 0.f));

    UVs.Add(FVector2D(0.f, 0.f));
    UVs.Add(FVector2D(1.f, 0.f));
    UVs.Add(FVector2D(1.f, 1.f));
    UVs.Add(FVector2D(0.f, 1.f));

    const FVector N(1.f, 0.f, 0.f);
    Normals.Add(N); Normals.Add(N); Normals.Add(N); Normals.Add(N);

    const FProcMeshTangent T(FVector(0.f, 1.f, 0.f), false);
    Tangents.Add(T); Tangents.Add(T); Tangents.Add(T); Tangents.Add(T);

    Tris = {0, 2, 1,  0, 3, 2};

    VideoMesh_->CreateMeshSection(
        0, Verts, Tris, Normals, UVs,
        TArray<FVector2D>{}, TArray<FVector2D>{}, TArray<FVector2D>{},
        TArray<FColor>{}, Tangents,
        false);

    if (VideoMatInst_)
        VideoMesh_->SetMaterial(0, VideoMatInst_);

    UE_LOG(LogPSXVideoSurface, Log,
        TEXT("Video surface plane rebuilt %.0fx%.0f UE units (%dx%d px)"), Pw, Ph, W, H);
}

void UPSXVideoSurfaceComponent::SetVideoVisible(bool bShow)
{
    bVideoVisible_ = bShow;
    const bool bShowLegacyMesh = bShow && !ShouldUseUnifiedSurfacePresenter();

    if (VideoMesh_)
    {
        VideoMesh_->SetVisibility(bShowLegacyMesh, /*bPropagateToChildren=*/false);
        VideoMesh_->SetHiddenInGame(!bShowLegacyMesh, /*bPropagateToChildren=*/false);
    }

    if (!bShow)
    {
        UE_LOG(LogPSXVideoSurface, Log, TEXT("Video surface hidden"));
        emu::logf(emu::LogLevel::debug, "VIDEO", "video plane hidden");
    }
    else
    {
        UE_LOG(LogPSXVideoSurface, Log, TEXT("Video surface shown (%dx%d 24-bit)"),
               VideoTexW_, VideoTexH_);
        emu::logf(emu::LogLevel::info, "VIDEO", "video plane shown %dx%d", VideoTexW_, VideoTexH_);
    }

    SyncUnifiedSurface(bShow);
}

void UPSXVideoSurfaceComponent::SyncUnifiedSurface(bool bSurfaceVisible)
{
    if (!bMirrorToUnifiedSurface)
        return;

    UPSXSurfaceComponent* Surface = SurfaceComp_.Get();
    if (!Surface)
    {
        Surface = GetOwner() ? GetOwner()->FindComponentByClass<UPSXSurfaceComponent>() : nullptr;
        SurfaceComp_ = Surface;
    }

    if (!Surface)
        return;

    FPSXSurfaceDecision Decision;
    Decision.bVisible = bSurfaceVisible && VideoTexture_ != nullptr;
    Decision.Mode = bSurfaceVisible ? EPSXSurfaceMode::VideoFmv : EPSXSurfaceMode::None;
    Decision.Shape = bSphereMode ? EPSXSurfaceShape::Hemisphere : EPSXSurfaceShape::Plane;
    if (Gpu_)
    {
        std::vector<gpu::CpuVramWriteInfo> RecentWrites;
        Gpu_->copy_recent_cpu_vram_writes(RecentWrites);
        Decision.Buffering = InferBufferingMode(RecentWrites, Gpu_->display_config(), Gpu_->vram_frame_count());
    }
    else
    {
        Decision.Buffering = EPSXSurfaceBufferingMode::AutoDetect;
    }
    Decision.PhysicalWidth = FMath::Max(PlaneWidth, 1.0f);
    Decision.AspectRatio = (VideoTexW_ > 0 && VideoTexH_ > 0)
        ? static_cast<float>(VideoTexW_) / static_cast<float>(VideoTexH_)
        : (4.0f / 3.0f);
    Decision.Distance = bSphereMode ? FMath::Max(SphereRadius, 10.0f) : Surface->Distance;

    Surface->SetSurfaceTexture(Decision.bVisible ? VideoTexture_ : nullptr);
    Surface->ApplyDecision(Decision);
}

bool UPSXVideoSurfaceComponent::ShouldUseUnifiedSurfacePresenter() const
{
    return bMirrorToUnifiedSurface && bPreferUnifiedSurfacePresenter && SurfaceComp_.IsValid();
}
