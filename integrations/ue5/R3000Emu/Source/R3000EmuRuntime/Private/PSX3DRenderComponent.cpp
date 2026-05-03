#include "PSX3DRenderComponent.h"
#include "PSXCameraDebugActor.h"
#include "PSX3DTrackingController.h"

#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Logging/LogMacros.h"
#include "Math/RotationMatrix.h"

#include "gpu/gpu.h"
#include "gpu/gpu_3d.h"
#include "log/emu_log.h"

DEFINE_LOG_CATEGORY_STATIC(LogR3000Gpu3D, Log, All);

// Set to 1 to keep only Warning/Error logs from this component.
#ifndef R3000_GPU3D_WARNINGS_ONLY
#define R3000_GPU3D_WARNINGS_ONLY 1
#endif

#if R3000_GPU3D_WARNINGS_ONLY
#define GPU3D_NOISE_UELOG(Verbosity, Format, ...) do {} while (0)
#define GPU3D_NOISE_LOGF(...) do {} while (0)
#else
#define GPU3D_NOISE_UELOG(Verbosity, Format, ...) UE_LOG(LogR3000Gpu3D, Verbosity, Format, ##__VA_ARGS__)
#define GPU3D_NOISE_LOGF(...) emu::logf(__VA_ARGS__)
#endif

namespace
{
static FVector MapPsxToUeVector(const FVector& V)
{
    return FVector(V.Y, -V.X, V.Z);
}

static bool TryBuildPsxCameraLocalTransform(
    const gpu::Gpu3D* Gpu3D,
    float WorldScale,
    const FVector& WorldOffset,
    FTransform& OutTransform,
    FString* OutDebugText = nullptr)
{
    if (!Gpu3D)
        return false;

    gpu::FrameDrawList DrawList;
    Gpu3D->copy_ready_draw_list(DrawList);

    for (const gpu::DrawCmd3D& Cmd3D : DrawList.cmds_3d)
    {
        if (Cmd3D.origin != gpu::PrimOrigin::origin_3d)
            continue;

        const gte::GteTransform& T = Cmd3D.transform;
        const FVector BasisX(
            static_cast<float>(T.rt[0]) / 4096.0f,
            static_cast<float>(T.rt[3]) / 4096.0f,
            static_cast<float>(T.rt[6]) / 4096.0f);
        const FVector BasisY(
            static_cast<float>(T.rt[1]) / 4096.0f,
            static_cast<float>(T.rt[4]) / 4096.0f,
            static_cast<float>(T.rt[7]) / 4096.0f);
        const FVector BasisZ(
            static_cast<float>(T.rt[2]) / 4096.0f,
            static_cast<float>(T.rt[5]) / 4096.0f,
            static_cast<float>(T.rt[8]) / 4096.0f);
        const FVector Translation(
            static_cast<float>(T.tr[0]),
            static_cast<float>(T.tr[1]),
            static_cast<float>(T.tr[2]));

        const FVector CameraPosPsx(
            -FVector::DotProduct(BasisX, Translation),
            -FVector::DotProduct(BasisY, Translation),
            -FVector::DotProduct(BasisZ, Translation));

        const FVector Forward = MapPsxToUeVector(BasisZ).GetSafeNormal();
        const FVector Up = MapPsxToUeVector(BasisY).GetSafeNormal();
        if (Forward.IsNearlyZero() || Up.IsNearlyZero())
            continue;

        const FVector CameraPosUe = MapPsxToUeVector(CameraPosPsx) * WorldScale + WorldOffset;
        OutTransform = FTransform(
            FRotationMatrix::MakeFromXZ(Forward, Up).ToQuat(),
            CameraPosUe);
        if (OutDebugText)
        {
            const uint16 NearSz = FMath::Min3(Cmd3D.sz[0], Cmd3D.sz[1], Cmd3D.sz[2]);
            *OutDebugText = FString::Printf(
                TEXT("PSX Cam\nnear(sz)=%u\notz=%u"),
                static_cast<unsigned>(NearSz),
                static_cast<unsigned>(Cmd3D.ot_z));
        }
        return true;
    }

    return false;
}
} // namespace

// ===================================================================
// Constructor
// ===================================================================
UPSX3DRenderComponent::UPSX3DRenderComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    SetMobility(EComponentMobility::Movable);
}

// ===================================================================
// BeginPlay
// ===================================================================
void UPSX3DRenderComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(true);
    if (!TrackingController_)
        TrackingController_ = new FPSX3DTrackingController(*this);
}

// ===================================================================
// EndPlay
// ===================================================================
void UPSX3DRenderComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Gpu_ = nullptr;
    Gpu3D_ = nullptr;
    if (SpawnedPsxCameraDebugActor_)
    {
        SpawnedPsxCameraDebugActor_->Destroy();
        SpawnedPsxCameraDebugActor_ = nullptr;
    }
    delete TrackingController_;
    TrackingController_ = nullptr;
    Super::EndPlay(EndPlayReason);
}

