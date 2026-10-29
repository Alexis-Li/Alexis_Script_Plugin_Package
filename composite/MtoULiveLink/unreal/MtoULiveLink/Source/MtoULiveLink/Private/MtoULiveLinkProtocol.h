#pragma once

#include "CoreMinimal.h"
#include "LiveLinkTypes.h"
#include "MtoUConnectionNegotiator.h"
#include "MtoULinkageTypes.h"

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
    // v10 identity: the explicitly paired Maya object, its object-set revision
    // and the common-space revision both adapters must agree on.
    FString ObjectId;
    int32 SetRevision = 0;
    int32 SpaceRevision = 0;
    // v11: the ordered object ids of the set this connection negotiates,
    // primary first; every init of the set repeats the same list.
    TArray<FString> SetObjects;
};

/** The stable object ids of a set: the primary character and the second object. */
struct FMtoUObjectIds
{
    static constexpr TCHAR Character[] = TEXT("character");
    static constexpr TCHAR Secondary[] = TEXT("secondary");
};

/** Connection greeting: version, scene units and declared capabilities. */
struct FMtoUHelloMessage
{
    int32 Version = 0;
    FString Host;
    FString MayaVersion;
    FString LinearUnit;
    double UnitScaleCm = 0.0;
    double Fps = 0.0;
    bool bLinkage = false;
    bool bReference = false;
    bool bCache = false;
    bool bModel = false;
};

/** Maya's authority statement after one time-control transition. */
struct FMtoUModeMessage
{
    int32 SessionId = 0;
    int32 ModeRevision = 0;
    FString Mode;
    FString Control;
    FString Transition;
    FString Detail;
};

/** Maya confirms it restored its own timeline, view and gate. */
struct FMtoULeaveLinkageMessage
{
    int32 SessionId = 0;
    int32 ModeRevision = 0;
    FString Target;
    FString Detail;
};

/** One complete pose sample answering one `camera_time` target. */
struct FMtoUPoseGroupMessage
{
    int32 SessionId = 0;
    int32 SetRevision = 0;
    int32 ModeRevision = 0;
    int32 SpaceRevision = 0;
    FString EvalId;
    int32 TransportSerial = 0;
    int32 SampleSerial = 0;
    TArray<FMtoUPoseGroup> Objects;
    TSharedPtr<FJsonObject> Camera;
    TSharedPtr<FJsonObject> Time;
    // One group answers one target, so the time it reports is the group's
    // applied time rather than a per-object value.
    double UeDisplayFrame = 0.0;
    double MayaFrame = 0.0;
};

/** Maya removes the second object; SetRevision names the new set. */
struct FMtoURemoveObjectMessage
{
    int32 SessionId = 0;
    FString ObjectId;
    int32 SetRevision = 0;
    FString Detail;
};

/** One explicitly paired target the greeting names. */
struct FMtoUHelloTarget
{
    FString ObjectId;
    FString Kind;
    FString BindingPath;
    FString Generation;
};

/** Outcome of one local handoff import, swap or removal. */
struct FMtoUReferenceResultMessage
{
    int32 SessionId = 0;
    FString TransferId;
    bool bAccepted = false;
    FString Detail;
    // The stable code the Maya importer refused the handoff with, when it sent
    // one; empty for an accepted import.
    FString Code;
    int64 ObjectCount = 0;
    int64 TriangleCount = 0;
    int64 ByteCount = 0;
    FString Container;
};

/** The user removed the tool-owned reference container. */
struct FMtoUReferenceRemovedMessage
{
    int32 SessionId = 0;
    FString Detail;
};

/** Maya asks for the handoff of the scope the user selected in Unreal. */
struct FMtoUReferenceQueryMessage
{
    int32 SessionId = 0;
    FString TransferId;
    TSharedPtr<FJsonObject> Scope;
};

