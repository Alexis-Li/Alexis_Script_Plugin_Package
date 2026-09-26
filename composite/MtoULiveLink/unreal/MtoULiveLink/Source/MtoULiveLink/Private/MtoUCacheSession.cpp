#include "MtoUCacheSession.h"

#include "HAL/PlatformTime.h"

DEFINE_LOG_CATEGORY_STATIC(LogMtoUCacheSession, Log, All);

namespace
{
const TCHAR* ToString(EMtoUCacheState State)
{
    switch (State)
    {
        case EMtoUCacheState::Idle: return TEXT("Idle");
        case EMtoUCacheState::Entered: return TEXT("Entered");
        case EMtoUCacheState::Receiving: return TEXT("Receiving");
        case EMtoUCacheState::Ready: return TEXT("Ready");
        case EMtoUCacheState::Playing: return TEXT("Playing");
        case EMtoUCacheState::Completed: return TEXT("Completed");
        case EMtoUCacheState::Stopped: return TEXT("Stopped");
        case EMtoUCacheState::Failed: return TEXT("Failed");
    }
    return TEXT("Unknown");
}

// Conservative parsed-memory estimate for one buffered cached frame, derived
// only from the negotiated transform/curve counts before any allocation.
constexpr int64 PerTransformParsedBytes = sizeof(FMtoUTransform);
constexpr int64 PerCurveParsedBytes = sizeof(double);
constexpr int64 PerFrameOverheadBytes = 64;
} // namespace

FMtoUCacheSession::FMtoUCacheSession()
    : Now([]() { return FPlatformTime::Seconds(); })
{
}

void FMtoUCacheSession::SetClock(FNow InNow)
{
    Now = MoveTemp(InNow);
}

void FMtoUCacheSession::SetPublish(FPublish InPublish)
{
    Publish = MoveTemp(InPublish);
}

void FMtoUCacheSession::SetProgressSink(FProgress InProgress)
{
    ProgressSink = MoveTemp(InProgress);
}

void FMtoUCacheSession::BeginSession(const FMtoUCacheSessionContext& Context)
{
    EndSession();
    ExpectedTransformCount = Context.TransformCount;
    ExpectedCurveCount = Context.CurveCount;
    NegotiatedRevision = Context.Revision;
}

double FMtoUCacheSession::FrameInterval() const
{
    return 1.0 / Begin.Fps;
}

void FMtoUCacheSession::ResetToIdle()
{
    State = EMtoUCacheState::Idle;
    Begin = FMtoUCacheBeginMessage();
    Frames.Reset();
    ActualPayloadBytes = 0;
    NextFrame = 0;
    AppliedCount = 0;
    LastAppliedIndex = INDEX_NONE;
    PlaybackStart = 0.0;
    ElapsedSeconds = 0.0;
    ActiveUploadId = 0;
    ActivePlayId = 0;
}

void FMtoUCacheSession::EndSession()
{
    ResetToIdle();
    LastSeenUploadId = 0;
    LastSeenPlayId = 0;
    ExpectedTransformCount = INDEX_NONE;
    ExpectedCurveCount = INDEX_NONE;
    NegotiatedRevision = 0;
    ErrorDetails.Reset();
}

FMtoUCachePlaybackView FMtoUCacheSession::GetView() const
{
    FMtoUCachePlaybackView View;
    View.State = State;
    View.StartFrame = Begin.StartFrame;
    View.EndFrame = Begin.EndFrame;
    View.Fps = Begin.Fps;
    View.FrameCount = Begin.FrameCount;
    View.AppliedFrames = AppliedCount;
    View.ErrorDetails = ErrorDetails;
    if (LastAppliedIndex != INDEX_NONE)
    {
        View.CurrentSourceFrame = Begin.StartFrame + LastAppliedIndex;
    }
    return View;
}

FMtoUCacheTransition FMtoUCacheSession::StartLocalPlayback()
{
    FMtoUCacheCommand Command;
    Command.Kind = FMtoUCacheCommand::EKind::Play;
    Command.PlayId = LastSeenPlayId + 1;
    return HandleCommand(Command);
}

