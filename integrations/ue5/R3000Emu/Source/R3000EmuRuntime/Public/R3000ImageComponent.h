#pragma once

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Texture2D.h"
#include "R3000ImageComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UR3000GpuComponent;
namespace gpu { class Gpu; struct CpuVramWriteInfo; }

/**
 * Static display-image component.
 *
 * Shows a plane only when the GPU receives a direct CPU->VRAM DMA write that
 * exactly matches the current display area. Decoding follows the active display
 * format (15-bit or 24-bit). Once latched, the plane stays visible until the
 * display format or display start changes.
 */
UCLASS(ClassGroup = (R3000Emu), meta = (BlueprintSpawnableComponent))
class UR3000ImageComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UR3000ImageComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    void BindGpu(gpu::Gpu* InGpu);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Image")
    UMaterialInterface* ImageMaterial{nullptr};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Image", meta = (ClampMin = "1.0"))
    float PlaneWidth{320.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Image|Debug")
    bool bHideGpu2DWhileImageVisible{true};

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|Image")
    bool IsImageActive() const { return bImageVisible_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|Image")
    UTexture2D* GetImageTexture() const { return ImageTexture_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|Image")
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

    gpu::Gpu* Gpu_{nullptr};
    TWeakObjectPtr<UR3000GpuComponent> Gpu2DComp_;

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
