#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkFactories.h"
#include "MtoULiveLinkPreview.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "IDetailCustomization.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Misc/ScopedSlowTask.h"
#include "Subsystems/ImportSubsystem.h"
#include "Subsystems/PlacementSubsystem.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MtoULiveLinkEditor"

namespace
{
FText StageText(EMtoUPreviewBuildStage Stage)
{
    switch (Stage)
    {
    case EMtoUPreviewBuildStage::Preflight:
        return LOCTEXT("Preflight", "Preflight");
    case EMtoUPreviewBuildStage::GeometryConversion:
        return LOCTEXT("GeometryConversion", "Geometry conversion");
    case EMtoUPreviewBuildStage::WeightTransfer:
        return LOCTEXT("WeightTransfer", "Weight transfer");
    case EMtoUPreviewBuildStage::SkeletalMeshBuild:
        return LOCTEXT("SkeletalMeshBuild", "Skeletal Mesh build");
    case EMtoUPreviewBuildStage::Validation:
        return LOCTEXT("Validation", "Validation");
    default:
        return FText::GetEmpty();
    }
}

class FMtoULiveLinkActorDetails final : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance()
    {
        return MakeShared<FMtoULiveLinkActorDetails>();
    }

    virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
    {
        TArray<TWeakObjectPtr<UObject>> Objects;
        DetailBuilder.GetObjectsBeingCustomized(Objects);
        TWeakObjectPtr<AMtoULiveLinkActor> Actor;
        for (const TWeakObjectPtr<UObject>& Object : Objects)
        {
            if (AMtoULiveLinkActor* Candidate = Cast<AMtoULiveLinkActor>(Object.Get()))
            {
                Actor = Candidate;
                break;
            }
        }

        IDetailCategoryBuilder& PreviewCategory = DetailBuilder.EditCategory(
            "MtoU Preview", LOCTEXT("MtoUPreviewCategory", "MtoU Preview"),
            ECategoryPriority::Important);

        PreviewCategory.AddCustomRow(LOCTEXT("RefreshPreviewFilter", "Refresh Preview"))
            .WholeRowContent()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .Padding(0.0f, 0.0f, 4.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("RefreshPreview", "Refresh Preview"))
                    .IsEnabled_Lambda([Actor]() { return Actor.IsValid(); })
                    .OnClicked_Lambda([Actor]()
                    {
                        if (AMtoULiveLinkActor* Target = Actor.Get())
                        {
                            FScopedSlowTask Progress(5.0f, LOCTEXT(
                                "PreparingPreview", "Preparing Generated Preview"));
                            Progress.MakeDialogDelayed(0.25f);
                            FMtoUPreviewPreparation::RefreshActor(
                                *Target,
                                [&Progress](EMtoUPreviewBuildStage Stage)
                                {
                                    Progress.EnterProgressFrame(1.0f, StageText(Stage));
                                });
                        }
                        return FReply::Handled();
                    })
                ]
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("DeletePreview", "Delete Preview"))
                    .IsEnabled_Lambda([Actor]()
                    {
                        return Actor.IsValid() && Actor->GetPreviewReadiness().IsUsable();
                    })
                    .OnClicked_Lambda([Actor]()
                    {
                        if (AMtoULiveLinkActor* Target = Actor.Get())
                        {
                            Target->NotifyGeneratedPreviewDeleted();
                        }
                        return FReply::Handled();
                    })
                ]
            ];

        PreviewCategory.AddCustomRow(LOCTEXT("ModifiedPartsFilter", "Modified parts"))
            .NameContent()
            [
                SNew(STextBlock)
                .Text(LOCTEXT("ModifiedParts", "Modified parts"))
            ]
            .ValueContent()
            .MinDesiredWidth(400.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(false)
                .Text_Lambda([Actor]()
                {
                    const FString Summary = Actor.IsValid()
                        ? Actor->GetPreviewReadiness().Summary : FString();
                    return !Summary.IsEmpty()
                        ? FText::FromString(Summary)
                        : LOCTEXT("NoModifiedParts", "Run Refresh Preview to compare meshes");
                })
            ];
    }
};
}

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
