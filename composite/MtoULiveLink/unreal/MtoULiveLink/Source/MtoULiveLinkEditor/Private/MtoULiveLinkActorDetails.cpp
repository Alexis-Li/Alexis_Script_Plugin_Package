#include "MtoULiveLinkActorDetails.h"

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"
#include "MtoULiveLinkPreview.h"

#include "MtoULinkage.h"
#include "MtoULinkageEditor.h"
#include "MtoUSceneRefExport.h"

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
        return LOCTEXT("DisplayDriver", "当前显示：原始角色");
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
        return LOCTEXT("ComparisonWrongDisplay", "先显示当前生成预览；动画工作流显示的原始角色不参与服装对比。");
    }
    return Target->GetDisplayTarget() == EMtoUDisplayTarget::OriginalGarment
        ? LOCTEXT("ComparisonShowGenerated", "显示生成服装；保持当前会话、姿势和角色部件。")
        : LOCTEXT("ComparisonShowOriginal", "显示原始服装；保持当前会话、姿势和角色部件。");
}

// Longest run without a space one line shows; the row wraps only at spaces,
// so an object path's '/' is no break. An eval_id carries the
// whole camera description and runs to kilobytes; the row keeps both ends of
// each long run and its tooltip keeps the full text.
constexpr int32 LinkageRunLimit = 52;

FText CompactLinkageRuns(const FText& Text)
{
    const FString Full = Text.ToString();
    FString Out;
    Out.Reserve(FMath::Min(Full.Len(), 512));
    int32 Start = 0;
    while (Start < Full.Len())
    {
        int32 End = Start;
        while (End < Full.Len() && Full[End] != TEXT(' ') && Full[End] != TEXT('\n'))
        {
            ++End;
        }
        const int32 Length = End - Start;
        if (Length > LinkageRunLimit)
        {
            const int32 Tail = LinkageRunLimit / 4;
            Out += Full.Mid(Start, LinkageRunLimit - Tail - 1);
            Out += TEXT("…");
            Out += Full.Mid(End - Tail, Tail);
        }
        else
        {
            Out += Full.Mid(Start, Length);
        }
        if (End < Full.Len())
        {
            Out.AppendChar(Full[End]);
        }
        Start = End + 1;
    }
    return FText::FromString(Out);
}

// The applied lag the panel reports: below it Maya keeps up and the lag row
// stays hidden. Display only; the protocol's convergence window is unchanged.
constexpr double LinkageLagNoticeSeconds = 0.5;

/** True while Maya does not keep up with the Level Sequence time. */
bool LinkageLagging(const FMtoULinkageView& View)
{
    if (View.Mode != TEXT("linkage") || View.bTransitioning || View.bConverged)
    {
        return false;
    }
    // A target waiting past the convergence window, or a slow but complete
    // application, both mean Maya is behind.
    return View.TargetOverdueSeconds > 0.0
        || (!View.AppliedEvalId.IsEmpty() && View.AppliedLagSeconds >= LinkageLagNoticeSeconds);
}

/** The lag in effect: an overdue target has waited longer than the last application took. */
double LinkageBehindSeconds(const FMtoULinkageView& View)
{
    return FMath::Max(View.TargetOverdueSeconds,
        View.AppliedEvalId.IsEmpty() ? 0.0 : View.AppliedLagSeconds);
}

FText LinkageSecondsText(double Seconds)
{
    FNumberFormattingOptions Options;
    Options.MinimumFractionalDigits = Seconds < 10.0 ? 1 : 0;
    Options.MaximumFractionalDigits = Seconds < 10.0 ? 1 : 0;
    return FText::AsNumber(Seconds, &Options);
}

FText LinkageStatusText(const FMtoULinkageView& View)
{
    if (View.bTransitioning)
    {
        return LOCTEXT("LinkageStatusTransitioning", "正在切换镜头联动，请稍候");
    }
    if (View.Mode == TEXT("linkage"))
    {
        if (LinkageLagging(View))
        {
            return LOCTEXT("LinkageStatusLagging", "镜头联动中 · Unreal 控制时间 · Maya 跟不上");
        }
        return View.AppliedEvalId.IsEmpty()
            ? LOCTEXT("LinkageStatusSyncing", "镜头联动中 · Unreal 控制时间 · 正在同步")
            : LOCTEXT("LinkageStatusSynced", "镜头联动中 · Unreal 控制时间 · 同步正常");
    }
    if (View.Mode == TEXT("cached"))
    {
        return LOCTEXT("LinkageStatusCached", "未进入联动 · 缓存播放中");
    }
    return View.bTakeoverHeld
        ? LOCTEXT("LinkageStatusHeld", "未进入联动 · 仍接管目标，点击“结束预览”交还原动画")
        : LOCTEXT("LinkageStatusIdle", "未进入联动");
}

FText LinkageLagText(const FMtoULinkageView& View)
{
    if (!LinkageLagging(View))
    {
        return FText::GetEmpty();
    }
    return FText::Format(
        LOCTEXT("LinkageLagValue", "Maya 跟不上，滞后 {0} 秒。可暂停 Sequencer 等待 Maya 追上，或关闭占用性能的程序。"),
        LinkageSecondsText(LinkageBehindSeconds(View)));
}

FText LinkageRestoreText(const FMtoULinkageView& View)
{
    if (View.RecoveryResidual.IsEmpty())
    {
        return FText::GetEmpty();
    }
    return LOCTEXT("LinkageRestoreFailed",
        "恢复未完成：部分被接管的动画或轨道没有交还。请在“高级设置与诊断”查看未恢复的项目并在 Sequencer 中手动恢复，或重新打开关卡。");
}

