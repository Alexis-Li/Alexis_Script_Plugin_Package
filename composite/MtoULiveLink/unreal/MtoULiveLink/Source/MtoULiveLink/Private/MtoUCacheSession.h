#pragma once

#include "MtoULiveLinkProtocol.h"
#include "MtoUCachePlayback.h"

// Game-thread owner of one negotiated connection's transient animation cache.
// It buffers a fully validated, byte-metered upload, gates playback behind an
// atomic Ready transition, and replays buffered poses at the captured scene
// rate using an injectable monotonic clock: at most one pose per source-frame
// position per update, and never a catch-up burst. It never touches packages
// or disk.
//
// Local playback is organized in segments. A segment is one ordered run of
// buffered frames with its own timing baseline: starting an attempt or
// seeking opens one, resuming shifts its baseline past the paused interval
// without replaying it, and a loop round continues the same schedule instead
// of restarting it. Only a segment that covered the whole cache without a
// seek or a wrap may claim the whole-cache completion scope.

struct FMtoUCacheCommand
{
    enum class EKind : uint8
    {
        Enter,
        Begin,
        Reject,
        Frame,
        End,
        Play,
        Stop,
        Clear,
        Pause,
        Resume,
        Seek,
        SetLoop,
    };

    // Transport identity: commands from older streaming sessions are ignored.
    uint64 SessionId = 0;
    EKind Kind = EKind::Enter;
    int32 Index = 0;
    int32 PlayId = 0;
    int32 ClearId = 0;
    // Local-only seek target; INDEX_NONE when the command carries none.
    int32 SeekSourceFrame = INDEX_NONE;
    bool bLoopEnabled = false;
    // Owning upload identity stamped by the worker at intake so a rejected,
    // superseded, or cleared upload can drop all of its queued frame/end
    // commands together and can never affect a newer attempt.
    int32 UploadId = 0;
    // Actual encoded payload bytes of this message, metered at the framing
    // boundary and accumulated into the upload's resource accounting.
    int64 EncodedBytes = 0;
    // Worker-side pre-allocation rejection carried as a control command so the
    // Game Thread drops partial cache ownership without admitting a parsed
    // frame to the bounded queue.
    FString ErrorCode;
    FString ErrorDetails;
    FMtoUCacheBeginMessage Begin;
    FMtoUFrameMessage Frame;
};

struct FMtoUCacheSessionContext
{
    int32 TransformCount = INDEX_NONE;
    int32 CurveCount = INDEX_NONE;
    int32 Revision = 0;
};

/** A semantic result, consumed inline by the source behind its publication gate. */
struct FMtoUCacheTransition
{
    enum class EKind : uint8
    {
        None, Entered, Receiving, Ready, Playing, Paused, Resumed, Seeked, Looped,
        LoopChanged, Stopped, Cleared, Completed, Rejected, PerformanceFailed,
    };

    EKind Kind = EKind::None;
    bool bAccepted = true;
    // Unset means preserve the existing override (including on completion).
    // The source alone applies this demand to editor viewports, and only for
    // a still-publishable streaming session.
    TOptional<bool> RealtimeOverride;
    int32 UploadId = INDEX_NONE;
    int32 PlayId = INDEX_NONE;
    int32 ClearId = INDEX_NONE;
    int32 Revision = 0;
    int32 FrameCount = 0;
    int32 AppliedFramesThisTick = 0;
    double ElapsedSeconds = 0.0;
    // Held or positioned source frame for pause, resume, seek, and loop.
    int32 SourceFrame = INDEX_NONE;
    // Frames applied in the segment the outcome belongs to.
    int32 AppliedFrames = 0;
    // Loop transitions only.
    int32 LoopRound = 0;
    bool bLoopEnabled = false;
    // Completion evidence: a whole-cache scope is the only claim that this
    // attempt replayed the complete cache once, in order.
    bool bWholeCacheScope = false;
    int32 StartFrame = 0;
    int32 EndFrame = 0;
    FString ErrorCode;
    FString Details;
};

class FMtoUCacheSession
{
public:
    using FNow = TFunction<double()>;

    // Publish applies one buffered pose exactly once, in order. Returns
    // whether the publication path accepted the pose; applied evidence only
    // advances on acceptance.
    using FPublish = TFunction<bool(const FMtoUFrameMessage&)>;

    // Bounded informational progress for the current play segment.
    using FProgress = TFunction<void(int32 PlayId, int32 AppliedFrames)>;

    FMtoUCacheSession();

    void SetClock(FNow InNow);
    void SetPublish(FPublish InPublish);
    void SetProgressSink(FProgress InProgress);
    // Atomically replaces the previous session, including stale-ID history
    // and negotiated validation inputs. Publication dependencies survive.
    void BeginSession(const FMtoUCacheSessionContext& Context);
    void EndSession();

    bool AcceptsLiveFrames() const { return State == EMtoUCacheState::Idle; }

