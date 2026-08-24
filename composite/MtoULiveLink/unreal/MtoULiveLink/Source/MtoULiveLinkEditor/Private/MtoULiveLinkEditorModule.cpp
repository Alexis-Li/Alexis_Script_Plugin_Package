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

        DetailBuilder.EditCategory("MtoU Preview")
            .AddCustomRow(LOCTEXT("RefreshPreviewFilter", "Refresh Preview"))
            .WholeRowContent()
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
            ];

        DetailBuilder.EditCategory("MtoU Preview")
            .AddCustomRow(LOCTEXT("ModelDiagnosticsFilter", "Model diagnostics"))
            .NameContent()
            [
                SNew(STextBlock)
                .Text(LOCTEXT("ModelDiagnostics", "Model diagnostics"))
            ]
            .ValueContent()
            .MinDesiredWidth(400.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Actor]()
                {
                    return Actor.IsValid()
                        ? FText::FromString(Actor->GetModelDiagnostics())
                        : FText::GetEmpty();
                })
                .ColorAndOpacity_Lambda([Actor]()
                {
                    if (!Actor.IsValid())
                    {
                        return FSlateColor::UseForeground();
                    }
                    switch (Actor->GetModelDiagnosticLevel())
                    {
                    case EMtoUModelDiagnosticLevel::Partial:
                        return FSlateColor(FLinearColor::Yellow);
                    case EMtoUModelDiagnosticLevel::BoneOnly:
                        return FSlateColor(FLinearColor(1.0f, 0.5f, 0.0f));
                    case EMtoUModelDiagnosticLevel::Error:
                        return FSlateColor(FLinearColor::Red);
                    default:
                        return FSlateColor::UseForeground();
                    }
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
            FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor")
                .UnregisterCustomClassLayout(AMtoULiveLinkActor::StaticClass()->GetFName());
        }
        for (TObjectIterator<AMtoULiveLinkActor> It; It; ++It)
        {
            if (!It->HasAnyFlags(RF_ClassDefaultObject))
            {
                It->ReleaseGeneratedPreview();
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
