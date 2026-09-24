#include "MtoULiveLinkActorDetails.h"

#include "MtoULiveLinkActor.h"
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
using ESeverity = FMtoULiveLinkActorDetails::ESeverity;

/** The user-facing meaning of the source's connection status string. */
enum class EMtoUConnectionState : uint8
{
    /** No session yet, or a status this build does not classify. */
    Unknown,
    Disconnected,
    Validating,
    NotReady,
    Partial,
    BoneOnly,
    Connected,
    Error
};

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

FSlateColor SeverityColor(ESeverity Severity)
{
    switch (Severity)
    {
    case ESeverity::Info:
        return FSlateColor(FLinearColor(0.45f, 0.72f, 1.0f));
    case ESeverity::Success:
        return FSlateColor(FLinearColor(0.35f, 0.85f, 0.45f));
    case ESeverity::Warning:
        return FSlateColor(FLinearColor(1.0f, 0.75f, 0.25f));
    case ESeverity::Error:
        return FSlateColor(FLinearColor(1.0f, 0.35f, 0.25f));
    case ESeverity::Neutral:
    default:
        return FSlateColor::UseForeground();
    }
}

/** The first line of a raw status, so a multi-line message stays scannable inline. */
FString FirstLine(const FString& Status)
{
    int32 LineBreak = INDEX_NONE;
    return Status.FindChar(TEXT('\n'), LineBreak) ? Status.Left(LineBreak) : Status;
}

EMtoUConnectionState ClassifyConnection(const FString& Status)
{
    // The strings are the ones MtoULiveLinkSource publishes for the actor; a
    // status this build does not know keeps its raw text in the headline.
    if (Status.IsEmpty())
    {
        return EMtoUConnectionState::Unknown;
    }
    if (Status.StartsWith(TEXT("Error"))
        || Status.StartsWith(TEXT("Preview morph mismatch")))
    {
        return EMtoUConnectionState::Error;
    }
    if (Status.StartsWith(TEXT("Preview not ready")))
    {
        return EMtoUConnectionState::NotReady;
    }
    if (Status.StartsWith(TEXT("Validating")))
    {
        return EMtoUConnectionState::Validating;
    }
    if (Status.StartsWith(TEXT("Connected: bone-only")))
    {
        return EMtoUConnectionState::BoneOnly;
    }
    if (Status.StartsWith(TEXT("Connected: partial")))
    {
        return EMtoUConnectionState::Partial;
    }
    if (Status.StartsWith(TEXT("Connected")))
    {
        return EMtoUConnectionState::Connected;
    }
    if (Status.StartsWith(TEXT("Disconnected")))
    {
        return EMtoUConnectionState::Disconnected;
    }
    return EMtoUConnectionState::Unknown;
}

FText ReadinessHeadline(const FMtoUPreviewReadiness& Readiness, ESeverity& OutSeverity)
{
    switch (Readiness.State)
    {
    case EMtoUPreviewState::Dirty:
        OutSeverity = ESeverity::Info;
        return LOCTEXT("ReadinessDirty", "Needs refresh");
    case EMtoUPreviewState::Building:
        OutSeverity = ESeverity::Info;
        return FText::Format(LOCTEXT("ReadinessBuilding", "Refreshing: {0}"), StageText(Readiness.Stage));
    case EMtoUPreviewState::Ready:
        OutSeverity = ESeverity::Success;
        return LOCTEXT("ReadinessReady", "Ready");
    case EMtoUPreviewState::Warning:
        OutSeverity = ESeverity::Warning;
        return LOCTEXT("ReadinessWarning", "Ready with a warning");
    case EMtoUPreviewState::Error:
        OutSeverity = ESeverity::Error;
        return LOCTEXT("ReadinessError", "Refresh failed");
    case EMtoUPreviewState::None:
    default:
        OutSeverity = ESeverity::Neutral;
        return LOCTEXT("ReadinessNone", "Not set up");
    }
}

