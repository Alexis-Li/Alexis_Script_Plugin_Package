#pragma once

#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "ILiveLinkSource.h"
#include "MtoUCacheCommandQueue.h"
#include "MtoUCacheSession.h"
#include "MtoULiveLinkProtocol.h"

class AMtoULiveLinkActor;
class UWorld;
class FRunnableThread;
class ILiveLinkClient;
class FSocket;

/**
 * The one idempotent session-termination boundary shared by the Live Link
 * source and the Binding actor. Any active streaming session ends when its
 * Preview revision is invalidated or the Binding actor's lifetime ends, so a
 * new Preview revision can never receive frames negotiated for an older
 * Character snapshot. Requests made while no session is active are no-ops;
 * a new connection is always required to stream the newer revision.
 * An explicit Preview refresh ends its actor's session through the same
 * boundary before replacing the display (Issue #39).
 */
void MtoURequestStreamingSessionEnd();

/** Current value of the idempotent termination counter for publish gating. */
uint64 MtoUGetStreamingSessionEndCount();

/**
 * The world-unload seam of the same idempotent termination boundary: the
 * Editor world that owns the active session's Binding Actor is being cleaned
 * up. UWorld::DestroyWorld and world unloading never call Actor::Destroyed(),
 * so the synchronous FWorldDelegates::OnWorldCleanup broadcast is the
 * authoritative unload seam. A late BeginDestroy cannot use the identity-free
 * boundary because it could terminate a newer session; this hook runs in
 * order with the actual unload instead.
 */
void MtoUNotifyEditorWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);

struct FMtoUOutgoing
{
    uint64 SessionId = 0;
    TArray<uint8> Packet;
    int32 ExpectedBoneCount = 0;
    int32 ExpectedCurveCount = 0;
    bool bReady = false;
    bool bCloseAfter = false;
};

struct FMtoUPendingInit
{
    uint64 SessionId = 0;
    FMtoUInitMessage Message;
};

struct FMtoUPendingFrame
{
    uint64 SessionId = 0;
    FMtoUFrameMessage Message;
};

class MTOULIVELINK_API FMtoULiveLinkSource final : public ILiveLinkSource,
                                  public FRunnable,
                                  public TSharedFromThis<FMtoULiveLinkSource>
{
public:
    explicit FMtoULiveLinkSource(uint16 InPort = 54321);
    virtual ~FMtoULiveLinkSource() override;

    virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;
    virtual void InitializeSettings(ULiveLinkSourceSettings* Settings) override;
    virtual void Update() override;
    virtual bool IsSourceStillValid() const override;
    virtual bool RequestSourceShutdown() override;
    virtual FText GetSourceType() const override;
    virtual FText GetSourceMachineName() const override;
    virtual FText GetSourceStatus() const override;

    virtual uint32 Run() override;
    virtual void Stop() override;

    bool StartListener();
    void StopListener();

    // Admission intake gauge used by automation to prove a stalled Game Thread
    // cannot grow queued parsed-cache ownership beyond the frozen budget.
    int32 GetQueuedCacheFrameCount() const;

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FMtoUSessionIsolationTestAccess;
#endif
    void HandleInitOnGameThread(FMtoUInitMessage&& Message);
    void HandleCacheCommandsOnGameThread();
    bool DispatchCacheCommandOnGameThread(const FMtoUCacheCommand& Command);
    bool PublishFrameOnGameThread(const FMtoUFrameMessage& Frame);
    void PublishLatestFrameOnGameThread();
    void EnqueueReplyPacketOnGameThread(uint64 SessionId, TArray<uint8> Packet, bool bCloseAfter);
    void EnqueueErrorOnGameThread(
        const FString& Code,
        const FString& Message,
        const FString& Details = FString());
    bool IsCurrentSession(uint64 SessionId) const;
    // True only while the session still owns GameThread publication: the
    // worker still considers it current and no explicit refresh has ended it
    // since negotiation. A refresh increments the termination counter before
    // replacing the display, so old queued frames and cached commands fail
    // this gate synchronously without waiting for the worker to close.
    bool IsSessionPublishableOnGameThread(uint64 SessionId) const;
    void SetStatus(const FString& InStatus);

    const uint16 ConfiguredPort;
    TAtomic<bool> bStopRequested{false};
    TAtomic<bool> bSourceValid{true};
    TUniquePtr<FRunnableThread> Thread;

    mutable FCriticalSection StatusMutex;
    FString Status;

    mutable FCriticalSection PendingMutex;
    uint64 CurrentWorkerSession = 0;
    TOptional<FMtoUPendingInit> PendingInit;
    TOptional<FMtoUPendingFrame> PendingFrame;
    TQueue<uint64, EQueueMode::Spsc> DisconnectedSessions;
    TQueue<FMtoUOutgoing, EQueueMode::Spsc> OutgoingReplies;
    FMtoUCacheCommandQueue CacheCommands;

    ILiveLinkClient* Client = nullptr;
    FGuid SourceGuid;
    FLiveLinkSubjectKey SubjectKey;
    uint64 GameThreadSession = 0;
    // Termination-counter value observed when GameThreadSession was
    // negotiated. An explicit refresh increments the counter before replacing
    // the display, so any later publish for the older epoch fails closed even
    // before the worker closes the socket.
    uint64 GameThreadSessionEndEpoch = 0;
    int32 ExpectedBoneCount = 0;
    int32 ExpectedCurveCount = 0;
    // Authoritative character snapshot revision from the accepted init.
    int32 NegotiatedRevision = 0;
    TArray<FTransform> SourceBindLocalPose;
    TArray<FTransform> TargetRefLocalPose;
    TArray<int32> BoneParents;
    TArray<int32> AcceptedCurveIndices;
    TArray<FName> AcceptedCurveNames;
    TArray<TWeakObjectPtr<AMtoULiveLinkActor>> ParticipatingActors;

    // Game-thread-only transient cache owner scoped to GameThreadSession.
    FMtoUCacheSession CacheSession;
    // Set when playback starts; one completion or performance-failure reply
    // is reported exactly once per replay attempt.
    bool bPlaybackOutcomePending = false;
};
