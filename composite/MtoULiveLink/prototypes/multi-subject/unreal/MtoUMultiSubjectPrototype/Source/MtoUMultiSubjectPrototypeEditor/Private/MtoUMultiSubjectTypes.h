// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UMovieSceneSequencePlayer;
class UMovieSceneTrack;
class USkeletalMeshComponent;

/**
 * Fixed two-subject realtime profile of the Issue 54 wire contract. The
 * prototype implements exactly this profile: it never generalizes to N
 * subjects, never retargets, and never accepts a partial frame.
 */
namespace MtoUMultiSubjectProtocol
{
	/** Wire version this receiver implements. */
	constexpr int32 Version = 1;
	/** One newline-delimited UTF-8 JSON object may not exceed this many bytes. */
	constexpr int64 MaxLineBytes = 1024 * 1024;
	/** The profile is fixed at exactly two subjects per connection. */
	constexpr int32 SubjectCount = 2;
}

/** Stable error codes; a client may key off them instead of the details text. */
namespace MtoUMultiSubjectError
{
	/** init version is not 1. */
	const TCHAR* const VersionUnsupported = TEXT("version_unsupported");
	/** init does not carry exactly two subjects with distinct ids, or shapes are wrong. */
	const TCHAR* const SubjectsShape = TEXT("subjects_shape");
	/** A shape problem inside a message other than the subject list. */
	const TCHAR* const MessageShape = TEXT("message_shape");
	/** A subject id is not registered as a target on this Unreal side. */
	const TCHAR* const UnknownSubjectId = TEXT("unknown_subject_id");
	/** Two subjects resolve to the same target object. */
	const TCHAR* const TargetReused = TEXT("target_reused");
	/** Declared bones and/or parent indices differ from the target skeleton. */
	const TCHAR* const SkeletonMismatch = TEXT("skeleton_mismatch");
	/** A declared curve is not a Morph Target of the target mesh. */
	const TCHAR* const CurveNotInTarget = TEXT("curve_not_in_target");
	/** The target is placed by a socket or parent motion, so anchor-once cannot hold. */
	const TCHAR* const AnchorConflict = TEXT("anchor_conflict");
	/** The target is not attached to its own anchor actor. */
	const TCHAR* const TargetDetached = TEXT("target_detached");
	/** A frame or remove arrived before a successful negotiation. */
	const TCHAR* const NoSession = TEXT("no_session");
	/** The message names a session that is not the receiver's current one. */
	const TCHAR* const SessionMismatch = TEXT("session_mismatch");
	/** A frame's serial is not strictly increasing inside its session. */
	const TCHAR* const FrameOrder = TEXT("frame_order");
	/** A frame does not carry exactly the enabled subjects, or an unknown one. */
	const TCHAR* const FrameSubjects = TEXT("frame_subjects");
	/** A frame subject's transforms/curves do not match its negotiated declaration. */
	const TCHAR* const FrameShape = TEXT("frame_shape");
	/** remove names a subject that is not currently enabled. */
	const TCHAR* const SubjectNotEnabled = TEXT("subject_not_enabled");
	/** The line is not one JSON object. The connection is closed afterwards. */
	const TCHAR* const MalformedJson = TEXT("malformed_json");
	/** The line is larger than 1 MiB. The connection is closed afterwards. */
	const TCHAR* const TooLarge = TEXT("too_large");
	/** The preview could not suppress an existing writer. */
	const TCHAR* const PreviewConflict = TEXT("preview_conflict");
}

/** One bone of a Maya subject declaration, in Maya skeleton order. */
struct FMtoUBoneDeclaration
{
	FName Name;
	int32 Parent = INDEX_NONE;
};

/** One Maya subject: explicit root, ordered bones, curve names, bind local poses. */
struct FMtoUSubjectDeclaration
{
	FString Id;
	FString Root;
	TArray<FMtoUBoneDeclaration> Bones;
	TArray<FName> Curves;
	TArray<FTransform> Bind;
};

/** The negotiated description of one connection. */
struct FMtoUInitMessage
{
	int32 Version = 0;
	double Fps = 0.0;
	TArray<FMtoUSubjectDeclaration> Subjects;
};

