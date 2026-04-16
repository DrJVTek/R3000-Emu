#include "PSX2DRenderComponent.h"
#include "PSXCameraDebugActor.h"

#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Logging/LogMacros.h"
#include "Math/RotationMatrix.h"
#include "DrawDebugHelpers.h"

#include "gpu/gpu.h"
#include "log/emu_log.h"

DEFINE_LOG_CATEGORY_STATIC(LogR3000Gpu, Log, All);

// ===================================================================
// Constructor
// ===================================================================
UPSX2DRenderComponent::UPSX2DRenderComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    SetMobility(EComponentMobility::Movable);

    UE_LOG(LogR3000Gpu, VeryVerbose, TEXT("2D render component constructed"));
}

// ===================================================================
// BeginPlay - ensure tick is enabled
// ===================================================================
void UPSX2DRenderComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(true);
    UE_LOG(LogR3000Gpu, VeryVerbose, TEXT("2D render component BeginPlay"));
}

// ===================================================================
// Cleanup
// ===================================================================
void UPSX2DRenderComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Gpu_ = nullptr;
    if (SpawnedPsxCameraDebugActor_)
    {
        SpawnedPsxCameraDebugActor_->Destroy();
        SpawnedPsxCameraDebugActor_ = nullptr;
    }
    Super::EndPlay(EndPlayReason);
}

// ===================================================================
// GetFixedScreenSize - fixed 4:3 CRT screen size in UE units
// ===================================================================
// Returns the CONSTANT physical screen dimensions regardless of PS1 resolution.
// Every PS1 mode (256x240, 320x240, 640x480, 640x240…) maps onto this same
// rectangle — exactly like a real CRT where the tube never changes size.
// When bUniformHdScale=false, falls back to PixelScale * 320×240.
void UPSX2DRenderComponent::GetFixedScreenSize(float& OutW, float& OutH) const
{
    if (!bUniformHdScale)
    {
        // Manual pixel scale: treat 320×240 as the reference size
        OutW = 320.0f * PixelScale;
        OutH = 240.0f * PixelScale;
        return;
    }

    // 4:3 fixed size derived from the HD height preset
    float FixedH;
    switch (HdDefinition)
    {
        case EHdDefinition::HD_720p:   FixedH =  720.0f; break;
        case EHdDefinition::HD_1080p:  FixedH = 1080.0f; break;
        case EHdDefinition::HD_1440p:  FixedH = 1440.0f; break;
        case EHdDefinition::HD_4K:     FixedH = 2160.0f; break;
        case EHdDefinition::Custom:
        default:
            // Custom: user provides the exact 4:3 size directly
            OutW = TargetWidth;
            OutH = TargetHeight;
            return;
    }
    OutH = FixedH;
    OutW = FixedH * (4.0f / 3.0f);  // always 4:3
}

// GetEffectivePixelScale — kept for Blueprint backward-compat.
// Returns the Y scale factor that maps the current PS1 height onto the fixed screen.
float UPSX2DRenderComponent::GetEffectivePixelScale() const
{
    float FixedW, FixedH;
    GetFixedScreenSize(FixedW, FixedH);
    const gpu::DisplayConfig* Disp = Gpu_ ? &Gpu_->display_config() : nullptr;
    const float PsxH = (Disp && Disp->height() > 0) ? static_cast<float>(Disp->height()) : 240.0f;
    return FixedH / PsxH;
}

