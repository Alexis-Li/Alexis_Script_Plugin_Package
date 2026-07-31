#if WITH_DEV_AUTOMATION_TESTS

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkProtocol.h"

#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Roles/LiveLinkAnimationTypes.h"

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
        TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[%s],\"curves\":[\"Smile\"]}"), *Bones);
    FMtoUInitMessage Message;
    FString Error;

    TestTrue(TEXT("701 parent-first bones are accepted"), FMtoUProtocol::ParseInit(Utf8(Valid), Message, Error));
    TestEqual(TEXT("all bones are retained"), Message.Bones.Num(), 701);
    TestFalse(TEXT("protocol version 2 is rejected"),
        FMtoUProtocol::ParseInit(Utf8(Valid.Replace(TEXT("\"version\":1"), TEXT("\"version\":2"))), Message, Error));
    TestFalse(TEXT("version must have numeric JSON type"),
        FMtoUProtocol::ParseInit(Utf8(Valid.Replace(TEXT("\"version\":1"), TEXT("\"version\":\"1\""))), Message, Error));
    TestFalse(TEXT("duplicate bone names are rejected"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[[\"root\",-1],[\"root\",0]],\"curves\":[]}")), Message, Error));
    TestFalse(TEXT("a second root is rejected"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[[\"root\",-1],[\"other\",-1]],\"curves\":[]}")), Message, Error));
    TestFalse(TEXT("parents must precede children"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[[\"root\",-1],[\"child\",1]],\"curves\":[]}")), Message, Error));

    const FString MarkerJson =
        TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[[\"@\",-1]],\"curves\":[]}");
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
        TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[[\"根\",-1]],\"curves\":[\"笑\"]}")), Message, Error));
    TestTrue(TEXT("multibyte bone name is preserved"), Message.Bones[0].Name == FName(TEXT("根")));
    TestTrue(TEXT("multibyte curve name is preserved"), Message.Curves[0] == FName(TEXT("笑")));

    const FString OverlongName = FString::ChrN(NAME_SIZE, TEXT('x'));
    TestFalse(TEXT("overlong bone name is rejected before FName construction"), FMtoUProtocol::ParseInit(Utf8(
        FString::Printf(TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[[\"root\",-1],[\"%s\",0]],\"curves\":[]}"),
            *OverlongName)), Message, Error));
    TestTrue(TEXT("overlong bone diagnostic identifies the limit"), Error.Contains(TEXT("Bone 1"))
        && Error.Contains(TEXT("NAME_SIZE")));
    TestFalse(TEXT("embedded NUL bone name is rejected before truncation"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[[\"root\",-1],[\"bad\\u0000tail\",0]],\"curves\":[]}")),
        Message, Error));
    TestTrue(TEXT("embedded NUL bone diagnostic is actionable"), Error.Contains(TEXT("Bone 1"))
        && Error.Contains(TEXT("U+0000")));
    TestFalse(TEXT("overlong curve name is rejected before FName construction"), FMtoUProtocol::ParseInit(Utf8(
        FString::Printf(TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[[\"root\",-1]],\"curves\":[\"%s\"]}"),
            *OverlongName)), Message, Error));
    TestTrue(TEXT("overlong curve diagnostic identifies the limit"), Error.Contains(TEXT("Curve 0"))
        && Error.Contains(TEXT("NAME_SIZE")));
    TestFalse(TEXT("embedded NUL curve name is rejected before truncation"), FMtoUProtocol::ParseInit(Utf8(
        TEXT("{\"type\":\"init\",\"version\":1,\"bones\":[[\"root\",-1]],\"curves\":[\"bad\\u0000tail\"]}")),
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
    TArray<uint8> Ready = FMtoUProtocol::EncodeReady({FName(TEXT("Blink_R"))});
    Decoder.Append(Ready.GetData(), Ready.Num());
    TestTrue(TEXT("ready reply is framed"), Decoder.Pop(ReplyPayload, Error) == EMtoUDecodeResult::Message);
    TestTrue(TEXT("ready reply names missing curve"), FromUtf8(ReplyPayload).Contains(TEXT("Blink_R")));
    TArray<uint8> Failure = FMtoUProtocol::EncodeError(TEXT("bad skeleton"));
    Decoder.Append(Failure.GetData(), Failure.Num());
    TestTrue(TEXT("error reply is framed"), Decoder.Pop(ReplyPayload, Error) == EMtoUDecodeResult::Message);
    TestTrue(TEXT("error reply keeps message"), FromUtf8(ReplyPayload).Contains(TEXT("bad skeleton")));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUSkeletonComparisonTest,
    "MtoULiveLink.Protocol.OrderIndependentSkeletonComparison",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSkeletonComparisonTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const TArray<FMtoUBone> Maya = {
        {FName(TEXT("root")), INDEX_NONE},
        {FName(TEXT("arm")), 0},
        {FName(TEXT("hand")), 1},
    };
    const TArray<FMtoUBone> ShuffledUnreal = {
        {FName(TEXT("hand")), 2},
        {FName(TEXT("root")), INDEX_NONE},
        {FName(TEXT("arm")), 1},
    };
    TestTrue(TEXT("equivalent shuffled skeletons match"),
        FMtoUProtocol::CompareSkeletons(Maya, ShuffledUnreal).IsEmpty());

    const TArray<FMtoUBone> DifferentMaya = {
        {FName(TEXT("root")), INDEX_NONE},
        {FName(TEXT("zebra")), 0},
        {FName(TEXT("alpha")), 0},
        {FName(TEXT("hand")), 1},
    };
    const TArray<FMtoUBone> DifferentUnreal = {
        {FName(TEXT("root")), INDEX_NONE},
        {FName(TEXT("omega")), 0},
        {FName(TEXT("beta")), 0},
        {FName(TEXT("zebra")), 0},
        {FName(TEXT("hand")), 0},
    };
    const FString Diagnostic = FMtoUProtocol::CompareSkeletons(DifferentMaya, DifferentUnreal);
    TestTrue(TEXT("missing section is separate"), Diagnostic.Contains(TEXT("Missing in Unreal")));
    TestTrue(TEXT("extra section is separate"), Diagnostic.Contains(TEXT("Extra in Unreal")));
    TestTrue(TEXT("parent mismatch section is separate"), Diagnostic.Contains(TEXT("Parent mismatches")));
    TestTrue(TEXT("extra names are sorted"),
        Diagnostic.Find(TEXT("beta")) < Diagnostic.Find(TEXT("omega")));
    TestTrue(TEXT("parent mismatch names both parents"),
        Diagnostic.Contains(TEXT("hand: Maya=zebra, Unreal=root")));
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

#endif
