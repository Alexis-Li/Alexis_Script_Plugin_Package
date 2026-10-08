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
	 * root, the skeleton names, and every parent. Empty when the declaration is
	 * accepted. Curves are checked separately so they get their own code.
	 */
	FString DescribeDeclarationMismatch(const FMtoUSubjectDeclaration& Declaration) const;

	/** The first declared curve that is not a Morph Target of the target mesh. */
	bool FindMissingCurve(const FMtoUSubjectDeclaration& Declaration, FName& OutMissing) const;

	bool IsDriving() const { return bDriving; }

	/** Installs the prototype pose instance, remembering the state it replaces. */
	bool TakeOver(FString& OutError);

	/** Target bones this declaration does not drive; they keep their reference pose. */
	void CollectUndrivenBones(
		const FMtoUSubjectDeclaration& Declaration,
		TArray<FName>& OutBones) const;

	/** Restores animation state, morph weights and editor tick settings. */
	bool Restore(FString& OutError);

	/**
	 * Publishes one subject pose and evaluates it, then measures what the
	 * component holds against what the wire carried. Returns false when the
	 * target can no longer be driven.
	 */
	bool ApplyPose(
		const FMtoUSubjectDeclaration& Declaration,
		const FMtoUFrameSubject& Frame,
		FMtoUSubjectMeasurement& OutMeasurement,
		FString& OutError);

	/** True while a pose of this session has been published at least once. */
	bool HasPose() const { return bHasPose; }

	/**
	 * True when the last restore had to put the component back into its
	 * reference pose because nothing else was driving it. Without that step a
	 * target with no animation driver would keep the preview's last pose.
	 */
	bool DidRestoreToReferencePose() const { return bRestoredToReferencePose; }

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
};

/** Largest observed difference of two component-space transforms. */
double MtoUSubjectTransformDelta(const FTransform& Expected, const FTransform& Actual);
