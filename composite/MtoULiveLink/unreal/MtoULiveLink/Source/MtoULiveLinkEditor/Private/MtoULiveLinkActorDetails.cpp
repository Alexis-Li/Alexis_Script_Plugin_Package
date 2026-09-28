#include "MtoULiveLinkActorDetails.h"

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"
#include "MtoULiveLinkPreview.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "IDetailPropertyRow.h"
#include "DetailWidgetRow.h"
#include "Engine/SkeletalMesh.h"
#include "Input/Reply.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Internationalization/Regex.h"
#include "Misc/ScopedSlowTask.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
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
        return LOCTEXT("Preflight", "检查输入");
    case EMtoUPreviewBuildStage::GeometryConversion:
        return LOCTEXT("GeometryConversion", "转换几何体");
    case EMtoUPreviewBuildStage::WeightTransfer:
        return LOCTEXT("WeightTransfer", "传递权重");
    case EMtoUPreviewBuildStage::SkeletalMeshBuild:
        return LOCTEXT("SkeletalMeshBuild", "构建骨骼网格");
    case EMtoUPreviewBuildStage::Validation:
        return LOCTEXT("Validation", "验证结果");
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

FText CacheStateText(const FMtoUCachePlaybackView& View)
{
    if (!View.bConnected)
    {
        return LOCTEXT("CacheDisconnected", "未连接");
    }
    switch (View.State)
    {
    case EMtoUCacheState::Entered:
        return LOCTEXT("CacheEntered", "等待 Maya 捕获");
    case EMtoUCacheState::Receiving:
        return LOCTEXT("CacheReceiving", "正在接收并验证");
    case EMtoUCacheState::Ready:
        return LOCTEXT("CacheReady", "已就绪，等待播放");
    case EMtoUCacheState::Playing:
        return View.bLoopEnabled
            ? FText::Format(
                LOCTEXT("CachePlayingLoop", "正在循环播放（第 {0} 轮）"),
                FText::AsNumber(View.LoopRound + 1))
            : LOCTEXT("CachePlaying", "正在播放");
    case EMtoUCacheState::Paused:
        // A seek shows where it positioned; a pause shows the held pose, and
        // a pause before the first pose names neither.
        if (View.bPositionedBySeek)
        {
            return View.bHasAppliedSourceFrame
                ? FText::Format(
                    LOCTEXT("CacheSeeked", "已定位到源帧 {0}，已暂停"),
                    FText::FromString(FString::FromInt(View.CurrentSourceFrame)))
                : LOCTEXT("CacheSeekedUnknown", "已定位，已暂停");
        }
        return View.bHasAppliedSourceFrame
            ? FText::Format(
                LOCTEXT("CachePaused", "已暂停于源帧 {0}，姿势保留"),
                FText::FromString(FString::FromInt(View.CurrentSourceFrame)))
            : LOCTEXT("CachePausedNoPose", "已暂停，姿势保留");
    case EMtoUCacheState::Stopped:
        return LOCTEXT("CacheStopped", "已停止，缓存保留");
    case EMtoUCacheState::Completed:
        return LOCTEXT("CacheCompleted", "已结束，停在最后一帧");
    case EMtoUCacheState::Failed:
        return LOCTEXT("CacheFailed", "播放失败，缓存保留");
    case EMtoUCacheState::Idle:
    default:
        return LOCTEXT("CacheIdle", "实时预览");
    }
}

FText CacheSummaryText(const FMtoUCachePlaybackView& View)
{
    if (!View.bConnected || View.FrameCount <= 0)
    {
        return LOCTEXT("CacheNoFrames", "缓存：无。请在 Maya 捕获并上传。");
    }
    // The numbers are data, not prose: they keep their exact digits, including
    // negative source frames, while the skeleton around them is localizable.
    const FText AppliedFrame = View.bHasAppliedSourceFrame
        ? FText::FromString(FString::FromInt(View.CurrentSourceFrame))
        : LOCTEXT("NoAppliedFrame", "—");
    return FText::Format(
        LOCTEXT("CacheFrameSummary", "源帧 {0}–{1} · {2} fps · 当前已应用帧 {3} · {4}/{5} 帧"),
        FText::FromString(FString::FromInt(View.StartFrame)),
        FText::FromString(FString::FromInt(View.EndFrame)),
        FText::FromString(FString::Printf(TEXT("%g"), View.Fps)),
        AppliedFrame,
        FText::FromString(FString::FromInt(View.AppliedFrames)),
        FText::FromString(FString::FromInt(View.FrameCount)));
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
        return LOCTEXT("ReadinessDirty", "需要刷新");
    case EMtoUPreviewState::Building:
        OutSeverity = ESeverity::Info;
        return FText::Format(LOCTEXT("ReadinessBuilding", "正在生成：{0}"), StageText(Readiness.Stage));
    case EMtoUPreviewState::Ready:
        OutSeverity = ESeverity::Success;
        return LOCTEXT("ReadinessReady", "预览已就绪");
    case EMtoUPreviewState::Warning:
        OutSeverity = ESeverity::Warning;
        return LOCTEXT("ReadinessWarning", "预览已就绪（有警告）");
    case EMtoUPreviewState::Error:
        OutSeverity = ESeverity::Error;
        return LOCTEXT("ReadinessError", "预览生成失败");
    case EMtoUPreviewState::None:
    default:
        OutSeverity = ESeverity::Neutral;
        return LOCTEXT("ReadinessNone", "尚未生成预览");
    }
}