    EMtoUCacheState GetState() const { return State; }
    const FString& GetErrorDetails() const { return ErrorDetails; }
    int32 GetBufferedFrameCount() const { return Frames.Num(); }
    // Frames applied in the current segment; reset by a new segment.
    int32 GetAppliedFrameCount() const { return ScheduleSlots - SegmentStartSlot; }
    int32 GetLastAppliedIndex() const { return LastAppliedIndex; }
    int32 GetActiveUploadId() const { return ActiveUploadId; }
    int32 GetActivePlayId() const { return ActivePlayId; }
    int32 GetLoopRound() const { return LoopRound; }
    bool IsLoopEnabled() const { return bLoopEnabled; }
    FMtoUCachePlaybackView GetView() const;

    // The Editor actor starts, pauses, resumes, seeks, and loops a complete
    // cache on the Game Thread. These use the same transition and identity
    // checks as protocol commands.
    FMtoUCacheTransition StartLocalPlayback();
    FMtoUCacheTransition StopLocalPlayback();
    FMtoUCacheTransition PauseLocalPlayback();
    FMtoUCacheTransition ResumeLocalPlayback();
    // Seeks to a sampled Maya source frame, applies it, and holds it paused.
    // An out-of-range frame is refused with CACHE_SEEK_INVALID and never
    // silently clamped.
    FMtoUCacheTransition SeekLocalPlayback(int32 SourceFrame);
    FMtoUCacheTransition SetLocalLoop(bool bEnabled);

    // Captures identities before destructive transitions; callers never need
    // to reconstruct an outcome from the command and post-transition getters.
    FMtoUCacheTransition HandleCommand(const FMtoUCacheCommand& Command);

    // Advances local replay and emits completion/failure only on the tick
    // that enters that terminal state. Later ticks produce no outcome.
    FMtoUCacheTransition Tick();

private:
    enum class EStep : uint8
    {
        None,
        Applied,
        Wrapped,
        Completed,
        PerformanceFailed,
    };

    // Drops any buffered cache and returns to Idle. Used by clear and
    // per-session teardown.
    void ResetToIdle();

    bool HandleEnter(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandleBegin(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandleFrame(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandleEnd(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandlePlay(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandlePause(FString& OutErrorCode, FString& OutDetails);
    bool HandleResume(FString& OutErrorCode, FString& OutDetails);
    bool HandleSeek(int32 SourceFrame, FString& OutErrorCode, FString& OutDetails);
    FMtoUCacheTransition HandleLoopChange(bool bEnabled);

    bool HasCache() const
    {
        return State == EMtoUCacheState::Ready
            || State == EMtoUCacheState::Playing
            || State == EMtoUCacheState::Paused
            || State == EMtoUCacheState::Stopped
            || State == EMtoUCacheState::Completed
            || State == EMtoUCacheState::Failed;
    }

    // An attempt is active exactly while it can still advance: playing or held.
    bool HasActiveAttempt() const
    {
        return State == EMtoUCacheState::Playing || State == EMtoUCacheState::Paused;
    }

    // A seek or loop round restarting the attempt's cache-relative evidence
    // frees the whole-cache completion scope for the rest of the attempt.
    void BeginAttempt(int32 PlayId);
    void BeginSegment(int32 StartIndex, int32 FrameCount, int32 StartSlot);
    int32 AppliedInSegment() const { return ScheduleSlots - SegmentStartSlot; }
    int32 CurrentSourceFrame() const;
    EStep ApplyNextPose();
    EStep FinishSegment();
    void FailPerformance(const FString& Details);
    double FrameInterval() const;

    FNow Now;
    FPublish Publish;
    FProgress ProgressSink;
    EMtoUCacheState State = EMtoUCacheState::Idle;
    FMtoUCacheBeginMessage Begin;
    TArray<FMtoUFrameMessage> Frames;
    int32 ExpectedTransformCount = INDEX_NONE;
    int32 ExpectedCurveCount = INDEX_NONE;
    int32 NegotiatedRevision = 0;
    int32 LastSeenUploadId = 0;
    int32 LastSeenPlayId = 0;
    int32 LastSeenClearId = 0;
    int32 ActiveUploadId = 0;
    int32 ActivePlayId = 0;
    int64 ActualPayloadBytes = 0;
    int32 NextFrame = 0;
    // Frames the current segment will apply, from SegmentStartIndex.
    int32 SegmentFrameCount = 0;
    int32 SegmentStartIndex = 0;
    // Schedule position: one slot per applied pose since ScheduleBaseline.
    int32 ScheduleSlots = 0;
    int32 SegmentStartSlot = 0;
    double ScheduleBaseline = 0.0;
    int32 LoopRound = 0;
    int32 LastAppliedIndex = INDEX_NONE;
    double ElapsedSeconds = 0.0;
    // Paused playback holds the pose and never lets the schedule run late.
    double PausedAt = 0.0;
    bool bLoopEnabled = false;
    bool bSeekedInAttempt = false;
    bool bWrappedInAttempt = false;
    bool bPositionedBySeek = false;
    FString ErrorDetails;
};
