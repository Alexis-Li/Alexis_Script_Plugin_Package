#pragma once

#include "CoreMinimal.h"

#include "Dom/JsonObject.h"

/**
 * Protocol v10 linkage identities. The runtime module owns them; the editor
 * adapter only reads and passes them back, so both hosts agree on which
 * session, object set, common space and mode a message belongs to.
 */
struct MTOULIVELINK_API FMtoULinkageIdentity
{
    int32 SessionId = 0;
    int32 SetRevision = 0;
    int32 SpaceRevision = 0;
    int32 ModeRevision = 0;
};

/**
 * One evaluated camera/time target the editor asks Maya to follow. The editor
 * fills it from the Level Sequence it owns; the runtime serializes it as the
 * `camera_time` message. Camera and gate stay as the payload the editor built
 * so the Unreal-to-Maya mapping lives in one place.
 */
struct MTOULIVELINK_API FMtoUCameraTimeTarget
{
    FString EvalId;
    int32 EvalSerial = 0;
    int32 TransportSerial = 0;
    int32 FrameSerial = 0;
    double DisplayFrame = 0.0;
    double Seconds = 0.0;
    int32 PlaybackRangeStart = 0;
    int32 PlaybackRangeEnd = 0;
    int32 DisplayRateNumerator = 0;
    int32 DisplayRateDenominator = 1;
    int32 TickNumerator = 0;
    int32 TickDenominator = 1;
    FString CutStage;
    FString CutName;
    /**
     * A discrete event produced this target: a pause, a jump or scrub (also
     * within one shot), a camera edit, a loop wrap. The scheduler cancels the
     * issued target instead of keeping it in flight; ordinary playback that
     * advances the time leaves this false so the in-flight answer survives.
     */
    bool bDiscontinuous = false;
    TSharedPtr<class FJsonObject> Camera;
    TSharedPtr<class FJsonObject> Gate;
};

/** The complete pose group Maya answered one target with. */
struct MTOULIVELINK_API FMtoUPoseGroup
{
    FString EvalId;
    FString ObjectId;
    int32 TransportSerial = 0;
    int32 SampleSerial = 0;
    TArray<FTransform> Transforms;
    TArray<double> Curves;
    TSharedPtr<class FJsonObject> CameraReadback;
    double MayaFrame = 0.0;
    double UeDisplayFrame = 0.0;
};

/** Actual component completion evidence for one applied group. */
struct MTOULIVELINK_API FMtoUAppliedEvidence
{
    FString EvalId;
    int32 SampleSerial = 0;
    bool bApplied = false;
    double AppliedDisplayFrame = 0.0;
    double LagSeconds = 0.0;
    FString Reason;
    int32 BoneCount = 0;
    int32 CurveCount = 0;
    int32 PartCount = 0;
};

/** What the panel shows; produced by the runtime, rendered by the editor. */
struct MTOULIVELINK_API FMtoULinkageView
{
    FString Mode = TEXT("realtime");
    FString Control = TEXT("maya");
    FMtoULinkageIdentity Identity;
    bool bPaired = false;
    FString PairedObjectId;
    FString TargetKind;
    FString TargetPath;
    FString TargetEvalId;
    double TargetDisplayFrame = 0.0;
    FString AppliedEvalId;
    double AppliedDisplayFrame = 0.0;
    int32 AppliedSampleSerial = 0;
    double AppliedLagSeconds = 0.0;
    bool bConverged = false;
    bool bTransitioning = false;
    /** The paired target's writers are still muted (after 返回实时 too). */
    bool bTakeoverHeld = false;
    /** The last takeover's writers were handed back. */
    bool bWritersRestored = false;
    /** What the last restore could not hand back; empty when nothing is left. */
    FString RecoveryResidual;
    /** Groups refused as normal scheduling (cancelled, superseded, older mode). */
    int32 RefusedGroupCount = 0;
    /** The code and message of the latest such refusal. */
    FString RefusedCode;
    FString RefusedMessage;
    FString Detail;
    FString Error;
    FString ErrorCode;
};

/**
 * Editor-side seam. The runtime asks the editor to evaluate the current
 * sequence camera/time and to take over or restore the paired target's
 * writers; the editor subscribes for status changes. Every callback runs on
 * the game thread.
 */
struct MTOULIVELINK_API FMtoULinkageHooks
{
    /** Fill OutTarget from the open Level Sequence; false when nothing to send. */
    TFunction<bool(FMtoUCameraTimeTarget& OutTarget)> EvaluateCameraTime;
    /** Mute the paired target's writers; false names the reason and its code. */
    TFunction<bool(FString& OutError, FString& OutErrorCode)> TakeOverTarget;
    /** Restore every borrowed writer; false names what could not be restored. */
    TFunction<bool(FString& OutError, FString& OutErrorCode)> RestoreTarget;
    /**
     * While writers are borrowed: false, with the reason, once the takeover's
     * Level Sequence was closed or switched, its world was closed or the
     * paired target was destroyed. The runtime then ends the linkage.
     */
    TFunction<bool(FString& OutReason)> ValidateTakeover;
    /** Report a new view snapshot for the panel. */
    TFunction<void(const FMtoULinkageView&)> OnViewChanged;
};
