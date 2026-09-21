#include "MtoULiveLinkBinding.h"

#if WITH_EDITOR

#include "MtoULiveLinkActor.h"

#include "UObject/UObjectIterator.h"

namespace
{
/**
 * True when the property belongs to one Additional Part entry, which is how an
 * edit inside a list element reports itself.
 */
bool IsCharacterPartProperty(const FProperty* Property)
{
    return Property && Property->GetOwnerStruct() == FMtoUCharacterPart::StaticStruct();
}

/** Runs Notify for every placed-or-transient actor that uses this Binding. */
template <typename Callback>
void ForEachUsingActor(const UMtoULiveLinkBinding& Binding, Callback&& Notify)
{
    for (TObjectIterator<AMtoULiveLinkActor> It; It; ++It)
    {
        if (!It->HasAnyFlags(RF_ClassDefaultObject) && It->GetBinding() == &Binding)
        {
            Notify(**It);
        }
    }
}
}
#endif

void UMtoULiveLinkBinding::EnsureCharacterPartIds()
{
    TSet<FGuid> Seen;
    for (FMtoUCharacterPart& Part : AdditionalParts)
    {
        if (Part.PartId.IsValid() && !Seen.Contains(Part.PartId))
        {
            Seen.Add(Part.PartId);
            continue;
        }
        // A duplicated array element copies its identity; a repeated identity
        // would make two components claim one part, so repair it here instead
        // of letting the character connect with an ambiguous composition.
        do
        {
            Part.PartId = FGuid::NewGuid();
        }
        while (Seen.Contains(Part.PartId));
        Seen.Add(Part.PartId);
    }
}

void UMtoULiveLinkBinding::PostLoad()
{
    Super::PostLoad();
    EnsureCharacterPartIds();
}

#if WITH_EDITOR
void UMtoULiveLinkBinding::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    // Array-element edits report the inner property, so fall back to it when
    // no member property is recorded.
    const FName ChangedName = PropertyChangedEvent.MemberProperty
        ? PropertyChangedEvent.MemberProperty->GetFName()
        : PropertyChangedEvent.GetPropertyName();
    if (ChangedName.IsNone())
    {
        // A transaction restore (undo or redo calls PostEditUndo, which reaches
        // this function through PostEditChange with an empty event) and any
        // other unnamed change cannot say what moved, so each actor classifies
        // the current Binding state against what it already applied.
        EnsureCharacterPartIds();
        ForEachUsingActor(*this, [](AMtoULiveLinkActor& Actor)
        {
            Actor.NotifyBindingStateChanged();
        });
        return;
    }

    const bool bPartsChanged =
        ChangedName == GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, AdditionalParts)
        || IsCharacterPartProperty(PropertyChangedEvent.Property)
        || IsCharacterPartProperty(PropertyChangedEvent.MemberProperty);
    if (bPartsChanged)
    {
        EnsureCharacterPartIds();
    }
    if (ChangedName != GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, SkeletalMesh)
        && ChangedName != GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, PreviewStaticMesh)
        && ChangedName != GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, DriverGarmentSlotOverride)
        && !bPartsChanged)
    {
        return;
    }

    ForEachUsingActor(*this, [bPartsChanged](AMtoULiveLinkActor& Actor)
    {
        if (bPartsChanged)
        {
            // Additional Parts are part of the character composition, not of
            // the Preview revision: adding, removing, enabling, or replacing a
            // part must not discard an already generated garment Preview.
            Actor.NotifyCharacterPartsChanged();
        }
        else
        {
            Actor.NotifyBindingInputsChanged();
        }
    });
}
#endif
