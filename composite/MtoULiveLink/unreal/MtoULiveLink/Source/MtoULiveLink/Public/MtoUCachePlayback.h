#pragma once

#include "CoreMinimal.h"

/** One transient cache owned by the current negotiated Animation session. */
enum class EMtoUCacheState : uint8
{
    Idle,
    Entered,
    Receiving,
    Ready,
    Playing,
    /** Playback or a seek is held: pose and cache stay, no frames are due. */
    Paused,
    Completed,
    Stopped,
    Failed,
};

/** Read-only actor view. bHasAppliedSourceFrame distinguishes an applied -1 from no pose. */
struct MTOULIVELINK_API FMtoUCachePlaybackView
{
    EMtoUCacheState State = EMtoUCacheState::Idle;
    bool bConnected = false;
    int32 StartFrame = 0;
    int32 EndFrame = 0;
    double Fps = 0.0;
    int32 FrameCount = 0;
    int32 CurrentSourceFrame = INDEX_NONE;
    bool bHasAppliedSourceFrame = false;
    /** Frames applied in the current playback segment, not since the attempt began. */
    int32 AppliedFrames = 0;
    bool bLoopEnabled = false;
    /** Completed loop rounds of the current attempt; zero when none wrapped yet. */
    int32 LoopRound = 0;
    /** The held pose came from a seek rather than from a pause. */
    bool bPositionedBySeek = false;
    FString ErrorDetails;

    bool CanPlay() const
    {
        return bConnected && FrameCount > 0
            && (State == EMtoUCacheState::Ready || State == EMtoUCacheState::Stopped
                || State == EMtoUCacheState::Completed || State == EMtoUCacheState::Failed);
    }

    bool CanStop() const
    {
        return bConnected
            && (State == EMtoUCacheState::Playing || State == EMtoUCacheState::Paused);
    }

    bool CanPause() const
    {
        return bConnected && State == EMtoUCacheState::Playing;
    }

    bool CanResume() const
    {
        return bConnected && State == EMtoUCacheState::Paused;
    }

    /** Seeking and loop selection need a complete cache, not an attempt. */
    bool CanSeek() const
    {
        return bConnected && FrameCount > 0
            && (State == EMtoUCacheState::Ready || State == EMtoUCacheState::Playing
                || State == EMtoUCacheState::Paused || State == EMtoUCacheState::Stopped
                || State == EMtoUCacheState::Completed || State == EMtoUCacheState::Failed);
    }
};
