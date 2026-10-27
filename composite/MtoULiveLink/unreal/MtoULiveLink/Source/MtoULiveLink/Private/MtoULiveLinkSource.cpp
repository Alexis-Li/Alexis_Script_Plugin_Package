#include "MtoULiveLinkSource.h"

#include "MtoULiveLinkActor.h"
#include "MtoUCharacterComposition.h"
#include "MtoULiveLinkBinding.h"
#include "MtoUConnectionNegotiator.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "ILiveLinkClient.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "ReferenceSkeleton.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "LiveLinkInstance.h"
#include "LiveLinkSourceSettings.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/UObjectIterator.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMtoULiveLinkSource, Log, All);

namespace
{
// Live Link frame metadata key carrying one linkage publication's generation.
const FName MtoULinkageGenerationKey(TEXT("MtoU.LinkageGeneration"));
}

namespace
{
constexpr int32 ReceiveBufferSize = 64 * 1024;
constexpr int32 IdleWaitMilliseconds = 5;
constexpr int32 BindRetryMilliseconds = 100;

#if WITH_EDITOR
const FText RealtimeOverrideName = FText::FromString(TEXT("MtoU Live Link"));

void SetEditorViewportRealtimeOverride(bool bEnable)
{
    if (!GEditor)
    {
        return;
    }
    for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
    {
        if (!ViewportClient)
        {
            continue;
        }
        ViewportClient->RemoveRealtimeOverride(RealtimeOverrideName, false);
        if (bEnable)
        {
            ViewportClient->AddRealtimeOverride(true, RealtimeOverrideName);
        }
    }
}
#else
void SetEditorViewportRealtimeOverride(bool)
{
}
#endif

void CloseSocket(ISocketSubsystem& SocketSubsystem, FSocket*& Socket)
{
    if (Socket)
    {
        Socket->Close();
        SocketSubsystem.DestroySocket(Socket);
        Socket = nullptr;
    }
}

bool SendPacket(FSocket& Socket, const TArray<uint8>& Packet, const TAtomic<bool>& bStopping)
{
    int32 Offset = 0;
    while (Offset < Packet.Num() && !bStopping.Load())
    {
        int32 Sent = 0;
        if (Socket.Send(Packet.GetData() + Offset, Packet.Num() - Offset, Sent) && Sent > 0)
        {
            Offset += Sent;
        }
        else if (Socket.GetConnectionState() != SCS_Connected)
        {
            return false;
        }
        else
        {
            Socket.Wait(
                ESocketWaitConditions::WaitForWrite,
                FTimespan::FromMilliseconds(IdleWaitMilliseconds));
        }
    }
    return Offset == Packet.Num();
}

bool IsPlacedEditorActor(const AMtoULiveLinkActor& Actor)
{
    if (Actor.HasAnyFlags(RF_ClassDefaultObject) || !IsValid(&Actor))
    {
        return false;
    }
    const UWorld* World = Actor.GetWorld();
    // PIE is not supported for this release: duplicated PIE-world actors are
    // excluded so entering PIE can never present a second streaming target.
    return World && World->WorldType == EWorldType::Editor;
}

TAtomic<uint64> GStreamingSessionEndRequests{0};
TWeakPtr<FMtoULiveLinkSource> GActiveSource;
#if WITH_DEV_AUTOMATION_TESTS
// Test-only hold on worker disconnect cleanup (see header). Defaults off, so
// production and ordinary automation never observe it.
TAtomic<bool> GDeferStreamingSessionEndCleanup{false};
#endif
}

FName MtoUSubjectNameForObject(const FString& ObjectId)
{
    // The primary keeps the subject every v9 and v10 display evaluates; the
    // second object of a set publishes under its own name, so no display of
    // one object can read the other's values.
    return ObjectId == FMtoUObjectIds::Secondary
        ? FName(TEXT("MtoU_Secondary"))
        : FName(TEXT("MtoU_Character"));
}

TSharedPtr<FMtoULiveLinkSource> MtoUSetActiveSource(TSharedPtr<FMtoULiveLinkSource> Source)
{
    check(IsInGameThread());
    TSharedPtr<FMtoULiveLinkSource> Previous = GActiveSource.Pin();
    GActiveSource = Source;
    return Previous;
}

TWeakPtr<FMtoULiveLinkSource> MtoUGetActiveLinkageSource()
{
    return GActiveSource;
}

FMtoUCachePlaybackView MtoUGetActorCachePlaybackView(const AMtoULiveLinkActor& Actor)
{
    if (TSharedPtr<FMtoULiveLinkSource> Source = GActiveSource.Pin())
    {
        return Source->GetActorCachePlaybackView(Actor);
    }
    return FMtoUCachePlaybackView();
}

bool MtoUStartActorCachedPlayback(const AMtoULiveLinkActor& Actor)
{
    if (TSharedPtr<FMtoULiveLinkSource> Source = GActiveSource.Pin())
    {
        return Source->StartActorCachedPlayback(Actor);
    }
    return false;
}

bool MtoUStopActorCachedPlayback(const AMtoULiveLinkActor& Actor)
{
    if (TSharedPtr<FMtoULiveLinkSource> Source = GActiveSource.Pin())
    {
        return Source->StopActorCachedPlayback(Actor);
    }
    return false;
}

bool MtoUPauseActorCachedPlayback(const AMtoULiveLinkActor& Actor)
{
    if (TSharedPtr<FMtoULiveLinkSource> Source = GActiveSource.Pin())
    {
        return Source->PauseActorCachedPlayback(Actor);
    }
    return false;
}

bool MtoUResumeActorCachedPlayback(const AMtoULiveLinkActor& Actor)
{
    if (TSharedPtr<FMtoULiveLinkSource> Source = GActiveSource.Pin())
    {
        return Source->ResumeActorCachedPlayback(Actor);
    }
    return false;
}

bool MtoUSeekActorCachedPlayback(const AMtoULiveLinkActor& Actor, int32 SourceFrame)
{
    if (TSharedPtr<FMtoULiveLinkSource> Source = GActiveSource.Pin())
    {
        return Source->SeekActorCachedPlayback(Actor, SourceFrame);
    }
    return false;
}

bool MtoUSetActorCachedLoop(const AMtoULiveLinkActor& Actor, bool bEnabled)
{
    if (TSharedPtr<FMtoULiveLinkSource> Source = GActiveSource.Pin())
    {
        return Source->SetActorCachedLoop(Actor, bEnabled);
    }
    return false;
}

void MtoURequestStreamingSessionEnd()
{
    ++GStreamingSessionEndRequests;
}

uint64 MtoUGetStreamingSessionEndCount()
{
    return GStreamingSessionEndRequests.Load();
}
#if WITH_DEV_AUTOMATION_TESTS
void MtoUSetDeferStreamingSessionEndCleanup(bool bDefer)
{
    GDeferStreamingSessionEndCleanup.Store(bDefer);
}
#endif

void MtoUNotifyEditorWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
    (void)bSessionEnded;
    (void)bCleanupResources;
    // PIE worlds never host a discoverable target and EditorPreview worlds
    // never negotiate a session, so only Editor worlds can end one here.
    if (!World || World->WorldType != EWorldType::Editor)
    {
        return;
    }
    for (TObjectIterator<AMtoULiveLinkActor> It; It; ++It)
    {
        if (!It->HasAnyFlags(RF_ClassDefaultObject) && It->GetWorld() == World)
        {
            MtoURequestStreamingSessionEnd();
            return;
        }
    }
}

FMtoULiveLinkSource::FMtoULiveLinkSource(uint16 InPort)
    : ConfiguredPort(InPort)
    , Status(TEXT("Starting listener..."))
{
    CacheSession.SetPublish([this](const FMtoUFrameMessage& Frame) -> bool
    {
        return PublishFrameOnGameThread(Frame);
    });
    CacheSession.SetProgressSink(
        [this](int32 PlayId, int32 AppliedFrames)
    {
        const uint64 SessionId = GameThreadSession;
        if (SessionId == 0 || !IsSessionPublishableOnGameThread(SessionId))
        {
            return;
        }
        FMtoUOutgoing Reply;
        Reply.SessionId = SessionId;
        Reply.Packet = FMtoUProtocol::EncodeCacheProgress(PlayId, AppliedFrames);
        OutgoingReplies.Enqueue(MoveTemp(Reply));
    });
    // The linkage session answers through the same ordered reply queue the
    // cache outcomes use, so one connection never interleaves two writers.
    LinkageSession.SendReply = [this](TArray<uint8>&& Packet, bool bControl)
    {
        SendLinkageControlPacketOnGameThread(MoveTemp(Packet), bControl);
    };
    StartListener();
}

FMtoULiveLinkSource::~FMtoULiveLinkSource()
{
    StopListener();
}

bool FMtoULiveLinkSource::OwnsActorCache(const AMtoULiveLinkActor& Actor) const
{
    check(IsInGameThread());
    if (!IsSessionPublishableOnGameThread(GameThreadSession))
    {
        return false;
    }
    // Cached Playback keeps the single-character contract: only the primary
    // of a one-object set owns the session's cache.
    return SessionObjects.Num() == 1 && SetObjects.Num() == 1
        && SessionObjects[0].Actor.Get() == &Actor;
}

FMtoUCachePlaybackView FMtoULiveLinkSource::GetActorCachePlaybackView(
    const AMtoULiveLinkActor& Actor) const
{
    FMtoUCachePlaybackView View;
    if (OwnsActorCache(Actor))
    {
        View = CacheSession.GetView();
        View.bConnected = true;
    }
    return View;
}

bool FMtoULiveLinkSource::StartActorCachedPlayback(const AMtoULiveLinkActor& Actor)
{
    if (!GetActorCachePlaybackView(Actor).CanPlay())
    {
        return false;
    }
    const FMtoUCacheTransition Transition = CacheSession.StartLocalPlayback();
    ApplyCacheTransitionOnGameThread(GameThreadSession, Transition);
    return Transition.bAccepted;
}

bool FMtoULiveLinkSource::StopActorCachedPlayback(const AMtoULiveLinkActor& Actor)
{
    if (!GetActorCachePlaybackView(Actor).CanStop())
    {
        return false;
    }
    const FMtoUCacheTransition Transition = CacheSession.StopLocalPlayback();
    ApplyCacheTransitionOnGameThread(GameThreadSession, Transition);
    return Transition.bAccepted;
}

// The four interactive actions below exist only on this Binding actor, unlike
// play/stop which Maya can also send. A refused one therefore stays a local
// prompt in the actor row and is never reported to Maya as a peer outcome.
bool FMtoULiveLinkSource::PauseActorCachedPlayback(const AMtoULiveLinkActor& Actor)
{
    if (!GetActorCachePlaybackView(Actor).CanPause())
    {
        return false;
    }
    const FMtoUCacheTransition Transition = CacheSession.PauseLocalPlayback();
    if (Transition.bAccepted)
    {
        ApplyCacheTransitionOnGameThread(GameThreadSession, Transition);
    }
    return Transition.bAccepted;
}

bool FMtoULiveLinkSource::ResumeActorCachedPlayback(const AMtoULiveLinkActor& Actor)
{
    if (!GetActorCachePlaybackView(Actor).CanResume())
    {
        return false;
    }
    const FMtoUCacheTransition Transition = CacheSession.ResumeLocalPlayback();
    if (Transition.bAccepted)
    {
        ApplyCacheTransitionOnGameThread(GameThreadSession, Transition);
    }
    return Transition.bAccepted;
}

bool FMtoULiveLinkSource::SeekActorCachedPlayback(const AMtoULiveLinkActor& Actor, int32 SourceFrame)
{
    if (!GetActorCachePlaybackView(Actor).CanSeek())
    {
        return false;
    }
    // A refused seek carries its reason in the session's error details, so the
    // actor row can prompt with the requested frame and the sampled range.
    const FMtoUCacheTransition Transition = CacheSession.SeekLocalPlayback(SourceFrame);
    if (Transition.bAccepted)
    {
        ApplyCacheTransitionOnGameThread(GameThreadSession, Transition);
    }
    return Transition.bAccepted;
}

bool FMtoULiveLinkSource::SetActorCachedLoop(const AMtoULiveLinkActor& Actor, bool bEnabled)
{
    if (!GetActorCachePlaybackView(Actor).CanSeek())
    {
        return false;
    }
    const FMtoUCacheTransition Transition = CacheSession.SetLocalLoop(bEnabled);
    if (Transition.bAccepted)
    {
        ApplyCacheTransitionOnGameThread(GameThreadSession, Transition);
    }
    return Transition.bAccepted;
}

void FMtoULiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
    check(IsInGameThread());
    Client = InClient;
    SourceGuid = InSourceGuid;
    for (FMtoUSessionObject& Object : SessionObjects)
    {
        Object.SubjectKey = FLiveLinkSubjectKey(SourceGuid, MtoUSubjectNameForObject(Object.ObjectId));
    }
}

void FMtoULiveLinkSource::InitializeSettings(ULiveLinkSourceSettings* Settings)
{
    if (Settings)
    {
        Settings->Mode = ELiveLinkSourceMode::Latest;
    }
}

