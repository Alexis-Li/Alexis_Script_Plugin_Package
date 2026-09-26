#include "MtoULiveLinkSource.h"

#include "MtoULiveLinkActor.h"
#include "MtoUCharacterComposition.h"
#include "MtoULiveLinkBinding.h"
#include "MtoUConnectionNegotiator.h"

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

TSharedPtr<FMtoULiveLinkSource> MtoUSetActiveSource(TSharedPtr<FMtoULiveLinkSource> Source)
{
    check(IsInGameThread());
    TSharedPtr<FMtoULiveLinkSource> Previous = GActiveSource.Pin();
    GActiveSource = Source;
    return Previous;
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
    for (const TWeakObjectPtr<AMtoULiveLinkActor>& Participant : ParticipatingActors)
    {
        if (Participant.Get() == &Actor)
        {
            return true;
        }
    }
    return false;
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

void FMtoULiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
    check(IsInGameThread());
    Client = InClient;
    SourceGuid = InSourceGuid;
    SubjectKey = FLiveLinkSubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));
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

    uint64 DisconnectedSession = 0;
    while (DisconnectedSessions.Dequeue(DisconnectedSession))
    {
        if (DisconnectedSession == GameThreadSession)
        {
            SetEditorViewportRealtimeOverride(false);
            if (Client && SourceGuid.IsValid())
            {
                Client->RemoveSubject_AnyThread(SubjectKey);
            }
            for (const TWeakObjectPtr<AMtoULiveLinkActor>& Actor : ParticipatingActors)
            {
                if (Actor.IsValid())
                {
                    Actor->ReapplyDisplayTarget();
                    Actor->SetConnectionStatus(TEXT("Disconnected"));
                }
            }
            GameThreadSession = 0;
            SourceBindLocalPose.Reset();
            TargetRefLocalPose.Reset();
            BoneParents.Reset();
            SourceBoneIndices.Reset();
            AcceptedCurveIndices.Reset();
            AcceptedCurveNames.Reset();
            // The transient cache and its upload/play identity history are
            // scoped to one negotiated streaming session; a newer connection
            // starts its own fresh sequence at 1.
            CacheSession.EndSession();
        }
    }

    TOptional<FMtoUPendingInit> Init;
    {
        FScopeLock Lock(&PendingMutex);
        if (PendingInit.IsSet())
        {
            Init = MoveTemp(PendingInit);
            PendingInit.Reset();
        }
    }
    if (Init.IsSet() && IsCurrentSession(Init->SessionId))
    {
        // An explicit refresh increments the termination counter before
        // replacing the display. An init queued for the replaced session must
        // never negotiate after the refresh started; only a new session id
        // may establish a new epoch and stream the newer revision.
        const bool bStaleInitForReplacedSession = Init->SessionId == GameThreadSession
            && GameThreadSession != 0
            && MtoUGetStreamingSessionEndCount() != GameThreadSessionEndEpoch;
        if (!bStaleInitForReplacedSession)
        {
            GameThreadSession = Init->SessionId;
            GameThreadSessionEndEpoch = MtoUGetStreamingSessionEndCount();
            HandleInitOnGameThread(MoveTemp(Init->Message));
        }
    }
    HandleCacheCommandsOnGameThread();

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
        SetEditorViewportRealtimeOverride(false);
        for (const TWeakObjectPtr<AMtoULiveLinkActor>& Actor : ParticipatingActors)
        {
            if (Actor.IsValid())
            {
                Actor->ReapplyDisplayTarget();
                Actor->SetConnectionStatus(TEXT("Disconnected"));
            }
        }
    }
}

