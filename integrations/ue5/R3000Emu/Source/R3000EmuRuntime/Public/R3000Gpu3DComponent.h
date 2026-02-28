#pragma once

#include "Components/ActorComponent.h"
#include "ProceduralMeshComponent.h"
#include "R3000Gpu3DComponent.generated.h"

class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;

namespace gpu { class Gpu; }

/**
 * PS1 GPU 3D reconstruction renderer.
 * Reads GTE-correlated 3D data (cmds_3d) from the same GPU draw list used
 * by the 2D component and renders original 3D geometry in UE5 world space.
 * Place on the same Actor as UR3000EmuComponent + UR3000GpuComponent.
 */
UCLASS(ClassGroup = (R3000Emu), meta = (BlueprintSpawnableComponent))
class UR3000Gpu3DComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UR3000Gpu3DComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Connect to the emulated GPU (called by R3000EmuComponent after core init). */
    void BindGpu(gpu::Gpu* InGpu);

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

    /** Scale factor: PS1 GTE units to UE5 units.
     *  GTE vertices are typically in the +-2000 range. 0.1 maps that to +-200 UE units. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D", meta = (ClampMin = "0.001", ClampMax = "10.0"))
    float WorldScale{0.1f};

    /** Offset for 3D geometry in UE world space (local to actor). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D")
    FVector WorldOffset{FVector::ZeroVector};

    /** Skip 2D elements (HUD/UI) — only render GTE-correlated 3D geometry. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D")
    bool bSkip2DElements{true};

    /** Debug: log 3D correlation stats per frame. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Debug")
    bool bDebug3DLog{false};

    // ------- Materials (same 5-slot system as 2D) -------

    /** Base material for opaque/masked 3D primitives. Must have "VramTexture" parameter. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D")
    UMaterialInterface* BaseMaterial{nullptr};

    /** Material for semi-transparency mode 0: B/2 + F/2. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials")
    UMaterialInterface* MatSemi0{nullptr};

    /** Material for semi-transparency mode 1: B + F (additive). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials")
    UMaterialInterface* MatSemi1{nullptr};

    /** Material for semi-transparency mode 2: B - F (subtractive). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials")
    UMaterialInterface* MatSemi2{nullptr};

    /** Material for semi-transparency mode 3: B + F/4 (25% additive). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU3D|Materials")
    UMaterialInterface* MatSemi3{nullptr};

private:
    void RebuildMesh3D();
    void EnsureMaterialInstances();

    gpu::Gpu* Gpu_{nullptr};

    static constexpr int32 kNumSections = 5; // 0=opaque, 1-4=semi modes 0-3

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
};