/** One `applied.components` row: actual completion evidence of one object. */
struct FMtoUAppliedComponent
{
    FString ObjectId;
    int32 BoneCount = 0;
    int32 MorphCount = 0;
    int32 PartCount = 0;
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

/** One object of a two-object realtime frame beside the primary. */
struct FMtoUFrameObject
{
    FString ObjectId;
    TArray<FMtoUTransform> Transforms;
    TArray<double> Curves;
};

struct FMtoUFrameMessage
{
    TArray<FMtoUTransform> Transforms;
    TArray<double> Curves;
    // v11: every other object of the set at the same Maya time; empty for a
    // one-object set and for cached frames.
    TArray<FMtoUFrameObject> Objects;
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
    static constexpr int32 Version = 11;
    // A set holds the primary character and at most one independent object.
    static constexpr int32 MaxSetObjects = 2;
    // Transient Unreal cache limits, recalibrated with the first real-project
    // capture (320 frames at 30 fps exceeded the original 64 MiB estimate) and
    // aligned with Maya's 1 GiB large-cache confirmation gate. These are the
    // frozen wire values; the single source of truth is the `limits` block of
    // protocol/conformance-v11.json, and both host adapters assert their
    // constants against the corpus. Encoded bytes are metered from the framing
    // boundary; parsed transient memory is preflighted from the negotiated
    // transform and curve counts before any allocation.
    static constexpr int64 MaxCachePayloadBytes = 1024ll * 1024ll * 1024ll;
    static constexpr int64 MaxCacheParsedMemoryBytes = 1536ll * 1024ll * 1024ll;
    static constexpr int32 MaxCacheFrameCount = 20000;
    static constexpr double MinCacheFps = 1.0;
    static constexpr double MaxCacheFps = 60.0;
    // Frozen per-message framing ceiling enforced at the length header by the
    // decoder before any JSON parse or allocation; oversized messages close
    // the connection. Pinned by limits.max_message_bytes in the corpus.
    static constexpr int64 MaxMessageBytes = 32ll * 1024ll * 1024ll;
    // The linkage group budget is metered over the whole group message, not
    // per object; it shares the framing ceiling and is enforced before the
    // group gains any staged ownership.
    static constexpr int64 MaxLinkageGroupBytes = 32ll * 1024ll * 1024ll;
    // Undelivered control messages per direction; overflow is an error instead
    // of an unbounded backlog.
    static constexpr int32 MaxControlQueue = 16;
    // At most one linkage target is in flight and one replaceable target
    // waits; an advancing target never cancels the issued one.
    static constexpr int32 MaxPendingCameraTargets = 2;
    // Scene-reference handoff budgets; the Maya importer enforces them before
    // staging, Unreal reports the offered counts so both sides agree.
    static constexpr int64 MaxReferenceBytes = 256ll * 1024ll * 1024ll;
    static constexpr int32 MaxReferenceObjects = 20000;
    static constexpr int32 MaxReferenceTriangles = 20000000;
    // After the last desired target an applied group must be observed within
    // this window before the view reports a non-converged linkage.
    static constexpr double LinkageConvergenceSeconds = 2.0;
    // Bounded admission budget for parsed cache frames queued between the
    // network worker and the Game Thread. A stalled Game Thread can never
    // grow queued parsed-frame ownership beyond these limits; the producer
    // waits (interruptibly) and the sender sees TCP backpressure instead.
    static constexpr int64 MaxQueuedCacheFrames = 512;
    static constexpr int64 MaxQueuedCacheBytes = 64ll * 1024ll * 1024ll;

