#include "MtoULiveLinkBinding.h"

#if WITH_EDITOR

#include "MtoULiveLinkActor.h"

#include "UObject/UObjectIterator.h"

void UMtoULiveLinkBinding::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName PropertyName = PropertyChangedEvent.GetPropertyName();
    if (PropertyName != GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, SkeletalMesh)
        && PropertyName != GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, PreviewStaticMesh))
    {
        return;
    }

    for (TObjectIterator<AMtoULiveLinkActor> It; It; ++It)
    {
        if (!It->HasAnyFlags(RF_ClassDefaultObject) && It->GetBinding() == this)
        {
            It->NotifyBindingInputsChanged();
        }
    }
}

#endif
