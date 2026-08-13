#if WITH_DEV_AUTOMATION_TESTS

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"
#include "MtoUConnectionNegotiator.h"
#include "MtoULiveLinkProtocol.h"
#include "MtoULiveLinkSource.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Features/IModularFeatures.h"
#include "HAL/PlatformProcess.h"
#include "ILiveLinkClient.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
TArray<uint8> Utf8(const FString& Text)
{
    FTCHARToUTF8 Converted(*Text);
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    return Bytes;
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

    FMtoUFrameDecoder MaximumDecoder;
    const TArray<uint8> MaximumPrefix = Prefix(static_cast<uint64>(MAX_int32) - 8);
    MaximumDecoder.Append(MaximumPrefix.GetData(), MaximumPrefix.Num());
    TestTrue(TEXT("maximum representable packet waits for its payload"),
        MaximumDecoder.Pop(Payload, Error) == EMtoUDecodeResult::NeedMore);

    FMtoUFrameDecoder OverflowDecoder;
    const TArray<uint8> OverflowPrefix = Prefix(static_cast<uint64>(MAX_int32) - 7);
    OverflowDecoder.Append(OverflowPrefix.GetData(), OverflowPrefix.Num());
    TestTrue(TEXT("packet one byte beyond the int32 container is rejected"),
        OverflowDecoder.Pop(Payload, Error) == EMtoUDecodeResult::Error);
    TestTrue(TEXT("packet length error is actionable"), Error.Contains(TEXT("int32")));
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
        Bones += FString::Printf(TEXT("[\"bone_%d\",%d]"), Index, Index - 1);
    }
    const FString Valid = FString::Printf(
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[%s],\"curves\":[\"Smile\"]}"), *Bones);
    FMtoUInitMessage Message;
    FString Error;

    TestTrue(TEXT("701 parent-first bones are accepted"), FMtoUProtocol::ParseInit(Utf8(Valid), Message, Error));
    TestEqual(TEXT("all bones are retained"), Message.Bones.Num(), 701);
    TestFalse(TEXT("protocol version 1 is rejected"),
        FMtoUProtocol::ParseInit(Utf8(Valid.Replace(TEXT("\"version\":2"), TEXT("\"version\":1"))), Message, Error));
    TestFalse(TEXT("version must have numeric JSON type"),
        FMtoUProtocol::ParseInit(Utf8(Valid.Replace(TEXT("\"version\":2"), TEXT("\"version\":\"2\""))), Message, Error));
    TestTrue(TEXT("duplicate Maya short bone names are retained for Unreal remapping"),
        FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"root\",-1],[\"root\",0]],\"curves\":[]}")), Message, Error));
    TestFalse(TEXT("a second root is rejected"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"root\",-1],[\"other\",-1]],\"curves\":[]}")), Message, Error));
    TestFalse(TEXT("parents must precede children"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"root\",-1],[\"child\",1]],\"curves\":[]}")), Message, Error));

    const FString MarkerJson =
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"@\",-1]],\"curves\":[]}");
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
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"根\",-1]],\"curves\":[\"笑\"]}")), Message, Error));
    TestTrue(TEXT("multibyte bone name is preserved"), Message.Bones[0].Name == FName(TEXT("根")));
    TestTrue(TEXT("multibyte curve name is preserved"), Message.Curves[0] == FName(TEXT("笑")));

    const FString OverlongName = FString::ChrN(NAME_SIZE, TEXT('x'));
    TestFalse(TEXT("overlong bone name is rejected before FName construction"), FMtoUProtocol::ParseInit(Utf8(
        FString::Printf(TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"root\",-1],[\"%s\",0]],\"curves\":[]}"),
            *OverlongName)), Message, Error));
    TestTrue(TEXT("overlong bone diagnostic identifies the limit"), Error.Contains(TEXT("Bone 1"))
        && Error.Contains(TEXT("NAME_SIZE")));
    TestFalse(TEXT("embedded NUL bone name is rejected before truncation"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"root\",-1],[\"bad\\u0000tail\",0]],\"curves\":[]}")),
        Message, Error));
    TestTrue(TEXT("embedded NUL bone diagnostic is actionable"), Error.Contains(TEXT("Bone 1"))
        && Error.Contains(TEXT("U+0000")));
    TestFalse(TEXT("overlong curve name is rejected before FName construction"), FMtoUProtocol::ParseInit(Utf8(
        FString::Printf(TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"root\",-1]],\"curves\":[\"%s\"]}"),
            *OverlongName)), Message, Error));
    TestTrue(TEXT("overlong curve diagnostic identifies the limit"), Error.Contains(TEXT("Curve 0"))
        && Error.Contains(TEXT("NAME_SIZE")));
    TestFalse(TEXT("embedded NUL curve name is rejected before truncation"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"root\",-1]],\"curves\":[\"bad\\u0000tail\"]}")),
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
        {TEXT("hair_7/tip -> tip1")});
    Decoder.Append(Ready.GetData(), Ready.Num());
    TestTrue(TEXT("ready reply is framed"), Decoder.Pop(ReplyPayload, Error) == EMtoUDecodeResult::Message);
    TestTrue(TEXT("ready reply names missing curve"), FromUtf8(ReplyPayload).Contains(TEXT("Blink_R")));
    TestTrue(TEXT("ready reply names Unreal-only morph"),
        FromUtf8(ReplyPayload).Contains(TEXT("Corrective")));
    TestTrue(TEXT("ready reply reports bone name remapping"),
        FromUtf8(ReplyPayload).Contains(TEXT("tip1")));
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
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"Bone01\",-1],[\"Bone02\",0]],\"curves\":[]}"));
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
    const TArray<uint8> Init = Packet(
        TEXT("{\"type\":\"init\",\"version\":2,\"bones\":[[\"Bone01\",-1],[\"Bone02\",0]],\"curves\":[\"Missing\"]}"));
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

    FSocket* Second = ConnectLoopback(*SocketSubsystem, Port);
    TestNotNull(TEXT("second TCP client reaches listener"), Second);
    Payload.Reset();
    TestTrue(TEXT("second client receives a framed rejection"),
        Second && ReceivePacket(*Second, Payload));
    TestTrue(TEXT("second client rejection is actionable"),
        FromUtf8(Payload).Contains(TEXT("already has a Maya client")));
    DestroySocket(*SocketSubsystem, Second);

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

    const FLiveLinkSubjectKey SubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));
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
        return Animation && Animation->Transforms.IsValidIndex(0);
    });
    TestTrue(TEXT("following valid frame is evaluated after the non-finite frame"), bEvaluated);
    if (bEvaluated)
    {
        const FLiveLinkAnimationFrameData* Animation =
            EvaluatedFrame.FrameData.Cast<FLiveLinkAnimationFrameData>();
        TestTrue(TEXT("following valid frame keeps the transmitted root translation"),
            Animation->Transforms[0].GetTranslation().Equals(FVector(1.0, 2.0, 3.0)));
    }

    const TArray<uint8> WrongCount = Packet(
        TEXT("{\"type\":\"frame\",\"transforms\":[],\"curves\":[0.5]}"));
    TestTrue(TEXT("structurally invalid frame is sent"),
        Primary && SendBytes(*Primary, WrongCount.GetData(), WrongCount.Num()));
    TestTrue(TEXT("structural count mismatch closes the session"),
        Primary && WaitForClose(*Primary));

    DestroySocket(*SocketSubsystem, Primary);
    Source->StopListener();
    LiveLinkClient.RemoveSource(Source);
    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUSourceBindErrorTest,
    "MtoULiveLink.Source.BindErrorStatus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSourceBindErrorTest::RunTest(const FString& Parameters)
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
    Second->StopListener();
    First->StopListener();
    return true;
}

#endif
