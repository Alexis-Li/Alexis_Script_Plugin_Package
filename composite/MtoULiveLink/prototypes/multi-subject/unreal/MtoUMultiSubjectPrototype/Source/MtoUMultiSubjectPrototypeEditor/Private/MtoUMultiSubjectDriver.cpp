// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "MtoUMultiSubjectDriver.h"

#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "MtoUMultiSubjectPoseInstance.h"
#include "MtoUMultiSubjectProtocol.h"
#include "ReferenceSkeleton.h"

namespace
{
	/** The last Maya DAG namespace or path element: "|ns:character:Root" -> "Root". */
	FString MayaLeafName(const FString& Path)
	{
		int32 Separator = INDEX_NONE;
		for (int32 Index = Path.Len() - 1; Index >= 0; --Index)
		{
			if (Path[Index] == TEXT('|') || Path[Index] == TEXT(':'))
			{
				Separator = Index;
				break;
			}
		}
		return Separator == INDEX_NONE ? Path : Path.RightChop(Separator + 1);
	}

	FString DescribeAnimationMode(EAnimationMode::Type Mode)
	{
		switch (Mode)
		{
		case EAnimationMode::AnimationBlueprint:
			return TEXT("AnimationBlueprint");
		case EAnimationMode::AnimationSingleNode:
			return TEXT("AnimationSingleNode");
		case EAnimationMode::AnimationCustomMode:
			return TEXT("AnimationCustomMode");
		default:
			return FString::Printf(TEXT("mode_%d"), static_cast<int32>(Mode));
		}
	}
}

double MtoUSubjectTransformDelta(const FTransform& Expected, const FTransform& Actual)
{
	const double TranslationDelta = (Expected.GetTranslation() - Actual.GetTranslation()).GetAbsMax();
	const double ScaleDelta = (Expected.GetScale3D() - Actual.GetScale3D()).GetAbsMax();
	const double RotationDelta = Expected.GetRotation().AngularDistance(Actual.GetRotation());
	return FMath::Max3(TranslationDelta, ScaleDelta, RotationDelta);
}

FString FMtoUTargetAnimationSnapshot::Describe() const
{
	TArray<FString> Parts;
	Parts.Add(FString::Printf(TEXT("mode=%s"), *DescribeAnimationMode(AnimationMode)));
	Parts.Add(FString::Printf(TEXT("anim_class=%s"),
		AnimClass != nullptr ? *AnimClass->GetName() : TEXT("none")));
	Parts.Add(FString::Printf(TEXT("anim_to_play=%s"),
		AnimToPlay != nullptr ? *AnimToPlay->GetName() : TEXT("none")));
	Parts.Add(FString::Printf(TEXT("single_node_playing=%s"), bWasPlaying ? TEXT("true") : TEXT("false")));
	Parts.Add(FString::Printf(TEXT("paused=%s"), bPauseAnims ? TEXT("true") : TEXT("false")));
	Parts.Add(FString::Printf(TEXT("update_in_editor=%s"), bUpdateAnimationInEditor ? TEXT("true") : TEXT("false")));
	Parts.Add(FString::Printf(TEXT("post_process_disabled=%s"),
		bDisablePostProcessBlueprint ? TEXT("true") : TEXT("false")));
	return FString::Join(Parts, TEXT(", "));
}

