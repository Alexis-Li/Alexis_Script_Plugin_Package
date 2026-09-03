#pragma once

#include "Animation/AnimInstance.h"

#include "MtoULiveLinkTestAnimInstance.generated.h"

UCLASS()
class UMtoULiveLinkConflictingPostProcess : public UAnimInstance
{
    GENERATED_BODY()

public:
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
};
