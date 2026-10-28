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

/** Where a linkage group is when the apply probe sees it. */
enum class EMtoULinkageApplyStage : uint8
{
    /** Published to the subjects; asked once per object before its subject is checked. */
    Published,
    /** Every display of every object staged it; the commit follows. */
    Staged,
};

/**
 * Test-only probe of the set completion barrier on the game thread. At
 * `Published` a false return reports that object's subject as not holding the
 * group yet, which delays one object under natural ticking; at `Staged`
 * (ObjectId empty) the probe may break a display before the commit, and its
 * return value is ignored. Production builds never see this; automation must
 * always clear it.
 */
MTOULIVELINK_API void MtoUSetLinkageApplyProbe(
    TFunction<bool(EMtoULinkageApplyStage Stage, const FString& ObjectId, const FString& Generation)> Probe);
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
    // A ready for ReadyObjectId: the worker validates that object's frame
    // rows with the counts above. bSetComplete marks the set's last ready,
    // after which the worker accepts frames.
    bool bReady = false;
    FString ReadyObjectId;
    bool bSetComplete = false;
    // The object this reply removes from the set; once the packet reached the
    // socket, the worker no longer expects that object's frame rows.
    FString RemovedObjectId;
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
        RemoveObject,
        ReferenceResult,
        ReferenceRemoved,
        ReferenceQuery
    };

    EKind Kind = EKind::Mode;
    FMtoUModeMessage Mode;
    FMtoULeaveLinkageMessage LeaveLinkage;
    FMtoURemoveObjectMessage RemoveObject;
    FMtoUReferenceResultMessage ReferenceResult;
    FMtoUReferenceRemovedMessage ReferenceRemoved;
    FMtoUReferenceQueryMessage ReferenceQuery;
};

struct FMtoUPendingPoseGroup
{
    uint64 SessionId = 0;
    FMtoUPoseGroupMessage Message;
};

/** One object's share of a published linkage group. */
struct FMtoUPendingObjectApplication
{
    // The published (retargeted) local pose per target bone and the published
    // value per accepted Morph curve: what this object's displays must hold.
    TArray<FTransform> ExpectedTransforms;
    TArray<float> ExpectedCurves;
    // The published frame itself, kept so a committed group becomes the
    // display a later failure or cancellation returns to.
    FLiveLinkFrameDataStruct Frame;
};

/**
 * One published linkage group whose display is not confirmed yet. Every
 * object's subject receives the group under one generation; `applied` is
 * answered only once every subject holds that generation and every display
 * component of every object evaluated exactly its bones and Morph values.
 */
struct FMtoUPendingApplication
{
    FString EvalId;
    int32 SampleSerial = 0;
    double DisplayFrame = 0.0;
    int32 ModeRevision = 0;
    int32 SetRevision = 0;
    // The issue of the target this group answers; a cancelled issue of the
    // same evaluation never confirms it.
    int32 IssueSerial = 0;
    FString Generation;
    // One entry per object of the set, in the set's order.
    TArray<FMtoUPendingObjectApplication> Objects;
    double PublishedSeconds = 0.0;
};

/** One Additional Part the negotiated character must display. */
struct FMtoUExpectedDisplayPart
{
    FGuid PartId;
    TWeakObjectPtr<USkeletalMesh> Mesh;
    FString Name;
};

/**
 * One negotiated object of the set: its Binding actor, its own skeleton
 * mapping and Morph set, and its own Live Link subject. Index 0 is always the
 * primary character; same-name bones and Morph Targets of two objects never
 * route to each other because each object publishes only its own subject.
 */
struct FMtoUSessionObject
{
    FString ObjectId;
    FLiveLinkSubjectKey SubjectKey;
    TWeakObjectPtr<AMtoULiveLinkActor> Actor;
    // The object was negotiated through an explicit pairing, not as the one
    // placed actor of the single-character workflow.
    bool bPairedAtNegotiation = false;
    int32 ExpectedBoneCount = 0;
    int32 ExpectedCurveCount = 0;
    int32 NegotiatedRevision = 0;
    TArray<FTransform> SourceBindLocalPose;
    TArray<FTransform> TargetRefLocalPose;
    TArray<int32> BoneParents;
    TArray<int32> SourceBoneIndices;
    TArray<int32> AcceptedCurveIndices;
    TArray<FName> AcceptedCurveNames;
    // Target bone names in the order of the published transforms.
    TArray<FName> TargetBoneNames;
    // The displays the negotiated object consists of.
    TWeakObjectPtr<USkeletalMesh> ExpectedPrimaryMesh;
    TArray<FMtoUExpectedDisplayPart> ExpectedDisplayParts;
    // The last frame every display of this object showed completely (a
    // confirmed group or the last live frame) and its accepted Morph values.
    FLiveLinkFrameDataStruct LastCompleteFrame;
    TArray<float> LastCompleteCurves;
};

