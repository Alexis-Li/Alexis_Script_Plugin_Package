// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "CoreMinimal.h"

#include "MtoUMultiSubjectPoseInstance.generated.h"

/** One subject pose the receiver published, in the declaration's bone order. */
struct FMtoUSubjectPose
{
	/** Target skeleton bone indices, one per (driven) pose entry. */
	TArray<int32> BoneIndices;
	/** Declared bone names, in the order the transforms follow; evidence only. */
	TArray<FName> BoneNames;
	/** Local (parent-relative) transforms; the root entry is the Maya world pose. */
	TArray<FTransform> LocalTransforms;
};

/**
 * The game thread publishes a pose; PreUpdate copies it onto the proxy,
 * Evaluate applies bones on the evaluation thread, and the target applies
 * Morph weights to its own component on the game thread.
 */
struct FMtoUMultiSubjectPoseProxy : public FAnimInstanceProxy
{
	FMtoUMultiSubjectPoseProxy() = default;
	explicit FMtoUMultiSubjectPoseProxy(UAnimInstance* InAnimInstance)
		: FAnimInstanceProxy(InAnimInstance)
	{
	}

	//~ Begin FAnimInstanceProxy interface
	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;
	//~ End FAnimInstanceProxy interface

	/** Pose copied on the game thread for this evaluation; null until the first frame. */
	TSharedPtr<FMtoUSubjectPose, ESPMode::ThreadSafe> CurrentPose;
	/** Increments with every published pose so the proxy can report freshness. */
	int64 PoseRevision = 0;
};

/**
 * The prototype's explicit equivalent of an independently driven LiveLink
 * subject: one instance per target, fed only by that target's subject, so two
 * subjects never share a pose. It is installed by a preview takeover and
 * removed again by the target's restore.
 */
UCLASS(Transient, NotBlueprintable)
class UMtoUMultiSubjectPoseInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** Publishes one subject pose for the next evaluation. Game thread only. */
	void SetSubjectPose(const FMtoUSubjectPose& Pose);

	/** Drops the published pose; the target then keeps the last evaluated pose. */
	void ClearSubjectPose();

	/** True once a subject pose has been published. */
	bool HasSubjectPose() const { return PendingPose.IsValid(); }

	/** Revision of the published pose; matches the proxy's copy after an evaluation. */
	int64 GetPoseRevision() const { return PoseRevision; }

	/** Copy the proxy took for its latest evaluation, exposed for evidence only. */
	int64 GetEvaluatedPoseRevision() const;

protected:
	//~ Begin UAnimInstance interface
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;
	//~ End UAnimInstance interface

private:
	friend struct FMtoUMultiSubjectPoseProxy;

	TSharedPtr<FMtoUSubjectPose, ESPMode::ThreadSafe> PendingPose;
	int64 PoseRevision = 0;
};
