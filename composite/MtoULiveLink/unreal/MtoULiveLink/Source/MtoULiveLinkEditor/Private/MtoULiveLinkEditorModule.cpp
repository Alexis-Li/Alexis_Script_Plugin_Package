#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkFactories.h"
#include "MtoULiveLinkPreview.h"
#include "MtoULiveLinkActorDetails.h"

#include "Editor.h"
#include "IDetailCustomization.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Subsystems/ImportSubsystem.h"
#include "Subsystems/PlacementSubsystem.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "MtoULiveLinkEditor"

class FMtoULiveLinkEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        ActorFactory = Cast<UMtoULiveLinkActorFactory>(
            GEditor->FindActorFactoryByClass(UMtoULiveLinkActorFactory::StaticClass()));
        if (!ActorFactory.IsValid())
        {
            ActorFactory = NewObject<UMtoULiveLinkActorFactory>();
            GEditor->ActorFactories.Add(ActorFactory.Get());
            bRegisteredWithEditor = true;
        }

        UPlacementSubsystem* PlacementSubsystem =
            GEditor->GetEditorSubsystem<UPlacementSubsystem>();
        if (PlacementSubsystem && !PlacementSubsystem->GetAssetFactoryFromFactoryClass(
                UMtoULiveLinkActorFactory::StaticClass()))
        {
            PlacementSubsystem->RegisterAssetFactory(ActorFactory.Get());
            bRegisteredWithPlacementSubsystem = true;
        }

        FPropertyEditorModule& PropertyEditor =
            FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
        PropertyEditor.RegisterCustomClassLayout(
            AMtoULiveLinkActor::StaticClass()->GetFName(),
            FOnGetDetailCustomizationInstance::CreateStatic(
                &FMtoULiveLinkActorDetails::MakeInstance));
        TSharedRef<FPropertySection> MtoUSection = PropertyEditor.FindOrCreateSection(
            AMtoULiveLinkActor::StaticClass()->GetFName(), "MtoU",
            LOCTEXT("MtoUSection", "MtoU"), 1000);
        MtoUSection->AddCategory("MtoU_LiveLink");
        MtoUSection->AddCategory("MtoU Preview");
        MtoUSection->AddCategory("MtoU Diagnostics");

        AssetReimportHandle = GEditor->GetEditorSubsystem<UImportSubsystem>()
            ->OnAssetReimport.AddRaw(this, &FMtoULiveLinkEditorModule::HandleAssetReimport);
    }

    virtual void ShutdownModule() override
    {
        if (GEditor)
        {
            if (ActorFactory.IsValid() && bRegisteredWithPlacementSubsystem)
            {
                if (UPlacementSubsystem* PlacementSubsystem =
                        GEditor->GetEditorSubsystem<UPlacementSubsystem>())
                {
                    PlacementSubsystem->UnregisterAssetFactory(ActorFactory.Get());
                }
            }
            if (ActorFactory.IsValid() && bRegisteredWithEditor)
            {
                GEditor->ActorFactories.Remove(ActorFactory.Get());
            }
            GEditor->GetEditorSubsystem<UImportSubsystem>()
                ->OnAssetReimport.Remove(AssetReimportHandle);
        }
        ActorFactory.Reset();
        bRegisteredWithEditor = false;
        bRegisteredWithPlacementSubsystem = false;
        if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
        {
            FPropertyEditorModule& PropertyEditor =
                FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
            PropertyEditor.UnregisterCustomClassLayout(
                AMtoULiveLinkActor::StaticClass()->GetFName());
            PropertyEditor.RemoveSection(
                AMtoULiveLinkActor::StaticClass()->GetFName(), "MtoU");
        }
        for (TObjectIterator<AMtoULiveLinkActor> It; It; ++It)
        {
            if (!It->HasAnyFlags(RF_ClassDefaultObject))
            {
                It->NotifyTransientPreviewReleased();
            }
        }
    }

private:
    void HandleAssetReimport(UObject* Asset)
    {
        for (TObjectIterator<AMtoULiveLinkActor> It; It; ++It)
        {
            if (!It->HasAnyFlags(RF_ClassDefaultObject))
            {
                It->NotifySourceAssetChanged(Asset, TEXT("Reimport"));
            }
        }
    }

    FDelegateHandle AssetReimportHandle;
    TWeakObjectPtr<UMtoULiveLinkActorFactory> ActorFactory;
    bool bRegisteredWithEditor = false;
    bool bRegisteredWithPlacementSubsystem = false;
};

IMPLEMENT_MODULE(FMtoULiveLinkEditorModule, MtoULiveLinkEditor)

#undef LOCTEXT_NAMESPACE