AActor* UPSX3DRenderComponent::ResolvePsxCameraDebugActor()
{
    if (PsxCameraDebugActor.IsValid())
        return PsxCameraDebugActor.Get();

    if (SpawnedPsxCameraDebugActor_)
        return SpawnedPsxCameraDebugActor_;

    if (!bAutoSpawnPsxCameraDebugActor || !GetWorld())
        return nullptr;

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Params.Name = TEXT("PSXCameraDebug3D");
    SpawnedPsxCameraDebugActor_ = GetWorld()->SpawnActor<APSXCameraDebugActor>(
        APSXCameraDebugActor::StaticClass(),
        GetComponentLocation(),
        GetComponentRotation(),
        Params);
    return SpawnedPsxCameraDebugActor_;
}

void UPSX3DRenderComponent::UpdatePsxCameraDebugActor()
{
    if (!bShowPsxCameraDebug)
    {
        if (PsxCameraDebugActor.IsValid())
            PsxCameraDebugActor.Get()->SetActorHiddenInGame(true);
        if (SpawnedPsxCameraDebugActor_)
            SpawnedPsxCameraDebugActor_->SetActorHiddenInGame(true);
        return;
    }

    AActor* CameraActor = ResolvePsxCameraDebugActor();
    if (!CameraActor)
        return;

    CameraActor->SetActorHiddenInGame(false);

    FTransform LocalCameraTransform;
    FString CameraDebugText(TEXT("PSX Cam\nnear(sz)=n/a"));
    const bool bUseApproxWorldCamera =
        bApplyGteTransform && bApproxWorldFromFrameRef &&
        TryBuildPsxCameraLocalTransform(Gpu3D_, WorldScale, WorldOffset, LocalCameraTransform, &CameraDebugText);

    if (bUseApproxWorldCamera)
    {
        const FTransform ComponentTransform = GetComponentTransform();
        CameraActor->SetActorTransform(FTransform(
            ComponentTransform.TransformRotation(LocalCameraTransform.GetRotation()),
            ComponentTransform.TransformPosition(LocalCameraTransform.GetLocation())));
    }
    else
    {
        CameraActor->SetActorTransform(FTransform(GetComponentQuat(), GetComponentLocation() + WorldOffset));
    }

    if (APSXCameraDebugActor* DebugActor = Cast<APSXCameraDebugActor>(CameraActor))
    {
        DebugActor->SetDebugColor(FColor(255, 170, 0));
        DebugActor->SetDebugText(CameraDebugText);
    }
}

// ===================================================================
// BindGpu
// ===================================================================
void UPSX3DRenderComponent::BindGpu(gpu::Gpu* InGpu)
{
    GPU3D_NOISE_UELOG(Log, TEXT("GPU3D v2 BindGpu called. InGpu=%p WorldScale=%.3f bSkip2D=%d"),
        InGpu, WorldScale, bSkip2DElements ? 1 : 0);
    GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D", "Gpu3DComponent bound (WorldScale=%.3f, bSkip2D=%d)",
        (double)WorldScale, bSkip2DElements ? 1 : 0);

    Gpu_ = InGpu;

    // Create ProceduralMeshComponent (attached to this SceneComponent)
    if (!MeshComp_)
    {
        MeshComp_ = NewObject<UProceduralMeshComponent>(GetOwner(), TEXT("PSX3DMesh"));
        MeshComp_->bUseAsyncCooking = true;
        MeshComp_->SetCastShadow(false);
        MeshComp_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComp_->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        MeshComp_->RegisterComponent();
        MeshComp_->SetVisibility(bEnabled);
        MeshComp_->SetHiddenInGame(!bEnabled);
    }

    EnsureMaterialInstances();

    if (!Mat3D_Opaque && !Mat2D_Opaque)
    {
        UE_LOG(LogR3000Gpu3D, Warning, TEXT("No materials assigned — 3D mesh will be invisible until Mat2D/Mat3D materials are set."));
    }
}

// ===================================================================
// BindGpu3D — shadow GPU for 3D reconstruction
// ===================================================================
void UPSX3DRenderComponent::BindGpu3D(gpu::Gpu3D* InGpu3D)
{
    Gpu3D_ = InGpu3D;
    GPU3D_NOISE_UELOG(Log, TEXT("GPU3D BindGpu3D: shadow=%p"), InGpu3D);
}

// ===================================================================
// SetVramTexture — receive shared texture from 2D component
// ===================================================================
void UPSX3DRenderComponent::SetVramTexture(UTexture2D* InTexture)
{
    VramTexture_ = InTexture;

    // Update existing material instances with the new texture
    for (int32 s = 0; s < MatInst_.Num(); ++s)
    {
        if (MatInst_[s] && VramTexture_)
            MatInst_[s]->SetTextureParameterValue(TEXT("VramTexture"), VramTexture_);
    }

    GPU3D_NOISE_UELOG(Log, TEXT("SetVramTexture: %p"), InTexture);
}

void UPSX3DRenderComponent::RecenterToPlayerView()
{
    if (!TrackingController_ || !MeshComp_)
        return;

    TrackingController_->RecenterToPlayerView(MeshComp_, VrViewOffset, bMeshDetachedForWorldLock_);
}