FText LinkageErrorText(const FMtoULinkageView& View)
{
    if (View.Error.IsEmpty())
    {
        return FText::GetEmpty();
    }
    return View.ErrorCode.IsEmpty()
        ? FText::FromString(View.Error)
        : FText::Format(LOCTEXT("LinkageErrorWithCode", "{0}（{1}）"),
            FText::FromString(View.Error), FText::FromString(View.ErrorCode));
}

FText LinkageDiagnosticsText(const FMtoULinkageView& View)
{
    const bool bHasLinkage = View.bPaired || View.Mode != TEXT("realtime") || View.bTakeoverHeld
        || View.bTransitioning || View.Identity.ModeRevision > 0 || !View.TargetEvalId.IsEmpty()
        || !View.AppliedEvalId.IsEmpty() || !View.RecoveryResidual.IsEmpty()
        || !View.Error.IsEmpty() || View.RefusedGroupCount > 0;
    if (!bHasLinkage)
    {
        return FText::GetEmpty();
    }
    // Diagnostic lines keep the protocol's own names and exact values, so a
    // copied report matches both hosts' logs.
    TArray<FString> Lines;
    Lines.Add(TEXT("镜头联动诊断"));
    Lines.Add(FString::Printf(TEXT("控制方：%s · 模式：%s%s"), *View.Control, *View.Mode,
        View.bTransitioning ? TEXT(" · 切换中") : TEXT("")));
    Lines.Add(FString::Printf(TEXT("session %d · set_revision %d · space_revision %d · mode_revision %d"),
        View.Identity.SessionId, View.Identity.SetRevision, View.Identity.SpaceRevision,
        View.Identity.ModeRevision));
    Lines.Add(FString::Printf(TEXT("配对：%s · %s %s"),
        View.bPaired ? *View.PairedObjectId : TEXT("未配对"), *View.TargetKind, *View.TargetPath));
    for (const FMtoULinkageObjectView& Object : View.Objects)
    {
        Lines.Add(FString::Printf(TEXT("对象 %s：%s · %s%s"),
            *Object.ObjectId,
            Object.bPaired ? *Object.TargetPath : TEXT("未配对"),
            Object.bNegotiated ? TEXT("已协商") : TEXT("未协商"),
            Object.ObjectId == TEXT("secondary") && View.bRemovingObject ? TEXT(" · 移除中") : TEXT("")));
    }
    Lines.Add(FString::Printf(TEXT("目标时间：%s · 帧 %s"),
        View.TargetEvalId.IsEmpty() ? TEXT("—") : *View.TargetEvalId,
        *FString::SanitizeFloat(View.TargetDisplayFrame)));
    Lines.Add(FString::Printf(TEXT("最新完整应用时间：%s · 帧 %s · 样本 %d"),
        View.AppliedEvalId.IsEmpty() ? TEXT("—") : *View.AppliedEvalId,
        *FString::SanitizeFloat(View.AppliedDisplayFrame), View.AppliedSampleSerial));
    Lines.Add(FString::Printf(TEXT("滞后：%s 秒（%s）· 目标超时等待 %s 秒"),
        *FString::SanitizeFloat(View.AppliedLagSeconds),
        View.bConverged ? TEXT("已同步") : TEXT("未收敛"),
        *FString::SanitizeFloat(View.TargetOverdueSeconds)));
    Lines.Add(FString::Printf(TEXT("恢复结果：%s"),
        !View.RecoveryResidual.IsEmpty() ? *View.RecoveryResidual
        : View.bWritersRestored ? TEXT("已恢复被接管的 writer") : TEXT("无待处理恢复")));
    Lines.Add(FString::Printf(TEXT("接管中：%s"), View.bTakeoverHeld ? TEXT("是") : TEXT("否")));
    Lines.Add(FString::Printf(TEXT("调度拒绝：%d%s"), View.RefusedGroupCount,
        View.RefusedCode.IsEmpty() ? TEXT("")
        : *FString::Printf(TEXT("（%s：%s）"), *View.RefusedCode, *View.RefusedMessage)));
    Lines.Add(FString::Printf(TEXT("详情：%s"), View.Detail.IsEmpty() ? TEXT("—") : *View.Detail));
    if (!View.Error.IsEmpty())
    {
        Lines.Add(FString::Printf(TEXT("错误：%s（%s）"), *View.Error, *View.ErrorCode));
    }
    return FText::FromString(FString::Join(Lines, TEXT("\n")));
}

FText LinkageEnterTooltip(TWeakObjectPtr<AMtoULiveLinkActor> Actor, const FMtoULinkageView& View)
{
    if (!Actor.IsValid())
    {
        return LOCTEXT("LinkageEnterNoActor", "选择绑定 Actor 后进入镜头联动。");
    }
    if (View.bTransitioning)
    {
        return LOCTEXT("LinkageEnterTransitioning", "正在切换镜头联动，请等待 Maya 确认。");
    }
    if (!View.bPaired)
    {
        return LOCTEXT("LinkageEnterUnpaired", "先点击“配对”，把这个角色与 Maya 配对。");
    }
    if (View.Mode == TEXT("linkage"))
    {
        return LOCTEXT("LinkageEnterAlready", "已在镜头联动中：Level Sequence 控制时间和相机。");
    }
    if (View.Mode == TEXT("cached"))
    {
        return LOCTEXT("LinkageEnterCached", "缓存播放中：先在 Maya 返回实时预览，再进入镜头联动。");
    }
    return LOCTEXT("LinkageEnterReady", "进入联动后，由 Level Sequence 控制时间和相机，Maya 跟随显示角色姿势。");
}

