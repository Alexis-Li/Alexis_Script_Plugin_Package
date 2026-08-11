#pragma once

#include "ActorFactories/ActorFactory.h"
#include "Factories/Factory.h"

#include "MtoULiveLinkFactories.generated.h"

UCLASS(hidecategories=Object)
class UMtoULiveLinkBindingFactory : public UFactory
{
    GENERATED_BODY()

public:
    UMtoULiveLinkBindingFactory();
    virtual FText GetDisplayName() const override;
    virtual UObject* FactoryCreateNew(UClass* Class, UObject* Parent, FName Name,
        EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
};

UCLASS()
class UMtoULiveLinkActorFactory : public UActorFactory
{
    GENERATED_BODY()

public:
    UMtoULiveLinkActorFactory();
    virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
    virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
    virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};
