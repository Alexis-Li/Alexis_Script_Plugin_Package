// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/EngineTypes.h"
#include "MtoUMultiSubjectTypes.h"

class AActor;
class UAnimationAsset;
class UMtoUMultiSubjectPoseInstance;
class USkeletalMeshComponent;
struct FReferenceSkeleton;

/** Everything the prototype touched on one target, so a restore is exact. */
struct FMtoUTargetAnimationSnapshot
{
	EAnimationMode::Type AnimationMode = EAnimationMode::AnimationBlueprint;
	TSubclassOf<UAnimInstance> AnimClass;
	TObjectPtr<UAnimationAsset> AnimToPlay;
	bool bWasPlaying = false;
	bool bWasLooping = true;
	float Position = 0.0f;
	float PlayRate = 1.0f;
	bool bPauseAnims = false;
	bool bUpdateAnimationInEditor = false;
	bool bDisablePostProcessBlueprint = false;
	EVisibilityBasedAnimTickOption VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	/** Every Morph Target weight of the mesh at takeover time. */
	TMap<FName, float> MorphWeights;
	/** The component carried its own animation driver at takeover time. */
	bool bHadAnimationDriver = false;
	/** Stable name of that driver for the ownership report. */
	FString PriorDriver;

	/** One line for the evidence file. */
	FString Describe() const;
};

/**
 * One explicit Maya-root-to-Unreal-target pairing: an actor anchor plus the
 * Skeletal Mesh Component it places. The prototype drives the component only
 * through a preview takeover, and the restore puts the component back into the
 * animation state it had before the takeover.
 */
class FMtoUMultiSubjectTarget
{
public:
	/** Anchor-once preflight; refuses socket or parent motion on the target. */
	bool Initialize(const FMtoUTargetRegistration& Registration, FString& OutError);

	const FString& GetId() const { return Id; }
	AActor* GetAnchor() const { return Anchor.Get(); }
	USkeletalMeshComponent* GetComponent() const { return Component.Get(); }
	const FMtoUAnchorCheck& GetAnchorCheck() const { return AnchorCheck; }
	const FString& GetSkeletonSignature() const { return SkeletonSignature; }
	const FMtoUTargetAnimationSnapshot& GetSnapshot() const { return Snapshot; }

	/**
	 * Strict identity of a Maya declaration against this target: the declared
	 * root plus the negotiated source-to-target mapping (necessary bones,
	 * unambiguous mapping, parent scope and bind pose). Empty when the
	 * declaration is accepted, in which case `OutMap` carries the assignment.
	 */
	FString DescribeDeclarationMismatch(
		const FMtoUSubjectDeclaration& Declaration,
		FMtoUNegotiationMap& OutMap) const;

	bool IsDriving() const { return bDriving; }

	/** Installs the prototype pose instance, remembering the state it replaces. */
	bool TakeOver(FString& OutError);

	/** Restores animation state, morph weights and editor tick settings. */
	bool Restore(FString& OutError);

	/**
	 * Publishes one subject pose and evaluates it, then measures what the
	 * component holds against what the wire carried. Returns false when the
	 * target can no longer be driven.
	 */
	bool ApplyPose(
		const FMtoUSubjectDeclaration& Declaration,
		const FMtoUNegotiationMap& Map,
		const FMtoUFrameSubject& Frame,
		FMtoUSubjectMeasurement& OutMeasurement,
		FString& OutError);

	/** True while a pose of this session has been published at least once. */
	bool HasPose() const { return bHasPose; }

	/**
	 * True when the last completed exit had to put the component back into its
	 * reference pose because nothing else was driving it. It is cleared by the
	 * next takeover, so it always describes the target's *current* ownership
	 * state and never an earlier session's exit.
	 */
	bool DidRestoreToReferencePose() const { return !bDriving && bRestoredToReferencePose; }

	/**
	 * How this target is driven right now: `preview_active` while the preview
	 * owns it, `reference_pose` / `own_driver_restored` after an exit, and
	 * `not_taken_over` before the first takeover. The report reads this instead
	 * of combining a historical flag with the current driving state.
	 */
	const TCHAR* DescribeExitState() const
	{
		if (bDriving)
		{
			return TEXT("preview_active");
		}
		if (!bTakenOver)
		{
			return TEXT("not_taken_over");
		}
		return bRestoredToReferencePose ? TEXT("reference_pose") : TEXT("own_driver_restored");
	}

private:
	FMtoUTargetRegistration Registration;
	FString Id;
	TWeakObjectPtr<AActor> Anchor;
	TWeakObjectPtr<USkeletalMeshComponent> Component;
	FMtoUAnchorCheck AnchorCheck;
	FString SkeletonSignature;
	FMtoUTargetAnimationSnapshot Snapshot;
	TWeakObjectPtr<UMtoUMultiSubjectPoseInstance> PoseInstance;
	bool bDriving = false;
	bool bHasPose = false;
	bool bRestoredToReferencePose = false;
	/** True once this target has gone through at least one takeover. */
	bool bTakenOver = false;
};

/** Largest observed difference of two component-space transforms. */
double MtoUSubjectTransformDelta(const FTransform& Expected, const FTransform& Actual);
