// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "MtoUMultiSubjectPoseInstance.h"

#include "BonePose.h"

void FMtoUMultiSubjectPoseProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);

	// The publication happens on the game thread right before PreUpdate, so the
	// proxy takes its own reference and Evaluate never touches the instance.
	const UMtoUMultiSubjectPoseInstance* Instance =
		Cast<UMtoUMultiSubjectPoseInstance>(InAnimInstance);
	if (Instance != nullptr)
	{
		CurrentPose = Instance->PendingPose;
		PoseRevision = Instance->PoseRevision;
	}
}

bool FMtoUMultiSubjectPoseProxy::Evaluate(FPoseContext& Output)
{
	const FMtoUSubjectPose* Pose = CurrentPose.Get();
	if (Pose == nullptr)
	{
		// No pose published yet: keep whatever the component already holds.
		return false;
	}

	const FBoneContainer& BoneContainer = Output.Pose.GetBoneContainer();
	for (int32 BoneIndex = 0; BoneIndex < Pose->LocalTransforms.Num(); ++BoneIndex)
	{
		if (!Pose->BoneIndices.IsValidIndex(BoneIndex))
		{
			continue;
		}
		// The target bone index comes from the negotiation map, so a target whose
		// skeleton repeats a name can never be driven on the wrong bone by a
		// name lookup.
		const int32 TargetBoneIndex = Pose->BoneIndices[BoneIndex];
		if (TargetBoneIndex == INDEX_NONE)
		{
			continue;
		}
		const FCompactPoseBoneIndex CompactIndex =
			BoneContainer.MakeCompactPoseIndex(FMeshPoseBoneIndex(TargetBoneIndex));
		if (CompactIndex != INDEX_NONE)
		{
			Output.Pose[CompactIndex] = Pose->LocalTransforms[BoneIndex];
		}
	}
	return true;
}

FAnimInstanceProxy* UMtoUMultiSubjectPoseInstance::CreateAnimInstanceProxy()
{
	return new FMtoUMultiSubjectPoseProxy(this);
}

void UMtoUMultiSubjectPoseInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	Super::DestroyAnimInstanceProxy(InProxy);
}

void UMtoUMultiSubjectPoseInstance::SetSubjectPose(const FMtoUSubjectPose& Pose)
{
	PendingPose = MakeShared<FMtoUSubjectPose, ESPMode::ThreadSafe>(Pose);
	++PoseRevision;
}

void UMtoUMultiSubjectPoseInstance::ClearSubjectPose()
{
	PendingPose.Reset();
	++PoseRevision;
}

int64 UMtoUMultiSubjectPoseInstance::GetEvaluatedPoseRevision() const
{
	return GetProxyOnGameThread<FMtoUMultiSubjectPoseProxy>().PoseRevision;
}
