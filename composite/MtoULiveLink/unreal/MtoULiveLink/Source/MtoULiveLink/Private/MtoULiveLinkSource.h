#pragma once

#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "ILiveLinkSource.h"
#include "MtoUCacheCommandQueue.h"
#include "MtoUCacheSession.h"
#include "MtoULinkageSession.h"
#include "MtoULiveLinkProtocol.h"

class AMtoULiveLinkActor;
class UWorld;
class FRunnableThread;
class ILiveLinkClient;
class FSocket;

TSharedPtr<class FMtoULiveLinkSource> MtoUSetActiveSource(
    TSharedPtr<class FMtoULiveLinkSource> Source);
FMtoUCachePlaybackView MtoUGetActorCachePlaybackView(const AMtoULiveLinkActor& Actor);
bool MtoUStartActorCachedPlayback(const AMtoULiveLinkActor& Actor);
bool MtoUStopActorCachedPlayback(const AMtoULiveLinkActor& Actor);
bool MtoUPauseActorCachedPlayback(const AMtoULiveLinkActor& Actor);
bool MtoUResumeActorCachedPlayback(const AMtoULiveLinkActor& Actor);
bool MtoUSeekActorCachedPlayback(const AMtoULiveLinkActor& Actor, int32 SourceFrame);
bool MtoUSetActorCachedLoop(const AMtoULiveLinkActor& Actor, bool bEnabled);

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
#if WITH_DEV_AUTOMATION_TESTS
/**
 * Test-only deterministic hold on the worker's disconnect cleanup: while set,
 * the worker keeps serving the pre-refresh socket and session instead of
 * closing on the termination counter, so automation can prove the
 * Game Thread publish gate refuses old frames before any worker cleanup.
 * Production builds never see this; automation must always release it.
 */
MTOULIVELINK_API void MtoUSetDeferStreamingSessionEndCleanup(bool bDefer);
#endif

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
    // Control messages (greeting, mode, camera/time, applied evidence,
    // reference offers and errors) count against the frozen per-direction
    // control queue; streamed cache outcomes do not.
    bool bControl = false;
};

struct FMtoUPendingHello
{
    uint64 SessionId = 0;
    FMtoUHelloMessage Message;
};

/**
 * One queued linkage control message from the network worker. Only the member
 * named by `Kind` is filled; the small control payloads share one queue so the
 * frozen control-queue budget is enforced in exactly one place.
 */
struct FMtoUPendingLinkageControl
{
    enum class EKind : uint8
    {
        Mode,
        LeaveLinkage,
        ReferenceResult,
        ReferenceRemoved,
        ReferenceQuery
    };

    EKind Kind = EKind::Mode;
    FMtoUModeMessage Mode;
    FMtoULeaveLinkageMessage LeaveLinkage;
    FMtoUReferenceResultMessage ReferenceResult;
    FMtoUReferenceRemovedMessage ReferenceRemoved;
    FMtoUReferenceQueryMessage ReferenceQuery;
};

struct FMtoUPendingPoseGroup
{
    uint64 SessionId = 0;
    FMtoUPoseGroupMessage Message;
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
    FMtoUCachePlaybackView GetActorCachePlaybackView(const AMtoULiveLinkActor& Actor) const;
    bool StartActorCachedPlayback(const AMtoULiveLinkActor& Actor);
    bool StopActorCachedPlayback(const AMtoULiveLinkActor& Actor);
    bool PauseActorCachedPlayback(const AMtoULiveLinkActor& Actor);
    bool ResumeActorCachedPlayback(const AMtoULiveLinkActor& Actor);
    bool SeekActorCachedPlayback(const AMtoULiveLinkActor& Actor, int32 SourceFrame);
    bool SetActorCachedLoop(const AMtoULiveLinkActor& Actor, bool bEnabled);
    // Admission intake gauge used by automation to prove a stalled Game Thread
    // cannot grow queued parsed-cache ownership beyond the frozen budget.
    int32 GetQueuedCacheFrameCount() const;

