#pragma once

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Texture2D.h"
#include "PSXVramViewerComponent.generated.h"

class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;

namespace gpu { class Gpu; }

/**
 * PS1 VRAM texture manager + debug viewer.
 * Owns the 1024x512 VRAM texture (thread-safe upload from emulator GPU).
 * Other components (2D, 3D) receive the texture via GetVramTexture().
 * Optionally displays a debug quad showing the full VRAM content.
 */
UCLASS(ClassGroup = (PSXEmu), meta = (BlueprintSpawnableComponent))
class UPSXVramViewerComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPSXVramViewerComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Bind to the primary GPU for VRAM access. Creates the texture. */
    void BindGpu(gpu::Gpu* InGpu);

    /** Get the shared VRAM texture (1024x512 BGRA8). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Vram")
    UTexture2D* GetVramTexture() const { return VramTexture_; }

    /** Toggle the VRAM viewer at runtime. */
    UFUNCTION(BlueprintCallable, Category = "PSXEmu|Vram|Viewer")
    void SetViewerVisible(bool bNewVisible);

    // ------- Viewer settings -------

    /** Show a debug plane displaying the full 1024x512 VRAM content. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Vram|Viewer")
    bool bShowViewer{false};

    /** Material for the VRAM viewer plane. Should be Unlit/Opaque with a "VramTexture" parameter. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Vram|Viewer")
    UMaterialInterface* ViewerMaterial{nullptr};

    /** Size of the VRAM viewer quad (1 UE unit = 1 VRAM pixel at scale 1.0).
     *  Use the component's transform (Scale) to resize in the viewport. */

    // ------- PS1 VRAM Constants (for Material/Blueprint use) -------

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Vram|Constants")
    static int32 GetVramWidth() { return 1024; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Vram|Constants")
    static int32 GetVramHeight() { return 512; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Vram|Constants")
    static int32 GetTexturePageWidth() { return 256; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Vram|Constants")
    static int32 GetTexturePageHeight() { return 256; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Vram|Constants")
    static float GetVramScaleForDepth(int32 TexDepthMode)
    {
        switch (TexDepthMode)
        {
            case 1: return 0.25f;  // 4-bit
            case 2: return 0.5f;   // 8-bit
            case 3: return 1.0f;   // 15-bit
            default: return 1.0f;
        }
    }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Vram|Constants")
    static int32 DecodeSemiMode(float UV3Y) { return static_cast<int32>(UV3Y) & 0x3; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Vram|Constants")
    static bool IsSemiTransparent(float UV3Y) { return (static_cast<int32>(UV3Y) & 0x4) != 0; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|Vram|Constants")
    static bool IsRawTexture(float UV3Y) { return (static_cast<int32>(UV3Y) & 0x8) != 0; }

private:
    void CreateVramTexture();
    void UpdateVramTexture();
    void CreateOrUpdateViewer();
    void DestroyViewer();

    gpu::Gpu* Gpu_{nullptr};

    // VRAM texture (shared with 2D + 3D components)
    UPROPERTY()
    UTexture2D* VramTexture_{nullptr};
    uint8* PixelBuffer_{nullptr};
    uint16* VramCopyBuffer_{nullptr};
    FUpdateTextureRegion2D* UpdateRegion_{nullptr};
    uint32 LastVramWriteSeq_{0xFFFFFFFFu};
    int32 VramUploadCount_{0};

    // Debug viewer
    UPROPERTY()
    UProceduralMeshComponent* ViewerMesh_{nullptr};
    UPROPERTY()
    UMaterialInstanceDynamic* ViewerMatInst_{nullptr};
    bool bViewerCreated_{false};

    static constexpr int32 kVramW = 1024;
    static constexpr int32 kVramH = 512;
};
