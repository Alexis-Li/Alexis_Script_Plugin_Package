// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "MtoUMultiSubjectFixture.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "DynamicMesh/DynamicBoneAttribute.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicVertexSkinWeightsAttribute.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "LevelSequence.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "MovieScene.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "SkeletalMeshAttributes.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "UObject/Package.h"
#include "UDynamicMesh.h"

namespace
{
	using UE::Geometry::FDynamicMesh3;

	// The full Maya DAG paths of the recipe's rigs. Only the last path/namespace
	// element is compared with the first declared bone (Root / Root / ArmsRoot).
	const TCHAR* const FixtureCharacterRoot = TEXT("|character:Group|character:Root");
	const TCHAR* const FixturePropRoot = TEXT("|prop:Root");
	const TCHAR* const FixtureArmsRoot = TEXT("|arms:Socket|arms:ArmsRoot");

	/** The four fixed anchor placements of the sample, all off-origin and rotated. */
	const FVector CharacterAnchorLocation(500.0, -275.5, 120.75);
	const FRotator CharacterAnchorRotation(0.0, 35.0, 0.0);
	const FVector PropAnchorLocation(-275.25, 610.5, 33.75);
	const FRotator PropAnchorRotation(12.0, -40.0, 7.0);
	const FVector ArmsAnchorLocation(140.5, 300.25, 65.0);
	const FRotator ArmsAnchorRotation(-18.0, 9.5, 0.0);

	struct FBoneSpec
	{
		const TCHAR* Name;
		int32 Parent;
		FVector LocalTranslation;
	};

	// The rigs mirror the Maya recipe (maya/MtoUMultiSubjectPrototype/scripts/
	// mtou_multi_subject_recipe.json): same bone names, same parents, same
	// "Shared" Morph on every subject. Bind locals are the recipe's Maya locals
	// converted to UE axes (x, z, y).
	const FBoneSpec FullBodyBones[] = {
		{ TEXT("Root"), INDEX_NONE, FVector::ZeroVector },
		{ TEXT("Spine"), 0, FVector(0.0, 0.0, 30.0) },
		{ TEXT("Chest"), 1, FVector(0.0, 0.0, 20.0) },
		{ TEXT("Head"), 2, FVector(0.0, 0.0, 15.0) },
	};
	const TCHAR* FullBodyCurves[] = { TEXT("Shared") };

	/** Prop rig: it shares the bone name Root with the character, and Shared with every rig. */
	const FBoneSpec PropBones[] = {
		{ TEXT("Root"), INDEX_NONE, FVector::ZeroVector },
		{ TEXT("PropBody"), 0, FVector(0.0, 0.0, 18.0) },
		{ TEXT("PropTip"), 1, FVector(0.0, 0.0, 22.0) },
	};
	const TCHAR* PropCurves[] = { TEXT("Shared") };

	/** Arms-only rig: a different skeleton with two chains, used for role replacement. */
	const FBoneSpec ArmsBones[] = {
		{ TEXT("ArmsRoot"), INDEX_NONE, FVector::ZeroVector },
		{ TEXT("UpperArm_L"), 0, FVector(25.0, 0.0, 30.0) },
		{ TEXT("Forearm_L"), 1, FVector(30.0, 0.0, 0.0) },
		{ TEXT("Hand_L"), 2, FVector(12.0, 0.0, 0.0) },
		{ TEXT("UpperArm_R"), 0, FVector(-25.0, 0.0, 30.0) },
		{ TEXT("Forearm_R"), 4, FVector(-30.0, 0.0, 0.0) },
		{ TEXT("Hand_R"), 5, FVector(-12.0, 0.0, 0.0) },
	};
	const TCHAR* ArmsCurves[] = { TEXT("Shared") };

