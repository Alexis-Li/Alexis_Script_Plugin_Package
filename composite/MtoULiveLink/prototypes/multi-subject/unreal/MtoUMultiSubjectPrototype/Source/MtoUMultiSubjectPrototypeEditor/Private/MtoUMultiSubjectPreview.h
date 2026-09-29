// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "MtoUMultiSubjectTypes.h"

class FMtoUMultiSubjectTarget;
class ISequencer;
class UMovieSceneSequencePlayer;
class UMovieSceneTrack;
class USkeletalMeshComponent;

/** Reversible suppression record of one writer the preview took over from. */
struct FMtoUSuppressedWriter
{
	/** Subject id this writer belongs to; empty means session-wide. */
	FString TargetId;
	bool bRestored = false;
	TWeakObjectPtr<UMovieSceneTrack> Track;
	bool bTrackWasLocallyDisabled = false;
	bool bTrackDisabled = false;
	TWeakObjectPtr<UMovieSceneSequencePlayer> Player;
	bool bPlayerWasPlaying = false;
	bool bPlayerPaused = false;
	bool bRepresentsSavedAsset = false;
};

/**
 * Explicit preview takeover for the bounded prototype.
 *
 * While the takeover is active the preview pose is the only writer of every
 * target: the writers handed in are disabled with the *local* evaluation flag
 * (`UMovieSceneTrack::SetLocalEvalDisabled`), which is not serialized with the
 * Level Sequence asset, and the open Sequencer is evaluated once so its
 * skeletal animation system releases the components instead of leaving a stale
 * pose behind. Exiting restores every target and every writer.
 *
 * This is the bounded answer to "one explicit driver at a time". Suppressing a
 * writer that belongs to a saved Level Sequence is an editor-local mute that is
 * invisible in the Sequencer UI, so the prototype reports it as a productization
 * risk; it never writes the asset.
 */
class FMtoUMultiSubjectPreview
{
public:
	/**
	 * Disables the writers, releases the components they were driving, and
	 * installs the prototype pose instance on every target.
	 */
	bool TakeOver(
		const TArray<FMtoUMultiSubjectTarget*>& Targets,
		const TArray<FMtoUPreviewWriter>& Writers,
		const TSharedPtr<ISequencer>& EditorSequencer,
		FString& OutError);

	/**
	 * Restores one target and the writers that belong to its subject, so removing
	 * a single subject does not leave its writer muted while the other subject
	 * keeps streaming.
	 */
	bool RestoreTarget(
		FMtoUMultiSubjectTarget& Target,
		const TSharedPtr<ISequencer>& EditorSequencer,
		FString& OutError);

	/** Restores every target and every writer; the preview stops driving. */
	bool Restore(const TSharedPtr<ISequencer>& EditorSequencer, FString& OutError);

	bool IsActive() const { return bActive; }
	const TArray<FMtoUSuppressedWriter>& GetSuppressedWriters() const { return SuppressedWriters; }

	/** True when every handed-in track currently reports the local mute we set. */
	bool AreWritersSuppressed() const;

private:
	void DisableWriters(const TArray<FMtoUPreviewWriter>& Writers);
	void RestoreWriters(const FString& TargetId, const TSharedPtr<ISequencer>& EditorSequencer);

	bool bActive = false;
	/** Targets this takeover installed the pose instance on. */
	TArray<FMtoUMultiSubjectTarget*> ActiveTargets;
	TArray<FMtoUSuppressedWriter> SuppressedWriters;
};

/**
 * Pose snapshot used for the before/active/after evidence of the Sequencer
 * takeover: component-space transforms and Morph Target weights of one
 * component.
 */
struct FMtoUPoseSnapshot
{
	TArray<FTransform> ComponentSpaceTransforms;
	TArray<TPair<FName, float>> MorphWeights;
	FName AnimInstanceClass;

	/** Largest difference against another snapshot of the same component. */
	double MaxDelta(const FMtoUPoseSnapshot& Other) const;
	FString Describe() const;
};

/** Reads the current pose of one component. */
void MtoUCapturePoseSnapshot(const USkeletalMeshComponent& Component, FMtoUPoseSnapshot& OutSnapshot);
