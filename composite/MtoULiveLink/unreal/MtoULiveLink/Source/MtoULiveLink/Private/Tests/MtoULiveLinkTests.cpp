#if WITH_DEV_AUTOMATION_TESTS

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"
#include "MtoUCacheCommandQueue.h"
#include "MtoUConnectionNegotiator.h"
#include "MtoULiveLinkProtocol.h"
#include "MtoULiveLinkSource.h"
#include "MtoULiveLinkTestAnimInstance.h"

#include "Animation/MorphTarget.h"
#include "Async/Async.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Features/IModularFeatures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeCounter.h"
#include "ILiveLinkClient.h"
#include "IPAddress.h"
#include "LiveLinkInstance.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Tests/EnsureScope.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectIterator.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

namespace
{
FThreadSafeCounter GMtoUConflictingPostProcessEvaluations;
}

void UMtoULiveLinkConflictingPostProcess::NativeUpdateAnimation(float DeltaSeconds)
{
    (void)DeltaSeconds;
    GMtoUConflictingPostProcessEvaluations.Increment();
}

#include "MtoUConformanceCorpus.inl"

/**
 * Development-only test seam. It drives the Binding actor's real private
 * refresh transition path so Runtime tests can establish readiness without
 * exposing production mutation methods (Issue #25).
 */
class FMtoUPreviewReadinessTestAccess
{
public:
    static bool Begin(AMtoULiveLinkActor& Actor)
    {
        return Actor.BeginPreviewBuild();
    }
    static void SetStage(AMtoULiveLinkActor& Actor, EMtoUPreviewBuildStage Stage)
    {
        Actor.SetPreviewBuildStage(Stage);
    }
    static bool Commit(AMtoULiveLinkActor& Actor, USkeletalMesh* Mesh, bool bWarning,
        const FString& Diagnostics, const FString& Summary = FString(),
        const TArray<int32>& DriverGarmentMaterialSlots = {})
    {
        return Actor.CompletePreviewBuild(
            Mesh, bWarning, Diagnostics, Summary, DriverGarmentMaterialSlots);
    }
    static bool Fail(AMtoULiveLinkActor& Actor, EMtoUPreviewBuildStage Stage,
        const FString& Diagnostics)
    {
        return Actor.FailPreviewBuild(Stage, Diagnostics);
    }
    static void Release(AMtoULiveLinkActor& Actor)
    {
        Actor.ReleaseGeneratedPreview();
    }
};

// Deliver delayed A envelopes through the real queues after B negotiates.
// Operation IDs collide deliberately; assertions inspect wire replies and poses.
class FMtoUSessionIsolationTestAccess
{
public:
    static void ObserveApplied(FMtoULiveLinkSource& Source,
        TFunction<void(uint64, const FMtoUFrameMessage&)> Observer)
    {
        Source.CacheSession.SetPublish([&Source, Observer = MoveTemp(Observer)](
            const FMtoUFrameMessage& Frame)
        {
            const bool bApplied = Source.PublishFrameOnGameThread(Frame);
            if (bApplied)
            {
                Observer(Source.GameThreadSession, Frame);
            }
            return bApplied;
        });
    }

    static uint64 Session(const FMtoULiveLinkSource& Source)
    {
        return Source.GameThreadSession;
    }

    static void DeliverDelayed(FMtoULiveLinkSource& Source, uint64 OldSession)
    {
        FMtoUCacheCommand Frame;
        Frame.SessionId = OldSession;
        Frame.UploadId = 1;
        Frame.Kind = FMtoUCacheCommand::EKind::Frame;
        Frame.Index = 1;
        Frame.Frame.Transforms.Add({FVector(777), FQuat::Identity, FVector::OneVector});
        Frame.Frame.Transforms.Add({FVector::ZeroVector, FQuat::Identity, FVector::OneVector});
        Frame.Frame.Curves.Add(0);
        Frame.EncodedBytes = 200;
        Source.CacheCommands.Produce(MoveTemp(Frame), [] { return false; });
        for (const auto Kind : {FMtoUCacheCommand::EKind::End,
                FMtoUCacheCommand::EKind::Play, FMtoUCacheCommand::EKind::Clear})
        {
            FMtoUCacheCommand Command;
            Command.SessionId = OldSession;
            Command.UploadId = 1;
            Command.PlayId = 1;
            Command.Kind = Kind;
            Source.CacheCommands.Produce(MoveTemp(Command), [] { return false; });
        }
        TArray<FMtoUOutgoing> Delayed;
        FMtoUOutgoing Ready;
        Ready.Packet = FMtoUProtocol::EncodeCacheReady(1, 9, 2);
        Delayed.Add(MoveTemp(Ready));
        FMtoUOutgoing Progress;
        Progress.Packet = FMtoUProtocol::EncodeCacheProgress(1, 777);
        Delayed.Add(MoveTemp(Progress));
        FMtoUOutgoing Complete;
        Complete.Packet = FMtoUProtocol::EncodeCacheComplete(1, 777, 0.5);
        Delayed.Add(MoveTemp(Complete));
        FMtoUOutgoing Error;
        Error.Packet = FMtoUProtocol::EncodeError(TEXT("CACHE_NOT_READY"), TEXT("delayed session A"));
        Error.bCloseAfter = true;
        Delayed.Add(MoveTemp(Error));
        for (FMtoUOutgoing& Reply : Delayed)
        {
            Reply.SessionId = OldSession;
            Source.OutgoingReplies.Enqueue(MoveTemp(Reply));
        }
    }
};

namespace
{
FString FromUtf8(const TArray<uint8>& Bytes);

TArray<uint8> Utf8(const FString& Text)
{
    FTCHARToUTF8 Converted(*Text);
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    return Bytes;
}

bool JsonObjectFromBytes(const TArray<uint8>& Bytes, TSharedPtr<FJsonObject>& OutObject)
{
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FromUtf8(Bytes));
    return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

TArray<uint8> JsonBytes(const TSharedPtr<FJsonValue>& Value)
{
    FString Text;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
    FJsonSerializer::Serialize(Value, TEXT(""), Writer);
    return Utf8(Text);
}

TArray<uint8> HexBytes(const FString& Text)
{
    TArray<uint8> Bytes;
    for (int32 Index = 0; Index + 1 < Text.Len(); Index += 2)
    {
        Bytes.Add(static_cast<uint8>(FCString::Strtoi(*Text.Mid(Index, 2), nullptr, 16)));
    }
    return Bytes;
}

bool ContainsKeywords(const FString& Diagnostic, const TArray<TSharedPtr<FJsonValue>>& Keywords)
{
    for (const TSharedPtr<FJsonValue>& Keyword : Keywords)
    {
        if (!Diagnostic.Contains(Keyword->AsString(), ESearchCase::IgnoreCase))
        {
            return false;
        }
    }
    return true;
}

FString FromUtf8(const TArray<uint8>& Bytes)
{
    FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
}

TArray<uint8> Prefix(uint64 Length)
{
    TArray<uint8> Bytes;
    Bytes.SetNumUninitialized(8);
    for (int32 Index = 0; Index < 8; ++Index)
    {
        Bytes[Index] = static_cast<uint8>(Length >> ((7 - Index) * 8));
    }
    return Bytes;
}

TArray<uint8> Packet(const FString& Text)
{
    TArray<uint8> Payload = Utf8(Text);
    TArray<uint8> Bytes = Prefix(Payload.Num());
    Bytes.Append(Payload);
    return Bytes;
}

FString TransformJson(const FTransform& Transform)
{
    const FVector Translation = Transform.GetTranslation();
    const FQuat Rotation = Transform.GetRotation();
    const FVector Scale = Transform.GetScale3D();
    return FString::Printf(
        TEXT("[%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g]"),
        Translation.X,
        Translation.Y,
        Translation.Z,
        Rotation.X,
        Rotation.Y,
        Rotation.Z,
        Rotation.W,
        Scale.X,
        Scale.Y,
        Scale.Z);
}

FString ReferenceSkeletonBonesJson(const FReferenceSkeleton& Skeleton)
{
    FString Bones;
    for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
    {
        if (Index > 0)
        {
            Bones += TEXT(",");
        }
        Bones += FString::Printf(
            TEXT("[\"%s\",%d,%s]"),
            *Skeleton.GetBoneName(Index).ToString(),
            Skeleton.GetParentIndex(Index),
            *TransformJson(Skeleton.GetRefBonePose()[Index]));
    }
    return Bones;
}

bool AddUniformMorph(USkeletalMesh& Mesh, FName Name, const FVector3f& PositionDelta)
{
    UMorphTarget* Morph = NewObject<UMorphTarget>(&Mesh, Name, RF_Transient);
    FMorphTargetLODModel& LODModel = Morph->GetMorphLODModels().AddDefaulted_GetRef();
    FMorphTargetDelta& Delta = LODModel.Vertices.AddDefaulted_GetRef();
    Delta.SourceIdx = 0;
    Delta.PositionDelta = PositionDelta;
    LODModel.NumVertices = 1;
    LODModel.NumBaseMeshVerts = 1;
    LODModel.SectionIndices.Add(0);
    const bool bRegistered = Mesh.RegisterMorphTarget(Morph, false);
    Mesh.InitMorphTargets();
    return bRegistered;
}

TSharedRef<FInternetAddr> LoopbackAddress(ISocketSubsystem& SocketSubsystem, uint16 Port)
{
    TSharedRef<FInternetAddr> Address = SocketSubsystem.CreateInternetAddr();
    bool bValid = false;
    Address->SetIp(TEXT("127.0.0.1"), bValid);
    check(bValid);
    Address->SetPort(Port);
    return Address;
}

void DestroySocket(ISocketSubsystem& SocketSubsystem, FSocket*& Socket)
{
    if (Socket)
    {
        Socket->Close();
        SocketSubsystem.DestroySocket(Socket);
        Socket = nullptr;
    }
}

bool PollUntil(TFunctionRef<bool()> Predicate)
{
    const double Deadline = FPlatformTime::Seconds() + 1.0;
    do
    {
        if (Predicate())
        {
            return true;
        }
        FPlatformProcess::Sleep(0.005f);
    }
    while (FPlatformTime::Seconds() < Deadline);
    return Predicate();
}

bool WaitForStatus(const TSharedRef<FMtoULiveLinkSource>& Source, const FString& Text)
{
    return PollUntil([&]() { return Source->GetSourceStatus().ToString().Contains(Text); });
}

int32 ListeningPort(const TSharedRef<FMtoULiveLinkSource>& Source)
{
    const FString Status = Source->GetSourceStatus().ToString();
    int32 Separator = INDEX_NONE;
    return Status.FindLastChar(TEXT(':'), Separator)
        ? FCString::Atoi(*Status.Mid(Separator + 1))
        : 0;
}

FSocket* BindLoopback(ISocketSubsystem& SocketSubsystem, uint16 Port, bool bListen)
{
    FSocket* Socket = SocketSubsystem.CreateSocket(NAME_Stream, TEXT("MtoULiveLink automation socket"));
    if (!Socket || !Socket->Bind(*LoopbackAddress(SocketSubsystem, Port))
        || (bListen && !Socket->Listen(1)))
    {
        DestroySocket(SocketSubsystem, Socket);
    }
    return Socket;
}

FSocket* ConnectLoopback(ISocketSubsystem& SocketSubsystem, uint16 Port)
{
    FSocket* Socket = SocketSubsystem.CreateSocket(NAME_Stream, TEXT("MtoULiveLink automation client"));
    if (!Socket || !Socket->Connect(*LoopbackAddress(SocketSubsystem, Port)))
    {
        DestroySocket(SocketSubsystem, Socket);
        return nullptr;
    }
    Socket->SetNonBlocking(true);
    return Socket;
}

bool SendBytes(FSocket& Socket, const uint8* Data, int32 Num)
{
    int32 Offset = 0;
    const double Deadline = FPlatformTime::Seconds() + 1.0;
    while (Offset < Num && FPlatformTime::Seconds() < Deadline)
    {
        int32 Sent = 0;
        if (Socket.Send(Data + Offset, Num - Offset, Sent) && Sent > 0)
        {
            Offset += Sent;
        }
        else
        {
            Socket.Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(5));
        }
    }
    return Offset == Num;
}

bool ReceivePacket(
    FSocket& Socket,
    TArray<uint8>& OutPayload,
    TFunctionRef<void()> Pump = []() {})
{
    FMtoUFrameDecoder Decoder;
    FString Error;
    return PollUntil([&]()
    {
        Pump();
        uint8 Buffer[65536];
        int32 Read = 0;
        if (Socket.Recv(Buffer, UE_ARRAY_COUNT(Buffer), Read) && Read > 0)
        {
            Decoder.Append(Buffer, Read);
        }
        return Decoder.Pop(OutPayload, Error) == EMtoUDecodeResult::Message;
    });
}

bool WaitForClose(FSocket& Socket)
{
    return PollUntil([&]()
    {
        uint8 Buffer[1024];
        int32 Read = 0;
        return !Socket.Recv(Buffer, UE_ARRAY_COUNT(Buffer), Read);
    });
}

AMtoULiveLinkActor* AddBoundActor(UWorld& World)
{
    USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(GetTransientPackage());
    Binding->SkeletalMesh = Mesh;
    AMtoULiveLinkActor* Actor = World.SpawnActor<AMtoULiveLinkActor>();
    if (Actor)
    {
        Actor->SetBinding(Binding);
    }
    return Actor;
}

FMtoUFrameMessage FrameFromPose(const TArray<FTransform>& Pose)
{
    FMtoUFrameMessage Frame;
    for (const FTransform& Transform : Pose)
    {
        Frame.Transforms.Add({
            Transform.GetTranslation(), Transform.GetRotation(), Transform.GetScale3D()});
    }
    return Frame;
}

TArray<FTransform> RetargetPose(
    const TArray<FTransform>& CurrentPose,
    const TArray<FTransform>& SourceBindPose,
    const TArray<FTransform>& TargetRefPose,
    const TArray<int32>& BoneParents)
{
    FLiveLinkFrameDataStruct FrameData;
    FString Error;
    const bool bBuilt = FMtoUProtocol::MakeRetargetedFrameData(
        FrameFromPose(CurrentPose), {}, SourceBindPose, TargetRefPose, BoneParents,
        FrameData, Error);
    ensureMsgf(bBuilt, TEXT("shared retarget test input rejected: %s"), *Error);
    const FLiveLinkAnimationFrameData* Animation = FrameData.Cast<FLiveLinkAnimationFrameData>();
    return Animation ? Animation->Transforms : TArray<FTransform>();
}
}

namespace
{
FMtoUCacheCommand MakeCacheBeginCommand(int32 Revision, int32 FrameCount,
    double Fps = 30.0, int32 UploadId = 1)
{
    FMtoUCacheCommand Command;
    Command.Kind = FMtoUCacheCommand::EKind::Begin;
    Command.Begin.UploadId = UploadId;
    Command.Begin.Revision = Revision;
    Command.Begin.Fps = Fps;
    Command.Begin.StartFrame = 1001;
    Command.Begin.EndFrame = 1001 + FrameCount - 1;
    Command.Begin.FrameCount = FrameCount;
    Command.Begin.PayloadSize = 64ll * FrameCount;
    return Command;
}

FMtoUCacheCommand MakeCachedFrameCommand(int32 Index, float Value,
    int64 EncodedBytes = 64, int32 CurveCount = 0)
{
    FMtoUCacheCommand Command;
    Command.Kind = FMtoUCacheCommand::EKind::Frame;
    Command.Index = Index;
    Command.EncodedBytes = EncodedBytes;
    FMtoUTransform Transform;
    Transform.Translation = FVector(Value, 0.0, 0.0);
    Command.Frame.Transforms.Add(Transform);
    for (int32 Curve = 0; Curve < CurveCount; ++Curve)
    {
        Command.Frame.Curves.Add(0.5);
    }
    return Command;
}

FMtoUCacheCommand MakeSimpleCacheCommand(FMtoUCacheCommand::EKind Kind)
{
    FMtoUCacheCommand Command;
    Command.Kind = Kind;
    return Command;
}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUConformanceCorpusTest,
    "MtoULiveLink.Protocol.ConformanceCorpus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUConformanceCorpusTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    TArray<uint8> CorpusBytes;
    CorpusBytes.Append(GMtoUConformanceCorpus, UE_ARRAY_COUNT(GMtoUConformanceCorpus));
    TSharedPtr<FJsonObject> Corpus;
    if (!JsonObjectFromBytes(CorpusBytes, Corpus))
    {
        AddError(TEXT("Generated conformance corpus must be valid JSON."));
        return false;
    }

    // The corpus `limits` block is the single source of truth for the frozen
    // wire ceilings both hosts hardcode independently. Pin the Unreal
    // constants against it so any one-sided edit fails here and in Maya.
    const TSharedPtr<FJsonObject> Limits = Corpus->GetObjectField(TEXT("limits"));
    if (Limits.IsValid())
    {
        TestEqual(TEXT("framing ceiling matches the shared corpus limit"),
            FMtoUProtocol::MaxMessageBytes,
            static_cast<int64>(Limits->GetNumberField(TEXT("max_message_bytes"))));
        TestEqual(TEXT("cache payload ceiling matches the shared corpus limit"),
            FMtoUProtocol::MaxCachePayloadBytes,
            static_cast<int64>(Limits->GetNumberField(TEXT("max_cache_payload_bytes"))));
        TestEqual(TEXT("cache frame ceiling matches the shared corpus limit"),
            FMtoUProtocol::MaxCacheFrameCount,
            static_cast<int32>(Limits->GetNumberField(TEXT("max_cache_frame_count"))));
    }
    else
    {
        AddError(TEXT("Conformance corpus must define a limits block."));
    }