	USkeleton* MakeSkeleton(const TCHAR* Name, const FBoneSpec* Bones, int32 BoneCount)
	{
		USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage(), Name, RF_Transient);
		FReferenceSkeletonModifier Modifier(Skeleton);
		for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
		{
			Modifier.Add(
				FMeshBoneInfo(FName(Bones[BoneIndex].Name), Bones[BoneIndex].Name, Bones[BoneIndex].Parent),
				FTransform(Bones[BoneIndex].LocalTranslation));
		}
		return Skeleton;
	}

	/** Records the declaration a Maya peer must send for this rig, bind included. */
	FMtoUSubjectDeclaration MakeDeclaration(
		const TCHAR* Id,
		const TCHAR* Root,
		const USkeleton& Skeleton,
		const TCHAR* const* Curves,
		int32 CurveCount)
	{
		FMtoUSubjectDeclaration Declaration;
		Declaration.Id = Id;
		Declaration.Root = Root;
		const FReferenceSkeleton& ReferenceSkeleton = Skeleton.GetReferenceSkeleton();
		for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetNum(); ++BoneIndex)
		{
			FMtoUBoneDeclaration Bone;
			Bone.Name = ReferenceSkeleton.GetBoneName(BoneIndex);
			Bone.Parent = ReferenceSkeleton.GetParentIndex(BoneIndex);
			Declaration.Bones.Add(Bone);
			Declaration.Bind.Add(ReferenceSkeleton.GetRefBonePose()[BoneIndex]);
		}
		for (int32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
		{
			Declaration.Curves.Add(FName(Curves[CurveIndex]));
		}
		return Declaration;
	}

	/** One uniform morph target: every vertex moves by the same delta. */
	bool AddUniformMorph(USkeletalMesh& Mesh, FName Name, const FVector3f& Delta)
	{
		FSkeletalMeshModel* ImportedModel = Mesh.GetImportedModel();
		if (ImportedModel == nullptr || !ImportedModel->LODModels.IsValidIndex(0))
		{
			return false;
		}
		const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[0];
		TArray<FMorphTargetDelta> Deltas;
		for (uint32 VertexIndex = 0; VertexIndex < LODModel.NumVertices; ++VertexIndex)
		{
			FMorphTargetDelta& MorphDelta = Deltas.AddDefaulted_GetRef();
			MorphDelta.SourceIdx = VertexIndex;
			MorphDelta.PositionDelta = Delta;
		}
		if (Deltas.IsEmpty())
		{
			return false;
		}
		UMorphTarget* Morph = NewObject<UMorphTarget>(&Mesh, Name, RF_Transient);
		Morph->PopulateDeltas(Deltas, 0, LODModel.Sections, false, false, 0.0f);
		return Mesh.RegisterMorphTarget(Morph, false);
	}

	/** The engine's skeletal cube supplies the geometry, bone attributes and skin weights. */
	FDynamicMesh3 LoadBaseGeometry()
	{
		USkeletalMesh* Base = LoadObject<USkeletalMesh>(
			nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
		if (Base == nullptr)
		{
			return FDynamicMesh3();
		}
		UDynamicMesh* Source = NewObject<UDynamicMesh>(GetTransientPackage());
		FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
		ReadOptions.bApplyBuildSettings = false;
		ReadOptions.bRequestTangents = true;
		FGeometryScriptMeshReadLOD ReadLOD;
		ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
		EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
			Base, Source, ReadOptions, ReadLOD, Outcome);
		if (Outcome != EGeometryScriptOutcomePins::Success)
		{
			return FDynamicMesh3();
		}
		FDynamicMesh3 Geometry(Source->GetMeshRef());
		UE::Geometry::FMergeCoincidentMeshEdges Weld(&Geometry);
		Weld.Apply();
		return Geometry;
	}

	/**
	 * Gives the geometry exactly the fixture skeleton's bones, with every vertex
	 * on the root. The engine's SkeletalCube names its bones Bone01/Bone02, so
	 * the fixture writes the names of its own rig instead of remapping those.
	 */
	bool ApplySkeletonBones(UE::Geometry::FDynamicMesh3& Geometry, const USkeleton& Skeleton)
	{
		if (!Geometry.HasAttributes())
		{
			Geometry.EnableAttributes();
		}
		UE::Geometry::FDynamicMeshAttributeSet* Attributes = Geometry.Attributes();
		if (Attributes == nullptr)
		{
			return false;
		}
		const FReferenceSkeleton& Reference = Skeleton.GetReferenceSkeleton();
		Attributes->EnableBones(Reference.GetNum());
		UE::Geometry::FDynamicMeshBoneNameAttribute* Names = Attributes->GetBoneNames();
		UE::Geometry::FDynamicMeshBoneParentIndexAttribute* Parents = Attributes->GetBoneParentIndices();
		UE::Geometry::FDynamicMeshBonePoseAttribute* Poses = Attributes->GetBonePoses();
		if (Names == nullptr || Parents == nullptr || Poses == nullptr)
		{
			return false;
		}
		for (int32 BoneIndex = 0; BoneIndex < Reference.GetNum(); ++BoneIndex)
		{
			Names->SetValue(BoneIndex, Reference.GetBoneName(BoneIndex));
			Parents->SetValue(BoneIndex, Reference.GetParentIndex(BoneIndex));
			Poses->SetValue(BoneIndex, Reference.GetRefBonePose()[BoneIndex]);
		}
		UE::AnimationCore::FBoneWeights Uniform;
		Uniform.SetBoneWeight(0, 1.0f);
		UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* SkinWeights =
			Attributes->GetSkinWeightsAttribute(FSkeletalMeshAttributes::DefaultSkinWeightProfileName);
		if (SkinWeights == nullptr)
		{
			return false;
		}
		for (const int32 VertexID : Geometry.VertexIndicesItr())
		{
			SkinWeights->SetValue(VertexID, Uniform);
		}
		return true;
	}

	/**
	 * A transient Skeletal Mesh on the fixture skeleton. The geometry carries
	 * the fixture rig's own bone names, so the mesh's reference skeleton and the
	 * geometry agree by construction.
	 */
	USkeletalMesh* MakeMesh(
		USkeleton& Skeleton,
		const FDynamicMesh3& BaseGeometry,
		const TCHAR* const* Curves,
		int32 CurveCount,
		FString& OutError)
	{
		if (BaseGeometry.VertexCount() == 0)
		{
			OutError = TEXT("the fixture geometry could not be read from SkeletalCube");
			return nullptr;
		}
		// GeometryScript refuses writes under /Engine/Transient. Use a unique,
		// unsaved project package while keeping the mesh itself transient.
		UPackage* MeshPackage = CreatePackage(*FString::Printf(
			TEXT("/Game/__MtoUMultiSubjectFixture_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		MeshPackage->SetFlags(RF_Transient);
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>(MeshPackage, NAME_None, RF_Transient);
		Mesh->SetSkeleton(&Skeleton);
		Mesh->SetRefSkeleton(Skeleton.GetReferenceSkeleton());
		Mesh->CalculateInvRefMatrices();

		FDynamicMesh3 Geometry(BaseGeometry);
		if (!ApplySkeletonBones(Geometry, Skeleton))
		{
			OutError = TEXT("the fixture geometry could not be bound to the fixture skeleton");
			return nullptr;
		}
		UDynamicMesh* Source = NewObject<UDynamicMesh>(GetTransientPackage());
		Source->SetMesh(MoveTemp(Geometry));
		FGeometryScriptCopyMeshToAssetOptions WriteOptions;
		WriteOptions.bEmitTransaction = false;
		WriteOptions.bEnableRecomputeNormals = true;
		WriteOptions.bEnableRecomputeTangents = true;
		WriteOptions.bReplaceMaterials = true;
		WriteOptions.bUseBuildScale = false;
		WriteOptions.BoneHierarchyMismatchHandling =
			EGeometryScriptBoneHierarchyMismatchHandling::RemapGeometryToReferenceSkeleton;
		WriteOptions.NewMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
		WriteOptions.NewMaterialSlotNames.Add(FName(TEXT("MtoUPrototype")));
		FGeometryScriptMeshWriteLOD WriteLOD;
		EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToSkeletalMesh(
			Source, Mesh, WriteOptions, WriteLOD, Outcome);
		if (Outcome != EGeometryScriptOutcomePins::Success)
		{
			OutError = TEXT("the fixture Skeletal Mesh could not be written from geometry");
			return nullptr;
		}
		for (int32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
		{
			const FName Curve(Curves[CurveIndex]);
			if (!AddUniformMorph(*Mesh, Curve, FVector3f(0.0f, 0.0f, 8.0f)))
			{
				OutError = FString::Printf(TEXT("the fixture Morph Target '%s' could not be built"), *Curve.ToString());
				return nullptr;
			}
		}
		// Mirror the existing product test fixtures: registration without
		// invalidation leaves FindMorphTarget's lookup empty.
		Mesh->InitMorphTargets();
		return Mesh;
	}

	/** A short keyed animation that moves the rig's Root along +X. */
	UAnimSequence* MakeRootAnimation(USkeleton& Skeleton, float Distance, int32 Frames, const TCHAR* Name)
	{
		UAnimSequence* Sequence = NewObject<UAnimSequence>(GetTransientPackage(), Name, RF_Transient);
		Sequence->SetSkeleton(&Skeleton);
		IAnimationDataController& Controller = Sequence->GetController();
		Controller.InitializeModel();
		Controller.OpenBracket(FText::FromString(TEXT("MtoU multi-subject fixture")), false);
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(Frames + 1), false);
		const FName RootName(TEXT("Root"));
		Controller.AddBoneCurve(RootName, false);
		TArray<FVector3f> Positions;
		TArray<FQuat4f> Rotations;
		TArray<FVector3f> Scales;
		for (int32 Frame = 0; Frame <= Frames; ++Frame)
		{
			Positions.Add(FVector3f(Distance * static_cast<float>(Frame) / static_cast<float>(Frames), 0.0f, 0.0f));
			Rotations.Add(FQuat4f::Identity);
			Scales.Add(FVector3f::OneVector);
		}
		Controller.SetBoneTrackKeys(RootName, Positions, Rotations, Scales, false);
		Controller.CloseBracket(false);
		Controller.NotifyPopulated();
		return Sequence;
	}

	bool SpawnAnchor(
		UWorld& World,
		const FString& Label,
		const FVector& Location,
		const FRotator& Rotation,
		USkeletalMesh& Mesh,
		AActor*& OutActor,
		USkeletalMeshComponent*& OutComponent)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World.SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
		if (Actor == nullptr)
		{
			return false;
		}
		// The anchor is a plain actor whose root the mesh component hangs under,
		// so the component's world placement is exactly the anchor's placement.
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("AnchorRoot"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		Actor->SetActorLocationAndRotation(Location, Rotation);
		Actor->SetActorLabel(Label);

		USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(Actor, TEXT("SkeletalMeshComponent"));
		Component->SetupAttachment(Root);
		Component->SetSkeletalMeshAsset(&Mesh);
		Component->SetUpdateAnimationInEditor(true);
		Component->RegisterComponent();

		OutActor = Actor;
		OutComponent = Component;
		return true;
	}

	/** Minimal Level Sequence: one skeletal animation track bound to the character component. */
	bool BuildSequence(
		UWorld& World,
		USkeletalMeshComponent& CharacterComponent,
		UAnimSequence& CharacterAnimation,
		ULevelSequence*& OutSequence,
		UMovieSceneSkeletalAnimationTrack*& OutTrack,
		int32& OutStartTick,
		int32& OutEndTick)
	{
		ULevelSequence* Sequence = NewObject<ULevelSequence>(GetTransientPackage(), NAME_None, RF_Transient);
		Sequence->Initialize();
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		MovieScene->SetDisplayRate(FFrameRate(30, 1));
		MovieScene->SetTickResolutionDirectly(FFrameRate(30000, 1));
		const int32 StartTick = 1 * 30000;
		const int32 EndTick = StartTick + 20 * 30000 / 30;
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(
			TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(StartTick)),
			TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(EndTick))));

		const FGuid Binding =
			MovieScene->AddPossessable(TEXT("CharacterMesh"), USkeletalMeshComponent::StaticClass());
		Sequence->BindPossessableObject(Binding, CharacterComponent, &World);

		UMovieSceneSkeletalAnimationTrack* Track =
			MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(Binding);
		if (Track == nullptr)
		{
			return false;
		}
		Track->SetEvalDisabled(false);
		UMovieSceneSection* Section = Track->AddNewAnimation(FFrameNumber(StartTick), &CharacterAnimation);
		if (Section == nullptr)
		{
			return false;
		}
		Section->SetRange(TRange<FFrameNumber>(
			TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(StartTick)),
			TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(EndTick))));

		OutSequence = Sequence;
		OutTrack = Track;
		OutStartTick = StartTick;
		OutEndTick = EndTick;
		return true;
	}

	/** The shared sample pose: the same formulas documented for the Maya peer. */
	FTransform SampleRootTransform(double Step, double Radius, double SpawnZ, double RiseZPerFrame, double YawPerFrame)
	{
		const double Angle = 2.0 * PI * Step / 24.0;
		return FTransform(
			FQuat(FRotator(0.0, 0.0, YawPerFrame * Step)),
			FVector(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), SpawnZ + RiseZPerFrame * Step));
	}
}

