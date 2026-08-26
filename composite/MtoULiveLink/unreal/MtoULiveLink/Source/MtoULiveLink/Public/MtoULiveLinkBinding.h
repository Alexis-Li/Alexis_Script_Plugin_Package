#pragma once

#include "Engine/DataAsset.h"

#include "MtoULiveLinkBinding.generated.h"

class USkeletalMesh;
class UStaticMesh;

UCLASS(BlueprintType, meta = (DisplayName = "MtoU_LiveLink Binding"))
class MTOULIVELINK_API UMtoULiveLinkBinding : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Category = "MtoU_LiveLink", meta = (DisplayName = "Driver Skeletal Mesh"))
    TObjectPtr<USkeletalMesh> SkeletalMesh;

    UPROPERTY(EditAnywhere, Category = "MtoU_LiveLink", meta = (DisplayName = "Preview Static Mesh"))
    TObjectPtr<UStaticMesh> PreviewStaticMesh;

    /**
     * Optional advanced manual override of the Driver garment source. Lists
     * stable imported Driver material-slot names; an empty list keeps automatic
     * garment resolution. Names must stay unique and present on the current
     * Driver import or Refresh fails before any build.
     */
    UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "MtoU_LiveLink",
        meta = (DisplayName = "Driver Garment Slot Override",
            Tooltip = "Manual selection of Driver source material slots for garment resolution. Leave empty for automatic resolution from geometry. Entries are stable imported Driver slot names (never numeric section indices) and every name must exist exactly once on the current Driver import. Selected regions still pass the same geometry coverage and alignment validation as automatic results."))
    TArray<FName> DriverGarmentSlotOverride;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
