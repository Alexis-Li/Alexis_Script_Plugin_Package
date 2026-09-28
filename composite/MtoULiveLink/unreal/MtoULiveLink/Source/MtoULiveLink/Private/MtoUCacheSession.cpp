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
        case EMtoUCacheState::Paused: return TEXT("Paused");
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
    SegmentFrameCount = 0;
    SegmentStartIndex = 0;
    ScheduleSlots = 0;
    SegmentStartSlot = 0;
    ScheduleBaseline = 0.0;
    LoopRound = 0;
    LastAppliedIndex = INDEX_NONE;
    ElapsedSeconds = 0.0;
    PausedAt = 0.0;
    bLoopEnabled = false;
    bSeekedInAttempt = false;
    bWrappedInAttempt = false;
    bPositionedBySeek = false;
    ActiveUploadId = 0;
    ActivePlayId = 0;
}

void FMtoUCacheSession::EndSession()
{
    ResetToIdle();
    LastSeenUploadId = 0;
    LastSeenPlayId = 0;
    LastSeenClearId = 0;
    ExpectedTransformCount = INDEX_NONE;
    ExpectedCurveCount = INDEX_NONE;
    NegotiatedRevision = 0;
    ErrorDetails.Reset();
}

int32 FMtoUCacheSession::CurrentSourceFrame() const
{
    return LastAppliedIndex == INDEX_NONE
        ? INDEX_NONE
        : Begin.StartFrame + LastAppliedIndex;
}

void FMtoUCacheSession::BeginAttempt(int32 PlayId)
{
    LastSeenPlayId = PlayId;
    ActivePlayId = PlayId;
    NextFrame = 0;
    SegmentFrameCount = Frames.Num();
    SegmentStartIndex = 0;
    ScheduleSlots = 0;
    SegmentStartSlot = 0;
    ScheduleBaseline = Now();
    LoopRound = 0;
    ElapsedSeconds = 0.0;
    PausedAt = 0.0;
    bSeekedInAttempt = false;
    bWrappedInAttempt = false;
    bPositionedBySeek = false;
    // The pose already displayed keeps its identity until the attempt applies
    // its own first pose; only a new upload invalidates it.
}

void FMtoUCacheSession::BeginSegment(int32 StartIndex, int32 FrameCount, int32 StartSlot)
{
    SegmentStartIndex = StartIndex;
    SegmentFrameCount = FrameCount;
    SegmentStartSlot = StartSlot;
}