bool FMtoUMultiSubjectFixture::IsValid() const
{
	return World != nullptr && CharacterComponent != nullptr && CharacterMesh != nullptr;
}

bool FMtoUMultiSubjectFixture::HasSubject(const FString& Id) const
{
	return FindDeclaration(Id) != nullptr && FindComponent(Id) != nullptr;
}

USkeletalMeshComponent* FMtoUMultiSubjectFixture::FindComponent(const FString& Id) const
{
	if (Id == FMtoUMultiSubjectFixtureBuilder::CharacterId())
	{
		return CharacterComponent;
	}
	if (Id == FMtoUMultiSubjectFixtureBuilder::PropId())
	{
		return PropComponent;
	}
	if (Id == FMtoUMultiSubjectFixtureBuilder::ArmsId())
	{
		return ArmsComponent;
	}
	return nullptr;
}

AActor* FMtoUMultiSubjectFixture::FindAnchor(const FString& Id) const
{
	if (Id == FMtoUMultiSubjectFixtureBuilder::CharacterId())
	{
		return CharacterAnchor;
	}
	if (Id == FMtoUMultiSubjectFixtureBuilder::PropId())
	{
		return PropAnchor;
	}
	if (Id == FMtoUMultiSubjectFixtureBuilder::ArmsId())
	{
		return ArmsAnchor;
	}
	return nullptr;
}