    int32 ApplicableCount = 0;
    const TArray<TSharedPtr<FJsonValue>>& Cases = Corpus->GetArrayField(TEXT("cases"));
    for (const TSharedPtr<FJsonValue>& CaseValue : Cases)
    {
        const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
        bool bApplies = false;
        for (const TSharedPtr<FJsonValue>& Host : Case->GetArrayField(TEXT("applies_to")))
        {
            bApplies |= Host->AsString() == TEXT("unreal");
        }
        if (!bApplies)
        {
            continue;
        }
        ++ApplicableCount;
        const FString Id = Case->GetStringField(TEXT("id"));
        const FString Operation = Case->GetStringField(TEXT("operation"));
        const TSharedPtr<FJsonObject> Expected = Case->GetObjectField(TEXT("expected"));
        const bool bExpectedAccepted = Expected->GetBoolField(TEXT("accepted"));
        const bool bExpectedClose = Expected->GetBoolField(TEXT("close"));
        const TArray<TSharedPtr<FJsonValue>>* Keywords = nullptr;
        Expected->TryGetArrayField(TEXT("keywords"), Keywords);

        if (Operation == TEXT("framing"))
        {
            FMtoUFrameDecoder Decoder;
            TArray<uint8> Payload;
            FString Error;
            if (Case->HasField(TEXT("raw_hex")))
            {
                const TArray<uint8> Raw = HexBytes(Case->GetStringField(TEXT("raw_hex")));
                Decoder.Append(Raw.GetData(), Raw.Num());
                const EMtoUDecodeResult Result = Decoder.Pop(Payload, Error);
                TestTrue(*FString::Printf(TEXT("%s acceptance"), *Id),
                    (Result != EMtoUDecodeResult::Error) == bExpectedAccepted);
                TestEqual(*FString::Printf(TEXT("%s close classification"), *Id),
                    Result == EMtoUDecodeResult::Error, bExpectedClose);
                if (Keywords)
                {
                    TestTrue(*FString::Printf(TEXT("%s diagnostic keywords"), *Id),
                        ContainsKeywords(Error, *Keywords));
                }
                continue;
            }
            TArray<uint8> Stream;
            for (const TSharedPtr<FJsonValue>& PayloadValue : Case->GetArrayField(TEXT("payloads")))
            {
                const TArray<uint8> Bytes = JsonBytes(PayloadValue);
                Stream.Append(Prefix(Bytes.Num()));
                Stream.Append(Bytes);
            }
            int32 Offset = 0;
            if (Case->HasField(TEXT("chunk_sizes")))
            {
                for (const TSharedPtr<FJsonValue>& Size : Case->GetArrayField(TEXT("chunk_sizes")))
                {
                    const int32 Count = FMath::Min(static_cast<int32>(Size->AsNumber()), Stream.Num() - Offset);
                    Decoder.Append(Stream.GetData() + Offset, Count);
                    Offset += Count;
                }
            }
            if (Offset < Stream.Num())
            {
                Decoder.Append(Stream.GetData() + Offset, Stream.Num() - Offset);
            }
            int32 MessageCount = 0;
            while (Decoder.Pop(Payload, Error) == EMtoUDecodeResult::Message)
            {
                ++MessageCount;
            }
            TestEqual(*FString::Printf(TEXT("%s decoded message count"), *Id),
                MessageCount, static_cast<int32>(Expected->GetNumberField(TEXT("message_count"))));
            continue;
        }

        if (Operation == TEXT("init"))
        {
            const TArray<uint8> Bytes = Case->HasField(TEXT("raw_hex"))
                ? HexBytes(Case->GetStringField(TEXT("raw_hex")))
                : JsonBytes(Case->TryGetField(TEXT("payload")));
            FMtoUInitMessage Message;
            FString Error;
            FString ErrorCode;
            const bool bAccepted = FMtoUProtocol::ParseInit(Bytes, Message, Error, &ErrorCode);
            TestEqual(*FString::Printf(TEXT("%s acceptance"), *Id), bAccepted, bExpectedAccepted);
            TestEqual(*FString::Printf(TEXT("%s close classification"), *Id), !bAccepted, bExpectedClose);
            if (!bAccepted)
            {
                TestEqual(*FString::Printf(TEXT("%s stable error code"), *Id),
                    ErrorCode, Expected->GetStringField(TEXT("error_code")));
                if (Keywords)
                {
                    TestTrue(*FString::Printf(TEXT("%s diagnostic keywords"), *Id),
                        ContainsKeywords(Error, *Keywords));
                }
            }
            continue;
        }

        if (Operation == TEXT("frame"))
        {
            const TArray<uint8> Bytes = Case->HasField(TEXT("raw_utf8"))
                ? Utf8(Case->GetStringField(TEXT("raw_utf8")))
                : JsonBytes(Case->TryGetField(TEXT("payload")));
            FMtoUFrameMessage Message;
            FString Error;
            bool bStructural = true;
            bool bAccepted = FMtoUProtocol::ParseFrame(Bytes, Message, Error);
            if (bAccepted && Case->HasField(TEXT("expected_counts")))
            {
                const TSharedPtr<FJsonObject> Counts = Case->GetObjectField(TEXT("expected_counts"));
                bAccepted = FMtoUProtocol::ValidateFrame(
                    Message,
                    static_cast<int32>(Counts->GetNumberField(TEXT("transforms"))),
                    static_cast<int32>(Counts->GetNumberField(TEXT("curves"))),
                    Error,
                    bStructural);
            }
            TestEqual(*FString::Printf(TEXT("%s acceptance"), *Id), bAccepted, bExpectedAccepted);
            TestEqual(*FString::Printf(TEXT("%s close classification"), *Id),
                !bAccepted && bStructural, bExpectedClose);
            if (!bAccepted && Keywords)
            {
                TestTrue(*FString::Printf(TEXT("%s diagnostic keywords"), *Id),
                    ContainsKeywords(Error, *Keywords));
            }
            continue;
        }

        if (Operation == TEXT("ready") || Operation == TEXT("error")
            || Operation == TEXT("cache_ready") || Operation == TEXT("cache_progress")
            || Operation == TEXT("cache_complete") || Operation == TEXT("cache_stopped")
            || Operation == TEXT("cache_cleared"))
        {
            const TSharedPtr<FJsonObject> Source = Case->GetObjectField(TEXT("payload"));
            TArray<uint8> PacketBytes;
            if (Operation == TEXT("ready"))
            {
                auto Names = [&](const TCHAR* Field)
                {
                    TArray<FName> Result;
                    for (const TSharedPtr<FJsonValue>& Value : Source->GetArrayField(Field))
                    {
                        Result.Add(FName(*Value->AsString()));
                    }
                    return Result;
                };
                TArray<FString> Remaps;
                for (const TSharedPtr<FJsonValue>& Value : Source->GetArrayField(TEXT("bone_name_remaps")))
                {
                    Remaps.Add(Value->AsString());
                }
                PacketBytes = FMtoUProtocol::EncodeReady(
                    Names(TEXT("missing_in_unreal")),
                    Names(TEXT("missing_in_maya")),
                    Remaps,
                    Source->GetStringField(TEXT("workflow")),
                    static_cast<int32>(Source->GetNumberField(TEXT("target_morph_count"))),
                    static_cast<int32>(Source->GetNumberField(TEXT("accepted_morph_count"))),
                    static_cast<int32>(Source->GetNumberField(TEXT("revision"))));
            }
            else if (Operation == TEXT("cache_ready"))
            {
                PacketBytes = FMtoUProtocol::EncodeCacheReady(
                    static_cast<int32>(Source->GetNumberField(TEXT("upload_id"))),
                    static_cast<int32>(Source->GetNumberField(TEXT("revision"))),
                    static_cast<int32>(Source->GetNumberField(TEXT("frame_count"))));
            }
            else if (Operation == TEXT("cache_progress"))
            {
                PacketBytes = FMtoUProtocol::EncodeCacheProgress(
                    static_cast<int32>(Source->GetNumberField(TEXT("play_id"))),
                    static_cast<int32>(Source->GetNumberField(TEXT("applied"))));
            }
            else if (Operation == TEXT("cache_complete"))
            {
                PacketBytes = FMtoUProtocol::EncodeCacheComplete(
                    static_cast<int32>(Source->GetNumberField(TEXT("play_id"))),
                    static_cast<int32>(Source->GetNumberField(TEXT("applied_frame_count"))),
                    Source->GetNumberField(TEXT("elapsed_seconds")));
            }
            else if (Operation == TEXT("cache_stopped"))
            {
                PacketBytes = FMtoUProtocol::EncodeCacheStopped(
                    static_cast<int32>(Source->GetNumberField(TEXT("play_id"))));
            }
            else if (Operation == TEXT("cache_cleared"))
            {
                PacketBytes = FMtoUProtocol::EncodeCacheCleared(
                    static_cast<int32>(Source->GetNumberField(TEXT("upload_id"))),
                    static_cast<int32>(Source->GetNumberField(TEXT("play_id"))));
            }
            else
            {
                // Cache-operation errors may echo the owning identity; the
                // encoder forwards it so the corpus pins the scoped shape.
                const int32 EchoUploadId = Source->HasField(TEXT("upload_id"))
                    ? static_cast<int32>(Source->GetNumberField(TEXT("upload_id")))
                    : INDEX_NONE;
                const int32 EchoPlayId = Source->HasField(TEXT("play_id"))
                    ? static_cast<int32>(Source->GetNumberField(TEXT("play_id")))
                    : INDEX_NONE;
                PacketBytes = FMtoUProtocol::EncodeError(
                    Source->GetStringField(TEXT("code")),
                    Source->GetStringField(TEXT("message")),
                    Source->GetStringField(TEXT("details")),
                    EchoUploadId,
                    EchoPlayId);
            }
            FMtoUFrameDecoder Decoder;
            TArray<uint8> Reply;
            FString Error;
            Decoder.Append(PacketBytes.GetData(), PacketBytes.Num());
            TestTrue(*FString::Printf(TEXT("%s encoder produces one framed reply"), *Id),
                Decoder.Pop(Reply, Error) == EMtoUDecodeResult::Message);
            TSharedPtr<FJsonObject> Encoded;
            TestTrue(*FString::Printf(TEXT("%s encoder produces JSON"), *Id),
                JsonObjectFromBytes(Reply, Encoded));
            if (Encoded.IsValid())
            {
                TestEqual(*FString::Printf(TEXT("%s reply type"), *Id),
                    Encoded->GetStringField(TEXT("type")), Operation);
                TArray<const TCHAR*> RequiredFields;
                if (Operation == TEXT("ready"))
                {
                    RequiredFields = {
                        TEXT("revision"),
                        TEXT("missing_in_unreal"), TEXT("missing_in_maya"),
                        TEXT("bone_name_remaps"), TEXT("workflow"),
                        TEXT("target_morph_count"), TEXT("accepted_morph_count")};
                }
                else if (Operation == TEXT("error"))
                {
                    RequiredFields = {TEXT("code"), TEXT("message"), TEXT("details")};
                }
                else if (Operation == TEXT("cache_cleared"))
                {
                    RequiredFields = {TEXT("upload_id"), TEXT("play_id")};
                }
                else if (Operation == TEXT("cache_progress"))
                {
                    RequiredFields = {TEXT("play_id"), TEXT("applied")};
                }
                else if (Operation == TEXT("cache_complete"))
                {
                    RequiredFields = {TEXT("play_id"), TEXT("applied_frame_count"),
                        TEXT("elapsed_seconds")};
                }
                else if (Operation == TEXT("cache_stopped"))
                {
                    RequiredFields = {TEXT("play_id")};
                }
                else
                {
                    RequiredFields = {TEXT("upload_id"), TEXT("revision"), TEXT("frame_count")};
                }
                for (const TCHAR* Field : RequiredFields)
                {
                    TestTrue(*FString::Printf(TEXT("%s required field %s"), *Id, Field),
                        Encoded->HasField(Field));
                }
                if (Operation == TEXT("ready")
                    || Operation == TEXT("cache_ready"))
                {
                    TestEqual(*FString::Printf(TEXT("%s echoes the negotiated revision"), *Id),
                        static_cast<int32>(Encoded->GetNumberField(TEXT("revision"))),
                        static_cast<int32>(Source->GetNumberField(TEXT("revision"))));
                }
                if (Operation == TEXT("error"))
                {
                    for (const TCHAR* IdentityField : {TEXT("upload_id"), TEXT("play_id")})
                    {
                        if (Source->HasField(IdentityField))
                        {
                            TestEqual(
                                *FString::Printf(TEXT("%s echoes %s"), *Id, IdentityField),
                                static_cast<int32>(Encoded->GetNumberField(IdentityField)),
                                static_cast<int32>(Source->GetNumberField(IdentityField)));
                        }
                    }
                }
            }
            continue;
        }

        if (Operation.StartsWith(TEXT("cache_")))
        {
            const TArray<uint8> Bytes = Case->HasField(TEXT("raw_utf8"))
                ? Utf8(Case->GetStringField(TEXT("raw_utf8")))
                : JsonBytes(Case->TryGetField(TEXT("payload")));
            FString Error;
            FString ErrorCode = TEXT("INVALID_MESSAGE");
            bool bAccepted = false;

            FMtoUCacheCommand Command;
            bool bCommandKnown = true;
            if (Operation == TEXT("cache_enter"))
            {
                Command.Kind = FMtoUCacheCommand::EKind::Enter;
                bAccepted = FMtoUProtocol::ParseCacheEnter(Bytes, Error);
            }
            else if (Operation == TEXT("cache_begin"))
            {
                Command.Kind = FMtoUCacheCommand::EKind::Begin;
                bAccepted = FMtoUProtocol::ParseCacheBegin(Bytes, Command.Begin, Error, ErrorCode);
            }
            else if (Operation == TEXT("cache_frame"))
            {
                Command.Kind = FMtoUCacheCommand::EKind::Frame;
                bAccepted = FMtoUProtocol::ParseCacheFrame(
                    Bytes,
                    INDEX_NONE,
                    INDEX_NONE,
                    Command.Index,
                    Command.Frame,
                    Error,
                    &ErrorCode);
            }
            else if (Operation == TEXT("cache_end"))
            {
                Command.Kind = FMtoUCacheCommand::EKind::End;
                bAccepted = FMtoUProtocol::ParseCacheEnd(Bytes, Error);
            }
            else if (Operation == TEXT("cache_play"))
            {
                Command.Kind = FMtoUCacheCommand::EKind::Play;
                bAccepted = FMtoUProtocol::ParseCachePlay(Bytes, Command.PlayId, Error);
            }
            else if (Operation == TEXT("cache_stop"))
            {
                Command.Kind = FMtoUCacheCommand::EKind::Stop;
                bAccepted = FMtoUProtocol::ParseCacheStop(Bytes, Error);
            }
            else if (Operation == TEXT("cache_clear"))
            {
                Command.Kind = FMtoUCacheCommand::EKind::Clear;
                bAccepted = FMtoUProtocol::ParseCacheClear(Bytes, Error);
            }
            else
            {
                bCommandKnown = false;
            }

            // State-sensitive cases run against a seeded transient cache
            // session so the corpus pins Unreal's session semantics too.
            const FString SessionMode = Case->HasField(TEXT("session"))
                ? Case->GetStringField(TEXT("session"))
                : FString();
            if (bCommandKnown && !SessionMode.IsEmpty())
            {
                double FakeNow = 100.0;
                FMtoUCacheSession Session;
                Session.SetClock([&FakeNow]() { return FakeNow; });
                Session.BeginSession({1, 0, Case->HasField(TEXT("negotiated_revision"))
                    ? static_cast<int32>(Case->GetNumberField(TEXT("negotiated_revision")))
                    : 7});
                if (SessionMode == TEXT("uploaded"))
                {
                    FMtoUCacheCommand SeedBegin;
                    SeedBegin.Kind = FMtoUCacheCommand::EKind::Begin;
                    SeedBegin.Begin.UploadId = 1;
                    SeedBegin.Begin.Revision = SessionMode == TEXT("uploaded")
                        ? (Case->HasField(TEXT("negotiated_revision"))
                            ? static_cast<int32>(Case->GetNumberField(TEXT("negotiated_revision")))
                            : 7)
                        : 7;
                    SeedBegin.Begin.Fps = 30.0;
                    SeedBegin.Begin.StartFrame = 0;
                    SeedBegin.Begin.EndFrame = 0;
                    SeedBegin.Begin.FrameCount = 1;
                    SeedBegin.Begin.PayloadSize = 96;
                    FMtoUCacheCommand SeedFrame =
                        MakeCachedFrameCommand(0, 1.0f, 64);
                    FMtoUCacheCommand SeedEnd;
                    SeedEnd.Kind = FMtoUCacheCommand::EKind::End;
                    TestTrue(*FString::Printf(TEXT("%s seeds an uploaded session"), *Id),
                        Session.HandleCommand(SeedBegin).bAccepted
                        && Session.HandleCommand(SeedFrame).bAccepted
                        && Session.HandleCommand(SeedEnd).bAccepted);
                }
                ErrorCode = TEXT("");
                const FMtoUCacheTransition Result = Session.HandleCommand(Command);
                bAccepted = Result.bAccepted;
                ErrorCode = Result.ErrorCode;
                Error = Result.Details;
                if (!bAccepted && ErrorCode.IsEmpty())
                {
                    ErrorCode = TEXT("INVALID_MESSAGE");
                }
            }
            else if (Operation == TEXT("cache_frame") && bAccepted
                && Case->HasField(TEXT("expected_counts")))
            {
                // Cached frames never close the connection over value
                // problems; they reject the whole upload with a stable code.
                const TSharedPtr<FJsonObject> Counts = Case->GetObjectField(TEXT("expected_counts"));
                bool bStructural = false;
                FString ValidationError;
                if (!FMtoUProtocol::ValidateFrame(
                        Command.Frame,
                        static_cast<int32>(Counts->GetNumberField(TEXT("transforms"))),
                        static_cast<int32>(Counts->GetNumberField(TEXT("curves"))),
                        ValidationError,
                        bStructural))
                {
                    bAccepted = false;
                    ErrorCode = TEXT("CACHE_FRAME_CONTENTS_INVALID");
                    Error = ValidationError;
                }
            }

            TestEqual(*FString::Printf(TEXT("%s acceptance"), *Id), bAccepted, bExpectedAccepted);
            TestEqual(*FString::Printf(TEXT("%s close classification"), *Id),
                !bAccepted && ErrorCode == TEXT("INVALID_MESSAGE"), bExpectedClose);
            if (!bAccepted)
            {
                TestEqual(*FString::Printf(TEXT("%s stable error code"), *Id),
                    ErrorCode, Expected->GetStringField(TEXT("error_code")));
                if (Keywords)
                {
                    TestTrue(*FString::Printf(TEXT("%s diagnostic keywords"), *Id),
                        ContainsKeywords(Error, *Keywords));
                }
            }
            continue;
        }
    }
    TestTrue(TEXT("Unreal exercised canonical conformance cases"), ApplicableCount > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUCacheSessionTest,
    "MtoULiveLink.CachedPlayback.CacheSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCacheSessionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    auto MakeSession = [](double& Clock, TArray<float>& Applied, TArray<int32>& ProgressPlays)
    {
        TSharedRef<FMtoUCacheSession> Session = MakeShared<FMtoUCacheSession>();
        Session->SetClock([&Clock]() { return Clock; });
        Session->BeginSession({1, 0, 7});
        Session->SetPublish([&Applied](const FMtoUFrameMessage& Frame) -> bool
        {
            Applied.Add(static_cast<float>(Frame.Transforms[0].Translation.X));
            return true;
        });
        Session->SetProgressSink([&ProgressPlays](int32 PlayId, int32 AppliedFrames)
        {
            ProgressPlays.Add(PlayId * 100000 + AppliedFrames);
        });
        return Session;
    };
    const auto UploadFrames = [](FMtoUCacheSession& Session, int32 Revision,
                                 int32 UploadId, const TArray<float>& Values,
                                 FMtoUCacheTransition& Result)
    {
        bool bOk = (Result = Session.HandleCommand(
            MakeCacheBeginCommand(Revision, Values.Num(), 30.0, UploadId))).bAccepted;
        for (int32 Index = 0; Index < Values.Num() && bOk; ++Index)
        {
            bOk = (Result = Session.HandleCommand(
                MakeCachedFrameCommand(Index, Values[Index], 64))).bAccepted;
        }
        if (bOk)
        {
            bOk = (Result = Session.HandleCommand(
                MakeSimpleCacheCommand(FMtoUCacheCommand::EKind::End))).bAccepted;
        }
        return bOk;
    };

    double Clock = 100.0;
    TArray<float> Applied;
    TArray<int32> ProgressPlays;
    TSharedRef<FMtoUCacheSession> Session = MakeSession(Clock, Applied, ProgressPlays);
    FMtoUCacheTransition Result;

    // Authoritative revision: the negotiated snapshot wins over client claims.
    TestFalse(TEXT("cache_begin for a foreign revision is rejected"),
        (Result = Session->HandleCommand(MakeCacheBeginCommand(6, 3, 30.0, 1))).bAccepted);
    TestEqual(TEXT("authoritative revision code"),
        Result.ErrorCode, FString(TEXT("CACHE_REVISION_MISMATCH")));
    TestEqual(TEXT("foreign-revision upload retains no frames"),
        Session->GetBufferedFrameCount(), 0);
    TestFalse(TEXT("a rejected upload identity is still consumed"),
        (Result = Session->HandleCommand(MakeCacheBeginCommand(6, 3, 30.0, 1))).bAccepted);
    TestEqual(TEXT("rejected upload identity reuse code"),
        Result.ErrorCode, FString(TEXT("CACHE_METADATA_INVALID")));

    // Upload identity must increase within the streaming session.
    TestTrue(TEXT("first upload identity is accepted"),
        (Result = Session->HandleCommand(MakeCacheBeginCommand(7, 2, 30.0, 4))).bAccepted);
    TestFalse(TEXT("reused upload identity is rejected"),
        (Result = Session->HandleCommand(MakeCacheBeginCommand(7, 2, 30.0, 4))).bAccepted);
    TestEqual(TEXT("reused upload identity code"),
        Result.ErrorCode, FString(TEXT("CACHE_METADATA_INVALID")));
    TestTrue(TEXT("increasing upload identity is accepted"),
        (Result = Session->HandleCommand(MakeCacheBeginCommand(7, 2, 30.0, 5))).bAccepted);

    // Actual encoded bytes are metered against the frozen limit with
    // overflow-safe accumulation.
    FMtoUCacheCommand HugeFrame = MakeCachedFrameCommand(0, 10.0f, 64);
    HugeFrame.EncodedBytes = (1ll << 40);
    TestFalse(TEXT("overflow-safe byte metering rejects the upload"),
        (Result = Session->HandleCommand(HugeFrame)).bAccepted);
    TestEqual(TEXT("metering code"),
        Result.ErrorCode, FString(TEXT("CACHE_PAYLOAD_TOO_LARGE")));
    TestEqual(TEXT("metered upload retains no frames"),
        Session->GetBufferedFrameCount(), 0);

    FMtoUCacheCommand UnderdeclaredBegin =
        MakeCacheBeginCommand(7, 2, 30.0, 6);
    UnderdeclaredBegin.Begin.PayloadSize = 8;
    TestTrue(TEXT("underdeclared upload begins"),
        (Result = Session->HandleCommand(UnderdeclaredBegin)).bAccepted);
    TestTrue(TEXT("frame within declaration is accepted"),
        (Result = Session->HandleCommand(MakeCachedFrameCommand(0, 11.0f, 8))).bAccepted);
    TestFalse(TEXT("actual bytes above the declared size reject the upload"),
        (Result = Session->HandleCommand(MakeCachedFrameCommand(1, 12.0f, 8))).bAccepted);
    TestEqual(TEXT("low-declared-size code"),
        Result.ErrorCode, FString(TEXT("CACHE_PAYLOAD_TOO_LARGE")));

    // Parsed-memory preflight from negotiated counts, before any allocation.
    // With the production counts of this session the prediction stays inside
    // the budget, so begin must succeed.
    TestTrue(TEXT("production-count begin stays within the parsed budget"),
        (Result = Session->HandleCommand(MakeCacheBeginCommand(7, 20000, 30.0, 7))).bAccepted);
    double BudgetClock = 50.0;
    TArray<float> BudgetApplied;
    TArray<int32> BudgetProgress;
    TSharedRef<FMtoUCacheSession> BudgetSession =
        MakeSession(BudgetClock, BudgetApplied, BudgetProgress);
    BudgetSession->BeginSession({10000, 5000, 7});
    FMtoUCacheCommand HugeBegin = MakeCacheBeginCommand(7, 20000, 30.0, 1);
    TestFalse(TEXT("predicted parsed memory above the budget rejects the upload"),
        (Result = BudgetSession->HandleCommand(HugeBegin)).bAccepted);
    TestEqual(TEXT("memory preflight code"),
        Result.ErrorCode, FString(TEXT("CACHE_PAYLOAD_TOO_LARGE")));
    TestFalse(TEXT("memory-rejected upload identity is still consumed"),
        (Result = BudgetSession->HandleCommand(HugeBegin)).bAccepted);
    TestEqual(TEXT("memory-rejected upload identity reuse code"),
        Result.ErrorCode, FString(TEXT("CACHE_METADATA_INVALID")));

    // A documented large production Character (701 bones, 500 BlendShapes)
    // stays supported at the maximum frozen frame count within the fixed
    // parsed-memory budget.
    double ProductionClock = 10.0;
    TArray<float> ProductionApplied;
    TArray<int32> ProductionProgress;
    TSharedRef<FMtoUCacheSession> ProductionSession =
        MakeSession(ProductionClock, ProductionApplied, ProductionProgress);
    ProductionSession->BeginSession({701, 500, 7});
    TestTrue(TEXT("a 701-bone production character at the maximum frame count"
                  " stays inside the fixed cache budget"),
        (Result = ProductionSession->HandleCommand(
            MakeCacheBeginCommand(7, 20000, 30.0, 1))).bAccepted);

    // Atomic Ready then identity-matched playback.
    TestTrue(TEXT("complete upload becomes Ready"),
        UploadFrames(*Session, 7, 8, {50.0f, 51.0f, 52.0f}, Result));
    TestEqual(TEXT("complete upload is Ready"),
        Session->GetState(), EMtoUCacheState::Ready);

    // A reused play identity cannot start a second attempt even after stop.
    FMtoUCacheCommand FirstPlay;
    FirstPlay.Kind = FMtoUCacheCommand::EKind::Play;
    FirstPlay.PlayId = 1;
    TestTrue(TEXT("initial play request is accepted"),
        (Result = Session->HandleCommand(FirstPlay)).bAccepted);
    TestTrue(TEXT("stop holds the attempt"),
        (Result = Session->HandleCommand(
            MakeSimpleCacheCommand(FMtoUCacheCommand::EKind::Stop))).bAccepted);
    TestEqual(TEXT("stopped state"),
        Session->GetState(), EMtoUCacheState::Stopped);
    TestFalse(TEXT("a reused play identity cannot start a second attempt"),
        (Result = Session->HandleCommand(FirstPlay)).bAccepted);
    TestEqual(TEXT("play-identity reuse code"),
        Result.ErrorCode, FString(TEXT("CACHE_METADATA_INVALID")));

    // Replay with deterministic windows: one pose per update at most, and
    // every pose - including the first - published by a later Tick.
    FMtoUCacheCommand Play;
    Play.Kind = FMtoUCacheCommand::EKind::Play;
    Play.PlayId = 2;
    const double Interval = 1.0 / 30.0;
    TestTrue(TEXT("matching play attempt starts local playback"),
        (Result = Session->HandleCommand(Play)).bAccepted);
    TestEqual(TEXT("playback state"),
        Session->GetState(), EMtoUCacheState::Playing);
    TestEqual(TEXT("cache_play only initializes the attempt"),
        Session->GetAppliedFrameCount(), 0);

    Clock += Interval / 2.0;
    TestEqual(TEXT("first cached pose is published by a later tick"),
        Session->Tick().AppliedFramesThisTick, 1);
    Clock += Interval / 2.0;
    TestEqual(TEXT("second pose applies in its own window"), Session->Tick().AppliedFramesThisTick, 1);
    Clock += Interval * 3.0;
    const int32 PosesBeforeLateTick = Applied.Num();
    Session->Tick();
    TestEqual(TEXT("delayed tick fails before any catch-up burst"),
        Applied.Num(), PosesBeforeLateTick);
    TestEqual(TEXT("late tick state"),
        Session->GetState(), EMtoUCacheState::Failed);
    TestTrue(TEXT("failure names the stable performance code"),
        Session->GetErrorDetails().Contains(TEXT("CACHED_PLAYBACK_PERFORMANCE")));

    // Retry on the retained cache: new identity, replay from zero.
    FMtoUCacheCommand RetryPlay;
    RetryPlay.Kind = FMtoUCacheCommand::EKind::Play;
    RetryPlay.PlayId = 9;
    TestTrue(TEXT("retry after failure reuses the uploaded cache"),
        (Result = Session->HandleCommand(RetryPlay)).bAccepted);
    Clock += Interval;
    Session->Tick();
    Clock += Interval;
    Session->Tick();
    Clock += Interval;
    Session->Tick();
    TestEqual(TEXT("retry applies every frame exactly once in order"),
        Session->GetAppliedFrameCount(), 3);
    TestEqual(TEXT("completion state"),
        Session->GetState(), EMtoUCacheState::Completed);
    TestEqual(TEXT("final pose held"),
        Session->GetLastAppliedIndex(), 2);
    TestTrue(TEXT("progress reported for the current attempt only"),
        !ProgressPlays.Contains(100001) && ProgressPlays.Contains(900001)
        && ProgressPlays.Contains(900002) && ProgressPlays.Contains(900003));

    // Publication refusal must not advance applied evidence or complete.
    TSharedRef<FMtoUCacheSession> RefusingSession =
        MakeSession(Clock, Applied, ProgressPlays);
    RefusingSession->SetPublish([](const FMtoUFrameMessage&) -> bool
    {
        return false;
    });
    TestTrue(TEXT("refusing session accepts its upload"),
        UploadFrames(*RefusingSession, 7, 3, {70.0f, 71.0f}, Result));
    FMtoUCacheCommand RefusingPlay;
    RefusingPlay.Kind = FMtoUCacheCommand::EKind::Play;
    RefusingPlay.PlayId = 4;
    TestTrue(TEXT("refusing session accepts play"),
        (Result = RefusingSession->HandleCommand(RefusingPlay)).bAccepted);
    Clock += Interval / 2.0;
    RefusingSession->Tick();
    TestEqual(TEXT("refused publication advances no applied evidence"),
        RefusingSession->GetAppliedFrameCount(), 0);
    Clock += Interval * 5.0;
    RefusingSession->Tick();
    TestEqual(TEXT("sustained refusal ends as a performance failure"),
        RefusingSession->GetState(), EMtoUCacheState::Failed);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUCacheTransitionTest,
    "MtoULiveLink.CachedPlayback.Transitions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCacheTransitionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using EKind = FMtoUCacheTransition::EKind;
    using ECommand = FMtoUCacheCommand::EKind;
    double Clock = 100.0;
    int32 Publications = 0;
    FMtoUCacheSession Session;
    Session.SetClock([&Clock]() { return Clock; });
    Session.SetPublish([&Publications](const FMtoUFrameMessage&)
    {
        ++Publications;
        return true;
    });
    Session.BeginSession({1, 0, 7});
    const auto CheckDemand = [this](const TCHAR* Label,
                                    const FMtoUCacheTransition& Result, bool bExpected)
    {
        TestTrue(Label, Result.RealtimeOverride.IsSet());
        if (Result.RealtimeOverride.IsSet())
        {
            TestEqual(Label, Result.RealtimeOverride.GetValue(), bExpected);
        }
    };
    const auto Upload = [this, &Session](int32 Revision, int32 UploadId)
    {
        const auto Receiving = Session.HandleCommand(MakeCacheBeginCommand(Revision, 1, 30.0, UploadId));
        TestTrue(TEXT("begin accepted"), Receiving.bAccepted);
        TestEqual(TEXT("begin reports expected frames"), Receiving.FrameCount, 1);
        TestFalse(TEXT("begin preserves refresh demand"), Receiving.RealtimeOverride.IsSet());
        TestTrue(TEXT("frame accepted"), Session.HandleCommand(MakeCachedFrameCommand(0, 42.0f, 64)).bAccepted);
        const auto Ready = Session.HandleCommand(MakeSimpleCacheCommand(ECommand::End));
        TestEqual(TEXT("complete upload outcome"), Ready.Kind, EKind::Ready);
        TestEqual(TEXT("ready upload identity"), Ready.UploadId, UploadId);
        TestEqual(TEXT("ready authoritative revision"), Ready.Revision, Revision);
        TestEqual(TEXT("ready buffered frames"), Ready.FrameCount, 1);
        TestFalse(TEXT("ready preserves refresh demand"), Ready.RealtimeOverride.IsSet());
    };
    const auto Play = [&Session](int32 PlayId)
    {
        auto Command = MakeSimpleCacheCommand(ECommand::Play);
        Command.PlayId = PlayId;
        return Session.HandleCommand(Command);
    };

    TestTrue(TEXT("new session accepts live frames"), Session.AcceptsLiveFrames());
    CheckDemand(TEXT("entry releases realtime"), Session.HandleCommand(MakeSimpleCacheCommand(ECommand::Enter)), false);
    TestFalse(TEXT("cached ownership excludes live frames"), Session.AcceptsLiveFrames());
    Upload(7, 1);
    CheckDemand(TEXT("play enables realtime"), Play(1), true);
    const auto Completed = Session.Tick();
    TestEqual(TEXT("final accepted pose emits completion"), Completed.Kind, EKind::Completed);
    TestEqual(TEXT("completion belongs to current play"), Completed.PlayId, 1);
    TestEqual(TEXT("completion reports actual accepted frames"), Completed.FrameCount, 1);
    TestEqual(TEXT("tick applies at most one pose"), Completed.AppliedFramesThisTick, 1);
    TestEqual(TEXT("injected clock determines elapsed time"), Completed.ElapsedSeconds, 0.0);
    TestFalse(TEXT("completion preserves realtime for natural final-pose evaluation"), Completed.RealtimeOverride.IsSet());
    TestEqual(TEXT("completion is emitted only once"), Session.Tick().Kind, EKind::None);
    TestEqual(TEXT("terminal tick does not republish"), Publications, 1);

    const auto Stopped = Session.HandleCommand(MakeSimpleCacheCommand(ECommand::Stop));
    CheckDemand(TEXT("stop releases realtime"), Stopped, false);
    TestEqual(TEXT("stop echoes held play identity"), Stopped.PlayId, 1);
    const auto StalePlay = Play(1);
    TestFalse(TEXT("stale play is rejected"), StalePlay.bAccepted);
    TestEqual(TEXT("rejected play echoes requested ID"), StalePlay.PlayId, 1);
    TestEqual(TEXT("rejected play retains upload identity"), StalePlay.UploadId, 1);
    TestFalse(TEXT("ownership-retaining rejection preserves refresh demand"), StalePlay.RealtimeOverride.IsSet());
    CheckDemand(TEXT("retry enables realtime"), Play(2), true);
    Clock += 1.0;
    const auto Failed = Session.Tick();
    TestEqual(TEXT("missed frame produces performance outcome"), Failed.Kind, EKind::PerformanceFailed);
    TestEqual(TEXT("performance outcome play identity"), Failed.PlayId, 2);
    TestEqual(TEXT("performance outcome upload identity"), Failed.UploadId, 1);
    TestEqual(TEXT("performance outcome stable code"), Failed.ErrorCode, FString(TEXT("CACHED_PLAYBACK_PERFORMANCE")));
    CheckDemand(TEXT("performance failure releases realtime"), Failed, false);
    TestEqual(TEXT("performance failure is emitted only once"), Session.Tick().Kind, EKind::None);

    const auto Cleared = Session.HandleCommand(MakeSimpleCacheCommand(ECommand::Clear));
    CheckDemand(TEXT("clear restores realtime"), Cleared, true);
    TestEqual(TEXT("clear captures released upload"), Cleared.UploadId, 1);
    TestEqual(TEXT("clear captures released play"), Cleared.PlayId, 2);
    TestTrue(TEXT("clear permits live frames"), Session.AcceptsLiveFrames());
    TestFalse(TEXT("clear preserves same-session stale upload rejection"),
        Session.HandleCommand(MakeCacheBeginCommand(7, 1, 30.0, 1)).bAccepted);
    const auto RejectedBegin = Session.HandleCommand(MakeCacheBeginCommand(8, 1, 30.0, 2));
    TestEqual(TEXT("rejected begin carries incoming identity after reset"), RejectedBegin.UploadId, 2);
    CheckDemand(TEXT("revision failure restores realtime"), RejectedBegin, true);
    Session.HandleCommand(MakeCacheBeginCommand(7, 1, 30.0, 3));
    const auto RejectedFrame = Session.HandleCommand(MakeCachedFrameCommand(-1, 42.0f, 64));
    TestEqual(TEXT("rejected frame retains pre-reset upload identity"), RejectedFrame.UploadId, 3);
    CheckDemand(TEXT("frame failure restores realtime"), RejectedFrame, true);
    Session.HandleCommand(MakeCacheBeginCommand(7, 1, 30.0, 4));
    const auto RejectedEnd = Session.HandleCommand(MakeSimpleCacheCommand(ECommand::End));
    TestEqual(TEXT("incomplete end retains pre-reset upload identity"), RejectedEnd.UploadId, 4);
    CheckDemand(TEXT("incomplete end restores realtime"), RejectedEnd, true);
    auto Reject = MakeSimpleCacheCommand(ECommand::Reject);
    Reject.UploadId = 5;
    Reject.ErrorCode = TEXT("CACHE_PAYLOAD_TOO_LARGE");
    Reject.ErrorDetails = TEXT("worker pre-allocation rejection");
    const auto RejectedIntake = Session.HandleCommand(Reject);
    TestEqual(TEXT("intake rejection uses worker-owned identity"), RejectedIntake.UploadId, 5);
    CheckDemand(TEXT("intake rejection restores realtime"), RejectedIntake, true);

    // Replacing a still-owned cache is atomic; it changes all validation
    // inputs and resets operation IDs while preserving injected dependencies.
    Upload(7, 6);
    Play(3);
    Session.BeginSession({2, 0, 8});
    TestEqual(TEXT("new session cancels old terminal work"), Session.Tick().Kind, EKind::None);
    TestEqual(TEXT("new session releases old frames"), Session.GetBufferedFrameCount(), 0);
    TestTrue(TEXT("new revision accepts fresh upload one"),
        Session.HandleCommand(MakeCacheBeginCommand(8, 1, 30.0, 1)).bAccepted);
    TestEqual(TEXT("new transform count is authoritative"),
        Session.HandleCommand(MakeCachedFrameCommand(0, 42.0f, 64)).ErrorCode,
        FString(TEXT("CACHE_FRAME_CONTENTS_INVALID")));
    Session.BeginSession({1, 1, 9});
    Session.HandleCommand(MakeCacheBeginCommand(9, 1, 30.0, 1));
    TestEqual(TEXT("new curve count is authoritative"),
        Session.HandleCommand(MakeCachedFrameCommand(0, 42.0f, 64)).ErrorCode,
        FString(TEXT("CACHE_FRAME_CONTENTS_INVALID")));
    Session.BeginSession({1, 0, 10});
    Upload(10, 1);
    TestTrue(TEXT("new session accepts fresh play one"), Play(1).bAccepted);
    TestEqual(TEXT("new session uses retained publication dependency"), Session.Tick().Kind, EKind::Completed);
    TestEqual(TEXT("new session publishes exactly one additional pose"), Publications, 2);
    Play(2);
    Session.EndSession();
    TestEqual(TEXT("teardown cannot emit old completion"), Session.Tick().Kind, EKind::None);
    TestEqual(TEXT("teardown releases frames"), Session.GetBufferedFrameCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUCacheQueueAdmissionTest,
    "MtoULiveLink.CachedPlayback.QueueAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCacheQueueAdmissionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    auto FrameCommand = [](int32 Index, int32 UploadId, int64 EncodedBytes)
    {
        FMtoUCacheCommand Command;
        Command.SessionId = 1;
        Command.Kind = FMtoUCacheCommand::EKind::Frame;
        Command.Index = Index;
        Command.UploadId = UploadId;
        Command.EncodedBytes = EncodedBytes;
        return Command;
    };
    const auto Never = []() { return false; };
    // A produce attempt that cannot admit must be released by its abort
    // predicate, so test producers use a short time-box instead of blocking.
    const auto BriefWait = []()
    {
        static thread_local double Deadline = 0.0;
        if (Deadline == 0.0)
        {
            Deadline = FPlatformTime::Seconds() + 0.05;
        }
        const bool bExpired = FPlatformTime::Seconds() > Deadline;
        if (bExpired)
        {
            Deadline = 0.0;
        }
        return bExpired;
    };
    const int64 PerFrame = FMtoUProtocol::MaxQueuedCacheBytes
        / FMtoUProtocol::MaxQueuedCacheFrames;

    // A stalled consumer can never grow queued parsed-frame ownership past
    // the frozen frame/byte budget: once full, the producer's wait is
    // released by its abort predicate instead of allocating another frame.
    FMtoUCacheCommandQueue Queue;
    int32 Admitted = 0;
    for (int32 Index = 0; Index < FMtoUProtocol::MaxQueuedCacheFrames + 32; ++Index)
    {
        if (Queue.Produce(FrameCommand(Index, 7, PerFrame), BriefWait))
        {
            ++Admitted;
        }
    }
    TestEqual(TEXT("frame admission stops at the frozen budget"),
        Admitted, static_cast<int32>(FMtoUProtocol::MaxQueuedCacheFrames));
    TestEqual(TEXT("queued frame count matches the budget"),
        Queue.GetPendingFrameCount(), static_cast<int32>(FMtoUProtocol::MaxQueuedCacheFrames));
    TestTrue(TEXT("queued byte ownership stays within budget"),
        Queue.GetPendingBytes() <= FMtoUProtocol::MaxQueuedCacheBytes);

    // A shutdown request aborts a would-be blocking producer immediately, so
    // teardown cannot be wedged by backpressure.
    TestFalse(TEXT("abort predicate releases the waiting producer"),
        Queue.Produce(FrameCommand(999, 7, PerFrame), []() { return true; }));

    // Control commands (a cache_clear carries weight 0) always make progress
    // even while the frame budget is saturated.
    {
        FMtoUCacheCommand Clear;
        Clear.SessionId = 1;
        Clear.Kind = FMtoUCacheCommand::EKind::Clear;
        TestTrue(TEXT("clear control is admitted despite a full frame budget"),
            Queue.Produce(Clear, Never));
    }
    TestTrue(TEXT("the full queue is cancellable wholesale by session"),
        Queue.CancelSession(1) > 0);
    TestEqual(TEXT("cancelling the session frees every queued frame"),
        Queue.GetPendingFrameCount(), 0);

    // A rejected or superseded upload discards exactly its own queued frames;
    // a newer upload identity is untouched and can still be admitted.
    Queue.Produce(FrameCommand(0, 5, PerFrame), Never);
    Queue.Produce(FrameCommand(1, 5, PerFrame), Never);
    Queue.Produce(FrameCommand(0, 6, PerFrame), Never);
    FMtoUCacheCommand End5;
    End5.SessionId = 1;
    End5.Kind = FMtoUCacheCommand::EKind::End;
    End5.UploadId = 5;
    Queue.Produce(End5, Never);
    TestEqual(TEXT("cancelling one upload drops only its frame/end commands"),
        Queue.CancelUpload(1, 5), 3);
    FMtoUCacheCommand Consumed;
    TestTrue(TEXT("the newer upload's frame survives"),
        Queue.TryConsume(Consumed));
    TestEqual(TEXT("surviving frame belongs to the newer upload"),
        Consumed.UploadId, 6);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUFramingTest,
    "MtoULiveLink.Protocol.PartialAndMultiplePackets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUFramingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString FirstText = TEXT("{\"type\":\"ready\",\"missing_curves\":[]}");
    const FString SecondText = TEXT("{\"type\":\"error\",\"message\":\"bad\"}");
    const TArray<uint8> First = Packet(FirstText);
    const TArray<uint8> Second = Packet(SecondText);
    FMtoUFrameDecoder Decoder;
    TArray<uint8> Payload;
    FString Error;

    Decoder.Append(First.GetData(), 1);
    TestTrue(TEXT("one byte needs more data"), Decoder.Pop(Payload, Error) == EMtoUDecodeResult::NeedMore);
    Decoder.Append(First.GetData() + 1, 3);
    TestTrue(TEXT("four bytes need more data"), Decoder.Pop(Payload, Error) == EMtoUDecodeResult::NeedMore);
    Decoder.Append(First.GetData() + 4, First.Num() - 4);
    TestTrue(TEXT("complete packet is decoded"), Decoder.Pop(Payload, Error) == EMtoUDecodeResult::Message);
    TestEqual(TEXT("decoded payload"), FromUtf8(Payload), FirstText);

    TArray<uint8> Combined = First;
    Combined.Append(Second);
    Decoder.Append(Combined.GetData(), Combined.Num());
    TestTrue(TEXT("first combined packet"), Decoder.Pop(Payload, Error) == EMtoUDecodeResult::Message);
    TestEqual(TEXT("first combined payload"), FromUtf8(Payload), FirstText);
    TestTrue(TEXT("second combined packet"), Decoder.Pop(Payload, Error) == EMtoUDecodeResult::Message);
    TestEqual(TEXT("second combined payload"), FromUtf8(Payload), SecondText);
    TestTrue(TEXT("later bytes are exhausted exactly"), Decoder.Pop(Payload, Error) == EMtoUDecodeResult::NeedMore);

    // The frozen per-message framing ceiling is enforced at the length header
    // before any payload accumulation: a maximal legal message still waits,
    // and one byte beyond the ceiling is rejected without allocation.
    FMtoUFrameDecoder MaximumDecoder;
    const TArray<uint8> MaximumPrefix =
        Prefix(static_cast<uint64>(FMtoUProtocol::MaxMessageBytes));
    MaximumDecoder.Append(MaximumPrefix.GetData(), MaximumPrefix.Num());
    TestTrue(TEXT("maximum representable packet waits for its payload"),
        MaximumDecoder.Pop(Payload, Error) == EMtoUDecodeResult::NeedMore);

    FMtoUFrameDecoder OverflowDecoder;
    const TArray<uint8> OverflowPrefix =
        Prefix(static_cast<uint64>(FMtoUProtocol::MaxMessageBytes) + 1);
    OverflowDecoder.Append(OverflowPrefix.GetData(), OverflowPrefix.Num());
    TestTrue(TEXT("packet one byte beyond the frozen message ceiling is rejected"),
        OverflowDecoder.Pop(Payload, Error) == EMtoUDecodeResult::Error);
    TestTrue(TEXT("packet length error is actionable"), Error.Contains(TEXT("length")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUInitValidationTest,
    "MtoULiveLink.Protocol.InitValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUInitValidationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FString Bones;
    for (int32 Index = 0; Index < 701; ++Index)
    {
        if (Index > 0)
        {
            Bones += TEXT(",");
        }
        Bones += FString::Printf(
            TEXT("[\"bone_%d\",%d,[0,0,0,0,0,0,1,1,1,1]]"), Index, Index - 1);
    }
    const FString Valid = FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[%s],\"curves\":[\"Smile\"]}"), *Bones);
    FMtoUInitMessage Message;
    FString Error;
    FString ErrorCode;

    TestTrue(TEXT("701 parent-first bones are accepted"), FMtoUProtocol::ParseInit(Utf8(Valid), Message, Error));
    TestEqual(TEXT("all bones are retained"), Message.Bones.Num(), 701);
    TestEqual(TEXT("all bind transforms are retained"), Message.SourceBindLocalPose.Num(), 701);
    TestTrue(TEXT("animation workflow is retained"), Message.Workflow == FMtoUWorkflows::Animation);
    TestTrue(TEXT("blendshape transmission is retained"), Message.bBlendshapesEnabled);
    TestFalse(TEXT("protocol version 2 is rejected"),
        FMtoUProtocol::ParseInit(Utf8(Valid.Replace(TEXT("\"revision\":9,\"version\":6"), TEXT("\"version\":2"))), Message, Error, &ErrorCode));
    TestEqual(TEXT("version 2 reports a protocol mismatch"), ErrorCode, FString(TEXT("PROTOCOL_VERSION_MISMATCH")));
    ErrorCode.Reset();
    TestFalse(TEXT("protocol-v3 clients are rejected"),
        FMtoUProtocol::ParseInit(Utf8(
            TEXT("{\"type\":\"init\",\"version\":3,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[]}")), Message, Error, &ErrorCode));
    TestEqual(TEXT("protocol-v3 clients report a version mismatch"), ErrorCode, FString(TEXT("PROTOCOL_VERSION_MISMATCH")));
    TestFalse(TEXT("version must have numeric JSON type"),
        FMtoUProtocol::ParseInit(Utf8(Valid.Replace(TEXT("\"revision\":9,\"version\":6"), TEXT("\"version\":\"4\""))), Message, Error));
    TestFalse(TEXT("a missing workflow field is rejected"), FMtoUProtocol::ParseInit(Utf8(Valid.Replace(
        TEXT("\"workflow\":\"animation\","), TEXT(""))), Message, Error));
    TestTrue(TEXT("missing workflow diagnostic identifies the field"), Error.Contains(TEXT("workflow")));
    TestFalse(TEXT("an unknown workflow value is rejected"), FMtoUProtocol::ParseInit(Utf8(Valid.Replace(
        TEXT("\"workflow\":\"animation\""), TEXT("\"workflow\":\"preview\""))), Message, Error));
    TestFalse(TEXT("a non-string workflow type is rejected"), FMtoUProtocol::ParseInit(Utf8(Valid.Replace(
        TEXT("\"workflow\":\"animation\""), TEXT("\"workflow\":7"))), Message, Error));
    TestTrue(TEXT("model workflow is accepted"), FMtoUProtocol::ParseInit(Utf8(Valid.Replace(
        TEXT("\"workflow\":\"animation\",\"blendshapes_enabled\":true"),
        TEXT("\"workflow\":\"model\",\"blendshapes_enabled\":false"))), Message, Error));
    TestTrue(TEXT("model workflow is retained"), Message.Workflow == FMtoUWorkflows::Model);
    TestFalse(TEXT("blendshape transmission choice is retained as disabled"), Message.bBlendshapesEnabled);
    TestFalse(TEXT("a missing blendshapes_enabled field is rejected"), FMtoUProtocol::ParseInit(Utf8(Valid.Replace(
        TEXT(",\"blendshapes_enabled\":true"), TEXT(""))), Message, Error));
    TestTrue(TEXT("missing blendshapes diagnostic identifies the field"), Error.Contains(TEXT("blendshapes_enabled")));
    TestFalse(TEXT("a non-boolean blendshapes_enabled type is rejected"), FMtoUProtocol::ParseInit(Utf8(Valid.Replace(
        TEXT("\"blendshapes_enabled\":true"), TEXT("\"blendshapes_enabled\":\"true\""))), Message, Error));
    TestTrue(TEXT("duplicate Maya short bone names are retained for Unreal remapping"),
        FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,1,1,1,1]],[\"root\",0,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[]}")), Message, Error));
    TestFalse(TEXT("a second root is rejected"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,1,1,1,1]],[\"other\",-1,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[]}")), Message, Error));
    TestFalse(TEXT("parents must precede children"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,1,1,1,1]],[\"child\",1,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[]}")), Message, Error));
    TestFalse(TEXT("bind-local transform is required"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1]],\"curves\":[]}")), Message, Error));
    TestFalse(TEXT("bind-local quaternion must be normalized"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,2,1,1,1]]],\"curves\":[]}")), Message, Error));
    TestFalse(TEXT("bind-local transform must be invertible"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,1,0,1,1]]],\"curves\":[]}")), Message, Error));

    const FString MarkerJson =
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"@\",-1,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[]}");
    TArray<uint8> OverlongUtf8 = Utf8(MarkerJson);
    const int32 OverlongMarker = OverlongUtf8.Find(static_cast<uint8>('@'));
    OverlongUtf8[OverlongMarker] = 0xc0;
    OverlongUtf8.Insert(static_cast<uint8>(0xaf), OverlongMarker + 1);
    TestFalse(TEXT("overlong UTF-8 encoding is rejected"),
        FMtoUProtocol::ParseInit(OverlongUtf8, Message, Error));
    TestTrue(TEXT("overlong UTF-8 diagnostic is actionable"), Error.Contains(TEXT("UTF-8")));

    TArray<uint8> LoneContinuation = Utf8(MarkerJson);
    const int32 ContinuationMarker = LoneContinuation.Find(static_cast<uint8>('@'));
    LoneContinuation[ContinuationMarker] = 0x80;
    TestFalse(TEXT("lone UTF-8 continuation byte is rejected"),
        FMtoUProtocol::ParseInit(LoneContinuation, Message, Error));
    TestTrue(TEXT("invalid UTF-8 diagnostic is actionable"), Error.Contains(TEXT("UTF-8")));

    TestTrue(TEXT("valid multibyte UTF-8 names are accepted"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"根\",-1,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[\"笑\"]}")), Message, Error));
    TestTrue(TEXT("multibyte bone name is preserved"), Message.Bones[0].Name == FName(TEXT("根")));
    TestTrue(TEXT("multibyte curve name is preserved"), Message.Curves[0] == FName(TEXT("笑")));

    const FString OverlongName = FString::ChrN(NAME_SIZE, TEXT('x'));
    TestFalse(TEXT("overlong bone name is rejected before FName construction"), FMtoUProtocol::ParseInit(Utf8(
        FString::Printf(TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,1,1,1,1]],[\"%s\",0,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[]}"),
            *OverlongName)), Message, Error));
    TestTrue(TEXT("overlong bone diagnostic identifies the limit"), Error.Contains(TEXT("Bone 1"))
        && Error.Contains(TEXT("NAME_SIZE")));
    TestFalse(TEXT("embedded NUL bone name is rejected before truncation"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,1,1,1,1]],[\"bad\\u0000tail\",0,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[]}")),
        Message, Error));
    TestTrue(TEXT("embedded NUL bone diagnostic is actionable"), Error.Contains(TEXT("Bone 1"))
        && Error.Contains(TEXT("U+0000")));
    TestFalse(TEXT("overlong curve name is rejected before FName construction"), FMtoUProtocol::ParseInit(Utf8(
        FString::Printf(TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[\"%s\"]}"),
            *OverlongName)), Message, Error));
    TestTrue(TEXT("overlong curve diagnostic identifies the limit"), Error.Contains(TEXT("Curve 0"))
        && Error.Contains(TEXT("NAME_SIZE")));
    TestFalse(TEXT("embedded NUL curve name is rejected before truncation"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"root\",-1,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[\"bad\\u0000tail\"]}")),
        Message, Error));
    TestTrue(TEXT("embedded NUL curve diagnostic is actionable"), Error.Contains(TEXT("Curve 0"))
        && Error.Contains(TEXT("U+0000")));

    FMtoUInitMessage Small;
    Small.Bones = {{FName(TEXT("root")), INDEX_NONE}, {FName(TEXT("jaw")), 0}};
    Small.Curves = {FName(TEXT("Smile")), FName(TEXT("Blink_R"))};
    FLiveLinkStaticDataStruct StaticData = FMtoUProtocol::MakeStaticData(Small, {FName(TEXT("Smile"))});
    const FLiveLinkSkeletonStaticData* Skeleton = StaticData.Cast<FLiveLinkSkeletonStaticData>();
    TestNotNull(TEXT("native skeleton data is built"), Skeleton);
    if (Skeleton)
    {
        TestEqual(TEXT("native bone count"), Skeleton->BoneNames.Num(), 2);
        TestTrue(TEXT("native bone name"), Skeleton->BoneNames[1] == FName(TEXT("jaw")));
        TestEqual(TEXT("native bone parent"), Skeleton->BoneParents[1], 0);
        TestEqual(TEXT("native accepted curve count"), Skeleton->PropertyNames.Num(), 1);
        TestTrue(TEXT("native accepted curve name"), Skeleton->PropertyNames[0] == FName(TEXT("Smile")));
    }

    FMtoUFrameDecoder Decoder;
    TArray<uint8> ReplyPayload;
    TArray<uint8> Ready = FMtoUProtocol::EncodeReady(
        {FName(TEXT("Blink_R"))},
        {FName(TEXT("Corrective"))},
        {TEXT("hair_7/tip -> tip1")},
        FMtoUWorkflows::Animation,
        5,
        2,
        9);
    Decoder.Append(Ready.GetData(), Ready.Num());
    TestTrue(TEXT("ready reply is framed"), Decoder.Pop(ReplyPayload, Error) == EMtoUDecodeResult::Message);
    TestTrue(TEXT("ready reply names missing curve"), FromUtf8(ReplyPayload).Contains(TEXT("Blink_R")));
    TestTrue(TEXT("ready reply names Unreal-only morph"),
        FromUtf8(ReplyPayload).Contains(TEXT("Corrective")));
    TestTrue(TEXT("ready reply reports bone name remapping"),
        FromUtf8(ReplyPayload).Contains(TEXT("tip1")));
    TSharedPtr<FJsonObject> ReadyJson;
    TestTrue(TEXT("ready reply is valid JSON"), JsonObjectFromBytes(ReplyPayload, ReadyJson));
    if (ReadyJson.IsValid())
    {
        TestEqual(TEXT("ready reply echoes the negotiated workflow"),
            ReadyJson->GetStringField(TEXT("workflow")), FString(TEXT("animation")));
        TestEqual(TEXT("ready reply reports the target morph count"),
            static_cast<int32>(ReadyJson->GetNumberField(TEXT("target_morph_count"))), 5);
        TestEqual(TEXT("ready reply reports the accepted morph count"),
            static_cast<int32>(ReadyJson->GetNumberField(TEXT("accepted_morph_count"))), 2);
    }
    TArray<uint8> ModelReady = FMtoUProtocol::EncodeReady({}, {}, {}, FMtoUWorkflows::Model, 12, 7, 9);
    Decoder.Append(ModelReady.GetData(), ModelReady.Num());
    TArray<uint8> ModelReplyPayload;
    TestTrue(TEXT("model ready reply is framed"),
        Decoder.Pop(ModelReplyPayload, Error) == EMtoUDecodeResult::Message);
    TestTrue(TEXT("model ready reply echoes the model workflow"),
        FromUtf8(ModelReplyPayload).Contains(TEXT("\"workflow\":\"model\""))
        && FromUtf8(ModelReplyPayload).Contains(TEXT("\"target_morph_count\":12"))
        && FromUtf8(ModelReplyPayload).Contains(TEXT("\"accepted_morph_count\":7")));
    TArray<uint8> Failure = FMtoUProtocol::EncodeError(
        TEXT("SKELETON_MISMATCH"), TEXT("bad skeleton"), TEXT("Missing in Unreal: jaw"));
    Decoder.Append(Failure.GetData(), Failure.Num());
    TestTrue(TEXT("error reply is framed"), Decoder.Pop(ReplyPayload, Error) == EMtoUDecodeResult::Message);
    TestTrue(TEXT("error reply keeps message"), FromUtf8(ReplyPayload).Contains(TEXT("bad skeleton")));
    TestTrue(TEXT("error reply keeps stable code"),
        FromUtf8(ReplyPayload).Contains(TEXT("SKELETON_MISMATCH")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUFrameValidationTest,
    "MtoULiveLink.Protocol.FrameValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUFrameValidationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString Valid = TEXT(
        "{\"type\":\"frame\",\"transforms\":[[1,2,3,0,0,0,1,1,1,1]],\"curves\":[0.5,0.25]}");
    FMtoUFrameMessage Frame;
    FString Error;
    bool bStructural = false;

    FString RoutedType;
    TestTrue(TEXT("streaming type route skips nested values"), FMtoUProtocol::PeekType(Utf8(
        TEXT("{\"metadata\":{\"type\":\"wrong\"},\"values\":[1,2,3],\"type\":\"cache_frame\"}")),
        RoutedType,
        Error));
    TestEqual(TEXT("streaming type route reads the root field"),
        RoutedType, FString(TEXT("cache_frame")));

    TestTrue(TEXT("valid frame parses"), FMtoUProtocol::ParseFrame(Utf8(Valid), Frame, Error));
    TestTrue(TEXT("matching counts and finite values validate"),
        FMtoUProtocol::ValidateFrame(Frame, 1, 2, Error, bStructural));
    TestFalse(TEXT("valid frame is not structural"), bStructural);

    TestFalse(TEXT("transform count mismatch is rejected"),
        FMtoUProtocol::ValidateFrame(Frame, 2, 2, Error, bStructural));
    TestTrue(TEXT("transform count mismatch is structural"), bStructural);
    TestFalse(TEXT("curve count mismatch is rejected"),
        FMtoUProtocol::ValidateFrame(Frame, 1, 1, Error, bStructural));
    TestTrue(TEXT("curve count mismatch is structural"), bStructural);

    int32 CacheIndex = INDEX_NONE;
    FString CacheErrorCode;
    FMtoUFrameMessage CacheFrame;
    TestFalse(TEXT("cached transform count is rejected during parse"),
        FMtoUProtocol::ParseCacheFrame(
            Utf8(TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[0,0,0,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}")),
            1,
            1,
            CacheIndex,
            CacheFrame,
            Error,
            &CacheErrorCode));
    TestEqual(TEXT("cached transform mismatch keeps its stable code"),
        CacheErrorCode, FString(TEXT("CACHE_FRAME_CONTENTS_INVALID")));
    TestTrue(TEXT("cached transform mismatch allocates no parsed arrays"),
        CacheFrame.Transforms.IsEmpty() && CacheFrame.Curves.IsEmpty());

    TestFalse(TEXT("cached curve count is rejected during parse"),
        FMtoUProtocol::ParseCacheFrame(
            Utf8(TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0,1]}")),
            1,
            1,
            CacheIndex,
            CacheFrame,
            Error,
            &CacheErrorCode));
    TestEqual(TEXT("cached curve mismatch keeps its stable code"),
        CacheErrorCode, FString(TEXT("CACHE_FRAME_CONTENTS_INVALID")));
    TestTrue(TEXT("cached curve mismatch allocates no parsed arrays"),
        CacheFrame.Transforms.IsEmpty() && CacheFrame.Curves.IsEmpty());

    FMtoUFrameMessage NonFinite;
    TestTrue(TEXT("overflowing JSON number parses for frame validation"), FMtoUProtocol::ParseFrame(Utf8(
        TEXT("{\"type\":\"frame\",\"transforms\":[[0,0,0,0,0,0,1,1,1,1]],\"curves\":[1e400]}")), NonFinite, Error));
    TestFalse(TEXT("non-finite value is rejected"),
        FMtoUProtocol::ValidateFrame(NonFinite, 1, 1, Error, bStructural));
    TestFalse(TEXT("non-finite frame does not close the connection"), bStructural);
    TestTrue(TEXT("non-finite diagnostic identifies curve"), Error.Contains(TEXT("curve 0")));

    FMtoUFrameMessage FloatOverflow;
    TestTrue(TEXT("finite double curve parses"), FMtoUProtocol::ParseFrame(Utf8(
        TEXT("{\"type\":\"frame\",\"transforms\":[[0,0,0,0,0,0,1,1,1,1]],\"curves\":[1e40]}")),
        FloatOverflow, Error));
    TestTrue(TEXT("overflow fixture is finite as double"), FMath::IsFinite(FloatOverflow.Curves[0]));
    TestFalse(TEXT("curve that overflows Live Link float is rejected"),
        FMtoUProtocol::ValidateFrame(FloatOverflow, 1, 1, Error, bStructural));
    TestFalse(TEXT("float narrowing overflow does not close the connection"), bStructural);
    TestTrue(TEXT("float narrowing diagnostic identifies curve"), Error.Contains(TEXT("curve 0"))
        && Error.Contains(TEXT("float")));

    TestFalse(TEXT("each transform requires ten numbers"), FMtoUProtocol::ParseFrame(Utf8(
        TEXT("{\"type\":\"frame\",\"transforms\":[[0,0,0]],\"curves\":[]}")), Frame, Error));
    TestTrue(TEXT("zero quaternion has valid JSON shape"), FMtoUProtocol::ParseFrame(Utf8(
        TEXT("{\"type\":\"frame\",\"transforms\":[[0,0,0,0,0,0,0,1,1,1]],\"curves\":[]}")), Frame, Error));
    TestFalse(TEXT("zero quaternion is rejected"),
        FMtoUProtocol::ValidateFrame(Frame, 1, 0, Error, bStructural));
    TestTrue(TEXT("zero quaternion is structural"), bStructural);
    TestTrue(TEXT("unnormalized quaternion has valid JSON shape"), FMtoUProtocol::ParseFrame(Utf8(
        TEXT("{\"type\":\"frame\",\"transforms\":[[0,0,0,0,0,0,2,1,1,1]],\"curves\":[]}")), Frame, Error));
    TestFalse(TEXT("unnormalized quaternion is rejected"),
        FMtoUProtocol::ValidateFrame(Frame, 1, 0, Error, bStructural));
    TestTrue(TEXT("unnormalized quaternion is structural"), bStructural);

    FMtoUFrameMessage NativeFrame;
    NativeFrame.Transforms = {{FVector(1.0, 2.0, 3.0), FQuat::Identity, FVector::OneVector}};
    NativeFrame.Curves = {0.5, 0.25};
    TestTrue(TEXT("native frame is validated before float narrowing"),
        FMtoUProtocol::ValidateFrame(NativeFrame, 1, 2, Error, bStructural));
    FLiveLinkFrameDataStruct FrameData = FMtoUProtocol::MakeFrameData(NativeFrame, {1});
    const FLiveLinkAnimationFrameData* Animation = FrameData.Cast<FLiveLinkAnimationFrameData>();
    TestNotNull(TEXT("native animation data is built"), Animation);
    if (Animation)
    {
        TestEqual(TEXT("native transform count"), Animation->Transforms.Num(), 1);
        TestTrue(TEXT("native translation"),
            Animation->Transforms[0].GetTranslation().Equals(FVector(1.0, 2.0, 3.0)));
        TestEqual(TEXT("only accepted curve is published"), Animation->PropertyValues.Num(), 1);
        TestEqual(TEXT("accepted curve value"), Animation->PropertyValues[0], 0.25f);
        TestTrue(TEXT("validated curve remains finite in Live Link data"),
            FMath::IsFinite(Animation->PropertyValues[0]));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUBindPoseInvariantTest,
    "MtoULiveLink.PoseRetargeting.BindPoseInvariant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUBindPoseInvariantTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const TArray<FTransform> SourceBindLocalPose = {
        FTransform(FRotator(0.0, 20.0, 0.0), FVector(10.0, 0.0, 0.0)),
        FTransform(FRotator(35.0, 0.0, 15.0), FVector(0.0, 8.0, 0.0)),
    };
    const TArray<FTransform> TargetRefLocalPose = {
        FTransform(FRotator(0.0, -10.0, 0.0), FVector(2.0, 0.0, 0.0)),
        FTransform(FRotator(-20.0, 5.0, 0.0), FVector(0.0, 12.0, 0.0)),
    };
    FMtoUFrameMessage SourceBindFrame;
    for (const FTransform& Transform : SourceBindLocalPose)
    {
        SourceBindFrame.Transforms.Add({
            Transform.GetTranslation(), Transform.GetRotation(), Transform.GetScale3D()});
    }

    FLiveLinkFrameDataStruct FrameData;
    FString Error;
    TestTrue(TEXT("a valid bind pose retargets"), FMtoUProtocol::MakeRetargetedFrameData(
        SourceBindFrame, {}, SourceBindLocalPose, TargetRefLocalPose, {INDEX_NONE, 0},
        FrameData, Error));
    const FLiveLinkAnimationFrameData* Animation = FrameData.Cast<FLiveLinkAnimationFrameData>();
    TestNotNull(TEXT("retargeted animation data is built"), Animation);
    if (!Animation)
    {
        return false;
    }
    TestEqual(TEXT("retargeted transform count"), Animation->Transforms.Num(), 2);
    for (int32 Index = 0; Index < TargetRefLocalPose.Num(); ++Index)
    {
        TestTrue(
            *FString::Printf(TEXT("source bind bone %d maps to target reference pose"), Index),
            Animation->Transforms[Index].Equals(TargetRefLocalPose[Index]));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUIdenticalReferencePoseTest,
    "MtoULiveLink.PoseRetargeting.IdenticalReferencePose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUIdenticalReferencePoseTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const TArray<FTransform> ReferencePose = {
        FTransform(FRotator(0.0, 15.0, 0.0), FVector(2.0, 3.0, 4.0)),
        FTransform(FRotator(10.0, 0.0, 20.0), FVector(0.0, 8.0, 0.0)),
    };
    const TArray<FTransform> CurrentPose = {
        FTransform(FRotator(5.0, 25.0, -3.0), FVector(7.0, 3.0, 4.0)),
        FTransform(FRotator(15.0, -7.0, 30.0), FVector(0.0, 8.0, 0.0)),
    };
    const TArray<FTransform> Output = RetargetPose(
        CurrentPose, ReferencePose, ReferencePose, {INDEX_NONE, 0});
    TestEqual(TEXT("identical reference output count"), Output.Num(), CurrentPose.Num());
    for (int32 Index = 0; Index < Output.Num(); ++Index)
    {
        TestTrue(*FString::Printf(TEXT("identical reference bone %d is unchanged"), Index),
            Output[Index].Equals(CurrentPose[Index], 1.0e-3f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUDifferentReferenceAxisTest,
    "MtoULiveLink.PoseRetargeting.DifferentReferenceAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUDifferentReferenceAxisTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FTransform SourceBind(FRotator(0.0, 0.0, 90.0));
    const FTransform TargetRef(FRotator(90.0, 0.0, 0.0));
    const FTransform Motion(FRotator(0.0, 30.0, 0.0), FVector(3.0, 4.0, 5.0));
    const FTransform SourceCurrent(SourceBind.ToMatrixWithScale() * Motion.ToMatrixWithScale());
    const FTransform Expected(TargetRef.ToMatrixWithScale() * Motion.ToMatrixWithScale());
    const TArray<FTransform> Output = RetargetPose(
        {SourceCurrent}, {SourceBind}, {TargetRef}, {INDEX_NONE});
    TestEqual(TEXT("different-axis output count"), Output.Num(), 1);
    TestTrue(TEXT("motion is transferred once in the target reference frame"),
        Output.IsValidIndex(0) && Output[0].Equals(Expected, 1.0e-3f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUParentChildRetargetingTest,
    "MtoULiveLink.PoseRetargeting.ParentAndChild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUParentChildRetargetingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FTransform SourceBindParent(FRotator(0.0, 20.0, 0.0), FVector(1.0, 2.0, 3.0));
    const FTransform SourceBindChild(FRotator(10.0, 0.0, 25.0), FVector(0.0, 8.0, 0.0));
    const FTransform TargetRefParent(FRotator(0.0, -15.0, 5.0), FVector(4.0, 0.0, 2.0));
    const FTransform TargetRefChild(FRotator(-20.0, 10.0, 0.0), FVector(0.0, 12.0, 0.0));
    const FTransform ParentMotion(FRotator(5.0, 15.0, 10.0), FVector(7.0, 0.0, 0.0));
    const FTransform ChildMotion(FRotator(-8.0, 12.0, 20.0), FVector(0.0, 2.0, 1.0));

    const FMatrix SourceBindParentComponent = SourceBindParent.ToMatrixWithScale();
    const FMatrix SourceBindChildComponent =
        SourceBindChild.ToMatrixWithScale() * SourceBindParentComponent;
    const FMatrix SourceCurrentParentComponent =
        SourceBindParentComponent * ParentMotion.ToMatrixWithScale();
    const FMatrix SourceCurrentChildComponent =
        SourceBindChildComponent * ChildMotion.ToMatrixWithScale();
    const TArray<FTransform> SourceCurrent = {
        FTransform(SourceCurrentParentComponent),
        FTransform(SourceCurrentChildComponent * SourceCurrentParentComponent.Inverse()),
    };

    const FMatrix ExpectedParentComponent =
        TargetRefParent.ToMatrixWithScale() * ParentMotion.ToMatrixWithScale();
    const FMatrix ExpectedChildComponent =
        TargetRefChild.ToMatrixWithScale()
        * TargetRefParent.ToMatrixWithScale()
        * ChildMotion.ToMatrixWithScale();
    const TArray<FTransform> Expected = {
        FTransform(ExpectedParentComponent),
        FTransform(ExpectedChildComponent * ExpectedParentComponent.Inverse()),
    };
    const TArray<FTransform> Output = RetargetPose(
        SourceCurrent,
        {SourceBindParent, SourceBindChild},
        {TargetRefParent, TargetRefChild},
        {INDEX_NONE, 0});
    TestEqual(TEXT("parent-child output count"), Output.Num(), 2);
    for (int32 Index = 0; Index < Output.Num(); ++Index)
    {
        TestTrue(*FString::Printf(TEXT("parent-child bone %d component motion maps"), Index),
            Output[Index].Equals(Expected[Index], 1.0e-3f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUTranslationRetargetingTest,
    "MtoULiveLink.PoseRetargeting.Translation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUTranslationRetargetingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const TArray<FTransform> SourceBind = {
        FTransform(FQuat::Identity, FVector(10.0, 0.0, 0.0)),
        FTransform(FQuat::Identity, FVector(0.0, 5.0, 0.0)),
    };
    const TArray<FTransform> TargetRef = {
        FTransform(FQuat::Identity, FVector(-3.0, 0.0, 0.0)),
        FTransform(FQuat::Identity, FVector(0.0, 9.0, 0.0)),
    };
    const TArray<FTransform> Current = {
        FTransform(FQuat::Identity, FVector(12.0, 4.0, 0.0)),
        FTransform(FQuat::Identity, FVector(1.0, 7.0, 0.0)),
    };
    const TArray<FTransform> Output = RetargetPose(
        Current, SourceBind, TargetRef, {INDEX_NONE, 0});
    TestTrue(TEXT("root translation delta is transferred"),
        Output.IsValidIndex(0)
        && Output[0].GetTranslation().Equals(FVector(-1.0, 4.0, 0.0), 1.0e-3f));
    TestTrue(TEXT("non-root translation is reference-corrected"),
        Output.IsValidIndex(1)
        && Output[1].GetTranslation().Equals(FVector(1.0, 11.0, 0.0), 1.0e-3f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUUnitScaleRetargetingTest,
    "MtoULiveLink.PoseRetargeting.UnitScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUUnitScaleRetargetingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FTransform SourceBind(FRotator(10.0, 20.0, 30.0), FVector(1.0, 2.0, 3.0));
    const FTransform TargetRef(FRotator(-5.0, 15.0, 25.0), FVector(4.0, 5.0, 6.0));
    const FTransform SourceCurrent(FRotator(20.0, 30.0, 40.0), FVector(7.0, 8.0, 9.0));
    const TArray<FTransform> Output = RetargetPose(
        {SourceCurrent}, {SourceBind}, {TargetRef}, {INDEX_NONE});
    TestTrue(TEXT("unit scale remains unit scale"), Output.IsValidIndex(0)
        && Output[0].GetScale3D().Equals(FVector::OneVector, 1.0e-3f));
    const FTransform UniformScaleMotion(
        FQuat::Identity, FVector::ZeroVector, FVector(1.25, 1.25, 1.25));
    const FTransform UniformScaleCurrent(
        SourceBind.ToMatrixWithScale() * UniformScaleMotion.ToMatrixWithScale());
    const TArray<FTransform> UniformScaleOutput = RetargetPose(
        {UniformScaleCurrent}, {SourceBind}, {TargetRef}, {INDEX_NONE});
    TestTrue(TEXT("uniform animated scale is transferred"),
        UniformScaleOutput.IsValidIndex(0)
        && UniformScaleOutput[0].GetScale3D().Equals(FVector(1.25), 1.0e-3f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUSingularRetargetTransformTest,
    "MtoULiveLink.PoseRetargeting.SingularTransformRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSingularRetargetTransformTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const TArray<FTransform> SourceBind = {
        FTransform(FRotator(0.0, 20.0, 0.0), FVector(1.0, 2.0, 3.0)),
        FTransform(FRotator(10.0, 0.0, 25.0), FVector(0.0, 8.0, 0.0)),
    };
    const TArray<FTransform> TargetRef = {
        FTransform(FRotator(0.0, -15.0, 5.0), FVector(4.0, 0.0, 2.0)),
        FTransform(FRotator(-20.0, 10.0, 0.0), FVector(0.0, 12.0, 0.0)),
    };
    const TArray<FTransform> Current = {
        FTransform(FRotator(5.0, 15.0, 10.0), FVector(7.0, 0.0, 0.0)),
        FTransform(FRotator(-8.0, 12.0, 20.0), FVector(0.0, 2.0, 1.0)),
    };
    const TArray<int32> Parents = {INDEX_NONE, 0};
    const auto WithScale = [](const TArray<FTransform>& Pose, int32 Bone, FVector Scale)
    {
        TArray<FTransform> Modified = Pose;
        Modified[Bone].SetScale3D(Scale);
        return Modified;
    };
    const auto RetargetAttempt = [&](
        const TArray<FTransform>& CurrentPose,
        const TArray<FTransform>& BindPose,
        const TArray<FTransform>& RefPose,
        FString& OutError)
    {
        FLiveLinkFrameDataStruct FrameData;
        const bool bBuilt = FMtoUProtocol::MakeRetargetedFrameData(
            FrameFromPose(CurrentPose), {}, BindPose, RefPose, Parents, FrameData, OutError);
        TestNull(TEXT("a rejected pose publishes no frame data"),
            bBuilt ? nullptr : FrameData.Cast<FLiveLinkAnimationFrameData>());
        return bBuilt;
    };

    FString Error;
    TestTrue(TEXT("the valid parent-child pose still retargets"),
        RetargetAttempt(Current, SourceBind, TargetRef, Error));
    TestTrue(TEXT("the valid pose reports no error"), Error.IsEmpty());

    TestFalse(TEXT("a zero-scale source current parent is rejected"),
        RetargetAttempt(WithScale(Current, 0, FVector(0.0, 1.0, 1.0)), SourceBind, TargetRef, Error));
    TestTrue(TEXT("the source current rejection names the bone and matrix set"),
        Error.Contains(TEXT("Bone 0")) && Error.Contains(TEXT("source current")));

    TestFalse(TEXT("a zero-scale source current leaf is rejected"),
        RetargetAttempt(WithScale(Current, 1, FVector(1.0, 1.0, 0.0)), SourceBind, TargetRef, Error));
    TestTrue(TEXT("the source current leaf rejection names the bone"),
        Error.Contains(TEXT("Bone 1")) && Error.Contains(TEXT("source current")));

    TestFalse(TEXT("a zero-scale source bind transform is rejected"),
        RetargetAttempt(Current, WithScale(SourceBind, 0, FVector(0.0, 0.0, 0.0)), TargetRef, Error));
    TestTrue(TEXT("the bind rejection names the bone and matrix set"),
        Error.Contains(TEXT("Bone 0")) && Error.Contains(TEXT("source bind")));

    TestFalse(TEXT("a zero-scale target reference transform is rejected"),
        RetargetAttempt(Current, SourceBind, WithScale(TargetRef, 0, FVector(1.0, 0.0, 1.0)), Error));
    TestTrue(TEXT("the target reference rejection names the bone and matrix set"),
        Error.Contains(TEXT("Bone 0")) && Error.Contains(TEXT("target reference")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUConnectionNegotiatorTest,
    "MtoULiveLink.Negotiation.ConnectionCompatibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUConnectionNegotiatorTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FMtoUCharacterDescription ExactCharacter = {
        {
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("arm")), 0},
            {FName(TEXT("hand")), 1},
        },
        {FName(TEXT("Smile")), FName(TEXT("Blink"))},
    };
    const FMtoUTargetDescription ExactTarget = {
        {
            {FName(TEXT("hand")), 2},
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("arm")), 1},
        },
        {FName(TEXT("Corrective")), FName(TEXT("Smile"))},
    };
    const FMtoUNegotiationOutcome Exact =
        FMtoUConnectionNegotiator::Negotiate(ExactCharacter, ExactTarget);
    TestTrue(TEXT("equivalent shuffled skeletons are usable"), Exact.bUsable);
    TestTrue(TEXT("usable outcome has no failure category"), Exact.FailureCategory.IsEmpty());
    TestTrue(TEXT("publish names preserve Maya transform order"),
        Exact.PublishBoneNames == TArray<FName>({TEXT("root"), TEXT("arm"), TEXT("hand")}));
    TestTrue(TEXT("matching Maya curve is accepted by index and name"),
        Exact.AcceptedCurveIndices == TArray<int32>({0})
        && Exact.AcceptedCurveNames == TArray<FName>({TEXT("Smile")}));
    TestTrue(TEXT("bidirectional Morph differences are reported and sorted"),
        Exact.MayaOnlyMorphNames == TArray<FName>({TEXT("Blink")})
        && Exact.UnrealOnlyMorphNames == TArray<FName>({TEXT("Corrective")}));

    const FMtoUCharacterDescription DuplicateCharacter = {
        {
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("hair_1")), 0},
            {FName(TEXT("tip_2")), 1},
            {FName(TEXT("tip_3")), 2},
            {FName(TEXT("hair_7")), 0},
            {FName(TEXT("tip_2")), 4},
            {FName(TEXT("tip_3")), 5},
        },
        {},
    };
    const FMtoUTargetDescription AutoRenamedTarget = {
        {
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("hair_1")), 0},
            {FName(TEXT("tip_2")), 1},
            {FName(TEXT("tip_3")), 2},
            {FName(TEXT("hair_7")), 0},
            {FName(TEXT("tip_21")), 4},
            {FName(TEXT("tip_31")), 5},
        },
        {},
    };
    const FMtoUNegotiationOutcome Remapped =
        FMtoUConnectionNegotiator::Negotiate(DuplicateCharacter, AutoRenamedTarget);
    TestTrue(TEXT("unique parent-scoped numeric suffix remaps are usable"), Remapped.bUsable);
    TestTrue(TEXT("second duplicate receives the UE imported name"),
        Remapped.PublishBoneNames[5] == FName(TEXT("tip_21")));
    TestEqual(TEXT("both imported renames are reported"), Remapped.BoneNameMappings.Num(), 2);

    FMtoUTargetDescription AmbiguousTarget = AutoRenamedTarget;
    AmbiguousTarget.Bones[6] = {FName(TEXT("tip_22")), 4};
    const FMtoUNegotiationOutcome Ambiguous =
        FMtoUConnectionNegotiator::Negotiate(DuplicateCharacter, AmbiguousTarget);
    TestFalse(TEXT("multiple numeric-suffix candidates are blocking"), Ambiguous.bUsable);
    TestEqual(TEXT("skeleton mismatch has a stable failure category"),
        Ambiguous.FailureCategory, FString(TEXT("SKELETON_MISMATCH")));
    TestTrue(TEXT("ambiguity details name the Maya bone"),
        Ambiguous.MappingAmbiguities.Num() == 1
        && Ambiguous.MappingAmbiguities[0].Contains(TEXT("tip_2")));
    TestTrue(TEXT("ambiguity blocks its descendant instead of guessing"),
        Ambiguous.DescendantsBlockedByParent.Num() == 1
        && Ambiguous.DescendantsBlockedByParent[0].Contains(TEXT("tip_3")));

    const FMtoUCharacterDescription BranchCharacter = {
        {
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("left")), 0},
            {FName(TEXT("left_child")), 1},
            {FName(TEXT("right")), 0},
            {FName(TEXT("right_child")), 3},
        },
        {FName(TEXT("Smile"))},
    };
    const FMtoUTargetDescription BrokenBranchesTarget = {
        {
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("wrong_left")), 0},
            {FName(TEXT("left_child")), 1},
            {FName(TEXT("right")), 0},
            {FName(TEXT("wrong_right_child")), 3},
            {FName(TEXT("extra")), 0},
        },
        {FName(TEXT("Smile"))},
    };
    const FMtoUNegotiationOutcome BrokenBranches =
        FMtoUConnectionNegotiator::Negotiate(BranchCharacter, BrokenBranchesTarget);
    TestFalse(TEXT("missing and extra bones are blocking"), BrokenBranches.bUsable);
    TestTrue(TEXT("independent branch failures are both reported"),
        BrokenBranches.MissingBones.Contains(TEXT("left"))
        && BrokenBranches.MissingBones.Contains(TEXT("right_child")));
    TestTrue(TEXT("a failed parent records its blocked descendant"),
        BrokenBranches.DescendantsBlockedByParent.Num() == 1
        && BrokenBranches.DescendantsBlockedByParent[0].Contains(TEXT("left_child")));
    TestTrue(TEXT("unmatched target bones are reported as extra"),
        BrokenBranches.ExtraBones.Contains(TEXT("extra")));
    TestTrue(TEXT("Morph comparison does not run for a blocking skeleton"),
        BrokenBranches.AcceptedCurveIndices.IsEmpty()
        && BrokenBranches.MayaOnlyMorphNames.IsEmpty()
        && BrokenBranches.UnrealOnlyMorphNames.IsEmpty());

    const FMtoUTargetDescription WrongParentTarget = {
        {
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("arm")), 0},
            {FName(TEXT("hand")), 0},
        },
        {},
    };
    const FMtoUNegotiationOutcome WrongParent =
        FMtoUConnectionNegotiator::Negotiate(ExactCharacter, WrongParentTarget);
    TestTrue(TEXT("wrong-parent details name both parents"),
        WrongParent.ParentMismatches.Num() == 1
        && WrongParent.ParentMismatches[0].Contains(TEXT("hand"))
        && WrongParent.ParentMismatches[0].Contains(TEXT("arm"))
        && WrongParent.ParentMismatches[0].Contains(TEXT("root")));
    TestTrue(TEXT("structured failures render protocol-ready technical details"),
        WrongParent.TechnicalDetails().Contains(TEXT("Parent mismatches")));

    const FMtoUCharacterDescription DuplicateRootName = {
        {
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("root")), 0},
        },
        {},
    };
    const FMtoUTargetDescription SuffixedRoot = {
        {
            {FName(TEXT("root1")), INDEX_NONE},
            {FName(TEXT("child")), 0},
        },
        {},
    };
    const FMtoUNegotiationOutcome RootRename =
        FMtoUConnectionNegotiator::Negotiate(DuplicateRootName, SuffixedRoot);
    TestFalse(TEXT("numeric suffix remapping requires an already mapped parent"), RootRename.bUsable);
    TestTrue(TEXT("a root typo is reported as missing instead of fuzzy matched"),
        RootRename.MissingBones.Contains(TEXT("root")));

    const FMtoUCharacterDescription SourceSiblingOrder = {
        {
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("left")), 0},
            {FName(TEXT("left_tip")), 1},
            {FName(TEXT("right")), 0},
        },
        {},
    };
    const FMtoUTargetDescription TargetSiblingOrder = {
        {
            {FName(TEXT("root")), INDEX_NONE},
            {FName(TEXT("right")), 0},
            {FName(TEXT("left")), 0},
            {FName(TEXT("left_tip")), 2},
        },
        {},
    };
    const FMtoUNegotiationOutcome SiblingOrder =
        FMtoUConnectionNegotiator::Negotiate(SourceSiblingOrder, TargetSiblingOrder);
    TestTrue(TEXT("different sibling array order remains usable"), SiblingOrder.bUsable);
    TestTrue(TEXT("source order retains the aligned target bone indices"),
        SiblingOrder.TargetBoneIndices == TArray<int32>({0, 2, 3, 1}));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUWorldOffsetTest,
    "MtoULiveLink.Protocol.WorldOffset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUWorldOffsetTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    TestNotNull(TEXT("editor preview world is created"), World);
    if (!World)
    {
        return false;
    }

    const FTransform PlacedTransform(FRotator(10.0, 20.0, 30.0), FVector(100.0, 200.0, 300.0));
    AMtoULiveLinkActor* Actor = World->SpawnActor<AMtoULiveLinkActor>(
        AMtoULiveLinkActor::StaticClass(), PlacedTransform);
    TestNotNull(TEXT("binding actor is spawned"), Actor);
    if (Actor)
    {
        TestTrue(TEXT("Live Link animation updates continuously in the editor"),
            Actor->GetSkeletalMeshComponent()->GetUpdateAnimationInEditor());
    }

    const FTransform StreamedRoot(FRotator(5.0, 15.0, 25.0), FVector(7.0, 8.0, 9.0));
    FMtoUFrameMessage Frame;
    Frame.Transforms = {{StreamedRoot.GetTranslation(), StreamedRoot.GetRotation(), StreamedRoot.GetScale3D()}};
    const FLiveLinkFrameDataStruct FrameData = FMtoUProtocol::MakeFrameData(Frame, {});
    const FLiveLinkAnimationFrameData* AnimationData = FrameData.Cast<FLiveLinkAnimationFrameData>();
    TestNotNull(TEXT("animation frame is built"), AnimationData);
    if (AnimationData)
    {
        TestTrue(TEXT("Root motion stays in Live Link frame"),
            AnimationData->Transforms[0].Equals(StreamedRoot));
    }
    if (Actor)
    {
        TestTrue(TEXT("Actor placement is unchanged"),
            Actor->GetActorTransform().Equals(PlacedTransform));
    }

    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUSourceShutdownTest,
    "MtoULiveLink.Source.IdempotentShutdown",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSourceShutdownTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }

    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(0);
    TestTrue(TEXT("port zero source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));
    const int32 Port = ListeningPort(Source);
    TestTrue(TEXT("ephemeral listening port is reported"), Port > 0 && Port <= MAX_uint16);

    Source->StopListener();
    Source->StopListener();
    TestTrue(TEXT("shutdown request remains idempotent"), Source->RequestSourceShutdown());
    TestFalse(TEXT("stopped source is no longer valid"), Source->IsSourceStillValid());

    FSocket* Replacement = Port > 0
        ? BindLoopback(*SocketSubsystem, static_cast<uint16>(Port), true)
        : nullptr;
    TestNotNull(TEXT("worker released its ephemeral listener socket"), Replacement);
    DestroySocket(*SocketSubsystem, Replacement);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUSourceSocketFlowTest,
    "MtoULiveLink.Source.SocketFlow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSourceSocketFlowTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

    FSocket* Reservation = BindLoopback(*SocketSubsystem, 0, true);
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(*SocketSubsystem, Reservation);

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    TestNotNull(TEXT("editor world is created"), World);
    if (World && GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = World ? AddBoundActor(*World) : nullptr;
    TestNotNull(TEXT("placed binding actor is created"), Actor);

    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    TestTrue(TEXT("source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));

    AMtoULiveLinkActor* ExtraActor = World ? AddBoundActor(*World) : nullptr;
    TestNotNull(TEXT("second binding actor fixture is created"), ExtraActor);
    FSocket* MultipleActorClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("multiple-actor validation client connects"), MultipleActorClient);
    const TArray<uint8> MultipleActorInit = Packet(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"Bone01\",-1,[0,0,0,0,0,0,1,1,1,1]],[\"Bone02\",0,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[]}"));
    TestTrue(TEXT("multiple-actor init is sent"), MultipleActorClient
        && SendBytes(*MultipleActorClient, MultipleActorInit.GetData(), MultipleActorInit.Num()));
    TArray<uint8> Payload;
    TestTrue(TEXT("multiple actors produce a framed rejection"),
        MultipleActorClient && ReceivePacket(
            *MultipleActorClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("multiple actors use the stable error code"),
        FromUtf8(Payload).Contains(TEXT("MULTIPLE_BINDING_ACTORS")));
    DestroySocket(*SocketSubsystem, MultipleActorClient);
    if (ExtraActor)
    {
        ExtraActor->MarkAsGarbage();
    }
    TestTrue(TEXT("source returns to listening after actor-count rejection"),
        WaitForStatus(Source, TEXT("Listening on")));

    FSocket* Primary = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("first Maya client connects"), Primary);
    const USkeletalMesh* TestMesh = Actor
        ? Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
        : nullptr;
    const FReferenceSkeleton* TestSkeleton = TestMesh ? &TestMesh->GetRefSkeleton() : nullptr;
    TestTrue(TEXT("test mesh has the two streamed bones"),
        TestSkeleton && TestSkeleton->GetNum() >= 2);
    const FString RootBoneName = TestSkeleton && TestSkeleton->GetNum() >= 1
        ? TestSkeleton->GetBoneName(0).ToString()
        : TEXT("Bone01");
    const FString ChildBoneName = TestSkeleton && TestSkeleton->GetNum() >= 2
        ? TestSkeleton->GetBoneName(1).ToString()
        : TEXT("Bone02");
    const FString RootBind = TestSkeleton && TestSkeleton->GetNum() >= 1
        ? TransformJson(TestSkeleton->GetRefBonePose()[0])
        : TEXT("[0,0,0,0,0,0,1,1,1,1]");
    const FString ChildBind = TestSkeleton && TestSkeleton->GetNum() >= 2
        ? TransformJson(TestSkeleton->GetRefBonePose()[1])
        : TEXT("[0,0,0,0,0,0,1,1,1,1]");
    const TArray<uint8> Init = Packet(FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"%s\",-1,%s],[\"%s\",0,%s]],\"curves\":[\"Missing\"]}"),
        *RootBoneName,
        *RootBind,
        *ChildBoneName,
        *ChildBind));
    if (Primary)
    {
        TestTrue(TEXT("partial init prefix is sent"), SendBytes(*Primary, Init.GetData(), 3));
        FPlatformProcess::Sleep(0.02f);
        TestTrue(TEXT("remaining init bytes are sent"),
            SendBytes(*Primary, Init.GetData() + 3, Init.Num() - 3));
    }

    Payload.Reset();
    TestTrue(TEXT("partial init produces ready response"), Primary && ReceivePacket(
        *Primary, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("ready response reports the omitted morph curve"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\""))
        && FromUtf8(Payload).Contains(TEXT("Missing")));
    TestTrue(TEXT("ready response echoes the animation workflow and morph counts"),
        FromUtf8(Payload).Contains(TEXT("\"workflow\":\"animation\""))
        && FromUtf8(Payload).Contains(TEXT("\"target_morph_count\":0"))
        && FromUtf8(Payload).Contains(TEXT("\"accepted_morph_count\":0")));
#if WITH_EDITOR
    if (GEditor)
    {
        const FText OverrideName = FText::FromString(TEXT("MtoU Live Link"));
        for (const FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient)
            {
                TestTrue(TEXT("connected stream forces editor viewport realtime"),
                    ViewportClient->HasRealtimeOverride(OverrideName));
            }
        }
    }
