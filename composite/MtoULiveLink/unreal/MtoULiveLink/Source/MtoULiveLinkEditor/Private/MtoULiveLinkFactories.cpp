#include "MtoULiveLinkFactories.h"

#include "MtoUCharacterComposition.h"
#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"

#include "AssetRegistry/AssetData.h"

#define LOCTEXT_NAMESPACE "MtoULiveLinkFactories"

UMtoULiveLinkBindingFactory::UMtoULiveLinkBindingFactory()
{
    bCreateNew = true;
    bEditAfterNew = true;
    SupportedClass = UMtoULiveLinkBinding::StaticClass();
}

FText UMtoULiveLinkBindingFactory::GetDisplayName() const
{
    return LOCTEXT("BindingDisplayName", "MtoU_LiveLink Binding");
}

UObject* UMtoULiveLinkBindingFactory::FactoryCreateNew(UClass* Class, UObject* Parent,
    FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
    return NewObject<UMtoULiveLinkBinding>(Parent, Class, Name, Flags | RF_Transactional);
}

UMtoULiveLinkActorFactory::UMtoULiveLinkActorFactory()
{
    DisplayName = LOCTEXT("ActorDisplayName", "MtoU_LiveLink Actor");
    NewActorClass = AMtoULiveLinkActor::StaticClass();
}

bool UMtoULiveLinkActorFactory::CanCreateActorFrom(
    const FAssetData& AssetData, FText& OutErrorMsg)
{
    if (!AssetData.IsInstanceOf(UMtoULiveLinkBinding::StaticClass()))
    {
        return false;
    }

    const UMtoULiveLinkBinding* Binding = Cast<UMtoULiveLinkBinding>(AssetData.GetAsset());
    // Only the Primary Driver is a placement requirement: an Additional Part
    // may stay incomplete while the user keeps configuring the Binding.
    if (!FMtoUCharacterComposition::HasPrimaryDriver(Binding))
    {
        OutErrorMsg = LOCTEXT("MissingSkeletalMesh",
            "Select a Skeletal Mesh on the binding before placing it.");
        return false;
    }
    return true;
}

void UMtoULiveLinkActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
    if (UMtoULiveLinkBinding* Binding = Cast<UMtoULiveLinkBinding>(Asset))
    {
        if (AMtoULiveLinkActor* Actor = Cast<AMtoULiveLinkActor>(NewActor))
        {
            Actor->SetBinding(Binding);
        }
    }
}

UObject* UMtoULiveLinkActorFactory::GetAssetFromActorInstance(AActor* ActorInstance)
{
    if (AMtoULiveLinkActor* Actor = Cast<AMtoULiveLinkActor>(ActorInstance))
    {
        return Actor->GetBinding();
    }
    return nullptr;
}

#undef LOCTEXT_NAMESPACE