void FMtoULiveLinkSource::Update()
{
    check(IsInGameThread());

    // Maya's leave_linkage messages this update takes off the intake queue;
    // the worker drops frames behind them until the mirror below shows what
    // they did.
    int32 LeavesTaken = 0;
    uint64 DisconnectedSession = 0;
    while (DisconnectedSessions.Dequeue(DisconnectedSession))
    {
        if (DisconnectedSession == GameThreadSession)
        {
            SetEditorViewportRealtimeOverride(false);
            HoldLinkageDisplaysOnGameThread(false);
            RestoreGeneration.Reset();
            if (Client && SourceGuid.IsValid())
            {
                for (const FMtoUSessionObject& Object : SessionObjects)
                {
                    Client->RemoveSubject_AnyThread(Object.SubjectKey);
                }
            }
            for (const TWeakObjectPtr<AMtoULiveLinkActor>& Actor : ParticipatingActors)
            {
                if (Actor.IsValid())
                {
                    Actor->ReapplyDisplayTarget();
                    Actor->SetConnectionStatus(TEXT("Disconnected"));
                    Actor->NoteSessionEnded();
                }
            }
            GameThreadSession = 0;
            SessionObjects.Reset();
            SetObjects.Reset();
            PendingApplication.Reset();
            NegotiatedSetRevision = 0;
            NegotiatedSpaceRevision = 1;
            // Every borrowed writer is restored per resource and the linkage
            // state returns to realtime; a new connection negotiates again.
            LinkageSession.EndSession();
            {
                FScopeLock Lock(&PendingMutex);
                FMtoUPendingHello Hello;
                while (PendingHellos.Dequeue(Hello))
                {
                    QueuedLinkageControls.Store(FMath::Max(0, QueuedLinkageControls.Load() - 1));
                }
                FMtoUPendingLinkageControl Control;
                while (PendingLinkageControls.Dequeue(Control))
                {
                    QueuedLinkageControls.Store(FMath::Max(0, QueuedLinkageControls.Load() - 1));
                    if (Control.Kind == FMtoUPendingLinkageControl::EKind::LeaveLinkage)
                    {
                        ++LeavesTaken;
                    }
                }
                FMtoUPendingPoseGroup Group;
                while (PendingPoseGroups.Dequeue(Group))
                {
                    QueuedLinkageControls.Store(FMath::Max(0, QueuedLinkageControls.Load() - 1));
                }
            }
            // The transient cache and its upload/play identity history are
            // scoped to one negotiated streaming session; a newer connection
            // starts its own fresh sequence at 1.
            CacheSession.EndSession();
        }
    }

    // The greeting is answered before any object is negotiated, so the intake
    // runs ahead of the init handling and the acknowledgement always reaches
    // the peer before a negotiation reply.
    DrainLinkageHellosOnGameThread();

    TArray<FMtoUPendingInit> Inits;
    {
        FScopeLock Lock(&PendingMutex);
        Inits = MoveTemp(PendingInits);
        PendingInits.Reset();
    }
    for (FMtoUPendingInit& Init : Inits)
    {
        if (!IsCurrentSession(Init.SessionId))
        {
            continue;
        }
        // An explicit refresh increments the termination counter before
        // replacing the display. An init queued for the replaced session must
        // never negotiate after the refresh started; only a new session id
        // may establish a new epoch and stream the newer revision.
        const bool bStaleInitForReplacedSession = Init.SessionId == GameThreadSession
            && GameThreadSession != 0
            && MtoUGetStreamingSessionEndCount() != GameThreadSessionEndEpoch;
        if (bStaleInitForReplacedSession)
        {
            continue;
        }
        if (Init.SessionId != GameThreadSession)
        {
            GameThreadSessionEndEpoch = MtoUGetStreamingSessionEndCount();
        }
        GameThreadSession = Init.SessionId;
        // A refused object ends the whole set: the rest of its inits are not
        // negotiated against a connection that is closing.
        if (!HandleInitOnGameThread(MoveTemp(Init.Message)))
        {
            break;
        }
    }
    HandleCacheCommandsOnGameThread();

    // Linkage intake and pump: the control messages and the staged pose groups
    // follow the greeting, and the camera/time pump asks the editor for the
    // current target on its bounded cadence.
    LeavesTaken += DrainLinkageControlsOnGameThread();
    {
        // A confirmed removal of the second object runs before any group of
        // the old set is staged, so none of them is published any more.
        FString RemovedObjectId;
        int32 RemovalSetRevision = 0;
        if (LinkageSession.ConsumeRemovalRequest(RemovedObjectId, RemovalSetRevision))
        {
            RemoveSecondaryObjectOnGameThread(RemovedObjectId, RemovalSetRevision);
        }
    }
    IntakeLinkageGroupsOnGameThread();
    // The editor is asked for the current target before published work is
    // resolved, at once while a group waits for its commit: a pause, jump,
    // cut or edit that happened since the group was published cancels it
    // before any display commits it or any evidence confirms it.
    LinkageSession.Tick(FPlatformTime::Seconds(), PendingApplication.IsSet());
    CommitLinkageGroupOnGameThread();
    HoldLinkageDisplaysOnGameThread(
        LinkageSession.GetView().Mode == TEXT("linkage")
        || PendingApplication.IsSet()
        || !RestoreGeneration.IsEmpty());
    // The worker reads this mirror to refuse cache work and live frames that
    // contradict the current time authority.
    bWorkerLinkageActive.Store(LinkageSession.GetView().Mode == TEXT("linkage"));
    bWorkerLinkageAwaitingPeer.Store(LinkageSession.GetView().bTransitioning);
    // Only once the mirror shows what the leaves did may the worker refuse
    // frames again: an applied exit accepts them, a refused one keeps linkage,
    // and a dead session's exit leaves realtime.
    if (LeavesTaken > 0)
    {
        WorkerLinkageLeavesPending -= LeavesTaken;
    }
    if (LinkageSession.ConsumeSessionEndRequest())
    {
        // A failed restore ended the session rather than hiding the residual.
        MtoURequestStreamingSessionEnd();
    }

    // While a cache owns this session, ordinary live frames must neither
    // mutate the cache nor overwrite local playback; drop them and their
    // stale pending slots silently.
    if (CacheSession.AcceptsLiveFrames())
    {
        PublishLatestFrameOnGameThread();
    }
    else
    {
        FScopeLock Lock(&PendingMutex);
        if (PendingFrame.IsSet() && PendingFrame->SessionId == GameThreadSession)
        {
            PendingFrame.Reset();
        }
    }
}

bool FMtoULiveLinkSource::IsSourceStillValid() const
{
    return bSourceValid.Load();
}

bool FMtoULiveLinkSource::RequestSourceShutdown()
{
    StopListener();
    return true;
}

FText FMtoULiveLinkSource::GetSourceType() const
{
    return FText::FromString(TEXT("MtoU_LiveLink"));
}

FText FMtoULiveLinkSource::GetSourceMachineName() const
{
    return FText::FromString(TEXT("127.0.0.1"));
}

FText FMtoULiveLinkSource::GetSourceStatus() const
{
    FScopeLock Lock(&StatusMutex);
    return FText::FromString(Status);
}

bool FMtoULiveLinkSource::StartListener()
{
    if (Thread)
    {
        return true;
    }
    bStopRequested.Store(false);
    bSourceValid.Store(true);
    SetStatus(TEXT("Starting listener..."));
    Thread.Reset(FRunnableThread::Create(
        this,
        *FString::Printf(TEXT("MtoULiveLink_%u"), ConfiguredPort),
        0,
        TPri_Normal));
    if (!Thread)
    {
        SetStatus(TEXT("Failed to start the MtoU_LiveLink listener thread."));
        return false;
    }
    return true;
}

void FMtoULiveLinkSource::Stop()
{
    bStopRequested.Store(true);
}

void FMtoULiveLinkSource::StopListener()
{
    Stop();
    if (Thread)
    {
        Thread->WaitForCompletion();
        Thread.Reset();
    }
    bSourceValid.Store(false);
    SetStatus(TEXT("Stopped"));
    if (IsInGameThread())
    {
        CacheSession.EndSession();
        HoldLinkageDisplaysOnGameThread(false);
        SetEditorViewportRealtimeOverride(false);
        for (const TWeakObjectPtr<AMtoULiveLinkActor>& Actor : ParticipatingActors)
        {
            if (Actor.IsValid())
            {
                Actor->ReapplyDisplayTarget();
                Actor->SetConnectionStatus(TEXT("Disconnected"));
                Actor->NoteSessionEnded();
            }
        }
    }
}

int32 FMtoULiveLinkSource::GetQueuedCacheFrameCount() const
{
    return CacheCommands.GetPendingFrameCount();
}
#if WITH_DEV_AUTOMATION_TESTS
int32 FMtoULiveLinkSource::GetQueuedControlMessageCount() const
{
    return QueuedLinkageControls.Load();
}

int32 FMtoULiveLinkSource::GetPendingLeaveCount() const
{
    return WorkerLinkageLeavesPending.Load();
}
#endif
#if WITH_DEV_AUTOMATION_TESTS
int32 FMtoULiveLinkSource::GetPendingCacheCommandCount() const
{
    return CacheCommands.GetPendingCommandCount();
}

EMtoUCacheState FMtoULiveLinkSource::GetCacheSessionState() const
{
    return CacheSession.GetState();
}
#endif