    /** The one linkage boundary this source owns; Game Thread only. */
    FMtoULinkageSession& GetLinkageSession() { return LinkageSession; }
    const FMtoULinkageSession& GetLinkageSession() const { return LinkageSession; }

#if WITH_DEV_AUTOMATION_TESTS
    // Automation gauges for the linkage state: undelivered control messages
    // in each direction and the shape of the current view. Production never
    // reads them; they exist so the behaviour tests need no wider
    // production-type exports.
    int32 GetQueuedControlMessageCount() const;
#endif

#if WITH_DEV_AUTOMATION_TESTS
    // Automation-only gauges for the explicit-refresh critical window
    // (Issue #39): total queued cache commands and the transient cache
    // state, read without disturbing the session. Production never calls
    // these; they exist so the Editor integration test needs no wider
    // production-type exports.
    int32 GetPendingCacheCommandCount() const;
    EMtoUCacheState GetCacheSessionState() const;
#endif

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FMtoUSessionIsolationTestAccess;
    // Editor integration coverage for explicit refresh during a session.
    friend class FMtoURefreshEndsSessionTestAccess;
#endif
    void HandleInitOnGameThread(FMtoUInitMessage&& Message);
    void HandleCacheCommandsOnGameThread();
    // Linkage intake and pump. The greeting is answered before any object is
    // negotiated; control messages and staged groups are applied on the game
    // thread and only then reported back to Maya.
    void DrainLinkageHellosOnGameThread();
    void DrainLinkageControlsOnGameThread();
    void DrainLinkageGroupsOnGameThread();
    void SendLinkageControlPacketOnGameThread(TArray<uint8> Packet, bool bControl);
    bool ApplyPoseGroupOnGameThread(
        const FMtoUPoseGroupMessage& Group,
        TArray<FMtoUAppliedComponent>& OutComponents,
        FString& OutReason);
    bool ReadBackComponentsOnGameThread(
        TArray<FMtoUAppliedComponent>& OutComponents,
        FString& OutReason);
    bool IsSessionActiveOnGameThread(uint64 SessionId) const;
    void ApplyCacheTransitionOnGameThread(uint64 SessionId, const FMtoUCacheTransition& Transition);
    bool PublishFrameOnGameThread(const FMtoUFrameMessage& Frame);
    void PublishLatestFrameOnGameThread();
    void EnqueueReplyPacketOnGameThread(uint64 SessionId, TArray<uint8> Packet, bool bCloseAfter);
    void EnqueueErrorOnGameThread(
        const FString& Code,
        const FString& Message,
        const FString& Details = FString());
    bool IsCurrentSession(uint64 SessionId) const;
    bool OwnsActorCache(const AMtoULiveLinkActor& Actor) const;
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
    // Linkage intake: the greeting, the control messages and the staged pose
    // groups the worker parsed. The game thread drains every one of them, so
    // the editor hooks and the published pose stay on one thread.
    TQueue<FMtoUPendingHello, EQueueMode::Spsc> PendingHellos;
    TQueue<FMtoUPendingLinkageControl, EQueueMode::Spsc> PendingLinkageControls;
    TQueue<FMtoUPendingPoseGroup, EQueueMode::Spsc> PendingPoseGroups;
    // Undelivered linkage control messages in the intake direction; a peer
    // that never lets the game thread drain is refused instead of backlogged.
    TAtomic<int32> QueuedLinkageControls{0};
    // Undelivered control messages in the reply direction, decremented by the
    // worker as each packet reaches the socket.
    TAtomic<int32> QueuedOutgoingControls{0};
    // Game-thread mirror of the linkage mode the worker reads to refuse work
    // that contradicts the current time authority.
    TAtomic<bool> bWorkerLinkageActive{false};
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
    // v10 identity of the negotiated session: the paired Maya object, its
    // object-set revision and the common space both adapters must match.
    FString NegotiatedObjectId;
    int32 NegotiatedSetRevision = 0;
    int32 NegotiatedSpaceRevision = 1;
    TArray<FTransform> SourceBindLocalPose;
    TArray<FTransform> TargetRefLocalPose;
    TArray<int32> BoneParents;
    TArray<int32> SourceBoneIndices;
    TArray<int32> AcceptedCurveIndices;
    TArray<FName> AcceptedCurveNames;
    TArray<TWeakObjectPtr<AMtoULiveLinkActor>> ParticipatingActors;

    // Game-thread-only transient cache owner scoped to GameThreadSession.
    FMtoUCacheSession CacheSession;

    // Game-thread-only linkage owner: pairing, mode transitions, the
    // desired/issued/applied target triple and the staged pose groups. The
    // worker never touches it; it queues parsed messages for Update().
    FMtoULinkageSession LinkageSession;
};