const FMtoUSubjectDeclaration* FMtoUMultiSubjectFixture::FindDeclaration(const FString& Id) const
{
	if (Id == FMtoUMultiSubjectFixtureBuilder::CharacterId())
	{
		return &CharacterDeclaration;
	}
	if (Id == FMtoUMultiSubjectFixtureBuilder::PropId())
	{
		return &PropDeclaration;
	}
	if (Id == FMtoUMultiSubjectFixtureBuilder::ArmsId())
	{
		return &ArmsDeclaration;
	}
	return nullptr;
}

TArray<FMtoUTargetRegistration> FMtoUMultiSubjectFixture::MakeRegistrations(
	const TArray<FString>& SubjectIds) const
{
	TArray<FMtoUTargetRegistration> Registrations;
	for (const FString& Id : SubjectIds)
	{
		FMtoUTargetRegistration Registration;
		Registration.Id = Id;
		Registration.Anchor = FindAnchor(Id);
		Registration.Component = FindComponent(Id);
		Registrations.Add(Registration);
	}
	return Registrations;
}

FMtoUInitMessage FMtoUMultiSubjectFixture::MakeInit(const TArray<FString>& SubjectIds) const
{
	FMtoUInitMessage Init;
	Init.Version = MtoUMultiSubjectProtocol::Version;
	Init.Fps = 30.0;
	for (const FString& Id : SubjectIds)
	{
		if (const FMtoUSubjectDeclaration* Declaration = FindDeclaration(Id))
		{
			Init.Subjects.Add(*Declaration);
		}
	}
	return Init;
}