FMtoUCacheTransition FMtoUCacheSession::StopLocalPlayback()
{
    if (State != EMtoUCacheState::Playing)
    {
        return FMtoUCacheTransition();
    }
    FMtoUCacheCommand Command;
    Command.Kind = FMtoUCacheCommand::EKind::Stop;
    return HandleCommand(Command);
}

FMtoUCacheTransition FMtoUCacheSession::HandleCommand(const FMtoUCacheCommand& Command)
{
    using EKind = FMtoUCacheTransition::EKind;
    FMtoUCacheTransition Result;
    Result.Revision = NegotiatedRevision;
    ErrorDetails.Reset();
    bool bAccepted = false;
    switch (Command.Kind)
    {
        case FMtoUCacheCommand::EKind::Enter:
            bAccepted = HandleEnter(Command, Result.ErrorCode, Result.Details);
            Result.Kind = EKind::Entered;
            Result.RealtimeOverride = false;
            break;
        case FMtoUCacheCommand::EKind::Begin:
            Result.UploadId = Command.Begin.UploadId;
            bAccepted = HandleBegin(Command, Result.ErrorCode, Result.Details);
            Result.Kind = EKind::Receiving;
            Result.FrameCount = Begin.FrameCount;
            break;
        case FMtoUCacheCommand::EKind::Reject:
            Result.UploadId = Command.UploadId;
            Result.ErrorCode = Command.ErrorCode;
            Result.Details = Command.ErrorDetails;
            ResetToIdle();
            break;
        case FMtoUCacheCommand::EKind::Frame:
            Result.UploadId = ActiveUploadId;
            bAccepted = HandleFrame(Command, Result.ErrorCode, Result.Details);
            break;
        case FMtoUCacheCommand::EKind::End:
            Result.UploadId = ActiveUploadId;
            bAccepted = HandleEnd(Command, Result.ErrorCode, Result.Details);
            Result.Kind = EKind::Ready;
            Result.FrameCount = Frames.Num();
            break;
        case FMtoUCacheCommand::EKind::Play:
            Result.UploadId = ActiveUploadId;
            Result.PlayId = Command.PlayId;
            bAccepted = HandlePlay(Command, Result.ErrorCode, Result.Details);
            Result.Kind = EKind::Playing;
            if (bAccepted)
            {
                Result.RealtimeOverride = true;
            }
            break;
        case FMtoUCacheCommand::EKind::Stop:
            if (State == EMtoUCacheState::Playing)
            {
                State = EMtoUCacheState::Stopped;
            }
            Result.Kind = EKind::Stopped;
            Result.PlayId = ActivePlayId;
            Result.RealtimeOverride = false;
            bAccepted = true;
            break;
        case FMtoUCacheCommand::EKind::Clear:
            // The result owns the released identity; no caller has to read
            // cache state before clear or keep a second last-cleared record.
            Result.UploadId = ActiveUploadId;
            Result.PlayId = ActivePlayId;
            ResetToIdle();
            Result.Kind = EKind::Cleared;
            Result.RealtimeOverride = true;
            bAccepted = true;
            break;
    }
    Result.bAccepted = bAccepted;
    if (!bAccepted)
    {
        Result.Kind = EKind::Rejected;
        if (Result.ErrorCode.IsEmpty())
        {
            Result.ErrorCode = TEXT("CACHE_INVALID_STATE");
            Result.Details = TEXT("Unknown cache command failure.");
        }
        ErrorDetails = Result.ErrorCode + TEXT(": ") + Result.Details;
        // Recoverable failures that release cached ownership restore live
        // refresh. Rejections that retain ownership preserve its demand.
        if (AcceptsLiveFrames())
        {
            Result.RealtimeOverride = true;
        }
    }
    return Result;
}

bool FMtoUCacheSession::HandleEnter(
    const FMtoUCacheCommand& Command,
    FString& OutErrorCode,
    FString& OutDetails)
{
    (void)Command;
    (void)OutErrorCode;
    (void)OutDetails;
    // Entry establishes cached ownership before capture; entering while
    // already owning a cache is an idempotent no-op so repeated mode entry
    // never destroys buffered work.
    if (State == EMtoUCacheState::Idle)
    {
        State = EMtoUCacheState::Entered;
    }
    return true;
}

