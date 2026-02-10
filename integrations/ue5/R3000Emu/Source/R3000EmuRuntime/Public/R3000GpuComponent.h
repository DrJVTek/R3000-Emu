#pragma once

#include "Components/ActorComponent.h"
#include "ProceduralMeshComponent.h"
#include "R3000GpuComponent.generated.h"

class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;

namespace gpu { class Gpu; }

/**
 * PS1 GPU bridge: renders emulated GPU draw commands as real UE5 geometry.
 * VRAM is uploaded as a texture for the material to sample (texture pages, CLUTs).
 * Also provides an optional VRAM debug viewer (flat plane showing the full 1024x512 VRAM).
 * Place on the same Actor as UR3000EmuComponent.
 */
UCLASS(ClassGroup = (R3000Emu), meta = (BlueprintSpawnableComponent))
class UR3000GpuComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UR3000GpuComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Connect to the emulated GPU (called by R3000EmuComponent after core init). */
    void BindGpu(gpu::Gpu* InGpu);

    // ------- VRAM Texture access -------

    /** VRAM texture (1024x512 BGRA8) — the raw PS1 VRAM as an UE5 texture.
     *  Use this for material sampling or for a debug display widget. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU")
    UTexture2D* GetVramTexture() const { return VramTexture_; }

    // ------- Geometry mesh access -------

    /** The ProceduralMeshComponent that receives PS1 geometry each frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU")
    UProceduralMeshComponent* GetMeshComponent() const { return MeshComp_; }

    // ------- Display info -------

    /** PS1 display resolution from GP1 registers. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU")
    int32 GetDisplayWidth() const;
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU")
    int32 GetDisplayHeight() const;
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU")
    bool IsDisplayEnabled() const;

    /** Number of triangles in the last rendered frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU")
    int32 GetLastTriangleCount() const { return LastTriCount_; }

    /** Number of VRAM texture uploads since start. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU")
    int32 GetVramUploadCount() const { return VramUploadCount_; }

    // ------- Rendering settings -------

    /** UE units per PS1 pixel. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU", meta = (ClampMin = "0.01", ClampMax = "100.0"))
    float PixelScale{1.0f};

    /** Z-axis increment per draw command (separates primitives for painter's algorithm). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU", meta = (ClampMin = "0.0001", ClampMax = "1.0"))
    float ZStep{0.01f};

    /** Manual offset for PS1→UE5 coordinate mapping (add to vertex positions). Tune if image is shifted. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU")
    FVector2D DisplayOffset{FVector2D::ZeroVector};

    /** Center the PS1 display in UE5 space. Uses display rect center (GP1(05)+wh/2). Disable if using custom layout. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU")
    bool bCenterDisplay{true};

    /** When enabled, log transform params and vertex coords to UE Output Log (Verbose). Useful for debugging offset/exploding polygons. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU|Debug")
    bool bDebugMeshLog{false};

    /**
     * Base material for PS1 rendering.
     * Should be Unlit/Translucent with Disable Depth Test.
     * Enable "Two Sided" to avoid holes from backface culling (PS1 has no culling).
     *
     * Future: multiple materials per blend mode (opaque, translucent, additive).
     * UV3.y encodes semi-transparency: 0-3 = semi mode, 4 = opaque.
     * Can use these texture/scalar parameters:
     *   - "VramTexture" (Texture2D): the 1024x512 VRAM
     * Vertex data encodes per-triangle info:
     *   - Vertex Color: PS1 flat/gouraud color
     *   - UV0: texture coords within texture page (0-1)
     *   - UV1: texture page base in VRAM (normalized)
     *   - UV2: CLUT base in VRAM (normalized)
     *   - UV3.x: tex depth mode (0=none, 1=4bit, 2=8bit, 3=15bit)
     *   - UV3.y: semi-transparency (0-3=semi mode, 4=opaque)
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU")
    UMaterialInterface* BaseMaterial{nullptr};

    // ------- VRAM Debug Viewer -------

    /** Show a debug plane displaying the full 1024x512 VRAM content. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU|VramViewer")
    bool bShowVramViewer{false};

    /** Material for the VRAM viewer plane.
     *  Should be Unlit/Opaque with a "VramTexture" Texture2D parameter.
     *  If not set, a default unlit material is used. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU|VramViewer")
    UMaterialInterface* VramViewerMaterial{nullptr};

    /** Scale of the VRAM viewer plane in UE units. Default 1.0 = 1024x512 UE units. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU|VramViewer", meta = (ClampMin = "0.01", ClampMax = "10.0"))
    float VramViewerScale{0.5f};

    /** Offset of the VRAM viewer plane from the actor origin (local space). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU|VramViewer")
    FVector VramViewerOffset{FVector(600.0f, -256.0f, 0.0f)};

    /** Rotation of the VRAM viewer plane (local space). Default faces -X (toward a player looking +X). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU|VramViewer")
    FRotator VramViewerRotation{FRotator(0.0f, 0.0f, 0.0f)};

    /** Toggle the VRAM viewer at runtime. */
    UFUNCTION(BlueprintCallable, Category = "R3000Emu|GPU|VramViewer")
    void SetVramViewerVisible(bool bVisible);

private:
    void CreateVramTexture();
    void UpdateVramTexture();
    void RebuildMesh();
    void CreateOrUpdateVramViewer();
    void DestroyVramViewer();

    gpu::Gpu* Gpu_{nullptr};

    // Geometry rendering
    UPROPERTY()
    UProceduralMeshComponent* MeshComp_{nullptr};
    UPROPERTY()
    UMaterialInstanceDynamic* MatInst_{nullptr};

    // VRAM texture (shared between geometry material + VRAM viewer)
    UPROPERTY()
    UTexture2D* VramTexture_{nullptr};
    uint8* PixelBuffer_{nullptr};
    uint16* VramCopyBuffer_{nullptr};  // Thread-safe copy of GPU VRAM
    uint32 LastVramWriteSeq_{0xFFFFFFFFu};
    uint32 LastVramFrame_{0xFFFFFFFFu};
    int32 LastTriCount_{0};
    int32 VramUploadCount_{0};

    // VRAM debug viewer
    UPROPERTY()
    UProceduralMeshComponent* VramViewerMesh_{nullptr};
    UPROPERTY()
    UMaterialInstanceDynamic* VramViewerMatInst_{nullptr};
    bool bVramViewerCreated_{false};

    static constexpr int32 kVramW = 1024;
    static constexpr int32 kVramH = 512;
};