/** One display component of one object of the set. */
struct FMtoULinkageDisplay
{
    int32 ObjectIndex = INDEX_NONE;
    USkeletalMeshComponent* Display = nullptr;
};

/** The Live Link subject name an object of the set publishes as. */
MTOULIVELINK_API FName MtoUSubjectNameForObject(const FString& ObjectId);

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
    /** Peak undelivered linkage control messages in each direction. */
    void GetControlQueuePeaks(int32& OutOutgoing, int32& OutIncoming) const
    {
        OutOutgoing = OutgoingControlPeak.Load();
        OutIncoming = IncomingControlPeak.Load();
    }
    void ResetControlQueuePeaks()
    {
        OutgoingControlPeak.Store(QueuedOutgoingControls.Load());
        IncomingControlPeak.Store(QueuedLinkageControls.Load());
    }
    const FMtoULinkageSession& GetLinkageSession() const { return LinkageSession; }
    /**
     * The actor the connected session streams one object into, or null when
     * the session did not negotiate that object. Takeover and publishing use
     * this one identity.
     */
    AMtoULiveLinkActor* FindNegotiatedActorOnGameThread(const FString& ObjectId) const;
    /**
     * A pairing changed: when it replaced or dropped a target of the connected
     * set, the connection ends, as any other change of the set does.
     */
    void NotePairingChangedOnGameThread();

