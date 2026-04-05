#include "R3000ImageComponent.h"
#include "R3000GpuComponent.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Logging/LogMacros.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "gpu/gpu.h"
#include "log/emu_log.h"

DEFINE_LOG_CATEGORY_STATIC(LogR3000Image, Log, All);

namespace
{
static uint8 Expand5To8(uint8 v)
{
    return static_cast<uint8>((v << 3) | (v >> 2));
}

static uint16 DisplayRowWords(const gpu::DisplayConfig& Disp)
{
    const uint16 DisplayW = Disp.width();
    if (!Disp.color_24bit)
        return DisplayW;

    return static_cast<uint16>(((static_cast<uint32>(DisplayW) * 3u) + 1u) / 2u);
}
}

UR3000ImageComponent::UR3000ImageComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    SetMobility(EComponentMobility::Movable);
}

void UR3000ImageComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(true);
}

void UR3000ImageComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Gpu_ = nullptr;
    delete[] VramCopyBuffer_; VramCopyBuffer_ = nullptr;
    delete[] PixelBuffer_; PixelBuffer_ = nullptr;
    delete UpdateRegion_; UpdateRegion_ = nullptr;
    Super::EndPlay(EndPlayReason);
}

void UR3000ImageComponent::BindGpu(gpu::Gpu* InGpu)
{
    Gpu_ = InGpu;
    Gpu2DComp_ = GetOwner() ? GetOwner()->FindComponentByClass<UR3000GpuComponent>() : nullptr;
    if (!VramCopyBuffer_)
        VramCopyBuffer_ = new uint16[kVramW * kVramH];

    emu::logf(emu::LogLevel::info, "IMAGE", "R3000ImageComponent bound to GPU");
}

bool UR3000ImageComponent::IsWriteEligible(const gpu::CpuVramWriteInfo& Write) const
{
    if (Write.seq == 0)
        return false;

    if (!Write.display.display_enabled)
        return false;

    const uint16_t DisplayW = Write.display.width();
    const uint16_t DisplayH = Write.display.height();
    const uint16_t ExpectedDmaW = DisplayRowWords(Write.display);
    if (DisplayW == 0 || DisplayH == 0)
        return false;

    return Write.x == Write.display.display_x &&
           Write.y == Write.display.display_y &&
           Write.w == ExpectedDmaW &&
           Write.h == DisplayH;
}

bool UR3000ImageComponent::DoesCurrentDisplayMatchLatch() const
{
    if (!Gpu_)
        return false;

    const gpu::DisplayConfig& Disp = Gpu_->display_config();
    return Disp.display_x == LatchedDisplayX_ &&
           Disp.display_y == LatchedDisplayY_ &&
           Disp.width() == LatchedDisplayW_ &&
           Disp.height() == LatchedDisplayH_ &&
           Disp.color_24bit == LatchedDisplay24Bit_ &&
           Disp.display_enabled == LatchedDisplayEnabled_;
}

