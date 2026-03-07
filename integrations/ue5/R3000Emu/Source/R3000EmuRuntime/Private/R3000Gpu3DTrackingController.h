#pragma once

#include "CoreMinimal.h"
#include "R3000Gpu3DComponent.h"

class UProceduralMeshComponent;
class UR3000Gpu3DComponent;

class FR3000Gpu3DTrackingController
{
public:
    explicit FR3000Gpu3DTrackingController(UR3000Gpu3DComponent& InOwner);

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

    UR3000Gpu3DComponent& Owner_;
    EGpu3DTrackingMode LastMode_{EGpu3DTrackingMode::LegacyFollowOwner};
};
