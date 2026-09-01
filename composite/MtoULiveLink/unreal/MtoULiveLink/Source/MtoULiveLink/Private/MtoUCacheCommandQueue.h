#pragma once

#include "MtoUCacheSession.h"

// Bounded producer/consumer intake for cache commands travelling from the
// network worker to the Game Thread. A parsed cache frame only gains queued
// ownership after the queue's frozen frame/byte admission budget accepts it,
// so a stalled Game Thread can never grow queued parsed-frame memory without
// limit. The primary throttle keeps the raw, never-parsed payload in the
// worker and pauses the socket reader (TCP backpressure) without blocking it,
// so disconnect stays observable; the in-queue wait is only reachable under a
// control-message flood and is released by shutdown or a session-termination
// request. Rejected, superseded, or cleared uploads are cancelled wholesale
// by session and upload identity so they can never affect a newer attempt.
class FMtoUCacheCommandQueue
{
public:
    // Producer-side admission probe performed before parsing a cache frame:
    // parsing must never begin when queued parsed-frame ownership already
    // holds the frozen frame/byte budget.
    bool CanAdmitFrame(int64 EncodedBytes) const;

    // Producer side. Returns false when the command was aborted before it
    // gained queued ownership; control commands are never held back by the
    // frame budget.
    bool Produce(FMtoUCacheCommand Command, const TFunctionRef<bool()> ShouldAbort);

    // Consumer (Game Thread) side. Returns false when nothing is queued.
    bool TryConsume(FMtoUCacheCommand& OutCommand);

    // Drops every pending command of one streaming session.
    int32 CancelSession(uint64 SessionId);

    // Drops pending Frame/End commands of one upload identity within one
    // streaming session.
    int32 CancelUpload(uint64 SessionId, int32 UploadId);

    int32 GetPendingFrameCount() const;
    int64 GetPendingBytes() const;

private:
    void ReleaseOwnership(const FMtoUCacheCommand& Command);

    mutable FCriticalSection Mutex;
    TArray<FMtoUCacheCommand> Pending;
    int32 PendingFrames = 0;
    int64 PendingBytes = 0;
};