FMtoUCachePlaybackView FMtoUCacheSession::GetView() const
{
    FMtoUCachePlaybackView View;
    View.State = State;
    View.StartFrame = Begin.StartFrame;
    View.EndFrame = Begin.EndFrame;
    View.Fps = Begin.Fps;
    View.FrameCount = Begin.FrameCount;
    View.AppliedFrames = AppliedInSegment();
    View.bLoopEnabled = bLoopEnabled;
    View.LoopRound = LoopRound;
    View.bPositionedBySeek = bPositionedBySeek;
    View.ErrorDetails = ErrorDetails;
    if (LastAppliedIndex != INDEX_NONE)
    {
        View.CurrentSourceFrame = Begin.StartFrame + LastAppliedIndex;
        View.bHasAppliedSourceFrame = true;
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
    if (!HasActiveAttempt())
    {
        return FMtoUCacheTransition();
    }
    FMtoUCacheCommand Command;
    Command.Kind = FMtoUCacheCommand::EKind::Stop;
    return HandleCommand(Command);
}

FMtoUCacheTransition FMtoUCacheSession::PauseLocalPlayback()
{
    FMtoUCacheCommand Command;
    Command.Kind = FMtoUCacheCommand::EKind::Pause;
    return HandleCommand(Command);
}

FMtoUCacheTransition FMtoUCacheSession::ResumeLocalPlayback()
{
    FMtoUCacheCommand Command;
    Command.Kind = FMtoUCacheCommand::EKind::Resume;
    return HandleCommand(Command);
}

FMtoUCacheTransition FMtoUCacheSession::SeekLocalPlayback(int32 SourceFrame)
{
    FMtoUCacheCommand Command;
    Command.Kind = FMtoUCacheCommand::EKind::Seek;
    Command.SeekSourceFrame = SourceFrame;
    return HandleCommand(Command);
}

FMtoUCacheTransition FMtoUCacheSession::SetLocalLoop(bool bEnabled)
{
    FMtoUCacheCommand Command;
    Command.Kind = FMtoUCacheCommand::EKind::SetLoop;
    Command.bLoopEnabled = bEnabled;
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
                Result.bLoopEnabled = bLoopEnabled;
            }
            break;
        case FMtoUCacheCommand::EKind::Stop:
            if (HasActiveAttempt())
            {
                State = EMtoUCacheState::Stopped;
                bPositionedBySeek = false;
            }
            Result.Kind = EKind::Stopped;
            Result.PlayId = ActivePlayId;
            Result.RealtimeOverride = false;
            bAccepted = true;
            break;
        case FMtoUCacheCommand::EKind::Pause:
            if (State == EMtoUCacheState::Paused)
            {
                // Repeated pause keeps the held pose and sends no second outcome.
                bAccepted = true;
                break;
            }
            bAccepted = HandlePause(Result.ErrorCode, Result.Details);
            Result.Kind = EKind::Paused;
            Result.PlayId = ActivePlayId;
            Result.SourceFrame = CurrentSourceFrame();
            Result.AppliedFrames = AppliedInSegment();
            break;
        case FMtoUCacheCommand::EKind::Resume:
            if (State == EMtoUCacheState::Playing)
            {
                // Repeated resume continues the running schedule unchanged.
                bAccepted = true;
                break;
            }
            bAccepted = HandleResume(Result.ErrorCode, Result.Details);
            Result.Kind = EKind::Resumed;
            Result.PlayId = ActivePlayId;
            Result.SourceFrame = CurrentSourceFrame();
            Result.AppliedFrames = AppliedInSegment();
            break;
        case FMtoUCacheCommand::EKind::Seek:
            bAccepted = HandleSeek(Command.SeekSourceFrame, Result.ErrorCode, Result.Details);
            Result.Kind = EKind::Seeked;
            Result.PlayId = ActivePlayId;
            Result.SourceFrame = CurrentSourceFrame();
            Result.AppliedFrames = AppliedInSegment();
            break;
        case FMtoUCacheCommand::EKind::SetLoop:
            return HandleLoopChange(Command.bLoopEnabled);
        case FMtoUCacheCommand::EKind::Clear:
            if (Command.ClearId <= LastSeenClearId)
            {
                Result.ErrorCode = TEXT("CACHE_METADATA_INVALID");
                Result.Details = TEXT("Clear ID must increase within the session.");
                break;
            }
            LastSeenClearId = Command.ClearId;
            Result.ClearId = Command.ClearId;
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
    SegmentFrameCount = 0;
    SegmentStartIndex = 0;
    ScheduleSlots = 0;
    SegmentStartSlot = 0;
    ScheduleBaseline = 0.0;
    LoopRound = 0;
    LastAppliedIndex = INDEX_NONE;
    ElapsedSeconds = 0.0;
    PausedAt = 0.0;
    // A fresh cache starts with loop off; the previous cache's playback
    // selection never survives a recapture.
    bLoopEnabled = false;
    bSeekedInAttempt = false;
    bWrappedInAttempt = false;
    bPositionedBySeek = false;
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
    // Every attempt opens its own whole-cache segment; only this fresh
    // segment (or a later unbroken continuation of it) may claim the
    // whole-cache completion scope.
    BeginAttempt(Command.PlayId);
    State = EMtoUCacheState::Playing;
    // Starting an attempt only initializes it: every pose, including the
    // first, is published by a later Tick so one game-thread update can never
    // apply two cached poses.
    return true;
}

bool FMtoUCacheSession::HandlePause(FString& OutErrorCode, FString& OutDetails)
{
    if (State != EMtoUCacheState::Playing)
    {
        OutErrorCode = TEXT("CACHE_NOT_READY");
        OutDetails = FString::Printf(
            TEXT("Pausing cached playback requires playing playback;"
                 " the cache state is %s."),
            ToString(State));
        return false;
    }
    // Holding the pose freezes the schedule: no frame becomes due while the
    // user inspects, so an arbitrarily long pause can never fail playback.
    PausedAt = Now();
    State = EMtoUCacheState::Paused;
    return true;
}

