#include "MtoULiveLinkSource.h"

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"
#include "MtoUConnectionNegotiator.h"

#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
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

FMtoUTargetDescription DescribeTarget(const USkeletalMesh& Mesh)
{
    const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
    FMtoUTargetDescription Target;
    Target.Bones.Reserve(Skeleton.GetNum());
    for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
    {
        Target.Bones.Add({Skeleton.GetBoneName(Index), Skeleton.GetParentIndex(Index)});
    }
    Target.MorphTargetNames.Reserve(Mesh.GetMorphTargets().Num());
    for (const TObjectPtr<UMorphTarget>& MorphTarget : Mesh.GetMorphTargets())
    {
        if (MorphTarget)
        {
            Target.MorphTargetNames.Add(MorphTarget->GetFName());
        }
    }
    return Target;
}

bool IsPlacedEditorActor(const AMtoULiveLinkActor& Actor)
{
    if (Actor.HasAnyFlags(RF_ClassDefaultObject) || !IsValid(&Actor))
    {
        return false;
    }
    const UWorld* World = Actor.GetWorld();
    return World
        && (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE);
}
}

FMtoULiveLinkSource::FMtoULiveLinkSource(uint16 InPort)
    : ConfiguredPort(InPort)
    , Status(TEXT("Starting listener..."))
{
    CacheSession.SetPublish([this](const FMtoUFrameMessage& Frame) -> bool
    {
        if (!Client || !SourceGuid.IsValid())
        {
            return false;
        }
        Client->PushSubjectFrameData_AnyThread(
            SubjectKey,
            FMtoUProtocol::MakeRetargetedFrameData(
                Frame,
                AcceptedCurveIndices,
                SourceBindLocalPose,
                TargetRefLocalPose,
                BoneParents));
        return true;
    });
    CacheSession.SetProgressSink(
        [this](int32 PlayId, int32 AppliedFrames)
    {
        const uint64 SessionId = GameThreadSession;
        if (SessionId == 0 || !IsCurrentSession(SessionId))
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
            // The transient cache is scoped to one negotiated streaming
            // session; a newer connection never inherits it.
            CacheSession.ResetToIdle();
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
        GameThreadSession = Init->SessionId;
        HandleInitOnGameThread(MoveTemp(Init->Message));
    }
    HandleCacheCommandsOnGameThread();

    // While a cache owns this session, ordinary live frames must neither
    // mutate the cache nor overwrite local playback; drop them and their
    // stale pending slots silently.
    if (CacheSession.GetState() == EMtoUCacheState::Idle)
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
        CacheSession.ResetToIdle();
        bPlaybackOutcomePending = false;
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
    bool bInitReceived = false;
    bool bReady = false;
    int32 WorkerExpectedBoneCount = 0;
    int32 WorkerExpectedCurveCount = 0;
    FMtoUFrameDecoder Decoder;

    auto MarkDisconnected = [&]()
    {
        if (ActiveSession != 0)
        {
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

    while (!bStopRequested.Load())
    {
        bool bDidWork = false;

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

        if (ClientSocket)
        {
            uint8 ReceiveBuffer[ReceiveBufferSize];
            int32 Read = 0;
            if (!ClientSocket->Recv(ReceiveBuffer, ReceiveBufferSize, Read))
            {
                bDidWork = true;
                MarkDisconnected();
            }
            else if (Read > 0)
            {
                bDidWork = true;
                Decoder.Append(ReceiveBuffer, Read);
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

                    auto EnqueueCacheCommand = [&](FMtoUCacheCommand Command)
                    {
                        bDidWork = true;
                        Command.SessionId = ActiveSession;
                        FScopeLock Lock(&PendingMutex);
                        if (CurrentWorkerSession == ActiveSession)
                        {
                            PendingCacheCommands.Enqueue(MoveTemp(Command));
                        }
                    };

                    if (MessageType == TEXT("cache_enter")
                        || MessageType == TEXT("cache_begin")
                        || MessageType == TEXT("cache_frame"))
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
                        }
                        else if (MessageType == TEXT("cache_frame"))
                        {
                            Command.Kind = FMtoUCacheCommand::EKind::Frame;
                            bShapeValid = FMtoUProtocol::ParseCacheFrame(
                                Payload, Command.Index, Command.Frame, CacheError, &ErrorCode);
                            // A negative index follows the production
                            // cache-session rejection path: the stable code is
                            // echoed by the session itself while it atomically
                            // discards the partial upload, and the negotiated
                            // connection stays open.
                            if (!bShapeValid && ErrorCode == TEXT("CACHE_FRAME_INDEX_INVALID"))
                            {
                                EnqueueCacheCommand(MoveTemp(Command));
                                continue;
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
            TEXT("Place an MtoU_LiveLink binding actor in an Editor or PIE world before connecting."));
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
    UMtoULiveLinkBinding* Binding = Actor->GetBinding();
    if (!Binding)
    {
        const FString Details = FString::Printf(
            TEXT("%s has no MtoU_LiveLink Binding."), *Actor->GetName());
        Actor->SetConnectionStatus(FString::Printf(TEXT("Error: %s"), *Details));
        EnqueueErrorOnGameThread(
            TEXT("INVALID_BINDING"),
            TEXT("The MtoU_LiveLink binding is invalid."),
            Details);
        return;
    }
    USkeletalMesh* DriverMesh = Binding->SkeletalMesh;
    if (!DriverMesh)
    {
        const FString Details = FString::Printf(
            TEXT("%s binding has no Skeletal Mesh."), *Actor->GetName());
        Actor->SetConnectionStatus(FString::Printf(TEXT("Error: %s"), *Details));
        EnqueueErrorOnGameThread(
            TEXT("INVALID_BINDING"),
            TEXT("The MtoU_LiveLink binding is invalid."),
            Details);
        return;
    }

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

    // Animation drives the bound Driver Skeletal Mesh. Once a Generated
    // Preview exists, the Model workflow negotiates against its projected
    // Morph library instead of the Driver's own library.
    USkeletalMesh* Mesh = bModelWorkflow ? Readiness.GeneratedPreview : DriverMesh;
    FMtoUTargetDescription Target = DescribeTarget(*Mesh);
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
                TEXT("%s\nMaya current BlendShape count: %d\nGenerated Preview Morph total: %d\nAccepted count: %d\nMaya-only count: %d (%s)\nUE-only count: %d (%s)"),
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

    if (bModelWorkflow && !bZeroMorphManifest
        && Outcome.AcceptedCurveIndices.IsEmpty())
    {
        // BS transmission with a non-empty manifest and zero accepted Preview
        // Morphs is blocking; the user must fix the pairing or explicitly
        // disable BS transmission.
        const FString Details = FString::Printf(
            TEXT("%s has no Morph Target intersection between Maya and the generated preview.\nMaya-only: %d, Unreal-only: %d."),
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
    CacheSession.SetValidationCounts(ExpectedBoneCount, ExpectedCurveCount);
    CacheSession.SetNegotiatedRevision(NegotiatedRevision);
    SourceBindLocalPose = Message.SourceBindLocalPose;
    TargetRefLocalPose.Reset(ExpectedBoneCount);
    BoneParents.Reset(ExpectedBoneCount);
    const TArray<FTransform>& RefBonePose = Mesh->GetRefSkeleton().GetRefBonePose();
    for (int32 Index = 0; Index < ExpectedBoneCount; ++Index)
    {
        TargetRefLocalPose.Add(RefBonePose[Outcome.TargetBoneIndices[Index]]);
        BoneParents.Add(Message.Bones[Index].ParentIndex);
    }
    // A bone-only session streams no Morph values at all. StaticData, the
    // ready reply, and the per-frame accepted indices must all agree on the
    // empty accepted set even though Maya still sends the full curve manifest.
    const bool bBoneOnlySession = bModelWorkflow && !Message.bBlendshapesEnabled;
    AcceptedCurveIndices = bBoneOnlySession
        ? TArray<int32>()
        : Outcome.AcceptedCurveIndices;
    for (int32 Index = 0; Index < Message.Bones.Num(); ++Index)
    {
        Message.Bones[Index].Name = Outcome.PublishBoneNames[Index];
    }
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
    while (PendingCacheCommands.Dequeue(Command))
    {
        if (Command.SessionId != GameThreadSession || GameThreadSession == 0)
        {
            continue;
        }
        if (!DispatchCacheCommandOnGameThread(Command))
        {
            continue;
        }
        const EMtoUCacheState State = CacheSession.GetState();
        switch (Command.Kind)
        {
            case FMtoUCacheCommand::EKind::Enter:
                // Cached ownership begins here: hold the recent pose, isolate
                // live frames, and release viewport realtime for the capture
                // and upload work that matters.
                SetEditorViewportRealtimeOverride(false);
                SetStatus(TEXT("Cached Playback entry; holding recent pose"));
                break;
            case FMtoUCacheCommand::EKind::Begin:
                SetStatus(FString::Printf(
                    TEXT("Receiving animation cache (%d frames)..."), CacheSession.GetExpectedFrameCount()));
                break;
            case FMtoUCacheCommand::EKind::End:
                {
                    SetStatus(TEXT("Cached animation ready"));
                    FMtoUOutgoing Reply;
                    Reply.SessionId = GameThreadSession;
                    Reply.Packet = FMtoUProtocol::EncodeCacheReady(
                        CacheSession.GetActiveUploadId(),
                        NegotiatedRevision,
                        CacheSession.GetBufferedFrameCount());
                    OutgoingReplies.Enqueue(MoveTemp(Reply));
                }
                break;
            case FMtoUCacheCommand::EKind::Play:
                SetEditorViewportRealtimeOverride(true);
                SetStatus(TEXT("Playing cached animation locally"));
                bPlaybackOutcomePending = true;
                break;
            case FMtoUCacheCommand::EKind::Stop:
                SetEditorViewportRealtimeOverride(false);
                SetStatus(TEXT("Cached playback stopped; last applied frame held"));
                if (CacheSession.GetActivePlayId() > 0)
                {
                    FMtoUOutgoing Reply;
                    Reply.SessionId = GameThreadSession;
                    Reply.Packet = FMtoUProtocol::EncodeCacheStopped(
                        CacheSession.GetActivePlayId());
                    OutgoingReplies.Enqueue(MoveTemp(Reply));
                }
                break;
            case FMtoUCacheCommand::EKind::Clear:
                SetEditorViewportRealtimeOverride(false);
                SetStatus(TEXT("Connected to Maya"));
                {
                    FMtoUOutgoing Reply;
                    Reply.SessionId = GameThreadSession;
                    Reply.Packet = FMtoUProtocol::EncodeCacheCleared(
                        CacheSession.GetLastClearedUploadId(),
                        CacheSession.GetLastClearedPlayId());
                    OutgoingReplies.Enqueue(MoveTemp(Reply));
                }
                break;
            default:
                break;
        }
    }

    // Drive local replay and report exactly one outcome per attempt:
    // completion with applied-frame evidence, or a stable performance error.
    if (CacheSession.GetState() == EMtoUCacheState::Playing)
    {
        CacheSession.Tick();
    }
    if (bPlaybackOutcomePending)
    {
        switch (CacheSession.GetState())
        {
            case EMtoUCacheState::Completed:
                SetStatus(TEXT("Cached playback complete; final frame held"));
                {
                    FMtoUOutgoing Reply;
                    Reply.SessionId = GameThreadSession;
                    Reply.Packet = FMtoUProtocol::EncodeCacheComplete(
                        CacheSession.GetActivePlayId(),
                        CacheSession.GetAppliedFrameCount(),
                        CacheSession.GetElapsedPlaybackSeconds());
                    OutgoingReplies.Enqueue(MoveTemp(Reply));
                }
                bPlaybackOutcomePending = false;
                break;
            case EMtoUCacheState::Failed:
                SetEditorViewportRealtimeOverride(false);
                SetStatus(TEXT("Cached playback missed the captured scene rate"));
                // The performance failure is recoverable and identity-scoped:
                // it ends this play attempt only, keeps the negotiated
                // connection open, and echoes the attempt it belongs to so
                // Maya never applies a stale failure to a newer replay.
                EnqueueReplyPacketOnGameThread(
                    GameThreadSession,
                    FMtoUProtocol::EncodeError(
                        TEXT("CACHED_PLAYBACK_PERFORMANCE"),
                        TEXT("Local playback fell behind the captured scene rate."),
                        CacheSession.GetErrorDetails(),
                        CacheSession.GetActiveUploadId(),
                        CacheSession.GetActivePlayId()),
                    false);
                bPlaybackOutcomePending = false;
                break;
            case EMtoUCacheState::Stopped:
            case EMtoUCacheState::Idle:
                // Manual stop, clear, or session loss supersedes the outcome.
                bPlaybackOutcomePending = false;
                break;
            default:
                break; // still playing
        }
    }
}

bool FMtoULiveLinkSource::DispatchCacheCommandOnGameThread(const FMtoUCacheCommand& Command)
{
    FString ErrorCode;
    FString Details;
    // Capture the owning identity before dispatch: a rejection resets the
    // session, so afterwards only the last-seen identities survive. Begin
    // echoes its own declared upload id because the previous ownership, if
    // any, is older than the request being rejected.
    int32 EchoUploadId = INDEX_NONE;
    int32 EchoPlayId = INDEX_NONE;
    switch (Command.Kind)
    {
        case FMtoUCacheCommand::EKind::Begin:
            EchoUploadId = Command.Begin.UploadId;
            break;
        case FMtoUCacheCommand::EKind::Frame:
        case FMtoUCacheCommand::EKind::End:
            EchoUploadId = CacheSession.GetActiveUploadId();
            break;
        case FMtoUCacheCommand::EKind::Play:
            EchoUploadId = CacheSession.GetActiveUploadId();
            EchoPlayId = Command.PlayId;
            break;
        default:
            break;
    }
    if (CacheSession.HandleCommand(Command, ErrorCode, Details))
    {
        return true;
    }
    UE_LOG(LogMtoULiveLinkSource, Warning, TEXT("Rejected cache command: %s"), *Details);
    // Upload and control errors reject the offending request but keep the
    // negotiated connection open; only structural garbage closes. The echoed
    // operation identity lets Maya discard late errors from older attempts.
    EnqueueReplyPacketOnGameThread(
        GameThreadSession,
        FMtoUProtocol::EncodeError(
            ErrorCode, Details, Details, EchoUploadId, EchoPlayId),
        false);
    return false;
}

void FMtoULiveLinkSource::EnqueueReplyPacketOnGameThread(uint64 SessionId, TArray<uint8> Packet, bool bCloseAfter)
{
    check(IsInGameThread());
    if (!IsCurrentSession(SessionId))
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
        || !Client
        || !SourceGuid.IsValid())
    {
        return;
    }
    Client->PushSubjectFrameData_AnyThread(
        SubjectKey,
        FMtoUProtocol::MakeRetargetedFrameData(
            Frame->Message,
            AcceptedCurveIndices,
            SourceBindLocalPose,
            TargetRefLocalPose,
            BoneParents));
}

void FMtoULiveLinkSource::EnqueueErrorOnGameThread(
    const FString& Code,
    const FString& Message,
    const FString& Details)
{
    check(IsInGameThread());
    if (!IsCurrentSession(GameThreadSession))
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

void FMtoULiveLinkSource::SetStatus(const FString& InStatus)
{
    FScopeLock Lock(&StatusMutex);
    Status = InStatus;
}