uint32 FMtoULiveLinkSource::Run()
{
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        SetStatus(TEXT("Socket subsystem is unavailable."));
        return 1;
    }

    TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
    bool bValidAddress = false;
    Address->SetIp(TEXT("127.0.0.1"), bValidAddress);
    Address->SetPort(ConfiguredPort);
    FSocket* ListenSocket = nullptr;
    bool bLoggedBindError = false;
    while (!bStopRequested.Load())
    {
        ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MtoU_LiveLink listener"));
        if (!ListenSocket)
        {
            SetStatus(TEXT("Failed to create the MtoU_LiveLink listener socket."));
            return 1;
        }
        if (bValidAddress
            && ListenSocket->Bind(*Address)
            && ListenSocket->Listen(1)
            && ListenSocket->SetNonBlocking(true))
        {
            break;
        }
        const ESocketErrors ErrorCode = SocketSubsystem->GetLastErrorCode();
        const FString Error = FString::Printf(
            TEXT("Failed to bind MtoU_LiveLink to 127.0.0.1:%u: %s"),
            ConfiguredPort,
            SocketSubsystem->GetSocketError(ErrorCode));
        if (!bLoggedBindError || ErrorCode != SE_EADDRINUSE)
        {
            UE_LOG(LogMtoULiveLinkSource, Warning, TEXT("%s"), *Error);
        }
        SetStatus(Error);
        CloseSocket(*SocketSubsystem, ListenSocket);
        if (ErrorCode != SE_EADDRINUSE)
        {
            return 1;
        }
        bLoggedBindError = true;
        FPlatformProcess::Sleep(BindRetryMilliseconds / 1000.0f);
    }
    if (!ListenSocket)
    {
        return 0;
    }
    FSocket* ClientSocket = nullptr;

    TSharedRef<FInternetAddr> BoundAddress = SocketSubsystem->CreateInternetAddr();
    ListenSocket->GetAddress(*BoundAddress);
    const int32 BoundPort = BoundAddress->GetPort();
    const FString ListeningStatus = FString::Printf(
        TEXT("Listening on 127.0.0.1:%d"), BoundPort);
    SetStatus(ListeningStatus);

    uint64 SessionCounter = 0;
    uint64 ActiveSession = 0;
    // Each worker tracks the shared counter independently, so one listener
    // can never swallow a termination request meant for another. Requests
    // made before this accept belong to older sessions and are never applied
    // retroactively.
    uint64 LastSeenSessionEndRequests = GStreamingSessionEndRequests.Load();
    // Every init of the declared set arrives before any other message.
    bool bInitReceived = false;
    int32 WorkerInitsExpected = 0;
    int32 WorkerInitsReceived = 0;
    bool bReady = false;
    int32 WorkerExpectedBoneCount = 0;
    int32 WorkerExpectedCurveCount = 0;
    // The negotiated counts of every object of the set beside the primary;
    // a frame of another set is dropped against them.
    TMap<FString, FIntPoint> WorkerObjectCounts;
    FMtoUFrameDecoder Decoder;
    // Upload identity observed at intake; frame/end commands are stamped with
    // it so a rejected, superseded, or cleared upload can be cancelled as a
    // unit and can never affect a newer attempt.
    int32 WorkerCacheUploadId = 0;
    bool bWorkerCacheUploadRejected = false;
    int32 WorkerLastSeenClearId = 0;
    // One raw (never parsed) cache_frame held while the Game Thread drains
    // the bounded intake queue; the sender experiences TCP backpressure
    // until then, so queued parsed ownership stays inside the budget.
    TOptional<TArray<uint8>> HeldCacheFrame;

    auto MarkDisconnected = [&]()
    {
        HeldCacheFrame.Reset();
        WorkerCacheUploadId = 0;
        bWorkerCacheUploadRejected = false;
        WorkerLastSeenClearId = 0;
        // Undelivered linkage intake belongs to the dead session, but the
        // game thread is the only consumer of these single-consumer queues:
        // it discards the dead session's messages and uncounts their leaves
        // and queue slots, so each message is taken exactly once.
        if (ActiveSession != 0)
        {
            // Cancel every queued command of the dead session immediately so
            // disconnect or teardown can never leave parsed frames in flight.
            CacheCommands.CancelSession(ActiveSession);
            {
                FScopeLock Lock(&PendingMutex);
                if (CurrentWorkerSession == ActiveSession)
                {
                    CurrentWorkerSession = 0;
                    PendingInits.Reset();
                    PendingFrame.Reset();
                }
            }
            DisconnectedSessions.Enqueue(ActiveSession);
        }
        CloseSocket(*SocketSubsystem, ClientSocket);
        ActiveSession = 0;
        bInitReceived = false;
        WorkerInitsExpected = 0;
        WorkerInitsReceived = 0;
        bReady = false;
        WorkerExpectedBoneCount = 0;
        WorkerExpectedCurveCount = 0;
        WorkerObjectCounts.Reset();
        Decoder = FMtoUFrameDecoder();
        if (!bStopRequested.Load())
        {
            SetStatus(ListeningStatus);
        }
    };

    auto SendErrorAndDisconnect = [&](const FString& Code, const FString& Message)
    {
        if (ClientSocket)
        {
            SendPacket(
                *ClientSocket,
                FMtoUProtocol::EncodeError(Code, Message),
                bStopRequested);
        }
        MarkDisconnected();
    };

    // A linkage refusal that keeps the connection open reports the exact code
    // without the close-after flag an unrecoverable error carries.
    auto SendLinkageError = [&](const FString& Code, const FString& Message)
    {
        if (ClientSocket)
        {
            SendPacket(
                *ClientSocket,
                FMtoUProtocol::EncodeError(Code, Message),
                bStopRequested);
        }
    };

    // Bounded linkage intake: the frozen control queue is enforced before a
    // message gains a queue slot, so a peer that never lets the game thread
    // drain is refused instead of backlogged.
    auto CanAdmitLinkageControl = [&]() -> bool
    {
        return QueuedLinkageControls.Load() < FMtoUProtocol::MaxControlQueue;
    };

    // Bounded admission enqueue: the producer never blocks the loop for
    // control messages, and a pre-admitted frame is always accepted. Only a
    // shutdown request can abort intake, which keeps disconnect, clear,
    // Editor shutdown, and worker teardown deadlock-free.
    auto EnqueueCacheCommand = [&](FMtoUCacheCommand Command) -> bool
    {
        Command.SessionId = ActiveSession;
        if (Command.Kind == FMtoUCacheCommand::EKind::Frame
            || Command.Kind == FMtoUCacheCommand::EKind::End)
        {
            Command.UploadId = WorkerCacheUploadId;
        }
        {
            FScopeLock Lock(&PendingMutex);
            if (CurrentWorkerSession != ActiveSession)
            {
                return true;
            }
        }
        const bool bEnqueued = CacheCommands.Produce(
            MoveTemp(Command),
            [this, ActiveSession]()
            {
                // Frames never reach this wait (CanAdmitFrame pre-gates them
                // through the non-blocking HeldCacheFrame path), so only a
                // control-message flood can block here; shutdown or a session
                // swap releases it so teardown and disconnect stay responsive.
                if (bStopRequested.Load())
                {
                    return true;
                }
                FScopeLock Lock(&PendingMutex);
                return CurrentWorkerSession != ActiveSession;
            });
        if (!bEnqueued)
        {
            MarkDisconnected();
        }
        return bEnqueued;
    };

    // Parse-and-intake one cache_frame payload. Returns false when decoding
    // must stop: a disconnect, or a backpressure hold that keeps the raw
    // payload (never its parsed form) until the Game Thread drains.
    auto HandleCacheFrame = [&](TArray<uint8>&& Payload) -> bool
    {
        if (bWorkerCacheUploadRejected)
        {
            return true;
        }
        if (!CacheCommands.CanAdmitFrame(Payload.Num()))
        {
            HeldCacheFrame = MoveTemp(Payload);
            return false;
        }
        FMtoUCacheCommand Command;
        Command.Kind = FMtoUCacheCommand::EKind::Frame;
        Command.EncodedBytes = Payload.Num();
        FString CacheError;
        FString ErrorCode = TEXT("INVALID_MESSAGE");
        const bool bShapeValid = FMtoUProtocol::ParseCacheFrame(
            Payload,
            WorkerExpectedBoneCount,
            WorkerExpectedCurveCount,
            Command.Index,
            Command.Frame,
            CacheError,
            &ErrorCode);
        // A negative index follows the production cache-session rejection
        // path: the stable code is echoed by the session itself while it
        // atomically discards the partial upload, and the negotiated
        // connection stays open.
        if (!bShapeValid && ErrorCode == TEXT("CACHE_FRAME_INDEX_INVALID"))
        {
            bWorkerCacheUploadRejected = true;
            CacheCommands.CancelUpload(ActiveSession, WorkerCacheUploadId);
            return EnqueueCacheCommand(MoveTemp(Command));
        }
        if (!bShapeValid && ErrorCode == TEXT("CACHE_FRAME_CONTENTS_INVALID"))
        {
            // The cardinality check ran before parsed-frame allocation. Carry
            // one tiny control rejection to the Game Thread, then poison this
            // intake identity so pipelined frame/end data is dropped raw.
            bWorkerCacheUploadRejected = true;
            CacheCommands.CancelUpload(ActiveSession, WorkerCacheUploadId);
            Command.Kind = FMtoUCacheCommand::EKind::Reject;
            Command.UploadId = WorkerCacheUploadId;
            Command.ErrorCode = ErrorCode;
            Command.ErrorDetails = FString::Printf(
                TEXT("Cached frame %d: %s"), Command.Index, *CacheError);
            return EnqueueCacheCommand(MoveTemp(Command));
        }
        if (!bShapeValid)
        {
            SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), CacheError);
            return false;
        }
        return EnqueueCacheCommand(MoveTemp(Command));
    };

    while (!bStopRequested.Load())
    {
        bool bDidWork = false;
        // One shared idempotent termination boundary: a Preview revision
        // invalidation or Binding actor lifetime end closes the socket here,
        // and the existing disconnect path performs the Game Thread cleanup.
        // Test-only deferral (automation, off by default) holds this cleanup
        // so the synchronous Game Thread publish gate can be proven before
        // any worker close; the pending counter is left unread so release
        // still disconnects exactly once.
        const uint64 SessionEndRequests = GStreamingSessionEndRequests.Load();
        bool bSessionEndCleanupDeferred = false;
#if WITH_DEV_AUTOMATION_TESTS
        bSessionEndCleanupDeferred = GDeferStreamingSessionEndCleanup.Load();
#endif
        if (SessionEndRequests != LastSeenSessionEndRequests && !bSessionEndCleanupDeferred)
        {
            LastSeenSessionEndRequests = SessionEndRequests;
            if (ActiveSession != 0)
            {
                MarkDisconnected();
                continue;
            }
        }

        // Resolve backpressure first: once the Game Thread has drained the
        // bounded intake queue, admit the held raw frame before touching the
        // socket again. Replies, termination, and shutdown keep being served
        // on every pass while a frame is held.
        if (HeldCacheFrame.IsSet()
            && CacheCommands.CanAdmitFrame(HeldCacheFrame.GetValue().Num()))
        {
            TArray<uint8> Retry = MoveTemp(HeldCacheFrame.GetValue());
            HeldCacheFrame.Reset();
            bDidWork = true;
            HandleCacheFrame(MoveTemp(Retry));
        }

        bool bPendingConnection = false;
        if (ListenSocket->HasPendingConnection(bPendingConnection) && bPendingConnection)
        {
            FSocket* Accepted = ListenSocket->Accept(TEXT("MtoU_LiveLink Maya client"));
            if (Accepted)
            {
                bDidWork = true;
                Accepted->SetNonBlocking(true);
                if (ClientSocket)
                {
                    SendPacket(
                        *Accepted,
                        FMtoUProtocol::EncodeError(
                            TEXT("SECOND_CLIENT_REJECTED"),
                            TEXT("MtoU_LiveLink already has a Maya client.")),
                        bStopRequested);
                    CloseSocket(*SocketSubsystem, Accepted);
                }
                else
                {
                    ClientSocket = Accepted;
                    ActiveSession = ++SessionCounter;
                    LastSeenSessionEndRequests = GStreamingSessionEndRequests.Load();
                    bInitReceived = false;
                    WorkerInitsExpected = 0;
                    WorkerInitsReceived = 0;
                    bReady = false;
                    WorkerObjectCounts.Reset();
                    Decoder = FMtoUFrameDecoder();
                    {
                        FScopeLock Lock(&PendingMutex);
                        CurrentWorkerSession = ActiveSession;
                        PendingInits.Reset();
                        PendingFrame.Reset();
                    }
                    SetStatus(TEXT("Validating Maya character..."));
                }
            }
        }

        if (ClientSocket)
        {
            FMtoUOutgoing Reply;
            while (OutgoingReplies.Dequeue(Reply))
            {
                if (Reply.SessionId != ActiveSession)
                {
                    continue;
                }
                bDidWork = true;
                if (Reply.bControl)
                {
                    QueuedOutgoingControls.Store(FMath::Max(0, QueuedOutgoingControls.Load() - 1));
                }
                if (!SendPacket(*ClientSocket, Reply.Packet, bStopRequested))
                {
                    MarkDisconnected();
                    break;
                }
                if (Reply.bCloseAfter)
                {
                    MarkDisconnected();
                    break;
                }
                if (Reply.bReady)
                {
                    if (Reply.ReadyObjectId.IsEmpty()
                        || Reply.ReadyObjectId == FMtoUObjectIds::Character)
                    {
                        WorkerExpectedBoneCount = Reply.ExpectedBoneCount;
                        WorkerExpectedCurveCount = Reply.ExpectedCurveCount;
                    }
                    else
                    {
                        WorkerObjectCounts.Add(Reply.ReadyObjectId,
                            FIntPoint(Reply.ExpectedBoneCount, Reply.ExpectedCurveCount));
                    }
                    // Frames stream only once every object of the set is ready.
                    if (Reply.bSetComplete)
                    {
                        bReady = true;
                        SetStatus(TEXT("Connected to Maya"));
                    }
                }
                if (!Reply.RemovedObjectId.IsEmpty())
                {
                    // Maya hears the new set with this packet; its later
                    // frames carry the primary only.
                    WorkerObjectCounts.Remove(Reply.RemovedObjectId);
                }
            }
        }

        // While a cache frame is held for backpressure the socket reader
        // pauses: the sender experiences TCP flow control instead of growing
        // parsed ownership on Unreal's side. Reply delivery, termination
        // requests, and shutdown checks above still run every pass, so no
        // disconnect, clear, Editor shutdown, or teardown can wedge here.
        if (ClientSocket && !HeldCacheFrame.IsSet())
        {
            uint8 ReceiveBuffer[ReceiveBufferSize];
            int32 Read = 0;
            if (!ClientSocket->Recv(ReceiveBuffer, ReceiveBufferSize, Read))
            {
                bDidWork = true;
                MarkDisconnected();
            }
            else
            {
                if (Read > 0)
                {
                    bDidWork = true;
                    Decoder.Append(ReceiveBuffer, Read);
                }
                // Drain complete messages whenever any exist, not only when
                // fresh bytes arrive: messages already buffered behind a
                // released backpressure hold must keep flowing immediately.
                while (ClientSocket)
                {
                    TArray<uint8> Payload;
                    FString Error;
                    const EMtoUDecodeResult Result = Decoder.Pop(Payload, Error);
                    if (Result == EMtoUDecodeResult::NeedMore)
                    {
                        break;
                    }
                    if (Result == EMtoUDecodeResult::Error)
                    {
                        SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), Error);
                        break;
                    }

                    if (!bInitReceived)
                    {
                        // The greeting opens a v10 connection. A mismatched
                        // version is refused here, before any target is
                        // discovered, a writer is muted or a handoff is read.
                        FString FirstType;
                        FString FirstTypeError;
                        if (FMtoUProtocol::PeekType(Payload, FirstType, FirstTypeError)
                            && FirstType == TEXT("hello"))
                        {
                            FMtoUHelloMessage Hello;
                            FString HelloError;
                            FString HelloErrorCode;
                            if (!FMtoUProtocol::ParseHello(Payload, Hello, HelloError, &HelloErrorCode))
                            {
                                SendErrorAndDisconnect(HelloErrorCode, HelloError);
                                break;
                            }
                            if (!CanAdmitLinkageControl())
                            {
                                SendLinkageError(
                                    TEXT("RESOURCE_LIMIT"),
                                    TEXT("The linkage control queue is full."));
                                break;
                            }
                            FMtoUPendingHello Pending;
                            Pending.SessionId = ActiveSession;
                            Pending.Message = MoveTemp(Hello);
                            PendingHellos.Enqueue(MoveTemp(Pending));
                            QueuedLinkageControls.Store(QueuedLinkageControls.Load() + 1);
                            continue;
                        }
                        FMtoUInitMessage Init;
                        FString ErrorCode;
                        if (!FMtoUProtocol::ParseInit(Payload, Init, Error, &ErrorCode))
                        {
                            SendErrorAndDisconnect(ErrorCode, Error);
                            break;
                        }
                        // The first init declares the set; one init follows
                        // per object before any other message.
                        if (WorkerInitsReceived == 0)
                        {
                            WorkerInitsExpected = Init.SetObjects.Num();
                        }
                        ++WorkerInitsReceived;
                        bInitReceived = WorkerInitsReceived >= WorkerInitsExpected;
                        FScopeLock Lock(&PendingMutex);
                        PendingInits.Add(FMtoUPendingInit{ActiveSession, MoveTemp(Init)});
                        continue;
                    }
                    if (!bReady)
                    {
                        SendErrorAndDisconnect(
                            TEXT("INVALID_MESSAGE"),
                            TEXT("A frame was received before the init message was accepted."));
                        break;
                    }

                    FString MessageType;
                    {
                        FString TypeError;
                        if (!FMtoUProtocol::PeekType(Payload, MessageType, TypeError))
                        {
                            SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), TypeError);
                            break;
                        }
                    }

                    // Exactly one host owns time in each mode: while linkage
                    // is active, cache work and live frames contradict the
                    // authority statement and are refused with their code.
                    if (bWorkerLinkageActive.Load()
                        && (bWorkerLinkageAwaitingPeer.Load() || WorkerLinkageLeavesPending.Load() > 0)
                        && MessageType == TEXT("frame"))
                    {
                        // Maya has not seen the transition Unreal started yet,
                        // or Maya left linkage and the game thread has not
                        // applied the exit yet; frames in flight are dropped,
                        // not refused. Maya's sampling timer sends the next.
                        continue;
                    }
                    if (bWorkerLinkageActive.Load()
                        && (MessageType == TEXT("frame")
                            || MessageType.StartsWith(TEXT("cache_"))))
                    {
                        SendLinkageError(
                            TEXT("MODE_CONFLICT"),
                            TEXT("Unreal owns the sequence time while linkage is active."));
                        continue;
                    }
                    // Cached Playback keeps the single-character contract: a
                    // two-object set is refused before any cache state exists.
                    if (!WorkerObjectCounts.IsEmpty() && MessageType.StartsWith(TEXT("cache_")))
                    {
                        SendLinkageError(
                            TEXT("OBJECT_SET_UNSUPPORTED"),
                            TEXT("Cached Playback supports one character; remove the second object first."));
                        continue;
                    }

                    if (MessageType == TEXT("mode")
                        || MessageType == TEXT("leave_linkage")
                        || MessageType == TEXT("remove_object")
                        || MessageType == TEXT("reference_result")
                        || MessageType == TEXT("reference_removed")
                        || MessageType == TEXT("reference_query"))
                    {
                        // Linkage control work happens on the game thread
                        // because it mutates the takeover and the published
                        // pose; the worker only parses and stages it.
                        if (!CanAdmitLinkageControl())
                        {
                            SendLinkageError(
                                TEXT("RESOURCE_LIMIT"),
                                TEXT("The linkage control queue is full."));
                            break;
                        }
                        FMtoUPendingLinkageControl Control;
                        FString LinkageError;
                        FString LinkageErrorCode = TEXT("INVALID_MESSAGE");
                        bool bValid = true;
                        if (MessageType == TEXT("mode"))
                        {
                            Control.Kind = FMtoUPendingLinkageControl::EKind::Mode;
                            bValid = FMtoUProtocol::ParseMode(
                                Payload, Control.Mode, LinkageError, &LinkageErrorCode);
                        }
                        else if (MessageType == TEXT("leave_linkage"))
                        {
                            Control.Kind = FMtoUPendingLinkageControl::EKind::LeaveLinkage;
                            bValid = FMtoUProtocol::ParseLeaveLinkage(
                                Payload, Control.LeaveLinkage, LinkageError, &LinkageErrorCode);
                        }
                        else if (MessageType == TEXT("remove_object"))
                        {
                            Control.Kind = FMtoUPendingLinkageControl::EKind::RemoveObject;
                            bValid = FMtoUProtocol::ParseRemoveObject(
                                Payload, Control.RemoveObject, LinkageError, &LinkageErrorCode);
                        }
                        else if (MessageType == TEXT("reference_result"))
                        {
                            Control.Kind = FMtoUPendingLinkageControl::EKind::ReferenceResult;
                            bValid = FMtoUProtocol::ParseReferenceResult(
                                Payload, Control.ReferenceResult, LinkageError, &LinkageErrorCode);
                        }
                        else if (MessageType == TEXT("reference_removed"))
                        {
                            Control.Kind = FMtoUPendingLinkageControl::EKind::ReferenceRemoved;
                            bValid = FMtoUProtocol::ParseReferenceRemoved(
                                Payload, Control.ReferenceRemoved, LinkageError, &LinkageErrorCode);
                        }
                        else
                        {
                            Control.Kind = FMtoUPendingLinkageControl::EKind::ReferenceQuery;
                            bValid = FMtoUProtocol::ParseReferenceQuery(
                                Payload, Control.ReferenceQuery, LinkageError, &LinkageErrorCode);
                        }
                        if (!bValid)
                        {
                            // A refused linkage control message is a
                            // per-message rejection: the session answers with
                            // its stable code and keeps the connection, so an
                            // operator can correct the action without losing
                            // the streaming session (protocol/v10.md, mode and
                            // reference message tables).
                            SendLinkageError(LinkageErrorCode, LinkageError);
                            continue;
                        }
                        if (Control.Kind == FMtoUPendingLinkageControl::EKind::LeaveLinkage)
                        {
                            // Counted before it is visible to the game thread,
                            // so the drain can never undercount it.
                            WorkerLinkageLeavesPending += 1;
                        }
                        PendingLinkageControls.Enqueue(MoveTemp(Control));
                        QueuedLinkageControls.Store(QueuedLinkageControls.Load() + 1);
                        continue;
                    }
                    if (MessageType == TEXT("group"))
                    {
                        // The group budget meters the whole message, so the
                        // encoded size is checked before any staging.
                        if (!CanAdmitLinkageControl())
                        {
                            SendLinkageError(
                                TEXT("RESOURCE_LIMIT"),
                                TEXT("The linkage control queue is full."));
                            break;
                        }
                        FMtoUPendingPoseGroup Pending;
                        Pending.SessionId = ActiveSession;
                        FString GroupError;
                        FString GroupErrorCode = TEXT("INVALID_MESSAGE");
                        if (!FMtoUProtocol::ParseGroup(
                                Payload, Payload.Num(), Pending.Message, GroupError, &GroupErrorCode))
                        {
                            // An oversized group is refused with the frozen
                            // code and keeps the last complete display; a
                            // malformed one is structurally invalid.
                            if (GroupErrorCode == TEXT("RESOURCE_LIMIT"))
                            {
                                SendLinkageError(TEXT("RESOURCE_LIMIT"), GroupError);
                                continue;
                            }
                            SendErrorAndDisconnect(GroupErrorCode, GroupError);
                            break;
                        }
                        PendingPoseGroups.Enqueue(MoveTemp(Pending));
                        QueuedLinkageControls.Store(QueuedLinkageControls.Load() + 1);
                        continue;
                    }
                    if (MessageType == TEXT("cache_frame"))
                    {
                        // Admission happens before parsing: a held frame
                        // never gains parsed-queue ownership beyond the
                        // frozen budget.
                        if (!HandleCacheFrame(MoveTemp(Payload)))
                        {
                            break;
                        }
                        continue;
                    }
                    if (MessageType == TEXT("cache_enter")
                        || MessageType == TEXT("cache_begin"))
                    {
                        FMtoUCacheCommand Command;
                        Command.EncodedBytes = Payload.Num();
                        FString CacheError;
                        FString ErrorCode = TEXT("INVALID_MESSAGE");
                        bool bShapeValid = true;
                        if (MessageType == TEXT("cache_begin"))
                        {
                            Command.Kind = FMtoUCacheCommand::EKind::Begin;
                            bShapeValid = FMtoUProtocol::ParseCacheBegin(
                                Payload,
                                Command.Begin,
                                CacheError,
                                ErrorCode);
                            // Frozen metadata/identity limits are recoverable:
                            // reject without closing so Maya can recapture.
                            if (!bShapeValid && ErrorCode != TEXT("INVALID_MESSAGE"))
                            {
                                // A structurally valid begin always switches
                                // intake identity. If later metadata fails,
                                // poison that identity so already-pipelined
                                // frame/end messages cannot inherit the prior
                                // upload or gain parsed queue ownership.
                                WorkerCacheUploadId = Command.Begin.UploadId;
                                bWorkerCacheUploadRejected = true;
                                CacheCommands.CancelUpload(
                                    ActiveSession, WorkerCacheUploadId);
                                bDidWork = true;
                                SendPacket(
                                    *ClientSocket,
                                    FMtoUProtocol::EncodeError(
                                        ErrorCode,
                                        CacheError,
                                        CacheError,
                                        Command.Begin.UploadId),
                                    bStopRequested);
                                continue;
                            }
                            if (bShapeValid)
                            {
                                // Later frame/end commands carry this upload
                                // identity for wholesale cancellation.
                                WorkerCacheUploadId = Command.Begin.UploadId;
                                bWorkerCacheUploadRejected = false;
                            }
                        }
                        else
                        {
                            Command.Kind = FMtoUCacheCommand::EKind::Enter;
                            bShapeValid = FMtoUProtocol::ParseCacheEnter(Payload, CacheError);
                        }
                        if (!bShapeValid)
                        {
                            SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), CacheError);
                            break;
                        }
                        EnqueueCacheCommand(MoveTemp(Command));
                        continue;
                    }
                    if (MessageType == TEXT("cache_end"))
                    {
                        if (bWorkerCacheUploadRejected)
                        {
                            continue;
                        }
                        FString CacheError;
                        if (!FMtoUProtocol::ParseCacheEnd(Payload, CacheError))
                        {
                            SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), CacheError);
                            break;
                        }
                        FMtoUCacheCommand Command;
                        Command.Kind = FMtoUCacheCommand::EKind::End;
                        EnqueueCacheCommand(MoveTemp(Command));
                        continue;
                    }
                    if (MessageType == TEXT("cache_play"))
                    {
                        FString CacheError;
                        FMtoUCacheCommand Command;
                        Command.Kind = FMtoUCacheCommand::EKind::Play;
                        if (!FMtoUProtocol::ParseCachePlay(Payload, Command.PlayId, CacheError))
                        {
                            SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), CacheError);
                            break;
                        }
                        EnqueueCacheCommand(MoveTemp(Command));
                        continue;
                    }
                    if (MessageType == TEXT("cache_stop") || MessageType == TEXT("cache_clear"))
                    {
                        FString CacheError;
                        bool bValid = false;
                        FMtoUCacheCommand Command;
                        if (MessageType == TEXT("cache_stop"))
                        {
                            bValid = FMtoUProtocol::ParseCacheStop(Payload, CacheError);
                            Command.Kind = FMtoUCacheCommand::EKind::Stop;
                        }
                        else
                        {
                            bValid = FMtoUProtocol::ParseCacheClear(
                                Payload, Command.ClearId, CacheError);
                            Command.Kind = FMtoUCacheCommand::EKind::Clear;
                        }
                        if (!bValid)
                        {
                            SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), CacheError);
                            break;
                        }
                        if (Command.Kind == FMtoUCacheCommand::EKind::Clear
                            && Command.ClearId > WorkerLastSeenClearId)
                        {
                            WorkerLastSeenClearId = Command.ClearId;
                            WorkerCacheUploadId = 0;
                            bWorkerCacheUploadRejected = false;
                        }
                        EnqueueCacheCommand(MoveTemp(Command));
                        continue;
                    }

                    FMtoUFrameMessage Frame;
                    bool bStructuralError = true;
                    if (!FMtoUProtocol::ParseFrame(Payload, Frame, Error))
                    {
                        SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), Error);
                        break;
                    }
                    if (!FMtoUProtocol::ValidateFrame(
                            Frame,
                            WorkerExpectedBoneCount,
                            WorkerExpectedCurveCount,
                            Error,
                            bStructuralError,
                            &WorkerObjectCounts))
                    {
                        if (bStructuralError)
                        {
                            SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), Error);
                            break;
                        }
                        UE_LOG(LogMtoULiveLinkSource, Warning, TEXT("Dropped frame: %s"), *Error);
                        SetStatus(FString::Printf(TEXT("Dropped frame: %s"), *Error));
                        continue;
                    }

                    {
                        FScopeLock Lock(&PendingMutex);
                        PendingFrame = FMtoUPendingFrame{ActiveSession, MoveTemp(Frame)};
                    }
                    SetStatus(TEXT("Connected to Maya"));
                }
            }
        }

        if (!bDidWork)
        {
            FPlatformProcess::Sleep(0.005f);
        }
    }

    MarkDisconnected();
    CloseSocket(*SocketSubsystem, ListenSocket);
    return 0;
}

