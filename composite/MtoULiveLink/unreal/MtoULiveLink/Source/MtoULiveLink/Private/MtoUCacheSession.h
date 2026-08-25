#pragma once

#include "MtoULiveLinkProtocol.h"

// Game-thread owner of one negotiated connection's transient animation cache.
// It buffers a fully validated upload, gates playback behind an atomic Ready
// transition, and replays the buffered frames on the captured scene rate using
// an injectable monotonic clock. It never touches packages or disk.

enum class EMtoUCacheState : uint8
{
    Idle,
    Receiving,
    Ready,
    Playing,
    Completed,
    Stopped,
    Failed,
};

struct FMtoUCacheCommand
{
    enum class EKind : uint8
    {
        Begin,
        Frame,
        End,
        Play,
        Stop,
        Clear,
    };

    // Transport identity: commands from older streaming sessions are ignored.
    uint64 SessionId = 0;
    EKind Kind = EKind::Begin;
    int32 Index = 0;
    int32 Revision = 0;
    FMtoUCacheBeginMessage Begin;
    FMtoUFrameMessage Frame;
};

class FMtoUCacheSession
{
public:
    using FNow = TFunction<double()>;

    // Publish applies one buffered pose exactly once, in order.
    using FPublish = TFunction<void(const FMtoUFrameMessage&)>;

    FMtoUCacheSession();

    void SetClock(FNow InNow);
    void SetPublish(FPublish InPublish);
    void SetValidationCounts(int32 ExpectedTransformCount, int32 ExpectedCurveCount);

    EMtoUCacheState GetState() const { return State; }
    const FString& GetErrorDetails() const { return ErrorDetails; }
    int32 GetBufferedFrameCount() const { return Frames.Num(); }
    int32 GetExpectedFrameCount() const { return Begin.FrameCount; }
    int32 GetAppliedFrameCount() const { return AppliedCount; }
    int32 GetLastAppliedIndex() const { return LastAppliedIndex; }

    // Returns false and fills the stable protocol error code when the
    // command violates the frozen contract. Rejected uploads drop to Idle.
    bool HandleCommand(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);

    // Advances local replay; returns the number of frames applied this tick.
    int32 Tick();

    // Drops any buffered cache and returns to Idle. Used by recapture,
    // clear, and per-session teardown.
    void ResetToIdle();

private:
    bool HandleBegin(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandleFrame(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandleEnd(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    bool HandlePlay(const FMtoUCacheCommand& Command, FString& OutErrorCode, FString& OutDetails);
    void ApplyNext();
    void FailPerformance(const FString& Details);
    double FrameInterval() const;

    FNow Now;
    FPublish Publish;
    EMtoUCacheState State = EMtoUCacheState::Idle;
    FMtoUCacheBeginMessage Begin;
    TArray<FMtoUFrameMessage> Frames;
    int32 ExpectedTransformCount = INDEX_NONE;
    int32 ExpectedCurveCount = INDEX_NONE;
    int32 NextFrame = 0;
    int32 AppliedCount = 0;
    int32 LastAppliedIndex = INDEX_NONE;
    double PlaybackStart = 0.0;
    FString ErrorDetails;
};
