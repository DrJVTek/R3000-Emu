#include "R3000Gpu3DComponent.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Logging/LogMacros.h"

#include "gpu/gpu.h"
#include "log/emu_log.h"

DEFINE_LOG_CATEGORY_STATIC(LogR3000Gpu3D, Log, All);

// ===================================================================
// Constructor
// ===================================================================
UR3000Gpu3DComponent::UR3000Gpu3DComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
}

// ===================================================================
// BeginPlay
// ===================================================================
void UR3000Gpu3DComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(true);
}

// ===================================================================
// EndPlay
// ===================================================================
void UR3000Gpu3DComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Gpu_ = nullptr;
    Super::EndPlay(EndPlayReason);
}

// ===================================================================
// BindGpu
// ===================================================================
void UR3000Gpu3DComponent::BindGpu(gpu::Gpu* InGpu)
{
    UE_LOG(LogR3000Gpu3D, Warning, TEXT("GPU3D v2 BindGpu called. InGpu=%p WorldScale=%.3f bSkip2D=%d"),
        InGpu, WorldScale, bSkip2DElements ? 1 : 0);
    emu::logf(emu::LogLevel::warn, "GPU3D", "Gpu3DComponent bound (WorldScale=%.3f, bSkip2D=%d)",
        (double)WorldScale, bSkip2DElements ? 1 : 0);

    Gpu_ = InGpu;

    // Create ProceduralMeshComponent
    if (!MeshComp_)
    {
        AActor* Owner = GetOwner();
        if (Owner)
        {
            MeshComp_ = NewObject<UProceduralMeshComponent>(Owner, TEXT("PSX3DMesh"));
            MeshComp_->bUseAsyncCooking = true;
            MeshComp_->SetCastShadow(false);
            MeshComp_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            if (Owner->GetRootComponent())
                MeshComp_->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
            MeshComp_->RegisterComponent();
            MeshComp_->SetVisibility(bEnabled);
            MeshComp_->SetHiddenInGame(!bEnabled);
        }
    }

    EnsureMaterialInstances();

    if (!BaseMaterial)
    {
        UE_LOG(LogR3000Gpu3D, Warning, TEXT("BaseMaterial is NULL — 3D mesh will be invisible until a material is assigned."));
    }
}

// ===================================================================
// SetVramTexture — receive shared texture from 2D component
// ===================================================================
void UR3000Gpu3DComponent::SetVramTexture(UTexture2D* InTexture)
{
    VramTexture_ = InTexture;

    // Update existing material instances with the new texture
    for (int32 s = 0; s < MatInst_.Num(); ++s)
    {
        if (MatInst_[s] && VramTexture_)
            MatInst_[s]->SetTextureParameterValue(TEXT("VramTexture"), VramTexture_);
    }

    UE_LOG(LogR3000Gpu3D, Log, TEXT("SetVramTexture: %p"), InTexture);
}