void UR3000ImageComponent::TickComponent(
    float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!Gpu_ || !VramCopyBuffer_)
        return;

    const gpu::DisplayConfig& Disp = Gpu_->display_config();
    if (!Disp.display_enabled || Disp.width() == 0 || Disp.height() == 0)
    {
        if (bImageVisible_)
            SetImageVisible(false);
        return;
    }

    if (bImageVisible_ && !DoesCurrentDisplayMatchLatch())
    {
        UE_LOG(LogR3000Image, Log,
            TEXT("ImageComponent: hiding latched image after display change old=(%u,%u %ux%u 24=%d en=%d) new=(%u,%u %ux%u 24=%d en=%d)"),
            LatchedDisplayX_, LatchedDisplayY_, LatchedDisplayW_, LatchedDisplayH_,
            LatchedDisplay24Bit_ ? 1 : 0, LatchedDisplayEnabled_ ? 1 : 0,
            Disp.display_x, Disp.display_y, Disp.width(), Disp.height(),
            Disp.color_24bit ? 1 : 0, Disp.display_enabled ? 1 : 0);
        SetImageVisible(false);
    }

    // Hide image when GPU draws primitives for multiple consecutive frames
    // (game switched to 3D rendering). Skip first 3 frames after showing
    // to avoid hiding during the same frame as the CPU→VRAM write.
    if (bImageVisible_)
    {
        ++ImageVisibleFrames_;
        if (ImageVisibleFrames_ > 3)
        {
            const auto& Stats = Gpu_->frame_stats();
            if (Stats.triangles > 0 || Stats.quads > 0 || Stats.lines > 0)
            {
                UE_LOG(LogR3000Image, Log,
                    TEXT("ImageComponent: hiding image — GPU drawing primitives (tri=%u quad=%u line=%u after %u frames)"),
                    Stats.triangles, Stats.quads, Stats.lines, ImageVisibleFrames_);
                SetImageVisible(false);
            }
        }
    }

    if (bImageVisible_ && DoesCurrentDisplayMatchLatch())
    {
        const uint32 CurrentVramSeq = Gpu_->vram_write_seq_locked();
        if (CurrentVramSeq != 0 && CurrentVramSeq != LastUploadedVramSeq_)
        {
            UE_LOG(LogR3000Image, Warning,
                TEXT("ImageComponent: refresh visible image vram_seq %u -> %u disp=(%u,%u %ux%u 24=%d)"),
                LastUploadedVramSeq_, CurrentVramSeq,
                Disp.display_x, Disp.display_y, Disp.width(), Disp.height(),
                Disp.color_24bit ? 1 : 0);
            UploadImageFrame(Disp.width(), Disp.height());
            LastUploadedVramSeq_ = CurrentVramSeq;
        }
    }

    gpu::CpuVramWriteInfo Write{};
    Gpu_->copy_last_cpu_vram_write(Write);
    if (Write.seq == 0 || Write.seq == LastSeenCpuWriteSeq_)
        return;

    LastSeenCpuWriteSeq_ = Write.seq;
    UE_LOG(LogR3000Image, Warning,
        TEXT("ImageComponent: cpu->vram write seq=%u vram_seq=%u frame=%u dma=(%u,%u %ux%u) disp=(%u,%u %ux%u 24=%d en=%d) eligible=%d"),
        Write.seq, Write.vram_write_seq, Write.frame_count,
        Write.x, Write.y, Write.w, Write.h,
        Write.display.display_x, Write.display.display_y,
        Write.display.width(), Write.display.height(),
        Write.display.color_24bit ? 1 : 0,
        Write.display.display_enabled ? 1 : 0,
        IsWriteEligible(Write) ? 1 : 0);

    if (!IsWriteEligible(Write))
        return;

    if (Write.display.display_x != Disp.display_x ||
        Write.display.display_y != Disp.display_y ||
        Write.display.width() != Disp.width() ||
        Write.display.height() != Disp.height() ||
        Write.display.color_24bit != Disp.color_24bit ||
        Write.display.display_enabled != Disp.display_enabled)
    {
        return;
    }

    const int32 W = Disp.width();
    const int32 H = Disp.height();
    if (W != ImageTexW_ || H != ImageTexH_)
    {
        CreateOrResizeTexture(W, H);
        RebuildPlaneMesh(W, H);
    }

    UploadImageFrame(W, H);

    // Don't show if content is mostly black (VRAM not ready or empty MDEC init)
    if (!bHasRealContent_)
        return;

    LastUploadedVramSeq_ = Write.vram_write_seq;
    LatchedDisplayX_ = Disp.display_x;
    LatchedDisplayY_ = Disp.display_y;
    LatchedDisplayW_ = static_cast<uint16>(W);
    LatchedDisplayH_ = static_cast<uint16>(H);
    LatchedDisplay24Bit_ = Disp.color_24bit;
    LatchedDisplayEnabled_ = Disp.display_enabled;

    UE_LOG(LogR3000Image, Warning,
        TEXT("ImageComponent: IMAGE DETECTED write_seq=%u vram_seq=%u frame=%u dma_xy=(%u,%u) dma_wh=%ux%u disp_xy=(%u,%u) w=%u h=%u depth=%d"),
        Write.seq, Write.vram_write_seq, Write.frame_count,
        Write.x, Write.y, Write.w, Write.h,
        Disp.display_x, Disp.display_y, Disp.width(), Disp.height(),
        Disp.color_24bit ? 24 : 15);

    if (!bImageVisible_)
        SetImageVisible(true);
}

