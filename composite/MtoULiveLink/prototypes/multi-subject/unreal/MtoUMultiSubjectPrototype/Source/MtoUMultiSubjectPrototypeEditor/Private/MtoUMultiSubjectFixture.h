// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "MtoUMultiSubjectTypes.h"

class AActor;
class UAnimSequence;
class ULevelSequence;
class UMovieSceneSkeletalAnimationTrack;
class USkeletalMesh;
class USkeletalMeshComponent;
class USkeleton;
class UWorld;
struct FWorldContext;

/**
 * The reproducible two-subject sample: a transient editor world with one actor
 * anchor per object, a transient skeletal mesh per skeleton, the keyed
 * "existing animation" the preview takes over from, and the declaration /
 * frame templates that define the shared cross-host sample.
 *
 * Nothing here is saved: every asset is transient and every actor is transient,
 * so a run can never change a project asset.
 */
struct FMtoUMultiSubjectFixture
{
	FString Scenario;
	UWorld* World = nullptr;
	FWorldContext* WorldContext = nullptr;
	TArray<TObjectPtr<AActor>> Actors;

	USkeleton* CharacterSkeleton = nullptr;
	USkeleton* PropSkeleton = nullptr;
	USkeleton* ArmsSkeleton = nullptr;
	USkeletalMesh* CharacterMesh = nullptr;
	USkeletalMesh* PropMesh = nullptr;
	USkeletalMesh* ArmsMesh = nullptr;

	/** Keyed animation used by the character's Level Sequence track. */
	UAnimSequence* CharacterAnimation = nullptr;
	/** Keyed animation already playing on the prop component (single node). */
	UAnimSequence* PropAnimation = nullptr;
	/** Minimal Level Sequence with one skeletal animation track bound to the character component. */
	ULevelSequence* Sequence = nullptr;
	UMovieSceneSkeletalAnimationTrack* CharacterAnimationTrack = nullptr;
	int32 SequenceStartTick = 0;
	int32 SequenceEndTick = 0;

	AActor* CharacterAnchor = nullptr;
	USkeletalMeshComponent* CharacterComponent = nullptr;
	AActor* PropAnchor = nullptr;
	USkeletalMeshComponent* PropComponent = nullptr;
	AActor* ArmsAnchor = nullptr;
	USkeletalMeshComponent* ArmsComponent = nullptr;

	/** Declaration templates: the sample spec both hosts build their rigs from. */
	FMtoUSubjectDeclaration CharacterDeclaration;
	FMtoUSubjectDeclaration PropDeclaration;
	FMtoUSubjectDeclaration ArmsDeclaration;

	bool IsValid() const;
	bool HasSubject(const FString& Id) const;
	USkeletalMeshComponent* FindComponent(const FString& Id) const;
	AActor* FindAnchor(const FString& Id) const;
	const FMtoUSubjectDeclaration* FindDeclaration(const FString& Id) const;

	TArray<FMtoUTargetRegistration> MakeRegistrations(const TArray<FString>& SubjectIds) const;
	FMtoUInitMessage MakeInit(const TArray<FString>& SubjectIds) const;
	/** The shared sample pose of one Maya source frame; both hosts can compute it. */
	FMtoUFrameMessage MakeFrame(const TArray<FString>& SubjectIds, int64 Serial, double SourceFrame) const;
	/** A deliberately mismatched init: the arms subject carries the full-body skeleton. */
	FMtoUInitMessage MakeMismatchedInit() const;

	/** Destroys the transient world; the transient assets then become garbage. */
	void Destroy();
};

/** Builds the fixture; `character-prop` and `character-arms` are the known scenarios. */
class FMtoUMultiSubjectFixtureBuilder
{
public:
	static const TCHAR* CharacterId() { return TEXT("character"); }
	static const TCHAR* PropId() { return TEXT("prop"); }
	static const TCHAR* ArmsId() { return TEXT("arms"); }
	static bool IsKnownScenario(const FString& Scenario);
	static TArray<FString> ScenarioSubjects(const FString& Scenario);

	static bool Build(const FString& Scenario, FMtoUMultiSubjectFixture& Out, FString& OutError);

	/**
	 * One transient anchor actor with one skeletal mesh component on the given
	 * mesh, placed off-origin and rotated. Used by the fixture and by the
	 * real-asset pair, which points the same shape at production meshes; nothing
	 * is saved and every object is transient.
	 */
	static bool SpawnAssetTarget(
		UWorld& World,
		const FString& Label,
		const FVector& Location,
		const FRotator& Rotation,
		USkeletalMesh& Mesh,
		AActor*& OutActor,
		USkeletalMeshComponent*& OutComponent);
};