FMtoUInitMessage FMtoUMultiSubjectFixture::MakeMismatchedInit() const
{
	// The arms subject carries the full-body skeleton: exactly the mistake of
	// pointing an arms target at the character input without renegotiating.
	FMtoUInitMessage Init;
	Init.Version = MtoUMultiSubjectProtocol::Version;
	Init.Fps = 30.0;
	Init.Subjects.Add(CharacterDeclaration);
	// The arms subject declared with the full-body input: same root path and
	// bones as the character, so the arms target refuses the bone set.
	FMtoUSubjectDeclaration Mismatched = CharacterDeclaration;
	Mismatched.Id = FMtoUMultiSubjectFixtureBuilder::ArmsId();
	Init.Subjects.Add(Mismatched);
	return Init;
}

FMtoUFrameMessage FMtoUMultiSubjectFixture::MakeFrame(
	const TArray<FString>& SubjectIds,
	int64 Serial,
	double SourceFrame) const
{
	FMtoUFrameMessage Frame;
	Frame.Serial = Serial;
	Frame.Time = SourceFrame;
	const double Step = SourceFrame - 1.0;

	for (const FString& Id : SubjectIds)
	{
		const FMtoUSubjectDeclaration* Declaration = FindDeclaration(Id);
		if (Declaration == nullptr)
		{
			continue;
		}
		FMtoUFrameSubject Subject;
		Subject.Id = Id;
		Subject.Transforms.Reserve(Declaration->Bones.Num());

		if (Id == FMtoUMultiSubjectFixtureBuilder::CharacterId())
		{
			for (const FMtoUBoneDeclaration& Bone : Declaration->Bones)
			{
				if (Bone.Name == FName(TEXT("Root")))
				{
					Subject.Transforms.Add(SampleRootTransform(Step, 100.0, 25.0, 2.0, 5.0));
				}
				else if (Bone.Name == FName(TEXT("Spine")))
				{
					Subject.Transforms.Add(FTransform(FQuat(FRotator(0.0, 0.0, 10.0 * Step)), FVector(0.0, 0.0, 30.0)));
				}
				else if (Bone.Name == FName(TEXT("Head")))
				{
					Subject.Transforms.Add(FTransform(FQuat(FRotator(0.0, 8.0 * Step, 0.0)), FVector(0.0, 0.0, 15.0)));
				}
				else
				{
					Subject.Transforms.Add(Declaration->Bind[Subject.Transforms.Num()]);
				}
			}
			Subject.Curves.Add(0.25f + 0.01f * static_cast<float>(Step));  // Shared
		}
		else if (Id == FMtoUMultiSubjectFixtureBuilder::PropId())
		{
			for (const FMtoUBoneDeclaration& Bone : Declaration->Bones)
			{
				if (Bone.Name == FName(TEXT("Root")))
				{
					Subject.Transforms.Add(FTransform(
						FQuat(FRotator(0.0, 0.0, 4.0 * Step)),
						FVector(0.0, 30.0, 10.0 + 1.5 * Step)));
				}
				else if (Bone.Name == FName(TEXT("PropBody")))
				{
					Subject.Transforms.Add(FTransform(FQuat(FRotator(15.0 * Step, 0.0, 0.0)), FVector(0.0, 0.0, 18.0)));
				}
				else
				{
					Subject.Transforms.Add(Declaration->Bind[Subject.Transforms.Num()]);
				}
			}
			Subject.Curves.Add(0.75f);                                      // Shared
		}
		else
		{
			for (const FMtoUBoneDeclaration& Bone : Declaration->Bones)
			{
				if (Bone.Name == FName(TEXT("ArmsRoot")))
				{
					Subject.Transforms.Add(FTransform(
						FQuat(FRotator(0.0, 0.0, 6.0 * Step)),
						FVector(0.0, -20.0, 5.0)));
				}
				else if (Bone.Name == FName(TEXT("UpperArm_L")))
				{
					Subject.Transforms.Add(FTransform(FQuat(FRotator(0.0, 12.0 * Step, 0.0)), FVector(25.0, 0.0, 30.0)));
				}
				else if (Bone.Name == FName(TEXT("UpperArm_R")))
				{
					Subject.Transforms.Add(FTransform(FQuat(FRotator(0.0, -12.0 * Step, 0.0)), FVector(-25.0, 0.0, 30.0)));
				}
				else
				{
					Subject.Transforms.Add(Declaration->Bind[Subject.Transforms.Num()]);
				}
			}
			Subject.Curves.Add(0.75f);                                      // Shared
		}
		Frame.Subjects.Add(MoveTemp(Subject));
	}
	return Frame;
}

