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

FText DisplaySourceText(const TWeakObjectPtr<AMtoULiveLinkActor>& Actor)
{
    if (!Actor.IsValid())
    {
        return FText::GetEmpty();
    }
    switch (Actor->GetDisplayTarget())
    {
    case EMtoUDisplayTarget::Driver:
        return LOCTEXT("DisplaySourceDriver", "Driver (real-time Animation preview)");
    case EMtoUDisplayTarget::GeneratedPreview:
        return LOCTEXT("DisplaySourceGenerated", "Generated Preview (Model garment preview)");
    case EMtoUDisplayTarget::Hidden:
    default:
        return LOCTEXT("DisplaySourceHidden", "Hidden (no mesh selected for display)");
    }
}

FText ConnectionText(const TWeakObjectPtr<AMtoULiveLinkActor>& Actor)
{
    if (!Actor.IsValid())
    {
        return FText::GetEmpty();
    }
    const FString& Status = Actor->GetConnectionStatus();
    return Status.IsEmpty()
        ? LOCTEXT("ConnectionUnknown", "Unknown: reconnect in Maya to refresh")
        : FText::FromString(Status);
}

FSlateColor ConnectionColor(const TWeakObjectPtr<AMtoULiveLinkActor>& Actor)
{
    if (!Actor.IsValid())
    {
        return FSlateColor::UseForeground();
    }
    const FString& Status = Actor->GetConnectionStatus();
    if (Status.StartsWith(TEXT("Error")) || Status.StartsWith(TEXT("Preview morph mismatch")))
    {
        return FSlateColor(FLinearColor(1.0f, 0.35f, 0.25f));
    }
    if (Status.StartsWith(TEXT("Connected")))
    {
        return FSlateColor(FLinearColor(0.35f, 0.85f, 0.45f));
    }
    return FSlateColor::UseForeground();
}

FText NextStepText(const TWeakObjectPtr<AMtoULiveLinkActor>& Actor)
{
    if (!Actor.IsValid())
    {
        return FText::GetEmpty();
    }
    const FString Status = Actor->GetConnectionStatus();
    const FMtoUPreviewReadiness Readiness = Actor->GetPreviewReadiness();
    if (Status.StartsWith(TEXT("Error")) || Status.StartsWith(TEXT("Preview morph mismatch")))
    {
        return LOCTEXT("NextStepFixError", "Fix the reported error, then reconnect in Maya.");
    }
    if (Status.StartsWith(TEXT("Preview not ready")))
    {
        return LOCTEXT("NextStepRefresh", "Run Refresh Preview, then connect the Model workflow in Maya.");
    }
    if (Status.StartsWith(TEXT("Validating")))
    {
        return LOCTEXT("NextStepValidating", "Negotiating the connection; wait for the result in Maya.");
    }
    if (Status.StartsWith(TEXT("Connected: partial")))
    {
        return LOCTEXT("NextStepPartial", "Connected with partial Morph coverage; check Model diagnostics before accepting.");
    }
    if (Status.StartsWith(TEXT("Connected: bone-only")))
    {
        return LOCTEXT("NextStepBoneOnly", "Bone-only diagnostic only; enable Transfer BS and reconnect for model acceptance.");
    }
    if (Status.StartsWith(TEXT("Connected")))
    {
        return LOCTEXT("NextStepConnected", "Connected: pose or play in Maya and observe this display.");
    }
    if (Readiness.State == EMtoUPreviewState::Building)
    {
        return LOCTEXT("NextStepBuilding", "Refresh is running; wait for it to finish.");
    }
    return LOCTEXT("NextStepDisconnected", "Disconnected: connect in Maya to start previewing.");
}

FText ReadinessText(const TWeakObjectPtr<AMtoULiveLinkActor>& Actor)
{
    if (!Actor.IsValid())
    {
        return FText::GetEmpty();
    }
    const FMtoUPreviewReadiness Readiness = Actor->GetPreviewReadiness();
    switch (Readiness.State)
    {
    case EMtoUPreviewState::None:
        return LOCTEXT("ReadinessNone", "None: assign the Binding inputs, then Refresh Preview.");
    case EMtoUPreviewState::Dirty:
        return LOCTEXT("ReadinessDirty", "Dirty: inputs changed; run Refresh Preview.");
    case EMtoUPreviewState::Building:
        return FText::Format(
            LOCTEXT("ReadinessBuilding", "Building: {0}."), StageText(Readiness.Stage));
    case EMtoUPreviewState::Ready:
        return LOCTEXT("ReadinessReady", "Ready: the current revision has a complete Generated Preview.");
    case EMtoUPreviewState::Warning:
        return LOCTEXT("ReadinessWarning", "Warning: usable preview; inspect the quality warning before accepting.");
    case EMtoUPreviewState::Error:
    default:
        return LOCTEXT("ReadinessError", "Error: Refresh failed; fix the inputs and run Refresh Preview again.");
    }
}

FSlateColor ReadinessColor(const TWeakObjectPtr<AMtoULiveLinkActor>& Actor)
{
    if (!Actor.IsValid())
    {
        return FSlateColor::UseForeground();
    }
    const EMtoUPreviewState State = Actor->GetPreviewReadiness().State;
    if (State == EMtoUPreviewState::Warning)
    {
        return FSlateColor(FLinearColor(1.0f, 0.75f, 0.25f));
    }
    if (State == EMtoUPreviewState::Error)
    {
        return FSlateColor(FLinearColor(1.0f, 0.35f, 0.25f));
    }
    if (State == EMtoUPreviewState::Ready)
    {
        return FSlateColor(FLinearColor(0.35f, 0.85f, 0.45f));
    }
    return FSlateColor::UseForeground();
}