bool FMtoUMultiSubjectTarget::Initialize(
	const FMtoUTargetRegistration& InRegistration,
	FString& OutError)
{
	Registration = InRegistration;
	Id = InRegistration.Id;
	Anchor = InRegistration.Anchor;
	Component = InRegistration.Component;
	AnchorCheck = FMtoUAnchorCheck();

	AActor* ResolvedAnchor = Anchor.Get();
	USkeletalMeshComponent* ResolvedComponent = Component.Get();
	if (ResolvedAnchor == nullptr || ResolvedComponent == nullptr)
	{
		OutError = FString::Printf(
			TEXT("target '%s' is missing its anchor actor or its skeletal mesh component"), *Id);
		return false;
	}
	if (ResolvedComponent->GetSkeletalMeshAsset() == nullptr)
	{
		OutError = FString::Printf(TEXT("target '%s' has no Skeletal Mesh"), *Id);
		return false;
	}
	if (ResolvedComponent->GetOwner() != ResolvedAnchor)
	{
		OutError = FString::Printf(
			TEXT("target '%s' is not owned by its anchor actor; one object may only have one anchor"),
			*Id);
		return false;
	}

	AnchorCheck.AnchorTransform = ResolvedComponent->GetComponentTransform();
	AnchorCheck.SocketName = ResolvedComponent->GetAttachSocketName();
	AnchorCheck.bActorAttached = ResolvedAnchor->GetAttachParentActor() != nullptr;
	AnchorCheck.bAttachedToOwnRoot = ResolvedComponent->GetAttachParent() == nullptr
		|| ResolvedComponent->GetAttachParent() == ResolvedAnchor->GetRootComponent();

	if (ResolvedAnchor->GetAttachParentActor() != nullptr)
	{
		// Actor attachment applies the parent's motion as well, so the Maya world
		// root pose would be applied once by Maya and once by the parent.
		OutError = FString::Printf(
			TEXT("target '%s' has an anchor actor attached to '%s'; anchor-once requires an unattached anchor"),
			*Id, *ResolvedAnchor->GetAttachParentActor()->GetActorNameOrLabel());
		return false;
	}
	if (ResolvedComponent->GetAttachSocketName() != NAME_None)
	{
		OutError = FString::Printf(
			TEXT("target '%s' is attached at socket '%s'; a socket already carries the parent motion, so the Maya world root pose would be applied twice"),
			*Id, *ResolvedComponent->GetAttachSocketName().ToString());
		return false;
	}
	if (!AnchorCheck.bAttachedToOwnRoot)
	{
		const USceneComponent* AttachParent = ResolvedComponent->GetAttachParent();
		OutError = FString::Printf(
			TEXT("target '%s' is attached to '%s' instead of its own anchor; that parent motion would be applied on top of the Maya world root pose"),
			*Id, AttachParent != nullptr ? *AttachParent->GetName() : TEXT("<none>"));
		return false;
	}

	AnchorCheck.bValid = true;
	SkeletonSignature = FMtoUMultiSubjectProtocol::DescribeSkeletonSignature(
		ResolvedComponent->GetSkeletalMeshAsset()->GetRefSkeleton());
	return true;
}

FString FMtoUMultiSubjectTarget::DescribeDeclarationMismatch(
	const FMtoUSubjectDeclaration& Declaration,
	FMtoUNegotiationMap& OutMap) const
{
	OutMap = FMtoUNegotiationMap();
	const USkeletalMeshComponent* ResolvedComponent = Component.Get();
	if (ResolvedComponent == nullptr || ResolvedComponent->GetSkeletalMeshAsset() == nullptr)
	{
		return FString::Printf(TEXT("target '%s' is no longer available"), *Id);
	}
	const USkeletalMesh* Mesh = ResolvedComponent->GetSkeletalMeshAsset();

	const FString RootLeaf = MayaLeafName(Declaration.Root);
	if (Declaration.Bones.IsEmpty() || Declaration.Bones[0].Name != FName(*RootLeaf))
	{
		return FString::Printf(
			TEXT("declared root '%s' (%s) is not the first, parentless bone '%s' of the declaration"),
			*Declaration.Root, *RootLeaf,
			Declaration.Bones.IsEmpty() ? TEXT("<none>") : *Declaration.Bones[0].Name.ToString());
	}
	// The mapping is the whole identity check: every required target bone has to
	// be covered unambiguously, and the mapped bones carry the advertised bind.
	return FMtoUMultiSubjectProtocol::DescribeNegotiationMismatch(Declaration, *Mesh, OutMap);
}

