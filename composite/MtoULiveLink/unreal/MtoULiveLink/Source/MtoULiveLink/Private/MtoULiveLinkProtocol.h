#pragma once

#include "CoreMinimal.h"
#include "LiveLinkTypes.h"

struct FMtoUBone
{
    FName Name;
    int32 ParentIndex = INDEX_NONE;
};

struct FMtoUTransform
{
    FVector Translation = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    FVector Scale = FVector::OneVector;
};

struct FMtoUInitMessage
{
    TArray<FMtoUBone> Bones;
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
    static bool ParseInit(const TArray<uint8>& Payload, FMtoUInitMessage& OutMessage, FString& OutError);
    static bool ParseFrame(const TArray<uint8>& Payload, FMtoUFrameMessage& OutMessage, FString& OutError);
    static bool ValidateFrame(
        const FMtoUFrameMessage& Frame,
        int32 ExpectedTransformCount,
        int32 ExpectedCurveCount,
        FString& OutError,
        bool& bOutStructuralError);
    static FString CompareSkeletons(const TArray<FMtoUBone>& Maya, const TArray<FMtoUBone>& Unreal);
    static TArray<uint8> EncodeReady(const TArray<FName>& MissingCurves);
    static TArray<uint8> EncodeError(const FString& Message);
    static FLiveLinkStaticDataStruct MakeStaticData(
        const FMtoUInitMessage& Init,
        const TArray<FName>& AcceptedCurveNames);
    static FLiveLinkFrameDataStruct MakeFrameData(
        const FMtoUFrameMessage& Frame,
        const TArray<int32>& AcceptedCurveIndices);
};
