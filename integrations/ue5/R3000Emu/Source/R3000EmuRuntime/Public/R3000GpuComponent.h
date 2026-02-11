#pragma once

#include "Components/ActorComponent.h"
#include "ProceduralMeshComponent.h"
#include "R3000GpuComponent.generated.h"

class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;

namespace gpu { class Gpu; }

/** HD output resolution presets for uniform scaling. */
UENUM(BlueprintType)
enum class EHdDefinition : uint8
{
    /** 1280x720 (HD 720p) */
    HD_720p     UMETA(DisplayName = "720p (1280x720)"),
    /** 1920x1080 (Full HD 1080p) - Default */
    HD_1080p    UMETA(DisplayName = "1080p (1920x1080)"),
    /** 2560x1440 (QHD 1440p) */
    HD_1440p    UMETA(DisplayName = "1440p (2560x1440)"),
    /** 3840x2160 (4K UHD) */
    HD_4K       UMETA(DisplayName = "4K (3840x2160)"),
    /** Custom: use TargetWidth/TargetHeight manually */
    Custom      UMETA(DisplayName = "Custom")
};

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

    /** Enable uniform HD scaling: output is always the same size regardless of PS1 resolution.
     *  When enabled, PixelScale is computed automatically based on HD definition.
     *  PS1 resolutions (256, 320, 512, 640) are all scaled to fill the target size. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU")
    bool bUniformHdScale{true};

    /** HD output resolution preset. Select a standard resolution or Custom for manual values. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU", meta = (EditCondition = "bUniformHdScale"))
    EHdDefinition HdDefinition{EHdDefinition::HD_1080p};

    /** Target output width in UE units (only used when HdDefinition is Custom). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU", meta = (ClampMin = "100.0", ClampMax = "8000.0", EditCondition = "bUniformHdScale && HdDefinition == EHdDefinition::Custom"))
    float TargetWidth{1920.0f};

    /** Target output height in UE units (only used when HdDefinition is Custom). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU", meta = (ClampMin = "100.0", ClampMax = "8000.0", EditCondition = "bUniformHdScale && HdDefinition == EHdDefinition::Custom"))
    float TargetHeight{1080.0f};

    /** Manual UE units per PS1 pixel (used only when bUniformHdScale is disabled). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU", meta = (ClampMin = "0.01", ClampMax = "100.0", EditCondition = "!bUniformHdScale"))
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

    /** Get the effective pixel scale (computed from target size if bUniformHdScale, else manual PixelScale). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU")
    float GetEffectivePixelScale() const;

    /** When enabled, log transform params and vertex coords to UE Output Log (Verbose). Useful for debugging offset/exploding polygons. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|GPU|Debug")
    bool bDebugMeshLog{false};

    /**
     * Base material for PS1 rendering.
     * Should be Unlit/Translucent with Disable Depth Test.
     * Enable "Two Sided" to avoid holes from backface culling (PS1 has no culling).
     *
     * ============================================================
     * MATERIAL TEXTURE PARAMETERS:
     * ============================================================
     *   - "VramTexture" (Texture2D): PS1 VRAM as 1024x512 BGRA8
     *
     * ============================================================
     * VERTEX DATA (per-vertex attributes):
     * ============================================================
     *   - Vertex Color RGB: PS1 flat/gouraud shading color
     *   - Vertex Color A: Reserved (1.0)
     *
     *   - UV0 (x, y): Texture coords in PIXELS (0-255) within texture page
     *       u = horizontal texel, v = vertical texel
     *
     *   - UV1 (x, y): Texture page base in VRAM PIXELS
     *       x = tpBaseX: 0, 64, 128, 192, 256, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960
     *       y = tpBaseY: 0 or 256
     *
     *   - UV2 (x, y): CLUT (palette) position in VRAM PIXELS
     *       x = clutX: 0, 16, 32, ... 1008 (multiples of 16)
     *       y = clutY: 0 - 511
     *
     *   - UV3.x: Texture depth mode
     *       0 = No texture (flat/gouraud color only)
     *       1 = 4-bit indexed (16 colors, CLUT lookup)
     *       2 = 8-bit indexed (256 colors, CLUT lookup)
     *       3 = 15-bit direct color (no CLUT)
     *
     *   - UV3.y: Packed flags (decode as int)
     *       bits 0-1: Semi-transparency mode (0-3)
     *       bit 2: Is semi-transparent (1=yes, 0=no)
     *       bit 3: Is raw texture (1=no color modulation, 0=multiply by vertex color)
     *
     * ============================================================
     * TEXTURE SAMPLING IN MATERIAL:
     * ============================================================
     * VRAM Layout: 1024x512 pixels, 16-bit per pixel
     *
     * For 4-bit textures (UV3.x == 1):
     *   - 4 texels packed per 16-bit VRAM word
     *   - VRAM X = tpBaseX + floor(u / 4)
     *   - Index = (vram_pixel >> ((u % 4) * 4)) & 0xF
     *   - Color = CLUT[clutY][clutX + index]
     *
     * For 8-bit textures (UV3.x == 2):
     *   - 2 texels packed per 16-bit VRAM word
     *   - VRAM X = tpBaseX + floor(u / 2)
     *   - Index = (u % 2 == 0) ? (vram_pixel & 0xFF) : (vram_pixel >> 8)
     *   - Color = CLUT[clutY][clutX + index]
     *
     * For 15-bit textures (UV3.x == 3):
     *   - Direct color, 1 texel per VRAM word
     *   - VRAM X = tpBaseX + u
     *   - Color = RGB555 to RGB888
     *
     * Semi-transparency modes (when bit2 of UV3.y is set):
     *   0: 0.5*Back + 0.5*Front
     *   1: 1.0*Back + 1.0*Front
     *   2: 1.0*Back - 1.0*Front
     *   3: 1.0*Back + 0.25*Front
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

    // ------- PS1 VRAM Constants (for Material/Blueprint use) -------

    /** PS1 VRAM width in pixels (1024). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU|Constants")
    static int32 GetVramWidth() { return 1024; }

    /** PS1 VRAM height in pixels (512). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU|Constants")
    static int32 GetVramHeight() { return 512; }

    /** Texture page width in texels (256 for all depths, but VRAM footprint varies). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU|Constants")
    static int32 GetTexturePageWidth() { return 256; }

    /** Texture page height in texels (256). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU|Constants")
    static int32 GetTexturePageHeight() { return 256; }

    /** Get VRAM X scale factor for a texture depth mode.
     *  4-bit=0.25 (64 VRAM pixels for 256 texels), 8-bit=0.5, 15-bit=1.0 */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU|Constants")
    static float GetVramScaleForDepth(int32 TexDepthMode)
    {
        switch (TexDepthMode)
        {
            case 1: return 0.25f;  // 4-bit: 4 texels per 16-bit word
            case 2: return 0.5f;   // 8-bit: 2 texels per 16-bit word
            case 3: return 1.0f;   // 15-bit: 1 texel per 16-bit word
            default: return 1.0f;
        }
    }

    /** Decode semi-transparency mode from UV3.y flags. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU|Constants")
    static int32 DecodeSemiMode(float UV3Y) { return static_cast<int32>(UV3Y) & 0x3; }

    /** Check if semi-transparent from UV3.y flags. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU|Constants")
    static bool IsSemiTransparent(float UV3Y) { return (static_cast<int32>(UV3Y) & 0x4) != 0; }

    /** Check if raw texture (no color modulation) from UV3.y flags. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu|GPU|Constants")
    static bool IsRawTexture(float UV3Y) { return (static_cast<int32>(UV3Y) & 0x8) != 0; }

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
