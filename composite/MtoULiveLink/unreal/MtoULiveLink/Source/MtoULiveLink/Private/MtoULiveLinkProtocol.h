#pragma once

#include "CoreMinimal.h"
#include "LiveLinkTypes.h"
#include "MtoUConnectionNegotiator.h"

struct FMtoUTransform
{
    FVector Translation = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    FVector Scale = FVector::OneVector;
};

struct FMtoUInitMessage
{
    TArray<FMtoUDescriptionBone> Bones;
    TArray<FTransform> SourceBindLocalPose;
    TArray<FName> Curves;
};

struct FMtoUFrameMessage
{
    TArray<FMtoUTransform> Transforms;
    TArray<double> Curves;
};

enum class EMtoUDecodeResult
{
    NeedMore,
    Message,
    Error,
};

class FMtoUFrameDecoder
{
public:
    void Append(const uint8* Data, int32 Num);
    EMtoUDecodeResult Pop(TArray<uint8>& OutPayload, FString& OutError);

private:
    TArray<uint8> Buffer;
};

class FMtoUProtocol
{
public:
    static constexpr int32 Version = 3;
    static bool ParseInit(
        const TArray<uint8>& Payload,
        FMtoUInitMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseFrame(const TArray<uint8>& Payload, FMtoUFrameMessage& OutMessage, FString& OutError);
    static bool ValidateFrame(
        const FMtoUFrameMessage& Frame,
        int32 ExpectedTransformCount,
        int32 ExpectedCurveCount,
        FString& OutError,
        bool& bOutStructuralError);
    static TArray<uint8> EncodeReady(
        const TArray<FName>& MissingInUnreal,
        const TArray<FName>& MissingInMaya,
        const TArray<FString>& BoneNameRemaps);
    static TArray<uint8> EncodeError(
        const FString& Code,
        const FString& Message,
        const FString& Details = FString());
    static FLiveLinkStaticDataStruct MakeStaticData(
        const FMtoUInitMessage& Init,
        const TArray<FName>& AcceptedCurveNames);
    static FLiveLinkFrameDataStruct MakeFrameData(
        const FMtoUFrameMessage& Frame,
        const TArray<int32>& AcceptedCurveIndices);
    static FLiveLinkFrameDataStruct MakeRetargetedFrameData(
        const FMtoUFrameMessage& Frame,
        const TArray<int32>& AcceptedCurveIndices,
        const TArray<FTransform>& SourceBindLocalPose,
        const TArray<FTransform>& TargetRefLocalPose,
        const TArray<int32>& BoneParents);
};