bool FMtoULiveLinkSource::HandleInitOnGameThread(FMtoUInitMessage&& Message)
{
    check(IsInGameThread());
    const uint64 SessionId = GameThreadSession;
    const bool bPrimary = Message.ObjectId == FMtoUObjectIds::Character;
    if (bPrimary)
    {
        // The primary opens the set; every later init of this connection
        // names the same set, in its order.
        SessionObjects.Reset();
        ParticipatingActors.Reset();
        PendingApplication.Reset();
        RestoreGeneration.Reset();
        SetObjects = Message.SetObjects;
    }
    else if (SessionObjects.IsEmpty()
        || Message.SetObjects != SetObjects
        || !SetObjects.IsValidIndex(SessionObjects.Num())
        || SetObjects[SessionObjects.Num()] != Message.ObjectId
        || Message.SetRevision != NegotiatedSetRevision
        || Message.SpaceRevision != NegotiatedSpaceRevision)
    {
        EnqueueErrorOnGameThread(
            TEXT("INVALID_MESSAGE"),
            TEXT("An init does not belong to the object set this connection negotiates."),
            FString::Printf(TEXT("init object '%s' of set [%s] at set revision %d"),
                *Message.ObjectId, *FString::Join(Message.SetObjects, TEXT(", ")),
                Message.SetRevision));
        return false;
    }

    // The v9 workflows (realtime, cached playback, Model) stream into the one
    // placed Binding actor without a linkage pairing; the pairing is the
    // consent to take over the actor's sequence writers and is required at the
    // linkage entry instead. Several placed actors, or a second object, need
    // the pairing to say which actor serves which object.
    const FMtoULinkagePairing* Pairing = LinkageSession.FindPairing(Message.ObjectId);
    TArray<AMtoULiveLinkActor*> Actors;
    // ponytail: one editor-world object scan per object; add actor registration only if profiling shows this scan matters.
    for (TObjectIterator<AMtoULiveLinkActor> It; It; ++It)
    {
        if (IsPlacedEditorActor(**It))
        {
            Actors.Add(*It);
        }
    }
    Actors.Sort([](const AMtoULiveLinkActor& Left, const AMtoULiveLinkActor& Right)
    {
        return Left.GetPathName() < Right.GetPathName();
    });

    if (Actors.IsEmpty())
    {
        EnqueueErrorOnGameThread(
            TEXT("NO_BINDING_ACTOR"),
            TEXT("Place an MtoU_LiveLink binding actor in the Editor world before connecting. PIE is not supported."));
        return false;
    }
    AMtoULiveLinkActor* Actor = nullptr;
    if (Pairing)
    {
        // The paired target must be a placed Binding actor this session can
        // stream into; a pairing that points anywhere else is not consent.
        Actor = Pairing->Actor.Get();
        if (!Actor || !Actors.Contains(Actor))
        {
            EnqueueErrorOnGameThread(
                TEXT("PAIRING_REQUIRED"),
                TEXT("Pair this actor's Binding with the Maya object before connecting."),
                FString::Printf(
                    TEXT("Maya object '%s' is paired with %s, which is not a placed Binding actor."),
                    *Message.ObjectId,
                    Pairing->BindingPath.IsEmpty() ? TEXT("another target") : *Pairing->BindingPath));
            return false;
        }
    }
    else if (!bPrimary || SetObjects.Num() > 1)
    {
        EnqueueErrorOnGameThread(
            TEXT("PAIRING_REQUIRED"),
            TEXT("Pair both Binding actors with their Maya objects before connecting the set."),
            FString::Printf(
                TEXT("Maya object '%s' has no paired Binding actor; a set of %d objects needs a pairing for each."),
                *Message.ObjectId, SetObjects.Num()));
        return false;
    }
    else if (Actors.Num() > 1)
    {
        TArray<FString> Names;
        Names.Reserve(Actors.Num());
        for (const AMtoULiveLinkActor* Candidate : Actors)
        {
            Names.Add(Candidate->GetPathName());
        }
        EnqueueErrorOnGameThread(
            TEXT("MULTIPLE_BINDING_ACTORS"),
            TEXT("Exactly one MtoU_LiveLink binding actor is required."),
            FString::Printf(TEXT("Found %d actors:\n%s"), Actors.Num(), *FString::Join(Names, TEXT("\n"))));
        return false;
    }
    else
    {
        Actor = Actors[0];
    }
    if (!bPrimary)
    {
        const AMtoULiveLinkActor* PrimaryActor = SessionObjects[0].Actor.Get();
        if (PrimaryActor == Actor)
        {
            EnqueueErrorOnGameThread(
                TEXT("TARGET_ALREADY_PAIRED"),
                TEXT("One Binding actor cannot serve two objects of the set."),
                FString::Printf(TEXT("%s already serves the primary character."), *Actor->GetName()));
            return false;
        }
        // Every object of a set is published in the primary's anchor, so the
        // second target must stand at the same placement; its own offset
        // would otherwise move the object a second time.
        FString Difference;
        if (PrimaryActor
            && !MtoUSharesAnchor(PrimaryActor->GetActorTransform(), Actor->GetActorTransform(), Difference))
        {
            const FString Details = FString::Printf(
                TEXT("%s stands %s %s; the targets of one set share one placement."),
                *Actor->GetName(), *Difference, *PrimaryActor->GetName());
            Actor->SetConnectionStatus(FString::Printf(TEXT("Error: %s"), *Details));
            EnqueueErrorOnGameThread(
                TEXT("ANCHOR_MISMATCH"),
                TEXT("The second object's Binding actor does not share the primary placement."),
                Details);
            return false;
        }
    }

    ParticipatingActors.Add(Actor);
    Actor->SetConnectionStatus(TEXT("Validating"));

    // One user-facing text for every invalid-Binding refusal.
    const TCHAR* const InvalidBindingMessage =
        TEXT("The MtoU_LiveLink binding is invalid.");
    UMtoULiveLinkBinding* Binding = Actor->GetBinding();
    if (!Binding)
    {
        const FString Details = FString::Printf(
            TEXT("%s has no MtoU_LiveLink Binding."), *Actor->GetName());
        Actor->SetConnectionStatus(FString::Printf(TEXT("Error: %s"), *Details));
        EnqueueErrorOnGameThread(
            FMtoUCompositionFailures::InvalidBinding,
            InvalidBindingMessage,
            Details);
        return false;
    }
    // Freeze the required skeleton of every enabled mesh for this session.
    const FMtoUCharacterComposition Composition = FMtoUCharacterComposition::Resolve(Binding);
    if (!Composition.IsUsable())
    {
        const FString Details = FString::Printf(
            TEXT("%s character composition is not usable:\n%s"),
            *Actor->GetName(),
            *Composition.Diagnostics);
        Actor->SetConnectionStatus(FString::Printf(TEXT("Error: %s"), *Details));
        EnqueueErrorOnGameThread(
            Composition.FailureCategory,
            Composition.FailureCategory == FMtoUCompositionFailures::SkeletonMismatch
                ? TEXT("The character has incompatible required skeletal dependencies.")
                : InvalidBindingMessage,
            Details);
        return false;
    }
    USkeletalMesh* DriverMesh = Composition.Parts[0].Mesh;

    const bool bModelWorkflow = Message.Workflow == FMtoUWorkflows::Model;
    const FMtoUPreviewReadiness Readiness = Actor->GetPreviewReadiness();
    if (bModelWorkflow && !Readiness.IsUsable())
    {
        // Model preview must refuse without changing the visible target until
        // an explicit Refresh produces a ready Generated Preview. Negotiation
        // consumes readiness and never modifies it.
        const FString Details = FString::Printf(
            TEXT("%s has no ready Generated Preview Skeletal Mesh. Run Refresh Preview in Unreal before connecting in the Model workflow."),
            *Actor->GetName());
        Actor->SetConnectionStatus(TEXT("Preview not ready"));
        EnqueueErrorOnGameThread(
            TEXT("PREVIEW_NOT_READY"),
            TEXT("The Model workflow requires a ready Generated Preview Skeletal Mesh."),
            Details);
        return false;
    }

    // Animation drives the bound Primary Driver Skeletal Mesh; Model drives the
    // Generated garment, the original Driver follower, and every enabled part.
    // Either way the streamed library is the composed character's Morph Targets,
    // so a name that only some meshes own still reaches the meshes that have it.
    USkeletalMesh* Mesh = bModelWorkflow ? Readiness.GeneratedPreview : DriverMesh;
    FMtoUTargetDescription Target;
    Target.bAllowUnusedSourceBones = true;
    Target.BoneOwners = Composition.BoneOwners;
    const FReferenceSkeleton& Required = Composition.RequiredSkeleton;
    for (int32 Index = 0; Index < Required.GetNum(); ++Index)
    {
        Target.Bones.Add({Required.GetBoneName(Index), Required.GetParentIndex(Index)});
    }
    for (const TObjectPtr<UMorphTarget>& Morph : Mesh->GetMorphTargets())
    {
        if (Morph) { Target.MorphTargetNames.AddUnique(Morph->GetFName()); }
    }
    Composition.AppendMorphNames(Target.MorphTargetNames);
    const int32 TargetMorphCount = Target.MorphTargetNames.Num();

    FMtoUCharacterDescription Character;
    Character.Bones = Message.Bones;
    Character.CurveNames = Message.Curves;
    FMtoUNegotiationOutcome Outcome =
        FMtoUConnectionNegotiator::Negotiate(Character, Target);
    if (!Outcome.bUsable)
    {
        const FString Details = FString::Printf(
            TEXT("%s skeleton cannot be mapped:\n%s"),
            *Actor->GetName(),
            *Outcome.TechnicalDetails());
        Actor->SetConnectionStatus(FString::Printf(TEXT("Error: %s"), *Details));
        EnqueueErrorOnGameThread(
            Outcome.FailureCategory,
            bPrimary
                ? TEXT("The Maya and Unreal skeletons do not match.")
                : TEXT("The second object's Maya and Unreal skeletons do not match."),
            Details);
        return false;
    }

    const auto JoinNames = [](const TArray<FName>& Names)
    {
        TArray<FString> Text;
        Text.Reserve(Names.Num());
        for (const FName Name : Names)
        {
            Text.Add(Name.ToString());
        }
        return Text.IsEmpty() ? FString(TEXT("none")) : FString::Join(Text, TEXT(", "));
    };
    // A zero-name manifest declares an intentionally bone-driven outfit: its
    // empty Accepted Preview Morph set is valid even with BS transmission
    // enabled. Only a non-empty manifest with zero accepted names is a failed
    // pairing, so it keeps blocking with PREVIEW_MORPH_MISMATCH.
    const bool bZeroMorphManifest = bModelWorkflow
        && Message.bBlendshapesEnabled
        && Message.Curves.IsEmpty();
    const bool bPartialMorphCoverage = bModelWorkflow
        && Message.bBlendshapesEnabled
        && !bZeroMorphManifest
        && (!Outcome.MayaOnlyMorphNames.IsEmpty() || !Outcome.UnrealOnlyMorphNames.IsEmpty());
    if (bModelWorkflow)
    {
        const int32 AcceptedCount = Message.bBlendshapesEnabled
            ? Outcome.AcceptedCurveNames.Num()
            : 0;
        const bool bEmptyRequiredIntersection = Message.bBlendshapesEnabled
            && !bZeroMorphManifest
            && Outcome.AcceptedCurveIndices.IsEmpty();
        const EMtoUModelDiagnosticLevel DiagnosticLevel = bEmptyRequiredIntersection
            ? EMtoUModelDiagnosticLevel::Error
            : Message.bBlendshapesEnabled
                ? (bPartialMorphCoverage
                    ? EMtoUModelDiagnosticLevel::Partial
                    : EMtoUModelDiagnosticLevel::Full)
                : EMtoUModelDiagnosticLevel::BoneOnly;
        const FString Indicator = bEmptyRequiredIntersection
            ? TEXT("ERROR: No accepted Preview Morphs.")
            : bZeroMorphManifest
                ? TEXT("Bone-driven outfit: the current Maya outfit declares no BlendShapes.")
                : Message.bBlendshapesEnabled
                    ? (bPartialMorphCoverage
                        ? TEXT("YELLOW: Partial Preview Morph coverage.")
                        : TEXT("Full Preview Morph coverage."))
                    : TEXT("ORANGE: Bone-only diagnostic; not valid for model acceptance.");
        Actor->SetModelDiagnostics(
            FString::Printf(
                TEXT("%s\nMaya current BlendShape count: %d\nModel display Morph total: %d\nAccepted count: %d\nMaya-only count: %d (%s)\nUE-only count: %d (%s)"),
                *Indicator,
                Message.Curves.Num(),
                TargetMorphCount,
                AcceptedCount,
                Outcome.MayaOnlyMorphNames.Num(),
                *JoinNames(Outcome.MayaOnlyMorphNames),
                Outcome.UnrealOnlyMorphNames.Num(),
                *JoinNames(Outcome.UnrealOnlyMorphNames)),
            DiagnosticLevel);
    }

    if (bModelWorkflow && Message.bBlendshapesEnabled
        && !bZeroMorphManifest
        && Outcome.AcceptedCurveIndices.IsEmpty())
    {
        // BS transmission with a non-empty manifest and zero accepted Preview
        // Morphs is blocking; the user must fix the pairing or explicitly
        // disable BS transmission.
        const FString Details = FString::Printf(
            TEXT("%s has no Morph Target intersection between Maya and the Model display meshes.\nMaya-only: %d, Unreal-only: %d."),
            *Actor->GetName(),
            Outcome.MayaOnlyMorphNames.Num(),
            Outcome.UnrealOnlyMorphNames.Num());
        Actor->SetConnectionStatus(TEXT("Preview morph mismatch"));
        EnqueueErrorOnGameThread(
            TEXT("PREVIEW_MORPH_MISMATCH"),
            TEXT("Model preview with BS transmission requires at least one accepted Morph Target."),
            Details);
        return false;
    }

    if (!Client || !SourceGuid.IsValid())
    {
        EnqueueErrorOnGameThread(
            TEXT("INTERNAL_ERROR"), TEXT("The Unreal Live Link client is unavailable."));
        return false;
    }
    if (!IsCurrentSession(SessionId))
    {
        return false;
    }

    FMtoUSessionObject Object;
    Object.ObjectId = Message.ObjectId;
    Object.SubjectKey = FLiveLinkSubjectKey(SourceGuid, MtoUSubjectNameForObject(Message.ObjectId));
    Object.Actor = Actor;
    Object.ExpectedBoneCount = Message.Bones.Num();
    Object.ExpectedCurveCount = Message.Curves.Num();
    Object.NegotiatedRevision = Message.Revision;
    if (bPrimary)
    {
        // The v11 identity of this session: the object set, its revision and
        // the common space every later group or mode message must repeat.
        NegotiatedSetRevision = Message.SetRevision;
        NegotiatedSpaceRevision = Message.SpaceRevision;
        LinkageSession.NegotiateIdentity(
            Message.ObjectId, NegotiatedSetRevision, NegotiatedSpaceRevision);
        CacheSession.BeginSession(
            {Object.ExpectedBoneCount, Object.ExpectedCurveCount, Object.NegotiatedRevision});
    }
    else
    {
        LinkageSession.NegotiateObject(Message.ObjectId);
    }
    Object.SourceBoneIndices.Init(INDEX_NONE, Required.GetNum());
    for (int32 MayaIndex = 0; MayaIndex < Outcome.TargetBoneIndices.Num(); ++MayaIndex)
    {
        const int32 TargetIndex = Outcome.TargetBoneIndices[MayaIndex];
        if (TargetIndex != INDEX_NONE) { Object.SourceBoneIndices[TargetIndex] = MayaIndex; }
    }
    Object.SourceBindLocalPose.Reset(Required.GetNum());
    Object.TargetRefLocalPose = Required.GetRefBonePose();
    Object.BoneParents.Reset(Required.GetNum());
    for (int32 Index = 0; Index < Required.GetNum(); ++Index)
    {
        Object.SourceBindLocalPose.Add(Message.SourceBindLocalPose[Object.SourceBoneIndices[Index]]);
        Object.BoneParents.Add(Required.GetParentIndex(Index));
    }
    // A bone-only session streams no Morph values at all. StaticData, the
    // ready reply, and the per-frame accepted indices must all agree on the
    // empty accepted set even though Maya still sends the full curve manifest.
    const bool bBoneOnlySession = bModelWorkflow && !Message.bBlendshapesEnabled;
    Object.AcceptedCurveIndices = bBoneOnlySession
        ? TArray<int32>()
        : Outcome.AcceptedCurveIndices;
    Object.AcceptedCurveNames = bBoneOnlySession
        ? TArray<FName>()
        : Outcome.AcceptedCurveNames;
    Message.Bones = Target.Bones;
    Object.ExpectedPrimaryMesh = Mesh;
    for (int32 PartIndex = 1; PartIndex < Composition.Parts.Num(); ++PartIndex)
    {
        const FMtoUCharacterPartResolution& Part = Composition.Parts[PartIndex];
        if (Part.bEnabled && Part.Mesh)
        {
            Object.ExpectedDisplayParts.Add({Part.PartId, Part.Mesh, Part.Name});
        }
    }
    Object.TargetBoneNames.Reserve(Target.Bones.Num());
    for (const FMtoUDescriptionBone& Bone : Target.Bones)
    {
        Object.TargetBoneNames.Add(Bone.Name);
    }
    // Every display of this actor evaluates this object's own subject.
    Actor->SetLiveLinkSubject(Object.SubjectKey.SubjectName);
    if (bModelWorkflow)
    {
        Actor->ShowGeneratedPreview(bBoneOnlySession);
    }
    else
    {
        Actor->ShowDriverMesh();
    }
    Actor->SetConnectionStatus(bBoneOnlySession
        ? TEXT("Connected: bone-only diagnostic; not valid for model acceptance")
        : bPartialMorphCoverage
            ? TEXT("Connected: partial Morph coverage")
            : bPrimary ? TEXT("Connected") : TEXT("Connected as the second object"));
    Actor->NoteSessionStreaming();
    SetEditorViewportRealtimeOverride(true);

    Client->PushSubjectStaticData_AnyThread(
        Object.SubjectKey,
        ULiveLinkAnimationRole::StaticClass(),
        FMtoUProtocol::MakeStaticData(
            Message,
            bBoneOnlySession ? TArray<FName>() : Outcome.AcceptedCurveNames));

    FMtoUOutgoing Reply;
    Reply.SessionId = SessionId;
    Reply.Packet = FMtoUProtocol::EncodeReady(
        Outcome.MayaOnlyMorphNames,
        Outcome.UnrealOnlyMorphNames,
        Outcome.BoneNameMappings,
        Message.Workflow,
        TargetMorphCount,
        bBoneOnlySession ? 0 : Outcome.AcceptedCurveNames.Num(),
        Object.NegotiatedRevision,
        static_cast<int32>(SessionId),
        Message.ObjectId,
        NegotiatedSetRevision,
        NegotiatedSpaceRevision);
    Reply.ExpectedBoneCount = Object.ExpectedBoneCount;
    Reply.ExpectedCurveCount = Object.ExpectedCurveCount;
    Reply.bReady = true;
    Reply.ReadyObjectId = Message.ObjectId;
    SessionObjects.Add(MoveTemp(Object));
    Reply.bSetComplete = SessionObjects.Num() == SetObjects.Num();
    OutgoingReplies.Enqueue(MoveTemp(Reply));
    return true;
}

