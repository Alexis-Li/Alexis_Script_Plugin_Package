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
	const FMtoUSubjectDeclaration& Declaration) const
{
	const USkeletalMeshComponent* ResolvedComponent = Component.Get();
	if (ResolvedComponent == nullptr || ResolvedComponent->GetSkeletalMeshAsset() == nullptr)
	{
		return FString::Printf(TEXT("target '%s' is no longer available"), *Id);
	}
	const FReferenceSkeleton& Skeleton = ResolvedComponent->GetSkeletalMeshAsset()->GetRefSkeleton();

	const FString RootLeaf = MayaLeafName(Declaration.Root);
	if (Declaration.Bones.IsEmpty() || Declaration.Bones[0].Name != FName(*RootLeaf))
	{
		return FString::Printf(
			TEXT("declared root '%s' (%s) is not the first, parentless bone '%s' of the declaration"),
			*Declaration.Root, *RootLeaf,
			Declaration.Bones.IsEmpty() ? TEXT("<none>") : *Declaration.Bones[0].Name.ToString());
	}

	const FString SkeletonDifference =
		FMtoUMultiSubjectProtocol::DescribeSkeletonMismatch(Declaration, Skeleton);
	if (!SkeletonDifference.IsEmpty())
	{
		return SkeletonDifference;
	}
	// Same names and parents is not enough: a rig resting differently would
	// deform on the same skeleton, so the advertised bind must match the target
	// reference pose within the prototype tolerance.
	return FMtoUMultiSubjectProtocol::DescribeBindMismatch(Declaration, Skeleton);
}

bool FMtoUMultiSubjectTarget::FindMissingCurve(
	const FMtoUSubjectDeclaration& Declaration,
	FName& OutMissing) const
{
	const USkeletalMeshComponent* ResolvedComponent = Component.Get();
	if (ResolvedComponent == nullptr)
	{
		OutMissing = NAME_None;
		return false;
	}
	return FMtoUMultiSubjectProtocol::FindMissingCurve(Declaration, *ResolvedComponent, OutMissing);
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
	}

	PoseInstance = nullptr;
	bDriving = false;
	bHasPose = false;
	return true;
}

bool FMtoUMultiSubjectTarget::ApplyPose(
	const FMtoUSubjectDeclaration& Declaration,
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

	FMtoUSubjectPose Pose;
	Pose.BoneNames.Reserve(Declaration.Bones.Num());
	Pose.LocalTransforms.Reserve(Frame.Transforms.Num());
	for (int32 BoneIndex = 0; BoneIndex < Declaration.Bones.Num(); ++BoneIndex)
	{
		Pose.BoneNames.Add(Declaration.Bones[BoneIndex].Name);
		Pose.LocalTransforms.Add(Frame.Transforms[BoneIndex]);
	}
	// GetMorphTarget reads the component's explicit MorphTargetCurves map,
	// not the animation proxy's curve container. Keep the values scoped to
	// this component so same-named curves cannot bleed between subjects.
	for (int32 CurveIndex = 0; CurveIndex < Declaration.Curves.Num(); ++CurveIndex)
	{
		ResolvedComponent->SetMorphTarget(Declaration.Curves[CurveIndex], Frame.Curves[CurveIndex]);
	}
	Instance->SetSubjectPose(Pose);
	bHasPose = true;

	// The editor path that turns a published pose into bone transforms: the
	// transient test world does not tick animation by itself.
	ResolvedComponent->TickAnimation(1.0f / 60.0f, false);
	ResolvedComponent->RefreshBoneTransforms();

	// Measure the applied pose against the wire values, composing the declared
	// hierarchy exactly as the animation pipeline does.
	const FReferenceSkeleton& Skeleton =
		ResolvedComponent->GetSkeletalMeshAsset()->GetRefSkeleton();
	const TArray<FTransform>& ComponentSpace = ResolvedComponent->GetComponentSpaceTransforms();
	TArray<FTransform> Expected;
	Expected.SetNum(Declaration.Bones.Num());
	for (int32 BoneIndex = 0; BoneIndex < Declaration.Bones.Num(); ++BoneIndex)
	{
		const int32 ParentIndex = Declaration.Bones[BoneIndex].Parent;
		Expected[BoneIndex] = ParentIndex == INDEX_NONE
			? Frame.Transforms[BoneIndex]
			: Frame.Transforms[BoneIndex] * Expected[ParentIndex];
	}

	OutMeasurement.Bones.Reserve(Declaration.Bones.Num());
	for (int32 BoneIndex = 0; BoneIndex < Declaration.Bones.Num(); ++BoneIndex)
	{
		const FName BoneName = Declaration.Bones[BoneIndex].Name;
		const int32 SkeletonIndex = Skeleton.FindBoneIndex(BoneName);
		FMtoUBoneMeasurement Measurement;
		Measurement.BoneName = BoneName;
		Measurement.ReceivedLocal = Frame.Transforms[BoneIndex];
		Measurement.ExpectedComponentSpace = Expected[BoneIndex];
		Measurement.ComponentSpace = ComponentSpace.IsValidIndex(SkeletonIndex)
			? ComponentSpace[SkeletonIndex]
			: FTransform::Identity;
		Measurement.Delta = MtoUSubjectTransformDelta(
			Measurement.ExpectedComponentSpace, Measurement.ComponentSpace);
		OutMeasurement.Bones.Add(Measurement);
	}

	OutMeasurement.Curves.Reserve(Declaration.Curves.Num());
	for (int32 CurveIndex = 0; CurveIndex < Declaration.Curves.Num(); ++CurveIndex)
	{
		OutMeasurement.Curves.Add(TPair<FName, float>(
			Declaration.Curves[CurveIndex],
			ResolvedComponent->GetMorphTarget(Declaration.Curves[CurveIndex])));
	}

	// Anchor applied exactly once: the root bone's world transform is the
	// component placement composed with the received Maya world root pose.
	const FTransform AnchorTransform = ResolvedComponent->GetComponentTransform();
	OutMeasurement.AnchorLocation = AnchorTransform.GetLocation();
	OutMeasurement.AnchorRotation = AnchorTransform.GetRotation();
	OutMeasurement.AnchorScale = AnchorTransform.GetScale3D();
	OutMeasurement.RootWorld = ResolvedComponent->GetBoneTransform(
		Declaration.Bones[0].Name, RTS_World);
	OutMeasurement.RootWorldDelta = MtoUSubjectTransformDelta(
		Frame.Transforms[0] * AnchorTransform, OutMeasurement.RootWorld);
	return true;
}
