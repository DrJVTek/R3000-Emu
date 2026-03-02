#include "R3000VramViewerComponent.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Logging/LogMacros.h"

#include "gpu/gpu.h"
#include "log/emu_log.h"

DEFINE_LOG_CATEGORY_STATIC(LogR3000Vram, Log, All);

// ===================================================================
// Constructor
// ===================================================================
UR3000VramViewerComponent::UR3000VramViewerComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    SetMobility(EComponentMobility::Movable);
}

// ===================================================================
// BeginPlay
// ===================================================================
void UR3000VramViewerComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(true);
}

// ===================================================================
// EndPlay
// ===================================================================
void UR3000VramViewerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Gpu_ = nullptr;
    delete[] PixelBuffer_;
    PixelBuffer_ = nullptr;
    delete[] VramCopyBuffer_;
    VramCopyBuffer_ = nullptr;
    delete UpdateRegion_;
    UpdateRegion_ = nullptr;
    Super::EndPlay(EndPlayReason);
}

// ===================================================================
// BindGpu — called by R3000EmuComponent after core init
// ===================================================================
void UR3000VramViewerComponent::BindGpu(gpu::Gpu* InGpu)
{
    UE_LOG(LogR3000Vram, Log, TEXT("VramViewer: BindGpu(%p)"), InGpu);
    emu::logf(emu::LogLevel::info, "VRAM", "VramViewerComponent bound to GPU");

    Gpu_ = InGpu;

    if (!VramTexture_)
        CreateVramTexture();

    if (bShowViewer)
        CreateOrUpdateViewer();
}

// ===================================================================
// VRAM Texture creation
// ===================================================================
void UR3000VramViewerComponent::CreateVramTexture()
{
    VramTexture_ = UTexture2D::CreateTransient(kVramW, kVramH, PF_B8G8R8A8);
    if (!VramTexture_)
    {
        UE_LOG(LogR3000Vram, Error, TEXT("Failed to create VRAM texture"));
        return;
    }

    VramTexture_->Filter = TF_Nearest;
    VramTexture_->SRGB = false;
    VramTexture_->NeverStream = true;
#if WITH_EDITORONLY_DATA
    VramTexture_->MipGenSettings = TMGS_NoMipmaps;
#endif
    VramTexture_->UpdateResource();

    PixelBuffer_ = new uint8[kVramW * kVramH * 4];
    FMemory::Memzero(PixelBuffer_, kVramW * kVramH * 4);

    VramCopyBuffer_ = new uint16[kVramW * kVramH];
    FMemory::Memzero(VramCopyBuffer_, kVramW * kVramH * sizeof(uint16));

    UE_LOG(LogR3000Vram, Log, TEXT("VRAM texture created: %dx%d PF_B8G8R8A8"), kVramW, kVramH);
}

// ===================================================================
// VRAM Texture upload (16-bit → BGRA8) - only when dirty
// ===================================================================
void UR3000VramViewerComponent::UpdateVramTexture()
{
    if (!Gpu_ || !VramTexture_ || !PixelBuffer_ || !VramCopyBuffer_)
        return;

    // Thread-safe: check if VRAM changed before doing the full copy
    const uint32 CurrentSeq = Gpu_->vram_write_seq_locked();
    if (CurrentSeq == LastVramWriteSeq_)
        return;

    // Thread-safe VRAM copy
    uint32 CopySeq = 0;
    Gpu_->copy_vram(VramCopyBuffer_, CopySeq);
    LastVramWriteSeq_ = CopySeq;

    // Convert 16-bit VRAM to BGRA8 for texture
    // Layout: B = low byte, G = high byte, R = unused, A = 0xFF
    const uint16_t* Vram = VramCopyBuffer_;
    uint8* Dst = PixelBuffer_;
    const int32 NumPixels = kVramW * kVramH;
    for (int32 i = 0; i < NumPixels; ++i)
    {
        const uint16_t Px = Vram[i];
        Dst[0] = Px & 0xFF;
        Dst[1] = (Px >> 8) & 0xFF;
        Dst[2] = 0;
        Dst[3] = 0xFF;
        Dst += 4;
    }

    // Upload to RHI texture via UpdateTextureRegions (render-thread safe)
    if (!UpdateRegion_)
        UpdateRegion_ = new FUpdateTextureRegion2D(0, 0, 0, 0, kVramW, kVramH);

    VramTexture_->UpdateTextureRegions(
        0,              // MipIndex
        1,              // NumRegions
        UpdateRegion_,  // Region
        kVramW * 4,     // SrcPitch (bytes per row)
        4,              // SrcBpp (bytes per pixel: BGRA8)
        PixelBuffer_,   // SrcData
        [](uint8* /*SrcData*/, const FUpdateTextureRegion2D* /*Regions*/) {
            // No cleanup — we own PixelBuffer_ and UpdateRegion_ persistently
        });

    VramUploadCount_++;

    if (VramUploadCount_ <= 3)
    {
        UE_LOG(LogR3000Vram, Log, TEXT("VRAM upload #%d: seq %u"), VramUploadCount_, CopySeq);
    }
}