// ===================================================================
// Material instance management
// ===================================================================
void UPSX3DRenderComponent::EnsureMaterialInstances()
{
    UMaterialInterface* Wanted[kNumSections] = {
        // 2D sections 0-4
        Mat2D_Opaque,
        Mat2D_Semi0 ? Mat2D_Semi0 : Mat2D_Opaque,
        Mat2D_Semi1 ? Mat2D_Semi1 : Mat2D_Opaque,
        Mat2D_Semi2 ? Mat2D_Semi2 : Mat2D_Opaque,
        Mat2D_Semi3 ? Mat2D_Semi3 : Mat2D_Opaque,
        // 3D sections 5-9
        Mat3D_Opaque,
        Mat3D_Semi0 ? Mat3D_Semi0 : Mat3D_Opaque,
        Mat3D_Semi1 ? Mat3D_Semi1 : Mat3D_Opaque,
        Mat3D_Semi2 ? Mat3D_Semi2 : Mat3D_Opaque,
        Mat3D_Semi3 ? Mat3D_Semi3 : Mat3D_Opaque,
    };

    if (MatInst_.Num() != kNumSections)
    {
        MatInst_.SetNum(kNumSections);
        MatInstSource_.SetNum(kNumSections);
        for (int32 s = 0; s < kNumSections; ++s)
        {
            MatInst_[s] = nullptr;
            MatInstSource_[s] = nullptr;
        }
    }

    for (int32 s = 0; s < kNumSections; ++s)
    {
        if (!Wanted[s])
            continue;
        if (MatInst_[s] && MatInstSource_[s] == Wanted[s])
            continue;

        MatInst_[s] = UMaterialInstanceDynamic::Create(Wanted[s], this);
        MatInstSource_[s] = Wanted[s];
        if (MatInst_[s] && VramTexture_)
            MatInst_[s]->SetTextureParameterValue(TEXT("VramTexture"), VramTexture_);
    }
}

// ===================================================================
// TickComponent
// ===================================================================
void UPSX3DRenderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    // Diagnostic: confirm tick is running (uses frame_count to avoid static-local stale across PIE sessions)
    const uint32 TickFrame = Gpu3D_ ? Gpu3D_->frame_count() : 0;
    if (TickFrame <= 5 || (TickFrame % 300) == 0)
    {
        GPU3D_NOISE_UELOG(Verbose, TEXT("GPU3D Tick frame=%u: Gpu3D_=%p MeshComp_=%p bEnabled=%d"),
            TickFrame, Gpu3D_, MeshComp_, bEnabled ? 1 : 0);
    }

    if (!Gpu3D_ || !MeshComp_)
        return;

    if (TrackingController_)
    {
        TrackingController_->Update(
            MeshComp_,
            TrackingMode,
            bAutoRecenterVrOnModeEnter,
            bTrackVrViewEveryTick,
            VrViewOffset,
            bMeshDetachedForWorldLock_);
    }

    // Toggle visibility based on bEnabled
    if (!bEnabled)
    {
        UpdatePsxCameraDebugActor();
        if (MeshComp_->IsVisible())
            MeshComp_->SetVisibility(false);
        return;
    }

    if (!MeshComp_->IsVisible())
        MeshComp_->SetVisibility(true);

    // Rebuild every tick — no frame gate, no VRAM sync needed.
    // Positions are double-buffered in Gpu3D (copy_ready_draw_list).
    RebuildMesh3D();
    UpdatePsxCameraDebugActor();
}

