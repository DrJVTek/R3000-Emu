#pragma once

#include "GameFramework/Actor.h"
#include "PSXCameraDebugActor.generated.h"

class UArrowComponent;
class UDrawFrustumComponent;
class USceneComponent;
class UTextRenderComponent;

UCLASS()
class APSXCameraDebugActor : public AActor
{
    GENERATED_BODY()

  public:
    APSXCameraDebugActor();

    UFUNCTION(BlueprintCallable, Category = "PSXEmu|Debug")
    void SetDebugColor(const FColor& InColor);

    UFUNCTION(BlueprintCallable, Category = "PSXEmu|Debug")
    void SetDebugText(const FString& InText);

  private:
    UPROPERTY()
    USceneComponent* Root_{nullptr};

    UPROPERTY()
    UArrowComponent* Arrow_{nullptr};

    UPROPERTY()
    UDrawFrustumComponent* Frustum_{nullptr};

    UPROPERTY()
    UTextRenderComponent* Text_{nullptr};
};