#endif

    FSocket* Second = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("second TCP client reaches listener"), Second);
    Payload.Reset();
    TestTrue(TEXT("second client receives a framed rejection"),
        Second && ReceivePacket(*Second, Payload));
    TestTrue(TEXT("second client rejection is actionable"),
        FromUtf8(Payload).Contains(TEXT("already has a Maya client")));
    DestroySocket(*SocketSubsystem, Second);

    const FLiveLinkSubjectKey SubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));
    const TArray<uint8> ReferencePoseFrame = Packet(FString::Printf(
        TEXT("{\"type\":\"frame\",\"transforms\":[%s,%s],\"curves\":[0]}"),
        *RootBind,
        *ChildBind));
    TestTrue(TEXT("target reference-pose diagnostic frame is sent"),
        Primary && SendBytes(
            *Primary, ReferencePoseFrame.GetData(), ReferencePoseFrame.Num()));
    FLiveLinkSubjectFrameData ReferenceEvaluatedFrame;
    const bool bReferenceEvaluated = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        if (!LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), ReferenceEvaluatedFrame))
        {
            return false;
        }
        const FLiveLinkAnimationFrameData* Animation =
            ReferenceEvaluatedFrame.FrameData.Cast<FLiveLinkAnimationFrameData>();
        return Animation && Animation->Transforms.Num() == 2;
    });
    TestTrue(TEXT("target reference-pose frame reaches Live Link"), bReferenceEvaluated);
    if (bReferenceEvaluated && TestSkeleton)
    {
        const FLiveLinkAnimationFrameData* Animation =
            ReferenceEvaluatedFrame.FrameData.Cast<FLiveLinkAnimationFrameData>();
        TestTrue(TEXT("target reference-pose root remains undistorted"),
            Animation->Transforms[0].Equals(TestSkeleton->GetRefBonePose()[0], 1.0e-3f));
        TestTrue(TEXT("target reference-pose child remains undistorted"),
            Animation->Transforms[1].Equals(TestSkeleton->GetRefBonePose()[1], 1.0e-3f));
    }

    const TArray<uint8> NonFinite = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[[0,0,0,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[1e400]}"));
    const TArray<uint8> Valid = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[[1,2,3,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0.5]}"));
    TArray<uint8> Combined = NonFinite;
    Combined.Append(Valid);
    AddExpectedError(TEXT("Dropped frame: Non-finite curve 0."),
        EAutomationExpectedErrorFlags::Contains, 1);
    TestTrue(TEXT("combined invalid and valid frame packets are sent"),
        Primary && SendBytes(*Primary, Combined.GetData(), Combined.Num()));

    FLiveLinkSubjectFrameData EvaluatedFrame;
    const bool bEvaluated = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        if (!LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), EvaluatedFrame))
        {
            return false;
        }
        const FLiveLinkAnimationFrameData* Animation =
            EvaluatedFrame.FrameData.Cast<FLiveLinkAnimationFrameData>();
        return Animation
            && Animation->Transforms.IsValidIndex(0)
            && Animation->Transforms[0].GetTranslation().Equals(FVector(1.0, 2.0, 3.0));
    });
    TestTrue(TEXT("following valid frame is evaluated after the non-finite frame"), bEvaluated);
    if (bEvaluated)
    {
        const FLiveLinkAnimationFrameData* Animation =
            EvaluatedFrame.FrameData.Cast<FLiveLinkAnimationFrameData>();
        TestTrue(TEXT("following valid frame keeps the transmitted root translation"),
            Animation->Transforms[0].GetTranslation().Equals(FVector(1.0, 2.0, 3.0)));
    }

    USkeletalMeshComponent* SkeletalMeshComponent =
        Actor ? Actor->GetSkeletalMeshComponent() : nullptr;
    TestNotNull(TEXT("binding actor exposes its skeletal mesh component"), SkeletalMeshComponent);
    TestTrue(TEXT("editor skeletal mesh component tick is enabled"),
        SkeletalMeshComponent && SkeletalMeshComponent->IsComponentTickEnabled());
    TestTrue(TEXT("editor skeletal mesh component is registered"),
        SkeletalMeshComponent && SkeletalMeshComponent->IsRegistered());
    TestTrue(TEXT("editor world actors are initialized"),
        World && World->AreActorsInitialized());
    ULiveLinkInstance* LiveLinkInstance = SkeletalMeshComponent
        ? Cast<ULiveLinkInstance>(SkeletalMeshComponent->GetAnimInstance())
        : nullptr;
    TestNotNull(TEXT("binding actor owns a Live Link animation instance"), LiveLinkInstance);
    TestTrue(TEXT("Live Link animation evaluation is enabled"),
        LiveLinkInstance && LiveLinkInstance->GetEnableLiveLinkEvaluation());

    const TArray<uint8> NextValid = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[[11,12,13,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0.75]}"));
    TestTrue(TEXT("next editor animation frame is sent"),
        Primary && SendBytes(*Primary, NextValid.GetData(), NextValid.Num()));
    bool bSecondFrameEvaluated = false;
    bool bSubjectEvaluatedAfterSecondSend = false;
    FVector LastCachedRoot = FVector::ZeroVector;
    PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        FLiveLinkSubjectFrameData SecondFrame;
        if (LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), SecondFrame))
        {
            if (const FLiveLinkAnimationFrameData* Animation =
                    SecondFrame.FrameData.Cast<FLiveLinkAnimationFrameData>())
            {
                if (Animation->Transforms.IsValidIndex(0))
                {
                    bSubjectEvaluatedAfterSecondSend = true;
                    LastCachedRoot = Animation->Transforms[0].GetTranslation();
                    bSecondFrameEvaluated = LastCachedRoot.Equals(FVector(11.0, 12.0, 13.0));
                }
            }
        }
        return bSecondFrameEvaluated;
    });
    TestTrue(TEXT("Live Link subject remains evaluable after the second send"),
        bSubjectEvaluatedAfterSecondSend);
    if (!bSecondFrameEvaluated)
    {
        AddError(FString::Printf(
            TEXT("Expected second cached root (11, 12, 13), got %s."),
            *LastCachedRoot.ToString()));
    }
    TestTrue(TEXT("second frame reaches the Live Link subject cache"), bSecondFrameEvaluated);

    // Cached Playback round trip on the same negotiated connection: enter,
    // upload a complete cache with identity and authoritative revision,
    // receive identity-matched Ready, then let Unreal apply every frame
    // locally exactly once in order while Maya sends no animation data.
    FLiveLinkSubjectFrameData CachedEvaluated;
    auto SendLine = [&](const FString& Text)
    {
        const TArray<uint8> Bytes = Packet(Text);
        return Primary && SendBytes(*Primary, Bytes.GetData(), Bytes.Num());
    };
    TestTrue(TEXT("cache entry is sent"),
        SendLine(TEXT("{\"type\":\"cache_enter\"}")));
    FPlatformProcess::Sleep(0.02f);
    Source->Update();
    TestTrue(TEXT("cache_begin is sent"),
        SendLine(TEXT("{\"type\":\"cache_begin\",\"upload_id\":1,\"revision\":9,\"fps\":30,\"start_frame\":2001,\"end_frame\":2002,\"frame_count\":2,\"payload_size\":512}")));
    TestTrue(TEXT("cached frame 0 is sent"),
        SendLine(TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[21,22,23,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}")));
    TestTrue(TEXT("negative cached frame index keeps the session open"),
        SendLine(TEXT("{\"type\":\"cache_frame\",\"index\":-1,\"transforms\":[[21,22,23,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}")));
    Payload.Reset();
    TestTrue(TEXT("negative index reports the stable index error"), Primary && ReceivePacket(
        *Primary, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("negative index uses CACHE_FRAME_INDEX_INVALID without closing"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"error\""))
        && FromUtf8(Payload).Contains(TEXT("CACHE_FRAME_INDEX_INVALID")));

    // A declared payload_size below the actual uploaded bytes is rejected
    // atomically while keeping the same streaming session usable.
    TestTrue(TEXT("underdeclared cache_begin is sent"),
        SendLine(TEXT("{\"type\":\"cache_begin\",\"upload_id\":2,\"revision\":9,\"fps\":30,\"start_frame\":2001,\"end_frame\":2002,\"frame_count\":2,\"payload_size\":64}")));
    TestTrue(TEXT("frame exceeding the declared size is sent"),
        SendLine(TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[61,62,63,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}")));
    Payload.Reset();
    TestTrue(TEXT("low-declared-size upload reports CACHE_PAYLOAD_TOO_LARGE"),
        Primary && ReceivePacket(*Primary, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("size violation uses the stable resource code"),
        FromUtf8(Payload).Contains(TEXT("CACHE_PAYLOAD_TOO_LARGE")));

    TestTrue(TEXT("valid upload begins after the rejected attempt"),
        SendLine(TEXT("{\"type\":\"cache_begin\",\"upload_id\":3,\"revision\":9,\"fps\":30,\"start_frame\":2001,\"end_frame\":2002,\"frame_count\":2,\"payload_size\":512}"))
        && SendLine(TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[21,22,23,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendLine(TEXT("{\"type\":\"cache_frame\",\"index\":1,\"transforms\":[[31,32,33,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendLine(TEXT("{\"type\":\"cache_end\"}")));

    Payload.Reset();
    TestTrue(TEXT("complete upload produces identity-matched cache_ready"), Primary && ReceivePacket(
        *Primary, Payload, [&]() { Source->Update(); }));
    {
        const FString ReadyText = FromUtf8(Payload);
        TestTrue(TEXT("cache_ready echoes upload identity, revision and count"),
            ReadyText.Contains(TEXT("\"type\":\"cache_ready\""))
            && ReadyText.Contains(TEXT("\"upload_id\":3"))
            && ReadyText.Contains(TEXT("\"revision\":9"))
            && ReadyText.Contains(TEXT("\"frame_count\":2")));
    }

    // A recoverably rejected begin switches intake to its own poisoned
    // identity. Frames/end already pipelined behind it are discarded without
    // touching the ready upload or producing an error flood.
    TArray<uint8> RejectedBeginPipeline = Packet(TEXT(
        "{\"type\":\"cache_begin\",\"upload_id\":4,\"revision\":9,\"fps\":0,"
        "\"start_frame\":1,\"end_frame\":1,\"frame_count\":1,\"payload_size\":256}"));
    RejectedBeginPipeline.Append(Packet(TEXT(
        "{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[71,72,73,0,0,0,1,1,1,1],"
        "[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}")));
    RejectedBeginPipeline.Append(Packet(TEXT("{\"type\":\"cache_end\"}")));
    TestTrue(TEXT("rejected begin and its pipelined upload are sent together"),
        Primary && SendBytes(
            *Primary, RejectedBeginPipeline.GetData(), RejectedBeginPipeline.Num()));
    Payload.Reset();
    TestTrue(TEXT("rejected begin reports one recoverable metadata error"),
        Primary && ReceivePacket(*Primary, Payload));
    const FString RejectedBeginError = FromUtf8(Payload);
    TestTrue(TEXT("rejected begin error echoes only its new upload identity"),
        RejectedBeginError.Contains(TEXT("CACHE_METADATA_INVALID"))
        && RejectedBeginError.Contains(TEXT("\"upload_id\":4")));
    FPlatformProcess::Sleep(0.02f);
    Source->Update();
    TestEqual(TEXT("poisoned pipelined frames gain no parsed queue ownership"),
        Source->GetQueuedCacheFrameCount(), 0);
    {
        uint8 Buffer[4096];
        int32 Read = 0;
        const bool bSilent = !Primary->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0;
        TestTrue(TEXT("poisoned frame/end data produces no error flood"), bSilent);
    }

    // While the cache owns the session, live frames must not reach Live Link.
    const TArray<uint8> IntrudingLiveFrame = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[[99,98,97,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"));
    TestTrue(TEXT("intruding live frame is sent during ownership"),
        Primary && SendBytes(*Primary, IntrudingLiveFrame.GetData(), IntrudingLiveFrame.Num()));
    {
        FPlatformProcess::Sleep(0.05f);
        Source->Update();
        // Wire silence is the wire-level guarantee; Live Link keeps older
        // frames evaluable, so the actor-visible proof is that the intruding
        // translation never appears while the cache owns playback.
        uint8 Buffer[4096];
        int32 Read = 0;
        const bool bSilent = !Primary->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0;
        TestTrue(TEXT("intruding live frame produces no reply"), bSilent);
    }

    TestTrue(TEXT("identity-matched cache_play is sent"),
        SendLine(TEXT("{\"type\":\"cache_play\",\"play_id\":1}")));
    bool bFirstCachedPoseApplied = false;
    bool bSecondCachedPoseApplied = false;
    bool bIntruderVisible = false;
    FVector CacheRoot = FVector::ZeroVector;
    FString CompletionPayload;
    PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        if (LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), CachedEvaluated))
        {
            if (const FLiveLinkAnimationFrameData* Animation =
                    CachedEvaluated.FrameData.Cast<FLiveLinkAnimationFrameData>())
            {
                CacheRoot = Animation->Transforms[0].GetTranslation();
                bIntruderVisible |= CacheRoot.Equals(FVector(99.0, 98.0, 97.0));
            }
        }
        bFirstCachedPoseApplied =
            bFirstCachedPoseApplied || CacheRoot.Equals(FVector(21.0, 22.0, 23.0));
        bSecondCachedPoseApplied = CacheRoot.Equals(FVector(31.0, 32.0, 33.0));
        if (bSecondCachedPoseApplied)
        {
            // Drain progress/completion replies arriving for this attempt.
            uint8 Buffer[65536];
            int32 Read = 0;
            while (Primary->Recv(Buffer, sizeof(Buffer), Read) && Read > 0)
            {
                CompletionPayload.Append(FromUtf8(TArray<uint8>(Buffer, Read)));
            }
            return CompletionPayload.Contains(TEXT("\"type\":\"cache_complete\""));
        }
        return false;
    });
    TestTrue(TEXT("first cached pose is applied before any later pose"), bFirstCachedPoseApplied);
    TestTrue(TEXT("second cached pose is applied in order at the captured rate"),
        bSecondCachedPoseApplied);
    TestFalse(TEXT("intruding live pose never becomes actor-visible during playback"),
        bIntruderVisible);
    TestTrue(TEXT("completion carries play identity, applied count and duration"),
        CompletionPayload.Contains(TEXT("\"type\":\"cache_complete\""))
        && CompletionPayload.Contains(TEXT("\"play_id\":1"))
        && CompletionPayload.Contains(TEXT("\"applied_frame_count\":2"))
        && CompletionPayload.Contains(TEXT("elapsed_seconds")));

    // The valid upload after the rejected identity is admitted normally.
    TestTrue(TEXT("new upload after poisoned identity is sent"),
        SendLine(TEXT("{\"type\":\"cache_begin\",\"upload_id\":5,\"revision\":9,\"fps\":30,\"start_frame\":3001,\"end_frame\":3001,\"frame_count\":1,\"payload_size\":256}"))
        && SendLine(TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[51,52,53,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendLine(TEXT("{\"type\":\"cache_end\"}")));
    Payload.Reset();
    TestTrue(TEXT("new upload after poison reaches ready"), Primary && ReceivePacket(
        *Primary, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("ready response belongs to the newer upload"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"cache_ready\""))
        && FromUtf8(Payload).Contains(TEXT("\"upload_id\":5")));

    // Far-surplus cardinalities stay under the framing ceiling but are
    // rejected before parsed transform/curve arrays gain queue ownership.
    FString ExcessTransforms;
    FString ExcessCurves;
    constexpr int32 ExcessCount = 4096;
    ExcessTransforms.Reserve(ExcessCount * 24);
    ExcessCurves.Reserve(ExcessCount * 2);
    for (int32 Index = 0; Index < ExcessCount; ++Index)
    {
        if (Index > 0)
        {
            ExcessTransforms += TEXT(",");
            ExcessCurves += TEXT(",");
        }
        ExcessTransforms += TEXT("[0,0,0,0,0,0,1,1,1,1]");
        ExcessCurves += TEXT("0");
    }
    const TArray<uint8> ExcessFrame = Packet(FString::Printf(TEXT(
        "{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[%s],\"curves\":[%s]}"),
        *ExcessTransforms,
        *ExcessCurves));
    TestTrue(TEXT("surplus-count fixture stays below the framing ceiling"),
        ExcessFrame.Num() - 8 < FMtoUProtocol::MaxMessageBytes);
    TestTrue(TEXT("surplus-count upload begin is sent"),
        SendLine(TEXT("{\"type\":\"cache_begin\",\"upload_id\":6,\"revision\":9,\"fps\":30,\"start_frame\":4001,\"end_frame\":4001,\"frame_count\":1,\"payload_size\":1000000}")));
    TestTrue(TEXT("surplus-count cache frame is sent"), Primary && SendBytes(
        *Primary, ExcessFrame.GetData(), ExcessFrame.Num()));
    FPlatformProcess::Sleep(0.05f);
    TestEqual(TEXT("surplus-count frame gains no parsed queue ownership"),
        Source->GetQueuedCacheFrameCount(), 0);
    Payload.Reset();
    TestTrue(TEXT("surplus-count frame reports a recoverable error"), Primary && ReceivePacket(
        *Primary, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("surplus-count error is stable and identity-scoped"),
        FromUtf8(Payload).Contains(TEXT("CACHE_FRAME_CONTENTS_INVALID"))
        && FromUtf8(Payload).Contains(TEXT("\"upload_id\":6")));

    // Switching back to live preview clears the Unreal buffer first; the
    // cleared outcome arrives before the resumed live pose can be evaluated.
    TestTrue(TEXT("cache_clear is sent"),
        SendLine(TEXT("{\"type\":\"cache_clear\"}")));
    Payload.Reset();
    TestTrue(TEXT("clear is acknowledged"), Primary && ReceivePacket(
        *Primary, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("cleared outcome received"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"cache_cleared\"")));
    const TArray<uint8> AfterClearLive = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[[41,42,43,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"));
    TestTrue(TEXT("live frame after clear is sent"),
        Primary && SendBytes(*Primary, AfterClearLive.GetData(), AfterClearLive.Num()));
    bool bLiveResumed = false;
    PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        FLiveLinkSubjectFrameData ResumedFrame;
        if (LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), ResumedFrame))
        {
            if (const FLiveLinkAnimationFrameData* Animation =
                    ResumedFrame.FrameData.Cast<FLiveLinkAnimationFrameData>())
            {
                bLiveResumed = Animation->Transforms.IsValidIndex(0)
                    && Animation->Transforms[0].GetTranslation().Equals(FVector(41.0, 42.0, 43.0));
            }
        }
        return bLiveResumed;
    });
    TestTrue(TEXT("live sampling resumes after the cached session clears"), bLiveResumed);



    const TArray<uint8> WrongCount = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[],\"curves\":[0.5]}"));
    TestTrue(TEXT("structurally invalid frame is sent"),
        Primary && SendBytes(*Primary, WrongCount.GetData(), WrongCount.Num()));
    TestTrue(TEXT("structural count mismatch closes the session"),
        Primary && WaitForClose(*Primary));

    TestTrue(TEXT("disconnected session returns source to listening"),
        WaitForStatus(Source, TEXT("Listening on")));
    const bool bDisconnectedFrameCleared = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        FLiveLinkSubjectFrameData StaleFrame;
        return !LiveLinkClient.EvaluateFrameFromSource_AnyThread(
            SubjectKey, ULiveLinkAnimationRole::StaticClass(), StaleFrame);
    });
    TestTrue(TEXT("disconnect clears the last streamed pose"), bDisconnectedFrameCleared);
#if WITH_EDITOR
    if (GEditor)
    {
        const FText OverrideName = FText::FromString(TEXT("MtoU Live Link"));
        for (const FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient)
            {
                TestFalse(TEXT("disconnect restores editor viewport realtime setting"),
                    ViewportClient->HasRealtimeOverride(OverrideName));
            }
        }
    }
#endif

    DestroySocket(*SocketSubsystem, Primary);
    Source->StopListener();
    LiveLinkClient.RemoveSource(Source);
    if (World)
    {
        World->DestroyWorld(false);
        if (GEngine)
        {
            GEngine->DestroyWorldContext(World);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUCacheClearRestoresLivePreviewTest,
    "MtoULiveLink.Source.CacheClearRestoresLivePreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCacheClearRestoresLivePreviewTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

    FSocket* Reservation = BindLoopback(*SocketSubsystem, 0, true);
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(*SocketSubsystem, Reservation);

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    TestNotNull(TEXT("editor world is created"), World);
    if (!World)
    {
        return false;
    }
    if (GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = AddBoundActor(*World);
    TestNotNull(TEXT("placed binding actor is created"), Actor);
    // Collect prior tests' destroyed-world actors so this negotiation never
    // sees a stale second binding actor (same pattern as the reconnect test).
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    if (!Actor)
    {
        if (GEngine)
        {
            GEngine->DestroyWorldContext(World);
        }
        World->DestroyWorld(false);
        return false;
    }

    // Save the user's base Realtime preference before negotiation owns any
    // override; with no override present the effective value is the base.
    TArray<TPair<FLevelEditorViewportClient*, bool>> SavedBaseRealtime;
#if WITH_EDITOR
    const FText OverrideName = FText::FromString(TEXT("MtoU Live Link"));
    if (GEditor)
    {
        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient)
            {
                SavedBaseRealtime.Add(TPair<FLevelEditorViewportClient*, bool>(
                    ViewportClient, ViewportClient->IsRealtime()));
            }
        }
    }
#endif

    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    TestTrue(TEXT("source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));

    const USkeletalMesh* TestMesh = Actor
        ? Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
        : nullptr;
    const FReferenceSkeleton* TestSkeleton = TestMesh ? &TestMesh->GetRefSkeleton() : nullptr;
    TestTrue(TEXT("test mesh has the two streamed bones"),
        TestSkeleton && TestSkeleton->GetNum() >= 2);
    const FString RootBoneName = TestSkeleton && TestSkeleton->GetNum() >= 1
        ? TestSkeleton->GetBoneName(0).ToString()
        : TEXT("Bone01");
    const FString ChildBoneName = TestSkeleton && TestSkeleton->GetNum() >= 2
        ? TestSkeleton->GetBoneName(1).ToString()
        : TEXT("Bone02");
    const FString RootBind = TestSkeleton && TestSkeleton->GetNum() >= 1
        ? TransformJson(TestSkeleton->GetRefBonePose()[0])
        : TEXT("[0,0,0,0,0,0,1,1,1,1]");
    const FString ChildBind = TestSkeleton && TestSkeleton->GetNum() >= 2
        ? TransformJson(TestSkeleton->GetRefBonePose()[1])
        : TEXT("[0,0,0,0,0,0,1,1,1,1]");
    const FLiveLinkSubjectKey SubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));

    auto SendText = [&](FSocket* Client, const FString& Text) -> bool
    {
        if (!Client)
        {
            return false;
        }
        const TArray<uint8> Bytes = Packet(Text);
        return SendBytes(*Client, Bytes.GetData(), Bytes.Num());
    };
    auto ReceiveText = [&](FSocket* Client) -> FString
    {
        TArray<uint8> Payload;
        if (!Client || !ReceivePacket(*Client, Payload, [&]() { Source->Update(); }))
        {
            return FString();
        }
        return FromUtf8(Payload);
    };
    auto CountLevelViewports = [&]() -> int32
    {
#if WITH_EDITOR
        int32 Count = 0;
        if (GEditor)
        {
            for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
            {
                if (ViewportClient)
                {
                    ++Count;
                }
            }
        }
        return Count;
#else
        return 0;
#endif
    };
    // Strict host checks: an empty viewport set must never count as proof.
    // Every override assertion below first requires at least one real Level
    // viewport; a viewport-less run fails instead of silently passing.
    auto HasOverride = [&]() -> bool
    {
#if WITH_EDITOR
        if (!GEditor || CountLevelViewports() == 0)
        {
            return false;
        }
        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (!ViewportClient || !ViewportClient->HasRealtimeOverride(OverrideName)
                || !ViewportClient->IsRealtime())
            {
                return false;
            }
        }
#else
        return false;
#endif
        return true;
    };
    auto HasNoOverride = [&]() -> bool
    {
#if WITH_EDITOR
        if (!GEditor || CountLevelViewports() == 0)
        {
            return false;
        }
        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient && ViewportClient->HasRealtimeOverride(OverrideName))
            {
                return false;
            }
        }
#else
        return false;
#endif
        return true;
    };
    auto EffectiveMatchesBase = [&](bool bExpectedBase) -> bool
    {
#if WITH_EDITOR
        if (!GEditor || CountLevelViewports() == 0)
        {
            return false;
        }
        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient && ViewportClient->IsRealtime() != bExpectedBase)
            {
                return false;
            }
        }