void FMtoULiveLinkSource::DrainLinkageHellosOnGameThread()
{
    check(IsInGameThread());
    FMtoUPendingHello Hello;
    while (PendingHellos.Dequeue(Hello))
    {
        QueuedLinkageControls.Store(FMath::Max(0, QueuedLinkageControls.Load() - 1));
        if (Hello.SessionId == 0 || !IsSessionActiveOnGameThread(Hello.SessionId))
        {
            continue;
        }
        if (LinkageSession.GetSessionId() != static_cast<int32>(Hello.SessionId))
        {
            LinkageSession.BeginSession(static_cast<int32>(Hello.SessionId));
        }
        LinkageSession.NoteHello(Hello.Message);
    }
}

int32 FMtoULiveLinkSource::DrainLinkageControlsOnGameThread()
{
    check(IsInGameThread());
    const double NowSeconds = FPlatformTime::Seconds();
    int32 LeavesDrained = 0;
    FMtoUPendingLinkageControl Control;
    while (PendingLinkageControls.Dequeue(Control))
    {
        QueuedLinkageControls.Store(FMath::Max(0, QueuedLinkageControls.Load() - 1));
        if (Control.Kind == FMtoUPendingLinkageControl::EKind::LeaveLinkage)
        {
            ++LeavesDrained;
        }
        int32 SessionId = INDEX_NONE;
        switch (Control.Kind)
        {
        case FMtoUPendingLinkageControl::EKind::Mode:
            SessionId = Control.Mode.SessionId;
            break;
        case FMtoUPendingLinkageControl::EKind::LeaveLinkage:
            SessionId = Control.LeaveLinkage.SessionId;
            break;
        case FMtoUPendingLinkageControl::EKind::RemoveObject:
            SessionId = Control.RemoveObject.SessionId;
            break;
        case FMtoUPendingLinkageControl::EKind::ReferenceResult:
            SessionId = Control.ReferenceResult.SessionId;
            break;
        case FMtoUPendingLinkageControl::EKind::ReferenceRemoved:
            SessionId = Control.ReferenceRemoved.SessionId;
            break;
        case FMtoUPendingLinkageControl::EKind::ReferenceQuery:
            SessionId = Control.ReferenceQuery.SessionId;
            break;
        }
        if (SessionId == INDEX_NONE || SessionId < 1
            || !IsSessionActiveOnGameThread(static_cast<uint64>(SessionId)))
        {
            continue;
        }
        if (LinkageSession.GetSessionId() != SessionId)
        {
            LinkageSession.BeginSession(SessionId);
        }
        switch (Control.Kind)
        {
        case FMtoUPendingLinkageControl::EKind::Mode:
            LinkageSession.NoteMode(Control.Mode, NowSeconds);
            break;
        case FMtoUPendingLinkageControl::EKind::LeaveLinkage:
            LinkageSession.NoteLeaveLinkage(Control.LeaveLinkage, NowSeconds);
            break;
        case FMtoUPendingLinkageControl::EKind::RemoveObject:
            LinkageSession.NoteRemoveObject(Control.RemoveObject);
            break;
        case FMtoUPendingLinkageControl::EKind::ReferenceResult:
            LinkageSession.NoteReferenceResult(Control.ReferenceResult);
            break;
        case FMtoUPendingLinkageControl::EKind::ReferenceRemoved:
            LinkageSession.NoteReferenceRemoved(Control.ReferenceRemoved);
            break;
        case FMtoUPendingLinkageControl::EKind::ReferenceQuery:
            LinkageSession.NoteReferenceQuery(Control.ReferenceQuery);
            break;
        }
    }
    return LeavesDrained;
}

void FMtoULiveLinkSource::IntakeLinkageGroupsOnGameThread()
{
    check(IsInGameThread());
    const double NowSeconds = FPlatformTime::Seconds();
    FMtoUPendingPoseGroup Pending;
    while (PendingPoseGroups.Dequeue(Pending))
    {
        QueuedLinkageControls.Store(FMath::Max(0, QueuedLinkageControls.Load() - 1));
        if (Pending.SessionId == 0 || !IsSessionActiveOnGameThread(Pending.SessionId))
        {
            continue;
        }
        if (LinkageSession.GetSessionId() != static_cast<int32>(Pending.SessionId))
        {
            LinkageSession.BeginSession(static_cast<int32>(Pending.SessionId));
        }
        LinkageSession.StagePoseGroup(Pending.Message, NowSeconds);
    }
}