void FMtoUMultiSubjectFixture::Destroy()
{
	if (World != nullptr)
	{
		World->DestroyWorld(false);
		if (GEngine != nullptr && WorldContext != nullptr)
		{
			GEngine->DestroyWorldContext(World);
		}
	}
	World = nullptr;
	WorldContext = nullptr;
	Actors.Reset();
}

bool FMtoUMultiSubjectFixtureBuilder::IsKnownScenario(const FString& Scenario)
{
	return Scenario == TEXT("character-prop") || Scenario == TEXT("character-arms");
}

TArray<FString> FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(const FString& Scenario)
{
	TArray<FString> SubjectIds;
	SubjectIds.Add(CharacterId());
	SubjectIds.Add(Scenario == TEXT("character-arms") ? ArmsId() : PropId());
	return SubjectIds;
}

bool FMtoUMultiSubjectFixtureBuilder::Build(
	const FString& Scenario,
	FMtoUMultiSubjectFixture& Out,
	FString& OutError)
{
	Out.Destroy();
	Out = FMtoUMultiSubjectFixture();
	Out.Scenario = Scenario;
	if (!IsKnownScenario(Scenario))
	{
		OutError = FString::Printf(
			TEXT("unknown scenario '%s'; known scenarios are character-prop and character-arms"), *Scenario);
		return false;
	}
	const bool bArmsScenario = Scenario == TEXT("character-arms");

	Out.World = UWorld::CreateWorld(EWorldType::Editor, false);
	if (Out.World == nullptr)
	{
		OutError = TEXT("the fixture world could not be created");
		return false;
	}
	Out.WorldContext = &GEngine->CreateNewWorldContext(EWorldType::Editor);
	Out.WorldContext->SetCurrentWorld(Out.World);
	Out.World->InitializeActorsForPlay(FURL());

	Out.CharacterSkeleton = MakeSkeleton(TEXT("MtoUPrototype_FullBody"),
		FullBodyBones, UE_ARRAY_COUNT(FullBodyBones));
	Out.PropSkeleton = MakeSkeleton(TEXT("MtoUPrototype_Prop"),
		PropBones, UE_ARRAY_COUNT(PropBones));
	Out.ArmsSkeleton = MakeSkeleton(TEXT("MtoUPrototype_Arms"),
		ArmsBones, UE_ARRAY_COUNT(ArmsBones));

	const FDynamicMesh3 BaseGeometry = LoadBaseGeometry();
	Out.CharacterMesh = MakeMesh(*Out.CharacterSkeleton, BaseGeometry,
		FullBodyCurves, UE_ARRAY_COUNT(FullBodyCurves), OutError);
	if (Out.CharacterMesh == nullptr)
	{
		Out.Destroy();
		return false;
	}
	Out.PropMesh = MakeMesh(*Out.PropSkeleton, BaseGeometry,
		PropCurves, UE_ARRAY_COUNT(PropCurves), OutError);
	if (Out.PropMesh == nullptr)
	{
		Out.Destroy();
		return false;
	}
	if (bArmsScenario)
	{
		Out.ArmsMesh = MakeMesh(*Out.ArmsSkeleton, BaseGeometry,
			ArmsCurves, UE_ARRAY_COUNT(ArmsCurves), OutError);
		if (Out.ArmsMesh == nullptr)
		{
			Out.Destroy();
			return false;
		}
	}

	Out.CharacterDeclaration = MakeDeclaration(CharacterId(), FixtureCharacterRoot,
		*Out.CharacterSkeleton, FullBodyCurves, UE_ARRAY_COUNT(FullBodyCurves));
	Out.PropDeclaration = MakeDeclaration(PropId(), FixturePropRoot,
		*Out.PropSkeleton, PropCurves, UE_ARRAY_COUNT(PropCurves));
	Out.ArmsDeclaration = MakeDeclaration(ArmsId(), FixtureArmsRoot,
		*Out.ArmsSkeleton, ArmsCurves, UE_ARRAY_COUNT(ArmsCurves));

	if (!SpawnAnchor(*Out.World, TEXT("MtoU_CharacterAnchor"), CharacterAnchorLocation,
			CharacterAnchorRotation, *Out.CharacterMesh, Out.CharacterAnchor, Out.CharacterComponent))
	{
		OutError = TEXT("the character anchor could not be spawned");
		Out.Destroy();
		return false;
	}
	Out.Actors.Add(Out.CharacterAnchor);
	if (bArmsScenario)
	{
		if (!SpawnAnchor(*Out.World, TEXT("MtoU_ArmsAnchor"), ArmsAnchorLocation,
				ArmsAnchorRotation, *Out.ArmsMesh, Out.ArmsAnchor, Out.ArmsComponent))
		{
			OutError = TEXT("the arms anchor could not be spawned");
			Out.Destroy();
			return false;
		}
		Out.Actors.Add(Out.ArmsAnchor);
	}
	else
	{
		if (!SpawnAnchor(*Out.World, TEXT("MtoU_PropAnchor"), PropAnchorLocation,
				PropAnchorRotation, *Out.PropMesh, Out.PropAnchor, Out.PropComponent))
		{
			OutError = TEXT("the prop anchor could not be spawned");
			Out.Destroy();
			return false;
		}
		Out.Actors.Add(Out.PropAnchor);
	}

	// The character starts inside a Level Sequence with a skeletal animation
	// track, and the prop starts on an ordinary single-node animation with a
	// non-zero Morph. Both are the "existing driver" the preview takes over from.
	Out.CharacterAnimation = MakeRootAnimation(*Out.CharacterSkeleton, 120.0f, 20, TEXT("MtoUPrototype_CharacterAnim"));
	if (!BuildSequence(*Out.World, *Out.CharacterComponent, *Out.CharacterAnimation,
			Out.Sequence, Out.CharacterAnimationTrack, Out.SequenceStartTick, Out.SequenceEndTick))
	{
		OutError = TEXT("the fixture Level Sequence could not be built");
		Out.Destroy();
		return false;
	}
	if (!bArmsScenario)
	{
		Out.PropAnimation = MakeRootAnimation(*Out.PropSkeleton, 40.0f, 20, TEXT("MtoUPrototype_PropAnim"));
		Out.PropComponent->PlayAnimation(Out.PropAnimation, true);
		if (Out.PropComponent->GetSingleNodeInstance() == nullptr)
		{
			OutError = TEXT("the prop's existing single-node animation could not start");
			Out.Destroy();
			return false;
		}
		Out.PropComponent->SetPlayRate(1.0f);
		Out.PropComponent->SetMorphTarget(FName(TEXT("Shared")), 0.4f);
	}
	return true;
}
