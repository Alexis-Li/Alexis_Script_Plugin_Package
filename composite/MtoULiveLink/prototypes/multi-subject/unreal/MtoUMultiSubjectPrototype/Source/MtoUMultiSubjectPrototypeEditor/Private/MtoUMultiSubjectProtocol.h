// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MtoUMultiSubjectTypes.h"

struct FReferenceSkeleton;

/**
 * Fixed two-subject wire profile: parsing, shape validation and reply
 * encoding. Every rejection carries one of the stable MtoUMultiSubjectError
 * codes so the Maya peer never has to parse English.
 */
class FMtoUMultiSubjectProtocol
{
public:
	/** Reads the `type` field; a message without one is a shape error. */
	static bool PeekType(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutType,
		FMtoUProtocolError& OutError);

	static bool ParseInit(
		const TSharedPtr<FJsonObject>& Object,
		FMtoUInitMessage& OutMessage,
		FMtoUProtocolError& OutError);

	/**
	 * Reads one `frame`, including the `session` it claims to belong to. A
	 * message without a positive integer session is a shape error; whether that
	 * session is the receiver's current one is the receiver's decision.
	 */
	static bool ParseFrame(
		const TSharedPtr<FJsonObject>& Object,
		FMtoUFrameMessage& OutMessage,
		FMtoUProtocolError& OutError);

	/** Reads one `remove`, including the `session` it claims to belong to. */
	static bool ParseRemove(
		const TSharedPtr<FJsonObject>& Object,
		FMtoURemoveMessage& OutMessage,
		FMtoUProtocolError& OutError);

	/** "forward", "backward", "hold" or "first" for one applied source time. */
	static FString DescribeTimeDirection(double PreviousTime, bool bHasPrevious, double Time);

	static TSharedRef<FJsonObject> MakeReady(int64 Session);
	static TSharedRef<FJsonObject> MakeApplied(
		int64 Session,
		int64 Serial,
		double Time,
		const TArray<FMtoUSubjectStatus>& SubjectStatuses);
	static TSharedRef<FJsonObject> MakeError(const FString& Code, const FString& Details);

	/** Serializes one message as the single line the contract requires. */
	static FString ToLine(const TSharedRef<FJsonObject>& Object);

	/**
	 * Reads the ten-number UE-space pose tuple the Maya side sends after its
	 * own convert_transform: tx,ty,tz,qx,qy,qz,qw,sx,sy,sz in centimetres.
	 * Finite values and a unit quaternion within tolerance are required.
	 */
	static bool ReadPoseTuple(const TArray<TSharedPtr<FJsonValue>>& Values, FTransform& OutTransform);

	/**
	 * Necessary-bone identity of one Maya declaration against an Unreal target
	 * skeleton: every declared bone must exist in the target, carry the declared
	 * parent, and have only declared bones above it, so no driven bone ever
	 * hangs under a bone the sender does not drive. Target bones the declaration
	 * does not name keep their reference pose and are listed by
	 * `CollectUndrivenBones`. Returns an empty string when the declaration is
	 * accepted; otherwise the first offending bone in one sentence.
	 */
	static FString DescribeSkeletonMismatch(
		const FMtoUSubjectDeclaration& Declaration,
		const FReferenceSkeleton& Skeleton);

	/** Target bones the declaration does not drive; they hold their reference pose. */
	static void CollectUndrivenBones(
		const FMtoUSubjectDeclaration& Declaration,
		const FReferenceSkeleton& Skeleton,
		TArray<FName>& OutBones);

	/**
	 * First advertised bind row that differs from the target's reference pose
	 * beyond the prototype tolerances (translation 0.25 cm, rotation 0.5 deg,
	 * scale 0.005). Names and parents are checked separately, so a difference
	 * here really is a differently resting rig.
	 */
	static FString DescribeBindMismatch(
		const FMtoUSubjectDeclaration& Declaration,
		const FReferenceSkeleton& Skeleton);

	/** The first declared curve that is not a Morph Target of the target mesh. */
	static bool FindMissingCurve(
		const FMtoUSubjectDeclaration& Declaration,
		const USkeletalMeshComponent& Component,
		FName& OutMissing);

	/** Human-readable skeleton signature for evidence: names and parent names, in skeleton order. */
	static FString DescribeSkeletonSignature(const FReferenceSkeleton& Skeleton);
};