// ===================================================================
// BindGpu - called by PSXEmulatorComponent after core init
// ===================================================================
void UPSX2DRenderComponent::BindGpu(gpu::Gpu* InGpu)
{
    UE_LOG(LogR3000Gpu, Verbose, TEXT("BindGpu called. InGpu=%p (was Gpu_=%p)"), InGpu, Gpu_);
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
        UE_LOG(LogR3000Gpu, Error, TEXT("BaseMaterial is NULL. Assign a material in the Blueprint or the mesh will be invisible."));
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

AActor* UPSX2DRenderComponent::ResolvePsxCameraDebugActor()
{
    if (PsxCameraDebugActor.IsValid())
        return PsxCameraDebugActor.Get();

    if (SpawnedPsxCameraDebugActor_)
        return SpawnedPsxCameraDebugActor_;

    if (!bAutoSpawnPsxCameraDebugActor || !GetWorld())
        return nullptr;

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Params.Name = TEXT("PSXCameraDebug2D");
    SpawnedPsxCameraDebugActor_ = GetWorld()->SpawnActor<APSXCameraDebugActor>(
        APSXCameraDebugActor::StaticClass(),
        GetComponentLocation(),
        GetComponentRotation(),
        Params);
    return SpawnedPsxCameraDebugActor_;
}

void UPSX2DRenderComponent::UpdatePsxCameraDebugActor()
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

    float FixedW, FixedH;
    GetFixedScreenSize(FixedW, FixedH);
    const float HalfWidth  = FixedW * 0.5f;
    const float HalfHeight = FixedH * 0.5f;
    constexpr float DebugFovDeg = 60.0f;
    const float HalfFovRad = FMath::DegreesToRadians(DebugFovDeg * 0.5f);
    const float CameraDistance = FMath::Max(
        100.0f,
        FMath::Max(HalfWidth, HalfHeight) / FMath::Tan(HalfFovRad));

    const FTransform PlaneTransform = GetComponentTransform();
    const FVector PivotWorld = PlaneTransform.GetLocation();
    const FVector ScreenCenter = PivotWorld;
    const FVector PlaneNormal = PlaneTransform.GetUnitAxis(EAxis::X);
    const FVector PlaneUp = PlaneTransform.GetUnitAxis(EAxis::Z);
    const FVector CameraPos = PivotWorld - PlaneNormal * CameraDistance;
    const FVector ViewDir = (ScreenCenter - CameraPos).GetSafeNormal();
    const FQuat CameraRot = FRotationMatrix::MakeFromXZ(
        ViewDir.IsNearlyZero() ? PlaneNormal : ViewDir,
        PlaneUp).ToQuat();

    CameraActor->SetActorHiddenInGame(false);
    CameraActor->SetActorTransform(FTransform(CameraRot, CameraPos));

    if (APSXCameraDebugActor* DebugActor = Cast<APSXCameraDebugActor>(CameraActor))
    {
        DebugActor->SetDebugColor(FColor(0, 220, 255));
        DebugActor->SetDebugText(FString::Printf(
            TEXT("PSX 2D Cam\n%.0fx%.0f (fixed)"),
            FixedW, FixedH));
    }
}

void UPSX2DRenderComponent::DrawPsxScreenFrameDebug() const
{
    if (!bShowPsxScreenFrameDebug || !GetWorld())
        return;

    float FixedW, FixedH;
    GetFixedScreenSize(FixedW, FixedH);

    const FTransform PlaneTransform = GetComponentTransform();
    const FVector Center = PlaneTransform.GetLocation();
    const FVector Right = PlaneTransform.GetUnitAxis(EAxis::Y);
    const FVector Up = PlaneTransform.GetUnitAxis(EAxis::Z);
    const FVector HalfRight = Right * (FixedW * 0.5f);
    const FVector HalfUp    = Up    * (FixedH * 0.5f);

    const FVector P0 = Center - HalfRight - HalfUp;
    const FVector P1 = Center + HalfRight - HalfUp;
    const FVector P2 = Center + HalfRight + HalfUp;
    const FVector P3 = Center - HalfRight + HalfUp;
    const FColor FrameColor(0, 220, 255);

    DrawDebugLine(GetWorld(), P0, P1, FrameColor, false, 0.0f, 0, 2.0f);
    DrawDebugLine(GetWorld(), P1, P2, FrameColor, false, 0.0f, 0, 2.0f);
    DrawDebugLine(GetWorld(), P2, P3, FrameColor, false, 0.0f, 0, 2.0f);
    DrawDebugLine(GetWorld(), P3, P0, FrameColor, false, 0.0f, 0, 2.0f);

    const FVector Pivot = PlaneTransform.GetLocation();
    const float CrossSize = 8.0f;
    DrawDebugLine(GetWorld(), Pivot - Right * CrossSize, Pivot + Right * CrossSize, FColor::Yellow, false, 0.0f, 0, 1.0f);
    DrawDebugLine(GetWorld(), Pivot - Up * CrossSize, Pivot + Up * CrossSize, FColor::Yellow, false, 0.0f, 0, 1.0f);
}

