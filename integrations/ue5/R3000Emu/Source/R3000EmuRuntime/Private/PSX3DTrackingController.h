#pragma once

#include "CoreMinimal.h"
#include "PSX3DRenderComponent.h"

class UProceduralMeshComponent;
class UPSX3DRenderComponent;

class FPSX3DTrackingController
{
public:
    explicit FPSX3DTrackingController(UPSX3DRenderComponent& InOwner);

    void Update(
        UProceduralMeshComponent* MeshComp,
        EGpu3DTrackingMode TrackingMode,
        bool bAutoRecenterVrOnModeEnter,
        bool bTrackVrViewEveryTick,
        const FVector& VrViewOffset,
        bool& bMeshDetachedForWorldLock);

    void RecenterToPlayerView(
        UProceduralMeshComponent* MeshComp,
        const FVector& VrViewOffset,
        bool& bMeshDetachedForWorldLock);

private:
    void EnsureDetached(UProceduralMeshComponent* MeshComp, bool& bMeshDetachedForWorldLock) const;
    void EnsureAttached(UProceduralMeshComponent* MeshComp, bool& bMeshDetachedForWorldLock) const;

    UPSX3DRenderComponent& Owner_;
    EGpu3DTrackingMode LastMode_{EGpu3DTrackingMode::LegacyFollowOwner};
};
