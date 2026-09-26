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
    int32 AppliedFrames = 0;
    FString ErrorDetails;

    bool CanPlay() const
    {
        return bConnected && FrameCount > 0
            && (State == EMtoUCacheState::Ready || State == EMtoUCacheState::Stopped
                || State == EMtoUCacheState::Completed || State == EMtoUCacheState::Failed);
    }

    bool CanStop() const { return bConnected && State == EMtoUCacheState::Playing; }
};