FText ConnectionHeadline(EMtoUConnectionState State, const FString& Status, ESeverity& OutSeverity)
{
    switch (State)
    {
    case EMtoUConnectionState::Disconnected:
        OutSeverity = ESeverity::Neutral;
        return LOCTEXT("ConnectionDisconnected", "Disconnected");
    case EMtoUConnectionState::Validating:
        OutSeverity = ESeverity::Info;
        return LOCTEXT("ConnectionValidating", "Connecting");
    case EMtoUConnectionState::NotReady:
        OutSeverity = ESeverity::Warning;
        return LOCTEXT("ConnectionNotReady", "Preview not ready");
    case EMtoUConnectionState::Partial:
        OutSeverity = ESeverity::Warning;
        return LOCTEXT("ConnectionPartial", "Connected with partial Morphs");
    case EMtoUConnectionState::BoneOnly:
        OutSeverity = ESeverity::Warning;
        return LOCTEXT("ConnectionBoneOnly", "Connected without Morphs");
    case EMtoUConnectionState::Connected:
        OutSeverity = ESeverity::Success;
        return LOCTEXT("ConnectionConnected", "Connected");
    case EMtoUConnectionState::Error:
        OutSeverity = ESeverity::Error;
        return LOCTEXT("ConnectionError", "Connection error");
    case EMtoUConnectionState::Unknown:
    default:
        OutSeverity = ESeverity::Neutral;
        return Status.TrimStartAndEnd().IsEmpty()
            ? LOCTEXT("ConnectionIdle", "Not connected")
            : FText::FromString(FirstLine(Status));
    }
}

/** The raw message is shown whenever it says more than the headline already does. */
bool ConnectionNeedsDetail(const FString& Status)
{
    return !Status.IsEmpty()
        && Status != TEXT("Connected")
        && Status != TEXT("Disconnected")
        && Status != TEXT("Validating");
}

FText NextStepText(EMtoUConnectionState Connection, const FMtoUPreviewReadiness& Readiness)
{
    switch (Connection)
    {
    case EMtoUConnectionState::Error:
        return LOCTEXT("NextStepConnectionError",
            "Fix the reported error, then reconnect in Maya; the full message stays in MtoU Diagnostics.");
    case EMtoUConnectionState::NotReady:
        return LOCTEXT("NextStepPreviewNotReady",
            "Run Refresh Preview, then connect the Model workflow in Maya.");
    case EMtoUConnectionState::Validating:
        return LOCTEXT("NextStepValidating",
            "Maya is negotiating the connection; wait for its result.");
    case EMtoUConnectionState::BoneOnly:
        return LOCTEXT("NextStepBoneOnly",
            "Enable Transfer BS in Maya and reconnect before accepting; this session streams no Morphs.");
    case EMtoUConnectionState::Partial:
        return LOCTEXT("NextStepPartial",
            "Connected with partial Morph coverage; review Model diagnostics before accepting.");
    case EMtoUConnectionState::Connected:
        return LOCTEXT("NextStepConnected",
            "Connected through the Maya workflow in use; this display follows the session.");
    case EMtoUConnectionState::Disconnected:
    case EMtoUConnectionState::Unknown:
    default:
        break;
    }
    switch (Readiness.State)
    {
    case EMtoUPreviewState::Building:
        return LOCTEXT("NextStepBuilding", "Refresh is running; wait for it to finish.");
    case EMtoUPreviewState::Error:
        return LOCTEXT("NextStepRefreshFailed",
            "Fix the reported inputs, then run Refresh Preview again; the failure message stays in MtoU Diagnostics.");
    case EMtoUPreviewState::Warning:
        return LOCTEXT("NextStepQualityWarning",
            "The preview is usable with a quality warning; review MtoU Diagnostics before accepting.");
    case EMtoUPreviewState::Dirty:
        return LOCTEXT("NextStepDirty",
            "Inputs changed; run Refresh Preview to rebuild the Generated Preview.");
    case EMtoUPreviewState::Ready:
        return LOCTEXT("NextStepReady",
            "Connect the Model workflow in Maya, or run Refresh Preview after changing inputs.");
    case EMtoUPreviewState::None:
    default:
        return LOCTEXT("NextStepNone", "Assign the Binding inputs, then run Refresh Preview.");
    }
}

