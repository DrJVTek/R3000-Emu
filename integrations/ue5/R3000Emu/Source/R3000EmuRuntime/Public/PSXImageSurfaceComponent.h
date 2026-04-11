#pragma once

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Texture2D.h"
#include "PSXImageSurfaceComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPSX2DRenderComponent;
class UPSXSurfaceComponent;
namespace gpu { class Gpu; struct CpuVramWriteInfo; }

/**
 * Presents static display images written directly into VRAM.
 *
 * Current detection is still based on CPU->VRAM writes compatible with the
 * display area. Longer term, this should become one source handled by the
 * unified PSX surface system.
 */
UCLASS(ClassGroup = (PSXEmu), meta = (BlueprintSpawnableComponent))
class UPSXImageSurfaceComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPSXImageSurfaceComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    void BindGpu(gpu::Gpu* InGpu);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Image")
    UMaterialInterface* ImageMaterial{nullptr};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Image", meta = (ClampMin = "1.0"))
    float PlaneWidth{320.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Image|Debug")
    bool bHideGpu2DWhileImageVisible{true};

    /** Mirror static-image detection into the unified PSX surface component while keeping legacy rendering active. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Image")
    bool bMirrorToUnifiedSurface{true};

    /** When a unified PSX surface is available, let it be the visible presenter instead of this legacy mesh. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Image")
    bool bPreferUnifiedSurfacePresenter{true};

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Image")
    bool IsImageActive() const { return bImageVisible_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Image")
    UTexture2D* GetImageTexture() const { return ImageTexture_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Image")
    UProceduralMeshComponent* GetImageMesh() const { return ImageMesh_; }

private:
    void CreateOrResizeTexture(int32 W, int32 H);
    void UploadImageFrame(int32 W, int32 H);
    void DumpImageFrameToPpm(int32 W, int32 H, uint32 VramSeq);
    void RebuildPlaneMesh(int32 W, int32 H);
    void SetImageVisible(bool bShow);
    bool IsWriteEligible(const gpu::CpuVramWriteInfo& Write) const;
    bool DoesCurrentDisplayMatchLatch() const;
    void SetGpu2DVisible(bool bGpuVisible);
    void SyncUnifiedSurface(bool bSurfaceVisible);
    bool ShouldUseUnifiedSurfacePresenter() const;

    gpu::Gpu* Gpu_{nullptr};
    TWeakObjectPtr<UPSX2DRenderComponent> Gpu2DComp_;
    TWeakObjectPtr<UPSXSurfaceComponent> SurfaceComp_;

    UPROPERTY()
    UProceduralMeshComponent* ImageMesh_{nullptr};
    UPROPERTY()
    UTexture2D* ImageTexture_{nullptr};
    UPROPERTY()
    UMaterialInstanceDynamic* ImageMatInst_{nullptr};

    uint16* VramCopyBuffer_{nullptr};
    uint8* PixelBuffer_{nullptr};
    FUpdateTextureRegion2D* UpdateRegion_{nullptr};

    int32 ImageTexW_{0};
    int32 ImageTexH_{0};
    bool bImageVisible_{false};
    bool bHasRealContent_{false};
    uint32 ImageVisibleFrames_{0};
    uint32 LastSeenCpuWriteSeq_{0};
    uint32 LastUploadedVramSeq_{0};
    uint32 DumpedImageFrames_{0};
    uint16 LatchedDisplayX_{0};
    uint16 LatchedDisplayY_{0};
    uint16 LatchedDisplayW_{0};
    uint16 LatchedDisplayH_{0};
    bool LatchedDisplay24Bit_{false};
    bool LatchedDisplayEnabled_{false};

    static constexpr int32 kVramW = 1024;
    static constexpr int32 kVramH = 512;
};
