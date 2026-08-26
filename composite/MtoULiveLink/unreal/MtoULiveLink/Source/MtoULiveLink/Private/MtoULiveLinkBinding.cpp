#include "MtoULiveLinkBinding.h"

#if WITH_EDITOR

#include "MtoULiveLinkActor.h"

#include "UObject/UObjectIterator.h"

void UMtoULiveLinkBinding::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    // Array-element edits report the inner property, so fall back to it when
    // no member property is recorded.
    const FName ChangedName = PropertyChangedEvent.MemberProperty
        ? PropertyChangedEvent.MemberProperty->GetFName()
        : PropertyChangedEvent.GetPropertyName();
    if (ChangedName != GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, SkeletalMesh)
        && ChangedName != GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, PreviewStaticMesh)
        && ChangedName != GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, DriverGarmentSlotOverride))
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
