#include "PSXSurfaceComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Logging/LogMacros.h"
#include "Materials/MaterialInstanceDynamic.h"

#include "gpu/gpu.h"
#include "log/emu_log.h"

DEFINE_LOG_CATEGORY_STATIC(LogPSXSurface, Log, All);

UPSXSurfaceComponent::UPSXSurfaceComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    SetMobility(EComponentMobility::Movable);
}

void UPSXSurfaceComponent::BeginPlay()
{
    Super::BeginPlay();
    EnsureRenderComponents();
    ApplyMaterial();
    ApplyVisibility();
}

void UPSXSurfaceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Gpu_ = nullptr;
    Super::EndPlay(EndPlayReason);
}

void UPSXSurfaceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UPSXSurfaceComponent::BindGpu(gpu::Gpu* InGpu)
{
    Gpu_ = InGpu;
    UE_LOG(LogPSXSurface, Log, TEXT("PSX surface component bound to GPU"));
    emu::logf(emu::LogLevel::warn, "SURFACE", "PSXSurfaceComponent bound to GPU");
}

void UPSXSurfaceComponent::SetSurfaceTexture(UTexture2D* InTexture)
{
    SurfaceTexture_ = InTexture;
    ApplyMaterial();
}

void UPSXSurfaceComponent::ApplyDecision(const FPSXSurfaceDecision& InDecision)
{
    const bool bOldVisible = bSurfaceVisible_;
    const EPSXSurfaceMode OldMode = ResolvedMode_;
    const EPSXSurfaceShape OldShape = ResolvedShape_;
    const EPSXSurfaceBufferingMode OldBuffering = BufferingMode;

    ActiveDecision_ = InDecision;
    ResolvedMode_ = InDecision.Mode;
    ResolvedShape_ = InDecision.Shape;
    bSurfaceVisible_ = InDecision.bVisible && InDecision.Mode != EPSXSurfaceMode::None;

    if (InDecision.PhysicalWidth > 0.0f)
        PhysicalWidth = InDecision.PhysicalWidth;
    if (InDecision.Distance > 0.0f)
        Distance = InDecision.Distance;
    if (InDecision.Buffering != EPSXSurfaceBufferingMode::AutoDetect)
        BufferingMode = InDecision.Buffering;

    EnsureRenderComponents();
    RebuildGeneratedMesh(FMath::Max(InDecision.AspectRatio, 0.1f));
    ApplyActiveShape();
    ApplyMaterial();
    ApplyVisibility();

    if (bOldVisible != bSurfaceVisible_ ||
        OldMode != ResolvedMode_ ||
        OldShape != ResolvedShape_ ||
        OldBuffering != BufferingMode)
    {
        UE_LOG(
            LogPSXSurface,
            Log,
            TEXT("Surface decision visible=%d mode=%d shape=%d buffering=%d width=%.1f aspect=%.3f distance=%.1f"),
            bSurfaceVisible_ ? 1 : 0,
            static_cast<int32>(ResolvedMode_),
            static_cast<int32>(ResolvedShape_),
            static_cast<int32>(BufferingMode),
            PhysicalWidth,
            InDecision.AspectRatio,
            Distance);
        emu::logf(
            emu::LogLevel::warn,
            "SURFACE",
            "decision visible=%d mode=%d shape=%d buffering=%d width=%.1f aspect=%.3f distance=%.1f",
            bSurfaceVisible_ ? 1 : 0,
            static_cast<int32>(ResolvedMode_),
            static_cast<int32>(ResolvedShape_),
            static_cast<int32>(BufferingMode),
            PhysicalWidth,
            InDecision.AspectRatio,
            Distance);
    }
}

void UPSXSurfaceComponent::EnsureRenderComponents()
{
    if (!GeneratedMesh_)
    {
        GeneratedMesh_ = NewObject<UProceduralMeshComponent>(GetOwner(), TEXT("PSXSurfaceGeneratedMesh"));
        GeneratedMesh_->bUseAsyncCooking = false;
        GeneratedMesh_->SetCastShadow(false);
        GeneratedMesh_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        GeneratedMesh_->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        GeneratedMesh_->RegisterComponent();
    }

    if (!StaticMeshComp_)
    {
        StaticMeshComp_ = NewObject<UStaticMeshComponent>(GetOwner(), TEXT("PSXSurfaceStaticMesh"));
        StaticMeshComp_->SetCastShadow(false);
        StaticMeshComp_->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        StaticMeshComp_->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        StaticMeshComp_->RegisterComponent();
    }
}