FText LinkageRealtimeTooltip(TWeakObjectPtr<AMtoULiveLinkActor> Actor, const FMtoULinkageView& View)
{
    if (!Actor.IsValid())
    {
        return LOCTEXT("LinkageRealtimeNoActor", "选择绑定 Actor 后返回实时。");
    }
    if (View.Mode != TEXT("linkage"))
    {
        return LOCTEXT("LinkageRealtimeWrongMode", "进入镜头联动后可返回实时。");
    }
    return LOCTEXT("LinkageRealtimeReady", "返回实时预览：Maya 恢复自己的时间和视图并暂停；目标仍由预览接管，点击“结束预览”交还。");
}

/**
 * Every Details transition names the next mode revision: the runtime accepts
 * only a revision newer than the one it holds, so passing the current one would
 * be refused as MODE_CONFLICT.
 */
int32 NextLinkageModeRevision(const FMtoULinkageView& View)
{
    return View.Identity.ModeRevision + 1;
}

FText LinkageEndTooltip(TWeakObjectPtr<AMtoULiveLinkActor> Actor, const FMtoULinkageView& View)
{
    if (!Actor.IsValid())
    {
        return LOCTEXT("LinkageEndNoActor", "选择绑定 Actor 后结束预览。");
    }
    if (View.Mode != TEXT("linkage") && !View.bTakeoverHeld)
    {
        return LOCTEXT("LinkageEndNothing", "没有需要结束的镜头联动。");
    }
    return LOCTEXT("LinkageEndReady", "结束预览：交还被接管的动画和轨道，恢复原来的播放；没能恢复的部分会在这里提示。");
}

FString PairedObjectOf(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    return Actor.IsValid() ? MtoUGetLinkagePairedObject(*Actor) : FString();
}

FText LinkagePairStateText(TWeakObjectPtr<AMtoULiveLinkActor> Actor)
{
    const FString Paired = PairedObjectOf(Actor);
    if (Paired == TEXT("character"))
    {
        return LOCTEXT("LinkagePairedPrimary", "已配对为主角色");
    }
    if (Paired == TEXT("secondary"))
    {
        return LOCTEXT("LinkagePairedSecondary", "已配对为副对象");
    }
    return LOCTEXT("LinkageUnpairedState", "未配对 Maya 对象");
}

FText LinkagePairTooltip(TWeakObjectPtr<AMtoULiveLinkActor> Actor, const FString& ObjectId)
{
    if (!Actor.IsValid())
    {
        return LOCTEXT("LinkagePairNoActor", "选择绑定 Actor 后配对 Maya 对象。");
    }
    if (!PairedObjectOf(Actor).IsEmpty())
    {
        return LOCTEXT("LinkagePairPaired", "解除配对后，需要重新配对才能进入镜头联动；进行中的连接不受影响。");
    }
    return ObjectId == TEXT("secondary")
        ? LOCTEXT("LinkagePairSecondaryReady",
            "把这个 Actor 配对为 Maya 中的副对象（骨骼道具或第二个角色）。"
            "副对象与主角色放在同一位置，重新连接后一起预览。")
        : LOCTEXT("LinkagePairReady", "把这个角色与 Maya 中连接的角色配对，配对后才能进入镜头联动。");
}

/** The second object's row: what the running set does with it. */
FText LinkageSecondaryText(const FMtoULinkageView& View)
{
    const FMtoULinkageObjectView* Secondary = View.Objects.FindByPredicate(
        [](const FMtoULinkageObjectView& Object) { return Object.ObjectId == TEXT("secondary"); });
    if (!Secondary)
    {
        return FText::GetEmpty();
    }
    if (View.bRemovingObject)
    {
        return LOCTEXT("LinkageSecondaryRemoving", "正在移除副对象，交还它原来的动画…");
    }
    if (Secondary->bNegotiated)
    {
        return LOCTEXT("LinkageSecondaryStreaming", "副对象已连接，随主角色一起预览");
    }
    return LOCTEXT("LinkageSecondaryPaired", "副对象已配对，Maya 重新连接后生效");
}

bool CanRemoveSecondary(const FMtoULinkageView& View)
{
    const FMtoULinkageObjectView* Secondary = View.Objects.FindByPredicate(
        [](const FMtoULinkageObjectView& Object) { return Object.ObjectId == TEXT("secondary"); });
    return Secondary && Secondary->bNegotiated && !View.bRemovingObject && !View.bTransitioning;
}