int32 FMtoULiveLinkSource::GetQueuedCacheFrameCount() const
{
    return CacheCommands.GetPendingFrameCount();
}
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
    bool bInitReceived = false;
    bool bReady = false;
    int32 WorkerExpectedBoneCount = 0;
    int32 WorkerExpectedCurveCount = 0;
    FMtoUFrameDecoder Decoder;
    // Upload identity observed at intake; frame/end commands are stamped with
    // it so a rejected, superseded, or cleared upload can be cancelled as a
    // unit and can never affect a newer attempt.
    int32 WorkerCacheUploadId = 0;
    bool bWorkerCacheUploadRejected = false;
    // One raw (never parsed) cache_frame held while the Game Thread drains
    // the bounded intake queue; the sender experiences TCP backpressure
    // until then, so queued parsed ownership stays inside the budget.
    TOptional<TArray<uint8>> HeldCacheFrame;

    auto MarkDisconnected = [&]()
    {
        HeldCacheFrame.Reset();
        WorkerCacheUploadId = 0;
        bWorkerCacheUploadRejected = false;
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
                    PendingInit.Reset();
                    PendingFrame.Reset();
                }
            }
            DisconnectedSessions.Enqueue(ActiveSession);
        }
        CloseSocket(*SocketSubsystem, ClientSocket);
        ActiveSession = 0;
        bInitReceived = false;
        bReady = false;
        WorkerExpectedBoneCount = 0;
        WorkerExpectedCurveCount = 0;
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
                    bReady = false;
                    Decoder = FMtoUFrameDecoder();
                    {
                        FScopeLock Lock(&PendingMutex);
                        CurrentWorkerSession = ActiveSession;
                        PendingInit.Reset();
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
                    WorkerExpectedBoneCount = Reply.ExpectedBoneCount;
                    WorkerExpectedCurveCount = Reply.ExpectedCurveCount;
                    bReady = true;
                    SetStatus(TEXT("Connected to Maya"));
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
                        FMtoUInitMessage Init;
                        FString ErrorCode;
                        if (!FMtoUProtocol::ParseInit(Payload, Init, Error, &ErrorCode))
                        {
                            SendErrorAndDisconnect(ErrorCode, Error);
                            break;
                        }
                        bInitReceived = true;
                        FScopeLock Lock(&PendingMutex);
                        PendingInit = FMtoUPendingInit{ActiveSession, MoveTemp(Init)};
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
                            bValid = FMtoUProtocol::ParseCacheClear(Payload, CacheError);
                            Command.Kind = FMtoUCacheCommand::EKind::Clear;
                        }
                        if (!bValid)
                        {
                            SendErrorAndDisconnect(TEXT("INVALID_MESSAGE"), CacheError);
                            break;
                        }
                        if (Command.Kind == FMtoUCacheCommand::EKind::Clear)
                        {
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
                            bStructuralError))
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