// ===================================================================
// RebuildMesh3D — build 3D geometry from GTE-correlated draw commands
//
// ALL data comes from the shadow GPU (Gpu3D_):
//   - cmds[]    : same GP0 stream as primary, has colors/UVs/texpage/clut
//   - cmds_3d[] : differential tag decoding → face cache → inline 3D data
//
// For 3D triangles (origin_3d): vertex positions come from RT*v+TR transform
//   → screen coords in cmds[].v are irrelevant (tagged dead-zone values)
// For 2D triangles: screen coords in cmds[].v are valid
//   (shadow GPU applies sign_extend_11 + draw offset for non-3D)
// ===================================================================
void UPSX3DRenderComponent::RebuildMesh3D()
{
    if (!Gpu3D_ || !MeshComp_)
        return;

    // Lazy material refresh
    EnsureMaterialInstances();

    // Thread-safe copy of draw list from shadow GPU
    gpu::FrameDrawList DrawListCopy;
    Gpu3D_->copy_ready_draw_list(DrawListCopy);
    const gpu::FrameDrawList& DL = DrawListCopy;

    const int32 NumCmds = static_cast<int32>(DL.cmds.size());
    const int32 Num3D = static_cast<int32>(DL.cmds_3d.size());

    // Diagnostic: log draw list size periodically + scene transitions
    static uint32 RebuildCount = 0;
    static int32 PrevNumCmds = 0;
    const bool bSceneChange = (NumCmds != PrevNumCmds && NumCmds > 0);
    if (RebuildCount < 10 || (RebuildCount % 300) == 0 || bSceneChange)
    {
        // Count 3D vs 2D in draw list
        int32 N3D = 0, N2D = 0;
        uint32 MaxOtZLocal = 0;
        for (int32 k = 0; k < FMath::Min(NumCmds, Num3D); ++k)
        {
            if (DL.cmds_3d[k].origin == gpu::PrimOrigin::origin_3d) ++N3D; else ++N2D;
            MaxOtZLocal = FMath::Max(MaxOtZLocal, DL.cmds_3d[k].ot_z);
        }
        GPU3D_NOISE_UELOG(Verbose,
            TEXT("GPU3D RebuildMesh3D #%u: cmds=%d cmds_3d=%d frame_id=%u | 3D=%d 2D=%d maxOtZ=%u %s"),
            RebuildCount, NumCmds, Num3D, DL.frame_id, N3D, N2D, MaxOtZLocal,
            bSceneChange ? TEXT("*** SCENE CHANGE ***") : TEXT(""));
    }
    PrevNumCmds = NumCmds;
    ++RebuildCount;

    // If the frame is empty, keep the previous frame's mesh (no clear).
    if (NumCmds == 0 || Num3D == 0)
    {
        Last3DTriCount_ = 0;
        Last2DSkipCount_ = 0;
        return;
    }

    const int32 Count = FMath::Min(NumCmds, Num3D);

    // Edge-strip reconstruction is handled in the emulator core when a valid
    // face_B / V3 hint exists. Rewriting quad pairs again here can fold real
    // GT4 grids, including Ridge Racer's menu flag.

    // 2D primitives coming from the GPU bridge use the same logical GP0 coords
    // as PSX2DRenderComponent: raw screen-space around the projection center,
    // without draw offset and without scan-out half-size baked in.
    //
    // Keep the 2D path here in the same coordinate contract as the standalone
    // 2D renderer so the 3D projection center matches the PSX2D view.
    constexpr float RefW = 320.0f;

    // ── Run-based sectioning (same approach as 2D) ──────────────────
    struct RunSection
    {
        TArray<FVector> Vertices;
        TArray<int32> Triangles;
        TArray<FVector> Normals;
        TArray<FVector2D> UV0, UV1, UV2, UV3;
        TArray<FLinearColor> Colors;
        TArray<FProcMeshTangent> Tangents;
        int32 MatIdx{0};

        void Reserve(int32 N)
        {
            const int32 V = N * 3;
            Vertices.Reserve(V); Triangles.Reserve(V); Normals.Reserve(V);
            UV0.Reserve(V); UV1.Reserve(V); UV2.Reserve(V); UV3.Reserve(V);
            Colors.Reserve(V); Tangents.Reserve(V);
        }
    };

    TArray<RunSection> Runs;
    Runs.Reserve(32);

    int32 CurMatIdx = -1;
    RunSection* Cur = nullptr;
    int32 Tri3DCount = 0;
    int32 Skip2DCount = 0;

    // =====================================================================
    // ABSOLUTELY DO NOT APPLY PS1 DOUBLE-BUFFER OFFSETS HERE.
    //
    // This is the 2D overlay path inside the 3D renderer, so it must follow the
    // same rule as PSX2DRenderComponent: UE is not rendering PS1 VRAM pages and
    // must not move the mesh when the game flips draw/display buffers.
    //
    // draw_env.offset_x/y and display.display_x/y describe PS1 VRAM placement.
    // They are diagnostic state for this path, not world-space transform input.
    // Subtracting either of them from CenterGx2D/CenterGy2D reintroduces the
    // classic "one frame here, one frame shifted" double-buffer bug.
    //
    // Keep this centred on the logical display size only. If a future change
    // needs real VRAM scan-out behaviour, implement it in a separate surface
    // renderer instead of moving polygon meshes here.
    // =====================================================================
    const float Psx2DW = FMath::Max(1.0f, static_cast<float>(DL.display.width()));
    const float Psx2DH = FMath::Max(1.0f, static_cast<float>(DL.display.height()));
    const float CenterGx2D = Psx2DW * 0.5f;
    const float CenterGy2D = Psx2DH * 0.5f;

    // ── Effective 2D parameters (auto-scale from previous frame's 3D bbox) ──
    float EffScale2D = WorldScale2D;
    float EffDepthBack = Depth2DBack;
    float EffDepthFront = Depth2DFront;
    if (bAutoScale2D && Last3DExtentY_ > 0.0f)
    {
        // Use reference width (320) for consistent auto-scale.
        EffScale2D = Last3DExtentY_ / RefW;

        // Depth range matches 3D scene with 10% margin so 2D can extend beyond
        const float DepthRange = Last3DMaxX_ - Last3DMinX_;
        const float Margin = DepthRange * 0.1f;
        EffDepthBack = Last3DMinX_ - Margin;
        EffDepthFront = Last3DMaxX_ + Margin;
    }

    // Find max OT Z in this frame for depth normalization
    uint32_t MaxOtZ = 1;
    for (int32 i = 0; i < Count; ++i)
    {
        MaxOtZ = FMath::Max(MaxOtZ, DL.cmds_3d[i].ot_z);
    }

    // Experimental world anchoring: use first 3D primitive transform as frame reference.
    bool bHaveRefFrameTransform = false;
    float RefRt[9] = {};
    float RefTr[3] = {};
    if (bApproxWorldFromFrameRef)
    {
        for (int32 i = 0; i < Count; ++i)
        {
            const gpu::DrawCmd3D& C = DL.cmds_3d[i];
            if (C.origin != gpu::PrimOrigin::origin_3d)
                continue;
            for (int k = 0; k < 9; ++k)
                RefRt[k] = static_cast<float>(C.transform.rt[k]) / 4096.0f;
            RefTr[0] = static_cast<float>(C.transform.tr[0]);
            RefTr[1] = static_cast<float>(C.transform.tr[1]);
            RefTr[2] = static_cast<float>(C.transform.tr[2]);
            bHaveRefFrameTransform = true;
            break;
        }
    }

    for (int32 i = 0; i < Count; ++i)
    {
        const gpu::DrawCmd& Cmd = DL.cmds[i];
        const gpu::DrawCmd3D& Cmd3D = DL.cmds_3d[i];

        const bool bHas3D = (Cmd3D.origin == gpu::PrimOrigin::origin_3d);

        // Skip non-3D primitives only if bSkip2DElements is true
        if (!bHas3D)
        {
            ++Skip2DCount;
            if (bSkip2DElements)
                continue;

            // Filter full-screen clear rectangles: untextured primitives that span
            // the full display width. PS1 games draw these to clear the framebuffer.
            // GPU3D now stores 2D coords as raw screen-space (no draw offset baked in),
            // so compare directly against display width.
            if (!(Cmd.flags & 1)) // untextured only
            {
                const int16_t dw = static_cast<int16_t>(DL.display.width());
                int16_t MinVx = 32767, MaxVx = -32768;
                for (int32 j = 0; j < 3; ++j)
                {
                    const int16_t vx = static_cast<int16_t>(Cmd.v[j].x);
                    MinVx = FMath::Min(MinVx, vx);
                    MaxVx = FMath::Max(MaxVx, vx);
                }
                // If the triangle spans at least the full display width, it's a clear rect
                if (MinVx <= 0 && MaxVx >= dw)
                    continue;
            }
        }

        const bool bSemiTrans = (Cmd.flags & 2) != 0;
        const int32 BaseOfs = bHas3D ? 5 : 0;  // 3D → sections 5-9, 2D → sections 0-4
        const int32 MatIdx = BaseOfs + (bSemiTrans ? 1 + (Cmd.semi_mode & 3) : 0);

        // New section needed?
        if (MatIdx != CurMatIdx)
        {
            Runs.AddDefaulted();
            Cur = &Runs.Last();
            Cur->MatIdx = MatIdx;
            Cur->Reserve(64);
            CurMatIdx = MatIdx;
        }

        const int32 BaseVert = Cur->Vertices.Num();

        // ── Build 3 vertex positions ──────────────────────────────────
        FVector TriPos[3];
        for (int32 j = 0; j < 3; ++j)
        {
            const gpu::DrawVertex& V = Cmd.v[j];

            if (bHas3D)
            {
                const gte::GteVertex3D& V3 = Cmd3D.verts_3d[j];
                float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                if (bApplyGteTransform)
                {
                    const gte::GteTransform& T = Cmd3D.transform;

                    // RT * vertex + TR → camera space.
                    // RT is 3×3 fixed-point 4.12 (int16_t). Use int64 to avoid
                    // overflow (int16 × int32 can exceed int32 range).
                    // Arithmetic right-shift (>> 12) matches GTE hardware behavior
                    // (truncates toward -∞, not toward zero like /4096).
                    const int64_t vx = V3.vx, vy = V3.vy, vz = V3.vz;
                    cx = static_cast<float>(((T.rt[0]*vx + T.rt[1]*vy + T.rt[2]*vz) >> 12) + T.tr[0]);
                    cy = static_cast<float>(((T.rt[3]*vx + T.rt[4]*vy + T.rt[5]*vz) >> 12) + T.tr[1]);
                    cz = static_cast<float>(((T.rt[6]*vx + T.rt[7]*vy + T.rt[8]*vz) >> 12) + T.tr[2]);
                }
                else
                {
                    // Raw model-space style mode for free exploration/debug.
                    cx = static_cast<float>(V3.vx);
                    cy = static_cast<float>(V3.vy);
                    cz = static_cast<float>(V3.vz);
                }

                if (bApplyGteTransform && bApproxWorldFromFrameRef && bHaveRefFrameTransform)
                {
                    // Approximate camera-space -> world-space: p_w = R_ref^T * (p_c - T_ref)
                    const float px = cx - RefTr[0];
                    const float py = cy - RefTr[1];
                    const float pz = cz - RefTr[2];
                    const float wx =
                        RefRt[0] * px +
                        RefRt[3] * py +
                        RefRt[6] * pz;
                    const float wy =
                        RefRt[1] * px +
                        RefRt[4] * py +
                        RefRt[7] * pz;
                    const float wz =
                        RefRt[2] * px +
                        RefRt[5] * py +
                        RefRt[8] * pz;
                    cx = wx;
                    cy = wy;
                    cz = wz;
                }

                // GTE camera-space → UE5 coordinate mapping:
                //   For Ridge Racer flag quads, X/Z span the cloth surface while Y
                //   carries the wave displacement. Map the surface to UE's YZ plane,
                //   keep the wave on UE X, then rotate 180 deg around UE Z so the
                //   flag faces the correct direction.
                TriPos[j] = FVector(
                    cy * WorldScale,    // GTE Y (wave/displacement) → UE X
                   -cx * WorldScale,    // GTE X (horizontal span)   → UE Y
                    cz * WorldScale     // GTE Z (vertical span)     → UE Z
                );
            }
            else
            {
                // 2D: use the same raw centered screen coords as PSX2DRenderComponent.
                // The shadow GPU stores se11(raw) WITHOUT draw offset — the offset
                // is for VRAM bank targeting (double-buffering) and varies per-primitive.
                // This avoids the offset mismatch problem where the frame-level draw_env
                // doesn't match the per-primitive draw state.
                //
                // OT traversal order is renderer-facing, not semantic "near/far". In practice
                // we want later OT layers to sit visually in front in UE, so invert the
                // normalized index before mapping it to UE depth.
                const float DepthT = 1.0f - (static_cast<float>(Cmd3D.ot_z) / static_cast<float>(MaxOtZ));
                // Keep the PS1 OT ordering intact, then shift the whole 2D stack in UE space.
                const float Depth2D = FMath::Lerp(EffDepthBack, EffDepthFront, DepthT) + Depth2DBias;

                const float sx = static_cast<float>(V.x);
                const float sy = static_cast<float>(V.y);
                TriPos[j] = FVector(
                    Depth2D,
                    (sx - CenterGx2D) * EffScale2D,
                   -(sy - CenterGy2D) * EffScale2D
                );
            }
            TriPos[j] += WorldOffset;
        }

        // ── Compute face normal ──────────────────────────────────────
        FVector VertNormals[3];
        if (bHas3D && (Cmd3D.nx[0] != 0 || Cmd3D.ny[0] != 0 || Cmd3D.nz[0] != 0))
        {
            // Use per-vertex normals from GTE (NCS/NCT/NCDS/NCDT).
            // Same coordinate remap as positions.
            for (int32 j = 0; j < 3; ++j)
            {
                VertNormals[j] = FVector(
                    static_cast<float>(Cmd3D.ny[j]),    // GTE Y → UE X
                   -static_cast<float>(Cmd3D.nx[j]),    // GTE X → UE Y
                    static_cast<float>(Cmd3D.nz[j])     // GTE Z → UE Z
                ).GetSafeNormal();
            }
        }
        else
        {
            // Fallback: compute face normal from cross product.
            // Negate because winding remap {0,2,1} flips CW→CCW.
            const FVector FaceNorm = -FVector::CrossProduct(
                TriPos[1] - TriPos[0], TriPos[2] - TriPos[0]).GetSafeNormal();
            VertNormals[0] = VertNormals[1] = VertNormals[2] = FaceNorm;
        }

        // ── Add vertices + attributes ────────────────────────────────
        for (int32 j = 0; j < 3; ++j)
        {
            const gpu::DrawVertex& V = Cmd.v[j];

            Cur->Vertices.Add(TriPos[j]);
            Cur->Normals.Add(VertNormals[j]);
            Cur->Tangents.Add(FProcMeshTangent(1.0f, 0.0f, 0.0f));
            Cur->Colors.Add(FLinearColor(V.r / 255.0f, V.g / 255.0f, V.b / 255.0f, 1.0f));

            // UV data: same encoding as 2D component (material shader uses these)
            Cur->UV0.Add(FVector2D(static_cast<float>(V.u), static_cast<float>(V.v)));

            const float TpBaseX = static_cast<float>((Cmd.texpage & 0xF) * 64);
            const float TpBaseY = static_cast<float>(((Cmd.texpage >> 4) & 1) * 256);
            Cur->UV1.Add(FVector2D(TpBaseX, TpBaseY));

            const float ClutX = static_cast<float>((Cmd.clut & 0x3F) * 16);
            const float ClutY = static_cast<float>((Cmd.clut >> 6) & 0x1FF);
            Cur->UV2.Add(FVector2D(ClutX, ClutY));

            const bool bTextured = (Cmd.flags & 1) != 0;
            const bool bRawTexture = (Cmd.flags & 4) != 0;
            const float TexMode = bTextured ? static_cast<float>(Cmd.tex_depth + 1) : 0.0f;
            const float FlagsPacked = static_cast<float>(
                (Cmd.semi_mode & 0x3) |
                (bSemiTrans ? 0x4 : 0) |
                (bRawTexture ? 0x8 : 0)
            );
            Cur->UV3.Add(FVector2D(TexMode, FlagsPacked));

            // Winding: CW → CCW flip (PS1 is CW, UE5 is CCW)
            static const int32 WindingRemap[3] = {0, 2, 1};
            Cur->Triangles.Add(BaseVert + WindingRemap[j]);
        }

        // ── Detailed diagnostic logging (gated by bDebug3DLog) ────────
        if (bDebug3DLog && Tri3DCount < 20)
        {
            if (bHas3D)
            {
                const gte::GteTransform& T = Cmd3D.transform;
                GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                    "=== 3D TRI[%d] frame=%u face_idx=%u quad=%d half=%d ===",
                    Tri3DCount, DL.frame_id, Cmd3D.face_idx, Cmd3D.is_quad ? 1 : 0, Cmd3D.quad_half);

                // Model-space vertices (raw from GTE face cache)
                for (int32 j = 0; j < 3; ++j)
                {
                    const gte::GteVertex3D& V3 = Cmd3D.verts_3d[j];
                    GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                        "  v[%d] model=(%d,%d,%d) normal=(%d,%d,%d) sz=%u",
                        j, V3.vx, V3.vy, V3.vz,
                        Cmd3D.nx[j], Cmd3D.ny[j], Cmd3D.nz[j], Cmd3D.sz[j]);
                }

                // Transform (RT 3x3 + TR)
                GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                    "  RT=[%d,%d,%d / %d,%d,%d / %d,%d,%d] TR=(%d,%d,%d)",
                    T.rt[0], T.rt[1], T.rt[2],
                    T.rt[3], T.rt[4], T.rt[5],
                    T.rt[6], T.rt[7], T.rt[8],
                    T.tr[0], T.tr[1], T.tr[2]);

                // Camera-space positions (RT*v+TR result, before UE5 remap)
                for (int32 j = 0; j < 3; ++j)
                {
                    const gte::GteVertex3D& V3 = Cmd3D.verts_3d[j];
                    const int64_t vx64 = V3.vx, vy64 = V3.vy, vz64 = V3.vz;
                    const int64_t cx = ((T.rt[0]*vx64 + T.rt[1]*vy64 + T.rt[2]*vz64) >> 12) + T.tr[0];
                    const int64_t cy = ((T.rt[3]*vx64 + T.rt[4]*vy64 + T.rt[5]*vz64) >> 12) + T.tr[1];
                    const int64_t cz = ((T.rt[6]*vx64 + T.rt[7]*vy64 + T.rt[8]*vz64) >> 12) + T.tr[2];
                    GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                        "  v[%d] cam=(%lld,%lld,%lld) -> UE(%.1f, %.1f, %.1f)",
                        j, (long long)cx, (long long)cy, (long long)cz,
                        TriPos[j].X, TriPos[j].Y, TriPos[j].Z);
                }

                // Screen coords from DrawCmd (should be dead-zone values for 3D)
                GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                    "  screen=(%d,%d)(%d,%d)(%d,%d) color=(%d,%d,%d)",
                    Cmd.v[0].x, Cmd.v[0].y, Cmd.v[1].x, Cmd.v[1].y, Cmd.v[2].x, Cmd.v[2].y,
                    Cmd.v[0].r, Cmd.v[0].g, Cmd.v[0].b);

                // Texture info
                GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                    "  uv=(%d,%d)(%d,%d)(%d,%d) tp=0x%04X clut=0x%04X flags=0x%02X semi=%d tex=%d",
                    Cmd.v[0].u, Cmd.v[0].v, Cmd.v[1].u, Cmd.v[1].v, Cmd.v[2].u, Cmd.v[2].v,
                    Cmd.texpage, Cmd.clut, Cmd.flags, Cmd.semi_mode, Cmd.tex_depth);
            }
            else
            {
                // 2D triangle log
                GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                    "=== 2D TRI[%d] frame=%u origin=%d ===",
                    Tri3DCount, DL.frame_id, (int)Cmd3D.origin);

                for (int32 j = 0; j < 3; ++j)
                {
                    const gpu::DrawVertex& V = Cmd.v[j];
                    GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                        "  v[%d] screen=(%d,%d) -> UE(%.1f, %.1f, %.1f) color=(%d,%d,%d) uv=(%d,%d)",
                        j, V.x, V.y, TriPos[j].X, TriPos[j].Y, TriPos[j].Z,
                        V.r, V.g, V.b, V.u, V.v);
                }

                GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                    "  origin2D=raw-centered tp=0x%04X clut=0x%04X flags=0x%02X semi=%d tex=%d",
                    Cmd.texpage, Cmd.clut, Cmd.flags, Cmd.semi_mode, Cmd.tex_depth);
            }
        }

        ++Tri3DCount;
    }

    // ── Update 3D bounding box for next frame's auto-scale ─────────────
    {
        float MinX = 1e9f, MaxX = -1e9f, MinY = 1e9f, MaxY = -1e9f;
        for (const RunSection& R : Runs)
        {
            if (R.MatIdx < 5) continue; // 3D sections only (5-9)
            for (const FVector& V : R.Vertices)
            {
                MinX = FMath::Min(MinX, static_cast<float>(V.X));
                MaxX = FMath::Max(MaxX, static_cast<float>(V.X));
                MinY = FMath::Min(MinY, static_cast<float>(V.Y));
                MaxY = FMath::Max(MaxY, static_cast<float>(V.Y));
            }
        }
        if (MaxX > MinX)
        {
            Last3DMinX_ = MinX;
            Last3DMaxX_ = MaxX;
            Last3DExtentY_ = MaxY - MinY;
        }
    }

    // ── Create mesh sections (overwrite in-place, no ClearAll) ─────────
    const int32 NewNumSections = Runs.Num();

    for (int32 r = 0; r < NewNumSections; ++r)
    {
        const RunSection& Run = Runs[r];
        if (Run.Vertices.Num() == 0)
            continue;

        MeshComp_->CreateMeshSection_LinearColor(
            r, Run.Vertices, Run.Triangles, Run.Normals,
            Run.UV0, Run.UV1, Run.UV2, Run.UV3,
            Run.Colors, Run.Tangents, false /*bCreateCollision*/);

        if (MatInst_.IsValidIndex(Run.MatIdx) && MatInst_[Run.MatIdx])
            MeshComp_->SetMaterial(r, MatInst_[Run.MatIdx]);
    }

    // Clear leftover sections from previous frame (new frame has fewer runs).
    // This ensures stale geometry (e.g., PS logo) disappears when scene changes.
    for (int32 r = NewNumSections; r < LastNumSections_; ++r)
        MeshComp_->ClearMeshSection(r);
    LastNumSections_ = NewNumSections;

    MeshComp_->MarkRenderStateDirty();

    Last3DTriCount_ = Tri3DCount;
    Last2DSkipCount_ = Skip2DCount;

    // Diagnostic: log mesh creation stats periodically
    if (RebuildCount < 15 || (RebuildCount % 300) == 0)
    {
        int32 TotalVerts = 0;
        for (const RunSection& R : Runs)
            TotalVerts += R.Vertices.Num();
        GPU3D_NOISE_UELOG(Verbose,
            TEXT("GPU3D mesh: %d sections, %d verts, %d 3D tris, %d 2D skip, bSkip2D=%d"),
            NewNumSections, TotalVerts, Tri3DCount, Skip2DCount, bSkip2DElements ? 1 : 0);
    }

    // Debug logging: frame summary with bounding box, display config, draw env
    if (bDebug3DLog)
    {
        GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
            "======== FRAME %u SUMMARY ========", DL.frame_id);
        GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
            "  Display: %dx%d (h_res=%d v_res=%d) enabled=%d pal=%d",
            DL.display.width(), DL.display.height(),
            DL.display.h_res, DL.display.v_res,
            DL.display.display_enabled ? 1 : 0, DL.display.is_pal ? 1 : 0);
        GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
            "  DrawEnv: offset=(%d,%d) clip=(%d,%d)-(%d,%d) texpage=0x%08X",
            DL.draw_env.offset_x, DL.draw_env.offset_y,
            DL.draw_env.clip_x1, DL.draw_env.clip_y1,
            DL.draw_env.clip_x2, DL.draw_env.clip_y2,
            DL.draw_env.texpage_raw);
        GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
            "  Origin2D=center(%.1f,%.1f) psx=%ux%u WorldScale=%.3f WorldScale2D=%.3f EffScale2D=%.4f",
            CenterGx2D, CenterGy2D, DL.display.width(), DL.display.height(), WorldScale, WorldScale2D, EffScale2D);
        GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
            "  Auto2D=%d DepthRange=[%.1f..%.1f] Last3D: X=[%.1f..%.1f] ExtY=%.1f",
            bAutoScale2D ? 1 : 0, EffDepthBack, EffDepthFront,
            Last3DMinX_, Last3DMaxX_, Last3DExtentY_);
        GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
            "  Total: %d cmds, %d 3D tris, %d 2D skip, %d sections",
            Count, Tri3DCount, Skip2DCount, Runs.Num());

        // Per-section breakdown
        for (int32 r = 0; r < Runs.Num(); ++r)
        {
            const RunSection& Run = Runs[r];
            const bool bIs3DSec = (Run.MatIdx >= 5);
            GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                "  Section[%d]: mat=%d (%s) %d verts %d tris",
                r, Run.MatIdx, bIs3DSec ? "3D" : "2D",
                Run.Vertices.Num(), Run.Triangles.Num() / 3);
        }

        // Bounding box per category (2D sections vs 3D sections)
        float Min3D[3] = {1e9f, 1e9f, 1e9f}, Max3D[3] = {-1e9f, -1e9f, -1e9f};
        float Min2D[3] = {1e9f, 1e9f, 1e9f}, Max2D[3] = {-1e9f, -1e9f, -1e9f};
        int32 Cnt3D = 0, Cnt2D = 0;
        for (const RunSection& R : Runs)
        {
            const bool b3D = (R.MatIdx >= 5);
            float* MinB = b3D ? Min3D : Min2D;
            float* MaxB = b3D ? Max3D : Max2D;
            int32& Cnt = b3D ? Cnt3D : Cnt2D;
            for (const FVector& V : R.Vertices)
            {
                MinB[0] = FMath::Min(MinB[0], (float)V.X); MaxB[0] = FMath::Max(MaxB[0], (float)V.X);
                MinB[1] = FMath::Min(MinB[1], (float)V.Y); MaxB[1] = FMath::Max(MaxB[1], (float)V.Y);
                MinB[2] = FMath::Min(MinB[2], (float)V.Z); MaxB[2] = FMath::Max(MaxB[2], (float)V.Z);
                ++Cnt;
            }
        }
        if (Cnt3D > 0)
            GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                "  BBox 3D: X=[%.1f..%.1f] Y=[%.1f..%.1f] Z=[%.1f..%.1f] (%d verts)",
                Min3D[0], Max3D[0], Min3D[1], Max3D[1], Min3D[2], Max3D[2], Cnt3D);
        if (Cnt2D > 0)
            GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
                "  BBox 2D: X=[%.1f..%.1f] Y=[%.1f..%.1f] Z=[%.1f..%.1f] (%d verts)",
                Min2D[0], Max2D[0], Min2D[1], Max2D[1], Min2D[2], Max2D[2], Cnt2D);
        GPU3D_NOISE_LOGF(emu::LogLevel::info, "GPU3D",
            "======== END FRAME %u ========", DL.frame_id);
    }
}