/** Formats the scene-reference handoff result into the panel's one report line. */
FText SceneReferenceResultText(const FMtoUSceneRefExportResult& Result)
{
    if (!Result.Error.IsEmpty())
    {
        return FText::Format(LOCTEXT("SceneReferenceFailed", "场景参考：{0}"),
            FText::FromString(Result.Error));
    }
    if (Result.Directory.IsEmpty())
    {
        return LOCTEXT("SceneReferenceIdle", "场景参考：尚未导出。");
    }
    const FText Objects = FText::AsNumber(Result.ExportedObjectCount);
    if (Result.bScopeComplete)
    {
        return FText::Format(LOCTEXT("SceneReferenceValue", "已导出 {0} 个对象，可在 Maya 导入。"), Objects);
    }
    // The export names why its scope is partial; the panel says which reason
    // applies so the user knows what to load or select.
    if (!Result.UnloadedSublevels.IsEmpty())
    {
        return FText::Format(LOCTEXT("SceneReferenceUnloadedValue",
            "已导出 {0} 个对象，可在 Maya 导入；勾选的 {1} 个子关卡未加载，未包含在内，加载后重新导出即可补上。"),
            Objects, FText::AsNumber(Result.UnloadedSublevels.Num()));
    }
    if (!Result.ExcludedSublevels.IsEmpty())
    {
        // Excluded names were selected but the world does not stream them.
        return FText::Format(LOCTEXT("SceneReferenceExcludedValue",
            "已导出 {0} 个对象，可在 Maya 导入；勾选的 {1} 个子关卡不在当前世界中，未包含在内。请检查关卡的流式设置，或重新选择子关卡。"),
            Objects, FText::AsNumber(Result.ExcludedSublevels.Num()));
    }
    return FText::Format(LOCTEXT("SceneReferencePartitionedValue",
        "已导出 {0} 个对象，可在 Maya 导入；World Partition 世界只包含当前已加载区域的内容。"), Objects);
}