bool FMtoUCacheSession::HandleBegin(
    const FMtoUCacheCommand& Command,
    FString& OutErrorCode,
    FString& OutDetails)
{
    const FMtoUCacheBeginMessage& Incoming = Command.Begin;
    if (Incoming.UploadId <= LastSeenUploadId)
    {
        OutErrorCode = TEXT("CACHE_METADATA_INVALID");
        OutDetails = FString::Printf(
            TEXT("upload_id %d must increase within the streaming session"
                 " (last seen %d)."),
            Incoming.UploadId,
            LastSeenUploadId);
        ResetToIdle();
        return false;
    }
    LastSeenUploadId = Incoming.UploadId;
    // A claim for any other snapshot can never become compatible. Its upload
    // identity is still consumed so a rejected attempt cannot be replayed.
    if (Incoming.Revision != NegotiatedRevision)
    {
        OutErrorCode = TEXT("CACHE_REVISION_MISMATCH");
        OutDetails = FString::Printf(
            TEXT("cache upload declares revision %d but the negotiated"
                 " character snapshot revision is %d."),
            Incoming.Revision,
            NegotiatedRevision);
        ResetToIdle();
        return false;
    }
    // Preflight the predicted parsed transient allocation from the negotiated
    // transform/curve counts before allocating anything. Overflow-safe:
    // every factor is bounded by frozen protocol limits.
    const int64 PerFrameBytes =
        static_cast<int64>(FMath::Max(ExpectedTransformCount, 0)) * PerTransformParsedBytes
        + static_cast<int64>(FMath::Max(ExpectedCurveCount, 0)) * PerCurveParsedBytes
        + PerFrameOverheadBytes;
    const int64 PredictedParsedBytes =
        static_cast<int64>(Incoming.FrameCount) * PerFrameBytes;
    if (PredictedParsedBytes > FMtoUProtocol::MaxCacheParsedMemoryBytes)
    {
        OutErrorCode = TEXT("CACHE_PAYLOAD_TOO_LARGE");
        OutDetails = FString::Printf(
            TEXT("predicted parsed cache memory %lld bytes exceeds the fixed"
                 " budget of %lld bytes."),
            PredictedParsedBytes,
            FMtoUProtocol::MaxCacheParsedMemoryBytes);
        ResetToIdle();
        UE_LOG(LogMtoUCacheSession, Warning, TEXT("Rejected cache upload (%s): %s"), *OutErrorCode, *OutDetails);
        return false;
    }

    // Recapture always replaces the previous cache coherently; old and new
    // frames can never mix because Begin resets the buffer unconditionally.
    ActiveUploadId = Incoming.UploadId;
    ActivePlayId = 0;
    Begin = Incoming;
    Frames.Reset(Begin.FrameCount);
    ActualPayloadBytes = 0;
    NextFrame = 0;
    AppliedCount = 0;
    LastAppliedIndex = INDEX_NONE;
    State = EMtoUCacheState::Receiving;
    return true;
}

