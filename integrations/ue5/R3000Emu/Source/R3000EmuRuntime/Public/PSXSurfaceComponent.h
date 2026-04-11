#pragma once

#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "ProceduralMeshComponent.h"
#include "PSXSurfaceComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

namespace gpu { class Gpu; }

UENUM(BlueprintType)
enum class EPSXSurfaceMode : uint8
{
    None UMETA(DisplayName = "None"),
    VideoFmv UMETA(DisplayName = "Video FMV"),
    StaticImage UMETA(DisplayName = "Static Image"),
    DirectDisplay UMETA(DisplayName = "Direct Display"),
    HoldLastSurface UMETA(DisplayName = "Hold Last Surface"),
    Hybrid UMETA(DisplayName = "Hybrid")
};

UENUM(BlueprintType)
enum class EPSXSurfaceModePolicy : uint8
{
    AutoDetect UMETA(DisplayName = "Auto Detect"),
    PreferVideo UMETA(DisplayName = "Prefer Video"),
    PreferImage UMETA(DisplayName = "Prefer Image"),
    PreferDirect UMETA(DisplayName = "Prefer Direct"),
    PreferHold UMETA(DisplayName = "Prefer Hold"),
    ForceSurfaceOn UMETA(DisplayName = "Force Surface On"),
    ForceSurfaceOff UMETA(DisplayName = "Force Surface Off")
};

UENUM(BlueprintType)
enum class EPSXSurfaceShape : uint8
{
    Plane UMETA(DisplayName = "Plane"),
    CurvedPlane UMETA(DisplayName = "Curved Plane"),
    Hemisphere UMETA(DisplayName = "Hemisphere"),
    CustomMesh UMETA(DisplayName = "Custom Mesh")
};

UENUM(BlueprintType)
enum class EPSXSurfaceBufferingMode : uint8
{
    AutoDetect UMETA(DisplayName = "Auto Detect"),
    Mono UMETA(DisplayName = "Mono"),
    DoubleBuffer UMETA(DisplayName = "Double Buffer"),
    TripleBuffer UMETA(DisplayName = "Triple Buffer")
};

UENUM(BlueprintType)
enum class EPSXSurfaceSamplingPolicy : uint8
{
    Stretch UMETA(DisplayName = "Stretch"),
    PreserveAspect UMETA(DisplayName = "Preserve Aspect"),
    Letterbox UMETA(DisplayName = "Letterbox"),
    Crop UMETA(DisplayName = "Crop")
};

UENUM(BlueprintType)
enum class EPSXSurfaceAnchor : uint8
{
    CameraLocked UMETA(DisplayName = "Camera Locked"),
    WorldLocked UMETA(DisplayName = "World Locked"),
    ProfileDefined UMETA(DisplayName = "Profile Defined")
};

USTRUCT(BlueprintType)
struct FPSXSurfaceDecision
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    bool bVisible{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    EPSXSurfaceMode Mode{EPSXSurfaceMode::None};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    EPSXSurfaceShape Shape{EPSXSurfaceShape::Plane};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    EPSXSurfaceBufferingMode Buffering{EPSXSurfaceBufferingMode::AutoDetect};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    float PhysicalWidth{320.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    float AspectRatio{4.0f / 3.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    float Distance{150.0f};
};

UCLASS(ClassGroup = (PSXEmu), meta = (BlueprintSpawnableComponent))
class UPSXSurfaceComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPSXSurfaceComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    void BindGpu(gpu::Gpu* InGpu);

    UFUNCTION(BlueprintCallable, Category = "PSXSurface")
    void SetSurfaceTexture(UTexture2D* InTexture);

    UFUNCTION(BlueprintCallable, Category = "PSXSurface")
    void ApplyDecision(const FPSXSurfaceDecision& InDecision);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXSurface")
    bool IsSurfaceVisible() const { return bSurfaceVisible_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXSurface")
    EPSXSurfaceMode GetResolvedMode() const { return ResolvedMode_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXSurface")
    EPSXSurfaceShape GetResolvedShape() const { return ResolvedShape_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXSurface")
    UTexture2D* GetSurfaceTexture() const { return SurfaceTexture_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXSurface")
    UProceduralMeshComponent* GetGeneratedMeshComponent() const { return GeneratedMesh_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXSurface")
    UStaticMeshComponent* GetCustomMeshComponent() const { return StaticMeshComp_; }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    UMaterialInterface* SurfaceMaterial{nullptr};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    EPSXSurfaceModePolicy ModePolicy{EPSXSurfaceModePolicy::AutoDetect};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    EPSXSurfaceShape Shape{EPSXSurfaceShape::Plane};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    EPSXSurfaceBufferingMode BufferingMode{EPSXSurfaceBufferingMode::AutoDetect};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    EPSXSurfaceSamplingPolicy SamplingPolicy{EPSXSurfaceSamplingPolicy::PreserveAspect};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    EPSXSurfaceAnchor Anchor{EPSXSurfaceAnchor::CameraLocked};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface", meta = (ClampMin = "1.0"))
    float PhysicalWidth{320.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface", meta = (ClampMin = "0.1"))
    float Distance{150.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Curvature{0.15f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface", meta = (ClampMin = "30.0", ClampMax = "180.0"))
    float HemisphereFovDeg{100.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    UStaticMesh* CustomMesh{nullptr};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    bool bUseGeneratedMesh{true};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    bool bAllowOverlayOverSurface{true};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXSurface")
    bool bAllowSurfaceWith3D{true};

private:
    void EnsureRenderComponents();
    void RebuildGeneratedMesh(float AspectRatio);
    void ApplyMaterial();
    void ApplyVisibility();
    void ApplyActiveShape();

    gpu::Gpu* Gpu_{nullptr};

    UPROPERTY()
    UProceduralMeshComponent* GeneratedMesh_{nullptr};

    UPROPERTY()
    UStaticMeshComponent* StaticMeshComp_{nullptr};

    UPROPERTY()
    UTexture2D* SurfaceTexture_{nullptr};

    UPROPERTY()
    UMaterialInstanceDynamic* SurfaceMatInst_{nullptr};

    FPSXSurfaceDecision ActiveDecision_{};
    EPSXSurfaceMode ResolvedMode_{EPSXSurfaceMode::None};
    EPSXSurfaceShape ResolvedShape_{EPSXSurfaceShape::Plane};
    bool bSurfaceVisible_{false};
};
