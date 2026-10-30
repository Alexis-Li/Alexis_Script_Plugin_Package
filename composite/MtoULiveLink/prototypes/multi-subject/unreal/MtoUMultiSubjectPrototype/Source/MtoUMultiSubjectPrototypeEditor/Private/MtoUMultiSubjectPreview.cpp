// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "MtoUMultiSubjectPreview.h"

#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ISequencer.h"
#include "MovieSceneSequencePlayer.h"
#include "MovieSceneTrack.h"
#include "MtoUMultiSubjectDriver.h"

void FMtoUMultiSubjectPreview::DisableWriters(const TArray<FMtoUPreviewWriter>& Writers)
{
	SuppressedWriters.Reset();
	for (const FMtoUPreviewWriter& Writer : Writers)
	{
		FMtoUSuppressedWriter Suppressed;
		Suppressed.TargetId = Writer.TargetId;
		Suppressed.Track = Writer.Track;
		if (UMovieSceneTrack* Track = Writer.Track.Get())
		{
			Suppressed.bTrackWasLocallyDisabled = Track->IsLocalEvalDisabled();
			Suppressed.bTrackDisabled = true;
			// Reporting only: a track that belongs to a saved package is the
			// productization risk the prototype must name, never write.
			const FString PackageName = Track->GetOutermost() != nullptr
				? Track->GetOutermost()->GetName()
				: FString();
			Suppressed.bRepresentsSavedAsset =
				!PackageName.StartsWith(TEXT("/Temp/"))
				&& !PackageName.StartsWith(TEXT("/Engine/Transient"));
			// The local flag is not serialized with the asset, so a preview can
			// never mute a Level Sequence permanently.
			Track->SetLocalEvalDisabled(true);
		}
		Suppressed.Player = Writer.Player;
		if (UMovieSceneSequencePlayer* Player = Writer.Player.Get())
		{
			Suppressed.bPlayerWasPlaying = Player->IsPlaying();
			if (Suppressed.bPlayerWasPlaying)
			{
				Player->Pause();
				Suppressed.bPlayerPaused = true;
			}
		}
		SuppressedWriters.Add(Suppressed);
	}
}

void FMtoUMultiSubjectPreview::RestoreWriters(
	const FString& TargetId,
	const TSharedPtr<ISequencer>& EditorSequencer)
{
	bool bRestoredAny = false;
	for (FMtoUSuppressedWriter& Suppressed : SuppressedWriters)
	{
		// An empty target id is session-wide, so it is restored with the session.
		const bool bBelongsToTarget = Suppressed.TargetId == TargetId
			|| TargetId.IsEmpty();
		if (Suppressed.bRestored || !bBelongsToTarget)
		{
			continue;
		}
		if (Suppressed.bTrackDisabled)
		{
			if (UMovieSceneTrack* Track = Suppressed.Track.Get())
			{
				Track->SetLocalEvalDisabled(Suppressed.bTrackWasLocallyDisabled);
			}
			Suppressed.bTrackDisabled = false;
		}
		if (Suppressed.bPlayerPaused)
		{
			if (UMovieSceneSequencePlayer* Player = Suppressed.Player.Get())
			{
				Player->Play();
			}
			Suppressed.bPlayerPaused = false;
		}
		Suppressed.bRestored = true;
		bRestoredAny = true;
	}
	// Everything is enabled again: one evaluation re-establishes the writer's
	// own pose instead of leaving the preview's last pose behind.
	if (bRestoredAny && EditorSequencer.IsValid())
	{
		EditorSequencer->ForceEvaluate();
	}
}

bool FMtoUMultiSubjectPreview::TakeOver(
	const TArray<FMtoUMultiSubjectTarget*>& Targets,
	const TArray<FMtoUPreviewWriter>& Writers,
	const TSharedPtr<ISequencer>& EditorSequencer,
	FString& OutError)
{
	OutError.Reset();
	if (bActive)
	{
		OutError = TEXT("the preview is already active");
		return false;
	}

	bActive = true;
	DisableWriters(Writers);
	// With the tracks locally disabled this evaluation releases the components
	// the writers were driving, so the takeover never fights a stale writer.
	if (EditorSequencer.IsValid())
	{
		EditorSequencer->ForceEvaluate();
	}

	ActiveTargets.Reset();
	for (FMtoUMultiSubjectTarget* Target : Targets)
	{
		FString TargetError;
		if (Target == nullptr || !Target->TakeOver(TargetError))
		{
			OutError = TargetError.IsEmpty()
				? FString(TEXT("a target could not be taken over"))
				: TargetError;
			for (FMtoUMultiSubjectTarget* TakenTarget : ActiveTargets)
			{
				FString RollbackError;
				TakenTarget->Restore(RollbackError);
			}
			ActiveTargets.Reset();
			RestoreWriters(FString(), EditorSequencer);
			bActive = false;
			return false;
		}
		ActiveTargets.Add(Target);
	}
	return true;
}