void FMtoULiveLinkSource::CommitLinkageGroupOnGameThread()
{
    check(IsInGameThread());
    // The published group answers first: its evidence follows only once the
    // displays really hold it, and no newer group is published meanwhile, so
    // a display can never mix two generations.
    if (PendingApplication.IsSet())
    {
        const FMtoULinkageView View = LinkageSession.GetView();
        if (View.Mode != TEXT("linkage")
            || View.Identity.ModeRevision != PendingApplication->ModeRevision
            || !LinkageSession.IsIssued(
                PendingApplication->EvalId, PendingApplication->IssueSerial))
        {
            // The mode moved on or the target was cancelled: the group is never
            // confirmed and the display returns to the last complete one.
            PendingApplication.Reset();
            RollBackPendingApplicationOnGameThread();
        }
        else
        {
            bool bApplied = false;
            TArray<FMtoUAppliedComponent> Components;
            FString Reason;
            if (!ResolvePendingApplicationOnGameThread(
                    PendingApplication.GetValue(), bApplied, Components, Reason))
            {
                return;
            }
            // The barrier decided: this is the only place the record is
            // released, so the evidence below always names the group it was.
            const FMtoUPendingApplication Done = MoveTemp(PendingApplication.GetValue());
            PendingApplication.Reset();
            if (!bApplied)
            {
                RollBackPendingApplicationOnGameThread();
            }
            LinkageSession.NoteGroupApplied(
                Done.EvalId,
                Done.SampleSerial,
                bApplied,
                Done.DisplayFrame,
                Components,
                Reason,
                FPlatformTime::Seconds());
            return;
        }
    }
    if (!FinishDisplayRestoreOnGameThread())
    {
        return;
    }

    // One staged group is published per Update() so a burst of answers can
    // never starve the editor tick.
    FMtoUPoseGroupMessage Group;
    if (!LinkageSession.TakeNextStagedGroup(Group))
    {
        return;
    }
    TArray<FMtoUAppliedComponent> Components;
    FString Reason;
    if (!ApplyPoseGroupOnGameThread(Group, Components, Reason))
    {
        LinkageSession.NoteGroupApplied(
            Group.EvalId,
            Group.SampleSerial,
            false,
            Group.UeDisplayFrame,
            Components,
            Reason,
            FPlatformTime::Seconds());
    }
}

void FMtoULiveLinkSource::RemoveSecondaryObjectOnGameThread(
    const FString& ObjectId, int32 SetRevision)
{
    check(IsInGameThread());
    const int32 Index = SessionObjects.IndexOfByPredicate(
        [&ObjectId](const FMtoUSessionObject& Object) { return Object.ObjectId == ObjectId; });
    FString Residual;
    if (Index <= 0)
    {
        Residual = FString::Printf(TEXT("object '%s' is not a removable object of the negotiated set"), *ObjectId);
    }
    else
    {
        // 1. The old set's published group is never confirmed: every object
        //    returns to its last complete group.
        if (PendingApplication.IsSet())
        {
            PendingApplication.Reset();
            RollBackPendingApplicationOnGameThread();
        }
        // 2. The object's own writers come back while the primary's stay
        //    muted, so the primary's takeover is never interrupted.
        const FMtoULinkageHooks& Hooks = LinkageSession.GetHooks();
        if (LinkageSession.GetView().bTakeoverHeld && Hooks.RestoreObject)
        {
            FString RestoreError;
            FString RestoreCode;
            if (!Hooks.RestoreObject(ObjectId, RestoreError, RestoreCode))
            {
                Residual = RestoreError.IsEmpty() ? RestoreCode : RestoreError;
            }
        }
        // 3. The object leaves the stream: its subject goes away and its
        //    displays show their own Binding state again.
        FMtoUSessionObject Removed = MoveTemp(SessionObjects[Index]);
        SessionObjects.RemoveAt(Index);
        SetObjects.Remove(ObjectId);
        ParticipatingActors.Remove(Removed.Actor);
        if (AMtoULiveLinkActor* RemovedActor = Removed.Actor.Get())
        {
            for (int32 HeldIndex = HeldDisplays.Num() - 1; HeldIndex >= 0; --HeldIndex)
            {
                USkeletalMeshComponent* Display = HeldDisplays[HeldIndex].Get();
                if (!Display || Display->GetOwner() == RemovedActor)
                {
                    if (Display)
                    {
                        Display->SetComponentTickEnabled(true);
                    }
                    HeldDisplays.RemoveAt(HeldIndex);
                }
            }
        }
        ReleaseSessionObjectOnGameThread(Removed, TEXT("Removed from the set"));
    }
    NegotiatedSetRevision = SetRevision;
    // 4. The new set is confirmed; the worker stops expecting the object's
    //    frame rows once Maya can see this packet.
    LinkageSession.CompleteObjectRemoval(ObjectId, SetRevision, Residual);
    FMtoUOutgoing Reply;
    Reply.SessionId = GameThreadSession;
    Reply.Packet = FMtoUProtocol::EncodeObjectRemoved(
        static_cast<int32>(GameThreadSession), ObjectId, SetRevision, Residual.IsEmpty(),
        Residual.IsEmpty() ? FString(TEXT("the removed target was restored")) : Residual);
    Reply.RemovedObjectId = ObjectId;
    Reply.bControl = true;
    QueuedOutgoingControls.Store(QueuedOutgoingControls.Load() + 1);
    OutgoingReplies.Enqueue(MoveTemp(Reply));
    if (!Residual.IsEmpty())
    {
        // A removal that cannot restore its object ends the whole set rather
        // than continuing with a target nobody can hand back.
        EnqueueErrorOnGameThread(
            TEXT("RECOVERY_REFUSED"),
            TEXT("The second object could not be restored; the connection ended."),
            Residual);
    }
}

void FMtoULiveLinkSource::ReleaseSessionObjectOnGameThread(
    FMtoUSessionObject& Object, const TCHAR* ActorStatus)
{
    check(IsInGameThread());
    if (Client && SourceGuid.IsValid())
    {
        Client->RemoveSubject_AnyThread(Object.SubjectKey);
    }
    if (AMtoULiveLinkActor* Actor = Object.Actor.Get())
    {
        Actor->ReapplyDisplayTarget();
        Actor->SetConnectionStatus(ActorStatus);
        Actor->NoteSessionEnded();
    }
}

bool FMtoULiveLinkSource::MakeObjectFrameOnGameThread(
    const FMtoUSessionObject& Object,
    const TArray<FMtoUTransform>& Transforms,
    const TArray<double>& Curves,
    FLiveLinkFrameDataStruct& OutFrame,
    FString& OutError) const
{
    FMtoUFrameMessage RequiredFrame;
    RequiredFrame.Curves = Curves;
    RequiredFrame.Transforms.Reserve(Object.SourceBoneIndices.Num());
    for (const int32 SourceIndex : Object.SourceBoneIndices)
    {
        if (!Transforms.IsValidIndex(SourceIndex))
        {
            OutError = FString::Printf(
                TEXT("object '%s' carries no transform for source bone %d"), *Object.ObjectId, SourceIndex);
            return false;
        }
        RequiredFrame.Transforms.Add(Transforms[SourceIndex]);
    }
    return FMtoUProtocol::MakeRetargetedFrameData(
        RequiredFrame,
        Object.AcceptedCurveIndices,
        Object.SourceBindLocalPose,
        Object.TargetRefLocalPose,
        Object.BoneParents,
        OutFrame,
        OutError);
}

bool FMtoULiveLinkSource::ApplyPoseGroupOnGameThread(
    const FMtoUPoseGroupMessage& Group,
    TArray<FMtoUAppliedComponent>& OutComponents,
    FString& OutReason)
{
    check(IsInGameThread());
    OutComponents.Reset();
    OutReason.Reset();
    if (SessionObjects.IsEmpty() || SessionObjects.Num() != SetObjects.Num())
    {
        OutReason = TEXT("The session has no negotiated object set to apply a group to.");
        return false;
    }
    // The group must answer for exactly the negotiated set, each object once.
    TArray<const FMtoUPoseGroup*> Rows;
    Rows.Init(nullptr, SessionObjects.Num());
    for (const FMtoUPoseGroup& Row : Group.Objects)
    {
        const int32 Index = SessionObjects.IndexOfByPredicate(
            [&Row](const FMtoUSessionObject& Object) { return Object.ObjectId == Row.ObjectId; });
        if (Index == INDEX_NONE)
        {
            OutReason = FString::Printf(
                TEXT("The group names object '%s' instead of the negotiated set."), *Row.ObjectId);
            return false;
        }
        if (Rows[Index])
        {
            OutReason = FString::Printf(TEXT("The group names object '%s' twice."), *Row.ObjectId);
            return false;
        }
        Rows[Index] = &Row;
    }
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        const FMtoUSessionObject& Object = SessionObjects[Index];
        const FMtoUPoseGroup* Row = Rows[Index];
        if (!Row)
        {
            OutReason = FString::Printf(
                TEXT("The group lacks object '%s' of the negotiated set."), *Object.ObjectId);
            return false;
        }
        if (Row->Transforms.Num() != Object.ExpectedBoneCount
            || Row->Curves.Num() != Object.ExpectedCurveCount)
        {
            OutReason = FString::Printf(
                TEXT("Object '%s': the group carries %d bones and %d curves; the session negotiated %d and %d."),
                *Object.ObjectId,
                Row->Transforms.Num(),
                Row->Curves.Num(),
                Object.ExpectedBoneCount,
                Object.ExpectedCurveCount);
            return false;
        }
    }
    if (!Client || !SourceGuid.IsValid()
        || GameThreadSession == 0 || !IsSessionPublishableOnGameThread(GameThreadSession))
    {
        OutReason = TEXT("The group could not be published through the Live Link subjects.");
        return false;
    }

    // Every display every object consists of must be able to show the group
    // before it is published; otherwise nothing is published and the last
    // complete display stays.
    TArray<FMtoULinkageDisplay> Displays;
    if (!CollectLinkageDisplaysOnGameThread(Displays, OutReason))
    {
        return false;
    }

    // Every object is retargeted before any subject receives the group, so a
    // pose one object cannot take publishes nothing at all.
    FMtoUPendingApplication Pending;
    Pending.EvalId = Group.EvalId;
    Pending.SampleSerial = Group.SampleSerial;
    Pending.DisplayFrame = Group.UeDisplayFrame;
    Pending.ModeRevision = Group.ModeRevision;
    Pending.SetRevision = Group.SetRevision;
    Pending.IssueSerial = LinkageSession.GetIssueSerial();
    Pending.Generation = FString::Printf(
        TEXT("%d:%s#%d"), ++LinkageGeneration, *Group.EvalId, Group.SampleSerial);
    Pending.Objects.SetNum(SessionObjects.Num());
    TArray<FLiveLinkFrameDataStruct> Frames;
    Frames.SetNum(SessionObjects.Num());
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        const FMtoUPoseGroup& Row = *Rows[Index];
        TArray<FMtoUTransform> Transforms;
        Transforms.Reserve(Row.Transforms.Num());
        for (const FTransform& Transform : Row.Transforms)
        {
            Transforms.Add({
                Transform.GetTranslation(), Transform.GetRotation(), Transform.GetScale3D()});
        }
        FString RetargetError;
        if (!MakeObjectFrameOnGameThread(
                SessionObjects[Index], Transforms, Row.Curves, Frames[Index], RetargetError))
        {
            // Fail closed exactly like a live frame: a pose transform that
            // cannot be inverted is a content fault the user must fix.
            EnqueueErrorOnGameThread(
                TEXT("BIND_POSE_INVALID"),
                TEXT("A pose transform cannot be inverted; streaming stopped."),
                RetargetError);
            OutReason = TEXT("The group could not be published through the Live Link subjects.");
            return false;
        }
        // The generation travels with each object's frame into its subject,
        // so the completion barrier can tell this publication from the last.
        if (FLiveLinkBaseFrameData* Base = Frames[Index].GetBaseData())
        {
            Base->MetaData.StringMetaData.Add(MtoULinkageGenerationKey, Pending.Generation);
        }
        if (const FLiveLinkAnimationFrameData* Animation =
                Frames[Index].Cast<FLiveLinkAnimationFrameData>())
        {
            Pending.Objects[Index].ExpectedTransforms = Animation->Transforms;
            Pending.Objects[Index].ExpectedCurves = Animation->PropertyValues;
        }
        Pending.Objects[Index].Frame.InitializeWith(Frames[Index]);
    }
    // The displays are held before any subject can hold the group, so none
    // shows it before the whole group is verified.
    HoldLinkageDisplaysOnGameThread(true);
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        Client->PushSubjectFrameData_AnyThread(SessionObjects[Index].SubjectKey, MoveTemp(Frames[Index]));
    }
    Pending.PublishedSeconds = FPlatformTime::Seconds();
    PendingApplication = MoveTemp(Pending);
    return true;
}

namespace
{
// A display holds a published value when it matches within these tolerances:
// centimetres for translation, quaternion components for rotation.
constexpr double MtoUAppliedTransformTolerance = 1.0e-2;
constexpr double MtoUAppliedMorphTolerance = 1.0e-3;

float DisplayedMorphWeight(const USkeletalMeshComponent& Display, const UMorphTarget& Morph)
{
    if (const int32* WeightIndex = Display.ActiveMorphTargets.Find(&Morph))
    {
        return Display.MorphTargetWeights.IsValidIndex(*WeightIndex)
            ? Display.MorphTargetWeights[*WeightIndex]
            : 0.0f;
    }
    return 0.0f;
}
}

FString FMtoULiveLinkSource::SubjectGenerationOnGameThread(int32 ObjectIndex) const
{
    check(IsInGameThread());
    FLiveLinkSubjectFrameData SubjectFrame;
    if (Client && SourceGuid.IsValid() && SessionObjects.IsValidIndex(ObjectIndex)
        && Client->EvaluateFrameFromSource_AnyThread(
            SessionObjects[ObjectIndex].SubjectKey, ULiveLinkAnimationRole::StaticClass(), SubjectFrame)
        && SubjectFrame.FrameData.GetBaseData())
    {
        return SubjectFrame.FrameData.GetBaseData()->MetaData.StringMetaData
            .FindRef(MtoULinkageGenerationKey);
    }
    return FString();
}