void UR3000ImageComponent::CreateOrResizeTexture(int32 W, int32 H)
{
    delete[] PixelBuffer_; PixelBuffer_ = nullptr;
    delete UpdateRegion_; UpdateRegion_ = nullptr;

    ImageTexW_ = W;
    ImageTexH_ = H;

    ImageTexture_ = UTexture2D::CreateTransient(W, H, PF_R8G8B8A8, TEXT("PS1_Image"));
    if (!ImageTexture_)
    {
        UE_LOG(LogR3000Image, Error, TEXT("ImageComponent: failed to create texture %dx%d"), W, H);
        return;
    }

    ImageTexture_->Filter = TF_Nearest;
    ImageTexture_->AddressX = TA_Clamp;
    ImageTexture_->AddressY = TA_Clamp;
    ImageTexture_->SRGB = false;
    ImageTexture_->NeverStream = true;
#if WITH_EDITORONLY_DATA
    ImageTexture_->MipGenSettings = TMGS_NoMipmaps;
#endif
    ImageTexture_->UpdateResource();

    PixelBuffer_ = new uint8[W * H * 4];
    FMemory::Memzero(PixelBuffer_, W * H * 4);
    UpdateRegion_ = new FUpdateTextureRegion2D(0, 0, 0, 0, static_cast<uint32>(W), static_cast<uint32>(H));

    if (ImageMaterial)
    {
        if (!ImageMatInst_ || ImageMatInst_->GetBaseMaterial() != ImageMaterial->GetBaseMaterial())
            ImageMatInst_ = UMaterialInstanceDynamic::Create(ImageMaterial, this);

        ImageMatInst_->SetTextureParameterValue(TEXT("ImageTexture"), ImageTexture_);
        ImageMatInst_->SetTextureParameterValue(TEXT("VideoTexture"), ImageTexture_);
        ImageMatInst_->SetTextureParameterValue(TEXT("FrameTexture"), ImageTexture_);
        if (ImageMesh_)
            ImageMesh_->SetMaterial(0, ImageMatInst_);
    }

    UE_LOG(LogR3000Image, Log, TEXT("ImageComponent: texture %dx%d created"), W, H);
    UE_LOG(LogR3000Image, Log, TEXT("ImageComponent: material=%d matinst=%d mesh=%d"),
        ImageMaterial ? 1 : 0,
        ImageMatInst_ ? 1 : 0,
        ImageMesh_ ? 1 : 0);
}