bool FMtoUMultiSubjectTarget::TakeOver(FString& OutError)
{
	USkeletalMeshComponent* ResolvedComponent = Component.Get();
	if (ResolvedComponent == nullptr)
	{
		OutError = FString::Printf(TEXT("target '%s' disappeared before the preview could take it over"), *Id);
		return false;
	}

	if (bDriving)
	{
		return true;
	}

	// Snapshot before anything is touched: this is what Restore puts back.
	Snapshot = FMtoUTargetAnimationSnapshot();
	Snapshot.AnimationMode = ResolvedComponent->GetAnimationMode();
	Snapshot.AnimClass = ResolvedComponent->AnimClass;
	if (UAnimSingleNodeInstance* SingleNode = ResolvedComponent->GetSingleNodeInstance())
	{
		Snapshot.AnimToPlay = SingleNode->GetCurrentAsset();
		Snapshot.bWasPlaying = SingleNode->IsPlaying();
		Snapshot.bWasLooping = SingleNode->IsLooping();
		Snapshot.Position = ResolvedComponent->GetPosition();
		Snapshot.PlayRate = ResolvedComponent->GetPlayRate();
	}
	Snapshot.bPauseAnims = ResolvedComponent->bPauseAnims != 0;
	Snapshot.bUpdateAnimationInEditor = ResolvedComponent->GetUpdateAnimationInEditor();
	Snapshot.bDisablePostProcessBlueprint = ResolvedComponent->GetDisablePostProcessBlueprint();
	// Drive ownership is part of the contract: a component that was driven by
	// something else must go back to that driver, and a component nothing drove
	// has no driver to go back to, so its exit pose has to be the reference pose.
	Snapshot.bHadAnimationDriver =
		(Snapshot.AnimationMode == EAnimationMode::AnimationSingleNode && Snapshot.AnimToPlay != nullptr)
		|| (Snapshot.AnimationMode == EAnimationMode::AnimationBlueprint && Snapshot.AnimClass != nullptr);
	Snapshot.PriorDriver = Snapshot.bHadAnimationDriver
		? Snapshot.Describe()
		: FString(TEXT("none (reference pose, no animation driver)"));
	Snapshot.VisibilityBasedAnimTickOption = ResolvedComponent->VisibilityBasedAnimTickOption;
	if (const USkeletalMesh* Mesh = ResolvedComponent->GetSkeletalMeshAsset())
	{
		for (const TObjectPtr<UMorphTarget>& Morph : Mesh->GetMorphTargets())
		{
			if (Morph != nullptr)
			{
				Snapshot.MorphWeights.Add(
					Morph->GetFName(), ResolvedComponent->GetMorphTarget(Morph->GetFName()));
			}
		}
	}

	bDriving = true;
	ResolvedComponent->bPauseAnims = false;
	ResolvedComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	ResolvedComponent->SetUpdateAnimationInEditor(true);
	ResolvedComponent->SetDisablePostProcessBlueprint(true);
	ResolvedComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	ResolvedComponent->SetAnimInstanceClass(UMtoUMultiSubjectPoseInstance::StaticClass());

	UMtoUMultiSubjectPoseInstance* Instance =
		Cast<UMtoUMultiSubjectPoseInstance>(ResolvedComponent->GetAnimInstance());
	if (Instance == nullptr)
	{
		OutError = FString::Printf(
			TEXT("target '%s' could not install the prototype pose instance"), *Id);
		FString RestoreError;
		Restore(RestoreError);
		return false;
	}
	PoseInstance = Instance;
	bHasPose = false;
	// A new takeover replaces any previous session's exit result: the ownership
	// report must describe who drives the target now, not how an earlier
	// session ended.
	bTakenOver = true;
	bRestoredToReferencePose = false;
	return true;
}

bool FMtoUMultiSubjectTarget::Restore(FString& OutError)
{
	OutError.Reset();
	if (!bDriving)
	{
		return true;
	}

	USkeletalMeshComponent* ResolvedComponent = Component.Get();
	if (ResolvedComponent != nullptr)
	{
		// Morph weights the preview may have driven: put every mesh morph back.
		if (const USkeletalMesh* Mesh = ResolvedComponent->GetSkeletalMeshAsset())
		{
			for (const TObjectPtr<UMorphTarget>& Morph : Mesh->GetMorphTargets())
			{
				if (Morph == nullptr)
				{
					continue;
				}
				const float* Saved = Snapshot.MorphWeights.Find(Morph->GetFName());
				ResolvedComponent->SetMorphTarget(Morph->GetFName(), Saved != nullptr ? *Saved : 0.0f);
			}
		}

		// The animation state the preview replaced.
		ResolvedComponent->SetAnimationMode(Snapshot.AnimationMode);
		if (Snapshot.AnimationMode == EAnimationMode::AnimationSingleNode)
		{
			ResolvedComponent->SetAnimation(Snapshot.AnimToPlay);
			if (Snapshot.AnimToPlay != nullptr)
			{
				ResolvedComponent->SetPlayRate(Snapshot.PlayRate);
				ResolvedComponent->SetPosition(Snapshot.Position, false);
				ResolvedComponent->Play(Snapshot.bWasLooping);
				if (!Snapshot.bWasPlaying)
				{
					ResolvedComponent->Stop();
				}
			}
		}
		else
		{
			ResolvedComponent->SetAnimInstanceClass(Snapshot.AnimClass);
		}

		ResolvedComponent->SetUpdateAnimationInEditor(Snapshot.bUpdateAnimationInEditor);
		ResolvedComponent->SetDisablePostProcessBlueprint(Snapshot.bDisablePostProcessBlueprint);
		ResolvedComponent->VisibilityBasedAnimTickOption = Snapshot.VisibilityBasedAnimTickOption;
		ResolvedComponent->bPauseAnims = Snapshot.bPauseAnims;

		// Nothing else was driving this component, so leaving the preview's last
		// pose on it would look like a stuck preview after the exit.
		bRestoredToReferencePose = !Snapshot.bHadAnimationDriver && bHasPose;
		if (bRestoredToReferencePose)
		{
			// With no animation driver left, re-initialising the component is what
			// puts its bones back on the reference pose.
			ResolvedComponent->InitAnim(true);
			ResolvedComponent->RefreshBoneTransforms();
		}
	}
	else
	{
		bRestoredToReferencePose = false;
	}

	PoseInstance = nullptr;
	bDriving = false;
	bHasPose = false;
	return true;
}