bool FMtoUCacheSession::HandleResume(FString& OutErrorCode, FString& OutDetails)
{
    if (State != EMtoUCacheState::Paused)
    {
        OutErrorCode = TEXT("CACHE_NOT_READY");
        OutDetails = FString::Printf(
            TEXT("Resuming cached playback requires paused playback;"
                 " the cache state is %s."),
            ToString(State));
        return false;
    }
    // Rebuilding the timing baseline continues from the held position: the
    // paused interval is never replayed and never counts as a late segment.
    ScheduleBaseline += Now() - PausedAt;
    PausedAt = 0.0;
    bPositionedBySeek = false;
    State = EMtoUCacheState::Playing;
    return true;
}

bool FMtoUCacheSession::HandleSeek(int32 SourceFrame, FString& OutErrorCode, FString& OutDetails)
{
    if (!HasCache())
    {
        OutErrorCode = TEXT("CACHE_NOT_READY");
        OutDetails = FString::Printf(
            TEXT("Seeking requires a received cache; the cache state is %s."),
            ToString(State));
        return false;
    }
    // Only frames this capture actually sampled are addressable; an
    // out-of-range request is reported instead of silently clamped.
    if (SourceFrame < Begin.StartFrame || SourceFrame > Begin.EndFrame)
    {
        OutErrorCode = TEXT("CACHE_SEEK_INVALID");
        OutDetails = FString::Printf(
            TEXT("source frame %d is not a sampled frame of the current cache"
                 " (%d..%d)."),
            SourceFrame,
            Begin.StartFrame,
            Begin.EndFrame);
        return false;
    }
    const int32 Index = SourceFrame - Begin.StartFrame;
    if (!Frames.IsValidIndex(Index))
    {
        OutErrorCode = TEXT("CACHE_SEEK_INVALID");
        OutDetails = FString::Printf(
            TEXT("source frame %d has no buffered cache frame."), SourceFrame);
        return false;
    }
    // The target is displayed immediately; only then does the seek hold.
    if (Publish && !Publish(Frames[Index]))
    {
        OutErrorCode = TEXT("CACHE_SEEK_FAILED");
        OutDetails = FString::Printf(
            TEXT("source frame %d could not be applied to the character."),
            SourceFrame);
        return false;
    }
    if (!HasActiveAttempt())
    {
        // Positioning before or after an attempt still opens one, so every
        // later outcome carries an increasing identity.
        ActivePlayId = LastSeenPlayId + 1;
        LastSeenPlayId = ActivePlayId;
    }
    bSeekedInAttempt = true;
    bPositionedBySeek = true;
    LastAppliedIndex = Index;
    // A seek opens a new segment: its schedule starts at the displayed frame
    // and only a later resume advances past it.
    NextFrame = Index + 1;
    BeginSegment(Index, Frames.Num() - Index, 0);
    ScheduleSlots = 1;
    ScheduleBaseline = Now();
    PausedAt = Now();
    State = EMtoUCacheState::Paused;
    return true;
}