bool FMtoUMultiSubjectPreview::RestoreTarget(
	FMtoUMultiSubjectTarget& Target,
	const TSharedPtr<ISequencer>& EditorSequencer,
	FString& OutError)
{
	OutError.Reset();
	// The preview instance has to be gone before the writer can bind its own
	// animation instance again.
	if (!Target.Restore(OutError))
	{
		return false;
	}
	ActiveTargets.Remove(&Target);
	RestoreWriters(Target.GetId(), EditorSequencer);
	return true;
}

bool FMtoUMultiSubjectPreview::Restore(
	const TSharedPtr<ISequencer>& EditorSequencer,
	FString& OutError)
{
	OutError.Reset();
	if (!bActive)
	{
		return true;
	}

	// Targets first, each with its own writers; then anything session-wide.
	const TArray<FMtoUMultiSubjectTarget*> TargetsToRestore = ActiveTargets;
	for (FMtoUMultiSubjectTarget* Target : TargetsToRestore)
	{
		if (Target == nullptr)
		{
			continue;
		}
		FString TargetError;
		if (!RestoreTarget(*Target, EditorSequencer, TargetError) && OutError.IsEmpty())
		{
			OutError = TargetError;
		}
	}
	ActiveTargets.Reset();
	RestoreWriters(FString(), EditorSequencer);
	bActive = false;
	return OutError.IsEmpty();
}

bool FMtoUMultiSubjectPreview::AreWritersSuppressed() const
{
	for (const FMtoUSuppressedWriter& Suppressed : SuppressedWriters)
	{
		UMovieSceneTrack* Track = Suppressed.Track.Get();
		if (Suppressed.bTrackDisabled && (Track == nullptr || !Track->IsLocalEvalDisabled()))
		{
			return false;
		}
	}
	return true;
}

void MtoUCapturePoseSnapshot(const USkeletalMeshComponent& Component, FMtoUPoseSnapshot& OutSnapshot)
{
	OutSnapshot = FMtoUPoseSnapshot();
	OutSnapshot.ComponentSpaceTransforms = Component.GetComponentSpaceTransforms();
	if (const USkeletalMesh* Mesh = Component.GetSkeletalMeshAsset())
	{
		for (const TObjectPtr<UMorphTarget>& Morph : Mesh->GetMorphTargets())
		{
			if (Morph != nullptr)
			{
				OutSnapshot.MorphWeights.Add(
					TPair<FName, float>(Morph->GetFName(), Component.GetMorphTarget(Morph->GetFName())));
			}
		}
	}
	const UAnimInstance* Instance = Component.GetAnimInstance();
	OutSnapshot.AnimInstanceClass = Instance != nullptr ? Instance->GetClass()->GetFName() : NAME_None;
}

double FMtoUPoseSnapshot::MaxDelta(const FMtoUPoseSnapshot& Other) const
{
	double Delta = 0.0;
	const int32 BoneCount = FMath::Min(ComponentSpaceTransforms.Num(), Other.ComponentSpaceTransforms.Num());
	for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
	{
		Delta = FMath::Max(Delta, MtoUSubjectTransformDelta(
			ComponentSpaceTransforms[BoneIndex], Other.ComponentSpaceTransforms[BoneIndex]));
	}
	for (const TPair<FName, float>& Entry : MorphWeights)
	{
		const TPair<FName, float>* OtherValue = Other.MorphWeights.FindByPredicate(
			[&Entry](const TPair<FName, float>& Candidate) { return Candidate.Key == Entry.Key; });
		const float DeltaValue = OtherValue != nullptr
			? FMath::Abs(OtherValue->Value - Entry.Value)
			: FMath::Abs(Entry.Value);
		Delta = FMath::Max(Delta, static_cast<double>(DeltaValue));
	}
	return Delta;
}

FString FMtoUPoseSnapshot::Describe() const
{
	return FString::Printf(TEXT("bones=%d morphs=%d instance=%s"),
		ComponentSpaceTransforms.Num(), MorphWeights.Num(), *AnimInstanceClass.ToString());
}
