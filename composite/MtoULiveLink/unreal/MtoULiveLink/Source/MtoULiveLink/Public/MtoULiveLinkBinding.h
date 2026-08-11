#pragma once

#include "Engine/DataAsset.h"

#include "MtoULiveLinkBinding.generated.h"

class USkeletalMesh;

UCLASS(BlueprintType, meta = (DisplayName = "MtoU_LiveLink Binding"))
class MTOULIVELINK_API UMtoULiveLinkBinding : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<USkeletalMesh> SkeletalMesh;
};