void UR3000ImageComponent::UploadImageFrame(int32 W, int32 H)
{
    if (!ImageTexture_ || !PixelBuffer_ || !UpdateRegion_)
        return;

    uint32 CopySeq = 0;
    Gpu_->copy_vram(VramCopyBuffer_, CopySeq);

    const gpu::DisplayConfig& Disp = Gpu_->display_config();
    uint8* Dst = PixelBuffer_;
    uint32 NonBlackPixels = 0;
    uint8 MinR = 255, MinG = 255, MinB = 255;
    uint8 MaxR = 0, MaxG = 0, MaxB = 0;
    if (Disp.color_24bit)
    {
        const uint8* VramBytes = reinterpret_cast<const uint8*>(VramCopyBuffer_);
        constexpr uint32 kRowStrideBytes = kVramW * 2u;
        const uint32 BaseOff = static_cast<uint32>(Disp.display_y) * kRowStrideBytes
                             + static_cast<uint32>(Disp.display_x) * 2u;

        for (int32 y = 0; y < H; ++y)
        {
            const uint8* Row = VramBytes + BaseOff + static_cast<uint32>(y) * kRowStrideBytes;
            for (int32 x = 0; x < W; ++x)
            {
                const uint8* Px = Row + x * 3;
                Dst[0] = Px[0];
                Dst[1] = Px[1];
                Dst[2] = Px[2];
                Dst[3] = 0xFF;
                if (Dst[0] || Dst[1] || Dst[2])
                    ++NonBlackPixels;
                MinR = FMath::Min(MinR, Dst[0]);
                MinG = FMath::Min(MinG, Dst[1]);
                MinB = FMath::Min(MinB, Dst[2]);
                MaxR = FMath::Max(MaxR, Dst[0]);
                MaxG = FMath::Max(MaxG, Dst[1]);
                MaxB = FMath::Max(MaxB, Dst[2]);
                Dst += 4;
            }
        }
    }
    else
    {
        for (int32 y = 0; y < H; ++y)
        {
            const uint32_t Vy = (static_cast<uint32_t>(Disp.display_y) + static_cast<uint32_t>(y)) % kVramH;
            for (int32 x = 0; x < W; ++x)
            {
                const uint32_t Vx = (static_cast<uint32_t>(Disp.display_x) + static_cast<uint32_t>(x)) % kVramW;
                const uint16 Pixel = VramCopyBuffer_[Vy * kVramW + Vx];
                Dst[0] = Expand5To8(static_cast<uint8>(Pixel & 0x1F));
                Dst[1] = Expand5To8(static_cast<uint8>((Pixel >> 5) & 0x1F));
                Dst[2] = Expand5To8(static_cast<uint8>((Pixel >> 10) & 0x1F));
                Dst[3] = 0xFF;
                if (Dst[0] || Dst[1] || Dst[2])
                    ++NonBlackPixels;
                MinR = FMath::Min(MinR, Dst[0]);
                MinG = FMath::Min(MinG, Dst[1]);
                MinB = FMath::Min(MinB, Dst[2]);
                MaxR = FMath::Max(MaxR, Dst[0]);
                MaxG = FMath::Max(MaxG, Dst[1]);
                MaxB = FMath::Max(MaxB, Dst[2]);
                Dst += 4;
            }
        }
    }

    // Check if frame has real content (>5% non-black)
    bHasRealContent_ = (NonBlackPixels > static_cast<uint32>(W * H / 20));

    bool bUploadedToTexture = false;
    if (FTexturePlatformData* PlatformData = ImageTexture_->GetPlatformData())
    {
        if (PlatformData->Mips.Num() > 0)
        {
            FTexture2DMipMap& Mip = PlatformData->Mips[0];
            void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
            if (Dest)
            {
                FMemory::Memcpy(Dest, PixelBuffer_, static_cast<SIZE_T>(W) * static_cast<SIZE_T>(H) * 4u);
                bUploadedToTexture = true;
            }
            Mip.BulkData.Unlock();
        }
    }

    if (bUploadedToTexture)
    {
        ImageTexture_->UpdateResource();
    }
    else
    {
        ImageTexture_->UpdateTextureRegions(
            0,
            1,
            UpdateRegion_,
            static_cast<uint32>(W) * 4,
            4,
            PixelBuffer_,
            [](uint8* /*Data*/, const FUpdateTextureRegion2D* /*Region*/) {});
    }

    static int32 sUploadLogCount = 0;
    if (sUploadLogCount < 24)
    {
        ++sUploadLogCount;
        const int32 CenterX = FMath::Clamp(W / 2, 0, W - 1);
        const int32 CenterY = FMath::Clamp(H / 2, 0, H - 1);
        const uint8* Center = PixelBuffer_ + ((CenterY * W) + CenterX) * 4;
        UE_LOG(LogR3000Image, Warning,
            TEXT("ImageComponent: upload seq=%u disp=(%u,%u %ux%u 24=%d) nonblack=%u/%d rgb_min=(%u,%u,%u) rgb_max=(%u,%u,%u) center=(%u,%u,%u) matinst=%d bulk=%d"),
            CopySeq,
            Disp.display_x, Disp.display_y, Disp.width(), Disp.height(),
            Disp.color_24bit ? 1 : 0,
            NonBlackPixels, W * H,
            MinR, MinG, MinB,
            MaxR, MaxG, MaxB,
            Center[0], Center[1], Center[2],
            ImageMatInst_ ? 1 : 0,
            bUploadedToTexture ? 1 : 0);
    }

    if (DumpedImageFrames_ < 8)
    {
        DumpImageFrameToPpm(W, H, CopySeq);
        ++DumpedImageFrames_;
    }
}

void UR3000ImageComponent::DumpImageFrameToPpm(int32 W, int32 H, uint32 VramSeq)
{
    if (!PixelBuffer_ || W <= 0 || H <= 0)
        return;

    TArray<uint8> Out;
    const FString Header = FString::Printf(TEXT("P6\n%d %d\n255\n"), W, H);
    FTCHARToUTF8 HeaderUtf8(*Header);
    Out.Append(reinterpret_cast<const uint8*>(HeaderUtf8.Get()), HeaderUtf8.Length());

    Out.Reserve(Out.Num() + (W * H * 3));
    for (int32 y = 0; y < H; ++y)
    {
        const uint8* Row = PixelBuffer_ + (y * W * 4);
        for (int32 x = 0; x < W; ++x)
        {
            const uint8* Px = Row + x * 4;
            Out.Add(Px[0]);
            Out.Add(Px[1]);
            Out.Add(Px[2]);
        }
    }

    const gpu::DisplayConfig& Disp = Gpu_->display_config();
    const FString Path = FPaths::Combine(
        FPaths::ProjectLogDir(),
        FString::Printf(TEXT("image_dump_%02u_seq%u_%ux%u_disp_%u_%u_%u.ppm"),
            DumpedImageFrames_,
            VramSeq,
            W,
            H,
            Disp.display_x,
            Disp.display_y,
            Disp.color_24bit ? 24u : 15u));

    if (FFileHelper::SaveArrayToFile(Out, *Path))
    {
        UE_LOG(LogR3000Image, Warning, TEXT("ImageComponent: dumped frame to %s"), *Path);
    }
    else
    {
        UE_LOG(LogR3000Image, Error, TEXT("ImageComponent: failed to dump frame to %s"), *Path);
    }
}

