#include "R3000Gpu3DTrackingController.h"

#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "ProceduralMeshComponent.h"

FR3000Gpu3DTrackingController::FR3000Gpu3DTrackingController(UR3000Gpu3DComponent& InOwner)
    : Owner_(InOwner)
{
}

void FR3000Gpu3DTrackingController::Update(
    UProceduralMeshComponent* MeshComp,
    EGpu3DTrackingMode TrackingMode,
    bool bAutoRecenterVrOnModeEnter,
    bool bTrackVrViewEveryTick,
    const FVector& VrViewOffset,
    bool& bMeshDetachedForWorldLock)
{
    if (!MeshComp)
        return;

    const bool bModeChanged = TrackingMode != LastMode_;

    if (TrackingMode == EGpu3DTrackingMode::LegacyFollowOwner)
    {
        EnsureAttached(MeshComp, bMeshDetachedForWorldLock);
    }
    else
    {
        EnsureDetached(MeshComp, bMeshDetachedForWorldLock);
    }

    if (TrackingMode == EGpu3DTrackingMode::VRRecenterable)
    {
        if ((bModeChanged && bAutoRecenterVrOnModeEnter) || bTrackVrViewEveryTick)
            RecenterToPlayerView(MeshComp, VrViewOffset, bMeshDetachedForWorldLock);
    }

    LastMode_ = TrackingMode;
}

void FR3000Gpu3DTrackingController::RecenterToPlayerView(
    UProceduralMeshComponent* MeshComp,
    const FVector& VrViewOffset,
    bool& bMeshDetachedForWorldLock)
{
    if (!MeshComp)
        return;

    EnsureDetached(MeshComp, bMeshDetachedForWorldLock);

    UWorld* World = Owner_.GetWorld();
    if (!World)
        return;

    APlayerController* PC = World->GetFirstPlayerController();
    if (!PC || !PC->PlayerCameraManager)
        return;

    const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
    const FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();
    const FVector WorldLoc = CamLoc + CamRot.RotateVector(VrViewOffset);

    FTransform MeshWorld = MeshComp->GetComponentTransform();
    MeshWorld.SetLocation(WorldLoc);
    MeshWorld.SetRotation(CamRot.Quaternion());
    MeshComp->SetWorldTransform(MeshWorld);
}

void FR3000Gpu3DTrackingController::EnsureDetached(
    UProceduralMeshComponent* MeshComp,
    bool& bMeshDetachedForWorldLock) const
{
    if (!bMeshDetachedForWorldLock)
    {
        MeshComp->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
        bMeshDetachedForWorldLock = true;
    }
}

void FR3000Gpu3DTrackingController::EnsureAttached(
    UProceduralMeshComponent* MeshComp,
    bool& bMeshDetachedForWorldLock) const
{
    if (bMeshDetachedForWorldLock)
    {
        MeshComp->AttachToComponent(&Owner_, FAttachmentTransformRules::KeepWorldTransform);
        bMeshDetachedForWorldLock = false;
    }
}