FSlateColor ModelDiagnosticsColor(const TWeakObjectPtr<AMtoULiveLinkActor>& Actor)
{
    if (!Actor.IsValid())
    {
        return FSlateColor::UseForeground();
    }
    switch (Actor->GetModelDiagnosticLevel())
    {
    case EMtoUModelDiagnosticLevel::Partial:
        return FSlateColor(FLinearColor(1.0f, 0.75f, 0.25f));
    case EMtoUModelDiagnosticLevel::BoneOnly:
        return FSlateColor(FLinearColor(1.0f, 0.6f, 0.25f));
    case EMtoUModelDiagnosticLevel::Error:
        return FSlateColor(FLinearColor(1.0f, 0.35f, 0.25f));
    case EMtoUModelDiagnosticLevel::Full:
        return FSlateColor(FLinearColor(0.35f, 0.85f, 0.45f));
    case EMtoUModelDiagnosticLevel::None:
    default:
        return FSlateColor::UseForeground();
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

    PreviewCategory.AddCustomRow(LOCTEXT("DisplaySourceFilter", "Display source"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("DisplaySource", "Display source"))
        ]
        .ValueContent()
        .MinDesiredWidth(400.0f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text_Lambda([Actor]()
            {
                return DisplaySourceText(Actor);
            })
        ];

    PreviewCategory.AddCustomRow(LOCTEXT("ConnectionFilter", "Connection"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("Connection", "Connection"))
        ]
        .ValueContent()
        .MinDesiredWidth(400.0f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .ColorAndOpacity_Lambda([Actor]()
            {
                return ConnectionColor(Actor);
            })
            .Text_Lambda([Actor]()
            {
                return ConnectionText(Actor);
            })
        ];

    PreviewCategory.AddCustomRow(LOCTEXT("NextStepFilter", "Next step"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("NextStep", "Next step"))
        ]
        .ValueContent()
        .MinDesiredWidth(400.0f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text_Lambda([Actor]()
            {
                return NextStepText(Actor);
            })
        ];

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
                .ToolTipText(LOCTEXT(
                    "RefreshPreviewTooltip",
                    "Generate the current Preview revision. Ends the active session and shows the Generated Preview."))
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
                .ToolTipText(LOCTEXT(
                    "DeletePreviewTooltip",
                    "Remove the Generated Preview and restore the Driver display. Only available while a preview is ready."))
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

    PreviewCategory.AddCustomRow(LOCTEXT("PreviewReadinessFilter", "Preview readiness"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("PreviewReadiness", "Preview readiness"))
        ]
        .ValueContent()
        .MinDesiredWidth(400.0f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .ColorAndOpacity_Lambda([Actor]()
            {
                return ReadinessColor(Actor);
            })
            .Text_Lambda([Actor]()
            {
                return ReadinessText(Actor);
            })
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
            .AutoWrapText(true)
            .Text_Lambda([Actor]()
            {
                const FString Summary = Actor.IsValid()
                    ? Actor->GetPreviewReadiness().Summary : FString();
                return !Summary.IsEmpty()
                    ? FText::FromString(Summary)
                    : LOCTEXT("NoModifiedParts", "Run Refresh Preview to compare meshes");
            })
        ];

    IDetailCategoryBuilder& DiagnosticsCategory = DetailBuilder.EditCategory(
        "MtoU Diagnostics", LOCTEXT("MtoUDiagnosticsCategory", "MtoU Diagnostics"),
        ECategoryPriority::Default);
    DiagnosticsCategory.InitiallyCollapsed(true);

    DiagnosticsCategory.AddCustomRow(LOCTEXT("CharacterPartsFilter", "Character parts"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("CharacterParts", "Character parts"))
        ]
        .ValueContent()
        .MinDesiredWidth(400.0f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
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

    DiagnosticsCategory.AddCustomRow(LOCTEXT("PreviewDetailsFilter", "Preview details"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("PreviewDetails", "Preview details"))
        ]
        .ValueContent()
        .MinDesiredWidth(400.0f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text_Lambda([Actor]()
            {
                if (!Actor.IsValid())
                {
                    return FText::GetEmpty();
                }
                const FString& Diagnostics = Actor->GetPreviewReadiness().Diagnostics;
                return !Diagnostics.IsEmpty()
                    ? FText::FromString(Diagnostics)
                    : LOCTEXT("NoPreviewDetails", "No preview diagnostics recorded");
            })
        ];

    DiagnosticsCategory.AddCustomRow(LOCTEXT("ModelDiagnosticsFilter", "Model diagnostics"))
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
            .ColorAndOpacity_Lambda([Actor]()
            {
                return ModelDiagnosticsColor(Actor);
            })
            .Text_Lambda([Actor]()
            {
                if (!Actor.IsValid())
                {
                    return FText::GetEmpty();
                }
                const FString& Diagnostics = Actor->GetModelDiagnostics();
                return !Diagnostics.IsEmpty()
                    ? FText::FromString(Diagnostics)
                    : LOCTEXT("NoModelDiagnostics", "No Model connection diagnostics recorded");
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

#if WITH_DEV_AUTOMATION_TESTS
FText FMtoULiveLinkActorDetails::TestDisplaySourceText(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return DisplaySourceText(Actor);
}

FText FMtoULiveLinkActorDetails::TestConnectionText(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return ConnectionText(Actor);
}

FText FMtoULiveLinkActorDetails::TestNextStepText(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return NextStepText(Actor);
}

FText FMtoULiveLinkActorDetails::TestReadinessText(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return ReadinessText(Actor);
}
#endif
#undef LOCTEXT_NAMESPACE