void UR3000ImageComponent::SetGpu2DVisible(bool bGpuVisible)
{
    UR3000GpuComponent* Gpu2D = Gpu2DComp_.Get();
    if (!Gpu2D)
        return;

    Gpu2D->SetVisibility(bGpuVisible, true);
    if (UProceduralMeshComponent* Mesh = Gpu2D->GetMeshComponent())
    {
        Mesh->SetVisibility(bGpuVisible, true);
        Mesh->SetHiddenInGame(!bGpuVisible, true);
    }
}

void UR3000ImageComponent::RebuildPlaneMesh(int32 W, int32 H)
{
    if (!ImageMesh_)
    {
        ImageMesh_ = NewObject<UProceduralMeshComponent>(GetOwner(), TEXT("ImageMesh"));
        ImageMesh_->bUseAsyncCooking = false;
        ImageMesh_->SetCastShadow(false);
        ImageMesh_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        ImageMesh_->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        ImageMesh_->RegisterComponent();
        ImageMesh_->SetTranslucentSortPriority(100);
        ImageMesh_->SetHiddenInGame(false, false);
        ImageMesh_->SetVisibility(true, false);
    }

    const float Pw = FMath::Max(PlaneWidth, 1.0f);
    const float Ph = (H > 0 && W > 0) ? Pw * static_cast<float>(H) / static_cast<float>(W) : Pw * 0.75f;
    const float HalfW = Pw * 0.5f;

    TArray<FVector> Verts;
    TArray<int32> Tris;
    TArray<FVector2D> UVs;
    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;

    Verts.Add(FVector(0.f, -HalfW, Ph));
    Verts.Add(FVector(0.f, HalfW, Ph));
    Verts.Add(FVector(0.f, HalfW, 0.f));
    Verts.Add(FVector(0.f, -HalfW, 0.f));

    UVs.Add(FVector2D(0.f, 0.f));
    UVs.Add(FVector2D(1.f, 0.f));
    UVs.Add(FVector2D(1.f, 1.f));
    UVs.Add(FVector2D(0.f, 1.f));

    const FVector Normal(1.f, 0.f, 0.f);
    Normals.Add(Normal); Normals.Add(Normal); Normals.Add(Normal); Normals.Add(Normal);

    const FProcMeshTangent Tangent(FVector(0.f, 1.f, 0.f), false);
    Tangents.Add(Tangent); Tangents.Add(Tangent); Tangents.Add(Tangent); Tangents.Add(Tangent);

    Tris = {0, 2, 1, 0, 3, 2};

    ImageMesh_->CreateMeshSection(
        0, Verts, Tris, Normals, UVs,
        TArray<FVector2D>{}, TArray<FVector2D>{}, TArray<FVector2D>{},
        TArray<FColor>{}, Tangents,
        false);

    if (ImageMatInst_)
        ImageMesh_->SetMaterial(0, ImageMatInst_);

    ImageMesh_->SetTranslucentSortPriority(100);

    UE_LOG(LogR3000Image, Log, TEXT("ImageComponent: plane rebuilt %.0fx%.0f UE units (%dx%d px)"), Pw, Ph, W, H);
}

void UR3000ImageComponent::SetImageVisible(bool bShow)
{
    bImageVisible_ = bShow;
    ImageVisibleFrames_ = 0;
    if (ImageMesh_)
    {
        ImageMesh_->SetVisibility(bShow, false);
        ImageMesh_->SetHiddenInGame(!bShow, false);
        ImageMesh_->SetTranslucentSortPriority(100);
        if (bShow && ImageMatInst_)
            ImageMesh_->SetMaterial(0, ImageMatInst_);
    }

    if (bHideGpu2DWhileImageVisible)
        SetGpu2DVisible(!bShow);

    if (bShow)
    {
        UE_LOG(LogR3000Image, Log, TEXT("ImageComponent: plane shown (%dx%d 15-bit)"), ImageTexW_, ImageTexH_);
        emu::logf(emu::LogLevel::info, "IMAGE", "image plane shown %dx%d", ImageTexW_, ImageTexH_);
    }
    else
    {
        UE_LOG(LogR3000Image, Log, TEXT("ImageComponent: plane hidden"));
        emu::logf(emu::LogLevel::debug, "IMAGE", "image plane hidden");
    }
}
