#pragma once

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "PSX2DRenderComponent.generated.h"

class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class AActor;

namespace gpu { class Gpu; struct DisplayConfig; }

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
 * Place on the same Actor as UPSXEmulatorComponent.
 */
UCLASS(ClassGroup = (PSXEmu), meta = (BlueprintSpawnableComponent))
class UPSX2DRenderComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPSX2DRenderComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Connect to the emulated GPU (called by PSXEmulatorComponent after core init). */
    void BindGpu(gpu::Gpu* InGpu);

    // ------- VRAM Texture (received from VramViewerComponent) -------

    /** Receive the shared VRAM texture from the VramViewerComponent (avoids duplicate upload). */
    void SetVramTexture(UTexture2D* InTexture);

    /** VRAM texture (1024x512 BGRA8) — the raw PS1 VRAM as an UE5 texture. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU")
    UTexture2D* GetVramTexture() const { return VramTexture_; }

    // ------- Materials -------

    /** Force refresh of material instances from current MatSemi0-3 / BaseMaterial slots.
     *  Called automatically each frame, but you can call it manually after assigning materials in Blueprint. */
    UFUNCTION(BlueprintCallable, Category = "PSXEmu|GPU|Materials")
    void RefreshMaterials();

    // ------- Geometry mesh access -------

    /** The ProceduralMeshComponent that receives PS1 geometry each frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU")
    UProceduralMeshComponent* GetMeshComponent() const { return MeshComp_; }

    // ------- Display info -------

    /** PS1 display resolution from GP1 registers. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU")
    int32 GetDisplayWidth() const;
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU")
    int32 GetDisplayHeight() const;
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU")
    bool IsDisplayEnabled() const;

    /** Number of triangles in the last rendered frame (all sections). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU")
    int32 GetLastTriangleCount() const { return LastTriCount_; }

    /** Number of opaque triangles (section 0) in the last frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU|Stats")
    int32 GetOpaqueTriCount() const { return SectionTriCount_[0]; }

    /** Number of semi-transparent triangles (sections 1-4) in the last frame. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU|Stats")
    int32 GetSemiTransTriCount() const { return SectionTriCount_[1] + SectionTriCount_[2] + SectionTriCount_[3] + SectionTriCount_[4]; }

    /** Number of non-empty mesh sections in the last frame (0-5). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU|Stats")
    int32 GetMeshSectionCount() const { return LastSectionCount_; }

    /** GPU frames per second (based on draw list swaps, not VBlank). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU|Stats")
    float GetGpuFramesPerSecond() const { return GpuFps_; }

    /** Per-section triangle count (0=opaque, 1-4=semi modes 0-3). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU|Stats")
    int32 GetSectionTriCount(int32 Section) const { return (Section >= 0 && Section < kNumSections) ? SectionTriCount_[Section] : 0; }


    // ------- Rendering settings -------

    /** Enable/disable this 2D renderer at runtime. When disabled, mesh is hidden but VRAM texture
     *  continues to be updated (shared with 3D component). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU")
    bool bEnabled{true};

    /** Enable uniform HD scaling: output is always the same size regardless of PS1 resolution.
     *  When enabled, PixelScale is computed automatically based on HD definition.
     *  PS1 resolutions (256, 320, 512, 640) are all scaled to fill the target size. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU")
    bool bUniformHdScale{true};

    /** HD output resolution preset. Select a standard resolution or Custom for manual values. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU", meta = (EditCondition = "bUniformHdScale"))
    EHdDefinition HdDefinition{EHdDefinition::HD_1080p};

    /** Target output width in UE units (only used when HdDefinition is Custom). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU", meta = (ClampMin = "100.0", ClampMax = "8000.0", EditCondition = "bUniformHdScale && HdDefinition == EHdDefinition::Custom"))
    float TargetWidth{1920.0f};

    /** Target output height in UE units (only used when HdDefinition is Custom). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU", meta = (ClampMin = "100.0", ClampMax = "8000.0", EditCondition = "bUniformHdScale && HdDefinition == EHdDefinition::Custom"))
    float TargetHeight{1080.0f};

    /** Manual UE units per PS1 pixel (used only when bUniformHdScale is disabled). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU", meta = (ClampMin = "0.01", ClampMax = "100.0", EditCondition = "!bUniformHdScale"))
    float PixelScale{1.0f};

    /** Z-axis increment per draw command (separates primitives for painter's algorithm). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU", meta = (ClampMin = "0.0001", ClampMax = "1.0"))
    float ZStep{0.01f};

    /** Manual offset for PS1→UE5 coordinate mapping (add to vertex positions). Tune if image is shifted. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU")
    FVector2D DisplayOffset{FVector2D::ZeroVector};

    /** Get the effective pixel scale (computed from target size if bUniformHdScale, else manual PixelScale). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|GPU")
    float GetEffectivePixelScale() const;

    /** Skip GP0(02h) fill rect commands (screen clears). Enable to prevent background fills
     *  from hiding 3D geometry in VR mode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU")
    bool bSkipFillRect{false};

    /** When enabled, log transform params and vertex coords to UE Output Log (Verbose). Useful for debugging offset/exploding polygons. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU|Debug")
    bool bDebugMeshLog{false};

    /** Show a UE-world camera actor representing how the PSX 2D plane is being viewed. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU|Debug")
    bool bShowPsxCameraDebug{false};

    /** Optional existing actor to reuse as the 2D debug camera. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU|Debug")
    TSoftObjectPtr<AActor> PsxCameraDebugActor;

    /** Auto-spawn a debug actor when no explicit actor is assigned. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU|Debug")
    bool bAutoSpawnPsxCameraDebugActor{true};

    /** Draw the logical screen frame in UE world space for camera/alignment debugging. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU|Debug")
    bool bShowPsxScreenFrameDebug{false};

    /**
     * PS1 GPU materials — 5 slots for opaque + 4 semi-transparency blend modes.
     * All materials share the same HLSL/vertex data layout (see UV docs below).
     * Only the UE5 Blend Mode differs between them.
     *
     * ============================================================
     * MESH SECTIONS:
     * ============================================================
     *   Section 0: Opaque/Masked — non-semi-transparent primitives
     *   Section 1: Semi mode 0 — B/2 + F/2 (50% blend)
     *   Section 2: Semi mode 1 — B + F     (additive)
     *   Section 3: Semi mode 2 — B - F     (subtractive)
     *   Section 4: Semi mode 3 — B + F/4   (25% additive)
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

    /** Base material for opaque/masked primitives (section 0). Also used as fallback for semi-transparent slots left empty. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU")
    UMaterialInterface* BaseMaterial{nullptr};

    /** Material for semi-transparency mode 0: B/2 + F/2. UE5 Blend Mode: Translucent, Opacity=0.5. Falls back to BaseMaterial if null. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU|Materials")
    UMaterialInterface* MatSemi0{nullptr};

    /** Material for semi-transparency mode 1: B + F (additive). UE5 Blend Mode: Additive. Falls back to BaseMaterial if null. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU|Materials")
    UMaterialInterface* MatSemi1{nullptr};

    /** Material for semi-transparency mode 2: B - F (subtractive). UE5 Blend Mode: Modulate (approx). Falls back to BaseMaterial if null. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU|Materials")
    UMaterialInterface* MatSemi2{nullptr};

    /** Material for semi-transparency mode 3: B + F/4 (25% additive). UE5 Blend Mode: Additive, color*0.25. Falls back to BaseMaterial if null. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|GPU|Materials")
    UMaterialInterface* MatSemi3{nullptr};


private:
    void RebuildMesh();
    void EnsureMaterialInstances();
    void UpdatePsxCameraDebugActor();
    void DrawPsxScreenFrameDebug() const;
    AActor* ResolvePsxCameraDebugActor();
    void GetFixedScreenSize(float& OutW, float& OutH) const;

    gpu::Gpu* Gpu_{nullptr};

    // Geometry rendering
    static constexpr int32 kNumSections = 5; // 0=opaque, 1-4=semi modes 0-3
    UPROPERTY()
    UProceduralMeshComponent* MeshComp_{nullptr};
    UPROPERTY()
    TArray<UMaterialInstanceDynamic*> MatInst_;
    // Track which source materials were used to create MatInst_, so we detect changes
    UPROPERTY()
    TArray<UMaterialInterface*> MatInstSource_;

    // VRAM texture (received from VramViewerComponent)
    UPROPERTY()
    UTexture2D* VramTexture_{nullptr};

    uint32 LastVramFrame_{0xFFFFFFFFu};
    int32 LastTriCount_{0};
    int32 EmptyFrameCount_{0};
    int32 SectionTriCount_[kNumSections]{0};
    int32 LastSectionCount_{0};
    float GpuFps_{0.0f};
    double LastGpuFpsTime_{0.0};
    uint32 LastGpuFpsFrame_{0};
    UPROPERTY(Transient)
    AActor* SpawnedPsxCameraDebugActor_{nullptr};
};
