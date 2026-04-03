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
    SetMobility(EComponentMobility::Movable);

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
    Super::EndPlay(EndPlayReason);
}

// ===================================================================
// GetEffectivePixelScale - compute uniform HD scale or return manual
// ===================================================================
float UR3000GpuComponent::GetEffectivePixelScale() const
{
    if (!bUniformHdScale)
    {
        return PixelScale;
    }

    // Get target resolution from HD definition preset
    float TgtWidth, TgtHeight;
    switch (HdDefinition)
    {
        case EHdDefinition::HD_720p:
            TgtWidth = 1280.0f;
            TgtHeight = 720.0f;
            break;
        case EHdDefinition::HD_1080p:
            TgtWidth = 1920.0f;
            TgtHeight = 1080.0f;
            break;
        case EHdDefinition::HD_1440p:
            TgtWidth = 2560.0f;
            TgtHeight = 1440.0f;
            break;
        case EHdDefinition::HD_4K:
            TgtWidth = 3840.0f;
            TgtHeight = 2160.0f;
            break;
        case EHdDefinition::Custom:
        default:
            TgtWidth = TargetWidth;
            TgtHeight = TargetHeight;
            break;
    }

    // Get PS1 display resolution
    float Ps1Width = 320.0f;  // Default
    float Ps1Height = 240.0f;

    if (Gpu_)
    {
        const gpu::DisplayConfig& Disp = Gpu_->display_config();
        Ps1Width = static_cast<float>(Disp.width());
        Ps1Height = static_cast<float>(Disp.height());
        // Clamp to sane values
        if (Ps1Width < 1.0f) Ps1Width = 320.0f;
        if (Ps1Height < 1.0f) Ps1Height = 240.0f;
    }

    // Scale to fit target while maintaining aspect ratio
    const float ScaleX = TgtWidth / Ps1Width;
    const float ScaleY = TgtHeight / Ps1Height;
    return FMath::Min(ScaleX, ScaleY);
}

// ===================================================================
// BindGpu - called by R3000EmuComponent after core init
// ===================================================================
void UR3000GpuComponent::BindGpu(gpu::Gpu* InGpu)
{
    UE_LOG(LogR3000Gpu, Warning, TEXT("BindGpu called. InGpu=%p (was Gpu_=%p)"), InGpu, Gpu_);
    emu::logf(emu::LogLevel::info, "GPU", "GpuComponent v11 (run_based_sections)");

    Gpu_ = InGpu;

    // ---- Geometry ProceduralMeshComponent (attached to this SceneComponent) ----
    if (!MeshComp_)
    {
        MeshComp_ = NewObject<UProceduralMeshComponent>(GetOwner(), TEXT("PSXMesh"));
        MeshComp_->bUseAsyncCooking = true;
        MeshComp_->SetCastShadow(false);
        MeshComp_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComp_->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        MeshComp_->RegisterComponent();
        MeshComp_->SetVisibility(true);
        MeshComp_->SetHiddenInGame(false);
    }

    // ---- Geometry material instances (5 sections) ----
    EnsureMaterialInstances();

    if (!BaseMaterial)
    {
        UE_LOG(LogR3000Gpu, Error, TEXT("WARNING: BaseMaterial is NULL! Assign a material in the Blueprint or mesh will be invisible."));
        emu::logf(emu::LogLevel::error, "GPU", "BaseMaterial is NULL - mesh will be invisible! Assign a material in Blueprint.");
    }

    UE_LOG(LogR3000Gpu, Log, TEXT("GPU bound. MeshComp=%d VramTex=%d Mat[0..4]=%d%d%d%d%d"),
        MeshComp_ != nullptr, VramTexture_ != nullptr,
        MatInst_.IsValidIndex(0) && MatInst_[0] != nullptr,
        MatInst_.IsValidIndex(1) && MatInst_[1] != nullptr,
        MatInst_.IsValidIndex(2) && MatInst_[2] != nullptr,
        MatInst_.IsValidIndex(3) && MatInst_[3] != nullptr,
        MatInst_.IsValidIndex(4) && MatInst_[4] != nullptr);
}