#if WITH_DEV_AUTOMATION_TESTS
    // Automation gauges for the linkage state: undelivered control messages
    // in each direction and the shape of the current view. Production never
    // reads them; they exist so the behaviour tests need no wider
    // production-type exports.
    int32 GetQueuedControlMessageCount() const;
    // Maya exits the worker staged that the game thread has not yet
    // reflected in the mirror; never negative.
    int32 GetPendingLeaveCount() const;
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
    // Negotiates one object of the set; false when it refused the object and
    // the connection ends.
    bool HandleInitOnGameThread(FMtoUInitMessage&& Message);
    void HandleCacheCommandsOnGameThread();
    // Linkage intake and pump. The greeting is answered before any object is
    // negotiated; control messages and staged groups are applied on the game
    // thread and only then reported back to Maya.
    void DrainLinkageHellosOnGameThread();
    // Returns how many leave_linkage messages it took off the queue.
    int32 DrainLinkageControlsOnGameThread();
    void IntakeLinkageGroupsOnGameThread();
    // Resolves the published group, finishes a display restore, or publishes
    // the next staged group; runs after the editor was asked for the target.
    void CommitLinkageGroupOnGameThread();
    // Removes the second object the linkage session confirmed a request for:
    // the old set's work is cancelled, the object's writers and displays are
    // restored, its subject leaves the stream and the new set is confirmed.
    void RemoveSecondaryObjectOnGameThread(const FString& ObjectId, int32 SetRevision);
    void SendLinkageControlPacketOnGameThread(TArray<uint8> Packet, bool bControl);
    bool ApplyPoseGroupOnGameThread(
        const FMtoUPoseGroupMessage& Group,
        TArray<FMtoUAppliedComponent>& OutComponents,
        FString& OutReason);
    // The completion barrier of the published group: false while it waits,
    // true once it decided; OutApplied tells which way. The caller owns the
    // record and releases it once this decided.
    bool ResolvePendingApplicationOnGameThread(
        const FMtoUPendingApplication& Pending,
        bool& bOutApplied,
        TArray<FMtoUAppliedComponent>& OutComponents,
        FString& OutReason);
    // Every display every object of the set needs, ready to show a group;
    // checked before a group is published so a missing part or object never
    // shows half.
    bool CollectLinkageDisplaysOnGameThread(
        TArray<FMtoULinkageDisplay>& OutDisplays,
        FString& OutReason) const;
    // Evaluates every display against its object's subject without changing
    // what it shows, and compares the result with the group: the whole group
    // of every object is verified before any display commits it.
    bool StageDisplaysOnGameThread(
        const FMtoUPendingApplication& Pending,
        const TArray<FMtoULinkageDisplay>& Displays,
        TArray<FMtoUAppliedComponent>& OutComponents,
        FString& OutMismatch);
    // Compares what the committed displays hold with the group.
    bool VerifyDisplaysOnGameThread(
        const FMtoUPendingApplication& Pending,
        const TArray<FMtoULinkageDisplay>& Displays,
        TArray<FMtoUAppliedComponent>& OutComponents,
        FString& OutMismatch) const;
    bool MatchDisplayPose(
        const FMtoUSessionObject& Object,
        const FMtoUPendingObjectApplication& Pending,
        const USkeletalMeshComponent& Display,
        const TArray<FTransform>& Local,
        TFunctionRef<float(const UMorphTarget&, FName)> MorphWeight,
        FMtoUAppliedComponent& InOutComponent,
        TSet<int32>& InOutDisplayedBones,
        FString& OutMismatch) const;
    bool CoversNegotiatedSkeleton(const FMtoUSessionObject& Object,
        const TSet<int32>& DisplayedBones, FString& OutMismatch) const;
    // The generation of the frame one object's Live Link subject holds now.
    FString SubjectGenerationOnGameThread(int32 ObjectIndex) const;
    // Retargets one object's sampled pose into a Live Link frame; false names
    // the transform that cannot be inverted.
    bool MakeObjectFrameOnGameThread(
        const FMtoUSessionObject& Object,
        const TArray<FMtoUTransform>& Transforms,
        const TArray<double>& Curves,
        FLiveLinkFrameDataStruct& OutFrame,
        FString& OutError) const;
    // Removes one object's subject and hands its displays back to their own
    // Binding state, as a disconnect does for the whole set.
    void ReleaseSessionObjectOnGameThread(FMtoUSessionObject& Object, const TCHAR* ActorStatus);
    // A published group that will not be confirmed (cancelled, failed, mode
    // left) gives the subject back the last complete frame; the displays
    // follow once the subject holds it.
    void RollBackPendingApplicationOnGameThread();
    // False while the subject has not received the restored frame yet.
    bool FinishDisplayRestoreOnGameThread();
    // While linkage owns time, the displays change only through a commit: a
    // display's own tick never consumes a group nobody verified yet.
    void HoldLinkageDisplaysOnGameThread(bool bHold);
    bool IsSessionActiveOnGameThread(uint64 SessionId) const;
    void ApplyCacheTransitionOnGameThread(uint64 SessionId, const FMtoUCacheTransition& Transition);
    // Publishes one live or cached frame of the whole set: every object is
    // retargeted first and only then pushed, so a frame that cannot be
    // retargeted for one object changes no display.
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
    // The set's inits in arrival order; the game thread negotiates each.
    TArray<FMtoUPendingInit> PendingInits;
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
    // Peaks of both control queues for the scheduling measurement.
    TAtomic<int32> OutgoingControlPeak{0};
    TAtomic<int32> IncomingControlPeak{0};
    // Game-thread mirror of the linkage mode the worker reads to refuse work
    // that contradicts the current time authority.
    TAtomic<bool> bWorkerLinkageActive{false};
    // True while an Unreal-started transition waits for Maya to confirm: the
    // live frames Maya sent before it saw the transition are still arriving.
    TAtomic<bool> bWorkerLinkageAwaitingPeer{false};
    // Maya's leave_linkage messages the worker staged that the game thread has
    // not yet reflected in the mirror: Maya resumes live frames right behind
    // its exit, and those frames are dropped, not refused, until it applies.
    TAtomic<int32> WorkerLinkageLeavesPending{0};
    FMtoUCacheCommandQueue CacheCommands;

    ILiveLinkClient* Client = nullptr;
    FGuid SourceGuid;
    uint64 GameThreadSession = 0;
    // Termination-counter value observed when GameThreadSession was
    // negotiated. An explicit refresh increments the counter before replacing
    // the display, so any later publish for the older epoch fails closed even
    // before the worker closes the socket.
    uint64 GameThreadSessionEndEpoch = 0;
    // v11 identity of the negotiated session: the declared object set, its
    // revision and the common space both adapters must match.
    TArray<FString> SetObjects;
    int32 NegotiatedSetRevision = 0;
    int32 NegotiatedSpaceRevision = 1;
    // The objects negotiated so far, primary first; streaming starts once it
    // holds every object of SetObjects.
    TArray<FMtoUSessionObject> SessionObjects;
    TArray<TWeakObjectPtr<AMtoULiveLinkActor>> ParticipatingActors;
    // The linkage group published and still awaiting its display evidence;
    // one generation is in flight at a time.
    TOptional<FMtoUPendingApplication> PendingApplication;
    int32 LinkageGeneration = 0;
    // The restored frames every subject must hold before the displays are
    // evaluated again, and when they were pushed.
    FString RestoreGeneration;
    double RestoreSeconds = 0.0;
    // Displays whose own tick linkage holds, given back when linkage ends.
    TArray<TWeakObjectPtr<USkeletalMeshComponent>> HeldDisplays;

    // Game-thread-only transient cache owner scoped to GameThreadSession.
    FMtoUCacheSession CacheSession;

    // Game-thread-only linkage owner: pairing, mode transitions, the
    // desired/issued/applied target triple and the staged pose groups. The
    // worker never touches it; it queues parsed messages for Update().
    FMtoULinkageSession LinkageSession;
};