/** One subject's pose inside a frame; values follow the declaration's order. */
struct FMtoUFrameSubject
{
	FString Id;
	TArray<FTransform> Transforms;
	TArray<float> Curves;
};

/** One atomic pair of subjects sampled at one Maya source time. */
struct FMtoUFrameMessage
{
	/** The `ready` session this frame belongs to; a stale session is refused. */
	int64 Session = 0;
	/** Evaluation identity: strictly increasing inside one session. */
	int64 Serial = 0;
	/** Maya source time; it may move backwards or repeat on a re-edit. */
	double Time = 0.0;
	TArray<FMtoUFrameSubject> Subjects;
};

/** Stop driving exactly one subject of the live session. */
struct FMtoURemoveMessage
{
	int64 Session = 0;
	FString Id;
};

/** A rejected message with its stable code. */
struct FMtoUProtocolError
{
	FString Code;
	FString Details;
};

/** One explicit anchor-plus-component pairing the receiver may negotiate. */
struct FMtoUTargetRegistration
{
	FString Id;
	TWeakObjectPtr<AActor> Anchor;
	TWeakObjectPtr<USkeletalMeshComponent> Component;
};

/** Result of the anchor-once preflight for one target. */
struct FMtoUAnchorCheck
{
	bool bValid = false;
	FString Details;
	FTransform AnchorTransform = FTransform::Identity;
	bool bAttachedToOwnRoot = false;
	bool bActorAttached = false;
	FName SocketName = NAME_None;
};

/** One subject's outcome as reported in an `applied` reply. */
struct FMtoUSubjectStatus
{
	FString Id;
	FString Status;
};

/**
 * A writer the preview takes over from. The prototype only suppresses writers
 * that are handed to it explicitly, so a bounded fixture (or an operator) owns
 * the decision instead of a heuristic.
 */
struct FMtoUPreviewWriter
{
	/** The track the preview suppresses while its subject is driven. */
	TWeakObjectPtr<UMovieSceneTrack> Track;
	/** The player that owns the track's time, paused while the preview drives. */
	TWeakObjectPtr<UMovieSceneSequencePlayer> Player;
	/**
	 * The subject id this writer belongs to. Removing that subject restores only
	 * its writers; an empty id makes the writer session-wide.
	 */
	FString TargetId;
};

/** Per-bone measurement of one applied subject. */
struct FMtoUBoneMeasurement
{
	FName BoneName;
	/** The local transform the wire carried. */
	FTransform ReceivedLocal = FTransform::Identity;
	/** Component-space transform composed from the received locals along the declared hierarchy. */
	FTransform ExpectedComponentSpace = FTransform::Identity;
	/** The component-space transform the target component actually holds. */
	FTransform ComponentSpace = FTransform::Identity;
	/** Largest absolute difference between ExpectedComponentSpace and ComponentSpace. */
	double Delta = 0.0;
};

/** Measurement of one subject after it was applied to its target. */
struct FMtoUSubjectMeasurement
{
	FString Id;
	FVector AnchorLocation = FVector::ZeroVector;
	FQuat AnchorRotation = FQuat::Identity;
	FVector AnchorScale = FVector::OneVector;
	/** The root bone's world transform, which must equal AnchorTransform * received root local. */
	FTransform RootWorld = FTransform::Identity;
	/** Anchor applied exactly once: |RootWorld - AnchorTransform * ReceivedRootLocal|. */
	double RootWorldDelta = 0.0;
	TArray<FMtoUBoneMeasurement> Bones;
	TArray<TPair<FName, float>> Curves;
};

/**
 * What one applied frame produced, kept for the machine-readable evidence.
 * `TimeDirection` records how the source time moved against the previous
 * applied frame, so a reverse scrub and a same-frame re-edit stay visible
 * instead of looking like ordinary forward playback.
 */
struct FMtoUFrameRecord
{
	int64 Session = 0;
	int64 Serial = 0;
	double Time = 0.0;
	FString TimeDirection;
	double ApplySeconds = 0.0;
	bool bPreviewActive = false;
	TArray<FMtoUSubjectMeasurement> Subjects;
};

/** A rejected message, kept for the machine-readable evidence. */
struct FMtoUEvidenceError
{
	FString Code;
	FString Details;
	int64 AtSerial = 0;
};