FText SceneReferenceDiagnosticsText(const FMtoUSceneRefExportResult& Result)
{
    if (Result.Directory.IsEmpty() && Result.Error.IsEmpty())
    {
        return FText::GetEmpty();
    }
    TArray<FString> Lines;
    Lines.Add(TEXT("场景参考诊断"));
    Lines.Add(FString::Printf(TEXT("交付 ID：%s"), Result.TransferId.IsEmpty() ? TEXT("—") : *Result.TransferId));
    Lines.Add(FString::Printf(TEXT("清单摘要：%s"), Result.ManifestDigest.IsEmpty() ? TEXT("—") : *Result.ManifestDigest));
    Lines.Add(FString::Printf(TEXT("目录：%s"), Result.Directory.IsEmpty() ? TEXT("—") : *Result.Directory));
    Lines.Add(FString::Printf(TEXT("范围：%s · 常驻关卡 %s · 范围%s"),
        *Result.ScopeName, *Result.LevelPackage,
        Result.bScopeComplete ? TEXT("完整") : TEXT("未确认")));
    Lines.Add(FString::Printf(TEXT("子关卡：请求 %s · 已加载 %s · 排除 %s · 未加载 %s"),
        *FString::Join(Result.RequestedSublevels, TEXT(", ")),
        *FString::Join(Result.LoadedSublevels, TEXT(", ")),
        *FString::Join(Result.ExcludedSublevels, TEXT(", ")),
        *FString::Join(Result.UnloadedSublevels, TEXT(", "))));
    Lines.Add(FString::Printf(TEXT("对象 %d · 三角形 %d · 顶点 %d · 字节 %lld（几何 %lld + 清单 %lld）"),
        Result.ExportedObjectCount, Result.TriangleCount, Result.VertexCount,
        Result.GeometryBytes + Result.ManifestBytes, Result.GeometryBytes, Result.ManifestBytes));
    Lines.Add(FString::Printf(TEXT("跳过 Actor %d · 抑制组件 %d · 图片数据 %s"),
        Result.SkippedActorCount, Result.SuppressedComponentCount,
        Result.bImageDataDetected ? TEXT("有") : TEXT("无")));
    for (const FString& Warning : Result.Warnings)
    {
        Lines.Add(FString::Printf(TEXT("告警：%s"), *Warning));
    }
    if (!Result.Error.IsEmpty())
    {
        Lines.Add(FString::Printf(TEXT("错误：%s"), *Result.Error));
    }
    return FText::FromString(FString::Join(Lines, TEXT("\n")));
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

FText FMtoULiveLinkActorDetails::CompactLinkageText(const FText& Text)
{
    return CompactLinkageRuns(Text);
}

FText FMtoULiveLinkActorDetails::FormatCacheState(const FMtoUCachePlaybackView& View)
{
    return CacheStateText(View);
}

FText FMtoULiveLinkActorDetails::FormatLinkageStatus(const FMtoULinkageView& View)
{
    return LinkageStatusText(View);
}

FText FMtoULiveLinkActorDetails::FormatLinkageLag(const FMtoULinkageView& View)
{
    return LinkageLagText(View);
}

FText FMtoULiveLinkActorDetails::FormatLinkageRecovery(const FMtoULinkageView& View)
{
    return LinkageRestoreText(View);
}

FText FMtoULiveLinkActorDetails::FormatLinkageDiagnostics(const FMtoULinkageView& View)
{
    return LinkageDiagnosticsText(View);
}

FText FMtoULiveLinkActorDetails::FormatSceneReferenceResult(const FMtoUSceneRefExportResult& Result)
{
    return SceneReferenceResultText(Result);
}

FText FMtoULiveLinkActorDetails::FormatSceneReferenceDiagnostics(const FMtoUSceneRefExportResult& Result)
{
    return SceneReferenceDiagnosticsText(Result);
}

bool FMtoULiveLinkActorDetails::bExpandAllGroups = false;

bool FMtoULiveLinkActorDetails::ParseSeekSourceFrame(const FString& Input, int32& OutFrame)
{
    const FString Text = Input.TrimStartAndEnd();
    int32 Index = 0;
    const bool bNegative = !Text.IsEmpty() && Text[0] == TEXT('-');
    if (!Text.IsEmpty() && (bNegative || Text[0] == TEXT('+')))
    {
        ++Index;
    }
    if (Index == Text.Len())
    {
        return false;
    }

    const int64 Limit = bNegative ? -(static_cast<int64>(MIN_int32)) : MAX_int32;
    int64 Value = 0;
    for (; Index < Text.Len(); ++Index)
    {
        const TCHAR Character = Text[Index];
        if (Character < TEXT('0') || Character > TEXT('9'))
        {
            return false;
        }
        const int64 Digit = Character - TEXT('0');
        if (Value > (Limit - Digit) / 10)
        {
            return false;
        }
        Value = Value * 10 + Digit;
    }
    OutFrame = static_cast<int32>(bNegative ? -Value : Value);
    return true;
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
            .InitiallyCollapsed(!bExpandAllGroups)
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
                        int32 SourceFrame = 0;
                        if (!FMtoULiveLinkActorDetails::ParseSeekSourceFrame(*SeekInput, SourceFrame))
                        {
                            *SeekMessage = LOCTEXT("CacheSeekNotInteger", "请输入有效范围内的整数 Maya 源帧。");
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

    IDetailCategoryBuilder& Linkage = DetailBuilder.EditCategory(
        "MtoU Linkage", LOCTEXT("LinkageCategory", "镜头联动"), ECategoryPriority::Important);
    Linkage.InitiallyCollapsed(false);

    const TSharedPtr<FText> LinkageMessage = MakeShared<FText>();
    // Every linkage row reads MtoUGetLinkageView() through a Slate attribute, so a
    // row that is not invalidated keeps the text it was last painted with. Each row
    // therefore registers itself with the editor module, which invalidates the
    // registered rows whenever the runtime publishes a new view.
    auto AddLinkageRow = [&Linkage](const FText& FilterText, TSharedRef<SWidget> Widget)
    {
        FDetailWidgetRow& Row = Linkage.AddCustomRow(FilterText);
        Row.WholeRowContent()[Widget];
        FMtoULinkageEditor::RegisterLinkagePanelWidget(Row.WholeRowWidget.Widget);
    };
    // Protocol v11 pairs the primary character ("character") and at most one
    // second object ("secondary"); each actor of the set serves one of them,
    // so the row names the role instead of asking for an id.
    const auto PairAs = [Actor, LinkageMessage](const FString& ObjectId)
    {
        AMtoULiveLinkActor* Target = Actor.Get();
        if (!Target)
        {
            return FReply::Handled();
        }
        *LinkageMessage = FText::GetEmpty();
        if (ObjectId == TEXT("character"))
        {
            FMtoULinkageEditor::SetObservedLinkageActor(Target);
        }
        FString PairError;
        if (!MtoUPairLinkageTarget(*Target, ObjectId, PairError))
        {
            *LinkageMessage = FText::Format(
                LOCTEXT("LinkagePairRefused", "无法配对：{0}"),
                FText::FromString(PairError));
        }
        return FReply::Handled();
    };
    const auto UnpairedVisibility = [Actor]()
    {
        return PairedObjectOf(Actor).IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
    };
    AddLinkageRow(LOCTEXT("LinkagePairingFilter", "配对 Maya 对象"),
        SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Actor]() { return LinkagePairStateText(Actor); })
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("LinkagePair", "配对为主角色"))
                .ToolTipText_Lambda([Actor]() { return LinkagePairTooltip(Actor, TEXT("character")); })
                .Visibility_Lambda(UnpairedVisibility)
                .IsEnabled_Lambda([Actor]() { return Actor.IsValid(); })
                .OnClicked_Lambda([PairAs]() { return PairAs(TEXT("character")); })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("LinkagePairSecondary", "配对为副对象"))
                .ToolTipText_Lambda([Actor]() { return LinkagePairTooltip(Actor, TEXT("secondary")); })
                .Visibility_Lambda(UnpairedVisibility)
                .IsEnabled_Lambda([Actor]() { return Actor.IsValid(); })
                .OnClicked_Lambda([PairAs]() { return PairAs(TEXT("secondary")); })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("LinkageClearPair", "解除配对"))
                .ToolTipText_Lambda([Actor]() { return LinkagePairTooltip(Actor, FString()); })
                .Visibility_Lambda([Actor]()
                {
                    return PairedObjectOf(Actor).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
                })
                .OnClicked_Lambda([Actor, LinkageMessage]()
                {
                    AMtoULiveLinkActor* Target = Actor.Get();
                    if (!Target)
                    {
                        return FReply::Handled();
                    }
                    *LinkageMessage = FText::GetEmpty();
                    if (PairedObjectOf(Actor) == TEXT("character"))
                    {
                        FMtoULinkageEditor::SetObservedLinkageActor(nullptr);
                    }
                    MtoUClearLinkageTarget(*Target);
                    return FReply::Handled();
                })
            ]
        );

    AddLinkageRow(LOCTEXT("LinkageSecondaryFilter", "副对象"),
        SNew(SHorizontalBox)
            .Visibility_Lambda([]()
            {
                return LinkageSecondaryText(MtoUGetLinkageView()).IsEmpty()
                    ? EVisibility::Collapsed : EVisibility::Visible;
            })
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([]() { return LinkageSecondaryText(MtoUGetLinkageView()); })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("LinkageRemoveSecondary", "移除副对象"))
                .ToolTipText(LOCTEXT("LinkageRemoveSecondaryTooltip",
                    "在当前连接内移除副对象：交还它原来的动画和轨道，主角色继续预览，不需要重新连接。"))
                .IsEnabled_Lambda([]() { return CanRemoveSecondary(MtoUGetLinkageView()); })
                .OnClicked_Lambda([LinkageMessage]()
                {
                    FString RemoveError;
                    *LinkageMessage = MtoURequestRemoveSecondaryObject(RemoveError)
                        ? FText::GetEmpty()
                        : FText::Format(
                            LOCTEXT("LinkageRemoveSecondaryRefused", "无法移除副对象：{0}"),
                            FText::FromString(RemoveError));
                    return FReply::Handled();
                })
            ]
        );

    AddLinkageRow(LOCTEXT("LinkageActionsFilter", "联动操作"),
        SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("LinkageEnter", "进入联动"))
                .ToolTipText_Lambda([Actor]()
                {
                    return LinkageEnterTooltip(Actor, MtoUGetLinkageView());
                })
                .IsEnabled_Lambda([Actor]()
                {
                    const FMtoULinkageView View = MtoUGetLinkageView();
                    return Actor.IsValid() && View.bPaired && !View.bTransitioning
                        && View.Mode != TEXT("linkage") && View.Mode != TEXT("cached");
                })
                .OnClicked_Lambda([Actor, LinkageMessage]()
                {
                    if (!Actor.IsValid())
                    {
                        return FReply::Handled();
                    }
                    const FMtoULinkageView View = MtoUGetLinkageView();
                    FString EnterError;
                    if (!MtoURequestLinkageEnter(NextLinkageModeRevision(View), EnterError))
                    {
                        *LinkageMessage = FText::Format(
                            LOCTEXT("LinkageEnterRefused", "无法进入镜头联动：{0}"),
                            FText::FromString(EnterError));
                        return FReply::Handled();
                    }
                    *LinkageMessage = FText::GetEmpty();
                    return FReply::Handled();
                })
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("LinkageRealtime", "返回实时"))
                .ToolTipText_Lambda([Actor]()
                {
                    return LinkageRealtimeTooltip(Actor, MtoUGetLinkageView());
                })
                .IsEnabled_Lambda([Actor]()
                {
                    const FMtoULinkageView View = MtoUGetLinkageView();
                    return Actor.IsValid() && View.Mode == TEXT("linkage") && !View.bTransitioning;
                })
                .OnClicked_Lambda([Actor, LinkageMessage]()
                {
                    if (!Actor.IsValid())
                    {
                        return FReply::Handled();
                    }
                    const FMtoULinkageView View = MtoUGetLinkageView();
                    FString LeaveError;
                    if (!MtoURequestLinkageLeave(TEXT("realtime"), NextLinkageModeRevision(View), LeaveError))
                    {
                        *LinkageMessage = FText::Format(
                            LOCTEXT("LinkageRealtimeRefused", "无法返回实时：{0}"),
                            FText::FromString(LeaveError));
                        return FReply::Handled();
                    }
                    *LinkageMessage = FText::GetEmpty();
                    return FReply::Handled();
                })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("LinkageEnd", "结束预览"))
                .ToolTipText_Lambda([Actor]()
                {
                    return LinkageEndTooltip(Actor, MtoUGetLinkageView());
                })
                .IsEnabled_Lambda([Actor]()
                {
                    const FMtoULinkageView View = MtoUGetLinkageView();
                    return Actor.IsValid()
                        && (View.Mode == TEXT("linkage") || View.bTakeoverHeld) && !View.bTransitioning;
                })
                .OnClicked_Lambda([Actor, LinkageMessage]()
                {
                    if (!Actor.IsValid())
                    {
                        return FReply::Handled();
                    }
                    const FMtoULinkageView View = MtoUGetLinkageView();
                    FString EndError;
                    if (!MtoURequestLinkageLeave(TEXT("end"), NextLinkageModeRevision(View), EndError))
                    {
                        *LinkageMessage = FText::Format(
                            LOCTEXT("LinkageEndRefused", "无法结束预览：{0}"),
                            FText::FromString(EndError));
                        return FReply::Handled();
                    }
                    *LinkageMessage = FText::GetEmpty();
                    return FReply::Handled();
                })
            ]
        );

    AddLinkageRow(LOCTEXT("LinkageStatusRow", "联动状态"),
        SNew(STextBlock)
            .AutoWrapText(true)
            .Text_Lambda([]() { return LinkageStatusText(MtoUGetLinkageView()); })
        );

    AddLinkageRow(LOCTEXT("LinkageLagRow", "滞后"),
        SNew(STextBlock)
            .AutoWrapText(true)
            .ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.25f)))
            .Text_Lambda([]() { return LinkageLagText(MtoUGetLinkageView()); })
            .Visibility_Lambda([]()
            {
                return LinkageLagText(MtoUGetLinkageView()).IsEmpty()
                    ? EVisibility::Collapsed : EVisibility::Visible;
            })
        );

    AddLinkageRow(LOCTEXT("LinkageRecoveryRow", "恢复结果"),
        SNew(STextBlock)
            .AutoWrapText(true)
            .ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.25f)))
            .Text_Lambda([]() { return LinkageRestoreText(MtoUGetLinkageView()); })
            .ToolTipText_Lambda([]() { return FText::FromString(MtoUGetLinkageView().RecoveryResidual); })
            .Visibility_Lambda([]()
            {
                return LinkageRestoreText(MtoUGetLinkageView()).IsEmpty()
                    ? EVisibility::Collapsed : EVisibility::Visible;
            })
        );

    AddLinkageRow(LOCTEXT("LinkageDiagnosticsRow", "错误"),
        SNew(STextBlock)
            .AutoWrapText(true)
            .ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.35f, 0.25f)))
            .Text_Lambda([Actor, LinkageMessage]()
            {
                if (!LinkageMessage->IsEmpty())
                {
                    return CompactLinkageText(*LinkageMessage);
                }
                return CompactLinkageText(LinkageErrorText(MtoUGetLinkageView()));
            })
            .ToolTipText_Lambda([LinkageMessage]()
            {
                return LinkageMessage->IsEmpty() ? LinkageErrorText(MtoUGetLinkageView()) : *LinkageMessage;
            })
            .Visibility_Lambda([Actor, LinkageMessage]()
            {
                return !LinkageMessage->IsEmpty()
                    || !LinkageErrorText(MtoUGetLinkageView()).IsEmpty()
                    ? EVisibility::Visible : EVisibility::Collapsed;
            })
        );

    IDetailCategoryBuilder& SceneReference = DetailBuilder.EditCategory(
        "MtoU Scene Reference", LOCTEXT("SceneReferenceCategory", "场景参考"),
        ECategoryPriority::Default);
    SceneReference.InitiallyCollapsed(!bExpandAllGroups);

    UWorld* PanelWorld = Actor.IsValid() ? Actor->GetWorld() : nullptr;
    struct FMtoUSublevelChoice
    {
        FString Package;
        TSharedPtr<bool> bRequested;
    };
    const TSharedPtr<TArray<FMtoUSublevelChoice>> SublevelChoices =
        MakeShared<TArray<FMtoUSublevelChoice>>();
    const TSharedPtr<FMtoUSceneRefExportResult> ReferenceResult =
        MakeShared<FMtoUSceneRefExportResult>();
    const TSharedPtr<FText> ReferenceMessage = MakeShared<FText>();
    if (PanelWorld)
    {
        for (ULevelStreaming* Streaming : PanelWorld->GetStreamingLevels())
        {
            if (Streaming && Streaming->GetLoadedLevel())
            {
                FMtoUSublevelChoice Choice;
                Choice.Package = FPaths::GetBaseFilename(
                    Streaming->GetWorldAsset().ToSoftObjectPath().GetLongPackageName());
                Choice.bRequested = MakeShared<bool>(false);
                SublevelChoices->Add(MoveTemp(Choice));
            }
        }
    }

    SceneReference.AddCustomRow(LOCTEXT("SceneReferenceScopeFilter", "范围 关卡"))
        .WholeRowContent()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Actor]()
                {
                    const UWorld* World = Actor.IsValid() ? Actor->GetWorld() : nullptr;
                    const ULevel* Level = World ? World->PersistentLevel : nullptr;
                    return FText::Format(LOCTEXT("SceneReferenceLevel", "常驻关卡：{0}"),
                        Level ? FText::FromString(Level->GetPackage()->GetName())
                              : LOCTEXT("SceneReferenceNoWorld", "未知"));
                })
                .ToolTipText(LOCTEXT("SceneReferenceScopeTip",
                    "范围是显式的：当前常驻关卡加上勾选的已加载子关卡；不会自动加载任何子关卡。"))
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                .Text_Lambda([Actor]()
                {
                    const UWorld* World = Actor.IsValid() ? Actor->GetWorld() : nullptr;
                    if (!World)
                    {
                        return LOCTEXT("SceneReferenceNoLevels", "子关卡：无世界");
                    }
                    if (World->IsPartitionedWorld())
                    {
                        return LOCTEXT("SceneReferencePartitioned",
                            "子关卡：该世界为 World Partition，范围只含已加载单元格内容。");
                    }
                    if (World->GetStreamingLevels().IsEmpty())
                    {
                        return LOCTEXT("SceneReferenceNoSublevels", "子关卡：该世界没有流式子关卡。");
                    }
                    return LOCTEXT("SceneReferenceSublevelCount", "子关卡：见下方勾选。");
                })
            ]
        ];

    if (SublevelChoices->Num() > 0)
    {
        TSharedRef<SVerticalBox> SublevelList = SNew(SVerticalBox);
        for (const FMtoUSublevelChoice& Choice : *SublevelChoices)
        {
            SublevelList->AddSlot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 2.0f)
                [
                    SNew(SCheckBox)
                    .Content()
                    [
                        SNew(STextBlock).Text(FText::FromString(Choice.Package))
                    ]
                    .ToolTipText(LOCTEXT("SceneReferenceSublevelTip",
                        "勾选后进入显式范围；未加载的子关卡不会出现在这里。"))
                    .IsChecked_Lambda([Choice]()
                    {
                        return Choice.bRequested.IsValid() && *Choice.bRequested
                            ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([Choice](ECheckBoxState State)
                    {
                        if (Choice.bRequested.IsValid())
                        {
                            *Choice.bRequested = State == ECheckBoxState::Checked;
                        }
                    })
                ];
        }
        SceneReference.AddCustomRow(LOCTEXT("SceneReferenceSublevelsFilter", "已加载子关卡"))
            .WholeRowContent()
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                    .Text(LOCTEXT("SceneReferenceSublevelsHint",
                        "勾选要放进移交的子关卡；未勾选的子关卡保持不动。"))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SublevelList
                ]
            ];
    }

    SceneReference.AddCustomRow(LOCTEXT("SceneReferenceActionsFilter", "导出 移交"))
        .WholeRowContent()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("SceneReferenceExport", "导出场景参考"))
                    .ToolTipText(LOCTEXT("SceneReferenceExportTip",
                        "用引擎自己的 FBX 关卡导出具名范围内的静态网格，写清单并检查无图片数据后移交给 Maya。"))
                    .IsEnabled_Lambda([Actor]()
                    {
                        return Actor.IsValid() && Actor->GetWorld() != nullptr;
                    })
                    .OnClicked_Lambda([Actor, SublevelChoices, ReferenceResult, ReferenceMessage]()
                    {
                        AMtoULiveLinkActor* Target = Actor.Get();
                        UWorld* World = Target ? Target->GetWorld() : nullptr;
                        if (!World)
                        {
                            return FReply::Handled();
                        }
                        TArray<FString> Requested;
                        for (const FMtoUSublevelChoice& Choice : *SublevelChoices)
                        {
                            if (Choice.bRequested.IsValid() && *Choice.bRequested)
                            {
                                Requested.Add(Choice.Package);
                            }
                        }
                        *ReferenceMessage = FText::GetEmpty();
                        FScopedSlowTask ExportTask(1.0f,
                            LOCTEXT("SceneReferenceExportProgress", "导出场景参考"));
                        ExportTask.MakeDialog();
                        if (!FMtoUSceneRefExport::Run(*World, Requested, *ReferenceResult))
                        {
                            *ReferenceMessage = FText::Format(
                                LOCTEXT("SceneReferenceExportRefused", "导出失败：{0}"),
                                FText::FromString(ReferenceResult->Error));
                        }
                        else if (ReferenceResult->bImageDataDetected)
                        {
                            *ReferenceMessage = LOCTEXT("SceneReferenceImageData",
                                "导出的移交包含图片数据，已拒绝。");
                        }
                        return FReply::Handled();
                    })
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([Actor, ReferenceResult, ReferenceMessage]()
                {
                    if (!ReferenceMessage->IsEmpty())
                    {
                        return *ReferenceMessage;
                    }
                    return SceneReferenceResultText(*ReferenceResult);
                })
                .Visibility_Lambda([Actor, ReferenceResult, ReferenceMessage]()
                {
                    return !ReferenceMessage->IsEmpty() || !ReferenceResult->Directory.IsEmpty()
                        || !ReferenceResult->Error.IsEmpty()
                        ? EVisibility::Visible : EVisibility::Collapsed;
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.35f, 0.25f)))
                .Text_Lambda([ReferenceResult]()
                {
                    if (ReferenceResult->bSucceeded)
                    {
                        return FText::GetEmpty();
                    }
                    return FText::Format(LOCTEXT("SceneReferenceError", "场景参考错误：{0}"),
                        FText::FromString(ReferenceResult->Error));
                })
                .Visibility_Lambda([ReferenceResult]()
                {
                    return ReferenceResult->bSucceeded || ReferenceResult->Error.IsEmpty()
                        ? EVisibility::Collapsed : EVisibility::Visible;
                })
            ]
        ];

    IDetailCategoryBuilder& Advanced = DetailBuilder.EditCategory(
        "MtoU Advanced", LOCTEXT("AdvancedCategory", "高级设置与诊断"),
        ECategoryPriority::Default);
    Advanced.InitiallyCollapsed(!bExpandAllGroups);
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
    // The full report: the preview and connection diagnostics plus the linkage
    // read-back and the handoff identity the groups above leave out.
    auto FullDiagnostics = [Status, ReferenceResult]()
    {
        const FText Parts[] = {
            Status->Get().RawDiagnostics,
            LinkageDiagnosticsText(MtoUGetLinkageView()),
            SceneReferenceDiagnosticsText(*ReferenceResult)};
        TArray<FString> Sections;
        for (const FText& Section : Parts)
        {
            if (!Section.IsEmpty())
            {
                Sections.Add(Section.ToString());
            }
        }
        return FText::FromString(FString::Join(Sections, TEXT("\n\n")));
    };
    FDetailWidgetRow& DiagnosticsRow = Advanced.AddCustomRow(
        LOCTEXT("DiagnosticsFilter", "错误详情 原始诊断 镜头联动 场景参考"));
    DiagnosticsRow.WholeRowContent()
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
                .Visibility_Lambda([FullDiagnostics]()
                {
                    return FullDiagnostics().IsEmpty()
                        ? EVisibility::Collapsed : EVisibility::Visible;
                })
                [
                    SNew(SMultiLineEditableTextBox)
                    .IsReadOnly(true)
                    .AutoWrapText(true)
                    .Text_Lambda([FullDiagnostics]() { return FullDiagnostics(); })
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
            [
                SNew(SButton)
                .HAlign(HAlign_Center)
                .Text(LOCTEXT("CopyDiagnostics", "复制完整诊断"))
                .Visibility_Lambda([FullDiagnostics]()
                {
                    return FullDiagnostics().IsEmpty()
                        ? EVisibility::Collapsed : EVisibility::Visible;
                })
                .OnClicked_Lambda([FullDiagnostics]()
                {
                    FPlatformApplicationMisc::ClipboardCopy(*FullDiagnostics().ToString());
                    return FReply::Handled();
                })
            ]
        ];
    // The linkage read-back changes without touching the actor, so this row is
    // invalidated with the linkage rows.
    FMtoULinkageEditor::RegisterLinkagePanelWidget(DiagnosticsRow.WholeRowWidget.Widget);
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
