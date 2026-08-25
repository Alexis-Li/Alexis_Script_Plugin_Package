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
    // Authoritative character snapshot revision established by Maya during
    // connection negotiation; echoed in the ready outcome.
    int32 Revision = 0;
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
    int32 UploadId = 0;
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
    static constexpr int32 Version = 6;
    // Transient Unreal cache limits, recalibrated with the first real-project
    // capture (320 frames at 30 fps exceeded the original 64 MiB estimate) and
    // aligned with Maya's 1 GiB large-cache confirmation gate. Frozen in the
    // protocol contract. Encoded bytes are metered from the framing boundary;
    // parsed transient memory is preflighted from the negotiated transform and
    // curve counts before any allocation.
    static constexpr int64 MaxCachePayloadBytes = 1024ll * 1024ll * 1024ll;
    static constexpr int64 MaxCacheParsedMemoryBytes = 1536ll * 1024ll * 1024ll;
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
    static bool ParseCacheEnter(const TArray<uint8>& Payload, FString& OutError);
    static bool ParseCacheBegin(
        const TArray<uint8>& Payload,
        FMtoUCacheBeginMessage& OutMessage,
        FString& OutError,
        FString& OutErrorCode);
    static bool ParseCacheFrame(
        const TArray<uint8>& Payload,
        int32& OutIndex,
        FMtoUFrameMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseCacheEnd(const TArray<uint8>& Payload, FString& OutError);
    static bool ParseCachePlay(
        const TArray<uint8>& Payload,
        int32& OutPlayId,
        FString& OutError);
    static bool ParseCacheStop(const TArray<uint8>& Payload, FString& OutError);
    static bool ParseCacheClear(const TArray<uint8>& Payload, FString& OutError);
    static TArray<uint8> EncodeReady(
        const TArray<FName>& MissingInUnreal,
        const TArray<FName>& MissingInMaya,
        const TArray<FString>& BoneNameRemaps,
        const FString& Workflow,
        int32 TargetMorphCount,
        int32 AcceptedMorphCount,
        int32 NegotiatedRevision);
    static TArray<uint8> EncodeCacheReady(int32 UploadId, int32 NegotiatedRevision, int32 FrameCount);
    static TArray<uint8> EncodeCacheProgress(int32 PlayId, int32 AppliedFrames);
    static TArray<uint8> EncodeCacheComplete(int32 PlayId, int32 AppliedFrameCount, double ElapsedSeconds);
    static TArray<uint8> EncodeCacheStopped(int32 PlayId);
    static TArray<uint8> EncodeCacheCleared();
    static TArray<uint8> EncodeError(
        const FString& Code,
        const FString& Message,
        const FString& Details = FString(),
        int32 UploadId = INDEX_NONE,
        int32 PlayId = INDEX_NONE);
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
