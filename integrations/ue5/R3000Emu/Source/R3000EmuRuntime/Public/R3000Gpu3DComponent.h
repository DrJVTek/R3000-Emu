#pragma once

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "R3000Gpu3DComponent.generated.h"

class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;

namespace gpu { class Gpu; class Gpu3D; }

/**
 * PS1 GPU 3D reconstruction renderer.
 * Reads 3D data from the shadow GPU (Gpu3D) draw list which uses differential
 * tag encoding for GTE-GPU correlation. Renders original 3D geometry in UE5 world space.
 * Place on the same Actor as UR3000EmuComponent + UR3000GpuComponent.
 */
UCLASS(ClassGroup = (R3000Emu), meta = (BlueprintSpawnableComponent))
class UR3000Gpu3DComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UR3000Gpu3DComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Connect to the emulated GPU (called by R3000EmuComponent after core init). */
    void BindGpu(gpu::Gpu* InGpu);

    /** Connect to the shadow GPU for 3D reconstruction (differential tags). */
    void BindGpu3D(gpu::Gpu3D* InGpu3D);

    /** Receive the shared VRAM texture from the 2D GPU component (avoids duplicate upload). */
    void SetVramTexture(UTexture2D* InTexture);

    /** The ProceduralMeshComponent that receives 3D geometry each frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU3D")
    UProceduralMeshComponent* GetMeshComponent() const { return MeshComp_; }

    /** Number of 3D triangles rendered in the last frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU3D|Stats")
    int32 GetLast3DTriCount() const { return Last3DTriCount_; }

    /** Number of 2D (skipped) primitives in the last frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU3D|Stats")
    int32 GetLast2DSkipCount() const { return Last2DSkipCount_; }

    // ------- Rendering settings -------

    /** Enable/disable this 3D renderer at runtime. OFF by default (2D is primary). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D")
    bool bEnabled{false};

    /** Scale factor for 3D geometry: PS1 GTE camera-space units to UE5 units.
     *  GTE vertices are typically in the +-2000 range. 0.1 maps that to +-200 UE units. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D", meta = (ClampMin = "0.001", ClampMax = "10.0"))
    float WorldScale{0.1f};

    /** Scale factor for 2D elements (HUD/UI) rendered in 3D space.
     *  PS1 screen coords are typically 0..320 × 0..240. 1.0 maps 1 PS1 pixel = 1 UE unit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D", meta = (ClampMin = "0.001", ClampMax = "10.0"))
    float WorldScale2D{1.0f};

    /** Offset for 3D geometry in UE world space (local to actor). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D")
    FVector WorldOffset{FVector::ZeroVector};

    /** Skip 2D elements (HUD/UI) — only render GTE-correlated 3D geometry. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D")
    bool bSkip2DElements{true};

    /** Automatically scale and position 2D elements to match the 3D scene extent.
     *  When enabled, 2D scale and depth range are derived from the 3D bounding box.
     *  When disabled, uses WorldScale2D / Depth2DBack / Depth2DFront manually. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D")
    bool bAutoScale2D{true};

    /** Depth for the farthest 2D element (first in OT = background). Only used when bAutoScale2D is off. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D", meta = (EditCondition = "!bAutoScale2D"))
    float Depth2DBack{-50.0f};

    /** Depth for the nearest 2D element (last in OT = HUD/foreground). Only used when bAutoScale2D is off. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D", meta = (EditCondition = "!bAutoScale2D"))
    float Depth2DFront{50.0f};

    /** Debug: log 3D correlation stats per frame. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Debug")
    bool bDebug3DLog{false};

    // ------- 2D Materials (sections 0-4: HUD/UI rendered in 3D space) -------

    /** 2D opaque material (section 0). Must have "VramTexture" parameter. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 2D")
    UMaterialInterface* Mat2D_Opaque{nullptr};

    /** 2D semi-transparency mode 0: B/2 + F/2 (section 1). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 2D")
    UMaterialInterface* Mat2D_Semi0{nullptr};

    /** 2D semi-transparency mode 1: B + F additive (section 2). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 2D")
    UMaterialInterface* Mat2D_Semi1{nullptr};

    /** 2D semi-transparency mode 2: B - F subtractive (section 3). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 2D")
    UMaterialInterface* Mat2D_Semi2{nullptr};

    /** 2D semi-transparency mode 3: B + F/4 25% additive (section 4). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 2D")
    UMaterialInterface* Mat2D_Semi3{nullptr};

    // ------- 3D Materials (sections 5-9: GTE-correlated 3D geometry) -------

    /** 3D opaque material (section 5). Must have "VramTexture" parameter. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 3D")
    UMaterialInterface* Mat3D_Opaque{nullptr};

    /** 3D semi-transparency mode 0: B/2 + F/2 (section 6). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 3D")
    UMaterialInterface* Mat3D_Semi0{nullptr};

    /** 3D semi-transparency mode 1: B + F additive (section 7). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 3D")
    UMaterialInterface* Mat3D_Semi1{nullptr};

    /** 3D semi-transparency mode 2: B - F subtractive (section 8). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 3D")
    UMaterialInterface* Mat3D_Semi2{nullptr};

    /** 3D semi-transparency mode 3: B + F/4 25% additive (section 9). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials 3D")
    UMaterialInterface* Mat3D_Semi3{nullptr};

private:
    void RebuildMesh3D();
    void EnsureMaterialInstances();

    gpu::Gpu* Gpu_{nullptr};
    gpu::Gpu3D* Gpu3D_{nullptr}; // Shadow GPU (preferred source for 3D data)

    static constexpr int32 kNumSections = 10; // 0-4=2D (opaque+semi), 5-9=3D (opaque+semi)

    UPROPERTY()
    UProceduralMeshComponent* MeshComp_{nullptr};

    UPROPERTY()
    UTexture2D* VramTexture_{nullptr};

    UPROPERTY()
    TArray<UMaterialInstanceDynamic*> MatInst_;

    UPROPERTY()
    TArray<UMaterialInterface*> MatInstSource_;

    uint32 LastVramFrame_{0xFFFFFFFFu};
    int32 Last3DTriCount_{0};
    int32 Last2DSkipCount_{0};
    int32 LastNumSections_{0};

    // Previous frame's 3D bounding box (for auto-scaling 2D)
    float Last3DMinX_{0.0f};  // Min depth (UE X)
    float Last3DMaxX_{0.0f};  // Max depth (UE X)
    float Last3DExtentY_{0.0f}; // Horizontal extent (UE Y)
};