#else
        return false;
#endif
        return true;
    };
    auto WaitEvaluatedRoot = [&](const FVector& Expected) -> bool
    {
        return PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            FLiveLinkSubjectFrameData Evaluated;
            if (!LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                    SubjectKey, ULiveLinkAnimationRole::StaticClass(), Evaluated))
            {
                return false;
            }
            const FLiveLinkAnimationFrameData* Animation =
                Evaluated.FrameData.Cast<FLiveLinkAnimationFrameData>();
            return Animation && Animation->Transforms.IsValidIndex(0)
                && Animation->Transforms[0].GetTranslation().Equals(Expected);
        });
    };
    // Visible-chain proof: pump the ordinary Live Link tick plus the component
    // animation tick, then read the displayed skeletal bone. This exercises
    // the LiveLinkInstance -> SkeletalMeshComponent path instead of only
    // proving the subject cache is readable.
    FVector LastDisplayedSeen = FVector::ZeroVector;
    bool bDisplaySeen = false;
    auto WaitDisplayedRoot = [&](const FVector& Expected) -> bool
    {
        const FName RootBoneId(*RootBoneName);
        LastDisplayedSeen = FVector::ZeroVector;
        bDisplaySeen = false;
        return PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            USkeletalMeshComponent* Display =
                Actor ? Actor->GetSkeletalMeshComponent() : nullptr;
            if (!Display || !Display->IsRegistered())
            {
                return false;
            }
            // The transient test world tick does not drive skeletal animation;
            // the explicit component animation tick is the per-frame editor
            // path that evaluates the Live Link instance into bone transforms.
            Display->TickAnimation(1.0f / 60.0f, false);
            Display->RefreshBoneTransforms();
            // World space is what the viewport shows: it follows the stream
            // whether the retarget applies root translation to the bone or
            // carries it as actor root motion.
            const FTransform Displayed =
                Display->GetBoneTransform(RootBoneId, RTS_World);
            LastDisplayedSeen = Displayed.GetTranslation();
            bDisplaySeen = true;
            return LastDisplayedSeen.Equals(Expected, 1.0f);
        });
    };
    auto SendLiveAndExpect = [&](FSocket* Client, const FVector& Root, const TCHAR* What) -> void
    {
        const FString Text = FString::Printf(
            TEXT("{\"type\":\"frame\",\"transforms\":[[%.17g,%.17g,%.17g,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"),
            Root.X, Root.Y, Root.Z);
        TestTrue(What, Client && SendText(Client, Text));
        TestTrue(*(FString(What) + TEXT(" reaches Live Link")), WaitEvaluatedRoot(Root));
        const bool bDisplayed = WaitDisplayedRoot(Root);
        if (!bDisplayed)
        {
            AddError(FString::Printf(TEXT("%s: displayed bone %s, expected %s."),
                What,
                bDisplaySeen ? *LastDisplayedSeen.ToString() : TEXT("<unseen>"),
                *Root.ToString()));
        }
        TestTrue(*(FString(What) + TEXT(" reaches the displayed bone through the editor tick")),
            bDisplayed);
    };

    FSocket* Primary = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("first Maya client connects"), Primary);
    if (!Primary)
    {
        LiveLinkClient.RemoveSource(Source);
        if (World)
        {
            World->DestroyWorld(false);
            if (GEngine)
            {
                GEngine->DestroyWorldContext(World);
            }
        }
        return false;
    }
    const TArray<uint8> Init = Packet(FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"%s\",-1,%s],[\"%s\",0,%s]],\"curves\":[\"Missing\"]}"),
        *RootBoneName,
        *RootBind,
        *ChildBoneName,
        *ChildBind));
    TestTrue(TEXT("init is sent"), SendBytes(*Primary, Init.GetData(), Init.Num()));
    TArray<uint8> Payload;
    TestTrue(TEXT("negotiation reaches ready"), ReceivePacket(
        *Primary, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("ready response is received"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\"")));
    TestTrue(TEXT("connected stream forces editor viewport realtime"), HasOverride());
    TestTrue(TEXT("at least one real level viewport backs the override check"),
        CountLevelViewports() > 0);
    USkeletalMeshComponent* DisplayComponent =
        Actor ? Actor->GetSkeletalMeshComponent() : nullptr;
    TestNotNull(TEXT("binding actor exposes its skeletal mesh component"), DisplayComponent);
    TestTrue(TEXT("editor skeletal mesh component tick is enabled"),
        DisplayComponent && DisplayComponent->IsComponentTickEnabled());
    TestTrue(TEXT("editor skeletal mesh component is registered"),
        DisplayComponent && DisplayComponent->IsRegistered());
    ULiveLinkInstance* DisplayInstance = DisplayComponent
        ? Cast<ULiveLinkInstance>(DisplayComponent->GetAnimInstance())
        : nullptr;
    TestNotNull(TEXT("binding actor owns a Live Link animation instance"), DisplayInstance);
    TestTrue(TEXT("Live Link animation evaluation is enabled"),
        DisplayInstance && DisplayInstance->GetEnableLiveLinkEvaluation());

    // Disable the underlying viewport Realtime so only the plugin override
    // can keep the preview refreshing; the base preference is restored below.
