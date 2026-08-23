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

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