bool FMtoUCacheSession::HandleFrame(
    const FMtoUCacheCommand& Command,
    FString& OutErrorCode,
    FString& OutDetails)
{
    // A negative index is always the stable index error, and it always
    // atomically discards any partial upload: reset before replying so a
    // later frame can never complete the poisoned upload.
    if (Command.Index < 0)
    {
        OutErrorCode = TEXT("CACHE_FRAME_INDEX_INVALID");
        OutDetails = FString::Printf(
            TEXT("Field 'index' must not be negative; got %d."), Command.Index);
        ResetToIdle();
        return false;
    }
    if (State != EMtoUCacheState::Receiving)
    {
        OutErrorCode = TEXT("CACHE_INVALID_STATE");
        OutDetails = FString::Printf(
            TEXT("cache_frame arrived while the cache state is %s."), ToString(State));
        // A rejected upload is never partially retained: the whole transient
        // buffer drops so a partial cache can never become Ready or replayable.
        ResetToIdle();
        return false;
    }

    if (Command.Index != Frames.Num() || Frames.Num() >= Begin.FrameCount)
    {
        OutErrorCode = TEXT("CACHE_FRAME_INDEX_INVALID");
        OutDetails = FString::Printf(
            TEXT("Expected cached frame index %d, got %d."),
            Frames.Num(),
            Command.Index);
        ResetToIdle();
        return false;
    }
    // Meter actual encoded bytes against both the client's declared size and
    // the frozen wire limit, with overflow-safe accumulation. The rejection
    // reports the computed candidate total alongside declaration and limit.
    if (Command.EncodedBytes < 0
        || ActualPayloadBytes > FMtoUProtocol::MaxCachePayloadBytes - Command.EncodedBytes
        || ActualPayloadBytes + Command.EncodedBytes > Begin.PayloadSize)
    {
        OutErrorCode = TEXT("CACHE_PAYLOAD_TOO_LARGE");
        OutDetails = FString::Printf(
            TEXT("actual uploaded encoded bytes %lld exceed the declared"
                 " payload_size %lld or the frozen limit of %lld bytes."),
            ActualPayloadBytes + Command.EncodedBytes,
            Begin.PayloadSize,
            FMtoUProtocol::MaxCachePayloadBytes);
        ResetToIdle();
        UE_LOG(LogMtoUCacheSession, Warning, TEXT("Rejected cache upload (%s): %s"), *OutErrorCode, *OutDetails);
        return false;
    }

    FString ValidationError;
    bool bStructural = false;
    if (!FMtoUProtocol::ValidateFrame(
            Command.Frame,
            ExpectedTransformCount,
            ExpectedCurveCount,
            ValidationError,
            bStructural))
    {
        OutErrorCode = TEXT("CACHE_FRAME_CONTENTS_INVALID");
        OutDetails = FString::Printf(TEXT("Cached frame %d: %s"), Command.Index, *ValidationError);
        ResetToIdle();
        UE_LOG(LogMtoUCacheSession, Warning, TEXT("Rejected cache upload (%s): %s"), *OutErrorCode, *OutDetails);
        return false;
    }

    ActualPayloadBytes += Command.EncodedBytes;
    Frames.Add(Command.Frame);
    return true;
}

bool FMtoUCacheSession::HandleEnd(
    const FMtoUCacheCommand& Command,
    FString& OutErrorCode,
    FString& OutDetails)
{
    (void)Command;
    if (State != EMtoUCacheState::Receiving)
    {
        OutErrorCode = TEXT("CACHE_INVALID_STATE");
        OutDetails = FString::Printf(
            TEXT("cache_end arrived while the cache state is %s."), ToString(State));
        return false;
    }
    if (Frames.Num() != Begin.FrameCount)
    {
        OutErrorCode = TEXT("CACHE_INVALID_STATE");
        OutDetails = FString::Printf(
            TEXT("cache_end arrived after %d of %d declared frames."),
            Frames.Num(),
            Begin.FrameCount);
        ResetToIdle();
        return false;
    }
    // Atomic Ready: only a complete, validated, locally buffered cache can
    // ever enter the Ready state.
    State = EMtoUCacheState::Ready;
    return true;
}

bool FMtoUCacheSession::HandlePlay(
    const FMtoUCacheCommand& Command,
    FString& OutErrorCode,
    FString& OutDetails)
{
    // Identity sanity comes before state so malformed or stale requests are
    // always reported as metadata problems, never as generic not-ready.
    if (Command.PlayId < 1)
    {
        OutErrorCode = TEXT("CACHE_METADATA_INVALID");
        OutDetails = TEXT("Field 'play_id' must be a positive integer.");
        return false;
    }
    if (Command.PlayId <= LastSeenPlayId)
    {
        OutErrorCode = TEXT("CACHE_METADATA_INVALID");
        OutDetails = FString::Printf(
            TEXT("play_id %d must increase within the streaming session"
                 " (last seen %d)."),
            Command.PlayId,
            LastSeenPlayId);
        return false;
    }
    const bool bHasCache =
        State == EMtoUCacheState::Ready
        || State == EMtoUCacheState::Stopped
        || State == EMtoUCacheState::Completed
        || State == EMtoUCacheState::Failed;
    if (!bHasCache)
    {
        OutErrorCode = TEXT("CACHE_NOT_READY");
        OutDetails = FString::Printf(
            TEXT("cache_play arrived while the cache state is %s."), ToString(State));
        return false;
    }
    LastSeenPlayId = Command.PlayId;
    ActivePlayId = Command.PlayId;
    NextFrame = 0;
    AppliedCount = 0;
    PlaybackStart = Now();
    State = EMtoUCacheState::Playing;
    // cache_play only initializes the attempt: every pose, including the
    // first, is published by a later Tick so one game-thread update can never
    // apply two cached poses.
    return true;
}