    static bool ParseInit(
        const TArray<uint8>& Payload,
        FMtoUInitMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    // The connection greeting. A differing version is refused with
    // PROTOCOL_VERSION_MISMATCH before any target, writer or handoff is
    // touched, because the greeting is the first message of a connection.
    static bool ParseHello(
        const TArray<uint8>& Payload,
        FMtoUHelloMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseMode(
        const TArray<uint8>& Payload,
        FMtoUModeMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseLeaveLinkage(
        const TArray<uint8>& Payload,
        FMtoULeaveLinkageMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    // EncodedBytes meters the whole group message against the frozen group
    // budget before the group gains staged ownership on the Game Thread.
    static bool ParseGroup(
        const TArray<uint8>& Payload,
        int64 EncodedBytes,
        FMtoUPoseGroupMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseRemoveObject(
        const TArray<uint8>& Payload,
        FMtoURemoveObjectMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseReferenceResult(
        const TArray<uint8>& Payload,
        FMtoUReferenceResultMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseReferenceRemoved(
        const TArray<uint8>& Payload,
        FMtoUReferenceRemovedMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseReferenceQuery(
        const TArray<uint8>& Payload,
        FMtoUReferenceQueryMessage& OutMessage,
        FString& OutError,
        FString* OutErrorCode = nullptr);
    static bool ParseFrame(const TArray<uint8>& Payload, FMtoUFrameMessage& OutMessage, FString& OutError);
    static bool PeekType(const TArray<uint8>& Payload, FString& OutType, FString& OutError);
    // ExpectedObjects maps every object of the current set beside the primary
    // to its negotiated transform (X) and curve (Y) counts. A frame of another
    // set (an object the set no longer holds, or one it lacks) is a droppable
    // frame, not a structural error.
    static bool ValidateFrame(
        const FMtoUFrameMessage& Frame,
        int32 ExpectedTransformCount,
        int32 ExpectedCurveCount,
        FString& OutError,
        bool& bOutStructuralError,
        const TMap<FString, FIntPoint>* ExpectedObjects = nullptr);
    static bool ParseCacheEnter(const TArray<uint8>& Payload, FString& OutError);
    static bool ParseCacheBegin(
        const TArray<uint8>& Payload,
        FMtoUCacheBeginMessage& OutMessage,
        FString& OutError,
        FString& OutErrorCode);
    static bool ParseCacheFrame(
        const TArray<uint8>& Payload,
        int32 ExpectedTransformCount,
        int32 ExpectedCurveCount,
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
    static bool ParseCacheClear(const TArray<uint8>& Payload, int32& OutClearId, FString& OutError);
    static TArray<uint8> EncodeReady(
        const TArray<FName>& MissingInUnreal,
        const TArray<FName>& MissingInMaya,
        const TArray<FString>& BoneNameRemaps,
        const FString& Workflow,
        int32 TargetMorphCount,
        int32 AcceptedMorphCount,
        int32 NegotiatedRevision,
        int32 SessionId = INDEX_NONE,
        const FString& ObjectId = FString(),
        int32 SetRevision = INDEX_NONE,
        int32 SpaceRevision = INDEX_NONE);
    // Answer to the greeting: the session identity, the declared capabilities,
    // the explicitly selected target (empty until the user pairs) and the
    // common space the group and reference work must match.
    static TArray<uint8> EncodeHelloAck(
        int32 SessionId,
        bool bLinkage,
        bool bReference,
        bool bCache,
        bool bModel,
        const TArray<FMtoUHelloTarget>& Targets,
        int32 SpaceRevision,
        const TSharedPtr<FJsonObject>& Anchor);
    /** Unreal removed the second object and confirms the new set revision. */
    static TArray<uint8> EncodeObjectRemoved(
        int32 SessionId,
        const FString& ObjectId,
        int32 SetRevision,
        bool bRestored,
        const FString& Detail);
    /**
     * Unreal's authority statement for the current time-control state. A
     * linkage statement names the takeover's anchor (`MakeAnchorObject`).
     */
    static TArray<uint8> EncodeModeState(
        int32 SessionId,
        int32 ModeRevision,
        const FString& Mode,
        const FString& Control,
        const FString& Detail,
        const TSharedPtr<FJsonObject>& Anchor = nullptr);
    /**
     * A rigid placement in the shape of the handoff manifest's `world.anchor`:
     * `location_cm[3]` and `rotation_deg{roll, pitch, yaw}`.
     */
    static TSharedRef<FJsonObject> MakeAnchorObject(const FTransform& Placement);
    /** The current linkage target the editor evaluated from its sequence. */
    static TArray<uint8> EncodeCameraTime(
        int32 SessionId,
        int32 SetRevision,
        int32 ModeRevision,
        int32 SpaceRevision,
        const FMtoUCameraTimeTarget& Target);
    /** Actual component completion evidence for one applied group. */
    static TArray<uint8> EncodeApplied(
        int32 SessionId,
        int32 ModeRevision,
        const FString& EvalId,
        int32 SampleSerial,
        bool bApplied,
        double AppliedDisplayFrame,
        double LagSeconds,
        const TArray<FMtoUAppliedComponent>& Components,
        const FString& Reason);
    /** A finished local handoff the user may import. */
    static TArray<uint8> EncodeReferenceOffer(
        int32 SessionId,
        const FString& TransferId,
        const FString& Directory,
        const FString& ManifestDigest,
        const TSharedPtr<FJsonObject>& Scope,
        int32 SpaceRevision,
        int64 ByteCount,
        int32 ObjectCount,
        int32 TriangleCount);
    static TArray<uint8> EncodeCacheReady(int32 UploadId, int32 NegotiatedRevision, int32 FrameCount);
    static TArray<uint8> EncodeCachePlaying(int32 UploadId, int32 PlayId, bool bLoopEnabled);
    static TArray<uint8> EncodeCacheProgress(int32 PlayId, int32 AppliedFrames);
    // Paused and resumed carry no source frame while the attempt has not
    // applied a pose yet; INDEX_NONE omits the field instead of inventing one.
    static TArray<uint8> EncodeCachePaused(int32 PlayId, int32 AppliedFrames, int32 SourceFrame);
    static TArray<uint8> EncodeCacheResumed(int32 PlayId, int32 AppliedFrames, int32 SourceFrame);
    static TArray<uint8> EncodeCacheSeeked(int32 PlayId, int32 SourceFrame, int32 AppliedFrames);
    static TArray<uint8> EncodeCacheLooped(int32 PlayId, int32 Round, int32 SourceFrame);
    static TArray<uint8> EncodeCacheLoopChanged(int32 PlayId, bool bLoopEnabled);
    // Only a whole-cache segment may claim ScopeCache: a seek or a loop round
    // completes a segment and never the attempt's single sequential replay.
    static TArray<uint8> EncodeCacheComplete(
        int32 PlayId,
        int32 AppliedFrameCount,
        double ElapsedSeconds,
        bool bWholeCacheScope,
        int32 StartFrame,
        int32 EndFrame);
    static TArray<uint8> EncodeCacheStopped(int32 PlayId);
    // Cleared echoes the owning identity of the cache ownership that was
    // dropped so Maya can discard late outcomes from older operations.
    static TArray<uint8> EncodeCacheCleared(int32 UploadId, int32 PlayId, int32 ClearId);
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
    // Returns false and publishes nothing when any local transform needed by
    // retargeting is singular; OutError then names the offending bone.
    static bool MakeRetargetedFrameData(
        const FMtoUFrameMessage& Frame,
        const TArray<int32>& AcceptedCurveIndices,
        const TArray<FTransform>& SourceBindLocalPose,
        const TArray<FTransform>& TargetRefLocalPose,
        const TArray<int32>& BoneParents,
        FLiveLinkFrameDataStruct& OutFrameData,
        FString& OutError);
};