bool FMtoUMultiSubjectTarget::ApplyPose(
	const FMtoUSubjectDeclaration& Declaration,
	const FMtoUNegotiationMap& Map,
	const FMtoUFrameSubject& Frame,
	FMtoUSubjectMeasurement& OutMeasurement,
	FString& OutError)
{
	OutMeasurement = FMtoUSubjectMeasurement();
	OutMeasurement.Id = Id;

	USkeletalMeshComponent* ResolvedComponent = Component.Get();
	UMtoUMultiSubjectPoseInstance* Instance = PoseInstance.Get();
	if (ResolvedComponent == nullptr || Instance == nullptr || !bDriving)
	{
		OutError = FString::Printf(
			TEXT("target '%s' is no longer under preview control"), *Id);
		return false;
	}
	if (Frame.Transforms.Num() != Declaration.Bones.Num()
		|| Frame.Curves.Num() != Declaration.Curves.Num())
	{
		OutError = FString::Printf(
			TEXT("target '%s' received %d transforms and %d curves for %d bones and %d curves"),
			*Id, Frame.Transforms.Num(), Frame.Curves.Num(),
			Declaration.Bones.Num(), Declaration.Curves.Num());
		return false;
	}
	if (Map.SourceToTarget.Num() != Declaration.Bones.Num())
	{
		OutError = FString::Printf(
			TEXT("target '%s' has no negotiation map for its declaration"), *Id);
		return false;
	}

	// Only mapped bones are published: a declared export branch the target does
	// not own has no target bone to drive. The target bone index comes from the
	// map, so the pose never depends on a name lookup.
	// The product's bind/frame projection: the target keeps its own reference
	// pose and receives the source's motion, measured against the source's own
	// bind. A rig whose root joint carries an import convention (a rotated
	// skeleton root) therefore cannot tilt the target, and a rest difference
	// that the negotiation accepted is absorbed here instead of deforming.
	TArray<FTransform> SourceCurrentComponent;
	SourceCurrentComponent.Init(FTransform::Identity, Map.SourceToTarget.Num());
	TArray<FTransform> TargetCurrentComponent;
	TargetCurrentComponent.Init(FTransform::Identity, Map.SourceToTarget.Num());
	TArray<FTransform> PublishedLocal;
	PublishedLocal.Init(FTransform::Identity, Map.SourceToTarget.Num());
	for (int32 SourceIndex = 0; SourceIndex < Map.SourceToTarget.Num(); ++SourceIndex)
	{
		if (Map.SourceToTarget[SourceIndex] == INDEX_NONE)
		{
			continue;
		}
		const int32 Parent = Declaration.Bones[SourceIndex].Parent;
		const FTransform& SourceCurrentLocal = Frame.Transforms[SourceIndex];
		SourceCurrentComponent[SourceIndex] = Parent == INDEX_NONE
			? SourceCurrentLocal
			: SourceCurrentLocal * SourceCurrentComponent[Parent];
		TargetCurrentComponent[SourceIndex] = Map.TargetRefComponentPose[SourceIndex]
			* Map.SourceBindComponentPose[SourceIndex].Inverse()
			* SourceCurrentComponent[SourceIndex];
		PublishedLocal[SourceIndex] = Parent == INDEX_NONE
			? TargetCurrentComponent[SourceIndex]
			: TargetCurrentComponent[SourceIndex] * TargetCurrentComponent[Parent].Inverse();
	}

	FMtoUSubjectPose Pose;
	Pose.BoneNames.Reserve(Map.DrivenTargetBones.Num());
	Pose.BoneIndices.Reserve(Map.DrivenTargetBones.Num());
	Pose.LocalTransforms.Reserve(Map.DrivenTargetBones.Num());
	for (int32 SourceIndex = 0; SourceIndex < Map.SourceToTarget.Num(); ++SourceIndex)
	{
		const int32 TargetIndex = Map.SourceToTarget[SourceIndex];
		if (TargetIndex == INDEX_NONE)
		{
			continue;
		}
		Pose.BoneNames.Add(Declaration.Bones[SourceIndex].Name);
		Pose.BoneIndices.Add(TargetIndex);
		Pose.LocalTransforms.Add(PublishedLocal[SourceIndex]);
	}
	// GetMorphTarget reads the component's explicit MorphTargetCurves map,
	// not the animation proxy's curve container. Keep the values scoped to
	// this component so same-named curves cannot bleed between subjects; a
	// declared curve the target has no Morph Target for is not applied.
	for (int32 CurveIndex = 0; CurveIndex < Declaration.Curves.Num(); ++CurveIndex)
	{
		const FName& Curve = Declaration.Curves[CurveIndex];
		if (Map.SourceOnlyCurves.Contains(Curve))
		{
			continue;
		}
		ResolvedComponent->SetMorphTarget(Curve, Frame.Curves[CurveIndex]);
	}
	Instance->SetSubjectPose(Pose);
	bHasPose = true;

	// The editor path that turns a published pose into bone transforms: the
	// transient test world does not tick animation by itself.
	ResolvedComponent->TickAnimation(1.0f / 60.0f, false);
	ResolvedComponent->RefreshBoneTransforms();

	// Measure the applied pose against the projected pose the wire describes.
	// Only the mapped bones are measured; an ignored export branch has no
	// target bone to compare.
	const TArray<FTransform>& ComponentSpace = ResolvedComponent->GetComponentSpaceTransforms();
	OutMeasurement.Bones.Reserve(Map.DrivenTargetBones.Num());
	for (int32 SourceIndex = 0; SourceIndex < Map.SourceToTarget.Num(); ++SourceIndex)
	{
		const int32 TargetIndex = Map.SourceToTarget[SourceIndex];
		if (TargetIndex == INDEX_NONE)
		{
			continue;
		}
		FMtoUBoneMeasurement Measurement;
		Measurement.BoneName = Declaration.Bones[SourceIndex].Name;
		Measurement.ReceivedLocal = Frame.Transforms[SourceIndex];
		Measurement.ExpectedComponentSpace = TargetCurrentComponent[SourceIndex];
		Measurement.ComponentSpace = ComponentSpace.IsValidIndex(TargetIndex)
			? ComponentSpace[TargetIndex]
			: FTransform::Identity;
		Measurement.Delta = MtoUSubjectTransformDelta(
			Measurement.ExpectedComponentSpace, Measurement.ComponentSpace);
		OutMeasurement.Bones.Add(Measurement);
	}

	OutMeasurement.Curves.Reserve(Declaration.Curves.Num());
	for (int32 CurveIndex = 0; CurveIndex < Declaration.Curves.Num(); ++CurveIndex)
	{
		const FName& Curve = Declaration.Curves[CurveIndex];
		if (Map.SourceOnlyCurves.Contains(Curve))
		{
			continue;
		}
		OutMeasurement.Curves.Add(TPair<FName, float>(
			Curve, ResolvedComponent->GetMorphTarget(Curve)));
	}

	// Anchor applied exactly once: the root bone's world transform is the
	// component placement composed with the projected root pose.
	const FTransform AnchorTransform = ResolvedComponent->GetComponentTransform();
	OutMeasurement.AnchorLocation = AnchorTransform.GetLocation();
	OutMeasurement.AnchorRotation = AnchorTransform.GetRotation();
	OutMeasurement.AnchorScale = AnchorTransform.GetScale3D();
	const int32 RootTargetIndex = Map.SourceToTarget.IsValidIndex(0)
		? Map.SourceToTarget[0]
		: INDEX_NONE;
	OutMeasurement.RootWorld = RootTargetIndex != INDEX_NONE
		? ResolvedComponent->GetBoneTransform(Declaration.Bones[0].Name, RTS_World)
		: FTransform::Identity;
	OutMeasurement.RootWorldDelta = MtoUSubjectTransformDelta(
		TargetCurrentComponent[0] * AnchorTransform, OutMeasurement.RootWorld);
	return true;
}