void FMtoUCacheSession::FailPerformance(const FString& Details)
{
    UE_LOG(LogMtoUCacheSession, Warning,
        TEXT("Cached playback stopped without dropping frames: %s"), *Details);
    State = EMtoUCacheState::Failed;
    ErrorDetails = FString::Printf(
        TEXT("CACHED_PLAYBACK_PERFORMANCE: %s; replay stopped without dropping frames."),
        *Details);
}

void FMtoUCacheSession::ApplyNextPose()
{
    if (NextFrame >= Frames.Num())
    {
        return;
    }
    const double Due = PlaybackStart + static_cast<double>(NextFrame) * FrameInterval();
    const double NowValue = Now();
    if (NowValue < Due)
    {
        return;
    }
    // Valid source-frame window check: this pose must still be inside its own
    // scheduled slot. Once the window is missed we stop BEFORE publishing, so
    // several overdue poses can never collapse into one visible catch-up burst.
    if (NowValue - Due >= FrameInterval())
    {
        FailPerformance(FString::Printf(
            TEXT("the valid source-frame window for cached frame %d was missed"
                 " %.3fs ago at the captured rate of %g fps"),
            NextFrame,
            NowValue - Due,
            Begin.Fps));
        return;
    }
    bool bAccepted = true;
    if (Publish)
    {
        bAccepted = Publish(Frames[NextFrame]);
    }
    if (!bAccepted)
    {
        // Applied evidence never advances past a refused publication; the
        // next update either publishes it in window or fails on the window.
        return;
    }
    LastAppliedIndex = NextFrame;
    ++AppliedCount;
    ++NextFrame;

    if (ProgressSink)
    {
        ProgressSink(ActivePlayId, AppliedCount);
    }

    if (NextFrame >= Frames.Num())
    {
        // Completion guard: successful completion requires every pose
        // accepted exactly once in order plus the elapsed monotonic duration
        // remaining within the captured-rate requirement.
        const double Schedule = static_cast<double>(Frames.Num()) * FrameInterval();
        const double Tolerance = FMath::Min(0.5, FMath::Max(0.05 * Schedule, FrameInterval()));
        if (NowValue - PlaybackStart > Schedule + Tolerance)
        {
            FailPerformance(FString::Printf(
                TEXT("local playback of %d cached frames took %.3fs for a %.3fs schedule"),
                Frames.Num(),
                NowValue - PlaybackStart,
                Schedule));
            return;
        }
        ElapsedSeconds = NowValue - PlaybackStart;
        // Successful completion: the final accepted pose stays held.
        State = EMtoUCacheState::Completed;
    }
}

FMtoUCacheTransition FMtoUCacheSession::Tick()
{
    FMtoUCacheTransition Result;
    if (State != EMtoUCacheState::Playing)
    {
        return Result;
    }
    const int32 Before = AppliedCount;
    ApplyNextPose();
    Result.AppliedFramesThisTick = AppliedCount - Before;
    Result.UploadId = ActiveUploadId;
    Result.PlayId = ActivePlayId;
    Result.FrameCount = AppliedCount;
    Result.ElapsedSeconds = ElapsedSeconds;
    if (State == EMtoUCacheState::Completed)
    {
        Result.Kind = FMtoUCacheTransition::EKind::Completed;
        // Preserve the existing realtime override to keep the final pose
        // naturally evaluated. Completion never changes viewport policy.
    }
    else if (State == EMtoUCacheState::Failed)
    {
        Result.Kind = FMtoUCacheTransition::EKind::PerformanceFailed;
        Result.ErrorCode = TEXT("CACHED_PLAYBACK_PERFORMANCE");
        Result.Details = ErrorDetails;
        Result.RealtimeOverride = false;
    }
    return Result;
}
