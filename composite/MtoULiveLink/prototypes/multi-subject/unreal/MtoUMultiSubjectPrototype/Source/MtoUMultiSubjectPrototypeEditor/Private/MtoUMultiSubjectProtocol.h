// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MtoUMultiSubjectTypes.h"

struct FReferenceSkeleton;
class USkeletalMesh;

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
	 * The target's required bones: every bone that carries a positive skin
	 * weight in any LOD, plus every ancestor of one. Returns false with a
	 * diagnostic when the mesh does not expose reliable skinning data, because
	 * a mapping decision is never guessed.
	 */
	static bool CollectRequiredTargetBones(
		const USkeletalMesh& Mesh,
		TArray<bool>& OutRequired,
		FString& OutProblem);

	/**
	 * Maps one Maya declaration onto one Unreal target and fills `OutMap` with
	 * the source-to-target assignment, the driven/required/undriven target
	 * bones and the declared curves the target does not own.
	 *
	 * Rules: a declared bone drives the target bone of the same name inside the
	 * already mapped parent scope; a declared bone without such a target bone is
	 * an ignored export branch; two declared bones resolving to one target bone
	 * are an ambiguity; every required target bone must be driven by exactly one
	 * declared bone; every mapped bone's advertised bind must match the target
	 * reference pose within the prototype tolerances (translation 0.25 cm,
	 * rotation 0.5 deg, scale 0.005).
	 *
	 * Returns an empty string when the declaration is accepted; otherwise the
	 * mismatch in one sentence.
	 */
	static FString DescribeNegotiationMismatch(
		const FMtoUSubjectDeclaration& Declaration,
		const USkeletalMesh& Mesh,
		FMtoUNegotiationMap& OutMap);

	/**
	 * One vertex of the target mesh with a positive weight for the given target
	 * bone, with the LOD it belongs to. The tests and the real-asset evidence
	 * use it to check that a driven bone actually moves skinned vertices rather
	 * than only being accepted by the negotiation.
	 */
	static bool FindWeightedTargetVertex(
		const USkeletalMesh& Mesh,
		int32 BoneIndex,
		int32& OutLODIndex,
		int32& OutVertexIndex,
		FString& OutProblem);

	/** Human-readable skeleton signature for evidence: names and parent names, in skeleton order. */
	static FString DescribeSkeletonSignature(const FReferenceSkeleton& Skeleton);
};