bool FMtoULiveLinkSource::ResolvePendingApplicationOnGameThread(
    const FMtoUPendingApplication& Pending,
    bool& bOutApplied,
    TArray<FMtoUAppliedComponent>& OutComponents,
    FString& OutReason)
{
    check(IsInGameThread());
    bOutApplied = false;
    OutComponents.Reset();
    OutReason.Reset();
    const bool bTimedOut = FPlatformTime::Seconds() - Pending.PublishedSeconds
        > FMtoUProtocol::LinkageConvergenceSeconds;
    if (!Client || !SourceGuid.IsValid())
    {
        OutReason = TEXT("The Unreal Live Link client is unavailable.");
        return true;
    }
    if (Pending.Objects.Num() != SessionObjects.Num())
    {
        OutReason = FString::Printf(
            TEXT("Group %s belongs to another object set."), *Pending.Generation);
        return true;
    }

    // 1. The Live Link client applies a pushed frame on its own tick, so every
    //    object's subject must hold this generation before any display can
    //    show it. The displays are held meanwhile, so none consumes it alone.
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        const FString Seen = SubjectGenerationOnGameThread(Index);
        if (Seen != Pending.Generation)
        {
            if (!bTimedOut)
            {
                return false;
            }
            OutReason = SessionObjects.Num() == 1
                ? FString::Printf(
                    TEXT("The Live Link subject never received group %s; it still holds %s."),
                    *Pending.Generation,
                    Seen.IsEmpty() ? TEXT("no linkage group") : *Seen)
                : FString::Printf(
                    TEXT("The Live Link subject of object '%s' never received group %s; it still holds %s."),
                    *SessionObjects[Index].ObjectId,
                    *Pending.Generation,
                    Seen.IsEmpty() ? TEXT("no linkage group") : *Seen);
            return true;
        }
    }

    // 2. Every display of every object is still there and still evaluates
    //    its object's subject.
    TArray<FMtoULinkageDisplay> Displays;
    FString Mismatch;
    if (!CollectLinkageDisplaysOnGameThread(Displays, Mismatch))
    {
        OutReason = FString::Printf(
            TEXT("Group %s was not displayed: %s"), *Pending.Generation, *Mismatch);
        return true;
    }

    // 3. The whole group is verified before anything is shown: each display
    //    evaluates its subject into a scratch pose, and only when every one of
    //    them, of every object, holds exactly the group's bones and Morph
    //    values does any of them commit. A failure here leaves every display
    //    on the previous group.
    if (!StageDisplaysOnGameThread(Pending, Displays, OutComponents, Mismatch))
    {
        OutReason = FString::Printf(
            TEXT("Group %s was not displayed completely: %s"), *Pending.Generation, *Mismatch);
        return true;
    }

    // 4. One commit for the whole set in this game-thread step, before the
    //    frame renders, together with the Model follower's Morph overrides.
    for (const FMtoULinkageDisplay& Entry : Displays)
    {
        Entry.Display->TickAnimation(0.0f, false);
        Entry.Display->RefreshBoneTransforms();
    }
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        if (AMtoULiveLinkActor* Actor = SessionObjects[Index].Actor.Get())
        {
            Actor->ApplyModelMorphCurves(
                SessionObjects[Index].AcceptedCurveNames, Pending.Objects[Index].ExpectedCurves);
        }
    }
    if (!VerifyDisplaysOnGameThread(Pending, Displays, OutComponents, Mismatch))
    {
        // The commit disagrees with the verified evaluation; the caller
        // restores the last complete group on every display.
        OutReason = FString::Printf(
            TEXT("Group %s was not displayed completely: %s"), *Pending.Generation, *Mismatch);
        return true;
    }
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        SessionObjects[Index].LastCompleteFrame.InitializeWith(Pending.Objects[Index].Frame);
        SessionObjects[Index].LastCompleteCurves = Pending.Objects[Index].ExpectedCurves;
    }
    bOutApplied = true;
    return true;
}

bool FMtoULiveLinkSource::CollectLinkageDisplaysOnGameThread(
    TArray<FMtoULinkageDisplay>& OutDisplays,
    FString& OutReason) const
{
    check(IsInGameThread());
    OutDisplays.Reset();
    OutReason.Reset();
    const bool bSingle = SessionObjects.Num() == 1;
    for (int32 ObjectIndex = 0; ObjectIndex < SessionObjects.Num(); ++ObjectIndex)
    {
        const FMtoUSessionObject& Object = SessionObjects[ObjectIndex];
        // A one-object set keeps the labels it always reported; a set names
        // the object a missing display belongs to.
        const FString Prefix = bSingle
            ? FString()
            : FString::Printf(TEXT("object '%s' "), *Object.ObjectId);
        const auto Ready = [&](USkeletalMeshComponent* Display, const USkeletalMesh* Expected,
            const FString& Label) -> bool
        {
            if (!Display || !Expected || Display->GetSkeletalMeshAsset() != Expected)
            {
                OutReason = FString::Printf(TEXT("%s does not display its Skeletal Mesh"), *Label);
                return false;
            }
            if (!Display->IsRegistered())
            {
                OutReason = FString::Printf(TEXT("%s is not registered"), *Label);
                return false;
            }
            if (!Cast<ULiveLinkInstance>(Display->GetAnimInstance()))
            {
                OutReason = FString::Printf(
                    TEXT("%s does not evaluate the Live Link subject"), *Label);
                return false;
            }
            OutDisplays.Add({ObjectIndex, Display});
            return true;
        };
        AMtoULiveLinkActor* Actor = Object.Actor.Get();
        if (!Actor)
        {
            OutReason = bSingle
                ? FString(TEXT("no participating actor owns this session"))
                : FString::Printf(TEXT("the target of object '%s' is gone"), *Object.ObjectId);
            return false;
        }
        if (!Ready(Actor->GetSkeletalMeshComponent(), Object.ExpectedPrimaryMesh.Get(),
                Prefix + TEXT("the primary display")))
        {
            return false;
        }
        // Every enabled Additional Part of the negotiated character must be
        // on screen; a part whose display went away is a missing part, even
        // when the primary mesh covers all of its bones.
        for (const FMtoUExpectedDisplayPart& Part : Object.ExpectedDisplayParts)
        {
            UMtoUCharacterPartComponent* Display = nullptr;
            for (const TObjectPtr<UMtoUCharacterPartComponent>& Candidate :
                Actor->GetCharacterPartComponents())
            {
                if (Candidate && Candidate->PartId == Part.PartId)
                {
                    Display = Candidate.Get();
                    break;
                }
            }
            if (!Ready(Display, Part.Mesh.Get(),
                    FString::Printf(TEXT("%spart '%s'"), *Prefix, *Part.Name)))
            {
                return false;
            }
        }
    }
    if (SessionObjects.IsEmpty())
    {
        OutReason = TEXT("no participating actor owns this session");
        return false;
    }
    return true;
}

bool FMtoULiveLinkSource::MatchDisplayPose(
    const FMtoUSessionObject& Object,
    const FMtoUPendingObjectApplication& Pending,
    const USkeletalMeshComponent& Display,
    const TArray<FTransform>& Local,
    TFunctionRef<float(const UMorphTarget&, FName)> MorphWeight,
    FMtoUAppliedComponent& InOutComponent,
    TSet<int32>& InOutDisplayedBones,
    FString& OutMismatch) const
{
    const USkeletalMesh* Mesh = Display.GetSkeletalMeshAsset();
    if (!Mesh)
    {
        OutMismatch = FString::Printf(TEXT("display '%s' has no mesh"), *Display.GetName());
        return false;
    }
    const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
    if (Local.Num() != Skeleton.GetNum())
    {
        OutMismatch = FString::Printf(
            TEXT("display '%s' holds no evaluated pose"), *Display.GetName());
        return false;
    }
    // Each object is compared only with its own bones and Morph Targets, so a
    // name the other object shares can never satisfy this one.
    for (int32 TargetIndex = 0; TargetIndex < Object.TargetBoneNames.Num(); ++TargetIndex)
    {
        const int32 MeshIndex = Skeleton.FindBoneIndex(Object.TargetBoneNames[TargetIndex]);
        if (MeshIndex == INDEX_NONE || !Pending.ExpectedTransforms.IsValidIndex(TargetIndex))
        {
            continue;
        }
        if (!Local[MeshIndex].Equals(
                Pending.ExpectedTransforms[TargetIndex], MtoUAppliedTransformTolerance))
        {
            OutMismatch = FString::Printf(
                TEXT("display '%s' shows another pose at bone '%s'"),
                *Display.GetName(), *Object.TargetBoneNames[TargetIndex].ToString());
            return false;
        }
        ++InOutComponent.BoneCount;
        InOutDisplayedBones.Add(TargetIndex);
    }
    for (int32 CurveIndex = 0; CurveIndex < Object.AcceptedCurveNames.Num(); ++CurveIndex)
    {
        const UMorphTarget* Morph = Mesh->FindMorphTarget(Object.AcceptedCurveNames[CurveIndex]);
        if (!Morph || !Pending.ExpectedCurves.IsValidIndex(CurveIndex))
        {
            continue;
        }
        const float Expected = Pending.ExpectedCurves[CurveIndex];
        const float Actual = MorphWeight(*Morph, Object.AcceptedCurveNames[CurveIndex]);
        if (!FMath::IsNearlyEqual(Actual, Expected, MtoUAppliedMorphTolerance))
        {
            OutMismatch = FString::Printf(
                TEXT("display '%s' shows Morph '%s' at %.4f instead of %.4f"),
                *Display.GetName(), *Object.AcceptedCurveNames[CurveIndex].ToString(),
                Actual, Expected);
            return false;
        }
        ++InOutComponent.MorphCount;
    }
    if (Display.IsA<UMtoUCharacterPartComponent>())
    {
        ++InOutComponent.PartCount;
    }
    return true;
}

bool FMtoULiveLinkSource::CoversNegotiatedSkeleton(
    const FMtoUSessionObject& Object, const TSet<int32>& DisplayedBones, FString& OutMismatch) const
{
    for (int32 TargetIndex = 0; TargetIndex < Object.TargetBoneNames.Num(); ++TargetIndex)
    {
        if (!DisplayedBones.Contains(TargetIndex))
        {
            OutMismatch = SessionObjects.Num() == 1
                ? FString::Printf(TEXT("bone '%s' is held by no display component"),
                    *Object.TargetBoneNames[TargetIndex].ToString())
                : FString::Printf(TEXT("bone '%s' of object '%s' is held by no display component"),
                    *Object.TargetBoneNames[TargetIndex].ToString(), *Object.ObjectId);
            return false;
        }
    }
    return true;
}

bool FMtoULiveLinkSource::StageDisplaysOnGameThread(
    const FMtoUPendingApplication& Pending,
    const TArray<FMtoULinkageDisplay>& Displays,
    TArray<FMtoUAppliedComponent>& OutComponents,
    FString& OutMismatch)
{
    check(IsInGameThread());
    OutComponents.Reset();
    OutMismatch.Reset();
#if WITH_EDITOR
    TArray<TSet<int32>> DisplayedBones;
    DisplayedBones.SetNum(SessionObjects.Num());
    OutComponents.SetNum(SessionObjects.Num());
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        OutComponents[Index].ObjectId = SessionObjects[Index].ObjectId;
    }
    for (const FMtoULinkageDisplay& Entry : Displays)
    {
        USkeletalMeshComponent* Display = Entry.Display;
        // The display's anim instance reads its object's subject; the
        // evaluation goes into scratch buffers, so the pose the display shows
        // stays as it is.
        Display->TickAnimation(0.0f, false);
        TArray<FTransform> Space;
        Space.SetNum(Display->GetComponentSpaceTransforms().Num());
        TArray<FTransform> Local;
        FVector RootTranslation = FVector::ZeroVector;
        FBlendedHeapCurve Curves;
        UE::Anim::FMeshAttributeContainer Attributes;
        if (Space.Num() > 0)
        {
            Display->PerformAnimationEvaluation(
                Display->GetSkeletalMeshAsset(), Display->GetAnimInstance(),
                Space, Local, RootTranslation, Curves, Attributes);
        }
        if (!MatchDisplayPose(
                SessionObjects[Entry.ObjectIndex], Pending.Objects[Entry.ObjectIndex],
                *Display, Local,
                [&Curves](const UMorphTarget&, FName Name) { return Curves.Get(Name); },
                OutComponents[Entry.ObjectIndex], DisplayedBones[Entry.ObjectIndex], OutMismatch))
        {
            OutComponents.Reset();
            return false;
        }
    }
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        if (!CoversNegotiatedSkeleton(SessionObjects[Index], DisplayedBones[Index], OutMismatch))
        {
            OutComponents.Reset();
            return false;
        }
    }
    return true;
#else
    (void)Pending;
    (void)Displays;
    OutMismatch = TEXT("linkage display verification needs the Unreal Editor");
    return false;
#endif
}

bool FMtoULiveLinkSource::VerifyDisplaysOnGameThread(
    const FMtoUPendingApplication& Pending,
    const TArray<FMtoULinkageDisplay>& Displays,
    TArray<FMtoUAppliedComponent>& OutComponents,
    FString& OutMismatch) const
{
    check(IsInGameThread());
    OutComponents.Reset();
    OutMismatch.Reset();
    TArray<TSet<int32>> DisplayedBones;
    DisplayedBones.SetNum(SessionObjects.Num());
    OutComponents.SetNum(SessionObjects.Num());
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        OutComponents[Index].ObjectId = SessionObjects[Index].ObjectId;
    }
    for (const FMtoULinkageDisplay& Entry : Displays)
    {
        USkeletalMeshComponent* Display = Entry.Display;
        if (!MatchDisplayPose(
                SessionObjects[Entry.ObjectIndex], Pending.Objects[Entry.ObjectIndex],
                *Display, Display->GetBoneSpaceTransforms(),
                [Display](const UMorphTarget& Morph, FName)
                {
                    return DisplayedMorphWeight(*Display, Morph);
                },
                OutComponents[Entry.ObjectIndex], DisplayedBones[Entry.ObjectIndex], OutMismatch))
        {
            OutComponents.Reset();
            return false;
        }
    }
    // The whole negotiated skeleton of every object must be on screen.
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        if (!CoversNegotiatedSkeleton(SessionObjects[Index], DisplayedBones[Index], OutMismatch))
        {
            OutComponents.Reset();
            return false;
        }
    }
    return true;
}

void FMtoULiveLinkSource::RollBackPendingApplicationOnGameThread()
{
    check(IsInGameThread());
    if (!Client || !SourceGuid.IsValid())
    {
        return;
    }
    // A fresh copy of every object's last complete frame, with a current time
    // and one restore generation, so each subject's latest frame is the
    // complete one again and the displays are evaluated once every subject
    // holds exactly this copy.
    const FString Generation = FString::Printf(TEXT("restore:%d"), ++LinkageGeneration);
    bool bPushed = false;
    for (FMtoUSessionObject& Object : SessionObjects)
    {
        if (!Object.LastCompleteFrame.IsValid())
        {
            continue;
        }
        FLiveLinkFrameDataStruct Restore;
        Restore.InitializeWith(Object.LastCompleteFrame);
        if (FLiveLinkBaseFrameData* Base = Restore.GetBaseData())
        {
            Base->WorldTime = FLiveLinkWorldTime();
            Base->MetaData.StringMetaData.Add(MtoULinkageGenerationKey, FString::Printf(
                TEXT("%s|%s"),
                *Base->MetaData.StringMetaData.FindRef(MtoULinkageGenerationKey),
                *Generation));
        }
        Client->PushSubjectFrameData_AnyThread(Object.SubjectKey, MoveTemp(Restore));
        bPushed = true;
    }
    if (bPushed)
    {
        RestoreGeneration = Generation;
        RestoreSeconds = FPlatformTime::Seconds();
    }
}

bool FMtoULiveLinkSource::FinishDisplayRestoreOnGameThread()
{
    check(IsInGameThread());
    if (RestoreGeneration.IsEmpty())
    {
        return true;
    }
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        if (!SessionObjects[Index].LastCompleteFrame.IsValid())
        {
            continue;
        }
        if (!SubjectGenerationOnGameThread(Index).EndsWith(TEXT("|") + RestoreGeneration))
        {
            if (FPlatformTime::Seconds() - RestoreSeconds <= FMtoUProtocol::LinkageConvergenceSeconds)
            {
                return false;
            }
            // A subject never took the restored frame: evaluating now would
            // show whatever it holds, so the displays keep what they show.
            RestoreGeneration.Reset();
            return true;
        }
    }
    RestoreGeneration.Reset();
    // Every display, also one re-initialized meanwhile, shows the last
    // complete group again, and the Model follower's overrides follow it.
    for (const FMtoUSessionObject& Object : SessionObjects)
    {
        AMtoULiveLinkActor* Actor = Object.Actor.Get();
        if (!Actor)
        {
            continue;
        }
        TArray<USkeletalMeshComponent*> Displays;
        Displays.Add(Actor->GetSkeletalMeshComponent());
        for (const TObjectPtr<UMtoUCharacterPartComponent>& Part :
            Actor->GetCharacterPartComponents())
        {
            Displays.Add(Part.Get());
        }
        for (USkeletalMeshComponent* Display : Displays)
        {
            if (Display && Display->IsRegistered() && Display->GetSkeletalMeshAsset()
                && Cast<ULiveLinkInstance>(Display->GetAnimInstance()))
            {
                Display->TickAnimation(0.0f, false);
                Display->RefreshBoneTransforms();
            }
        }
        if (!Object.LastCompleteCurves.IsEmpty())
        {
            Actor->ApplyModelMorphCurves(Object.AcceptedCurveNames, Object.LastCompleteCurves);
        }
    }
    return true;
}

