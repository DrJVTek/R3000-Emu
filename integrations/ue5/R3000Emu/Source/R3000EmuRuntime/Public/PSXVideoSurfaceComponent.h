#pragma once

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Texture2D.h"
#include "PSXVideoSurfaceComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPSXSurfaceComponent;
namespace gpu { class Gpu; }

/**
 * Presents PS1 video content decoded into the display area.
 *
 * Current implementation still focuses on MDEC-driven full-screen video.
 * Longer term, this component is expected to become a presenter behind the
 * unified surface system described in the strategy docs.
 */
UCLASS(ClassGroup = (PSXEmu), meta = (BlueprintSpawnableComponent))
class UPSXVideoSurfaceComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPSXVideoSurfaceComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Connects the component to the emulated GPU. */
    void BindGpu(gpu::Gpu* InGpu);

    // ─── Display settings ───────────────────────────────────────────

    /** Material for the video plane. Must have a Texture2D parameter "VideoTexture". */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Video")
    UMaterialInterface* VideoMaterial{nullptr};

    /** Width of the video plane in UE world units. Height is auto-computed from aspect ratio. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Video", meta = (ClampMin = "1.0"))
    float PlaneWidth{320.0f};

    /** Frames of non-video mode before hiding the plane. Set 0 to hide immediately. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Video", meta = (ClampMin = "0"))
    int32 HideDelayFrames{10};

    // ─── Surface settings ────────────────────────────────────────────

    /** Render on a sphere section instead of a flat plane. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Video|VR")
    bool bSphereMode{false};

    /** Sphere radius in world units. Only used when bSphereMode=true. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Video|VR",
              meta = (ClampMin = "10.0", EditCondition = "bSphereMode"))
    float SphereRadius{500.0f};

    /** Mirror video detection into the unified PSX surface component while keeping legacy rendering active. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Video")
    bool bMirrorToUnifiedSurface{true};

    /** When a unified PSX surface is available, let it be the visible presenter instead of this legacy mesh. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Video")
    bool bPreferUnifiedSurfacePresenter{true};

    // ─── Runtime queries ─────────────────────────────────────────────

    /** True when the PS1 GPU is in 24-bit display mode (video active). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Video")
    bool IsVideoActive() const { return bVideoVisible_; }

    /** The current video texture (RGBA8, sized to the PS1 display area). May be null before first frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Video")
    UTexture2D* GetVideoTexture() const { return VideoTexture_; }

    /** PS1 display width of the current video frame (0 if no video). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Video")
    int32 GetVideoWidth() const { return VideoTexW_; }

    /** PS1 display height of the current video frame (0 if no video). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Video")
    int32 GetVideoHeight() const { return VideoTexH_; }

    /** The procedural mesh used to present the current video surface. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Video")
    UProceduralMeshComponent* GetVideoMesh() const { return VideoMesh_; }

private:
    void CreateOrResizeTexture(int32 W, int32 H);
    void UploadVideoFrame(int32 W, int32 H);
    void RebuildPlaneMesh(int32 W, int32 H);
    void SetVideoVisible(bool bShow);
    void SyncUnifiedSurface(bool bSurfaceVisible);
    bool ShouldUseUnifiedSurfacePresenter() const;

    gpu::Gpu* Gpu_{nullptr};
    TWeakObjectPtr<UPSXSurfaceComponent> SurfaceComp_;

    UPROPERTY()
    UProceduralMeshComponent* VideoMesh_{nullptr};
    UPROPERTY()
    UTexture2D* VideoTexture_{nullptr};
    UPROPERTY()
    UMaterialInstanceDynamic* VideoMatInst_{nullptr};

    // VRAM copy buffer (1024x512 uint16) + per-frame pixel buffer
    uint16* VramCopyBuffer_{nullptr};
    uint8*  PixelBuffer_{nullptr};
    FUpdateTextureRegion2D* UpdateRegion_{nullptr};

    int32  VideoTexW_{0};       // current texture dimensions (0 = not created)
    int32  VideoTexH_{0};
    int32  FramesSinceVideo_{0};
    bool   bVideoVisible_{false};
    bool   bIsVideo15Bit_{false};  // true when showing 15-bit MDEC video
    bool   bHasRealContent_{false}; // true when frame has >5% non-black pixels
    uint32 DumpedFrames_{0};        // PPM dump counter
    uint32 LastCpuWriteSeq_{0};    // track CPU→VRAM writes for 15-bit detection
    uint32 LastVramSeq_{0};        // track vram_write_seq for rapid-update detection
    uint32 ConsecutiveWrites_{0};  // consecutive CPU→VRAM writes (rapid = video)
    uint32 FramesSinceLastWrite_{0}; // ticks since last new CPU→VRAM write
    uint16 SrcX_{0};               // VRAM source X for current frame (DMA or display)
    uint16 SrcY_{0};               // VRAM source Y for current frame

    static constexpr int32 kVramW = 1024;
    static constexpr int32 kVramH = 512;
};
