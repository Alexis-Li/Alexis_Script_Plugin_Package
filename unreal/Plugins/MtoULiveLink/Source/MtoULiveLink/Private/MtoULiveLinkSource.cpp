#include "MtoULiveLinkSource.h"

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"

#include "Algo/AllOf.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "ILiveLinkClient.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "ReferenceSkeleton.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogMtoULiveLinkSource, Log, All);

namespace
{
constexpr int32 ReceiveBufferSize = 64 * 1024;
constexpr int32 IdleWaitMilliseconds = 5;

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

TArray<FMtoUBone> BonesFromMesh(const USkeletalMesh& Mesh)
{
    const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
    TArray<FMtoUBone> Bones;
    Bones.Reserve(Skeleton.GetNum());
    for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
    {
        Bones.Add({Skeleton.GetBoneName(Index), Skeleton.GetParentIndex(Index)});
    }
    return Bones;
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

void FMtoULiveLinkSource::Update()
{
    check(IsInGameThread());

    uint64 DisconnectedSession = 0;
    while (DisconnectedSessions.Dequeue(DisconnectedSession))
    {
        if (DisconnectedSession == GameThreadSession)
        {
            for (const TWeakObjectPtr<AMtoULiveLinkActor>& Actor : ParticipatingActors)
            {
                if (Actor.IsValid())
                {
                    Actor->SetConnectionStatus(TEXT("Disconnected"));
                }
            }
            GameThreadSession = 0;
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
    PublishLatestFrameOnGameThread();
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
        for (const TWeakObjectPtr<AMtoULiveLinkActor>& Actor : ParticipatingActors)
        {
            if (Actor.IsValid())
            {
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

    FSocket* ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MtoU_LiveLink listener"));
    FSocket* ClientSocket = nullptr;
    if (!ListenSocket)
    {
        SetStatus(TEXT("Failed to create the MtoU_LiveLink listener socket."));
        return 1;
    }

    TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
    bool bValidAddress = false;
    Address->SetIp(TEXT("127.0.0.1"), bValidAddress);
    Address->SetPort(ConfiguredPort);
    if (!bValidAddress
        || !ListenSocket->SetReuseAddr(true)
        || !ListenSocket->Bind(*Address)
        || !ListenSocket->Listen(1)
        || !ListenSocket->SetNonBlocking(true))
    {
        const FString Error = FString::Printf(
            TEXT("Failed to bind MtoU_LiveLink to 127.0.0.1:%u: %s"),
            ConfiguredPort,
            SocketSubsystem->GetSocketError(SocketSubsystem->GetLastErrorCode()));
        UE_LOG(LogMtoULiveLinkSource, Error, TEXT("%s"), *Error);
        SetStatus(Error);
        CloseSocket(*SocketSubsystem, ListenSocket);
        return 1;
    }

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

    auto SendErrorAndDisconnect = [&](const FString& Message)
    {
        if (ClientSocket)
        {
            SendPacket(*ClientSocket, FMtoUProtocol::EncodeError(Message), bStopRequested);
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
                        SendErrorAndDisconnect(Error);
                        break;
                    }

                    if (!bInitReceived)
                    {
                        FMtoUInitMessage Init;
                        if (!FMtoUProtocol::ParseInit(Payload, Init, Error))
                        {
                            SendErrorAndDisconnect(Error);
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
                            TEXT("A frame was received before the init message was accepted."));
                        break;
                    }

                    FMtoUFrameMessage Frame;
                    bool bStructuralError = true;
                    if (!FMtoUProtocol::ParseFrame(Payload, Frame, Error))
                    {
                        SendErrorAndDisconnect(Error);
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
                            SendErrorAndDisconnect(Error);
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
            TEXT("Place an MtoU_LiveLink binding actor in an Editor or PIE world before connecting."));
        return;
    }

    ParticipatingActors.Reset(Actors.Num());
    for (AMtoULiveLinkActor* Actor : Actors)
    {
        ParticipatingActors.Add(Actor);
        Actor->SetConnectionStatus(TEXT("Validating"));
    }

    TArray<FString> Errors;
    TArray<USkeletalMesh*> Meshes;
    Meshes.Reserve(Actors.Num());
    for (AMtoULiveLinkActor* Actor : Actors)
    {
        UMtoULiveLinkBinding* Binding = Actor->GetBinding();
        if (!Binding)
        {
            Errors.Add(FString::Printf(
                TEXT("%s has no MtoU_LiveLink Binding."), *Actor->GetName()));
            continue;
        }
        USkeletalMesh* Mesh = Binding->SkeletalMesh;
        if (!Mesh)
        {
            Errors.Add(FString::Printf(
                TEXT("%s binding has no Skeletal Mesh."), *Actor->GetName()));
            continue;
        }
        Meshes.Add(Mesh);
        const FString Difference = FMtoUProtocol::CompareSkeletons(
            Message.Bones, BonesFromMesh(*Mesh));
        if (!Difference.IsEmpty())
        {
            Errors.Add(FString::Printf(
                TEXT("%s skeleton differs:\n%s"), *Actor->GetName(), *Difference));
        }
    }

    if (!Errors.IsEmpty())
    {
        const FString Error = FString::Join(Errors, TEXT("\n"));
        for (AMtoULiveLinkActor* Actor : Actors)
        {
            Actor->SetConnectionStatus(FString::Printf(TEXT("Error: %s"), *Error));
        }
        EnqueueErrorOnGameThread(Error);
        return;
    }

    TArray<FName> AcceptedCurveNames;
    TArray<FName> MissingCurveNames;
    AcceptedCurveIndices.Reset();
    for (int32 Index = 0; Index < Message.Curves.Num(); ++Index)
    {
        const FName CurveName = Message.Curves[Index];
        const bool bPresentOnEveryMesh = Algo::AllOf(Meshes, [CurveName](const USkeletalMesh* Mesh)
        {
            return Mesh->FindMorphTarget(CurveName) != nullptr;
        });
        if (bPresentOnEveryMesh)
        {
            AcceptedCurveIndices.Add(Index);
            AcceptedCurveNames.Add(CurveName);
        }
        else
        {
            MissingCurveNames.Add(CurveName);
        }
    }

    if (!Client || !SourceGuid.IsValid())
    {
        EnqueueErrorOnGameThread(TEXT("The Unreal Live Link client is unavailable."));
        return;
    }
    if (!IsCurrentSession(SessionId))
    {
        return;
    }

    ExpectedBoneCount = Message.Bones.Num();
    ExpectedCurveCount = Message.Curves.Num();
    for (AMtoULiveLinkActor* Actor : Actors)
    {
        Actor->SetConnectionStatus(TEXT("Connected"));
    }

    Client->PushSubjectStaticData_AnyThread(
        SubjectKey,
        ULiveLinkAnimationRole::StaticClass(),
        FMtoUProtocol::MakeStaticData(Message, AcceptedCurveNames));

    FMtoUOutgoing Reply;
    Reply.SessionId = SessionId;
    Reply.Packet = FMtoUProtocol::EncodeReady(MissingCurveNames);
    Reply.ExpectedBoneCount = ExpectedBoneCount;
    Reply.ExpectedCurveCount = ExpectedCurveCount;
    Reply.bReady = true;
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
        FMtoUProtocol::MakeFrameData(Frame->Message, AcceptedCurveIndices));
}

void FMtoULiveLinkSource::EnqueueErrorOnGameThread(const FString& Message)
{
    check(IsInGameThread());
    if (!IsCurrentSession(GameThreadSession))
    {
        return;
    }
    SetStatus(Message);
    FMtoUOutgoing Reply;
    Reply.SessionId = GameThreadSession;
    Reply.Packet = FMtoUProtocol::EncodeError(Message);
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