void FMtoULiveLinkSource::HandleInitOnGameThread(FMtoUInitMessage&& Message)
{
    check(IsInGameThread());
    const uint64 SessionId = GameThreadSession;
    TArray<AMtoULiveLinkActor*> Actors;
    // ponytail: one editor-world object scan per connection; add actor registration only if profiling shows this scan matters.
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
        return;
    }
    if (Actors.Num() > 1)
    {
        TArray<FString> Names;
        Names.Reserve(Actors.Num());
        for (const AMtoULiveLinkActor* Actor : Actors)
        {
            Names.Add(Actor->GetPathName());
        }
        EnqueueErrorOnGameThread(
            TEXT("MULTIPLE_BINDING_ACTORS"),
            TEXT("Exactly one MtoU_LiveLink binding actor is required."),
            FString::Printf(TEXT("Found %d actors:\n%s"), Actors.Num(), *FString::Join(Names, TEXT("\n"))));
        return;
    }

    ParticipatingActors.Reset(Actors.Num());
    for (AMtoULiveLinkActor* Actor : Actors)
    {
        ParticipatingActors.Add(Actor);
        Actor->SetConnectionStatus(TEXT("Validating"));
    }

    AMtoULiveLinkActor* Actor = Actors[0];
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
        return;
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
        return;
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
        return;
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
            TEXT("The Maya and Unreal skeletons do not match."),
            Details);
        return;
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
        return;
    }

    if (!Client || !SourceGuid.IsValid())
    {
        EnqueueErrorOnGameThread(
            TEXT("INTERNAL_ERROR"), TEXT("The Unreal Live Link client is unavailable."));
        return;
    }
    if (!IsCurrentSession(SessionId))
    {
        return;
    }

    ExpectedBoneCount = Message.Bones.Num();
    ExpectedCurveCount = Message.Curves.Num();
    NegotiatedRevision = Message.Revision;
    CacheSession.BeginSession({ExpectedBoneCount, ExpectedCurveCount, NegotiatedRevision});
    SourceBoneIndices.Init(INDEX_NONE, Required.GetNum());
    for (int32 MayaIndex = 0; MayaIndex < Outcome.TargetBoneIndices.Num(); ++MayaIndex)
    {
        const int32 TargetIndex = Outcome.TargetBoneIndices[MayaIndex];
        if (TargetIndex != INDEX_NONE) { SourceBoneIndices[TargetIndex] = MayaIndex; }
    }
    SourceBindLocalPose.Reset(Required.GetNum());
    TargetRefLocalPose = Required.GetRefBonePose();
    BoneParents.Reset(Required.GetNum());
    for (int32 Index = 0; Index < Required.GetNum(); ++Index)
    {
        SourceBindLocalPose.Add(Message.SourceBindLocalPose[SourceBoneIndices[Index]]);
        BoneParents.Add(Required.GetParentIndex(Index));
    }
    // A bone-only session streams no Morph values at all. StaticData, the
    // ready reply, and the per-frame accepted indices must all agree on the
    // empty accepted set even though Maya still sends the full curve manifest.
    const bool bBoneOnlySession = bModelWorkflow && !Message.bBlendshapesEnabled;
    AcceptedCurveIndices = bBoneOnlySession
        ? TArray<int32>()
        : Outcome.AcceptedCurveIndices;
    AcceptedCurveNames = bBoneOnlySession
        ? TArray<FName>()
        : Outcome.AcceptedCurveNames;
    Message.Bones = Target.Bones;
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
            : TEXT("Connected"));
    SetEditorViewportRealtimeOverride(true);

    Client->PushSubjectStaticData_AnyThread(
        SubjectKey,
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
        NegotiatedRevision);
    Reply.ExpectedBoneCount = ExpectedBoneCount;
    Reply.ExpectedCurveCount = ExpectedCurveCount;
    Reply.bReady = true;
    OutgoingReplies.Enqueue(MoveTemp(Reply));
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
            Packet = FMtoUProtocol::EncodeCachePlaying(Transition.UploadId, Transition.PlayId);
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
            Packet = FMtoUProtocol::EncodeCacheCleared(Transition.UploadId, Transition.PlayId);
            break;
        case EKind::Completed:
            SetStatus(TEXT("Cached playback complete; final frame held"));
            Packet = FMtoUProtocol::EncodeCacheComplete(
                Transition.PlayId, Transition.FrameCount, Transition.ElapsedSeconds);
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
    if (GameThreadSession == 0 || !IsSessionPublishableOnGameThread(GameThreadSession))
    {
        return false;
    }
    FMtoUFrameMessage RequiredFrame;
    RequiredFrame.Curves = Frame.Curves;
    RequiredFrame.Transforms.Reserve(SourceBoneIndices.Num());
    for (int32 SourceIndex : SourceBoneIndices) { RequiredFrame.Transforms.Add(Frame.Transforms[SourceIndex]); }
    FLiveLinkFrameDataStruct FrameData;
    FString RetargetError;
    if (!FMtoUProtocol::MakeRetargetedFrameData(
            RequiredFrame,
            AcceptedCurveIndices,
            SourceBindLocalPose,
            TargetRefLocalPose,
            BoneParents,
            FrameData,
            RetargetError))
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
    if (const FLiveLinkAnimationFrameData* Animation =
            FrameData.Cast<FLiveLinkAnimationFrameData>())
    {
        for (const TWeakObjectPtr<AMtoULiveLinkActor>& Actor : ParticipatingActors)
        {
            if (Actor.IsValid())
            {
                Actor->ApplyModelMorphCurves(
                    AcceptedCurveNames, Animation->PropertyValues);
            }
        }
    }
    Client->PushSubjectFrameData_AnyThread(SubjectKey, MoveTemp(FrameData));
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