#if WITH_EDITOR
    if (GEditor)
    {
        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient)
            {
                ViewportClient->SetRealtime(false);
            }
        }
    }
#endif
    TestTrue(TEXT("plugin override keeps viewport realtime while base is off"), HasOverride());
    SendLiveAndExpect(Primary, FVector(41.0, 42.0, 43.0), TEXT("initial live frame is sent"));

    // Normal leave and capture cancellation share one path: enter cached
    // ownership, then clear without any upload before live resumes.
    TestTrue(TEXT("cache entry is sent"), SendText(Primary, TEXT("{\"type\":\"cache_enter\"}")));
    FPlatformProcess::Sleep(0.02f);
    Source->Update();
    TestTrue(TEXT("cached entry releases viewport realtime"), HasNoOverride());
    TestTrue(TEXT("cached entry exposes the disabled base realtime"), EffectiveMatchesBase(false));
    TestTrue(TEXT("cache clear is sent"), SendText(Primary, TEXT("{\"type\":\"cache_clear\"}")));
    TestTrue(TEXT("clear is acknowledged"),
        ReceiveText(Primary).Contains(TEXT("\"type\":\"cache_cleared\"")));
    TestTrue(TEXT("leaving cached ownership restores live preview realtime"), HasOverride());
    SendLiveAndExpect(Primary, FVector(51.0, 52.0, 53.0), TEXT("live frame after clear is sent"));

    // Repeated switches stay idempotent and leave no override residue.
    TestTrue(TEXT("second cache entry is sent"), SendText(Primary, TEXT("{\"type\":\"cache_enter\"}")));
    FPlatformProcess::Sleep(0.02f);
    Source->Update();
    TestTrue(TEXT("second entry releases viewport realtime again"), HasNoOverride());
    TestTrue(TEXT("second entry exposes the disabled base realtime again"), EffectiveMatchesBase(false));
    TestTrue(TEXT("second cache clear is sent"), SendText(Primary, TEXT("{\"type\":\"cache_clear\"}")));
    TestTrue(TEXT("second clear is acknowledged"),
        ReceiveText(Primary).Contains(TEXT("\"type\":\"cache_cleared\"")));
    TestTrue(TEXT("repeated clear restores live preview realtime again"), HasOverride());
    SendLiveAndExpect(Primary, FVector(61.0, 62.0, 63.0), TEXT("live frame after second clear is sent"));

    // A recoverable upload validation failure drops to Idle and must restore
    // the same live preview behavior, with the ordered clear keeping the
    // Maya recovery path ahead of resumed live poses.
    TestTrue(TEXT("failure-path cache entry is sent"), SendText(Primary, TEXT("{\"type\":\"cache_enter\"}")));
    FPlatformProcess::Sleep(0.02f);
    Source->Update();
    TestTrue(TEXT("failure-path entry releases viewport realtime"), HasNoOverride());
    TestTrue(TEXT("failure-path entry exposes the disabled base realtime"), EffectiveMatchesBase(false));
    TestTrue(TEXT("mismatched-revision upload is sent"),
        SendText(Primary, TEXT("{\"type\":\"cache_begin\",\"upload_id\":7,\"revision\":8,\"fps\":30,\"start_frame\":2001,\"end_frame\":2001,\"frame_count\":1,\"payload_size\":256}")));
    TestTrue(TEXT("mismatched revision reports a recoverable error"),
        ReceiveText(Primary).Contains(TEXT("CACHE_REVISION_MISMATCH")));
    TestTrue(TEXT("recoverable failure restores live preview realtime"), HasOverride());
    TestTrue(TEXT("recovery clear is sent"), SendText(Primary, TEXT("{\"type\":\"cache_clear\"}")));
    TestTrue(TEXT("recovery clear is acknowledged"),
        ReceiveText(Primary).Contains(TEXT("\"type\":\"cache_cleared\"")));
    TestTrue(TEXT("recovery clear keeps live preview realtime"), HasOverride());
    SendLiveAndExpect(Primary, FVector(71.0, 72.0, 73.0), TEXT("live frame after recovery is sent"));

    // Disconnect under base-off removes only the plugin override and preserves
    // the disabled base. The worker reports Listening immediately, but the
    // game-thread disconnect cleanup runs on Update, so pump it first.
    DestroySocket(*SocketSubsystem, Primary);
    TestTrue(TEXT("disconnected session returns source to listening"),
        WaitForStatus(Source, TEXT("Listening on")));
    TestTrue(TEXT("disconnect cleanup removes the plugin viewport override"), PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            return HasNoOverride();
        }));
    TestTrue(TEXT("disconnect removes the plugin viewport override"), HasNoOverride());
    TestTrue(TEXT("disconnect preserves the disabled base realtime"), EffectiveMatchesBase(false));
    // Initially-on phase on a fresh negotiation of the same listener: raise the
    // base first so this connection starts with Realtime on, then prove the
    // same enter/clear cycle keeps the preview refreshing and the display bone
    // tracking consecutive distinct poses through the normal editor tick.
#if WITH_EDITOR
    if (GEditor)
    {
        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient)
            {
                ViewportClient->SetRealtime(true);
            }
        }
    }
#endif
    FSocket* Reconnect = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("reconnected Maya client connects"), Reconnect);
    if (!Reconnect)
    {
#if WITH_EDITOR
        if (GEditor)
        {
            for (const TPair<FLevelEditorViewportClient*, bool>& Saved : SavedBaseRealtime)
            {
                if (Saved.Key)
                {
                    Saved.Key->SetRealtime(Saved.Value);
                    Saved.Key->RemoveRealtimeOverride(OverrideName, false);
                }
            }
        }
#endif
        LiveLinkClient.RemoveSource(Source);
        if (World)
        {
            World->DestroyWorld(false);
            if (GEngine)
            {
                GEngine->DestroyWorldContext(World);
            }
        }
        return false;
    }
    TestTrue(TEXT("reconnected init is sent"), SendBytes(*Reconnect, Init.GetData(), Init.Num()));
    Payload.Reset();
    TestTrue(TEXT("reconnected negotiation reaches ready"), ReceivePacket(
        *Reconnect, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("reconnected stream forces editor viewport realtime"), HasOverride());
    SendLiveAndExpect(Reconnect, FVector(81.0, 82.0, 83.0), TEXT("initial live frame on base-on is sent"));
    TestTrue(TEXT("base-on cache entry is sent"), SendText(Reconnect, TEXT("{\"type\":\"cache_enter\"}")));
    FPlatformProcess::Sleep(0.02f);
    Source->Update();
    TestTrue(TEXT("base-on entry releases the plugin viewport override"), HasNoOverride());
    TestTrue(TEXT("base-on entry keeps the enabled base realtime"), EffectiveMatchesBase(true));
    TestTrue(TEXT("base-on cache clear is sent"), SendText(Reconnect, TEXT("{\"type\":\"cache_clear\"}")));
    TestTrue(TEXT("base-on clear is acknowledged"),
        ReceiveText(Reconnect).Contains(TEXT("\"type\":\"cache_cleared\"")));
    TestTrue(TEXT("base-on clear restores live preview realtime"), HasOverride());
    SendLiveAndExpect(Reconnect, FVector(91.0, 92.0, 93.0), TEXT("live frame after base-on clear is sent"));
    // Active shutdown while still connected removes the override without
    // touching the enabled base preference.
    Source->StopListener();
    TestTrue(TEXT("active shutdown removes the plugin viewport override"), HasNoOverride());
    TestTrue(TEXT("active shutdown preserves the enabled base realtime"), EffectiveMatchesBase(true));
    DestroySocket(*SocketSubsystem, Reconnect);
    LiveLinkClient.RemoveSource(Source);

    // Restore the user's original viewport Realtime preference.
#if WITH_EDITOR
    if (GEditor)
    {
        for (const TPair<FLevelEditorViewportClient*, bool>& Saved : SavedBaseRealtime)
        {
            if (Saved.Key)
            {
                Saved.Key->SetRealtime(Saved.Value);
                Saved.Key->RemoveRealtimeOverride(OverrideName, false);
            }
        }
    }
#endif
    if (World)
    {
        World->DestroyWorld(false);
        if (GEngine)
        {
            GEngine->DestroyWorldContext(World);
        }
    }
    return true;
}
// Natural-refresh proof for Issue #38. The simple CacheClear test above keeps
// the full override/preference matrix with manually pumped Live Link and
// component animation ticks. This latent test keeps the same cached
// enter/clear and recoverable-error preconditions but proves the actual
// editor refresh: the binding actor lives in the real viewport-driven editor
// world, and every displayed-bone observation below reads
// GetBoneTransform only. The observation lambdas never call
// Source->Update, LiveLinkClient.ForceTick, TickAnimation, or
// RefreshBoneTransforms; between the socket send and the bone read only the
// editor main loop (LiveLinkClient::Tick -> Source::Update plus the natural
// component tick behind the restored realtime override) may advance the
// pose. Control-plane steps (negotiation, cache_enter/clear, error acks)
// still pump Source->Update for determinism; the display proof itself does
// not.
struct FMtoUNaturalRefreshState
{
    ISocketSubsystem* SocketSubsystem = nullptr;
    FSocket* Client = nullptr;
    TSharedPtr<FMtoULiveLinkSource> Source;
    FGuid SourceGuid;
    UWorld* EditorWorld = nullptr;
    AMtoULiveLinkActor* Actor = nullptr;
    FName RootBoneId = NAME_None;
    FString InitPacket;
    TArray<TPair<FLevelEditorViewportClient*, bool>> SavedBase;
    bool bSetupFailed = false;
};

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FMtoUCacheClearNaturalRefreshTest,
    "MtoULiveLink.Source.CacheClearNaturalRefresh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FMtoUCacheClearNaturalRefreshTest::GetTests(
    TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
    OutBeautifiedNames.Add(TEXT("MtoULiveLink.Source.CacheClearNaturalRefresh"));
    OutTestCommands.Add(TEXT(""));
}

bool FMtoUCacheClearNaturalRefreshTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
#if !WITH_EDITOR
    AddError(TEXT("natural refresh test requires an editor build."));
    return false;
#else
    if (!GEditor)
    {
        AddError(TEXT("natural refresh test requires GEditor."));
        return false;
    }
    FMtoUCacheClearNaturalRefreshTest* Self = this;
    TSharedRef<FMtoUNaturalRefreshState> State = MakeShared<FMtoUNaturalRefreshState>();
    const FText OverrideName = FText::FromString(TEXT("MtoU Live Link"));

    auto CountEditorViewports = []() -> int32
    {
        int32 Count = 0;
        if (GEditor)
        {
            for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
            {
                if (ViewportClient)
                {
                    ++Count;
                }
            }
        }
        return Count;
    };
    auto HasOverride = [OverrideName, CountEditorViewports]() -> bool
    {
        if (!GEditor || CountEditorViewports() == 0)
        {
            return false;
        }
        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (!ViewportClient || !ViewportClient->HasRealtimeOverride(OverrideName)
                || !ViewportClient->IsRealtime())
            {
                return false;
            }
        }
        return true;
    };
    auto HasNoOverride = [OverrideName, CountEditorViewports]() -> bool
    {
        if (!GEditor || CountEditorViewports() == 0)
        {
            return false;
        }
        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient && ViewportClient->HasRealtimeOverride(OverrideName))
            {
                return false;
            }
        }
        return true;
    };

    // Setup: negotiate in the real editor world with base Realtime off, then
    // prove cached enter releases the override and clear restores it. This
    // control plane pumps Source->Update for determinism; no displayed-bone
    // conclusion is drawn here.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Self, State, OverrideName, HasOverride, HasNoOverride, CountEditorViewports]()
    {
        ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        Self->TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
        if (!SocketSubsystem)
        {
            State->bSetupFailed = true;
            return true;
        }
        State->SocketSubsystem = SocketSubsystem;
        IModularFeatures& Features = IModularFeatures::Get();
        Self->TestTrue(TEXT("Live Link client feature is available"),
            Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
        if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
        {
            State->bSetupFailed = true;
            return true;
        }
        ILiveLinkClient& LiveLinkClient =
            Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

        UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        Self->TestNotNull(TEXT("real editor world is available"), EditorWorld);
        Self->TestTrue(TEXT("real editor world is an editor world"),
            EditorWorld && EditorWorld->WorldType == EWorldType::Editor);
        if (!EditorWorld || EditorWorld->WorldType != EWorldType::Editor)
        {
            State->bSetupFailed = true;
            return true;
        }
        State->EditorWorld = EditorWorld;
        Self->TestTrue(TEXT("at least one real level viewport backs the override check"),
            CountEditorViewports() > 0);
        if (CountEditorViewports() == 0)
        {
            State->bSetupFailed = true;
            return true;
        }
        int32 ExistingPlaced = 0;
        for (TObjectIterator<AMtoULiveLinkActor> It; It; ++It)
        {
            if (!It->HasAnyFlags(RF_ClassDefaultObject) && IsValid(*It) && It->GetWorld() == EditorWorld)
            {
                ++ExistingPlaced;
            }
        }
        Self->TestEqual(TEXT("editor world starts without a stale binding actor"), ExistingPlaced, 0);
        if (ExistingPlaced != 0)
        {
            State->bSetupFailed = true;
            return true;
        }
        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient)
            {
                State->SavedBase.Add(TPair<FLevelEditorViewportClient*, bool>(
                    ViewportClient, ViewportClient->IsRealtime()));
            }
        }
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
        AMtoULiveLinkActor* Actor = AddBoundActor(*EditorWorld);
        Self->TestNotNull(TEXT("binding actor is placed in the editor world"), Actor);
        if (!Actor)
        {
            State->bSetupFailed = true;
            return true;
        }
        State->Actor = Actor;

        FSocket* Reservation = BindLoopback(*SocketSubsystem, 0, true);
        Self->TestNotNull(TEXT("test port can be reserved"), Reservation);
        if (!Reservation)
        {
            State->bSetupFailed = true;
            return true;
        }
        TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
        Reservation->GetAddress(*ReservedAddress);
        const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
        DestroySocket(*SocketSubsystem, Reservation);

        TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
        State->Source = Source;
        const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
        State->SourceGuid = SourceGuid;
        Self->TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
        Self->TestTrue(TEXT("source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));

        USkeletalMeshComponent* DisplayComponent = Actor->GetSkeletalMeshComponent();
        Self->TestNotNull(TEXT("binding actor exposes its skeletal mesh component"), DisplayComponent);
        const USkeletalMesh* TestMesh = DisplayComponent
            ? DisplayComponent->GetSkeletalMeshAsset()
            : nullptr;
        const FReferenceSkeleton* TestSkeleton = TestMesh ? &TestMesh->GetRefSkeleton() : nullptr;
        Self->TestTrue(TEXT("test mesh has the two streamed bones"),
            TestSkeleton && TestSkeleton->GetNum() >= 2);
        if (!TestSkeleton || TestSkeleton->GetNum() < 2)
        {
            State->bSetupFailed = true;
            return true;
        }
        const FString RootBoneName = TestSkeleton->GetBoneName(0).ToString();
        const FString ChildBoneName = TestSkeleton->GetBoneName(1).ToString();
        State->RootBoneId = TestSkeleton->GetBoneName(0);
        State->InitPacket = FString::Printf(
            TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"%s\",-1,%s],[\"%s\",0,%s]],\"curves\":[\"Missing\"]}"),
            *RootBoneName,
            *TransformJson(TestSkeleton->GetRefBonePose()[0]),
            *ChildBoneName,
            *TransformJson(TestSkeleton->GetRefBonePose()[1]));

        FSocket* Client = ConnectLoopback(*SocketSubsystem, Port);
        Self->TestNotNull(TEXT("Maya client connects"), Client);
        if (!Client)
        {
            State->bSetupFailed = true;
            return true;
        }
        State->Client = Client;
        const TArray<uint8> InitBytes = Packet(State->InitPacket);
        Self->TestTrue(TEXT("init is sent"), SendBytes(*Client, InitBytes.GetData(), InitBytes.Num()));
        TArray<uint8> Payload;
        Self->TestTrue(TEXT("negotiation reaches ready"),
            ReceivePacket(*Client, Payload, [&]() { Source->Update(); }));
        Self->TestTrue(TEXT("ready response is received"),
            FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\"")));
        Self->TestTrue(TEXT("connected stream forces editor viewport realtime"), HasOverride());
        Self->TestTrue(TEXT("editor skeletal mesh component tick is enabled"),
            DisplayComponent && DisplayComponent->IsComponentTickEnabled());
        Self->TestTrue(TEXT("editor skeletal mesh component is registered"),
            DisplayComponent && DisplayComponent->IsRegistered());
        ULiveLinkInstance* DisplayInstance = DisplayComponent
            ? Cast<ULiveLinkInstance>(DisplayComponent->GetAnimInstance())
            : nullptr;
        Self->TestNotNull(TEXT("binding actor owns a Live Link animation instance"), DisplayInstance);
        Self->TestTrue(TEXT("Live Link animation evaluation is enabled"),
            DisplayInstance && DisplayInstance->GetEnableLiveLinkEvaluation());

        for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
        {
            if (ViewportClient)
            {
                ViewportClient->SetRealtime(false);
            }
        }
        Self->TestTrue(TEXT("plugin override keeps viewport realtime while base is off"), HasOverride());

        const TArray<uint8> EnterBytes = Packet(TEXT("{\"type\":\"cache_enter\"}"));
        Self->TestTrue(TEXT("cache entry is sent"),
            SendBytes(*Client, EnterBytes.GetData(), EnterBytes.Num()));
        FPlatformProcess::Sleep(0.02f);
        Source->Update();
        Self->TestTrue(TEXT("cached entry releases viewport realtime"), HasNoOverride());
        const TArray<uint8> ClearBytes = Packet(TEXT("{\"type\":\"cache_clear\"}"));
        Self->TestTrue(TEXT("cache clear is sent"),
            SendBytes(*Client, ClearBytes.GetData(), ClearBytes.Num()));
        TArray<uint8> ClearPayload;
        Self->TestTrue(TEXT("clear is acknowledged"),
            ReceivePacket(*Client, ClearPayload, [&]() { Source->Update(); })
            && FromUtf8(ClearPayload).Contains(TEXT("\"type\":\"cache_cleared\"")));
        Self->TestTrue(TEXT("leaving cached ownership restores live preview realtime"), HasOverride());
        return true;
    }));

    // Natural observation 1: pose after a normal cache clear. The send only
    // writes socket bytes; the wait below reads the displayed bone while the
    // editor main loop advances. No manual pumps here by construction.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Self, State]()
    {
        if (State->bSetupFailed || !State->Client)
        {
            return true;
        }
        const TArray<uint8> Bytes = Packet(
            TEXT("{\"type\":\"frame\",\"transforms\":[[101,102,103,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"));
        Self->TestTrue(TEXT("natural live frame after clear is sent"),
            SendBytes(*State->Client, Bytes.GetData(), Bytes.Num()));
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand(
        [State]() -> bool
        {
            if (State->bSetupFailed || !State->Actor || State->RootBoneId.IsNone())
            {
                return true;
            }
            const USkeletalMeshComponent* Display = State->Actor->GetSkeletalMeshComponent();
            if (!Display || !Display->IsRegistered())
            {
                return false;
            }
            return Display->GetBoneTransform(State->RootBoneId, RTS_World)
                .GetTranslation().Equals(FVector(101.0, 102.0, 103.0), 1.0f);
        },
        [Self, State]() -> bool
        {
            FVector Seen = FVector::ZeroVector;
            bool bSeen = false;
            if (State->Actor && State->Actor->GetSkeletalMeshComponent()
                && State->Actor->GetSkeletalMeshComponent()->IsRegistered()
                && !State->RootBoneId.IsNone())
            {
                Seen = State->Actor->GetSkeletalMeshComponent()
                    ->GetBoneTransform(State->RootBoneId, RTS_World).GetTranslation();
                bSeen = true;
            }
            Self->AddError(FString::Printf(TEXT("natural refresh after clear: displayed bone %s, expected %s."),
                bSeen ? *Seen.ToString() : TEXT("<unseen>"),
                *FVector(101.0, 102.0, 103.0).ToString()));
            return true;
        }, 5.0f));

    // Natural observation 2: a second consecutive distinct pose proves the
    // preview keeps refreshing instead of holding one frame.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Self, State]()
    {
        if (State->bSetupFailed || !State->Client)
        {
            return true;
        }
        const TArray<uint8> Bytes = Packet(
            TEXT("{\"type\":\"frame\",\"transforms\":[[111,112,113,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"));
        Self->TestTrue(TEXT("second natural live frame is sent"),
            SendBytes(*State->Client, Bytes.GetData(), Bytes.Num()));
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand(
        [State]() -> bool
        {
            if (State->bSetupFailed || !State->Actor || State->RootBoneId.IsNone())
            {
                return true;
            }
            const USkeletalMeshComponent* Display = State->Actor->GetSkeletalMeshComponent();
            if (!Display || !Display->IsRegistered())
            {
                return false;
            }
            return Display->GetBoneTransform(State->RootBoneId, RTS_World)
                .GetTranslation().Equals(FVector(111.0, 112.0, 113.0), 1.0f);
        },
        [Self, State]() -> bool
        {
            FVector Seen = FVector::ZeroVector;
            bool bSeen = false;
            if (State->Actor && State->Actor->GetSkeletalMeshComponent()
                && State->Actor->GetSkeletalMeshComponent()->IsRegistered()
                && !State->RootBoneId.IsNone())
            {
                Seen = State->Actor->GetSkeletalMeshComponent()
                    ->GetBoneTransform(State->RootBoneId, RTS_World).GetTranslation();
                bSeen = true;
            }
            Self->AddError(FString::Printf(TEXT("continuous natural refresh: displayed bone %s, expected %s."),
                bSeen ? *Seen.ToString() : TEXT("<unseen>"),
                *FVector(111.0, 112.0, 113.0).ToString()));
            return true;
        }, 5.0f));

    // Recoverable-error precondition (control plane, pumped): a revision
    // mismatch drops to Idle and must restore the same live override before
    // the next natural observation.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Self, State, HasOverride, HasNoOverride]()
    {
        if (State->bSetupFailed || !State->Client || !State->Source.IsValid())
        {
            return true;
        }
        FSocket& Client = *State->Client;
        FMtoULiveLinkSource& Source = *State->Source;
        const TArray<uint8> EnterBytes = Packet(TEXT("{\"type\":\"cache_enter\"}"));
        Self->TestTrue(TEXT("failure-path cache entry is sent"),
            SendBytes(Client, EnterBytes.GetData(), EnterBytes.Num()));
        FPlatformProcess::Sleep(0.02f);
        Source.Update();
        Self->TestTrue(TEXT("failure-path entry releases viewport realtime"), HasNoOverride());
        const TArray<uint8> BadBegin = Packet(
            TEXT("{\"type\":\"cache_begin\",\"upload_id\":7,\"revision\":8,\"fps\":30,\"start_frame\":2001,\"end_frame\":2001,\"frame_count\":1,\"payload_size\":256}"));
        Self->TestTrue(TEXT("mismatched-revision upload is sent"),
            SendBytes(Client, BadBegin.GetData(), BadBegin.Num()));
        TArray<uint8> ErrorPayload;
        Self->TestTrue(TEXT("mismatched revision reports a recoverable error"),
            ReceivePacket(Client, ErrorPayload, [&]() { Source.Update(); })
            && FromUtf8(ErrorPayload).Contains(TEXT("CACHE_REVISION_MISMATCH")));
        Self->TestTrue(TEXT("recoverable failure restores live preview realtime"), HasOverride());
        const TArray<uint8> ClearBytes = Packet(TEXT("{\"type\":\"cache_clear\"}"));
        Self->TestTrue(TEXT("recovery clear is sent"),
            SendBytes(Client, ClearBytes.GetData(), ClearBytes.Num()));
        TArray<uint8> ClearPayload;
        Self->TestTrue(TEXT("recovery clear is acknowledged"),
            ReceivePacket(Client, ClearPayload, [&]() { Source.Update(); })
            && FromUtf8(ClearPayload).Contains(TEXT("\"type\":\"cache_cleared\"")));
        Self->TestTrue(TEXT("recovery clear keeps live preview realtime"), HasOverride());
        return true;
    }));

    // Natural observation 3: pose after the recoverable failure takes the
    // same natural path as the normal clear above.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Self, State]()
    {
        if (State->bSetupFailed || !State->Client)
        {
            return true;
        }
        const TArray<uint8> Bytes = Packet(
            TEXT("{\"type\":\"frame\",\"transforms\":[[121,122,123,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"));
        Self->TestTrue(TEXT("natural live frame after recovery is sent"),
            SendBytes(*State->Client, Bytes.GetData(), Bytes.Num()));
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand(
        [State]() -> bool
        {
            if (State->bSetupFailed || !State->Actor || State->RootBoneId.IsNone())
            {
                return true;
            }
            const USkeletalMeshComponent* Display = State->Actor->GetSkeletalMeshComponent();
            if (!Display || !Display->IsRegistered())
            {
                return false;
            }
            return Display->GetBoneTransform(State->RootBoneId, RTS_World)
                .GetTranslation().Equals(FVector(121.0, 122.0, 123.0), 1.0f);
        },
        [Self, State]() -> bool
        {
            FVector Seen = FVector::ZeroVector;
            bool bSeen = false;
            if (State->Actor && State->Actor->GetSkeletalMeshComponent()
                && State->Actor->GetSkeletalMeshComponent()->IsRegistered()
                && !State->RootBoneId.IsNone())
            {
                Seen = State->Actor->GetSkeletalMeshComponent()
                    ->GetBoneTransform(State->RootBoneId, RTS_World).GetTranslation();
                bSeen = true;
            }
            Self->AddError(FString::Printf(TEXT("natural refresh after recovery: displayed bone %s, expected %s."),
                bSeen ? *Seen.ToString() : TEXT("<unseen>"),
                *FVector(121.0, 122.0, 123.0).ToString()));
            return true;
        }, 5.0f));

    // Cleanup always runs: active shutdown removes only the plugin override,
    // the original base preference is restored, and the editor-world actor
    // is destroyed so later tests never see a second binding actor.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Self, State, OverrideName]()
    {
        if (State->Source.IsValid())
        {
            State->Source->StopListener();
        }
        if (IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName)
            && State->Source.IsValid())
        {
            IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(
                ILiveLinkClient::ModularFeatureName).RemoveSource(State->Source);
        }
        State->Source.Reset();
        if (GEditor)
        {
            for (const TPair<FLevelEditorViewportClient*, bool>& Saved : State->SavedBase)
            {
                if (Saved.Key)
                {
                    Saved.Key->SetRealtime(Saved.Value);
                    Saved.Key->RemoveRealtimeOverride(OverrideName, false);
                }
            }
        }
        if (State->Actor && State->EditorWorld)
        {
            State->EditorWorld->DestroyActor(State->Actor);
            State->Actor = nullptr;
        }
        if (State->SocketSubsystem && State->Client)
        {
            FSocket* Client = State->Client;
            State->Client = nullptr;
            DestroySocket(*State->SocketSubsystem, Client);
        }
        bool bNoOverride = true;
        if (GEditor)
        {
            for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
            {
                if (ViewportClient && ViewportClient->HasRealtimeOverride(OverrideName))
                {
                    bNoOverride = false;
                    break;
                }
            }
        }
        Self->TestTrue(TEXT("cleanup leaves no plugin viewport override"), bNoOverride);
        return true;
    }));
    return true;