void UPSXSurfaceComponent::RebuildGeneratedMesh(float AspectRatio)
{
    EnsureRenderComponents();

    if (!GeneratedMesh_)
        return;

    const float Width = FMath::Max(PhysicalWidth, 1.0f);
    const float Height = Width / FMath::Max(AspectRatio, 0.1f);
    const float HalfW = Width * 0.5f;

    if (ResolvedShape_ == EPSXSurfaceShape::CustomMesh)
        return;

    if (ResolvedShape_ == EPSXSurfaceShape::Hemisphere)
    {
        UE_LOG(LogPSXSurface, Verbose, TEXT("Hemisphere shape is not implemented yet; using curved plane fallback."));
    }

    TArray<FVector> Verts;
    TArray<int32> Tris;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<FProcMeshTangent> Tangents;

    const int32 Segments = (ResolvedShape_ == EPSXSurfaceShape::Plane) ? 1 : 24;
    const float ArcDepth = (ResolvedShape_ == EPSXSurfaceShape::Plane) ? 0.0f : (Width * Curvature);

    for (int32 i = 0; i <= Segments; ++i)
    {
        const float T = Segments > 0 ? static_cast<float>(i) / static_cast<float>(Segments) : 0.0f;
        const float Y = FMath::Lerp(-HalfW, HalfW, T);
        const float Normalized = (T - 0.5f) * 2.0f;
        const float X = (ResolvedShape_ == EPSXSurfaceShape::Plane)
            ? 0.0f
            : -(1.0f - (Normalized * Normalized)) * ArcDepth;

        Verts.Add(FVector(X, Y, Height));
        Verts.Add(FVector(X, Y, 0.0f));

        const FVector Normal = FVector(1.0f, 0.0f, 0.0f).GetSafeNormal();
        Normals.Add(Normal);
        Normals.Add(Normal);

        UVs.Add(FVector2D(T, 0.0f));
        UVs.Add(FVector2D(T, 1.0f));

        const FProcMeshTangent Tangent(FVector(0.0f, 1.0f, 0.0f), false);
        Tangents.Add(Tangent);
        Tangents.Add(Tangent);
    }

    for (int32 i = 0; i < Segments; ++i)
    {
        const int32 Base = i * 2;
        Tris.Add(Base + 0);
        Tris.Add(Base + 3);
        Tris.Add(Base + 1);

        Tris.Add(Base + 0);
        Tris.Add(Base + 2);
        Tris.Add(Base + 3);
    }

    GeneratedMesh_->CreateMeshSection(
        0, Verts, Tris, Normals, UVs,
        TArray<FVector2D>{}, TArray<FVector2D>{}, TArray<FVector2D>{},
        TArray<FColor>{}, Tangents,
        false);
}

void UPSXSurfaceComponent::ApplyMaterial()
{
    if (!SurfaceMaterial)
    {
        emu::logf(emu::LogLevel::warn, "SURFACE", "SurfaceMaterial is NULL - surface will be invisible");
        return;
    }

    if (!SurfaceMatInst_ || SurfaceMatInst_->GetBaseMaterial() != SurfaceMaterial->GetBaseMaterial())
        SurfaceMatInst_ = UMaterialInstanceDynamic::Create(SurfaceMaterial, this);

    if (SurfaceTexture_)
    {
        SurfaceMatInst_->SetTextureParameterValue(TEXT("VideoTexture"), SurfaceTexture_);
        SurfaceMatInst_->SetTextureParameterValue(TEXT("ImageTexture"), SurfaceTexture_);
        SurfaceMatInst_->SetTextureParameterValue(TEXT("FrameTexture"), SurfaceTexture_);
    }

    if (GeneratedMesh_)
        GeneratedMesh_->SetMaterial(0, SurfaceMatInst_);
    if (StaticMeshComp_)
        StaticMeshComp_->SetMaterial(0, SurfaceMatInst_);
}

void UPSXSurfaceComponent::ApplyVisibility()
{
    if (GeneratedMesh_)
    {
        const bool bShowGenerated = bUseGeneratedMesh && bSurfaceVisible_ && ResolvedShape_ != EPSXSurfaceShape::CustomMesh;
        GeneratedMesh_->SetVisibility(bShowGenerated, false);
        GeneratedMesh_->SetHiddenInGame(!bShowGenerated, false);
    }

    if (StaticMeshComp_)
    {
        const bool bShowStatic = bSurfaceVisible_ && ResolvedShape_ == EPSXSurfaceShape::CustomMesh;
        StaticMeshComp_->SetVisibility(bShowStatic, false);
        StaticMeshComp_->SetHiddenInGame(!bShowStatic, false);
    }
}

void UPSXSurfaceComponent::ApplyActiveShape()
{
    EnsureRenderComponents();

    SetRelativeLocation(FVector(Distance, 0.0f, 0.0f));

    if (StaticMeshComp_)
    {
        StaticMeshComp_->SetStaticMesh((ResolvedShape_ == EPSXSurfaceShape::CustomMesh) ? CustomMesh : nullptr);
    }
}
