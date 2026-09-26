#pragma once

#include "MtoULiveLinkProtocol.h"
#include "MtoUCachePlayback.h"

// Game-thread owner of one negotiated connection's transient animation cache.
// It buffers a fully validated, byte-metered upload, gates playback behind an
// atomic Ready transition, and replays buffered poses at the captured scene
// rate using an injectable monotonic clock: at most one pose per source-frame
// position per update, and never a catch-up burst. It never touches packages
// or disk.

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
    };

    // Transport identity: commands from older streaming sessions are ignored.
    uint64 SessionId = 0;
    EKind Kind = EKind::Enter;
    int32 Index = 0;
    int32 PlayId = 0;
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
        None, Entered, Receiving, Ready, Playing, Stopped, Cleared, Completed,
        Rejected, PerformanceFailed,
    };

    EKind Kind = EKind::None;
    bool bAccepted = true;
    // Unset means preserve the existing override (including on completion).
    // The source alone applies this demand to editor viewports, and only for
    // a still-publishable streaming session.
    TOptional<bool> RealtimeOverride;
    int32 UploadId = INDEX_NONE;
    int32 PlayId = INDEX_NONE;
    int32 Revision = 0;
    int32 FrameCount = 0;
    int32 AppliedFramesThisTick = 0;
    double ElapsedSeconds = 0.0;
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

    // Bounded informational progress for the current play attempt.
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
    int32 GetAppliedFrameCount() const { return AppliedCount; }
    int32 GetLastAppliedIndex() const { return LastAppliedIndex; }
    int32 GetActiveUploadId() const { return ActiveUploadId; }
    int32 GetActivePlayId() const { return ActivePlayId; }
    FMtoUCachePlaybackView GetView() const;

    // The Editor actor starts and stops a complete cache on the Game Thread.
    // These use the same transition and identity checks as protocol commands.
    FMtoUCacheTransition StartLocalPlayback();
    FMtoUCacheTransition StopLocalPlayback();

    // Captures identities before destructive transitions; callers never need
    // to reconstruct an outcome from the command and post-transition getters.
    FMtoUCacheTransition HandleCommand(const FMtoUCacheCommand& Command);

    // Advances local replay and emits completion/failure only on the tick
    // that enters that terminal state. Later ticks produce no outcome.
    FMtoUCacheTransition Tick();

private:
    // Drops any buffered cache and returns to Idle. Used by clear and
    // per-session teardown.
    void ResetToIdle();

    bool HandleEnter(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandleBegin(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandleFrame(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandleEnd(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandlePlay(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    void ApplyNextPose();
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
    int32 ActiveUploadId = 0;
    int32 ActivePlayId = 0;
    int64 ActualPayloadBytes = 0;
    int32 NextFrame = 0;
    int32 AppliedCount = 0;
    int32 LastAppliedIndex = INDEX_NONE;
    double PlaybackStart = 0.0;
    double ElapsedSeconds = 0.0;
    FString ErrorDetails;
};
