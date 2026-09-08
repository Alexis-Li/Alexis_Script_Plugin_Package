#include "MtoUCacheCommandQueue.h"

#include "HAL/PlatformProcess.h"

namespace
{
// Only parsed cache frames carry a real owned-memory weight; control commands
// are tiny and must always be able to make progress so a full frame budget can
// never strand cache_clear, cache_end, or teardown signals. The frame budget is
// enforced by CanAdmitFrame/HeldCacheFrame (never a blocking producer wait);
// this entry ceiling is the backstop that only a control-message flood can
// reach, so the +16 slack is control-headroom, not a producer deadlock point.
constexpr int32 MaxQueuedCacheEntries =
    static_cast<int32>(FMtoUProtocol::MaxQueuedCacheFrames) + 16;

int64 ParsedWeightBytes(const FMtoUCacheCommand& Command)
{
    if (Command.Kind != FMtoUCacheCommand::EKind::Frame)
    {
        return 0;
    }
    return FMath::Max<int64>(Command.EncodedBytes, 0);
}
}

bool FMtoUCacheCommandQueue::CanAdmitFrame(int64 EncodedBytes) const
{
    FScopeLock Lock(&Mutex);
    return PendingFrames < FMtoUProtocol::MaxQueuedCacheFrames
        && PendingBytes + FMath::Max<int64>(EncodedBytes, 0)
            <= FMtoUProtocol::MaxQueuedCacheBytes;
}

bool FMtoUCacheCommandQueue::Produce(
    FMtoUCacheCommand Command, const TFunctionRef<bool()> ShouldAbort)
{
    const bool bParsedFrame = Command.Kind == FMtoUCacheCommand::EKind::Frame;
    const int64 WeightBytes = ParsedWeightBytes(Command);
    while (true)
    {
        {
            FScopeLock Lock(&Mutex);
            const bool bAdmit = !bParsedFrame
                || (PendingFrames < FMtoUProtocol::MaxQueuedCacheFrames
                    && PendingBytes + WeightBytes <= FMtoUProtocol::MaxQueuedCacheBytes);
            const bool bRoom = Pending.Num() < MaxQueuedCacheEntries;
            if (bAdmit && bRoom)
            {
                if (bParsedFrame)
                {
                    ++PendingFrames;
                    PendingBytes += WeightBytes;
                }
                Pending.Add(MoveTemp(Command));
                return true;
            }
        }
        if (ShouldAbort())
        {
            return false;
        }
        FPlatformProcess::Sleep(0.001f);
    }
}

bool FMtoUCacheCommandQueue::TryConsume(FMtoUCacheCommand& OutCommand)
{
    FScopeLock Lock(&Mutex);
    if (Pending.Num() == 0)
    {
        return false;
    }
    OutCommand = MoveTemp(Pending[0]);
    Pending.RemoveAt(0, 1, EAllowShrinking::No);
    ReleaseOwnership(OutCommand);
    return true;
}

// ponytail: O(n) scans over a <=528-entry TArray under one lock (n=frame+control
// budget); a ring-buffer with head/tail accounting only matters if profiling
// ever shows intake cancellation showing up on the Game Thread clock.
int32 FMtoUCacheCommandQueue::CancelSession(uint64 SessionId)
{
    FScopeLock Lock(&Mutex);
    int32 Removed = 0;
    for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
    {
        if (Pending[Index].SessionId == SessionId)
        {
            ReleaseOwnership(Pending[Index]);
            Pending.RemoveAt(Index, 1, EAllowShrinking::No);
            ++Removed;
        }
    }
    return Removed;
}

int32 FMtoUCacheCommandQueue::CancelUpload(uint64 SessionId, int32 UploadId)
{
    FScopeLock Lock(&Mutex);
    int32 Removed = 0;
    for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
    {
        const FMtoUCacheCommand& Command = Pending[Index];
        const bool bFrameLike = Command.Kind == FMtoUCacheCommand::EKind::Frame
            || Command.Kind == FMtoUCacheCommand::EKind::End;
        if (bFrameLike && Command.SessionId == SessionId && Command.UploadId == UploadId)
        {
            ReleaseOwnership(Pending[Index]);
            Pending.RemoveAt(Index, 1, EAllowShrinking::No);
            ++Removed;
        }
    }
    return Removed;
}

int32 FMtoUCacheCommandQueue::GetPendingFrameCount() const
{
    FScopeLock Lock(&Mutex);
    return PendingFrames;
}

int64 FMtoUCacheCommandQueue::GetPendingBytes() const
{
    FScopeLock Lock(&Mutex);
    return PendingBytes;
}

int32 FMtoUCacheCommandQueue::GetPendingCommandCount() const
{
    FScopeLock Lock(&Mutex);
    return Pending.Num();
}

void FMtoUCacheCommandQueue::ReleaseOwnership(const FMtoUCacheCommand& Command)
{
    if (Command.Kind == FMtoUCacheCommand::EKind::Frame)
    {
        --PendingFrames;
        PendingBytes -= ParsedWeightBytes(Command);
        if (PendingFrames < 0)
        {
            PendingFrames = 0;
        }
        if (PendingBytes < 0)
        {
            PendingBytes = 0;
        }
    }
}
