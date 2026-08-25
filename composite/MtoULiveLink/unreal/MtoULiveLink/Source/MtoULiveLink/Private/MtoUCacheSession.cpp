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
        case EMtoUCacheState::Receiving: return TEXT("Receiving");
        case EMtoUCacheState::Ready: return TEXT("Ready");
        case EMtoUCacheState::Playing: return TEXT("Playing");
        case EMtoUCacheState::Completed: return TEXT("Completed");
        case EMtoUCacheState::Stopped: return TEXT("Stopped");
        case EMtoUCacheState::Failed: return TEXT("Failed");
    }
    return TEXT("Unknown");
}
}

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

void FMtoUCacheSession::SetValidationCounts(int32 InExpectedTransformCount, int32 InExpectedCurveCount)
{
    ExpectedTransformCount = InExpectedTransformCount;
    ExpectedCurveCount = InExpectedCurveCount;
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
    NextFrame = 0;
    AppliedCount = 0;
    LastAppliedIndex = INDEX_NONE;
    PlaybackStart = 0.0;
}

bool FMtoUCacheSession::HandleCommand(
    const FMtoUCacheCommand& Command,
    FString& OutErrorCode,
    FString& OutDetails)
{
    ErrorDetails.Reset();
    bool bAccepted = false;
    switch (Command.Kind)
    {
        case FMtoUCacheCommand::EKind::Begin:
            bAccepted = HandleBegin(Command, OutErrorCode, OutDetails);
            break;
        case FMtoUCacheCommand::EKind::Frame:
            bAccepted = HandleFrame(Command, OutErrorCode, OutDetails);
            break;
        case FMtoUCacheCommand::EKind::End:
            bAccepted = HandleEnd(Command, OutErrorCode, OutDetails);
            break;
        case FMtoUCacheCommand::EKind::Play:
            bAccepted = HandlePlay(Command, OutErrorCode, OutDetails);
            break;
        case FMtoUCacheCommand::EKind::Stop:
            if (State == EMtoUCacheState::Playing)
            {
                // Manual stop holds the last applied pose and keeps the
                // completed cache available for replay-again.
                State = EMtoUCacheState::Stopped;
            }
            bAccepted = true;
            break;
        case FMtoUCacheCommand::EKind::Clear:
            ResetToIdle();
            bAccepted = true;
            break;
    }
    if (!bAccepted)
    {
        if (OutErrorCode.IsEmpty())
        {
            OutErrorCode = TEXT("CACHE_INVALID_STATE");
            OutDetails = TEXT("Unknown cache command failure.");
        }
        ErrorDetails = OutErrorCode + TEXT(": ") + OutDetails;
    }
    // On success ErrorDetails keeps whatever FailPerformance wrote during
    // this command (a play can fail immediately); it was cleared above.
    return bAccepted;
}

bool FMtoUCacheSession::HandleBegin(
    const FMtoUCacheCommand& Command,
    FString& OutErrorCode,
    FString& OutDetails)
{
    (void)OutErrorCode;
    (void)OutDetails;
    // Recapture always replaces the previous cache coherently; old and new
    // frames can never mix because Begin resets the buffer unconditionally.
    Begin = Command.Begin;
    Frames.Reset(Begin.FrameCount);
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
    if (Command.Revision != Begin.Revision)
    {
        OutErrorCode = TEXT("CACHE_REVISION_MISMATCH");
        OutDetails = FString::Printf(
            TEXT("Buffered revision %d does not match requested revision %d."),
            Begin.Revision,
            Command.Revision);
        return false;
    }
    NextFrame = 0;
    AppliedCount = 0;
    PlaybackStart = Now();
    State = EMtoUCacheState::Playing;
    ApplyNext();
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

void FMtoUCacheSession::ApplyNext()
{
    const double CallStart = Now();
    int32 AppliedThisCall = 0;
    while (State == EMtoUCacheState::Playing && NextFrame < Frames.Num())
    {
        const double Due = PlaybackStart + static_cast<double>(NextFrame) * FrameInterval();
        if (Now() < Due)
        {
            return;
        }
        if (Publish)
        {
            Publish(Frames[NextFrame]);
        }
        LastAppliedIndex = NextFrame;
        ++AppliedCount;
        ++NextFrame;
        ++AppliedThisCall;

        if (NextFrame >= Frames.Num())
        {
            // Completion guard: a successful review must actually run at the
            // captured scene rate. A long tick stall followed by an instant
            // catch-up burst applied every frame once, but it silently
            // stretched the review, so it reports a performance failure
            // instead of success. The tolerance absorbs tick jitter without
            // letting short clips pass a multi-fold stretch.
            const double Schedule = static_cast<double>(Frames.Num() - 1) * FrameInterval();
            const double Tolerance = FMath::Min(0.5, FMath::Max(0.05 * Schedule, FrameInterval()));
            if (Now() - PlaybackStart > Schedule + Tolerance)
            {
                FailPerformance(FString::Printf(
                    TEXT("local playback of %d cached frames took %.3fs for a %.3fs schedule"
                         " before cached frame %d"),
                    Frames.Num(),
                    Now() - PlaybackStart,
                    Schedule,
                    NextFrame));
                return;
            }
            // Successful completion: every buffered frame was applied exactly
            // once in order and the final pose stays held.
            State = EMtoUCacheState::Completed;
            return;
        }
        // Underrun guard: applying the frames we just did must cost less wall
        // time than the schedule they consumed, with one interval of slack.
        // Otherwise this machine cannot sustain the captured scene rate and we
        // stop instead of silently skipping or stretching the review.
        const double ConsumedSchedule =
            static_cast<double>(AppliedThisCall) * FrameInterval();
        if (Now() - CallStart > ConsumedSchedule + FrameInterval())
        {
            FailPerformance(FString::Printf(
                TEXT("applying %d cached frames could not keep up with the captured rate"),
                AppliedThisCall));
            return;
        }
    }
}

int32 FMtoUCacheSession::Tick()
{
    const int32 Before = AppliedCount;
    if (State == EMtoUCacheState::Playing && NextFrame < Frames.Num())
    {
        ApplyNext();
    }
    return AppliedCount - Before;
}