// ===================================================================
// Material instance management — lazy create/refresh
// ===================================================================
void UR3000GpuComponent::EnsureMaterialInstances()
{
    // Resolve source material for each section: specific slot → BaseMaterial fallback
    UMaterialInterface* Wanted[kNumSections] = {
        BaseMaterial,
        MatSemi0 ? MatSemi0 : BaseMaterial,
        MatSemi1 ? MatSemi1 : BaseMaterial,
        MatSemi2 ? MatSemi2 : BaseMaterial,
        MatSemi3 ? MatSemi3 : BaseMaterial,
    };

    // Initialize arrays on first call
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

    // (Re)create dynamic instances if the source material changed or wasn't set
    for (int32 s = 0; s < kNumSections; ++s)
    {
        if (!Wanted[s])
            continue;

        // Already created from this exact source? Skip.
        if (MatInst_[s] && MatInstSource_[s] == Wanted[s])
            continue;

        // Source changed or first time — (re)create
        MatInst_[s] = UMaterialInstanceDynamic::Create(Wanted[s], this);
        MatInstSource_[s] = Wanted[s];
        if (MatInst_[s] && VramTexture_)
            MatInst_[s]->SetTextureParameterValue(TEXT("VramTexture"), VramTexture_);

        UE_LOG(LogR3000Gpu, Log, TEXT("MatInst_[%d] (re)created from %s"), s,
            *Wanted[s]->GetName());
    }
}

void UR3000GpuComponent::RefreshMaterials()
{
    // Force re-evaluation by clearing source tracking
    MatInstSource_.SetNum(kNumSections);
    for (int32 s = 0; s < kNumSections; ++s)
        MatInstSource_[s] = nullptr;

    EnsureMaterialInstances();

    UE_LOG(LogR3000Gpu, Log, TEXT("RefreshMaterials: Mat[0..4]=%d%d%d%d%d"),
        MatInst_[0] != nullptr, MatInst_[1] != nullptr, MatInst_[2] != nullptr,
        MatInst_[3] != nullptr, MatInst_[4] != nullptr);
}