void FMtoULiveLinkSource::HoldLinkageDisplaysOnGameThread(bool bHold)
{
    check(IsInGameThread());
    if (!bHold)
    {
        for (const TWeakObjectPtr<USkeletalMeshComponent>& Held : HeldDisplays)
        {
            if (USkeletalMeshComponent* Display = Held.Get())
            {
                Display->SetComponentTickEnabled(true);
            }
        }
        HeldDisplays.Reset();
        return;
    }
    // Only the tick is held, never a saved property, so nothing of the hold
    // reaches the level; a display registered again is held again here.
    const auto Hold = [this](USkeletalMeshComponent* Display)
    {
        if (Display && Display->IsComponentTickEnabled())
        {
            Display->SetComponentTickEnabled(false);
            HeldDisplays.AddUnique(Display);
        }
    };
    for (const TWeakObjectPtr<AMtoULiveLinkActor>& WeakActor : ParticipatingActors)
    {
        if (AMtoULiveLinkActor* Actor = WeakActor.Get())
        {
            Hold(Actor->GetSkeletalMeshComponent());
            for (const TObjectPtr<UMtoUCharacterPartComponent>& Part :
                Actor->GetCharacterPartComponents())
            {
                Hold(Part.Get());
            }
        }
    }
}

void FMtoULiveLinkSource::SendLinkageControlPacketOnGameThread(TArray<uint8> Packet, bool bControl)
{
    check(IsInGameThread());
    if (Packet.IsEmpty())
    {
        return;
    }
    const int32 SessionId = LinkageSession.GetSessionId() != 0
        ? LinkageSession.GetSessionId()
        : static_cast<int32>(GameThreadSession);
    if (bControl && QueuedOutgoingControls.Load() >= FMtoUProtocol::MaxControlQueue)
    {
        // Overflow is an error, never an unbounded backlog.
        SetStatus(TEXT("Linkage control queue overflow."));
        FMtoUOutgoing Reply;
        Reply.SessionId = SessionId;
        Reply.Packet = FMtoUProtocol::EncodeError(
            TEXT("RESOURCE_LIMIT"),
            TEXT("The linkage control queue overflowed."),
            FString::Printf(TEXT("%d undelivered control messages"), FMtoUProtocol::MaxControlQueue));
        Reply.bCloseAfter = true;
        Reply.bControl = true;
        QueuedOutgoingControls.Store(QueuedOutgoingControls.Load() + 1);
        OutgoingReplies.Enqueue(MoveTemp(Reply));
        return;
    }
    if (bControl)
    {
        QueuedOutgoingControls.Store(QueuedOutgoingControls.Load() + 1);
    }
    FMtoUOutgoing Reply;
    Reply.SessionId = SessionId;
    Reply.Packet = MoveTemp(Packet);
    Reply.bControl = bControl;
    OutgoingReplies.Enqueue(MoveTemp(Reply));
}

bool FMtoULiveLinkSource::IsSessionActiveOnGameThread(uint64 SessionId) const
{
    check(IsInGameThread());
    if (SessionId == 0)
    {
        return false;
    }
    if (SessionId == GameThreadSession)
    {
        return IsSessionPublishableOnGameThread(SessionId);
    }
    // The greeting phase precedes negotiation: the worker still owns the
    // socket and no explicit refresh has ended this session yet.
    return IsCurrentSession(SessionId);
}

void FMtoULiveLinkSource::HandleCacheCommandsOnGameThread()
{
    check(IsInGameThread());
    FMtoUCacheCommand Command;
    while (CacheCommands.TryConsume(Command))
    {
        if (Command.SessionId != GameThreadSession || GameThreadSession == 0
            || !IsSessionPublishableOnGameThread(Command.SessionId))
        {
            continue;
        }
        if ((Command.Kind == FMtoUCacheCommand::EKind::Frame
             || Command.Kind == FMtoUCacheCommand::EKind::End)
            && Command.UploadId != CacheSession.GetActiveUploadId())
        {
            // Rejected, superseded, or cleared upload: discard every pending
            // command of that identity at once. They can never produce a
            // flood of stale errors or touch a newer upload attempt.
            CacheCommands.CancelUpload(Command.SessionId, Command.UploadId);
            continue;
        }
        ApplyCacheTransitionOnGameThread(
            Command.SessionId, CacheSession.HandleCommand(Command));
    }

    // The synchronous epoch gate stays in the source: a replaced session
    // neither advances cached replay nor applies a transition's side effects.
    if (IsSessionPublishableOnGameThread(GameThreadSession))
    {
        ApplyCacheTransitionOnGameThread(GameThreadSession, CacheSession.Tick());
    }
}

void FMtoULiveLinkSource::ApplyCacheTransitionOnGameThread(
    uint64 SessionId, const FMtoUCacheTransition& Transition)
{
    check(IsInGameThread());
    if (!IsSessionPublishableOnGameThread(SessionId))
    {
        return;
    }
    if (Transition.RealtimeOverride.IsSet())
    {
        SetEditorViewportRealtimeOverride(
            Transition.RealtimeOverride.GetValue() && bSourceValid.Load());
    }

    // This adapter only formats status and encodes semantic outcomes. Cache
    // command/state interpretation and identity capture belong to the session.
    TArray<uint8> Packet;
    using EKind = FMtoUCacheTransition::EKind;
    switch (Transition.Kind)
    {
        case EKind::Entered:
            SetStatus(TEXT("Cached Playback entry; holding recent pose"));
            break;
        case EKind::Receiving:
            SetStatus(FString::Printf(
                TEXT("Receiving animation cache (%d frames)..."), Transition.FrameCount));
            break;
        case EKind::Ready:
            SetStatus(TEXT("Cached animation ready"));
            Packet = FMtoUProtocol::EncodeCacheReady(
                Transition.UploadId, Transition.Revision, Transition.FrameCount);
            break;
        case EKind::Playing:
            SetStatus(TEXT("Playing cached animation locally"));
            Packet = FMtoUProtocol::EncodeCachePlaying(
                Transition.UploadId, Transition.PlayId, Transition.bLoopEnabled);
            break;
        case EKind::Paused:
            SetStatus(TEXT("Cached playback paused; current pose held"));
            Packet = FMtoUProtocol::EncodeCachePaused(
                Transition.PlayId, Transition.AppliedFrames, Transition.SourceFrame);
            break;
        case EKind::Resumed:
            SetStatus(TEXT("Cached playback resumed from the held pose"));
            Packet = FMtoUProtocol::EncodeCacheResumed(
                Transition.PlayId, Transition.AppliedFrames, Transition.SourceFrame);
            break;
        case EKind::Seeked:
            SetStatus(TEXT("Cached playback positioned; pose held"));
            Packet = FMtoUProtocol::EncodeCacheSeeked(
                Transition.PlayId, Transition.SourceFrame, Transition.AppliedFrames);
            break;
        case EKind::Looped:
            SetStatus(TEXT("Cached playback looped to the first frame"));
            Packet = FMtoUProtocol::EncodeCacheLooped(
                Transition.PlayId, Transition.LoopRound, Transition.SourceFrame);
            break;
        case EKind::LoopChanged:
            SetStatus(Transition.bLoopEnabled
                ? TEXT("Cached playback will loop")
                : TEXT("Cached playback completes the current round"));
            Packet = FMtoUProtocol::EncodeCacheLoopChanged(
                Transition.PlayId, Transition.bLoopEnabled);
            break;
        case EKind::Stopped:
            SetStatus(TEXT("Cached playback stopped; last applied frame held"));
            if (Transition.PlayId > 0)
            {
                Packet = FMtoUProtocol::EncodeCacheStopped(Transition.PlayId);
            }
            break;
        case EKind::Cleared:
            SetStatus(TEXT("Connected to Maya"));
            Packet = FMtoUProtocol::EncodeCacheCleared(
                Transition.UploadId, Transition.PlayId, Transition.ClearId);
            break;
        case EKind::Completed:
            SetStatus(Transition.bWholeCacheScope
                ? TEXT("Cached playback complete; final frame held")
                : TEXT("Cached playback segment complete; final frame held"));
            Packet = FMtoUProtocol::EncodeCacheComplete(
                Transition.PlayId,
                Transition.FrameCount,
                Transition.ElapsedSeconds,
                Transition.bWholeCacheScope,
                Transition.StartFrame,
                Transition.EndFrame);
            break;
        case EKind::PerformanceFailed:
            SetStatus(TEXT("Cached playback missed the captured scene rate"));
            Packet = FMtoUProtocol::EncodeError(
                Transition.ErrorCode,
                TEXT("Local playback fell behind the captured scene rate."),
                Transition.Details, Transition.UploadId, Transition.PlayId);
            break;
        case EKind::Rejected:
            UE_LOG(LogMtoULiveLinkSource, Warning,
                TEXT("Rejected cache command: %s"), *Transition.Details);
            Packet = FMtoUProtocol::EncodeError(
                Transition.ErrorCode, Transition.Details, Transition.Details,
                Transition.UploadId, Transition.PlayId);
            break;
        case EKind::None:
            break;
    }
    if (!Packet.IsEmpty())
    {
        EnqueueReplyPacketOnGameThread(SessionId, MoveTemp(Packet), false);
    }
}

void FMtoULiveLinkSource::EnqueueReplyPacketOnGameThread(uint64 SessionId, TArray<uint8> Packet, bool bCloseAfter)
{
    check(IsInGameThread());
    if (!IsSessionPublishableOnGameThread(SessionId))
    {
        return;
    }
    FMtoUOutgoing Reply;
    Reply.SessionId = SessionId;
    Reply.Packet = MoveTemp(Packet);
    Reply.bCloseAfter = bCloseAfter;
    OutgoingReplies.Enqueue(MoveTemp(Reply));
}

void FMtoULiveLinkSource::PublishLatestFrameOnGameThread()
{
    check(IsInGameThread());
    TOptional<FMtoUPendingFrame> Frame;
    {
        FScopeLock Lock(&PendingMutex);
        if (PendingFrame.IsSet())
        {
            Frame = MoveTemp(PendingFrame);
            PendingFrame.Reset();
        }
    }
    if (!Frame.IsSet()
        || Frame->SessionId != GameThreadSession
        || !IsSessionPublishableOnGameThread(Frame->SessionId)
        || !Client
        || !SourceGuid.IsValid())
    {
        return;
    }
    PublishFrameOnGameThread(Frame->Message);
}
bool FMtoULiveLinkSource::PublishFrameOnGameThread(const FMtoUFrameMessage& Frame)
{
    check(IsInGameThread());
    if (!Client || !SourceGuid.IsValid())
    {
        return false;
    }
    if (GameThreadSession == 0 || !IsSessionPublishableOnGameThread(GameThreadSession)
        || SessionObjects.IsEmpty())
    {
        return false;
    }
    // Every object of the set is retargeted first and only then pushed, so a
    // frame one object cannot take changes no display: Unreal never shows the
    // two objects at two Maya times.
    TArray<FLiveLinkFrameDataStruct> Frames;
    Frames.SetNum(SessionObjects.Num());
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        const FMtoUSessionObject& Object = SessionObjects[Index];
        const TArray<FMtoUTransform>* Transforms = &Frame.Transforms;
        const TArray<double>* Curves = &Frame.Curves;
        if (Index > 0)
        {
            const FMtoUFrameObject* Row = Frame.Objects.FindByPredicate(
                [&Object](const FMtoUFrameObject& Candidate)
                {
                    return Candidate.ObjectId == Object.ObjectId;
                });
            if (!Row)
            {
                // A frame of another set (before Maya adopted a removal)
                // changes nothing.
                return false;
            }
            Transforms = &Row->Transforms;
            Curves = &Row->Curves;
        }
        FString RetargetError;
        if (!MakeObjectFrameOnGameThread(Object, *Transforms, *Curves, Frames[Index], RetargetError))
        {
            // Fail closed: publishing a partially wrong pose is worse than
            // refusing the frame, and silently dropping the pose instead would
            // hide a deterministic content fault that the user must fix, so the
            // session stops with an actionable diagnostic.
            EnqueueErrorOnGameThread(
                TEXT("BIND_POSE_INVALID"),
                TEXT("A pose transform cannot be inverted; streaming stopped."),
                RetargetError);
            return false;
        }
    }
    for (int32 Index = 0; Index < SessionObjects.Num(); ++Index)
    {
        FMtoUSessionObject& Object = SessionObjects[Index];
        // A live frame is complete as soon as it is shown; it is what a
        // linkage group that fails returns to.
        Object.LastCompleteFrame.InitializeWith(Frames[Index]);
        if (const FLiveLinkAnimationFrameData* Animation =
                Frames[Index].Cast<FLiveLinkAnimationFrameData>())
        {
            Object.LastCompleteCurves = Animation->PropertyValues;
            if (AMtoULiveLinkActor* Actor = Object.Actor.Get())
            {
                Actor->ApplyModelMorphCurves(Object.AcceptedCurveNames, Animation->PropertyValues);
            }
        }
        Client->PushSubjectFrameData_AnyThread(Object.SubjectKey, MoveTemp(Frames[Index]));
    }
    return true;
}

void FMtoULiveLinkSource::EnqueueErrorOnGameThread(
    const FString& Code,
    const FString& Message,
    const FString& Details)
{
    check(IsInGameThread());
    if (!IsSessionPublishableOnGameThread(GameThreadSession))
    {
        return;
    }
    SetStatus(Message);
    FMtoUOutgoing Reply;
    Reply.SessionId = GameThreadSession;
    Reply.Packet = FMtoUProtocol::EncodeError(Code, Message, Details);
    Reply.bCloseAfter = true;
    OutgoingReplies.Enqueue(MoveTemp(Reply));
}

bool FMtoULiveLinkSource::IsCurrentSession(uint64 SessionId) const
{
    FScopeLock Lock(&PendingMutex);
    return SessionId != 0 && CurrentWorkerSession == SessionId;
}

bool FMtoULiveLinkSource::IsSessionPublishableOnGameThread(uint64 SessionId) const
{
    check(IsInGameThread());
    if (SessionId == 0 || SessionId != GameThreadSession)
    {
        return false;
    }
    if (MtoUGetStreamingSessionEndCount() != GameThreadSessionEndEpoch)
    {
        return false;
    }
    return IsCurrentSession(SessionId);
}

void FMtoULiveLinkSource::SetStatus(const FString& InStatus)
{
    FScopeLock Lock(&StatusMutex);
    Status = InStatus;
}