FMtoUCacheTransition FMtoUCacheSession::HandleLoopChange(bool bEnabled)
{
    FMtoUCacheTransition Result;
    Result.Revision = NegotiatedRevision;
    ErrorDetails.Reset();
    if (bLoopEnabled == bEnabled)
    {
        // No outcome for a selection that already holds: repeated clicks can
        // never produce duplicate notifications.
        return Result;
    }
    if (!HasCache())
    {
        Result.Kind = FMtoUCacheTransition::EKind::Rejected;
        Result.bAccepted = false;
        Result.ErrorCode = TEXT("CACHE_NOT_READY");
        Result.Details = FString::Printf(
            TEXT("Selecting loop playback requires a received cache;"
                 " the cache state is %s."),
            ToString(State));
        ErrorDetails = Result.ErrorCode + TEXT(": ") + Result.Details;
        if (AcceptsLiveFrames())
        {
            Result.RealtimeOverride = true;
        }
        return Result;
    }
    bLoopEnabled = bEnabled;
    if (HasActiveAttempt())
    {
        Result.Kind = FMtoUCacheTransition::EKind::LoopChanged;
        Result.PlayId = ActivePlayId;
        Result.bLoopEnabled = bEnabled;
    }
    // Without an attempt the selection rides along in the next cache_playing.
    return Result;
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

FMtoUCacheSession::EStep FMtoUCacheSession::FinishSegment()
{
    if (bLoopEnabled)
    {
        // The last pose of the round was accepted; the next round continues
        // the same continuous schedule instead of restarting it.
        ++LoopRound;
        bWrappedInAttempt = true;
        BeginSegment(0, Frames.Num(), ScheduleSlots);
        NextFrame = 0;
        return EStep::Wrapped;
    }
    const int32 Applied = AppliedInSegment();
    const double Schedule = static_cast<double>(Applied) * FrameInterval();
    const double SegmentStart = ScheduleBaseline
        + static_cast<double>(SegmentStartSlot) * FrameInterval();
    const double Elapsed = Now() - SegmentStart;
    const double Tolerance = FMath::Min(0.5, FMath::Max(0.05 * Schedule, FrameInterval()));
    // Completion guard: successful completion requires every pose of the
    // segment accepted exactly once in order plus the active playback
    // duration remaining within the captured-rate requirement. A pause shifts
    // the baseline, so paused time is excluded rather than considered late.
    if (Applied != SegmentFrameCount || Elapsed > Schedule + Tolerance || Elapsed < 0.0)
    {
        FailPerformance(FString::Printf(
            TEXT("local playback of %d cached frames took %.3fs for a %.3fs schedule"),
            Applied,
            Elapsed,
            Schedule));
        return EStep::PerformanceFailed;
    }
    ElapsedSeconds = Elapsed;
    // Successful completion: the final accepted pose stays held.
    State = EMtoUCacheState::Completed;
    return EStep::Completed;
}

FMtoUCacheSession::EStep FMtoUCacheSession::ApplyNextPose()
{
    if (NextFrame >= Frames.Num())
    {
        // The segment's final pose was already applied by the previous update
        // or by a seek onto it; this update only settles the outcome.
        return FinishSegment();
    }
    const double Due =
        ScheduleBaseline + static_cast<double>(ScheduleSlots) * FrameInterval();
    const double NowValue = Now();
    if (NowValue < Due)
    {
        return EStep::None;
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
        return EStep::PerformanceFailed;
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
        return EStep::None;
    }
    LastAppliedIndex = NextFrame;
    ++ScheduleSlots;
    ++NextFrame;

    if (ProgressSink)
    {
        ProgressSink(ActivePlayId, AppliedInSegment());
    }

    if (NextFrame >= Frames.Num())
    {
        return FinishSegment();
    }
    return EStep::Applied;
}

FMtoUCacheTransition FMtoUCacheSession::Tick()
{
    FMtoUCacheTransition Result;
    if (State != EMtoUCacheState::Playing)
    {
        return Result;
    }
    using EKind = FMtoUCacheTransition::EKind;
    const int32 SlotsBefore = ScheduleSlots;
    const EStep Step = ApplyNextPose();
    Result.AppliedFramesThisTick = ScheduleSlots - SlotsBefore;
    Result.UploadId = ActiveUploadId;
    Result.PlayId = ActivePlayId;
    Result.AppliedFrames = AppliedInSegment();
    Result.FrameCount = AppliedInSegment();
    Result.ElapsedSeconds = ElapsedSeconds;
    switch (Step)
    {
        case EStep::Wrapped:
            Result.Kind = EKind::Looped;
            Result.LoopRound = LoopRound;
            Result.SourceFrame = Begin.StartFrame;
            break;
        case EStep::Completed:
            Result.Kind = EKind::Completed;
            // Only an unbroken whole-cache segment may claim the whole-cache
            // scope; a seek or a completed loop round ends a segment instead.
            Result.bWholeCacheScope = !bSeekedInAttempt && !bWrappedInAttempt;
            Result.StartFrame = Begin.StartFrame + SegmentStartIndex;
            Result.EndFrame = Begin.StartFrame + SegmentStartIndex + SegmentFrameCount - 1;
            break;
        case EStep::PerformanceFailed:
            Result.Kind = EKind::PerformanceFailed;
            Result.ErrorCode = TEXT("CACHED_PLAYBACK_PERFORMANCE");
            Result.Details = ErrorDetails;
            Result.RealtimeOverride = false;
            break;
        case EStep::None:
        case EStep::Applied:
        default:
            break;
    }
    return Result;
}