// ===================================================================
// Material instance management — lazy create/refresh
// ===================================================================
void UPSX2DRenderComponent::EnsureMaterialInstances()
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

void UPSX2DRenderComponent::RefreshMaterials()
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
void UPSX2DRenderComponent::SetVramTexture(UTexture2D* InTexture)
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
void UPSX2DRenderComponent::RebuildMesh()
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
        UE_LOG(LogR3000Gpu, Verbose, TEXT("RebuildMesh #%d: %d triangles in draw list, frame_id=%u"),
            sRebuildCount, NumCmds, DrawList.frame_id);
    }

    if (NumCmds == 0)
    {
        // PS1 double-buffer: every other VBlank the "ready" draw list may be
        // the one currently being drawn into (empty). Keep the previous mesh
        // until we see a significant run of empty frames (e.g. STR video).
        EmptyFrameCount_++;
        if (EmptyFrameCount_ < 10)
            return;
        if (MeshComp_)
            MeshComp_->ClearAllMeshSections();
        return;
    }
    EmptyFrameCount_ = 0;

    // Lazy-(re)create material instances if slots were assigned after BindGpu
    // (e.g., Blueprint BeginPlay sets MatSemi0-3 after InitEmulator already called BindGpu)
    EnsureMaterialInstances();

    // Log first time we receive primitives (confirms GPU bridge working)
    static bool bFirstPrimitives = true;
    if (bFirstPrimitives)
    {
        const bool bHasMat0 = MatInst_.IsValidIndex(0) && MatInst_[0] != nullptr;
        UE_LOG(LogR3000Gpu, Log, TEXT("First primitives received: %d triangles. Mat[0]=%d"), NumCmds, bHasMat0 ? 1 : 0);
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

    // ─────────────────────────────────────────────────────────────────────
    // PS1 → UE5 coordinate transform — DOUBLE-BUFFER COLLAPSE POLICY
    // ─────────────────────────────────────────────────────────────────────
    //
    // We DELIBERATELY ignore the two pieces of state that drive PS1
    // double-buffering and render-targets:
    //
    //   1. draw_env.offset_x / offset_y  (where the GPU writes in VRAM)
    //   2. display.display_x / display_y (where the CRTC scans out from VRAM)
    //
    // The core already stores DrawCmd.v[].x/y as raw GP0 polygon coords (see
    // the contract on gpu::DrawVertex in src/gpu/gpu.h), so we just feed those
    // straight through. Concretely this means:
    //
    //   • Both halves of a double-buffered game (the typical pattern: draw to
    //     VRAM(0,0) one frame, draw to VRAM(0,256) the next, flip display_y in
    //     sync) collapse onto the SAME logical frame in UE world space. The
    //     mesh becomes stable across the buffer flip — visually you only ever
    //     see "one screen".
    //
    //   • Pivot is *always* at the centre of the logical screen
    //     (width/2, height/2). The component origin maps to the screen centre
    //     so the 2D layer can be placed inside a 3D world (billboards,
    //     spatial UI, etc.) without any extra offset. There is no flag — the
    //     same convention applies to the 3D component and to PSXSurface.
    //
    // ── KNOWN LIMITATION: render-to-texture (RTT) ────────────────────────
    // A game that renders into a sub-region of VRAM that is NOT the current
    // display window (e.g. a reflection map, a UI sub-texture, a mini-map
    // baked into a texture page) will have its draw_offset point OUTSIDE
    // (display_x..display_x+width, display_y..display_y+height). With the
    // current policy that RTT geometry will get smashed onto the main frame.
    // This is a known TODO — once we hit a real RTT case, we'll detect it by
    // comparing (offset + bbox) against the display window and route those
    // draws to a separate render section.
    //
    // DO NOT ADD display_x/y, offset_x/y, or a synthetic width/2,height/2
    // translation back into this transform.
    //
    // DrawVertex carries the logical GP0 screen coords consumed by the
    // UE front-end. On TREX those coords are already centered around the
    // projection origin (see DRAWLIST_SUMMARY raw_xy), so subtracting the
    // scan-out half-size shifts the whole frame off the component pivot.
    // ─────────────────────────────────────────────────────────────────────
    const gpu::DisplayConfig& Disp = DrawList.display;

    // Fixed 4:3 CRT screen: every PS1 resolution maps onto the same UE5 rectangle.
    // Separate ScaleX/ScaleY fill the fixed area exactly (like a real tube, no letterbox).
    // PS1 screen centre → component origin (0,0) so camera placement never changes.
    float FixedW, FixedH;
    GetFixedScreenSize(FixedW, FixedH);
    const float PsxW = FMath::Max(1.0f, static_cast<float>(Disp.width()));
    const float PsxH = FMath::Max(1.0f, static_cast<float>(Disp.height()));
    const float ScaleX  = FixedW / PsxW;   // may differ from ScaleY for non-4:3 PS1 modes
    const float ScaleY  = FixedH / PsxH;
    // Screen centre in game coordinate space:
    //   draw_env.offset is where the GPU places game(0,0) in VRAM — so the display
    //   centre in game coords = PsxW/2 - offset_x (avoid display_x: it flips with
    //   double-buffering and would shift the whole image every other frame).
    const float CenterGx = PsxW * 0.5f - static_cast<float>(DrawList.draw_env.offset_x);
    const float CenterGy = PsxH * 0.5f - static_cast<float>(DrawList.draw_env.offset_y);

    if (bDebugMeshLog)
    {
        emu::logf(emu::LogLevel::info, "GPU",
            "MeshRebuild: %d tris | psx=%ux%u | fixed=%.0fx%.0f | scaleX=%.3f scaleY=%.3f | center=(%.1f,%.1f) offset=(%d,%d)",
            NumCmds, Disp.width(), Disp.height(), FixedW, FixedH, ScaleX, ScaleY,
            CenterGx, CenterGy, DrawList.draw_env.offset_x, DrawList.draw_env.offset_y);
    }

    // Collect runs: walk draw list, flush section on material change
    TArray<RunSection> Runs;
    Runs.Reserve(32);  // Typical: 5-20 material transitions per frame

    int32 CurMatIdx = -1;  // Force first primitive to open a run
    RunSection* Cur = nullptr;
    float Ps1MinX = 1e9f, Ps1MaxX = -1e9f, Ps1MinY = 1e9f, Ps1MaxY = -1e9f;
    int32 SemiTriCounts[kNumSections] = {};
    int32 WeirdTriLogCount = 0;
    const float WeirdAbsX = FMath::Max(512.0f, static_cast<float>(Disp.width()) * 2.0f);
    const float WeirdAbsY = FMath::Max(512.0f, static_cast<float>(Disp.height()) * 2.0f);
    const float NearZeroAreaThreshold = 8.0f;

    for (int32 i = 0; i < NumCmds; ++i)
    {
        const gpu::DrawCmd& Cmd = DrawList.cmds[i];
        const float Depth = static_cast<float>(NumCmds - 1 - i) * ZStep;
        const gpu::DrawVertex& Va = Cmd.v[0];
        const gpu::DrawVertex& Vb = Cmd.v[1];
        const gpu::DrawVertex& Vc = Cmd.v[2];

        const bool bWeirdCoord =
            FMath::Abs(static_cast<float>(Va.x)) > WeirdAbsX || FMath::Abs(static_cast<float>(Va.y)) > WeirdAbsY ||
            FMath::Abs(static_cast<float>(Vb.x)) > WeirdAbsX || FMath::Abs(static_cast<float>(Vb.y)) > WeirdAbsY ||
            FMath::Abs(static_cast<float>(Vc.x)) > WeirdAbsX || FMath::Abs(static_cast<float>(Vc.y)) > WeirdAbsY;
        const int32 Area2D =
            (static_cast<int32>(Vb.x) - static_cast<int32>(Va.x)) * (static_cast<int32>(Vc.y) - static_cast<int32>(Va.y)) -
            (static_cast<int32>(Vb.y) - static_cast<int32>(Va.y)) * (static_cast<int32>(Vc.x) - static_cast<int32>(Va.x));
        const bool bNearZeroArea = FMath::Abs(static_cast<float>(Area2D)) <= NearZeroAreaThreshold;

        if ((bWeirdCoord || bNearZeroArea) && WeirdTriLogCount < 12)
        {
            emu::logf(
                emu::LogLevel::warn,
                "GPU",
                "WeirdTri frame=%u tri=%d mat=%d area=%d weird=%d near0=%d v0=(%d,%d) v1=(%d,%d) v2=(%d,%d) disp=%ux%u",
                DrawList.frame_id,
                i,
                (Cmd.flags & 2) ? 1 + (Cmd.semi_mode & 3) : 0,
                Area2D,
                bWeirdCoord ? 1 : 0,
                bNearZeroArea ? 1 : 0,
                Va.x, Va.y,
                Vb.x, Vb.y,
                Vc.x, Vc.y,
                static_cast<unsigned>(Disp.width()),
                static_cast<unsigned>(Disp.height()));
            ++WeirdTriLogCount;
        }

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

            const float dx = static_cast<float>(V.x) + 0.5f;
            const float dy = static_cast<float>(V.y) + 0.5f;
            Cur->Vertices.Add(FVector(Depth,
                (dx - CenterGx) * ScaleX + DisplayOffset.X,
                -(dy - CenterGy) * ScaleY + DisplayOffset.Y));

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
void UPSX2DRenderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    // DEBUG: Log first few ticks to verify TickComponent is running
    static int sTickCount = 0;
    sTickCount++;
    if (sTickCount <= 5 || (sTickCount % 300) == 0)
    {
        UE_LOG(LogR3000Gpu, VeryVerbose, TEXT("TickComponent #%d: Gpu_=%p"), sTickCount, Gpu_);
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
            UE_LOG(LogR3000Gpu, Error, TEXT("TickComponent: Gpu_ magic=0x%08X (expected 0x%08X) - stopping until rebind"),
                Gpu_->magic_, gpu::Gpu::kMagicValid);
            emu::logf(emu::LogLevel::error, "GPU", "GpuComponent stale GPU pointer detected (magic=0x%08X expected=0x%08X) - skipping tick until rebind",
                Gpu_->magic_, gpu::Gpu::kMagicValid);
            sStaleLogCount++;
        }
        if (MeshComp_ && MeshComp_->IsVisible())
            MeshComp_->SetVisibility(false);
        return;
    }

    // If disabled, hide mesh
    if (!bEnabled)
    {
        UpdatePsxCameraDebugActor();
        DrawPsxScreenFrameDebug();
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
            UE_LOG(LogR3000Gpu, Verbose, TEXT("RebuildMesh: frame %u -> %u (LastTriCount=%d)"),
                LastVramFrame_, CurrentFrame, LastTriCount_);
            sLoggedFrames++;
        }
        RebuildMesh();
        LastVramFrame_ = CurrentFrame;
    }

    UpdatePsxCameraDebugActor();
    DrawPsxScreenFrameDebug();
}

// ===================================================================
// Display info accessors
// ===================================================================
int32 UPSX2DRenderComponent::GetDisplayWidth() const
{
    return Gpu_ ? static_cast<int32>(Gpu_->display_config().width()) : 320;
}

int32 UPSX2DRenderComponent::GetDisplayHeight() const
{
    return Gpu_ ? static_cast<int32>(Gpu_->display_config().height()) : 240;
}

bool UPSX2DRenderComponent::IsDisplayEnabled() const
{
    return Gpu_ ? Gpu_->display_config().display_enabled : false;
}