FText DisplayText(EMtoUDisplayTarget Target)
{
    switch (Target)
    {
    case EMtoUDisplayTarget::GeneratedPreview:
        return LOCTEXT("DisplayGeneratedPreview", "Showing Generated Preview");
    case EMtoUDisplayTarget::Driver:
        return LOCTEXT("DisplayDriver", "Showing Driver");
    case EMtoUDisplayTarget::Hidden:
    default:
        return LOCTEXT("DisplayHidden", "Showing nothing (no mesh selected)");
    }
}

FText RefreshPreviewTooltip(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return Actor.IsValid()
        ? LOCTEXT("RefreshPreviewTooltip",
            "Rebuild the Generated Preview from the current Binding inputs. Ends the active session and shows the new Generated Preview.")
        : LOCTEXT("RefreshPreviewUnavailable",
            "Select a Binding actor to refresh its Generated Preview.");
}

FText DeletePreviewTooltip(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return FMtoULiveLinkActorDetails::CanDeletePreview(Actor)
        ? LOCTEXT("DeletePreviewTooltip",
            "Remove the Generated Preview and return to the Driver display.")
        : LOCTEXT("DeletePreviewUnavailable",
            "Available after Refresh Preview produces a usable Generated Preview.");
}

/** Builds the one status view from an already-read axis snapshot. */
FMtoULiveLinkActorDetails::FStatusView BuildStatusView(
    AMtoULiveLinkActor& Target, const FMtoUPreviewReadiness& Readiness)
{
    FMtoULiveLinkActorDetails::FStatusView View;
    const FString& ConnectionStatus = Target.GetConnectionStatus();
    const EMtoUConnectionState Connection = ClassifyConnection(ConnectionStatus);

    ESeverity ReadinessSeverity = ESeverity::Neutral;
    const FText ReadinessText = ReadinessHeadline(Readiness, ReadinessSeverity);
    ESeverity ConnectionSeverity = ESeverity::Neutral;
    const FText ConnectionText = ConnectionHeadline(Connection, ConnectionStatus, ConnectionSeverity);

    View.State = FText::Format(LOCTEXT("StatusHeadline", "{0} \u00B7 {1}"), ReadinessText, ConnectionText);
    View.Severity = ReadinessSeverity > ConnectionSeverity ? ReadinessSeverity : ConnectionSeverity;
    View.Detail = ConnectionNeedsDetail(ConnectionStatus)
        ? FText::FromString(ConnectionStatus.TrimStartAndEnd())
        : FText::GetEmpty();
    View.NextStep = NextStepText(Connection, Readiness);
    View.Display = DisplayText(Target.GetDisplayTarget());
    return View;
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

    // The curated Status row below is the one place the connection state is
    // read; the actor's raw status property row would repeat it verbatim.
    DetailBuilder.HideProperty(FName(TEXT("ConnectionStatus")));

    IDetailCategoryBuilder& PreviewCategory = DetailBuilder.EditCategory(
        "MtoU Preview", LOCTEXT("MtoUPreviewCategory", "MtoU Preview"),
        ECategoryPriority::Important);

    // One presenter per panel: every binding of the status row reads the same
    // cached view, so a Slate poll keeps the status instead of rebuilding it.
    const TSharedRef<FStatusPresenter> Status = MakeShared<FStatusPresenter>(Actor);

    // One status block: Preview readiness and connection state in the colored
    // headline, the raw message only when it adds information, the single next
    // step, and the display source that follows the actor.
    PreviewCategory.AddCustomRow(LOCTEXT("StatusFilter", "Status"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("Status", "Status"))
        ]
        .ValueContent()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity_Lambda([Status]()
                {
                    return SeverityColor(Status->Get().Severity);
                })
                .Text_Lambda([Status]()
                {
                    return Status->Get().State;
                })
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 2.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                .Text_Lambda([Status]()
                {
                    return Status->Get().Detail;
                })
                .Visibility_Lambda([Status]()
                {
                    return Status->Get().Detail.IsEmpty()
                        ? EVisibility::Collapsed
                        : EVisibility::Visible;
                })
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 2.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                .Text_Lambda([Status]()
                {
                    return Status->Get().NextStep;
                })
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 2.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                .Text_Lambda([Status]()
                {
                    return Status->Get().Display;
                })
            ]
        ];

    // The two Preview actions sit together, content-sized, directly under the
    // status that explains them.
    PreviewCategory.AddCustomRow(LOCTEXT("PreviewActionsFilter", "Preview actions"))
        .WholeRowContent()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("RefreshPreview", "Refresh Preview"))
                .ToolTipText_Lambda([Actor]() { return RefreshPreviewTooltip(Actor); })
                .IsEnabled_Lambda([Actor]() { return Actor.IsValid(); })
                .OnClicked_Lambda([Actor]()
                {
                    return HandleRefreshPreviewClicked(Actor);
                })
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("DeletePreview", "Delete Preview"))
                .ToolTipText_Lambda([Actor]() { return DeletePreviewTooltip(Actor); })
                .IsEnabled_Lambda([Actor]()
                {
                    return CanDeletePreview(Actor);
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

    DiagnosticsCategory.AddCustomRow(LOCTEXT("ModifiedPartsFilter", "Modified parts"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("ModifiedParts", "Modified parts"))
        ]
        .ValueContent()
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

    DiagnosticsCategory.AddCustomRow(LOCTEXT("PreviewDetailsFilter", "Preview details"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("PreviewDetails", "Preview details"))
        ]
        .ValueContent()
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
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .ColorAndOpacity_Lambda([Actor]()
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

    DiagnosticsCategory.AddCustomRow(LOCTEXT("ConnectionDetailsFilter", "Connection details"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("ConnectionDetails", "Connection"))
        ]
        .ValueContent()
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text_Lambda([Actor]()
            {
                if (!Actor.IsValid())
                {
                    return FText::GetEmpty();
                }
                const FString& Status = Actor->GetConnectionStatus();
                return !Status.IsEmpty()
                    ? FText::FromString(Status)
                    : LOCTEXT("NoConnectionStatus", "No connection status recorded");
            })
        ];
}

FMtoULiveLinkActorDetails::FStatusView FMtoULiveLinkActorDetails::MakeStatusView(
    TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    if (AMtoULiveLinkActor* Target = Actor.Get())
    {
        return BuildStatusView(*Target, Target->GetPreviewReadiness());
    }
    FStatusView View;
    View.State = LOCTEXT("StatusNoActor", "No Binding actor selected");
    View.NextStep = LOCTEXT("NextStepNoActor",
        "Select a Binding actor to refresh or delete its Generated Preview.");
    return View;
}

FMtoULiveLinkActorDetails::FStatusPresenter::FStatusPresenter(TWeakObjectPtr<AMtoULiveLinkActor> InActor)
    : Actor(InActor)
{
}

const FMtoULiveLinkActorDetails::FStatusView& FMtoULiveLinkActorDetails::FStatusPresenter::Get()
{
    AMtoULiveLinkActor* Target = Actor.Get();
    if (!Target)
    {
        // A destroyed actor drops its cached view instead of presenting the
        // state it had while it was alive.
        if (!bHasView || CachedKey.bHasActor)
        {
            CachedKey = FKey();
            View = FMtoULiveLinkActorDetails::MakeStatusView(TWeakObjectPtr<AMtoULiveLinkActor>());
            bHasView = true;
        }
        return View;
    }

    // The change key reads each axis once: readiness state and stage, the
    // connection message, the display selection. Readiness is read through the
    // actor's one coherent snapshot and only formatted when the key moved.
    const FMtoUPreviewReadiness Readiness = Target->GetPreviewReadiness();
    const FString& Connection = Target->GetConnectionStatus();
    const EMtoUDisplayTarget Display = Target->GetDisplayTarget();
    if (bHasView
        && CachedKey.bHasActor
        && CachedKey.State == Readiness.State
        && CachedKey.Stage == Readiness.Stage
        && CachedKey.Display == Display
        && CachedKey.Connection == Connection)
    {
        return View;
    }

    View = BuildStatusView(*Target, Readiness);
    CachedKey.bHasActor = true;
    CachedKey.State = Readiness.State;
    CachedKey.Stage = Readiness.Stage;
    CachedKey.Display = Display;
    CachedKey.Connection = Connection;
    bHasView = true;
    return View;
}

bool FMtoULiveLinkActorDetails::CanDeletePreview(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return Actor.IsValid() && Actor->GetPreviewReadiness().IsUsable();
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
