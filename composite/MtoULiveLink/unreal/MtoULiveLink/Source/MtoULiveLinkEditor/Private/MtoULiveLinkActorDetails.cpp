#include "MtoULiveLinkActorDetails.h"

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkFactories.h"
#include "MtoULiveLinkPreview.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Input/Reply.h"
#include "Misc/ScopedSlowTask.h"
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
}

TSharedRef<IDetailCustomization> FMtoULiveLinkActorDetails::MakeInstance()
{
    return MakeShared<FMtoULiveLinkActorDetails>();
}

void FMtoULiveLinkActorDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
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
                    return HandleRefreshPreviewClicked(Actor);
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

    PreviewCategory.AddCustomRow(LOCTEXT("CharacterPartsFilter", "Character parts"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("CharacterParts", "Character parts"))
        ]
        .ValueContent()
        .MinDesiredWidth(400.0f)
        [
            SNew(STextBlock)
            .AutoWrapText(false)
            .ColorAndOpacity_Lambda([Actor]()
            {
                return Actor.IsValid() && !Actor->GetCharacterPartDiagnostics().IsEmpty()
                    ? FSlateColor(FLinearColor(1.0f, 0.35f, 0.25f))
                    : FSlateColor::UseForeground();
            })
            .Text_Lambda([Actor]()
            {
                if (!Actor.IsValid())
                {
                    return FText::GetEmpty();
                }
                const FString& Diagnostics = Actor->GetCharacterPartDiagnostics();
                if (!Diagnostics.IsEmpty())
                {
                    return FText::FromString(FString::Printf(
                        TEXT("%s\n%s"), *Actor->GetCharacterPartSummary(), *Diagnostics));
                }
                return Actor->GetCharacterPartSummary().IsEmpty()
                    ? LOCTEXT("NoCharacterParts", "No Primary Driver Skeletal Mesh")
                    : FText::FromString(Actor->GetCharacterPartSummary());
            })
        ];
}

FReply FMtoULiveLinkActorDetails::HandleRefreshPreviewClicked(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
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
}

#undef LOCTEXT_NAMESPACE
