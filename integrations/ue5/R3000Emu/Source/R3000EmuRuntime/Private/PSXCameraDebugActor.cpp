#include "PSXCameraDebugActor.h"

#include "Components/ArrowComponent.h"
#include "Components/DrawFrustumComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"

APSXCameraDebugActor::APSXCameraDebugActor()
{
    PrimaryActorTick.bCanEverTick = false;

    Root_ = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root_);

    Arrow_ = CreateDefaultSubobject<UArrowComponent>(TEXT("Arrow"));
    Arrow_->SetupAttachment(Root_);
    Arrow_->ArrowSize = 1.5f;
    Arrow_->ArrowLength = 80.0f;
    Arrow_->SetHiddenInGame(false);

    Frustum_ = CreateDefaultSubobject<UDrawFrustumComponent>(TEXT("Frustum"));
    Frustum_->SetupAttachment(Root_);
    Frustum_->FrustumAngle = 60.0f;
    Frustum_->FrustumAspectRatio = 4.0f / 3.0f;
    Frustum_->FrustumStartDist = 10.0f;
    Frustum_->FrustumEndDist = 120.0f;
    Frustum_->SetHiddenInGame(false);

    Text_ = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Text"));
    Text_->SetupAttachment(Root_);
    Text_->SetRelativeLocation(FVector(0.0f, 0.0f, 40.0f));
    Text_->SetHorizontalAlignment(EHorizTextAligment::EHTA_Left);
    Text_->SetWorldSize(20.0f);
    Text_->SetHiddenInGame(false);
    Text_->SetText(FText::FromString(TEXT("PSX Cam")));

    SetActorEnableCollision(false);
    SetActorHiddenInGame(false);
    SetReplicates(false);
    SetDebugColor(FColor(255, 170, 0));
}

void APSXCameraDebugActor::SetDebugColor(const FColor& InColor)
{
    if (Arrow_)
        Arrow_->ArrowColor = InColor;
    if (Frustum_)
        Frustum_->FrustumColor = InColor;
    if (Text_)
        Text_->SetTextRenderColor(InColor);
}

void APSXCameraDebugActor::SetDebugText(const FString& InText)
{
    if (Text_)
        Text_->SetText(FText::FromString(InText));
}