// ===================================================================
// Material instance management (same pattern as 2D component)
// ===================================================================
void UR3000Gpu3DComponent::EnsureMaterialInstances()
{
    UMaterialInterface* Wanted[kNumSections] = {
        BaseMaterial,
        MatSemi0 ? MatSemi0 : BaseMaterial,
        MatSemi1 ? MatSemi1 : BaseMaterial,
        MatSemi2 ? MatSemi2 : BaseMaterial,
        MatSemi3 ? MatSemi3 : BaseMaterial,
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
void UR3000Gpu3DComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    // Diagnostic: confirm tick is running
    static int32 s3DTickCount = 0;
    s3DTickCount++;
    if (s3DTickCount <= 5 || (s3DTickCount % 300) == 0)
    {
        UE_LOG(LogR3000Gpu3D, Warning, TEXT("GPU3D Tick #%d: Gpu_=%p MeshComp_=%p bEnabled=%d"),
            s3DTickCount, Gpu_, MeshComp_, bEnabled ? 1 : 0);
    }

    if (!Gpu_ || !MeshComp_)
        return;

    // Toggle visibility based on bEnabled
    if (!bEnabled)
    {
        if (MeshComp_->IsVisible())
            MeshComp_->SetVisibility(false);
        LastVramFrame_ = Gpu_->vram_frame_count();
        return;
    }

    if (!MeshComp_->IsVisible())
        MeshComp_->SetVisibility(true);

    // Rebuild when a new frame is ready
    const uint32 CurrentFrame = Gpu_->vram_frame_count();
    if (CurrentFrame != LastVramFrame_)
    {
        RebuildMesh3D();
        LastVramFrame_ = CurrentFrame;
    }
}

// ===================================================================
// RebuildMesh3D — build 3D geometry from GTE-correlated draw commands
// ===================================================================
void UR3000Gpu3DComponent::RebuildMesh3D()
{
    if (!Gpu_ || !MeshComp_)
        return;

    // Lazy material refresh
    EnsureMaterialInstances();

    // Thread-safe copy of draw list
    gpu::FrameDrawList DrawListCopy;
    Gpu_->copy_ready_draw_list(DrawListCopy);
    const gpu::FrameDrawList& DL = DrawListCopy;

    const int32 NumCmds = static_cast<int32>(DL.cmds.size());
    const int32 Num3D = static_cast<int32>(DL.cmds_3d.size());

    // cmds_3d must be parallel to cmds
    if (NumCmds == 0 || Num3D == 0)
        return;

    const int32 Count = FMath::Min(NumCmds, Num3D);

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
        }

        const bool bSemiTrans = (Cmd.flags & 2) != 0;
        const int32 MatIdx = bSemiTrans ? 1 + (Cmd.semi_mode & 3) : 0;

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

        for (int32 j = 0; j < 3; ++j)
        {
            const gpu::DrawVertex& V = Cmd.v[j];

            FVector Pos;
            if (bHas3D)
            {
                const gte::GteVertex3D& V3 = Cmd3D.verts_3d[j];
                const gte::GteTransform& T = Cmd3D.transform;

                // Apply RT * vertex + TR to get camera-space coordinates.
                // RT is 3x3 fixed-point 4.12 (int16_t), so divide by 4096.
                // TR is int32 camera-space translation.
                const int32_t vx = V3.vx, vy = V3.vy, vz = V3.vz;
                const float cx = static_cast<float>((T.rt[0]*vx + T.rt[1]*vy + T.rt[2]*vz) / 4096 + T.tr[0]);
                const float cy = static_cast<float>((T.rt[3]*vx + T.rt[4]*vy + T.rt[5]*vz) / 4096 + T.tr[1]);
                const float cz = static_cast<float>((T.rt[6]*vx + T.rt[7]*vy + T.rt[8]*vz) / 4096 + T.tr[2]);

                // GTE camera-space → UE5 coordinate mapping:
                //   GTE: X=right, Y=down, Z=into screen
                //   UE5: X=forward, Y=right, Z=up
                Pos = FVector(
                    cz * WorldScale,    // GTE Z (depth) → UE X (forward)
                    cx * WorldScale,    // GTE X (right) → UE Y (right)
                   -cy * WorldScale     // GTE Y (down)  → UE -Z (up)
                );
            }
            else
            {
                // 2D fallback: use screen coords on a flat plane
                // PS1 screen: 0..320 X, 0..240 Y → center at (160, 120)
                Pos = FVector(
                    200.0f * WorldScale,                                           // Fixed depth (in front)
                    static_cast<float>(V.x - 160) * WorldScale,                   // Screen X → UE Y
                   -static_cast<float>(V.y - 120) * WorldScale                    // Screen Y → UE -Z
                );
            }
            Pos += WorldOffset;
            Cur->Vertices.Add(Pos);

            // Normal: compute per-triangle after loop (use placeholder for now)
            Cur->Normals.Add(FVector(0.0f, 0.0f, 1.0f));
            Cur->Tangents.Add(FProcMeshTangent(1.0f, 0.0f, 0.0f));
            Cur->Colors.Add(FLinearColor(V.r / 255.0f, V.g / 255.0f, V.b / 255.0f, 1.0f));

            // UV data: same encoding as 2D (material shader uses these)
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

            // Winding: same CW→CCW flip as 2D component
            static const int32 WindingRemap[3] = {0, 2, 1};
            Cur->Triangles.Add(BaseVert + WindingRemap[j]);
        }

        // Compute proper face normal for this triangle
        if (Cur->Vertices.Num() >= BaseVert + 3)
        {
            const FVector& A = Cur->Vertices[BaseVert];
            const FVector& B = Cur->Vertices[BaseVert + 1];
            const FVector& C = Cur->Vertices[BaseVert + 2];
            FVector FaceNorm = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
            Cur->Normals[BaseVert] = FaceNorm;
            Cur->Normals[BaseVert + 1] = FaceNorm;
            Cur->Normals[BaseVert + 2] = FaceNorm;
        }

        // Diagnostic: log first triangles regardless of 3D status
        static int32 s3DLog = 0;
        if (s3DLog < 10 && DL.frame_id > 200)
        {
            const gte::GteVertex3D& DV0 = Cmd3D.verts_3d[0];
            const gte::GteTransform& DT = Cmd3D.transform;
            emu::logf(emu::LogLevel::warn, "GPU3D",
                "TRI[%d] f=%u is3D=%d origin=%d model=(%d,%d,%d) TR=(%d,%d,%d) RT0=(%d,%d,%d) sxy=(%d,%d)",
                s3DLog, DL.frame_id, bHas3D ? 1 : 0, (int)Cmd3D.origin,
                DV0.vx, DV0.vy, DV0.vz,
                DT.tr[0], DT.tr[1], DT.tr[2],
                DT.rt[0], DT.rt[1], DT.rt[2],
                Cmd.v[0].x, Cmd.v[0].y);
            ++s3DLog;
        }

        ++Tri3DCount;
    }

    // ── Create mesh sections ──────────────────────────────────────────
    MeshComp_->ClearAllMeshSections();

    for (int32 r = 0; r < Runs.Num(); ++r)
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
    MeshComp_->MarkRenderStateDirty();

    Last3DTriCount_ = Tri3DCount;
    Last2DSkipCount_ = Skip2DCount;

    // Debug logging: compute 3D bounding box to verify depth variation
    if (bDebug3DLog || Tri3DCount > 0)
    {
        // Scan all runs for position extents (UE coords)
        float MinX = 1e9f, MaxX = -1e9f;
        float MinY = 1e9f, MaxY = -1e9f;
        float MinZ = 1e9f, MaxZ = -1e9f;
        int32 TotalVerts3D = 0;
        for (const RunSection& R : Runs)
        {
            for (const FVector& V : R.Vertices)
            {
                MinX = FMath::Min(MinX, (float)V.X);
                MaxX = FMath::Max(MaxX, (float)V.X);
                MinY = FMath::Min(MinY, (float)V.Y);
                MaxY = FMath::Max(MaxY, (float)V.Y);
                MinZ = FMath::Min(MinZ, (float)V.Z);
                MaxZ = FMath::Max(MaxZ, (float)V.Z);
                ++TotalVerts3D;
            }
        }

        static int32 sLogCount3D = 0;
        const bool bDoLog = bDebug3DLog || (sLogCount3D < 20);
        if (bDoLog && TotalVerts3D > 0)
        {
            emu::logf(emu::LogLevel::warn, "GPU3D",
                "Frame %u: %d 3D tris, %d 2D skip, %d sections | "
                "BBox X=[%.1f..%.1f] Y=[%.1f..%.1f] Z=[%.1f..%.1f] (%d verts)",
                DL.frame_id, Tri3DCount, Skip2DCount, Runs.Num(),
                MinX, MaxX, MinY, MaxY, MinZ, MaxZ, TotalVerts3D);
            ++sLogCount3D;
        }
    }
    else
    {
        // Log first few frames to confirm pipeline working
        static int32 sLogCount = 0;
        if (sLogCount < 5 && (Tri3DCount > 0 || Skip2DCount > 0))
        {
            UE_LOG(LogR3000Gpu3D, Warning, TEXT("3D Rebuild #%d: %d 3D tris, %d 2D skipped"), sLogCount, Tri3DCount, Skip2DCount);
            ++sLogCount;
        }
    }
}
