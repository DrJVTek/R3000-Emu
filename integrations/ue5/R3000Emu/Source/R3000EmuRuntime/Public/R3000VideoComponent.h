#pragma once

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Texture2D.h"
#include "R3000VideoComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
namespace gpu { class Gpu; }

/**
 * PS1 FMV video component — auto-detects 24-bit GPU display mode (MDEC video)
 * and renders the decoded framebuffer onto a plane (or sphere for VR).
 *
 * Usage: place on the same Actor as R3000EmuComponent.
 * The plane appears automatically when 24-bit mode is active and hides
 * after HideDelayFrames of inactivity. No manual intervention needed.
 *
 * Material setup: Unlit/Opaque material with a Texture2D parameter named "VideoTexture".
 * The texture is RGBA8, sized to the PS1 display area (e.g. 320x240).
 */
UCLASS(ClassGroup = (R3000Emu), meta = (BlueprintSpawnableComponent))
class UR3000VideoComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UR3000VideoComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Called by R3000EmuComponent after core init — connects to the PS1 GPU. */
    void BindGpu(gpu::Gpu* InGpu);

    // ─── Display settings ───────────────────────────────────────────

    /** Material for the video plane. Must have a Texture2D parameter "VideoTexture". */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Video")
    UMaterialInterface* VideoMaterial{nullptr};

    /** Width of the video plane in UE world units. Height is auto-computed from aspect ratio. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Video", meta = (ClampMin = "1.0"))
    float PlaneWidth{320.0f};

    /** Frames of non-video mode before hiding the plane. Set 0 to hide immediately. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Video", meta = (ClampMin = "0"))
    int32 HideDelayFrames{10};

    // ─── VR settings (future) ────────────────────────────────────────

    /** [Future VR] Render on a sphere section instead of flat plane. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Video|VR")
    bool bSphereMode{false};

    /** [Future VR] Sphere radius (world units). Only used when bSphereMode=true. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Video|VR",
              meta = (ClampMin = "10.0", EditCondition = "bSphereMode"))
    float SphereRadius{500.0f};

    // ─── Runtime queries ─────────────────────────────────────────────

    /** True when the PS1 GPU is in 24-bit display mode (video active). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|Video")
    bool IsVideoActive() const { return bVideoVisible_; }

    /** The current video texture (RGBA8, sized to the PS1 display area). May be null before first frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|Video")
    UTexture2D* GetVideoTexture() const { return VideoTexture_; }

    /** PS1 display width of the current video frame (0 if no video). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|Video")
    int32 GetVideoWidth() const { return VideoTexW_; }

    /** PS1 display height of the current video frame (0 if no video). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|Video")
    int32 GetVideoHeight() const { return VideoTexH_; }

    /** The procedural mesh holding the video plane. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|Video")
    UProceduralMeshComponent* GetVideoMesh() const { return VideoMesh_; }

private:
    void CreateOrResizeTexture(int32 W, int32 H);
    void UploadVideoFrame(int32 W, int32 H);
    void RebuildPlaneMesh(int32 W, int32 H);
    void SetVideoVisible(bool bShow);

    gpu::Gpu* Gpu_{nullptr};

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

    static constexpr int32 kVramW = 1024;
    static constexpr int32 kVramH = 512;
};
