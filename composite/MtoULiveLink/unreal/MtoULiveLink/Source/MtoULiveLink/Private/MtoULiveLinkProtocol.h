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
    FString Workflow;
    bool bBlendshapesEnabled = false;
};

struct FMtoUWorkflows
{
    static constexpr TCHAR Animation[] = TEXT("animation");
    static constexpr TCHAR Model[] = TEXT("model");
    static bool IsValid(const FString& Workflow)
    {
        return Workflow == Animation || Workflow == Model;
    }
};

struct FMtoUFrameMessage
{
    TArray<FMtoUTransform> Transforms;
    TArray<double> Curves;
};

struct FMtoUCacheBeginMessage
{
    int32 Revision = 0;
    double Fps = 0.0;
    int32 StartFrame = 0;
    int32 EndFrame = 0;
    int32 FrameCount = 0;
    int64 PayloadSize = 0;
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
    static constexpr int32 Version = 5;
    // Transient Unreal cache limits, calibrated for the current production
    // range (C01: 321 frames at 30 fps) and frozen in the protocol contract.
    static constexpr int64 MaxCachePayloadBytes = 64ll * 1024ll * 1024ll;
    static constexpr int32 MaxCacheFrameCount = 20000;
    static constexpr double MinCacheFps = 1.0;
    static constexpr double MaxCacheFps = 60.0;

    static bool ParseInit(
        const TArray<uint8>& Payload,
        FMtoUInitMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseFrame(const TArray<uint8>& Payload, FMtoUFrameMessage& OutMessage, FString& OutError);
    static bool PeekType(const TArray<uint8>& Payload, FString& OutType, FString& OutError);
    static bool ValidateFrame(
        const FMtoUFrameMessage& Frame,
        int32 ExpectedTransformCount,
        int32 ExpectedCurveCount,
        FString& OutError,
        bool& bOutStructuralError);
    static bool ParseCacheBegin(
        const TArray<uint8>& Payload,
        FMtoUCacheBeginMessage& OutMessage,
        FString& OutError,
        FString& OutErrorCode);
    static bool ParseCacheFrame(
        const TArray<uint8>& Payload,
        int32& OutIndex,
        FMtoUFrameMessage& OutMessage,
        FString& OutError);
    static bool ParseCacheEnd(const TArray<uint8>& Payload, FString& OutError);
    static bool ParseCachePlay(
        const TArray<uint8>& Payload,
        int32& OutRevision,
        FString& OutError);
    static bool ParseCacheStop(const TArray<uint8>& Payload, FString& OutError);
    static bool ParseCacheClear(const TArray<uint8>& Payload, FString& OutError);
    static TArray<uint8> EncodeReady(
        const TArray<FName>& MissingInUnreal,
        const TArray<FName>& MissingInMaya,
        const TArray<FString>& BoneNameRemaps,
        const FString& Workflow,
        int32 TargetMorphCount,
        int32 AcceptedMorphCount);
    static TArray<uint8> EncodeCacheReady(int32 FrameCount);
    static TArray<uint8> EncodeCacheComplete(int32 FrameCount);
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