// ===================================================================
// SetVramTexture — receive shared texture from VramViewerComponent
// ===================================================================
void UR3000GpuComponent::SetVramTexture(UTexture2D* InTexture)
{
    VramTexture_ = InTexture;
    UE_LOG(LogR3000Gpu, Log, TEXT("SetVramTexture: %p"), InTexture);

    // Update material instances with new texture
    for (int32 s = 0; s < MatInst_.Num(); ++s)
    {
        if (MatInst_[s] && VramTexture_)
            MatInst_[s]->SetTextureParameterValue(TEXT("VramTexture"), VramTexture_);
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
        // Don't clear the mesh on empty frames — keep the previous frame visible.
        // PS1 games often skip GPU commands on some VBlanks (e.g. Ridge Racer
        // draws 2 frames then skips 1 in 30fps interlaced mode). Clearing
        // the mesh on empty frames causes visible flickering.
        return;
    }

    // Lazy-(re)create material instances if slots were assigned after BindGpu
    // (e.g., Blueprint BeginPlay sets MatSemi0-3 after InitEmulator already called BindGpu)
    EnsureMaterialInstances();

    // Log first time we receive primitives (confirms GPU bridge working)
    static bool bFirstPrimitives = true;
    if (bFirstPrimitives)
    {
        const bool bHasMat0 = MatInst_.IsValidIndex(0) && MatInst_[0] != nullptr;
        UE_LOG(LogR3000Gpu, Warning, TEXT("GPU: First primitives received! %d triangles. Mat[0]=%d"), NumCmds, bHasMat0 ? 1 : 0);
        emu::logf(emu::LogLevel::info, "GPU", "UE5 First primitives! %d tris, Mat[0]=%s", NumCmds, bHasMat0 ? "OK" : "NULL (INVISIBLE!)");
        bFirstPrimitives = false;
    }

    // ── Run-based sectioning ──────────────────────────────────────────
    // Walk the draw list linearly. A new mesh section is created only when
    // the blend mode (material index) changes. Consecutive primitives with
    // the same mode share one section → minimum draw calls while preserving
    // PS1 painter's algorithm order via depth (ZStep).
    //
    // Material mapping: 0=opaque(BaseMaterial), 1-4=semi modes 0-3 (MatSemi0-3)

    // Per-section vertex buffers (reused across runs via swap)
    struct RunSection
    {
        TArray<FVector> Vertices;
        TArray<int32> Triangles;
        TArray<FVector> Normals;
        TArray<FVector2D> UV0, UV1, UV2, UV3;
        TArray<FLinearColor> Colors;
        TArray<FProcMeshTangent> Tangents;
        int32 MatIdx{0};  // Which material (0=opaque, 1-4=semi modes)

        void Reserve(int32 NumTris)
        {
            const int32 N = NumTris * 3;
            Vertices.Reserve(N); Triangles.Reserve(N); Normals.Reserve(N);
            UV0.Reserve(N); UV1.Reserve(N); UV2.Reserve(N); UV3.Reserve(N);
            Colors.Reserve(N); Tangents.Reserve(N);
        }
    };

    const FVector FaceNormal(-1.0f, 0.0f, 0.0f);
    const FProcMeshTangent FaceTangent(0.0f, 1.0f, 0.0f);

    // PS1→UE5 coordinate transform
    const gpu::DisplayConfig& Disp = DrawList.display;
    const float OriginX = 0.5f * static_cast<float>(Disp.width());
    const float OriginY = 0.5f * static_cast<float>(Disp.height());
    const float EffScale = GetEffectivePixelScale();

    if (bDebugMeshLog)
    {
        const char* HdNames[] = {"720p", "1080p", "1440p", "4K", "Custom"};
        const char* HdName = (static_cast<int>(HdDefinition) < 5) ? HdNames[static_cast<int>(HdDefinition)] : "?";
        emu::logf(emu::LogLevel::info, "GPU", "MeshRebuild: %d tris | disp=(%u,%u)+(%ux%u) | EffScale=%.3f (HD=%s) Origin=(%.1f,%.1f)",
            NumCmds, Disp.display_x, Disp.display_y, Disp.width(), Disp.height(),
            EffScale, HdName, OriginX, OriginY);
    }

    // Collect runs: walk draw list, flush section on material change
    TArray<RunSection> Runs;
    Runs.Reserve(32);  // Typical: 5-20 material transitions per frame

    int32 CurMatIdx = -1;  // Force first primitive to open a run
    RunSection* Cur = nullptr;
    float Ps1MinX = 1e9f, Ps1MaxX = -1e9f, Ps1MinY = 1e9f, Ps1MaxY = -1e9f;
    int32 SemiTriCounts[kNumSections] = {};

    for (int32 i = 0; i < NumCmds; ++i)
    {
        const gpu::DrawCmd& Cmd = DrawList.cmds[i];
        const float Depth = static_cast<float>(NumCmds - 1 - i) * ZStep;

        const bool bSemiTrans = (Cmd.flags & 2) != 0;
        const int32 MatIdx = bSemiTrans ? 1 + (Cmd.semi_mode & 3) : 0;
        SemiTriCounts[MatIdx]++;

        // New section needed?
        if (MatIdx != CurMatIdx)
        {
            Runs.AddDefaulted();
            Cur = &Runs.Last();
            Cur->MatIdx = MatIdx;
            Cur->Reserve(64);  // Reasonable initial reserve
            CurMatIdx = MatIdx;
        }

        const int32 BaseVert = Cur->Vertices.Num();

        for (int32 j = 0; j < 3; ++j)
        {
            const gpu::DrawVertex& V = Cmd.v[j];

            const float dx = (static_cast<float>(V.x) + 0.5f) - OriginX;
            const float dy = (static_cast<float>(V.y) + 0.5f) - OriginY;
            Cur->Vertices.Add(FVector(Depth, dx * EffScale + DisplayOffset.X, -dy * EffScale + DisplayOffset.Y));

            if (bDebugMeshLog)
            {
                Ps1MinX = FMath::Min(Ps1MinX, static_cast<float>(V.x));
                Ps1MaxX = FMath::Max(Ps1MaxX, static_cast<float>(V.x));
                Ps1MinY = FMath::Min(Ps1MinY, static_cast<float>(V.y));
                Ps1MaxY = FMath::Max(Ps1MaxY, static_cast<float>(V.y));
            }

            Cur->Normals.Add(FaceNormal);
            Cur->Tangents.Add(FaceTangent);
            Cur->Colors.Add(FLinearColor(V.r / 255.0f, V.g / 255.0f, V.b / 255.0f, 1.0f));

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

            static const int32 WindingRemap[3] = {0, 2, 1};
            Cur->Triangles.Add(BaseVert + WindingRemap[j]);
        }

        if (bDebugMeshLog && i < 3)
        {
            const gpu::DrawVertex& Va = Cmd.v[0];
            const gpu::DrawVertex& Vb = Cmd.v[1];
            const gpu::DrawVertex& Vc = Cmd.v[2];
            emu::logf(emu::LogLevel::info, "GPU", "  Tri[%d] mat=%d: PS1 (%d,%d)(%d,%d)(%d,%d) | tex=%d semi=%d smode=%d",
                i, MatIdx, Va.x, Va.y, Vb.x, Vb.y, Vc.x, Vc.y,
                (Cmd.flags & 1) ? 1 : 0, (Cmd.flags & 2) ? 1 : 0, Cmd.semi_mode);
        }
    }

    if (bDebugMeshLog && NumCmds > 0)
    {
        emu::logf(emu::LogLevel::info, "GPU", "  Bounds PS1: X=[%.0f..%.0f] Y=[%.0f..%.0f] span=%.0fx%.0f",
            Ps1MinX, Ps1MaxX, Ps1MinY, Ps1MaxY, Ps1MaxX - Ps1MinX, Ps1MaxY - Ps1MinY);
    }

    // ── Create mesh sections in draw order ───────────────────────────
    MeshComp_->ClearAllMeshSections();
    int32 TotalTris = 0;

    for (int32 r = 0; r < Runs.Num(); ++r)
    {
        const RunSection& Run = Runs[r];
        if (Run.Vertices.Num() == 0)
            continue;

        MeshComp_->CreateMeshSection_LinearColor(
            r, Run.Vertices, Run.Triangles, Run.Normals,
            Run.UV0, Run.UV1, Run.UV2, Run.UV3,
            Run.Colors, Run.Tangents, false /*bCreateCollision*/);

        // Assign the correct material for this run's blend mode
        if (MatInst_.IsValidIndex(Run.MatIdx) && MatInst_[Run.MatIdx])
            MeshComp_->SetMaterial(r, MatInst_[Run.MatIdx]);

        TotalTris += Run.Triangles.Num() / 3;
    }
    MeshComp_->MarkRenderStateDirty();

    if (bDebugMeshLog)
    {
        FBoxSphereBounds MeshBounds = MeshComp_->Bounds;
        emu::logf(emu::LogLevel::info, "GPU", "MeshCreated: %d tris, %d sections (runs) [opq=%d s0=%d s1=%d s2=%d s3=%d] Bounds=(%.1f,%.1f,%.1f)±(%.1f,%.1f,%.1f)",
            TotalTris, Runs.Num(),
            SemiTriCounts[0], SemiTriCounts[1], SemiTriCounts[2], SemiTriCounts[3], SemiTriCounts[4],
            MeshBounds.Origin.X, MeshBounds.Origin.Y, MeshBounds.Origin.Z,
            MeshBounds.BoxExtent.X, MeshBounds.BoxExtent.Y, MeshBounds.BoxExtent.Z);
    }

    // Warn if no materials assigned
    if (!MatInst_.IsValidIndex(0) || !MatInst_[0])
    {
        static bool bWarnedNoMat = false;
        if (!bWarnedNoMat)
        {
            UE_LOG(LogR3000Gpu, Error, TEXT("GPU RebuildMesh: No BaseMaterial! %d triangles may be INVISIBLE."), NumCmds);
            emu::logf(emu::LogLevel::error, "GPU", "RebuildMesh: No BaseMaterial! %d tris may be INVISIBLE!", NumCmds);
            bWarnedNoMat = true;
        }
    }

    LastTriCount_ = TotalTris;
    for (int32 s = 0; s < kNumSections; ++s)
        SectionTriCount_[s] = SemiTriCounts[s];
    LastSectionCount_ = Runs.Num();

    // Compute GPU FPS (frames with actual draw commands per second)
    const double Now = FPlatformTime::Seconds();
    const double GpuElapsed = Now - LastGpuFpsTime_;
    if (GpuElapsed >= 1.0)
    {
        const uint32 CurrentGpuFrame = DrawList.frame_id;
        const uint32 DeltaFrames = CurrentGpuFrame - LastGpuFpsFrame_;
        GpuFps_ = static_cast<float>(DeltaFrames / GpuElapsed);
        LastGpuFpsTime_ = Now;
        LastGpuFpsFrame_ = CurrentGpuFrame;
    }
}

// ===================================================================
// Tick - rebuild geometry when new frame available
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

    // Sync skip_fill_rect option to GPU
    Gpu_->set_skip_fill_rect(bSkipFillRect);

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

    // If disabled, hide mesh
    if (!bEnabled)
    {
        if (MeshComp_ && MeshComp_->IsVisible())
            MeshComp_->SetVisibility(false);
        LastVramFrame_ = Gpu_->vram_frame_count();
        return;
    }

    // Re-show mesh if it was hidden by bEnabled toggle
    if (MeshComp_ && !MeshComp_->IsVisible())
        MeshComp_->SetVisibility(true);

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