#endif
}



IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUCacheSessionReconnectTest,
    "MtoULiveLink.Source.CacheSessionReconnect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCacheSessionReconnectTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

    FSocket* Reservation = BindLoopback(*SocketSubsystem, 0, true);
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(*SocketSubsystem, Reservation);

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    TestNotNull(TEXT("editor world is created"), World);
    if (World && GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = World ? AddBoundActor(*World) : nullptr;
    TestNotNull(TEXT("placed binding actor is created"), Actor);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    TestTrue(TEXT("source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));

    const USkeletalMesh* TestMesh = Actor
        ? Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
        : nullptr;
    const FReferenceSkeleton* TestSkeleton = TestMesh ? &TestMesh->GetRefSkeleton() : nullptr;
    TestTrue(TEXT("test mesh has the two streamed bones"),
        TestSkeleton && TestSkeleton->GetNum() >= 2);
    const FString RootBoneName = TestSkeleton && TestSkeleton->GetNum() >= 1
        ? TestSkeleton->GetBoneName(0).ToString()
        : TEXT("Bone01");
    const FString ChildBoneName = TestSkeleton && TestSkeleton->GetNum() >= 2
        ? TestSkeleton->GetBoneName(1).ToString()
        : TEXT("Bone02");
    const FString RootBind = TestSkeleton && TestSkeleton->GetNum() >= 1
        ? TransformJson(TestSkeleton->GetRefBonePose()[0])
        : TEXT("[0,0,0,0,0,0,1,1,1,1]");
    const FString ChildBind = TestSkeleton && TestSkeleton->GetNum() >= 2
        ? TransformJson(TestSkeleton->GetRefBonePose()[1])
        : TEXT("[0,0,0,0,0,0,1,1,1,1]");
    const TArray<uint8> Init = Packet(FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[[\"%s\",-1,%s],[\"%s\",0,%s]],\"curves\":[\"Missing\"]}"),
        *RootBoneName,
        *RootBind,
        *ChildBoneName,
        *ChildBind));
    const FLiveLinkSubjectKey SubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));

    auto SendText = [&](FSocket* Client, const FString& Text) -> bool
    {
        if (!Client)
        {
            return false;
        }
        const TArray<uint8> Bytes = Packet(Text);
        return SendBytes(*Client, Bytes.GetData(), Bytes.Num());
    };
    auto NegotiateReady = [&](FSocket* Client) -> FString
    {
        if (!Client || !SendBytes(*Client, Init.GetData(), Init.Num()))
        {
            return FString();
        }
        TArray<uint8> Payload;
        if (!ReceivePacket(*Client, Payload, [&]() { Source->Update(); }))
        {
            return FString();
        }
        return FromUtf8(Payload);
    };
    auto ReceiveText = [&](FSocket* Client) -> FString
    {
        TArray<uint8> Payload;
        if (!Client || !ReceivePacket(*Client, Payload, [&]() { Source->Update(); }))
        {
            return FString();
        }
        return FromUtf8(Payload);
    };
    auto WaitListening = [&]() -> bool
    {
        return PollUntil([&]()
        {
            Source->Update();
            return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on"));
        });
    };
    auto WaitNoSubject = [&]() -> bool
    {
        return PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            FLiveLinkSubjectFrameData StaleFrame;
            return !LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), StaleFrame);
        });
    };
    TFunction<void()> AfterFirstApplied;
    auto PlayToCompletion = [&](FSocket* Client, int32 PlayId, const FVector& FirstRoot,
        const FVector& SecondRoot, FString& OutCompletion) -> bool
    {
        OutCompletion.Reset();
        bool bFirstApplied = false;
        bool bSecondApplied = false;
        bool bForeignPose = false;
        FVector ObservedRoot = FVector::ZeroVector;
        const bool bCompleted = PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            FLiveLinkSubjectFrameData Evaluated;
            if (LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                    SubjectKey, ULiveLinkAnimationRole::StaticClass(), Evaluated))
            {
                if (const FLiveLinkAnimationFrameData* Animation =
                        Evaluated.FrameData.Cast<FLiveLinkAnimationFrameData>())
                {
                    if (Animation->Transforms.IsValidIndex(0))
                    {
                        ObservedRoot = Animation->Transforms[0].GetTranslation();
                        bFirstApplied = bFirstApplied || ObservedRoot.Equals(FirstRoot);
                        bSecondApplied = ObservedRoot.Equals(SecondRoot);
                        bForeignPose |= !ObservedRoot.Equals(FirstRoot) && !ObservedRoot.Equals(SecondRoot);
                        if (bFirstApplied && AfterFirstApplied)
                        {
                            TFunction<void()> Deliver = MoveTemp(AfterFirstApplied);
                            AfterFirstApplied = nullptr;
                            Deliver();
                        }
                    }
                }
            }
            if (bSecondApplied && Client)
            {
                uint8 Buffer[65536];
                int32 Read = 0;
                while (Client->Recv(Buffer, sizeof(Buffer), Read) && Read > 0)
                {
                    OutCompletion.Append(FromUtf8(TArray<uint8>(Buffer, Read)));
                }
                return OutCompletion.Contains(TEXT("\"type\":\"cache_complete\""));
            }
            return false;
        });
        return bCompleted && bFirstApplied && bSecondApplied && !bForeignPose
            && !OutCompletion.Contains(TEXT("\"applied\":777"))
            && !OutCompletion.Contains(TEXT("\"applied_frame_count\":777"))
            && !OutCompletion.Contains(TEXT("delayed session A"))
            && !OutCompletion.Contains(TEXT("cache_ready"))
            && OutCompletion.Contains(FString::Printf(TEXT("\"play_id\":%d"), PlayId))
            && OutCompletion.Contains(TEXT("\"applied_frame_count\":2"));
    };

    // Session A on the same listener: complete one upload/play cycle, then
    // leave a partial upload in flight while the transport drops.
    FSocket* Primary = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("first Maya client connects"), Primary);
    TestTrue(TEXT("initial negotiation reaches ready"),
        NegotiateReady(Primary).Contains(TEXT("\"type\":\"ready\"")));
    const uint64 OldSession = FMtoUSessionIsolationTestAccess::Session(*Source);
    TestTrue(TEXT("session A cache entry is sent"),
        SendText(Primary, TEXT("{\"type\":\"cache_enter\"}")));
    FPlatformProcess::Sleep(0.02f);
    Source->Update();
    TestTrue(TEXT("session A upload is sent"),
        SendText(Primary, TEXT("{\"type\":\"cache_begin\",\"upload_id\":1,\"revision\":9,\"fps\":30,\"start_frame\":2001,\"end_frame\":2002,\"frame_count\":2,\"payload_size\":512}"))
        && SendText(Primary, TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[21,22,23,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendText(Primary, TEXT("{\"type\":\"cache_frame\",\"index\":1,\"transforms\":[[31,32,33,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendText(Primary, TEXT("{\"type\":\"cache_end\"}")));
    TestTrue(TEXT("session A upload produces identity-matched ready"),
        ReceiveText(Primary).Contains(TEXT("\"type\":\"cache_ready\"")));
    TestTrue(TEXT("session A play is sent"),
        SendText(Primary, TEXT("{\"type\":\"cache_play\",\"play_id\":1}")));
    {
        FString Completion;
        TestTrue(TEXT("session A play completes with its own applied frames"),
            PlayToCompletion(Primary, 1, FVector(21.0, 22.0, 23.0), FVector(31.0, 32.0, 33.0), Completion));
    }
    TestTrue(TEXT("stale in-flight upload is sent before transport loss"),
        SendText(Primary, TEXT("{\"type\":\"cache_begin\",\"upload_id\":2,\"revision\":9,\"fps\":30,\"start_frame\":2001,\"end_frame\":2002,\"frame_count\":2,\"payload_size\":512}")));
    DestroySocket(*SocketSubsystem, Primary);
    TestTrue(TEXT("transport loss returns source to listening"), WaitListening());
    TestTrue(TEXT("transport loss clears the last streamed pose"), WaitNoSubject());

    // Session B on the same source/listener: the new negotiation must accept
    // its own fresh identities and ignore everything from session A.
    FSocket* Reconnect = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("reconnected Maya client connects"), Reconnect);
    TestTrue(TEXT("second connection negotiates ready on the same listener"),
        NegotiateReady(Reconnect).Contains(TEXT("\"type\":\"ready\"")));
    auto ExpectDelayedIsolation = [&](const TCHAR* Stage)
    {
        FMtoUSessionIsolationTestAccess::DeliverDelayed(*Source, OldSession);
        const double Deadline = FPlatformTime::Seconds() + 0.15;
        bool bUnexpectedReply = false;
        bool bUnexpectedPose = false;
        while (FPlatformTime::Seconds() < Deadline)
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            uint32 PendingBytes = 0;
            bUnexpectedReply |= Reconnect->HasPendingData(PendingBytes) && PendingBytes > 0;
            FLiveLinkSubjectFrameData Frame;
            bUnexpectedPose |= LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), Frame);
            FPlatformProcess::Sleep(0.001f);
        }
        TestFalse(FString::Printf(TEXT("%s: A cannot emit replies on B"), Stage), bUnexpectedReply);
        TestFalse(FString::Printf(TEXT("%s: A cannot publish a pose on B"), Stage), bUnexpectedPose);
        TestTrue(FString::Printf(TEXT("%s: delayed A error cannot close B"), Stage),
            Reconnect->GetConnectionState() == SCS_Connected);
    };
    ExpectDelayedIsolation(TEXT("B before upload"));
    TestTrue(TEXT("reconnected play without upload is sent"),
        SendText(Reconnect, TEXT("{\"type\":\"cache_play\",\"play_id\":1}")));
    TestTrue(TEXT("new session starts without inherited readiness"),
        ReceiveText(Reconnect).Contains(TEXT("CACHE_NOT_READY")));
    TestTrue(TEXT("reconnected cache entry is sent"),
        SendText(Reconnect, TEXT("{\"type\":\"cache_enter\"}")));
    FPlatformProcess::Sleep(0.02f);
    Source->Update();
    TestTrue(TEXT("reconnected upload is sent"),
        SendText(Reconnect, TEXT("{\"type\":\"cache_begin\",\"upload_id\":1,\"revision\":9,\"fps\":30,\"start_frame\":2001,\"end_frame\":2002,\"frame_count\":2,\"payload_size\":512}"))
        && SendText(Reconnect, TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[41,42,43,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendText(Reconnect, TEXT("{\"type\":\"cache_frame\",\"index\":1,\"transforms\":[[51,52,53,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendText(Reconnect, TEXT("{\"type\":\"cache_end\"}")));
    {
        const FString ReadyText = ReceiveText(Reconnect);
        TestTrue(TEXT("reconnected session accepts fresh upload identity"),
            ReadyText.Contains(TEXT("\"type\":\"cache_ready\""))
            && ReadyText.Contains(TEXT("\"upload_id\":1")));
    }
    ExpectDelayedIsolation(TEXT("B ready before play"));
    AfterFirstApplied = [&]()
    {
        FMtoUSessionIsolationTestAccess::DeliverDelayed(*Source, OldSession);
    };
    TestTrue(TEXT("reconnected play is sent"),
        SendText(Reconnect, TEXT("{\"type\":\"cache_play\",\"play_id\":1}")));
    {
        FString Completion;
        TestTrue(TEXT("reconnected session accepts fresh play identity with its own frames"),
            PlayToCompletion(Reconnect, 1, FVector(41.0, 42.0, 43.0), FVector(51.0, 52.0, 53.0), Completion));
    }
    TestTrue(TEXT("reconnected clear is sent"),
        SendText(Reconnect, TEXT("{\"type\":\"cache_clear\"}")));
    TestTrue(TEXT("clear is acknowledged"),
        ReceiveText(Reconnect).Contains(TEXT("\"type\":\"cache_cleared\"")));
    TestTrue(TEXT("duplicate upload after clear is sent"),
        SendText(Reconnect, TEXT("{\"type\":\"cache_begin\",\"upload_id\":1,\"revision\":9,\"fps\":30,\"start_frame\":2001,\"end_frame\":2002,\"frame_count\":2,\"payload_size\":512}")));
    TestTrue(TEXT("same-session stale upload stays rejected after clear"),
        ReceiveText(Reconnect).Contains(TEXT("CACHE_METADATA_INVALID")));
    TestTrue(TEXT("duplicate play after clear is sent"),
        SendText(Reconnect, TEXT("{\"type\":\"cache_play\",\"play_id\":1}")));
    TestTrue(TEXT("same-session stale play stays rejected after clear"),
        ReceiveText(Reconnect).Contains(TEXT("CACHE_METADATA_INVALID")));
    TestTrue(TEXT("increasing upload after stale rejections is sent"),
        SendText(Reconnect, TEXT("{\"type\":\"cache_begin\",\"upload_id\":2,\"revision\":9,\"fps\":30,\"start_frame\":3001,\"end_frame\":3001,\"frame_count\":1,\"payload_size\":256}"))
        && SendText(Reconnect, TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[55,56,57,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendText(Reconnect, TEXT("{\"type\":\"cache_end\"}")));
    TestTrue(TEXT("increasing upload stays usable after stale rejections"),
        ReceiveText(Reconnect).Contains(TEXT("\"upload_id\":2")));
    DestroySocket(*SocketSubsystem, Reconnect);
    TestTrue(TEXT("second transport loss returns source to listening"), WaitListening());
    TestTrue(TEXT("second loss clears the last streamed pose"), WaitNoSubject());

    // Session C proves repeated reconnects stay consistent with no hidden
    // accumulation from either prior session.
    FSocket* Third = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("third Maya client connects"), Third);
    TestTrue(TEXT("third connection negotiates ready on the same listener"),
        NegotiateReady(Third).Contains(TEXT("\"type\":\"ready\"")));
    TestTrue(TEXT("third cache entry is sent"),
        SendText(Third, TEXT("{\"type\":\"cache_enter\"}")));
    FPlatformProcess::Sleep(0.02f);
    Source->Update();
    TestTrue(TEXT("third upload is sent"),
        SendText(Third, TEXT("{\"type\":\"cache_begin\",\"upload_id\":1,\"revision\":9,\"fps\":30,\"start_frame\":2001,\"end_frame\":2002,\"frame_count\":2,\"payload_size\":512}"))
        && SendText(Third, TEXT("{\"type\":\"cache_frame\",\"index\":0,\"transforms\":[[61,62,63,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendText(Third, TEXT("{\"type\":\"cache_frame\",\"index\":1,\"transforms\":[[71,72,73,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0]}"))
        && SendText(Third, TEXT("{\"type\":\"cache_end\"}")));
    {
        const FString ReadyText = ReceiveText(Third);
        TestTrue(TEXT("third session accepts fresh upload identity"),
            ReadyText.Contains(TEXT("\"type\":\"cache_ready\""))
            && ReadyText.Contains(TEXT("\"upload_id\":1")));
    }
    TestTrue(TEXT("third play is sent"),
        SendText(Third, TEXT("{\"type\":\"cache_play\",\"play_id\":1}")));
    {
        FString Completion;
        TestTrue(TEXT("third session accepts fresh play identity with its own frames"),
            PlayToCompletion(Third, 1, FVector(61.0, 62.0, 63.0), FVector(71.0, 72.0, 73.0), Completion));
    }
    DestroySocket(*SocketSubsystem, Third);
    TestTrue(TEXT("final disconnect returns source to listening"), WaitListening());

    Source->StopListener();
    LiveLinkClient.RemoveSource(Source);
    if (World)
    {
        World->DestroyWorld(false);
        if (GEngine)
        {
            GEngine->DestroyWorldContext(World);
        }
    }
    return true;
}

// Explicit opt-in: the ordinary Runtime suite never assumes Maya is installed.
// The peer executes real Maya capture, controller reconnect and retained replay.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUMayaCacheReconnectTest,
    "MtoULiveLink.Source.MayaCacheReconnect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMayaCacheReconnectTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FString Mayapy, Peer, Evidence;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUMayapy="), Mayapy))
    {
        AddInfo(TEXT("Host check not requested; supply MtoUMayapy, MtoUMayaPeer and MtoUEvidence."));
        return true;
    }
    if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUMayaPeer="), Peer)
        || !FParse::Value(FCommandLine::Get(), TEXT("MtoUEvidence="), Evidence)
        || !FPaths::FileExists(Mayapy) || !FPaths::FileExists(Peer))
    {
        AddError(TEXT("Host check requires existing mayapy/peer paths and an evidence directory."));
        return false;
    }
    ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Sockets) { return false; }
    FSocket* Reservation = BindLoopback(*Sockets, 0, true);
    if (!Reservation) { return false; }
    TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
    Reservation->GetAddress(*Address);
    const uint16 Port = static_cast<uint16>(Address->GetPort());
    DestroySocket(*Sockets, Reservation);
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    if (!World) { return false; }
    FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
    Context.SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    AMtoULiveLinkActor* Actor = AddBoundActor(*World);
    TestNotNull(TEXT("real host binding actor"), Actor);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    ILiveLinkClient& Client = IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(
        ILiveLinkClient::ModularFeatureName);
    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid Guid = Client.AddSource(Source);
    const FLiveLinkSubjectKey Key(Guid, FName(TEXT("MtoU_Character")));
    TArray<uint64> Sessions;
    TArray<FVector> Applied;
    TArray<FVector> Evaluated;
    FMtoUSessionIsolationTestAccess::ObserveApplied(*Source,
        [&](uint64 Session, const FMtoUFrameMessage& Frame)
        {
            Sessions.Add(Session);
            Applied.Add(Frame.Transforms[0].Translation);
        });
    const FReferenceSkeleton& Skeleton = Actor->GetSkeletalMeshComponent()
        ->GetSkeletalMeshAsset()->GetRefSkeleton();
    FString Bones;
    for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
    {
        if (Index) { Bones += TEXT(","); }
        Bones += FString::Printf(TEXT("[\"%s\",%d]"),
            *Skeleton.GetBoneName(Index).ToString(), Skeleton.GetParentIndex(Index));
    }
    const FString Fixture = FPaths::Combine(Evidence, TEXT("maya-fixture.json"));
    const FString Result = FPaths::Combine(Evidence, TEXT("maya-result.json"));
    TestTrue(TEXT("write host fixture"), FFileHelper::SaveStringToFile(
        FString::Printf(TEXT("{\"port\":%d,\"bones\":[%s]}"), Port, *Bones), *Fixture));
    const FString Args = FString::Printf(TEXT("\"%s\" --fixture \"%s\" --result \"%s\""),
        *Peer, *Fixture, *Result);
    FProcHandle Process = FPlatformProcess::CreateProc(*Mayapy, *Args, false, true, true,
        nullptr, 0, nullptr, nullptr);
    TestTrue(TEXT("real Maya process starts"), Process.IsValid());
    const double Deadline = FPlatformTime::Seconds() + 120.0;
    while (Process.IsValid() && FPlatformProcess::IsProcRunning(Process)
        && FPlatformTime::Seconds() < Deadline)
    {
        const int32 Before = Applied.Num();
        Source->Update();
        Client.ForceTick();
        if (Applied.Num() > Before)
        {
            FLiveLinkSubjectFrameData Frame;
            if (Client.EvaluateFrameFromSource_AnyThread(Key,
                    ULiveLinkAnimationRole::StaticClass(), Frame))
            {
                const auto* Animation = Frame.FrameData.Cast<FLiveLinkAnimationFrameData>();
                if (Animation && !Animation->Transforms.IsEmpty())
                {
                    Evaluated.Add(Animation->Transforms[0].GetTranslation());
                }
            }
        }
        FPlatformProcess::Sleep(0.001f);
    }
    int32 ExitCode = -1;
    if (Process.IsValid())
    {
        if (FPlatformProcess::IsProcRunning(Process))
        {
            FPlatformProcess::TerminateProc(Process, true);
            AddError(TEXT("Maya host check timed out"));
        }
        FPlatformProcess::GetProcReturnCode(Process, &ExitCode);
        FPlatformProcess::CloseProc(Process);
    }
    TestEqual(TEXT("Maya host assertions and cleanup pass"), ExitCode, 0);
    FString ResultText;
    TSharedPtr<FJsonObject> ResultObject;
    TestTrue(TEXT("Maya result exists"), FFileHelper::LoadFileToString(ResultText, *Result));
    if (JsonObjectFromBytes(Utf8(ResultText), ResultObject))
    {
        TestTrue(TEXT("Maya reports retained-cache recovery"), ResultObject->GetBoolField(TEXT("ok")));
        AddInfo(ResultText);
    }
    else { AddError(TEXT("Invalid Maya result JSON")); }
    TestEqual(TEXT("two real Maya cached cycles publish exactly four frames each"), Applied.Num(), 8);
    TestEqual(TEXT("every cached frame can be evaluated from the Live Link subject"), Evaluated.Num(), 8);
    if (Applied.Num() == 8 && Evaluated.Num() == 8)
    {
        TestTrue(TEXT("same source negotiated a different session"), Sessions[0] != Sessions[4]);
        for (int32 Index = 0; Index < 4; ++Index)
        {
            TestEqual(TEXT("A owns its four frames"), Sessions[Index], Sessions[0]);
            TestEqual(TEXT("B owns its four frames"), Sessions[Index + 4], Sessions[4]);
            TestTrue(TEXT("B applies retained poses despite changed Maya animation"),
                Applied[Index].Equals(Applied[Index + 4]));
            TestTrue(TEXT("B evaluated pose matches A"), Evaluated[Index].Equals(Evaluated[Index + 4]));
            if (Index) { TestFalse(TEXT("animated frames differ"), Applied[Index].Equals(Applied[Index - 1])); }
            AddInfo(FString::Printf(TEXT("Applied frame %d: A session=%llu B session=%llu input=%s evaluated=%s"),
                Index, Sessions[Index], Sessions[Index + 4], *Applied[Index].ToString(),
                *Evaluated[Index].ToString()));
        }
    }
    Source->StopListener();
    Client.RemoveSource(Source);
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUCacheBackpressureSocketTest,
    "MtoULiveLink.Source.CacheBackpressure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCacheBackpressureSocketTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

    FSocket* Reservation = BindLoopback(*SocketSubsystem, 0, true);
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(*SocketSubsystem, Reservation);

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    TestNotNull(TEXT("editor world is created"), World);
    if (World && GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = World ? AddBoundActor(*World) : nullptr;
    TestNotNull(TEXT("placed binding actor is created"), Actor);

    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    TestTrue(TEXT("source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));

    FSocket* Primary = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("Maya client connects"), Primary);
    if (!Primary)
    {
        Source->StopListener();
        LiveLinkClient.RemoveSource(Source);
        return false;
    }
    const FString InitText = TEXT(
        "{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\","
        "\"blendshapes_enabled\":true,\"bones\":[[\"Bone01\",-1,"
        "[0,0,0,0,0,0,1,1,1,1]],[\"Bone02\",0,[0,0,0,0,0,0,1,1,1,1]]],\"curves\":[]}");
    const TArray<uint8> InitBytes = Packet(InitText);
    TestTrue(TEXT("init is sent"),
        SendBytes(*Primary, InitBytes.GetData(), InitBytes.Num()));
    TArray<uint8> Payload;
    TestTrue(TEXT("ready response is received"), Primary && ReceivePacket(
        *Primary, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("connection reached ready"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\"")));

    // The Game Thread is now stalled: no Update() runs. A flood sender thread
    // uploads far more cache frames than the frozen intake budget allows.
    auto SendLine = [&](const FString& Text)
    {
        const TArray<uint8> Bytes = Packet(Text);
        return SendBytes(*Primary, Bytes.GetData(), Bytes.Num());
    };
    TestTrue(TEXT("cache entry is sent"), SendLine(TEXT("{\"type\":\"cache_enter\"}")));
    TestTrue(TEXT("cache begin is sent"), SendLine(TEXT(
        "{\"type\":\"cache_begin\",\"upload_id\":1,\"revision\":9,\"fps\":30,"
        "\"start_frame\":1,\"end_frame\":1000,\"frame_count\":1000,\"payload_size\":2000000}")));
    const int32 FloodCount = static_cast<int32>(FMtoUProtocol::MaxQueuedCacheFrames) * 2;
    TAtomic<bool> bStopSender{false};
    TAtomic<bool> bSenderDone{false};
    Async(EAsyncExecution::Thread, [&, FloodCount]()
    {
        for (int32 Index = 0; Index < FloodCount; ++Index)
        {
            if (bStopSender.Load())
            {
                break;
            }
            const FString FrameText = FString::Printf(TEXT(
                "{\"type\":\"cache_frame\",\"index\":%d,"
                "\"transforms\":[[0,0,0,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[]}"),
                Index);
            const TArray<uint8> Bytes = Packet(FrameText);
            if (!SendBytes(*Primary, Bytes.GetData(), Bytes.Num()) || bStopSender.Load())
            {
                break;
            }
        }
        bSenderDone.Store(true);
    });

    int32 ObservedQueuedPeak = 0;
    const double SampleEnd = FPlatformTime::Seconds() + 0.5;
    while (FPlatformTime::Seconds() < SampleEnd)
    {
        ObservedQueuedPeak = FMath::Max(
            ObservedQueuedPeak, Source->GetQueuedCacheFrameCount());
        FPlatformProcess::Sleep(0.005f);
    }
    TestTrue(TEXT("the stalled Game Thread never lets queued parsed frames"
                  " exceed the frozen intake budget"),
        ObservedQueuedPeak <= static_cast<int32>(FMtoUProtocol::MaxQueuedCacheFrames));
    TestTrue(TEXT("the worker held back real demand (queue actually filled)"),
        ObservedQueuedPeak > 0);
    TestTrue(TEXT("the streaming session is still open during backpressure"),
        Source->GetSourceStatus().ToString().Contains(TEXT("Connected to Maya")));

    // Worker teardown completes while the producer is mid-backpressure.
    const double TeardownStart = FPlatformTime::Seconds();
    Source->StopListener();
    TestTrue(TEXT("worker teardown does not deadlock during backpressure"),
        FPlatformTime::Seconds() - TeardownStart < 2.0);
    bStopSender.Store(true);
    PollUntil([&]() { return bSenderDone.Load(); });

    DestroySocket(*SocketSubsystem, Primary);
    LiveLinkClient.RemoveSource(Source);
    if (World)
    {
        World->DestroyWorld(false);
        if (GEngine)
        {
            GEngine->DestroyWorldContext(World);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPostProcessIsolationTest,
    "MtoULiveLink.Actor.PostProcessIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPostProcessIsolationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    USkeletalMesh* SourceDriver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UPackage* Package = CreatePackage(TEXT("/Temp/MtoUPostProcessIsolation"));
    USkeletalMesh* Driver = SourceDriver
        ? DuplicateObject<USkeletalMesh>(SourceDriver, Package, TEXT("Driver"))
        : nullptr;
    const TSubclassOf<UAnimInstance> ConflictingClass =
        UMtoULiveLinkConflictingPostProcess::StaticClass();
    if (Driver)
    {
        Driver->SetPostProcessAnimBlueprint(ConflictingClass);
    }
    UWorld* World = UWorld::CreateWorld(
        EWorldType::Editor, false, TEXT("MtoUPostProcessIsolationWorld"), Package, true);
    if (World && GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }

    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = World
        ? NewObject<UMtoULiveLinkBinding>(World)
        : nullptr;
    if (Binding)
    {
        Binding->SkeletalMesh = Driver;
    }
    if (Actor && Binding)
    {
        Actor->SetBinding(Binding);
    }

    AActor* ProductionActor = World ? World->SpawnActor<AActor>() : nullptr;
    USkeletalMeshComponent* ProductionComponent = ProductionActor
        ? NewObject<USkeletalMeshComponent>(ProductionActor, TEXT("ProductionSkeletalMesh"))
        : nullptr;
    if (ProductionActor && ProductionComponent)
    {
        ProductionActor->AddInstanceComponent(ProductionComponent);
        ProductionActor->SetRootComponent(ProductionComponent);
        ProductionComponent->RegisterComponent();
        ProductionComponent->SetSkeletalMeshAsset(Driver);
        ProductionComponent->SetUpdateAnimationInEditor(true);
        ProductionComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint, true);
        ProductionComponent->SetAnimInstanceClass(ULiveLinkInstance::StaticClass());
        ProductionComponent->InitializeAnimScriptInstance();
        ProductionComponent->SetDisablePostProcessBlueprint(false);
    }
    if (Actor)
    {
        Actor->OnConstruction(Actor->GetActorTransform());
    }

    TestNotNull(TEXT("post-process isolation Driver fixture exists"), Driver);
    TestNotNull(TEXT("MtoU actor fixture exists"), Actor);
    TestNotNull(TEXT("ordinary production component fixture exists"), ProductionComponent);
    if (!Driver || !Actor || !ProductionComponent)
    {
        if (World)
        {
            World->DestroyWorld(false);
            if (GEngine)
            {
                GEngine->DestroyWorldContext(World);
            }
        }
        return false;
    }

    TestTrue(TEXT("Driver keeps its deliberately conflicting post-process class"),
        Driver->GetPostProcessAnimBlueprint() == ConflictingClass);
    TestTrue(TEXT("MtoU component disables Driver post-process evaluation"),
        Actor->GetSkeletalMeshComponent()->GetDisablePostProcessBlueprint());
    TestFalse(TEXT("ordinary component keeps post-process evaluation enabled"),
        ProductionComponent->GetDisablePostProcessBlueprint());
    TestTrue(TEXT("ordinary component resolves the Driver post-process class"),
        ProductionComponent->GetPostProcessAnimBPClassToBeUsed() == ConflictingClass);

    GMtoUConflictingPostProcessEvaluations.Reset();
    World->Tick(LEVELTICK_All, 1.0f / 60.0f);
    TestTrue(TEXT("ordinary component evaluates the conflicting post-process"),
        GMtoUConflictingPostProcessEvaluations.GetValue() > 0);

    ProductionComponent->UnregisterComponent();
    GMtoUConflictingPostProcessEvaluations.Reset();
    World->Tick(LEVELTICK_All, 1.0f / 60.0f);
    TestEqual(TEXT("MtoU component skips the conflicting post-process"),
        GMtoUConflictingPostProcessEvaluations.GetValue(), 0);
    TestTrue(TEXT("Driver post-process assignment remains unchanged after MtoU use"),
        Driver->GetPostProcessAnimBlueprint() == ConflictingClass);

    World->DestroyWorld(false);
    if (GEngine)
    {
        GEngine->DestroyWorldContext(World);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUWorkflowNegotiationTest,
    "MtoULiveLink.Workflow.Negotiation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUWorkflowNegotiationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

    FSocket* Reservation = BindLoopback(*SocketSubsystem, 0, true);
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(*SocketSubsystem, Reservation);

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    TestNotNull(TEXT("editor world is created"), World);
    if (World && GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = World ? AddBoundActor(*World) : nullptr;
    TestNotNull(TEXT("placed binding actor is created"), Actor);
    USkeletalMesh* SharedDriver = Actor
        ? Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
        : nullptr;
    USkeletalMesh* TransientDriver = Actor && SharedDriver
        ? DuplicateObject<USkeletalMesh>(SharedDriver, Actor)
        : nullptr;
    if (TransientDriver)
    {
        TransientDriver->ClearFlags(RF_Public | RF_Standalone);
        TransientDriver->SetFlags(RF_Transient);
        Actor->GetBinding()->SkeletalMesh = TransientDriver;
        Actor->SetBinding(Actor->GetBinding());
    }
    USkeletalMeshComponent* SkeletalMeshComponent =
        Actor ? Actor->GetSkeletalMeshComponent() : nullptr;
    USkeletalMesh* VisibleTargetBefore =
        SkeletalMeshComponent ? SkeletalMeshComponent->GetSkeletalMeshAsset() : nullptr;
    TestNotNull(TEXT("animation binding shows its Driver Skeletal Mesh"), VisibleTargetBefore);
    TestTrue(TEXT("workflow negotiation owns transient Driver test data"),
        VisibleTargetBefore
            && VisibleTargetBefore->HasAnyFlags(RF_Transient)
            && VisibleTargetBefore->GetOuter() == Actor);
    TestTrue(TEXT("MtoU display component bypasses Driver post-process animation"),
        SkeletalMeshComponent
        && SkeletalMeshComponent->GetDisablePostProcessBlueprint());

    // Earlier automation worlds are only pending destruction at this point;
    // collect them so global actor discovery sees exactly this test's actor.
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    TestTrue(TEXT("source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));

    auto DriverInitPacket = [&](
        const FString& Workflow,
        bool bBlendshapes,
        const FString& CurvesJson)
    {
        const USkeletalMesh* TestMesh = Actor
            ? Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
            : nullptr;
        const FReferenceSkeleton* TestSkeleton = TestMesh ? &TestMesh->GetRefSkeleton() : nullptr;
        if (!TestSkeleton || TestSkeleton->GetNum() < 2)
        {
            return TArray<uint8>();
        }
        const FString RootBoneName = TestSkeleton->GetBoneName(0).ToString();
        const FString ChildBoneName = TestSkeleton->GetBoneName(1).ToString();
        const FString RootBind = TransformJson(TestSkeleton->GetRefBonePose()[0]);
        const FString ChildBind = TransformJson(TestSkeleton->GetRefBonePose()[1]);
        return Packet(FString::Printf(
            TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"%s\",\"blendshapes_enabled\":%s,\"bones\":[[\"%s\",-1,%s],[\"%s\",0,%s]],\"curves\":%s}"),
            *Workflow,
            bBlendshapes ? TEXT("true") : TEXT("false"),
            *RootBoneName,
            *RootBind,
            *ChildBoneName,
            *ChildBind,
            *CurvesJson));
    };

    FSocket* ModelClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("model client connects"), ModelClient);
    const TArray<uint8> ModelInit = DriverInitPacket(
        FMtoUWorkflows::Model, true, TEXT("[]"));
    TestTrue(TEXT("model init is sent"), ModelClient
        && SendBytes(*ModelClient, ModelInit.GetData(), ModelInit.Num()));
    TArray<uint8> Payload;
    TestTrue(TEXT("model workflow receives a framed rejection"), ModelClient && ReceivePacket(
        *ModelClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("model workflow refuses a missing Generated Preview"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"error\""))
        && FromUtf8(Payload).Contains(TEXT("PREVIEW_NOT_READY")));
    TestTrue(TEXT("model refusal closes the session"), ModelClient && WaitForClose(*ModelClient));
    DestroySocket(*SocketSubsystem, ModelClient);

    TestTrue(TEXT("model rejection leaves no visible target change"),
        SkeletalMeshComponent
        && SkeletalMeshComponent->GetSkeletalMeshAsset() == VisibleTargetBefore);
    TestTrue(TEXT("model rejection keeps the actor disconnected from streaming"),
        Actor && !Actor->GetConnectionStatus().Equals(TEXT("Connected"))
        && !Actor->GetPreviewReadiness().IsUsable());
    TestTrue(TEXT("a refused Model connection leaves readiness unmodified"),
        Actor && Actor->GetPreviewReadiness().State == EMtoUPreviewState::None
        && Actor->GetPreviewReadiness().GeneratedPreview == nullptr);
    TestTrue(TEXT("source returns to listening after model rejection"),
        WaitForStatus(Source, TEXT("Listening on")));

    USkeletalMesh* GeneratedPreview = Actor && VisibleTargetBefore
        ? DuplicateObject<USkeletalMesh>(VisibleTargetBefore, Actor)
        : nullptr;
    if (GeneratedPreview && VisibleTargetBefore)
    {
        GeneratedPreview->ClearFlags(RF_Public | RF_Standalone);
        GeneratedPreview->SetFlags(RF_Transient);
        GeneratedPreview->SetSkeleton(VisibleTargetBefore->GetSkeleton());
        GeneratedPreview->SetRefSkeleton(VisibleTargetBefore->GetRefSkeleton());
        TestTrue(TEXT("test Generated Preview has accepted and non-accepted Morphs"),
            AddUniformMorph(*GeneratedPreview, FName(TEXT("Accepted")), FVector3f(1.0f, 0.0f, 0.0f))
            && AddUniformMorph(*GeneratedPreview, FName(TEXT("Unaccepted")), FVector3f(0.0f, 1.0f, 0.0f))
            && AddUniformMorph(*GeneratedPreview, FName(TEXT("NeverAccepted")), FVector3f(0.0f, 0.0f, 1.0f)));
        TestTrue(TEXT("the original Driver owns a character-only Morph"),
            AddUniformMorph(
                *VisibleTargetBefore, FName(TEXT("DriverOnly")), FVector3f(2.0f, 0.0f, 0.0f)));
        FMtoUPreviewReadinessTestAccess::Begin(*Actor);
        FMtoUPreviewReadinessTestAccess::Commit(
            *Actor, GeneratedPreview, false, TEXT("test preview"), FString(), {0});
    }
    TestTrue(TEXT("test actor owns a ready transient Generated Preview"),
        Actor && Actor->GetPreviewReadiness().IsUsable());
    Actor->ShowGeneratedPreview(false);
    TestTrue(TEXT("Model preview selection records the Generated Preview target"),
        Actor && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
        && Actor->GetPreviewReadiness().IsUsable());
    TArray<USkeletalMeshComponent*> DisplayMeshes;
    Actor->GetComponents(DisplayMeshes);
    USkeletalMeshComponent** DriverDisplayEntry = DisplayMeshes.FindByPredicate(
        [Actor](const USkeletalMeshComponent* Component)
        {
            return Component != Actor->GetSkeletalMeshComponent();
        });
    USkeletalMeshComponent* DriverDisplay =
        DriverDisplayEntry ? *DriverDisplayEntry : nullptr;
    TestTrue(TEXT("Model preview creates the original Driver follower"),
        DriverDisplay && DriverDisplay->GetSkeletalMeshAsset() == VisibleTargetBefore);

    const FLiveLinkSubjectKey ModelSubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));

    FSocket* BoneOnlyClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("bone-only model client connects"), BoneOnlyClient);
    SkeletalMeshComponent->SetMorphTarget(FName(TEXT("Unaccepted")), 0.75f);
    // Real Maya always sends the complete curve manifest even with BS
    // transmission disabled, so mimic that instead of an empty manifest.
    const TArray<uint8> BoneOnlyInit = DriverInitPacket(
        FMtoUWorkflows::Model,
        false,
        TEXT("[\"Accepted\",\"Unaccepted\",\"NeverAccepted\"]"));
    TestTrue(TEXT("bone-only model init is sent"), BoneOnlyClient
        && SendBytes(*BoneOnlyClient, BoneOnlyInit.GetData(), BoneOnlyInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("bone-only model workflow produces ready response"),
        BoneOnlyClient && ReceivePacket(*BoneOnlyClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("bone-only model workflow selects the Generated Preview"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\""))
        && FromUtf8(Payload).Contains(TEXT("\"workflow\":\"model\""))
        && Actor
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == GeneratedPreview);
    SkeletalMeshComponent->SetDisablePostProcessBlueprint(false);
    Actor->OnConstruction(Actor->GetActorTransform());
    TestTrue(TEXT("construction preserves the negotiated Model display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
        && SkeletalMeshComponent->GetSkeletalMeshAsset() == GeneratedPreview
        && SkeletalMeshComponent->GetDisablePostProcessBlueprint());
    SkeletalMeshComponent->SetDisablePostProcessBlueprint(false);
    Actor->PostRegisterAllComponents();
    TestTrue(TEXT("registration preserves the negotiated Model display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
        && SkeletalMeshComponent->GetSkeletalMeshAsset() == GeneratedPreview
        && SkeletalMeshComponent->GetDisablePostProcessBlueprint());
    TestTrue(TEXT("bone-only ready reports zero accepted Morphs despite the full manifest"),
        FromUtf8(Payload).Contains(TEXT("\"accepted_morph_count\":0")));
    TestTrue(TEXT("bone-only result is visibly excluded from model acceptance"),
        Actor && Actor->GetConnectionStatus().Contains(TEXT("not valid for model acceptance")));
    TestEqual(TEXT("bone-only connection clears every Generated Morph"),
        SkeletalMeshComponent->GetMorphTarget(FName(TEXT("Unaccepted"))), 0.0f);

    const TArray<uint8> BoneOnlyFrame = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[[1,2,3,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0.9,0.25,0.75]}"));
    TestTrue(TEXT("bone-only manifest frame is sent"), BoneOnlyClient
        && SendBytes(*BoneOnlyClient, BoneOnlyFrame.GetData(), BoneOnlyFrame.Num()));
    FLiveLinkSubjectFrameData BoneOnlyEvaluatedFrame;
    const bool bBoneOnlyFrameEvaluated = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        return LiveLinkClient.EvaluateFrameFromSource_AnyThread(
            ModelSubjectKey,
            ULiveLinkAnimationRole::StaticClass(),
            BoneOnlyEvaluatedFrame);
    });
    const FLiveLinkSkeletonStaticData* BoneOnlyStatic = bBoneOnlyFrameEvaluated
        ? BoneOnlyEvaluatedFrame.StaticData.Cast<FLiveLinkSkeletonStaticData>()
        : nullptr;
    const FLiveLinkAnimationFrameData* BoneOnlyAnimation = bBoneOnlyFrameEvaluated
        ? BoneOnlyEvaluatedFrame.FrameData.Cast<FLiveLinkAnimationFrameData>()
        : nullptr;
    TestTrue(TEXT("bone-only session keeps StaticData, Ready, and FrameData consistent"),
        BoneOnlyStatic
        && BoneOnlyStatic->PropertyNames.IsEmpty()
        && BoneOnlyAnimation
        && BoneOnlyAnimation->Transforms.Num() == 2
        && !BoneOnlyAnimation->Transforms[0].ContainsNaN()
        && BoneOnlyAnimation->PropertyValues.IsEmpty());
    DestroySocket(*SocketSubsystem, BoneOnlyClient);
    TestTrue(TEXT("source returns to listening after bone-only disconnect"),
        WaitForStatus(Source, TEXT("Listening on")));
    TestTrue(TEXT("bone-only disconnect keeps and displays the Generated Preview"),
        Actor && Actor->GetPreviewReadiness().GeneratedPreview == GeneratedPreview
        && Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == GeneratedPreview);
    TestTrue(TEXT("bone-only comparison is a connection diagnostic outside readiness"),
        Actor && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Ready
        && Actor->GetModelDiagnosticLevel() == EMtoUModelDiagnosticLevel::BoneOnly);

    FSocket* EmptyIntersectionClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("empty-intersection bone-only model client connects"), EmptyIntersectionClient);
    const TArray<uint8> EmptyIntersectionInit = DriverInitPacket(
        FMtoUWorkflows::Model,
        false,
        TEXT("[\"GhostOne\",\"GhostTwo\"]"));
    TestTrue(TEXT("empty-intersection bone-only model init is sent"), EmptyIntersectionClient
        && SendBytes(*EmptyIntersectionClient, EmptyIntersectionInit.GetData(), EmptyIntersectionInit.Num()));
    Payload.Reset();
    const bool bEmptyIntersectionReady = EmptyIntersectionClient && ReceivePacket(
        *EmptyIntersectionClient, Payload, [&]() { Source->Update(); });
    TestTrue(TEXT("disabled BS transmission lets an empty Morph intersection negotiate Ready"),
        bEmptyIntersectionReady
        && FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\""))
        && FromUtf8(Payload).Contains(TEXT("\"workflow\":\"model\""))
        && FromUtf8(Payload).Contains(TEXT("\"accepted_morph_count\":0")));
    TestTrue(TEXT("empty-intersection connection is labelled a Bone-only comparison"),
        Actor && Actor->GetConnectionStatus().Contains(TEXT("bone-only diagnostic"))
        && Actor->GetModelDiagnosticLevel() == EMtoUModelDiagnosticLevel::BoneOnly
        && Actor->GetModelDiagnostics().Contains(TEXT("ORANGE: Bone-only diagnostic")));
    DestroySocket(*SocketSubsystem, EmptyIntersectionClient);
    TestTrue(TEXT("source returns to listening after empty-intersection bone-only disconnect"),
        WaitForStatus(Source, TEXT("Listening on")));

    FSocket* BoneDrivenClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("bone-driven model client connects"), BoneDrivenClient);
    const TArray<uint8> BoneDrivenInit = DriverInitPacket(
        FMtoUWorkflows::Model, true, TEXT("[]"));
    TestTrue(TEXT("bone-driven model init is sent"), BoneDrivenClient
        && SendBytes(*BoneDrivenClient, BoneDrivenInit.GetData(), BoneDrivenInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("bone-driven outfit produces ready response"),
        BoneDrivenClient && ReceivePacket(*BoneDrivenClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("zero-name manifest negotiates Ready with an empty accepted set"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\""))
        && FromUtf8(Payload).Contains(TEXT("\"workflow\":\"model\""))
        && FromUtf8(Payload).Contains(TEXT("\"accepted_morph_count\":0")));
    TestTrue(TEXT("bone-driven outfit is not labelled an invalid diagnostic"),
        Actor
        && Actor->GetConnectionStatus().Equals(TEXT("Connected"))
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Ready
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == GeneratedPreview);
    TestTrue(TEXT("bone-driven diagnostics report the declared-zero manifest"),
        Actor->GetModelDiagnostics().Contains(TEXT("Maya current BlendShape count: 0"))
        && Actor->GetModelDiagnostics().Contains(TEXT("Bone-driven outfit"))
        && Actor->GetModelDiagnosticLevel() == EMtoUModelDiagnosticLevel::Full);
    const TArray<uint8> BoneDrivenFrame = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[[1,2,3,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[]}"));
    TestTrue(TEXT("bone-driven frame is sent"), BoneDrivenClient
        && SendBytes(*BoneDrivenClient, BoneDrivenFrame.GetData(), BoneDrivenFrame.Num()));
    FLiveLinkSubjectFrameData BoneDrivenEvaluatedFrame;
    const bool bBoneDrivenFrameEvaluated = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        return LiveLinkClient.EvaluateFrameFromSource_AnyThread(
            ModelSubjectKey,
            ULiveLinkAnimationRole::StaticClass(),
            BoneDrivenEvaluatedFrame);
    });
    const FLiveLinkSkeletonStaticData* BoneDrivenStatic = bBoneDrivenFrameEvaluated
        ? BoneDrivenEvaluatedFrame.StaticData.Cast<FLiveLinkSkeletonStaticData>()
        : nullptr;
    const FLiveLinkAnimationFrameData* BoneDrivenAnimation = bBoneDrivenFrameEvaluated
        ? BoneDrivenEvaluatedFrame.FrameData.Cast<FLiveLinkAnimationFrameData>()
        : nullptr;
    TestTrue(TEXT("bone-driven session streams bones with no Morph properties"),
        BoneDrivenStatic
        && BoneDrivenStatic->PropertyNames.IsEmpty()
        && BoneDrivenAnimation
        && BoneDrivenAnimation->Transforms.Num() == 2
        && !BoneDrivenAnimation->Transforms[0].ContainsNaN()
        && BoneDrivenAnimation->PropertyValues.IsEmpty());
    const TArray<uint8> SingularFrame = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[[1,2,3,0,0,0,1,0,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[]}"));
    TestTrue(TEXT("singular-parent frame is sent"), BoneDrivenClient
        && SendBytes(*BoneDrivenClient, SingularFrame.GetData(), SingularFrame.Num()));
    Payload.Reset();
    const bool bSingularRejected = BoneDrivenClient && ReceivePacket(
        *BoneDrivenClient, Payload, [&]() { Source->Update(); });
    TestTrue(TEXT("a singular source current transform is rejected before publication"),
        bSingularRejected
        && FromUtf8(Payload).Contains(TEXT("\"type\":\"error\""))
        && FromUtf8(Payload).Contains(TEXT("BIND_POSE_INVALID")));
    TestTrue(TEXT("singular transform rejection closes the session"),
        BoneDrivenClient && WaitForClose(*BoneDrivenClient));
    DestroySocket(*SocketSubsystem, BoneDrivenClient);
    TestTrue(TEXT("source returns to listening after bone-driven disconnect"),
        WaitForStatus(Source, TEXT("Listening on")));

    FSocket* BlendshapeClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("BlendShape-enabled model client connects"), BlendshapeClient);
    const TArray<uint8> BlendshapeInit = DriverInitPacket(
        FMtoUWorkflows::Model, true, TEXT("[\"OtherOutfit\"]"));
    TestTrue(TEXT("BlendShape-enabled model init is sent"), BlendshapeClient
        && SendBytes(*BlendshapeClient, BlendshapeInit.GetData(), BlendshapeInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("BlendShape-enabled model workflow receives rejection"),
        BlendshapeClient
        && ReceivePacket(*BlendshapeClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("a non-empty manifest without intersection remains unavailable with BS enabled"),
        FromUtf8(Payload).Contains(TEXT("PREVIEW_MORPH_MISMATCH")));
    TestTrue(TEXT("BlendShape rejection closes the session"),
        BlendshapeClient && WaitForClose(*BlendshapeClient));
    DestroySocket(*SocketSubsystem, BlendshapeClient);
    TestTrue(TEXT("a rejected Model pairing leaves Preview readiness unchanged"),
        Actor && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Ready
        && Actor->GetPreviewReadiness().IsUsable());
    TestTrue(TEXT("source returns to listening after BlendShape rejection"),
        WaitForStatus(Source, TEXT("Listening on")));

    FSocket* PartialClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("partial Morph model client connects"), PartialClient);
    const TArray<uint8> PartialInit = DriverInitPacket(
        FMtoUWorkflows::Model,
        true,
        TEXT("[\"OtherOutfit\",\"Accepted\",\"Unaccepted\",\"DriverOnly\"]"));
    TestTrue(TEXT("partial Morph model init is sent"), PartialClient
        && SendBytes(*PartialClient, PartialInit.GetData(), PartialInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("partial Morph intersection produces ready response"),
        PartialClient && ReceivePacket(*PartialClient, Payload, [&]() { Source->Update(); }));
    const FString PartialReply = FromUtf8(Payload);
    TestTrue(TEXT("partial ready reports v4 Morph counts and both differences"),
        PartialReply.Contains(TEXT("\"target_morph_count\":4"))
        && PartialReply.Contains(TEXT("\"accepted_morph_count\":3"))
        && PartialReply.Contains(TEXT("OtherOutfit"))
        && PartialReply.Contains(TEXT("NeverAccepted")));
    TestTrue(TEXT("partial Model coverage is a connection diagnostic that keeps readiness Ready"),
        Actor && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Ready
        && Actor->GetModelDiagnosticLevel() == EMtoUModelDiagnosticLevel::Partial
        && Actor->GetConnectionStatus().Contains(TEXT("partial Morph coverage")));
    TestTrue(TEXT("Model diagnostics report all five requested counts"),
        Actor && Actor->GetModelDiagnostics().Contains(TEXT("Maya current BlendShape count: 4"))
        && Actor->GetModelDiagnostics().Contains(TEXT("Model display Morph total: 4"))
        && Actor->GetModelDiagnostics().Contains(TEXT("Accepted count: 3"))
        && Actor->GetModelDiagnostics().Contains(TEXT("Maya-only count: 1"))
        && Actor->GetModelDiagnostics().Contains(TEXT("UE-only count: 1")));
    const TArray<uint8> PartialFrame = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[[1,2,3,0,0,0,1,1,1,1],[0,0,0,0,0,0,1,1,1,1]],\"curves\":[0.9,0.25,0.75,0.6]}"));
    TestTrue(TEXT("partial Model frame is sent"), PartialClient
        && SendBytes(*PartialClient, PartialFrame.GetData(), PartialFrame.Num()));
    FLiveLinkSubjectFrameData PartialEvaluatedFrame;
    const bool bPartialFrameEvaluated = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        return LiveLinkClient.EvaluateFrameFromSource_AnyThread(
            ModelSubjectKey,
            ULiveLinkAnimationRole::StaticClass(),
            PartialEvaluatedFrame);
    });
    const FLiveLinkSkeletonStaticData* PartialStatic = bPartialFrameEvaluated
        ? PartialEvaluatedFrame.StaticData.Cast<FLiveLinkSkeletonStaticData>()
        : nullptr;
    const FLiveLinkAnimationFrameData* PartialAnimation = bPartialFrameEvaluated
        ? PartialEvaluatedFrame.FrameData.Cast<FLiveLinkAnimationFrameData>()
        : nullptr;
    TestTrue(TEXT("another-outfit value is excluded and only accepted values are published"),
        PartialStatic
        && PartialStatic->PropertyNames == TArray<FName>({
            FName(TEXT("Accepted")), FName(TEXT("Unaccepted")), FName(TEXT("DriverOnly"))})
        && PartialAnimation
        && PartialAnimation->Transforms.Num() == 2
        && !PartialAnimation->Transforms[0].ContainsNaN()
        && PartialAnimation->PropertyValues == TArray<float>({0.25f, 0.75f, 0.6f}));
    float PartialAcceptedValue = 0.0f;
    float PartialUnacceptedValue = 0.0f;
    const bool bPreviewValuesApplied = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        World->Tick(LEVELTICK_All, 1.0f / 60.0f);
        return SkeletalMeshComponent->GetCurveValue(
                FName(TEXT("Accepted")), 0.0f, PartialAcceptedValue)
            && FMath::IsNearlyEqual(PartialAcceptedValue, 0.25f)
            && SkeletalMeshComponent->GetCurveValue(
                FName(TEXT("Unaccepted")), 0.0f, PartialUnacceptedValue)
            && FMath::IsNearlyEqual(PartialUnacceptedValue, 0.75f);
    });
    TestTrue(TEXT("bone and multiple accepted Morph values apply to the displayed Preview"),
        bPreviewValuesApplied);
    const bool bDriverValueApplied = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        World->Tick(LEVELTICK_All, 1.0f / 60.0f);
        return DriverDisplay
            && FMath::IsNearlyEqual(
                DriverDisplay->GetMorphTarget(FName(TEXT("DriverOnly"))), 0.6f);
    });
    TestTrue(TEXT("Driver-only Morph value applies to the complete character display"),
        bDriverValueApplied);
    float NeverAcceptedValue = 0.0f;
    TestTrue(TEXT("non-accepted Generated Morph remains zero"),
        !SkeletalMeshComponent->GetCurveValue(
            FName(TEXT("NeverAccepted")), 0.0f, NeverAcceptedValue)
        || FMath::IsNearlyZero(NeverAcceptedValue));
    DestroySocket(*SocketSubsystem, PartialClient);
    TestTrue(TEXT("source returns to listening after partial Model disconnect"),
        WaitForStatus(Source, TEXT("Listening on")));

    FSocket* FullClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("full Morph model client connects"), FullClient);
    const TArray<uint8> FullInit = DriverInitPacket(
        FMtoUWorkflows::Model,
        true,
        TEXT("[\"Accepted\",\"Unaccepted\",\"NeverAccepted\",\"DriverOnly\"]"));
    TestTrue(TEXT("full Morph model init is sent"), FullClient
        && SendBytes(*FullClient, FullInit.GetData(), FullInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("full Morph intersection produces ready response"),
        FullClient && ReceivePacket(*FullClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("full ready reports complete accepted intersection"),
        FromUtf8(Payload).Contains(TEXT("\"target_morph_count\":4"))
        && FromUtf8(Payload).Contains(TEXT("\"accepted_morph_count\":4")));
    DestroySocket(*SocketSubsystem, FullClient);
    TestTrue(TEXT("source returns to listening after full Model disconnect"),
        WaitForStatus(Source, TEXT("Listening on")));

    FSocket* AnimationClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("animation client connects"), AnimationClient);
    const TArray<uint8> AnimationInit = DriverInitPacket(
        FMtoUWorkflows::Animation, true, TEXT("[]"));
    TestTrue(TEXT("animation init is sent"), AnimationClient
        && SendBytes(*AnimationClient, AnimationInit.GetData(), AnimationInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("animation workflow produces ready response"), AnimationClient && ReceivePacket(
        *AnimationClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("animation workflow selects the Driver Skeletal Mesh"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\""))
        && FromUtf8(Payload).Contains(TEXT("\"workflow\":\"animation\""))
        && FromUtf8(Payload).Contains(TEXT("\"target_morph_count\":1")));
    TestTrue(TEXT("animation connection marks the actor connected"),
        Actor && Actor->GetConnectionStatus().Equals(TEXT("Connected")));
    TestTrue(TEXT("Animation workflow restores the Driver Skeletal Mesh"),
        SkeletalMeshComponent
        && SkeletalMeshComponent->GetSkeletalMeshAsset() == VisibleTargetBefore);
    TestTrue(TEXT("Animation display selection leaves the ready Generated Preview intact"),
        Actor && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Ready
        && Actor->GetPreviewReadiness().IsUsable());

    SkeletalMeshComponent->SetDisablePostProcessBlueprint(false);
    Actor->OnConstruction(Actor->GetActorTransform());
    TestTrue(TEXT("construction preserves the negotiated Animation Driver target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && SkeletalMeshComponent->GetSkeletalMeshAsset() == VisibleTargetBefore
        && SkeletalMeshComponent->GetDisablePostProcessBlueprint());
    SkeletalMeshComponent->SetDisablePostProcessBlueprint(false);
    Actor->PostRegisterAllComponents();
    TestTrue(TEXT("registration preserves the negotiated Animation Driver target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && SkeletalMeshComponent->GetSkeletalMeshAsset() == VisibleTargetBefore
        && SkeletalMeshComponent->GetDisablePostProcessBlueprint());

    DestroySocket(*SocketSubsystem, AnimationClient);
    TestTrue(TEXT("source returns to listening after Animation disconnect"),
        WaitForStatus(Source, TEXT("Listening on")));
    TestTrue(TEXT("Animation disconnect preserves the Driver display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && SkeletalMeshComponent->GetSkeletalMeshAsset() == VisibleTargetBefore
        && SkeletalMeshComponent->GetDisablePostProcessBlueprint());
    Source->StopListener();
    LiveLinkClient.RemoveSource(Source);
    if (World)
    {
        World->DestroyWorld(false);
        if (GEngine)
        {
            GEngine->DestroyWorldContext(World);
        }
    }
    return true;
}

namespace
{
USkeletalMesh* MakeTransientGeneratedPreview(USkeletalMesh* Template, AActor& Owner)
{
    USkeletalMesh* Mesh = Template
        ? DuplicateObject<USkeletalMesh>(Template, &Owner)
        : nullptr;
    if (Mesh)
    {
        Mesh->ClearFlags(RF_Public | RF_Standalone);
        Mesh->SetFlags(RF_Transient);
    }
    return Mesh;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewReadinessTest,
    "MtoULiveLink.Preview.Readiness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewReadinessTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    USkeletalMesh* Driver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    if (!Actor || !Driver)
    {
        AddError(TEXT("readiness fixtures were not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(GetTransientPackage());
    Binding->SkeletalMesh = Driver;

    TestTrue(TEXT("an actor without a Binding reads None"),
        !Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::None);

    Actor->SetBinding(Binding);
    TestTrue(TEXT("a Binding missing the Preview input is None"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::None);
    Binding->PreviewStaticMesh = NewObject<UStaticMesh>(GetTransientPackage());
    Actor->NotifyBindingInputsChanged();
    TestTrue(TEXT("fully configured unrefreshed inputs are Dirty"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && Actor->GetPreviewReadiness().Diagnostics.Contains(TEXT("Run Refresh Preview")));

    TestTrue(TEXT("the seam accepts a refresh start"),
        FMtoUPreviewReadinessTestAccess::Begin(*Actor));
    TestTrue(TEXT("build start enters Building at Preflight and hides the display"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Building
        && Actor->GetPreviewReadiness().Stage == EMtoUPreviewBuildStage::Preflight
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Hidden
        && !Actor->GetPreviewReadiness().IsUsable());

    for (const EMtoUPreviewBuildStage Stage : { EMtoUPreviewBuildStage::GeometryConversion,
            EMtoUPreviewBuildStage::WeightTransfer, EMtoUPreviewBuildStage::SkeletalMeshBuild,
            EMtoUPreviewBuildStage::Validation })
    {
        FMtoUPreviewReadinessTestAccess::SetStage(*Actor, Stage);
        TestTrue(TEXT("the build stage stays observable while Building"),
            Actor->GetPreviewReadiness().Stage == Stage
            && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Building);
    }

    USkeletalMesh* Generated = MakeTransientGeneratedPreview(Driver, *Actor);
    TestTrue(TEXT("a valid commit is accepted while Building"),
        FMtoUPreviewReadinessTestAccess::Commit(
            *Actor, Generated, false, TEXT("ready diagnostics"), TEXT("ready summary")));
    const FMtoUPreviewReadiness Ready = Actor->GetPreviewReadiness();
    TestTrue(TEXT("Ready exposes the complete coherent snapshot"),
        Ready.State == EMtoUPreviewState::Ready
        && Ready.IsUsable()
        && Ready.GeneratedPreview == Generated
        && Ready.Summary == TEXT("ready summary")
        && Ready.Diagnostics == TEXT("ready diagnostics")
        && Ready.Stage == EMtoUPreviewBuildStage::Validation);
    TestTrue(TEXT("the readiness commit does not own display selection"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Hidden
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    FMtoUPreviewReadinessTestAccess::Commit(*Actor,
        MakeTransientGeneratedPreview(Driver, *Actor), true, TEXT("warning diagnostics"));
    TestTrue(TEXT("a Preview quality warning remains usable readiness"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Warning
        && Actor->GetPreviewReadiness().IsUsable());

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    TestFalse(TEXT("a commit of a non-actor-owned mesh is not accepted as ready"),
        FMtoUPreviewReadinessTestAccess::Commit(*Actor,
            NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient),
            false, TEXT("invalid")));
    TestTrue(TEXT("invalid ownership becomes a Validation-stage failure"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
        && Actor->GetPreviewReadiness().Stage == EMtoUPreviewBuildStage::Validation
        && !Actor->GetPreviewReadiness().IsUsable());

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    TestFalse(TEXT("a commit of persistent asset data is not accepted as ready"),
        FMtoUPreviewReadinessTestAccess::Commit(*Actor,
            NewObject<USkeletalMesh>(Actor), false, TEXT("persisted")));
    TestTrue(TEXT("invalid persistence flags become a Validation-stage failure"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
        && Actor->GetPreviewReadiness().Stage == EMtoUPreviewBuildStage::Validation);

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    FMtoUPreviewReadinessTestAccess::SetStage(*Actor, EMtoUPreviewBuildStage::WeightTransfer);
    TestTrue(TEXT("a failure is accepted while Building"),
        FMtoUPreviewReadinessTestAccess::Fail(
            *Actor, EMtoUPreviewBuildStage::WeightTransfer, TEXT("transfer failed")));
    TestTrue(TEXT("a failed refresh establishes Error with the actionable stage and restores the Driver"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
        && Actor->GetPreviewReadiness().Stage == EMtoUPreviewBuildStage::WeightTransfer
        && Actor->GetPreviewReadiness().Diagnostics == TEXT("transfer failed")
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    FMtoUPreviewReadinessTestAccess::Commit(*Actor,
        MakeTransientGeneratedPreview(Driver, *Actor), false, TEXT("ready"));
    TWeakObjectPtr<USkeletalMesh> Deleted = Actor->GetPreviewReadiness().GeneratedPreview;
    Actor->NotifyGeneratedPreviewDeleted();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("explicit deletion leaves a configured Binding Dirty and shows the Driver"),
        !Deleted.IsValid()
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    FMtoUPreviewReadinessTestAccess::Commit(*Actor,
        MakeTransientGeneratedPreview(Driver, *Actor), false, TEXT("ready"));
    Actor->NotifySourceAssetChanged(Binding->PreviewStaticMesh, TEXT("test change"));
    TestTrue(TEXT("changing either Preview input invalidates the current revision"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && !Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetPreviewReadiness().GeneratedPreview == nullptr);

    // A source build completion reentrant to the actor's own synchronous
    // preparation never self-invalidates, while a real rebuild still does.
    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    Binding->PreviewStaticMesh->OnPostMeshBuild().Broadcast(Binding->PreviewStaticMesh);
    TestTrue(TEXT("a reentrant source build during Building is not a new revision"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Building);
    FMtoUPreviewReadinessTestAccess::Commit(*Actor,
        MakeTransientGeneratedPreview(Driver, *Actor), false, TEXT("ready"));
    Binding->PreviewStaticMesh->OnPostMeshBuild().Broadcast(Binding->PreviewStaticMesh);
    TestTrue(TEXT("a source rebuild notification after commit invalidates the revision"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && !Actor->GetPreviewReadiness().IsUsable());

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    FMtoUPreviewReadinessTestAccess::Commit(*Actor,
        MakeTransientGeneratedPreview(Driver, *Actor), false, TEXT("ready"));
    Actor->PostLoad();
    TestTrue(TEXT("level load discards transient Preview data and reports Dirty"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && Actor->GetPreviewReadiness().Diagnostics.Contains(TEXT("Level loaded")));

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    FMtoUPreviewReadinessTestAccess::Commit(*Actor,
        MakeTransientGeneratedPreview(Driver, *Actor), false, TEXT("ready"));
    AMtoULiveLinkActor* Duplicate = DuplicateObject<AMtoULiveLinkActor>(
        Actor, World->GetCurrentLevel());
    TestTrue(TEXT("actor duplication discards transient Preview data and reports Dirty"),
        Duplicate
        && Duplicate->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && !Duplicate->GetPreviewReadiness().IsUsable());
    if (Duplicate)
    {
        Duplicate->Destroy();
    }

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    USkeletalMesh* Retained = MakeTransientGeneratedPreview(Driver, *Actor);
    FMtoUPreviewReadinessTestAccess::Commit(*Actor, Retained, false, TEXT("ready"));
    Actor->ShowGeneratedPreview(false);
    Actor->SetConnectionStatus(TEXT("Disconnected"));
    TestTrue(TEXT("disconnect preserves an unchanged ready Generated Preview for reuse"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Ready
        && Actor->GetPreviewReadiness().GeneratedPreview == Retained);

    TWeakObjectPtr<USkeletalMesh> Shutdown = Retained;
    Actor->NotifyTransientPreviewReleased();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("shutdown release discards transient Preview ownership"),
        !Shutdown.IsValid()
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty);

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    USkeletalMesh* Teardown = MakeTransientGeneratedPreview(Driver, *Actor);
    FMtoUPreviewReadinessTestAccess::Commit(*Actor, Teardown, false, TEXT("ready"));
    TWeakObjectPtr<USkeletalMesh> TeardownWeak = Teardown;
    Actor->Destroy();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("actor teardown releases transient Preview data"), TeardownWeak.IsValid());

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewReadinessGuardsTest,
    "MtoULiveLink.Preview.ReadinessGuards",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewReadinessGuardsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    USkeletalMesh* Driver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    if (!Actor || !Driver)
    {
        AddError(TEXT("readiness guard fixtures were not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(GetTransientPackage());
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = NewObject<UStaticMesh>(GetTransientPackage());
    Actor->SetBinding(Binding);

    {
        FEnsureScope Scope;
        TestFalse(TEXT("a commit outside Building is rejected"),
            FMtoUPreviewReadinessTestAccess::Commit(*Actor,
                MakeTransientGeneratedPreview(Driver, *Actor), false, TEXT("stale commit")));
        TestEqual(TEXT("the rejected commit emits one ensure"), Scope.GetCount(), 1);
    }
    TestTrue(TEXT("the rejected commit does not overwrite Dirty readiness"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && Actor->GetPreviewReadiness().GeneratedPreview == nullptr);

    {
        FEnsureScope Scope;
        TestFalse(TEXT("a failure outside Building is rejected"),
            FMtoUPreviewReadinessTestAccess::Fail(*Actor,
                EMtoUPreviewBuildStage::Preflight, TEXT("stale failure")));
        TestEqual(TEXT("the rejected failure emits one ensure"), Scope.GetCount(), 1);
    }
    TestTrue(TEXT("the rejected failure does not overwrite Dirty readiness"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty);

    {
        FEnsureScope Scope;
        FMtoUPreviewReadinessTestAccess::SetStage(*Actor, EMtoUPreviewBuildStage::Validation);
        TestEqual(TEXT("stage observation outside Building is ignored"),
            static_cast<int32>(Actor->GetPreviewReadiness().Stage),
            static_cast<int32>(EMtoUPreviewBuildStage::None));
        TestEqual(TEXT("the ignored stage change emits one ensure"), Scope.GetCount(), 1);
    }

    FMtoUPreviewReadinessTestAccess::Begin(*Actor);
    FMtoUPreviewReadinessTestAccess::SetStage(*Actor, EMtoUPreviewBuildStage::Validation);
    {
        FEnsureScope Scope;
        FMtoUPreviewReadinessTestAccess::SetStage(*Actor, EMtoUPreviewBuildStage::GeometryConversion);
        TestTrue(TEXT("an out-of-order stage is ignored"),
            Actor->GetPreviewReadiness().Stage == EMtoUPreviewBuildStage::Validation
            && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Building);
        TestEqual(TEXT("the out-of-order stage emits one ensure"), Scope.GetCount(), 1);
    }
    {
        FEnsureScope Scope;
        TestFalse(TEXT("a reentrant refresh start is rejected"),
            FMtoUPreviewReadinessTestAccess::Begin(*Actor));
        TestEqual(TEXT("the reentrant start emits one ensure and preserves Building"),
            Scope.GetCount(), 1);
        TestTrue(TEXT("the rejected start did not release the running build"),
            Actor->GetPreviewReadiness().State == EMtoUPreviewState::Building
            && Actor->GetPreviewReadiness().Stage == EMtoUPreviewBuildStage::Validation);
    }

    USkeletalMesh* Current = MakeTransientGeneratedPreview(Driver, *Actor);
    TestTrue(TEXT("the running build still accepts its commit"),
        FMtoUPreviewReadinessTestAccess::Commit(*Actor, Current, false, TEXT("ready")));
    {
        FEnsureScope Scope;
        TestFalse(TEXT("a stale second commit is rejected"),
            FMtoUPreviewReadinessTestAccess::Commit(*Actor,
                MakeTransientGeneratedPreview(Driver, *Actor), true, TEXT("stale completion")));
        TestEqual(TEXT("the stale commit emits one ensure"), Scope.GetCount(), 1);
    }
    TestTrue(TEXT("the stale completion does not overwrite the successful result"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Ready
        && Actor->GetPreviewReadiness().GeneratedPreview == Current);
    {
        FEnsureScope Scope;
        TestFalse(TEXT("a stale failure after success is rejected"),
            FMtoUPreviewReadinessTestAccess::Fail(*Actor,
                EMtoUPreviewBuildStage::WeightTransfer, TEXT("stale failure")));
        TestEqual(TEXT("the stale failure emits one ensure"), Scope.GetCount(), 1);
    }
    TestTrue(TEXT("the stale failure does not overwrite the successful result"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Ready
        && Actor->GetPreviewReadiness().GeneratedPreview == Current);

    {
        // A real input change during a build wins: the newer revision makes
        // the pending build's completion a stale result.
        FMtoUPreviewReadinessTestAccess::Begin(*Actor);
        Actor->NotifyBindingInputsChanged();
        FEnsureScope Scope;
        TestFalse(TEXT("a delayed commit after invalidation is rejected"),
            FMtoUPreviewReadinessTestAccess::Commit(*Actor,
                MakeTransientGeneratedPreview(Driver, *Actor), false, TEXT("stale")));
        TestEqual(TEXT("the delayed commit emits one ensure"), Scope.GetCount(), 1);
    }
    TestTrue(TEXT("invalidation keeps the newer Dirty revision"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && Actor->GetPreviewReadiness().GeneratedPreview == nullptr);

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUSourceBindRecoveryTest,
    "MtoULiveLink.Source.BindRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSourceBindRecoveryTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    TSharedRef<FMtoULiveLinkSource> First = MakeShared<FMtoULiveLinkSource>(0);
    TestTrue(TEXT("first source reaches listening state"), WaitForStatus(First, TEXT("Listening on")));
    const int32 Port = ListeningPort(First);
    TestTrue(TEXT("first source reports its ephemeral port"), Port > 0 && Port <= MAX_uint16);

    AddExpectedError(
        TEXT("Failed to bind MtoU_LiveLink"), EAutomationExpectedErrorFlags::Contains, 1);
    TSharedRef<FMtoULiveLinkSource> Second =
        MakeShared<FMtoULiveLinkSource>(static_cast<uint16>(Port));
    TestTrue(TEXT("second source reports an actionable bind failure"), WaitForStatus(Second, TEXT("bind")));
    TestTrue(TEXT("first source retains exclusive ownership"), WaitForStatus(First, TEXT("Listening on")));
    TestTrue(TEXT("bind-failed second source remains displayable"), Second->IsSourceStillValid());
    First->StopListener();
    TestTrue(TEXT("second source listens after the port is released"), WaitForStatus(Second, TEXT("Listening on")));
    Second->StopListener();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUSessionTerminationBoundaryTest,
    "MtoULiveLink.Source.SessionTerminationBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSessionTerminationBoundaryTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

    FSocket* Reservation = BindLoopback(*SocketSubsystem, 0, true);
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(*SocketSubsystem, Reservation);

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    TestNotNull(TEXT("editor world is created"), World);
    if (World && GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = World ? AddBoundActor(*World) : nullptr;
    UMtoULiveLinkBinding* Binding = Actor ? Actor->GetBinding() : nullptr;
    TestNotNull(TEXT("placed binding actor is created"), Actor);
    TestNotNull(TEXT("actor exposes its Binding"), Binding);
    USkeletalMesh* Driver = Actor
        ? Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
        : nullptr;
    TestNotNull(TEXT("binding supplies the Driver Skeletal Mesh"), Driver);
    if (Binding)
    {
        Binding->PreviewStaticMesh = NewObject<UStaticMesh>(GetTransientPackage());
    }

    auto MakeReadyRevision = [&](USkeletalMesh*& OutGenerated)
    {
        OutGenerated = Driver ? DuplicateObject<USkeletalMesh>(Driver, Actor) : nullptr;
        if (OutGenerated)
        {
            OutGenerated->ClearFlags(RF_Public | RF_Standalone);
            OutGenerated->SetFlags(RF_Transient);
            OutGenerated->SetSkeleton(Driver->GetSkeleton());
            OutGenerated->SetRefSkeleton(Driver->GetRefSkeleton());
            AddUniformMorph(*OutGenerated, FName(TEXT("Accepted")), FVector3f(1.0f, 0.0f, 0.0f));
            FMtoUPreviewReadinessTestAccess::Begin(*Actor);
            FMtoUPreviewReadinessTestAccess::Commit(
                *Actor, OutGenerated, false, TEXT("test preview"));
        }
    };
    USkeletalMesh* FirstGenerated = nullptr;
    MakeReadyRevision(FirstGenerated);
    TestTrue(TEXT("test actor owns a ready transient Generated Preview"),
        Actor && Actor->GetPreviewReadiness().IsUsable());

    // Earlier automation worlds are only pending destruction at this point;
    // collect them so global actor discovery sees exactly this test's actor.
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    TestTrue(TEXT("source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));

    const FReferenceSkeleton* TestSkeleton = Driver ? &Driver->GetRefSkeleton() : nullptr;
    TestTrue(TEXT("test mesh has at least one bone"), TestSkeleton && TestSkeleton->GetNum() >= 1);
    const TArray<uint8> ModelInit = Packet(FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"model\",\"blendshapes_enabled\":true,\"bones\":[%s],\"curves\":[\"Accepted\"]}"),
        *ReferenceSkeletonBonesJson(*TestSkeleton)));

    auto NegotiateModelReady = [&](FSocket* Client) -> FString
    {
        if (!Client)
        {
            return FString();
        }
        TestTrue(TEXT("model init is sent"),
            SendBytes(*Client, ModelInit.GetData(), ModelInit.Num()));
        TArray<uint8> Payload;
        if (!ReceivePacket(*Client, Payload, [&]() { Source->Update(); }))
        {
            return FString();
        }
        return FromUtf8(Payload);
    };

    FSocket* Primary = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("model client connects"), Primary);
    TestTrue(TEXT("ready Model revision negotiates a streaming session"),
        NegotiateModelReady(Primary).Contains(TEXT("\"type\":\"ready\"")));
    TestTrue(TEXT("connected actor reports the live session"),
        Actor && Actor->GetConnectionStatus().Equals(TEXT("Connected")));

    // ADR-0002 boundary: a relevant reimport of a Preview input ends the
    // active session without implicitly generating a new Preview.
    TestNotNull(TEXT("test Generated Preview exists for the reimport"), FirstGenerated);
    Actor->NotifySourceAssetChanged(Binding->PreviewStaticMesh, TEXT("test reimport"));
    TestTrue(TEXT("reimport ends the negotiated session on the wire"),
        Primary && WaitForClose(*Primary));
    const bool bReturnedToListening = PollUntil([&]()
    {
        Source->Update();
        return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on"));
    });
    TestTrue(TEXT("source returns to listening after the boundary termination"), bReturnedToListening);
    TestTrue(TEXT("terminated actor shows the true disconnected state"),
        Actor && Actor->GetConnectionStatus().Equals(TEXT("Disconnected")));
    const FMtoUPreviewReadiness AfterReimport = Actor->GetPreviewReadiness();
    TestTrue(TEXT("reimport invalidates the previous revision without a new Preview"),
        AfterReimport.State == EMtoUPreviewState::Dirty
        && AfterReimport.GeneratedPreview == nullptr
        && !AfterReimport.IsUsable());

    // A new connection must negotiate against the new revision; the older
    // Character snapshot can never resume streaming into it.
    FSocket* StaleRevisionClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("stale-revision client connects"), StaleRevisionClient);
    TestTrue(TEXT("a new connection on the dirty revision refuses the old snapshot"),
        NegotiateModelReady(StaleRevisionClient).Contains(TEXT("PREVIEW_NOT_READY")));
    TestTrue(TEXT("refused stale revision closes immediately"),
        StaleRevisionClient && WaitForClose(*StaleRevisionClient));
    DestroySocket(*SocketSubsystem, StaleRevisionClient);

    // Binding actor deletion is the same terminal boundary.
    USkeletalMesh* SecondGenerated = nullptr;
    MakeReadyRevision(SecondGenerated);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("an explicit refresh prepares the new revision"),
        Actor && Actor->GetPreviewReadiness().IsUsable());
    FSocket* Second = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("second model client connects"), Second);
    TestTrue(TEXT("refreshed revision negotiates ready again"),
        NegotiateModelReady(Second).Contains(TEXT("\"type\":\"ready\"")));
    const FLiveLinkSubjectKey SubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));
    Actor->Destroy();
    TestTrue(TEXT("actor destruction ends the live session on the wire"),
        Second && WaitForClose(*Second));
    const bool bDestroyedCleanup = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        FLiveLinkSubjectFrameData StaleFrame;
        return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on"))
            && !LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), StaleFrame);
    });
    TestTrue(TEXT("actor destruction leaves no active subject"), bDestroyedCleanup);
    DestroySocket(*SocketSubsystem, Second);

    // Late duplicate requests stay idempotent and never affect a later host.
    MtoURequestStreamingSessionEnd();
    MtoURequestStreamingSessionEnd();
    FSocket* AfterDestroyClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("post-destruction client connects"), AfterDestroyClient);
    TestTrue(TEXT("a deleted Binding actor leaves no streaming target"),
        NegotiateModelReady(AfterDestroyClient).Contains(TEXT("NO_BINDING_ACTOR")));
    DestroySocket(*SocketSubsystem, AfterDestroyClient);

    Source->StopListener();
    LiveLinkClient.RemoveSource(Source);
    if (World)
    {
        World->DestroyWorld(false);
        if (GEngine)
        {
            GEngine->DestroyWorldContext(World);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPieTargetPolicyTest,
    "MtoULiveLink.Source.PieTargetPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPieTargetPolicyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

    FSocket* Reservation = BindLoopback(*SocketSubsystem, 0, true);
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(*SocketSubsystem, Reservation);

    UWorld* EditorWorld = UWorld::CreateWorld(EWorldType::Editor, false);
    TestNotNull(TEXT("editor world is created"), EditorWorld);
    if (EditorWorld && GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(EditorWorld);
        EditorWorld->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* EditorActor = EditorWorld ? AddBoundActor(*EditorWorld) : nullptr;
    TestNotNull(TEXT("editor binding actor is created"), EditorActor);
    // The PIE duplicated world is the same actor a real Play would create.
    UWorld* PieWorld = UWorld::CreateWorld(EWorldType::PIE, false);
    TestNotNull(TEXT("PIE world is created"), PieWorld);
    AMtoULiveLinkActor* PieActor = PieWorld ? AddBoundActor(*PieWorld) : nullptr;
    TestNotNull(TEXT("PIE duplicate binding actor is created"), PieActor);

    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    TestTrue(TEXT("source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));

    USkeletalMesh* Driver = EditorActor
        ? EditorActor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
        : nullptr;
    const FReferenceSkeleton* TestSkeleton = Driver ? &Driver->GetRefSkeleton() : nullptr;
    TestTrue(TEXT("test mesh has at least one bone"), TestSkeleton && TestSkeleton->GetNum() >= 1);
    const TArray<uint8> Init = Packet(FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[%s],\"curves\":[]}"),
        *ReferenceSkeletonBonesJson(*TestSkeleton)));

    // PIE is not supported: its duplicated Binding actor must not be treated
    // as an additional editor target.
    FSocket* Client = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("PIE-era client connects"), Client);
    TestTrue(TEXT("init is sent"), Client && SendBytes(*Client, Init.GetData(), Init.Num()));
    TArray<uint8> Payload;
    TestTrue(TEXT("PIE-era validation client receives a reply"),
        Client && ReceivePacket(*Client, Payload, [&]() { Source->Update(); }));
    const FString PieReply = FromUtf8(Payload);
    TestFalse(TEXT("a duplicated PIE world never produces a multiple-Binding failure"),
        PieReply.Contains(TEXT("MULTIPLE_BINDING_ACTORS")));
    TestTrue(TEXT("the editor Binding actor alone negotiates ready during PIE"),
        PieReply.Contains(TEXT("\"type\":\"ready\"")));
    TestTrue(TEXT("the editor actor is the connected target"),
        EditorActor && EditorActor->GetConnectionStatus().Equals(TEXT("Connected"))
        && PieActor && PieActor->GetConnectionStatus().Equals(TEXT("Disconnected")));
    DestroySocket(*SocketSubsystem, Client);
    TestTrue(TEXT("source returns to listening after the PIE-era session"),
        PollUntil([&]()
        {
            Source->Update();
            return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on"));
        }));

    // Discovery still fails closed for a genuine second editor target.
    AMtoULiveLinkActor* ExtraEditorActor = EditorWorld ? AddBoundActor(*EditorWorld) : nullptr;
    TestNotNull(TEXT("second editor binding actor is created"), ExtraEditorActor);
    FSocket* DuplicateClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("duplicate-target client connects"), DuplicateClient);
    TestTrue(TEXT("duplicate init is sent"), DuplicateClient
        && SendBytes(*DuplicateClient, Init.GetData(), Init.Num()));
    Payload.Reset();
    TestTrue(TEXT("duplicate-target client receives a reply"),
        DuplicateClient && ReceivePacket(
            *DuplicateClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("two editor-world actors still fail as multiple Binding actors"),
        FromUtf8(Payload).Contains(TEXT("MULTIPLE_BINDING_ACTORS")));
    DestroySocket(*SocketSubsystem, DuplicateClient);

    if (ExtraEditorActor)
    {
        ExtraEditorActor->Destroy();
    }
    Source->StopListener();
    LiveLinkClient.RemoveSource(Source);
    if (PieWorld)
    {
        PieWorld->DestroyWorld(false);
    }
    if (EditorWorld)
    {
        EditorWorld->DestroyWorld(false);
        if (GEngine)
        {
            GEngine->DestroyWorldContext(EditorWorld);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUWorldUnloadTerminationTest,
    "MtoULiveLink.Source.WorldUnloadTermination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUWorldUnloadTerminationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

    FSocket* Reservation = BindLoopback(*SocketSubsystem, 0, true);
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(*SocketSubsystem, Reservation);

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    TestNotNull(TEXT("editor world is created"), World);
    if (World && GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = World ? AddBoundActor(*World) : nullptr;
    UMtoULiveLinkBinding* Binding = Actor ? Actor->GetBinding() : nullptr;
    TestNotNull(TEXT("placed binding actor is created"), Actor);
    TestNotNull(TEXT("actor exposes its Binding"), Binding);
    USkeletalMesh* Driver = Actor
        ? Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
        : nullptr;
    TestNotNull(TEXT("binding supplies the Driver Skeletal Mesh"), Driver);
    if (Binding)
    {
        Binding->PreviewStaticMesh = NewObject<UStaticMesh>(GetTransientPackage());
    }

    auto MakeReadyRevision = [&](USkeletalMesh*& OutGenerated)
    {
        OutGenerated = Driver ? DuplicateObject<USkeletalMesh>(Driver, Actor) : nullptr;
        if (OutGenerated)
        {
            OutGenerated->ClearFlags(RF_Public | RF_Standalone);
            OutGenerated->SetFlags(RF_Transient);
            OutGenerated->SetSkeleton(Driver->GetSkeleton());
            OutGenerated->SetRefSkeleton(Driver->GetRefSkeleton());
            AddUniformMorph(*OutGenerated, FName(TEXT("Accepted")), FVector3f(1.0f, 0.0f, 0.0f));
            FMtoUPreviewReadinessTestAccess::Begin(*Actor);
            FMtoUPreviewReadinessTestAccess::Commit(
                *Actor, OutGenerated, false, TEXT("test preview"));
        }
    };
    USkeletalMesh* FirstGenerated = nullptr;
    MakeReadyRevision(FirstGenerated);
    TestTrue(TEXT("test actor owns a ready transient Generated Preview"),
        Actor && Actor->GetPreviewReadiness().IsUsable());

    // Earlier automation worlds are only pending destruction at this point;
    // collect them so global actor discovery sees exactly this test's actor.
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    TestTrue(TEXT("source reaches listening state"), WaitForStatus(Source, TEXT("Listening on")));

    const FReferenceSkeleton* TestSkeleton = Driver ? &Driver->GetRefSkeleton() : nullptr;
    TestTrue(TEXT("test mesh has at least one bone"), TestSkeleton && TestSkeleton->GetNum() >= 1);
    const TArray<uint8> ModelInit = Packet(FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"model\",\"blendshapes_enabled\":true,\"bones\":[%s],\"curves\":[\"Accepted\"]}"),
        *ReferenceSkeletonBonesJson(*TestSkeleton)));

    auto NegotiateModelReady = [&](FSocket* Client) -> FString
    {
        if (!Client)
        {
            return FString();
        }
        TestTrue(TEXT("model init is sent"),
            SendBytes(*Client, ModelInit.GetData(), ModelInit.Num()));
        TArray<uint8> Payload;
        if (!ReceivePacket(*Client, Payload, [&]() { Source->Update(); }))
        {
            return FString();
        }
        return FromUtf8(Payload);
    };

    FSocket* Primary = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("model client connects"), Primary);
    TestTrue(TEXT("ready Model revision negotiates a streaming session"),
        NegotiateModelReady(Primary).Contains(TEXT("\"type\":\"ready\"")));
    TestTrue(TEXT("connected actor reports the live session"),
        Actor && Actor->GetConnectionStatus().Equals(TEXT("Connected")));

    // One streamed reference-pose frame proves the session owns a live
    // Live Link subject, not merely a negotiated socket.
    const int32 BoneCount = TestSkeleton ? TestSkeleton->GetNum() : 0;
    FString Transforms;
    for (int32 Index = 0; Index < BoneCount; ++Index)
    {
        Transforms += (Index > 0 ? TEXT(",") : TEXT(""));
        Transforms += TransformJson(TestSkeleton->GetRefBonePose()[Index]);
    }
    const TArray<uint8> RefPoseFrame = Packet(FString::Printf(
        TEXT("{\"type\":\"frame\",\"transforms\":[%s],\"curves\":[0]}"), *Transforms));
    const FLiveLinkSubjectKey SubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));
    TestTrue(TEXT("live frame is sent"),
        Primary && SendBytes(*Primary, RefPoseFrame.GetData(), RefPoseFrame.Num()));
    const bool bSubjectLive = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        FLiveLinkSubjectFrameData LiveFrame;
        if (!LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), LiveFrame))
        {
            return false;
        }
        const FLiveLinkAnimationFrameData* Animation =
            LiveFrame.FrameData.Cast<FLiveLinkAnimationFrameData>();
        return Animation && Animation->Transforms.Num() == BoneCount;
    });
    TestTrue(TEXT("the active session publishes its Live Link subject"), bSubjectLive);

    // Distinguishes a real peer close from a would-block read: a graceful
    // close signals readable and then reports zero bytes.
    auto IsSocketClosed = [](FSocket* Client) -> bool
    {
        if (!Client)
        {
            return false;
        }
        if (!Client->Wait(
                ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(150)))
        {
            return false;
        }
        uint8 Probe[16];
        int32 Read = 0;
        return !Client->Recv(Probe, UE_ARRAY_COUNT(Probe), Read) && Read == 0;
    };

    FLiveLinkSubjectFrameData KeptFrame;
    // An unrelated editor world cleanup must never end the active session.
    UWorld* UnrelatedWorld = UWorld::CreateWorld(EWorldType::Editor, false);
    TestNotNull(TEXT("unrelated editor world is created"), UnrelatedWorld);
    UnrelatedWorld->DestroyWorld(false);
    TestTrue(TEXT("an unrelated editor world cleanup keeps the live session"),
        !IsSocketClosed(Primary));
    TestTrue(TEXT("the unrelated cleanup leaves the subject in place"),
        LiveLinkClient.EvaluateFrameFromSource_AnyThread(
            SubjectKey, ULiveLinkAnimationRole::StaticClass(), KeptFrame));

    // The real unload boundary: destroying the Editor world that owns the
    // Binding Actor routes through FWorldDelegates::OnWorldCleanup into the
    // shared idempotent termination boundary. Actor::Destroyed() never runs
    // on the DestroyWorld/CleanupWorld path.
    World->DestroyWorld(false);
    TestTrue(TEXT("the world unload ends the live session on the wire"),
        IsSocketClosed(Primary));
    const bool bUnloadCleanup = PollUntil([&]()
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        FLiveLinkSubjectFrameData StaleFrame;
        return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on"))
            && !LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                SubjectKey, ULiveLinkAnimationRole::StaticClass(), StaleFrame);
    });
    TestTrue(TEXT("the world unload leaves no active subject"), bUnloadCleanup);
    TestTrue(TEXT("the unloaded actor shows the true disconnected state"),
        Actor && Actor->GetConnectionStatus().Equals(TEXT("Disconnected")));

    // Late duplicate termination requests stay idempotent, and once the
    // editor reaps the unloaded world the stale actor leaves no target.
    MtoURequestStreamingSessionEnd();
    MtoURequestStreamingSessionEnd();
    if (GEngine)
    {
        GEngine->DestroyWorldContext(World);
    }
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    FSocket* AfterUnloadClient = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("post-unload client connects"), AfterUnloadClient);
    TestTrue(TEXT("an unloaded world leaves no stale streaming target"),
        NegotiateModelReady(AfterUnloadClient).Contains(TEXT("NO_BINDING_ACTOR")));
    DestroySocket(*SocketSubsystem, AfterUnloadClient);

    Source->StopListener();
    LiveLinkClient.RemoveSource(Source);
    return true;
}

#endif