// ===================================================================
// Debug viewer quad
// ===================================================================
void UR3000VramViewerComponent::CreateOrUpdateViewer()
{
    if (!VramTexture_)
        return;

    if (!ViewerMesh_)
    {
        ViewerMesh_ = NewObject<UProceduralMeshComponent>(GetOwner(), TEXT("VramViewerMesh"));
        ViewerMesh_->bUseAsyncCooking = true;
        ViewerMesh_->SetCastShadow(false);
        ViewerMesh_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        // Attach to THIS SceneComponent — inherits our transform (pos/rot/scale)
        ViewerMesh_->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        ViewerMesh_->RegisterComponent();

        // Build quad at local origin, 1024x512 UE units (1:1 with VRAM pixels).
        // Use the component's Scale to resize in the viewport.
        const float W = static_cast<float>(kVramW);
        const float H = static_cast<float>(kVramH);
        const float HalfW = W * 0.5f;

        TArray<FVector> Verts;
        Verts.Add(FVector(0.0f, -HalfW, 0.0f));
        Verts.Add(FVector(0.0f,  HalfW, 0.0f));
        Verts.Add(FVector(0.0f,  HalfW, H));
        Verts.Add(FVector(0.0f, -HalfW, H));

        TArray<int32> Tris;
        Tris.Add(0); Tris.Add(2); Tris.Add(1);
        Tris.Add(0); Tris.Add(3); Tris.Add(2);

        TArray<FVector> Norms;
        for (int32 i = 0; i < 4; ++i)
            Norms.Add(FVector(-1, 0, 0));

        TArray<FVector2D> UVs;
        UVs.Add(FVector2D(0.0f, 1.0f));
        UVs.Add(FVector2D(1.0f, 1.0f));
        UVs.Add(FVector2D(1.0f, 0.0f));
        UVs.Add(FVector2D(0.0f, 0.0f));

        TArray<FLinearColor> Colors;
        for (int32 i = 0; i < 4; ++i)
            Colors.Add(FLinearColor::White);

        TArray<FProcMeshTangent> Tangents;
        for (int32 i = 0; i < 4; ++i)
            Tangents.Add(FProcMeshTangent(0, 1, 0));

        ViewerMesh_->CreateMeshSection_LinearColor(
            0, Verts, Tris, Norms, UVs, Colors, Tangents, false);

        bViewerCreated_ = true;
    }

    if (!ViewerMatInst_)
    {
        UMaterialInterface* ViewerBase = ViewerMaterial;
        if (ViewerBase)
        {
            ViewerMatInst_ = UMaterialInstanceDynamic::Create(ViewerBase, this);
            if (ViewerMatInst_)
                ViewerMatInst_->SetTextureParameterValue(TEXT("VramTexture"), VramTexture_);
        }
    }

    if (ViewerMatInst_)
        ViewerMesh_->SetMaterial(0, ViewerMatInst_);

    ViewerMesh_->SetVisibility(true);
}

void UR3000VramViewerComponent::DestroyViewer()
{
    if (ViewerMesh_)
    {
        ViewerMesh_->SetVisibility(false);
        ViewerMesh_->ClearAllMeshSections();
    }
    bViewerCreated_ = false;
}

void UR3000VramViewerComponent::SetViewerVisible(bool bNewVisible)
{
    bShowViewer = bNewVisible;
    if (bNewVisible && Gpu_)
        CreateOrUpdateViewer();
    else
        DestroyViewer();
}

// ===================================================================
// TickComponent — update VRAM texture + viewer toggle/transform
// ===================================================================
void UR3000VramViewerComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!Gpu_)
        return;

    // Always upload VRAM texture (shared with other components)
    UpdateVramTexture();

    // Viewer toggle (transform is handled by the SceneComponent hierarchy)
    if (bShowViewer && !bViewerCreated_)
        CreateOrUpdateViewer();
    else if (!bShowViewer && bViewerCreated_)
        DestroyViewer();
}