FText ConnectionHeadline(
    EMtoUConnectionState State, const FString& Status, EMtoULinkSessionState Session,
    ESeverity& OutSeverity)
{
    switch (State)
    {
    case EMtoUConnectionState::Disconnected:
        OutSeverity = ESeverity::Neutral;
        // Every session that ends reports the same "Disconnected" string, so
        // only the actor's session lifecycle separates a link that was lost
        // from one that was never established.
        return Session == EMtoULinkSessionState::Ended
            ? LOCTEXT("ConnectionInterrupted", "连接已中断")
            : LOCTEXT("ConnectionDisconnected", "未连接");
    case EMtoUConnectionState::Validating:
        OutSeverity = ESeverity::Info;
        return LOCTEXT("ConnectionValidating", "连接中");
    case EMtoUConnectionState::NotReady:
        OutSeverity = ESeverity::Warning;
        return LOCTEXT("ConnectionNotReady", "未连接（预览未就绪）");
    case EMtoUConnectionState::Partial:
        OutSeverity = ESeverity::Warning;
        return LOCTEXT("ConnectionPartial", "已连接（部分 Morph）");
    case EMtoUConnectionState::BoneOnly:
        OutSeverity = ESeverity::Warning;
        return LOCTEXT("ConnectionBoneOnly", "已连接（仅骨骼）");
    case EMtoUConnectionState::Connected:
        OutSeverity = ESeverity::Success;
        return LOCTEXT("ConnectionConnected", "已连接");
    case EMtoUConnectionState::Error:
        OutSeverity = ESeverity::Error;
        return LOCTEXT("ConnectionError", "连接出错");
    case EMtoUConnectionState::Unknown:
    default:
        OutSeverity = ESeverity::Neutral;
        return Status.TrimStartAndEnd().IsEmpty()
            ? LOCTEXT("ConnectionIdle", "未连接")
            : LOCTEXT("ConnectionUnknown", "连接状态未知");
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

/**
 * The one ordered problem list the status summary and the next step both
 * follow, from the current blocker down to the link's own progress. A
 * connection that never reached streaming outranks the Preview's non-blocking
 * quality warning, and both texts are read off this single decision.
 */
enum class EStatusIssue : uint8
{
    /** The Generated Preview could not be built. */
    PreviewFailed,
    /** The link reported a blocking failure instead of streaming. */
    ConnectionBlocked,
    /** The Preview streams, but carries quality warnings. */
    QualityWarning,
    /** A Preview build is in flight. */
    Refreshing,
    /** Inputs changed since the usable Preview. */
    StalePreview,
    /** No usable Preview yet. */
    UnconfiguredPreview,
    /** Nothing is wrong; only the link's own phase is left. */
    LinkValidating,
    LinkBoneOnly,
    LinkPartial,
    LinkConnected,
    /** No link activity and no work in flight. */
    Idle
};

EStatusIssue ClassifyIssue(EMtoUConnectionState Connection, const FMtoUPreviewReadiness& Readiness)
{
    if (Readiness.State == EMtoUPreviewState::Error)
    {
        return EStatusIssue::PreviewFailed;
    }
    if (Connection == EMtoUConnectionState::Error)
    {
        return EStatusIssue::ConnectionBlocked;
    }
    switch (Readiness.State)
    {
    case EMtoUPreviewState::Building:
        return EStatusIssue::Refreshing;
    case EMtoUPreviewState::Warning:
        return EStatusIssue::QualityWarning;
    case EMtoUPreviewState::Dirty:
        return EStatusIssue::StalePreview;
    case EMtoUPreviewState::None:
        return EStatusIssue::UnconfiguredPreview;
    case EMtoUPreviewState::Ready:
    case EMtoUPreviewState::Error:
    default:
        break;
    }
    switch (Connection)
    {
    case EMtoUConnectionState::Validating:
        return EStatusIssue::LinkValidating;
    case EMtoUConnectionState::BoneOnly:
        return EStatusIssue::LinkBoneOnly;
    case EMtoUConnectionState::Partial:
        return EStatusIssue::LinkPartial;
    case EMtoUConnectionState::Connected:
        return EStatusIssue::LinkConnected;
    default:
        return EStatusIssue::Idle;
    }
}

FText NextStepText(EStatusIssue Issue, const FMtoUPreviewReadiness& Readiness)
{
    switch (Issue)
    {
    case EStatusIssue::PreviewFailed:
        if (Readiness.Diagnostics.Contains(TEXT("two indistinguishable source regions")))
        {
            return LOCTEXT("NextStepAmbiguous", "检查候选区段；确认目标材质槽后重新刷新预览。");
        }
        return LOCTEXT("NextStepRefreshFailed", "查看错误详情，修正输入后重新刷新预览。");
    case EStatusIssue::ConnectionBlocked:
        return LOCTEXT("NextStepConnectionError", "检查连接诊断，然后在 Maya 中重新连接。");
    case EStatusIssue::QualityWarning:
        return LOCTEXT("NextStepQualityWarning", "预览可用；请先检查高级诊断中的质量警告。");
    case EStatusIssue::Refreshing:
        return LOCTEXT("NextStepBuilding", "正在刷新预览，请等待结果。");
    case EStatusIssue::StalePreview:
        return LOCTEXT("NextStepDirty", "输入已改变，请刷新预览。");
    case EStatusIssue::UnconfiguredPreview:
        return LOCTEXT("NextStepNone", "配置绑定资产中的主体和预览网格，然后刷新预览。");
    case EStatusIssue::LinkValidating:
        return LOCTEXT("NextStepValidating", "正在建立连接，请等待结果。");
    case EStatusIssue::LinkBoneOnly:
        return LOCTEXT("NextStepBoneOnly", "如需 Morph，请在 Maya 中启用传递 BS 后重新连接。");
    case EStatusIssue::LinkPartial:
        return LOCTEXT("NextStepPartial", "检查模型诊断中的 Morph 覆盖情况。");
    case EStatusIssue::LinkConnected:
        return LOCTEXT("NextStepConnected", "预览跟随当前 Maya 会话；修改输入后请重新刷新。");
    case EStatusIssue::Idle:
    default:
        return LOCTEXT("NextStepReady", "如需实时更新，请在 Maya 中连接模型工作流。");
    }
}

FText DisplayText(EMtoUDisplayTarget Target)
{
    switch (Target)
    {
    case EMtoUDisplayTarget::GeneratedPreview:
        return LOCTEXT("DisplayGeneratedPreview", "当前显示：生成的预览网格");
    case EMtoUDisplayTarget::OriginalGarment:
        return LOCTEXT("DisplayOriginalGarment", "当前显示：原始服装与角色其他部分");
    case EMtoUDisplayTarget::Driver:
        return LOCTEXT("DisplayDriver", "当前显示：原始主体网格（Driver，非模型对比）");
    case EMtoUDisplayTarget::Hidden:
    default:
        return LOCTEXT("DisplayHidden", "当前未显示网格");
    }
}

FText RefreshPreviewTooltip(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return Actor.IsValid()
        ? LOCTEXT("RefreshPreviewTooltip",
            "根据当前绑定输入重新生成预览；会结束当前会话并显示新网格。")
        : LOCTEXT("RefreshPreviewUnavailable",
            "选择绑定 Actor 后刷新预览。");
}

FText DeletePreviewTooltip(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return FMtoULiveLinkActorDetails::CanDeletePreview(Actor)
        ? LOCTEXT("DeletePreviewTooltip",
            "删除生成的预览，改为显示主体网格。")
        : LOCTEXT("DeletePreviewUnavailable",
            "生成可用预览后才能删除。");
}

FText GarmentComparisonTooltip(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    const AMtoULiveLinkActor* Target = Actor.Get();
    if (!Target)
    {
        return LOCTEXT("ComparisonNoActor", "选择绑定 Actor 后比较服装。");
    }
    const FMtoUPreviewReadiness Readiness = Target->GetPreviewReadiness();
    if (!Readiness.IsUsable())
    {
        switch (Readiness.State)
        {
        case EMtoUPreviewState::Error:
            return LOCTEXT("ComparisonFailed", "预览生成失败；查看错误详情，修正输入后刷新预览。");
        case EMtoUPreviewState::Dirty:
            return LOCTEXT("ComparisonDirty", "当前输入尚无可用预览；先刷新预览。");
        case EMtoUPreviewState::Building:
            return LOCTEXT("ComparisonBuilding", "正在生成预览，请等待完成。");
        default:
            return LOCTEXT("ComparisonUnconfigured", "配置主体和预览网格，再刷新预览。");
        }
    }
    if (!Target->CanCompareGarments())
    {
        return LOCTEXT("ComparisonWrongDisplay", "先显示当前生成预览；动画工作流中的原始主体不属于模型对比。");
    }
    return Target->GetDisplayTarget() == EMtoUDisplayTarget::OriginalGarment
        ? LOCTEXT("ComparisonShowGenerated", "显示生成服装；保持当前会话、姿势和角色部件。")
        : LOCTEXT("ComparisonShowOriginal", "显示原始服装；保持当前会话、姿势和角色部件。");
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
    const FText ConnectionText = ConnectionHeadline(
        Connection, ConnectionStatus, Target.GetLinkSessionState(), ConnectionSeverity);

    View.Preview = ReadinessText;
    View.Connection = ConnectionText;
    View.PreviewSeverity = ReadinessSeverity;
    View.ConnectionSeverity = ConnectionSeverity;
    View.State = FText::Format(LOCTEXT("StatusHeadline", "{0} · {1}"), ReadinessText, ConnectionText);
    View.Severity = ReadinessSeverity > ConnectionSeverity ? ReadinessSeverity : ConnectionSeverity;
    // The summary and the next step read the same ordered issue, so the first
    // line and the detailed group can never name different problems.
    const EStatusIssue Issue = ClassifyIssue(Connection, Readiness);
    View.NextStep = NextStepText(Issue, Readiness);
    View.Display = DisplayText(Target.GetDisplayTarget());

    switch (Issue)
    {
    case EStatusIssue::PreviewFailed:
        if (Readiness.Diagnostics.Contains(TEXT("Automatic garment resolution found two indistinguishable source regions")))
        {
            View.Summary = LOCTEXT("AmbiguousGarmentSummary", "预览生成失败：服装区域匹配冲突");
            View.Cause = LOCTEXT("AmbiguousGarmentCause", "自动识别发现两个无法可靠区分的候选区域，预览未生成。");
            const FRegexPattern RegionPattern(TEXT("\\[([0-9]+) tris in sections? ([0-9/]+)\\]"));
            FRegexMatcher Matcher(RegionPattern, Readiness.Diagnostics);
            TArray<FString> CandidateLines;
            while (Matcher.FindNext())
            {
                CandidateLines.Add(FText::Format(
                    LOCTEXT("GarmentCandidate", "区段 {0}：{1} 个三角面"),
                    FText::FromString(Matcher.GetCaptureGroup(2)),
                    FText::FromString(Matcher.GetCaptureGroup(1))).ToString());
            }
            View.Candidates = FText::FromString(FString::Join(CandidateLines, TEXT("\n")));
        }
        else
        {
            View.Summary = LOCTEXT("UnknownPreviewFailure", "预览生成失败：请查看错误详情");
            View.Cause = LOCTEXT("UnknownPreviewCause", "当前输入未能生成预览；原始诊断保留在下方。");
        }
        break;
    case EStatusIssue::ConnectionBlocked:
        View.Summary = LOCTEXT("ConnectionFailureSummary", "连接出错：请查看诊断");
        break;
    case EStatusIssue::QualityWarning:
        View.Summary = LOCTEXT("PreviewWarningSummary", "预览已就绪，但存在质量警告");
        break;
    default:
        break;
    }

    // The raw source strings are collected once. Summary and diagnostics often
    // carry the exact same failure, so showing both would duplicate the error.
    TArray<FString> RawMessages;
    auto AddRaw = [&RawMessages](const FString& Message)
    {
        const FString Trimmed = Message.TrimStartAndEnd();
        if (!Trimmed.IsEmpty() && !RawMessages.Contains(Trimmed))
        {
            RawMessages.Add(Trimmed);
        }
    };
    AddRaw(Readiness.Diagnostics);
    if (Readiness.Summary != Readiness.Diagnostics)
    {
        AddRaw(Readiness.Summary);
    }
    AddRaw(Target.GetCharacterPartDiagnostics());
    AddRaw(Target.GetModelDiagnostics());
    if (ConnectionNeedsDetail(ConnectionStatus))
    {
        AddRaw(ConnectionStatus);
    }
    View.RawDiagnostics = FText::FromString(FString::Join(RawMessages, TEXT("\n\n")));
    return View;
}
}

TSharedRef<IDetailCustomization> FMtoULiveLinkActorDetails::MakeInstance()
{
    return MakeShared<FMtoULiveLinkActorDetails>();
}

FText FMtoULiveLinkActorDetails::FormatCacheSummary(const FMtoUCachePlaybackView& View)
{
    return CacheSummaryText(View);
}

FText FMtoULiveLinkActorDetails::FormatCacheState(const FMtoUCachePlaybackView& View)
{
    return CacheStateText(View);
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

    // Curated rows own all status text. Keep the serialized property intact.
    DetailBuilder.HideProperty(FName(TEXT("ConnectionStatus")));
    const TSharedRef<FStatusPresenter> Status = MakeShared<FStatusPresenter>(Actor);

    IDetailCategoryBuilder& Runtime = DetailBuilder.EditCategory(
        "MtoU Runtime", LOCTEXT("RuntimeCategory", "MtoU · 运行状态"),
        ECategoryPriority::Important);
    Runtime.InitiallyCollapsed(false);
    Runtime.AddCustomRow(LOCTEXT("RuntimeFilter", "预览 连接 状态"))
        .WholeRowContent()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity_Lambda([Status]() { return SeverityColor(Status->Get().PreviewSeverity); })
                .Text_Lambda([Status]()
                {
                    return FText::Format(LOCTEXT("PreviewStateLine", "预览：{0}"), Status->Get().Preview);
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity_Lambda([Status]() { return SeverityColor(Status->Get().ConnectionSeverity); })
                .Text_Lambda([Status]()
                {
                    return FText::Format(LOCTEXT("ConnectionStateLine", "连接：{0}"), Status->Get().Connection);
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity_Lambda([Status]() { return SeverityColor(Status->Get().Severity); })
                .Text_Lambda([Status]() { return Status->Get().Summary; })
                .Visibility_Lambda([Status]()
                {
                    return Status->Get().Summary.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                .Text_Lambda([Status]() { return Status->Get().NextStep; })
            ]
        ];

    IDetailCategoryBuilder& Character = DetailBuilder.EditCategory(
        "MtoU Character", LOCTEXT("CharacterCategory", "角色组成"),
        ECategoryPriority::Important);
    Character.InitiallyCollapsed(false);
    Character.AddCustomRow(LOCTEXT("PrimaryFilter", "主体"))
        .WholeRowContent()
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text_Lambda([Actor]()
            {
                const UMtoULiveLinkBinding* Binding = Actor.IsValid() ? Actor->GetBinding() : nullptr;
                const USkeletalMesh* Mesh = Binding ? Binding->SkeletalMesh.Get() : nullptr;
                return FText::Format(LOCTEXT("PrimaryValue", "主体：{0}"),
                    Mesh ? FText::FromString(Mesh->GetName()) : LOCTEXT("NoPrimary", "未指定"));
            })
            .ToolTipText_Lambda([Actor]()
            {
                const UMtoULiveLinkBinding* Binding = Actor.IsValid() ? Actor->GetBinding() : nullptr;
                const USkeletalMesh* Mesh = Binding ? Binding->SkeletalMesh.Get() : nullptr;
                return Mesh ? FText::FromString(Mesh->GetPathName()) : FText::GetEmpty();
            })
        ];
    Character.AddCustomRow(LOCTEXT("AdditionalPartsFilter", "附加部件"))
        .WholeRowContent()
        [
            SNew(SExpandableArea)
            .InitiallyCollapsed(true)
            .HeaderContent()
            [
                SNew(STextBlock)
                .Text_Lambda([Actor]()
                {
                    const UMtoULiveLinkBinding* Binding = Actor.IsValid() ? Actor->GetBinding() : nullptr;
                    return FText::Format(LOCTEXT("AdditionalPartsCount", "附加部件：{0} 个"),
                        FText::AsNumber(Binding ? Binding->AdditionalParts.Num() : 0));
                })
            ]
            .BodyContent()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Actor]()
                {
                    const UMtoULiveLinkBinding* Binding = Actor.IsValid() ? Actor->GetBinding() : nullptr;
                    if (!Binding || Binding->AdditionalParts.IsEmpty())
                    {
                        return LOCTEXT("NoAdditionalParts", "无附加部件");
                    }
                    TArray<FString> Lines;
                    for (const FMtoUCharacterPart& Part : Binding->AdditionalParts)
                    {
                        // The asset name stays verbatim; only the two states the
                        // panel adds are user-facing text.
                        const FText Name = Part.SkeletalMesh
                            ? FText::FromString(Part.SkeletalMesh->GetName())
                            : LOCTEXT("NoPartMesh", "未指定网格");
                        Lines.Add(Part.bEnabled
                            ? Name.ToString()
                            : FText::Format(LOCTEXT("PartDisabled", "{0}（已停用）"), Name).ToString());
                    }
                    return FText::FromString(FString::Join(Lines, TEXT("\n")));
                })
                .ToolTipText_Lambda([Actor]()
                {
                    const UMtoULiveLinkBinding* Binding = Actor.IsValid() ? Actor->GetBinding() : nullptr;
                    TArray<FString> Paths;
                    if (Binding)
                    {
                        for (const FMtoUCharacterPart& Part : Binding->AdditionalParts)
                        {
                            if (Part.SkeletalMesh)
                            {
                                Paths.Add(Part.SkeletalMesh->GetPathName());
                            }
                        }
                    }
                    return FText::FromString(FString::Join(Paths, TEXT("\n")));
                })
            ]
        ];

    IDetailCategoryBuilder& Controls = DetailBuilder.EditCategory(
        "MtoU Preview Controls", LOCTEXT("ControlsCategory", "预览控制"),
        ECategoryPriority::Important);
    Controls.InitiallyCollapsed(false);
    Controls.AddProperty(DetailBuilder.GetProperty(FName(TEXT("Binding"))))
        .DisplayName(LOCTEXT("BindingAsset", "绑定资产"));
    Controls.AddProperty(DetailBuilder.GetProperty(FName(TEXT("GeneratedPreviewMesh"))))
        .DisplayName(LOCTEXT("GeneratedPreviewMesh", "生成的预览网格"));
    Controls.AddCustomRow(LOCTEXT("DisplayFilter", "当前显示"))
        .WholeRowContent()
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            .Text_Lambda([Status]() { return Status->Get().Display; })
        ];
    Controls.AddCustomRow(LOCTEXT("GarmentComparisonFilter", "服装对比"))
        .WholeRowContent()
        [
            SNew(SButton)
            .Text_Lambda([Actor]()
            {
                return Actor.IsValid()
                    && Actor->GetDisplayTarget() == EMtoUDisplayTarget::OriginalGarment
                    ? LOCTEXT("ShowGeneratedGarment", "显示生成服装")
                    : LOCTEXT("ShowOriginalGarment", "显示原始服装");
            })
            .ToolTipText_Lambda([Actor]() { return GarmentComparisonTooltip(Actor); })
            .IsEnabled_Lambda([Actor]() { return Actor.IsValid() && Actor->CanCompareGarments(); })
            .OnClicked_Lambda([Actor]() { return HandleGarmentComparisonClicked(Actor); })
        ];
    Controls.AddCustomRow(LOCTEXT("PreviewActionsFilter", "预览操作"))
        .WholeRowContent()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("RefreshPreview", "刷新预览"))
                .ToolTipText_Lambda([Actor]() { return RefreshPreviewTooltip(Actor); })
                .IsEnabled_Lambda([Actor]() { return Actor.IsValid(); })
                .OnClicked_Lambda([Actor]() { return HandleRefreshPreviewClicked(Actor); })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("DeletePreview", "删除预览"))
                .ToolTipText_Lambda([Actor]() { return DeletePreviewTooltip(Actor); })
                .IsEnabled_Lambda([Actor]() { return CanDeletePreview(Actor); })
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

    // The seek box and its prompt live as long as the customized widget row;
    // the prompt only reports refusals in the user's terms, the session stays
    // the authority that accepted or refused the frame.
    const TSharedPtr<FString> SeekInput = MakeShared<FString>();
    const TSharedPtr<FText> SeekMessage = MakeShared<FText>();
    Controls.AddCustomRow(LOCTEXT("CacheStatusFilter", "缓存播放 帧范围 状态"))
        .WholeRowContent()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Actor]()
                {
                    const FMtoUCachePlaybackView View = Actor.IsValid()
                        ? Actor->GetCachePlaybackView() : FMtoUCachePlaybackView();
                    return FText::Format(LOCTEXT("CacheStateLine", "缓存播放：{0}"),
                        CacheStateText(View));
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                .Text_Lambda([Actor]()
                {
                    return FMtoULiveLinkActorDetails::FormatCacheSummary(Actor.IsValid()
                        ? Actor->GetCachePlaybackView() : FMtoUCachePlaybackView());
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text_Lambda([Actor]()
                    {
                        const FMtoUCachePlaybackView View = Actor.IsValid()
                            ? Actor->GetCachePlaybackView() : FMtoUCachePlaybackView();
                        return View.State == EMtoUCacheState::Ready
                            ? LOCTEXT("CachePlay", "播放")
                            : LOCTEXT("CachePlayAgain", "再次播放");
                    })
                    .ToolTipText(LOCTEXT("CachePlayTip", "仅完整验证且匹配当前连接的缓存可播放；未就绪时请先在 Maya 捕获并上传。"))
                    .IsEnabled_Lambda([Actor]()
                    {
                        return Actor.IsValid() && Actor->GetCachePlaybackView().CanPlay();
                    })
                    .OnClicked_Lambda([Actor, SeekMessage]()
                    {
                        if (AMtoULiveLinkActor* Target = Actor.Get())
                        {
                            *SeekMessage = FText::GetEmpty();
                            Target->StartCachedPlayback();
                        }
                        return FReply::Handled();
                    })
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("CachePause", "暂停"))
                    .ToolTipText(LOCTEXT("CachePauseTip", "保留当前姿势与缓存；等待任意时长都不会触发播放性能失败。"))
                    .IsEnabled_Lambda([Actor]()
                    {
                        return Actor.IsValid() && Actor->GetCachePlaybackView().CanPause();
                    })
                    .OnClicked_Lambda([Actor, SeekMessage]()
                    {
                        if (AMtoULiveLinkActor* Target = Actor.Get())
                        {
                            *SeekMessage = FText::GetEmpty();
                            Target->PauseCachedPlayback();
                        }
                        return FReply::Handled();
                    })
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("CacheResume", "继续"))
                    .ToolTipText(LOCTEXT("CacheResumeTip", "从当前位置继续，并重建计时基准；暂停期间的帧不会补发。"))
                    .IsEnabled_Lambda([Actor]()
                    {
                        return Actor.IsValid() && Actor->GetCachePlaybackView().CanResume();
                    })
                    .OnClicked_Lambda([Actor, SeekMessage]()
                    {
                        if (AMtoULiveLinkActor* Target = Actor.Get())
                        {
                            *SeekMessage = FText::GetEmpty();
                            Target->ResumeCachedPlayback();
                        }
                        return FReply::Handled();
                    })
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("CacheStop", "停止"))
                    .ToolTipText(LOCTEXT("CacheStopTip", "停止播放或暂停并保留完整缓存及最后已应用姿势。"))
                    .IsEnabled_Lambda([Actor]()
                    {
                        return Actor.IsValid() && Actor->GetCachePlaybackView().CanStop();
                    })
                    .OnClicked_Lambda([Actor, SeekMessage]()
                    {
                        if (AMtoULiveLinkActor* Target = Actor.Get())
                        {
                            *SeekMessage = FText::GetEmpty();
                            Target->StopCachedPlayback();
                        }
                        return FReply::Handled();
                    })
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SCheckBox)
                    .Content()
                    [
                        SNew(STextBlock).Text(LOCTEXT("CacheLoop", "循环"))
                    ]
                    .ToolTipText(LOCTEXT("CacheLoopTip", "循环在最后一帧被接受后从首帧继续；关闭后完成当前轮。"))
                    .IsEnabled_Lambda([Actor]()
                    {
                        return Actor.IsValid() && Actor->GetCachePlaybackView().CanSeek();
                    })
                    .IsChecked_Lambda([Actor]()
                    {
                        return (Actor.IsValid() && Actor->GetCachePlaybackView().bLoopEnabled)
                            ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([Actor, SeekMessage](ECheckBoxState State)
                    {
                        if (AMtoULiveLinkActor* Target = Actor.Get())
                        {
                            *SeekMessage = FText::GetEmpty();
                            Target->SetCachedLoopEnabled(State == ECheckBoxState::Checked);
                        }
                    })
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SBox)
                    .WidthOverride(90.0f)
                    [
                        SNew(SEditableTextBox)
                        .HintText(LOCTEXT("CacheSeekHint", "Maya 源帧"))
                        .Text_Lambda([SeekInput]() { return FText::FromString(*SeekInput); })
                        .OnTextChanged_Lambda([SeekInput](const FText& NewText)
                        {
                            *SeekInput = NewText.ToString();
                        })
                        .IsEnabled_Lambda([Actor]()
                        {
                            return Actor.IsValid() && Actor->GetCachePlaybackView().CanSeek();
                        })
                    ]
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("CacheSeek", "定位"))
                    .ToolTipText(LOCTEXT("CacheSeekTip", "显示指定 Maya 源帧后进入暂停；只接受本缓存已采样的帧。"))
                    .IsEnabled_Lambda([Actor]()
                    {
                        return Actor.IsValid() && Actor->GetCachePlaybackView().CanSeek();
                    })
                    .OnClicked_Lambda([Actor, SeekInput, SeekMessage]()
                    {
                        AMtoULiveLinkActor* Target = Actor.Get();
                        if (!Target)
                        {
                            return FReply::Handled();
                        }
                        const FString Text = SeekInput->TrimStartAndEnd();
                        int32 SourceFrame = 0;
                        if (Text.IsEmpty() || !LexTryParseString(SourceFrame, *Text))
                        {
                            *SeekMessage = LOCTEXT("CacheSeekNotInteger", "请输入整数 Maya 源帧。");
                            return FReply::Handled();
                        }
                        *SeekMessage = FText::GetEmpty();
                        if (!Target->SeekCachedPlayback(SourceFrame))
                        {
                            const FMtoUCachePlaybackView Refused = Target->GetCachePlaybackView();
                            *SeekMessage = FText::Format(
                                LOCTEXT("CacheSeekRefused",
                                    "无法定位到源帧 {0}；当前缓存只采样了源帧 {1}–{2}。"),
                                FText::FromString(FString::FromInt(SourceFrame)),
                                FText::FromString(FString::FromInt(Refused.StartFrame)),
                                FText::FromString(FString::FromInt(Refused.EndFrame)));
                        }
                        return FReply::Handled();
                    })
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.35f, 0.25f)))
                .Text_Lambda([Actor, SeekMessage]()
                {
                    return SeekMessage->IsEmpty()
                        ? FText::FromString(Actor.IsValid()
                            ? Actor->GetCachePlaybackView().ErrorDetails : FString())
                        : *SeekMessage;
                })
                .Visibility_Lambda([Actor, SeekMessage]()
                {
                    return !SeekMessage->IsEmpty()
                        || (Actor.IsValid() && !Actor->GetCachePlaybackView().ErrorDetails.IsEmpty())
                        ? EVisibility::Visible : EVisibility::Collapsed;
                })
            ]
        ];

    IDetailCategoryBuilder& Advanced = DetailBuilder.EditCategory(
        "MtoU Advanced", LOCTEXT("AdvancedCategory", "高级设置与诊断"),
        ECategoryPriority::Default);
    Advanced.InitiallyCollapsed(true);
    Advanced.AddCustomRow(LOCTEXT("GarmentOverrideFilter", "服装材质槽覆盖"))
        .WholeRowContent()
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text_Lambda([Actor]()
            {
                const UMtoULiveLinkBinding* Binding = Actor.IsValid() ? Actor->GetBinding() : nullptr;
                if (!Binding)
                {
                    return LOCTEXT("OverrideNeedsBinding", "服装材质槽覆盖：请先指定绑定资产。");
                }
                if (Binding->DriverGarmentSlotOverride.IsEmpty())
                {
                    return LOCTEXT("OverrideAutomatic", "服装材质槽覆盖：自动识别。可在绑定资产的 Driver Garment Slot Override 中指定已确认的材质槽。");
                }
                TArray<FString> Slots;
                for (const FName Slot : Binding->DriverGarmentSlotOverride)
                {
                    Slots.Add(Slot.ToString());
                }
                return FText::Format(LOCTEXT("OverrideManual", "服装材质槽覆盖：{0}（在绑定资产中编辑）"),
                    FText::FromString(FString::Join(Slots, TEXT(", "))));
            })
        ];
    Advanced.AddCustomRow(LOCTEXT("DiagnosticsFilter", "错误详情 原始诊断"))
        .WholeRowContent()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Status]() { return Status->Get().Summary; })
                .Visibility_Lambda([Status]()
                {
                    return Status->Get().Summary.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Status]() { return Status->Get().Cause; })
                .Visibility_Lambda([Status]()
                {
                    return Status->Get().Cause.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Status]() { return Status->Get().Candidates; })
                .Visibility_Lambda([Status]()
                {
                    return Status->Get().Candidates.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Status]()
                {
                    return FText::Format(LOCTEXT("AdvancedNextStep", "下一步：{0}"),
                        Status->Get().NextStep);
                })
                .Visibility_Lambda([Status]()
                {
                    return Status->Get().Summary.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f)
            [
                SNew(SBox)
                .MaxDesiredHeight(180.0f)
                .Visibility_Lambda([Status]()
                {
                    return Status->Get().RawDiagnostics.IsEmpty()
                        ? EVisibility::Collapsed : EVisibility::Visible;
                })
                [
                    SNew(SMultiLineEditableTextBox)
                    .IsReadOnly(true)
                    .AutoWrapText(true)
                    .Text_Lambda([Status]() { return Status->Get().RawDiagnostics; })
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
            [
                SNew(SButton)
                .HAlign(HAlign_Center)
                .Text(LOCTEXT("CopyDiagnostics", "复制完整诊断"))
                .Visibility_Lambda([Status]()
                {
                    return Status->Get().RawDiagnostics.IsEmpty()
                        ? EVisibility::Collapsed : EVisibility::Visible;
                })
                .OnClicked_Lambda([Status]()
                {
                    FPlatformApplicationMisc::ClipboardCopy(*Status->Get().RawDiagnostics.ToString());
                    return FReply::Handled();
                })
            ]
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
    View.Preview = LOCTEXT("StatusNoActor", "未选择绑定 Actor");
    View.Connection = LOCTEXT("ConnectionIdle", "未连接");
    View.State = View.Preview;
    View.NextStep = LOCTEXT("NextStepNoActor",
        "选择绑定 Actor 后刷新或删除预览。");
    return View;
}

FMtoULiveLinkActorDetails::FStatusView FMtoULiveLinkActorDetails::MakeStatusViewForReadiness(
    TWeakObjectPtr<AMtoULiveLinkActor> Actor, const FMtoUPreviewReadiness& Readiness)
{
    return Actor.IsValid() ? BuildStatusView(*Actor.Get(), Readiness)
        : MakeStatusView(Actor);
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
    const EMtoULinkSessionState Session = Target->GetLinkSessionState();
    if (bHasView
        && CachedKey.bHasActor
        && CachedKey.State == Readiness.State
        && CachedKey.Stage == Readiness.Stage
        && CachedKey.Display == Display
        && CachedKey.Session == Session
        && CachedKey.PreviewDiagnostics == Readiness.Diagnostics
        && CachedKey.PreviewSummary == Readiness.Summary
        && CachedKey.Connection == Connection
        && CachedKey.ModelDiagnostics == Target->GetModelDiagnostics()
        && CachedKey.CharacterDiagnostics == Target->GetCharacterPartDiagnostics())
    {
        return View;
    }

    View = BuildStatusView(*Target, Readiness);
    CachedKey.bHasActor = true;
    CachedKey.State = Readiness.State;
    CachedKey.Stage = Readiness.Stage;
    CachedKey.PreviewDiagnostics = Readiness.Diagnostics;
    CachedKey.PreviewSummary = Readiness.Summary;
    CachedKey.Display = Display;
    CachedKey.Session = Session;
    CachedKey.Connection = Connection;
    CachedKey.ModelDiagnostics = Target->GetModelDiagnostics();
    CachedKey.CharacterDiagnostics = Target->GetCharacterPartDiagnostics();
    bHasView = true;
    return View;
}

bool FMtoULiveLinkActorDetails::CanDeletePreview(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return Actor.IsValid() && Actor->GetPreviewReadiness().IsUsable();
}

FReply FMtoULiveLinkActorDetails::HandleGarmentComparisonClicked(
    TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    if (AMtoULiveLinkActor* Target = Actor.Get())
    {
        if (Target->GetDisplayTarget() == EMtoUDisplayTarget::OriginalGarment)
        {
            Target->RestoreGeneratedGarment();
        }
        else
        {
            Target->ShowOriginalGarment();
        }
    }
    return FReply::Handled();
}

FReply FMtoULiveLinkActorDetails::HandleRefreshPreviewClicked(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    if (AMtoULiveLinkActor* Target = Actor.Get())
    {
        FScopedSlowTask Progress(5.0f, LOCTEXT(
            "PreparingPreview", "正在生成预览网格"));
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
