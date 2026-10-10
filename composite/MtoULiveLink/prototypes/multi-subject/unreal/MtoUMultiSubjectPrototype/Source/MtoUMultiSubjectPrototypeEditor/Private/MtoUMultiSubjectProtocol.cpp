// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "MtoUMultiSubjectProtocol.h"

#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr double PoseTupleQuaternionTolerance = 0.01;

	void SetShapeError(FMtoUProtocolError& OutError, const FString& Details)
	{
		OutError.Code = MtoUMultiSubjectError::MessageShape;
		OutError.Details = Details;
	}

	bool ReadSubjectArray(
		const TSharedPtr<FJsonObject>& Object,
		const TArray<TSharedPtr<FJsonValue>>*& OutValues,
		FMtoUProtocolError& OutError)
	{
		if (!Object->TryGetArrayField(TEXT("subjects"), OutValues) || OutValues == nullptr)
		{
			SetShapeError(OutError, TEXT("message has no subjects array"));
			return false;
		}
		return true;
	}

	bool ReadRequiredString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		FString& OutValue,
		FMtoUProtocolError& OutError)
	{
		if (!Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			SetShapeError(OutError, FString::Printf(TEXT("field '%s' must be a non-empty string"), Field));
			return false;
		}
		return true;
	}

	bool ReadRequiredNumber(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		double& OutValue,
		FMtoUProtocolError& OutError)
	{
		if (!Object->TryGetNumberField(Field, OutValue) || !FMath::IsFinite(OutValue))
		{
			SetShapeError(OutError, FString::Printf(TEXT("field '%s' must be a finite number"), Field));
			return false;
		}
		return true;
	}

	bool ReadNumberArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		TArray<TSharedPtr<FJsonValue>>& OutValues,
		FMtoUProtocolError& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(Field, Values) || Values == nullptr)
		{
			SetShapeError(OutError, FString::Printf(TEXT("field '%s' must be an array"), Field));
			return false;
		}
		OutValues = *Values;
		return true;
	}

	bool ParseSubjectDeclaration(
		const TSharedPtr<FJsonObject>& Object,
		int32 SubjectIndex,
		FMtoUSubjectDeclaration& OutSubject,
		FMtoUProtocolError& OutError)
	{
		if (!ReadRequiredString(Object, TEXT("id"), OutSubject.Id, OutError))
		{
			return false;
		}
		if (!ReadRequiredString(Object, TEXT("root"), OutSubject.Root, OutError))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* BoneValues = nullptr;
		if (!Object->TryGetArrayField(TEXT("bones"), BoneValues) || BoneValues == nullptr || BoneValues->IsEmpty())
		{
			SetShapeError(OutError, FString::Printf(
				TEXT("subject %d (%s) needs a non-empty bones array"), SubjectIndex, *OutSubject.Id));
			return false;
		}
		for (int32 BoneIndex = 0; BoneIndex < BoneValues->Num(); ++BoneIndex)
		{
			const TSharedPtr<FJsonObject>* BoneObject = nullptr;
			if (!(*BoneValues)[BoneIndex]->TryGetObject(BoneObject) || BoneObject == nullptr)
			{
				SetShapeError(OutError, FString::Printf(
					TEXT("subject %s bone %d is not an object"), *OutSubject.Id, BoneIndex));
				return false;
			}
			FMtoUBoneDeclaration Bone;
			FString BoneName;
			if (!ReadRequiredString(*BoneObject, TEXT("name"), BoneName, OutError))
			{
				return false;
			}
			Bone.Name = FName(*BoneName);
			double Parent = -1.0;
			if ((*BoneObject)->TryGetNumberField(TEXT("parent"), Parent))
			{
				if (!FMath::IsFinite(Parent) || Parent < -1.0
					|| FMath::FloorToDouble(Parent) != Parent
					|| (Parent >= 0.0 && Parent >= BoneIndex))
				{
					SetShapeError(OutError, FString::Printf(
						TEXT("subject %s bone %d (%s) has an invalid parent index"),
						*OutSubject.Id, BoneIndex, *BoneName));
					return false;
				}
				Bone.Parent = static_cast<int32>(Parent);
			}
			OutSubject.Bones.Add(Bone);
		}

		const TArray<TSharedPtr<FJsonValue>>* CurveValues = nullptr;
		if (!Object->TryGetArrayField(TEXT("curves"), CurveValues) || CurveValues == nullptr)
		{
			SetShapeError(OutError, FString::Printf(
				TEXT("subject %s needs a curves array (it may be empty)"), *OutSubject.Id));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& CurveValue : *CurveValues)
		{
			FString CurveName;
			if (!CurveValue.IsValid() || !CurveValue->TryGetString(CurveName) || CurveName.IsEmpty())
			{
				SetShapeError(OutError, FString::Printf(
					TEXT("subject %s has a curve entry that is not a non-empty string"), *OutSubject.Id));
				return false;
			}
			const FName Curve(*CurveName);
			if (OutSubject.Curves.Contains(Curve))
			{
				SetShapeError(OutError, FString::Printf(
					TEXT("subject %s declares curve '%s' twice"), *OutSubject.Id, *CurveName));
				return false;
			}
			OutSubject.Curves.Add(Curve);
		}

		TArray<TSharedPtr<FJsonValue>> BindValues;
		if (!ReadNumberArray(Object, TEXT("bind"), BindValues, OutError))
		{
			return false;
		}
		if (BindValues.Num() != OutSubject.Bones.Num())
		{
			SetShapeError(OutError, FString::Printf(
				TEXT("subject %s declares %d bones but %d bind poses"),
				*OutSubject.Id, OutSubject.Bones.Num(), BindValues.Num()));
			return false;
		}
		OutSubject.Bind.Reserve(BindValues.Num());
		for (int32 BindIndex = 0; BindIndex < BindValues.Num(); ++BindIndex)
		{
			// A bind pose uses the same ten-number tuple as a frame transform.
			const TArray<TSharedPtr<FJsonValue>>* BindTuple = nullptr;
			FTransform BindTransform;
			if (BindValues[BindIndex].IsValid())
			{
				BindValues[BindIndex]->TryGetArray(BindTuple);
			}
			if (BindTuple == nullptr
				|| !FMtoUMultiSubjectProtocol::ReadPoseTuple(*BindTuple, BindTransform))
			{
				SetShapeError(OutError, FString::Printf(
					TEXT("subject %s bind %d is not a ten-number pose"), *OutSubject.Id, BindIndex));
				return false;
			}
			OutSubject.Bind.Add(BindTransform);
		}
		return true;
	}
}

bool FMtoUMultiSubjectProtocol::PeekType(
	const TSharedPtr<FJsonObject>& Object,
	FString& OutType,
	FMtoUProtocolError& OutError)
{
	if (!Object.IsValid() || !ReadRequiredString(Object, TEXT("type"), OutType, OutError))
	{
		return false;
	}
	return true;
}

bool FMtoUMultiSubjectProtocol::ParseInit(
	const TSharedPtr<FJsonObject>& Object,
	FMtoUInitMessage& OutMessage,
	FMtoUProtocolError& OutError)
{
	OutMessage = FMtoUInitMessage();
	if (!Object.IsValid())
	{
		SetShapeError(OutError, TEXT("init is not a JSON object"));
		return false;
	}

	double Version = 0.0;
	if (!ReadRequiredNumber(Object, TEXT("version"), Version, OutError))
	{
		return false;
	}
	OutMessage.Version = static_cast<int32>(Version);
	if (OutMessage.Version != MtoUMultiSubjectProtocol::Version)
	{
		OutError.Code = MtoUMultiSubjectError::VersionUnsupported;
		OutError.Details = FString::Printf(
			TEXT("wire version %d is not supported; this receiver implements version %d"),
			OutMessage.Version, MtoUMultiSubjectProtocol::Version);
		return false;
	}
	if (!ReadRequiredNumber(Object, TEXT("fps"), OutMessage.Fps, OutError) || OutMessage.Fps <= 0.0)
	{
		SetShapeError(OutError, TEXT("init fps must be a positive finite number"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* SubjectValues = nullptr;
	if (!ReadSubjectArray(Object, SubjectValues, OutError))
	{
		return false;
	}
	if (SubjectValues->Num() != MtoUMultiSubjectProtocol::SubjectCount)
	{
		OutError.Code = MtoUMultiSubjectError::SubjectsShape;
		OutError.Details = FString::Printf(
			TEXT("the realtime profile carries exactly %d subjects; init carries %d"),
			MtoUMultiSubjectProtocol::SubjectCount, SubjectValues->Num());
		return false;
	}

	for (int32 SubjectIndex = 0; SubjectIndex < SubjectValues->Num(); ++SubjectIndex)
	{
		const TSharedPtr<FJsonObject>* SubjectObject = nullptr;
		if (!(*SubjectValues)[SubjectIndex]->TryGetObject(SubjectObject) || SubjectObject == nullptr)
		{
			SetShapeError(OutError, FString::Printf(TEXT("subject %d is not an object"), SubjectIndex));
			return false;
		}
		FMtoUSubjectDeclaration Subject;
		if (!ParseSubjectDeclaration(*SubjectObject, SubjectIndex, Subject, OutError))
		{
			return false;
		}
		if (OutMessage.Subjects.ContainsByPredicate(
				[&Subject](const FMtoUSubjectDeclaration& Existing) { return Existing.Id == Subject.Id; }))
		{
			OutError.Code = MtoUMultiSubjectError::SubjectsShape;
			OutError.Details = FString::Printf(TEXT("subject id '%s' is declared twice"), *Subject.Id);
			return false;
		}
		OutMessage.Subjects.Add(MoveTemp(Subject));
	}
	return true;
}

bool FMtoUMultiSubjectProtocol::ParseFrame(
	const TSharedPtr<FJsonObject>& Object,
	FMtoUFrameMessage& OutMessage,
	FMtoUProtocolError& OutError)
{
	OutMessage = FMtoUFrameMessage();
	if (!Object.IsValid())
	{
		SetShapeError(OutError, TEXT("frame is not a JSON object"));
		return false;
	}

	double Serial = 0.0;
	if (!ReadRequiredNumber(Object, TEXT("serial"), Serial, OutError)
		|| FMath::FloorToDouble(Serial) != Serial || Serial < 1.0)
	{
		SetShapeError(OutError, TEXT("frame serial must be a positive integer"));
		return false;
	}
	OutMessage.Serial = static_cast<int64>(Serial);
	if (!ReadRequiredNumber(Object, TEXT("time"), OutMessage.Time, OutError))
	{
		return false;
	}

	double Session = 0.0;
	if (!ReadRequiredNumber(Object, TEXT("session"), Session, OutError)
		|| FMath::FloorToDouble(Session) != Session || Session < 1.0)
	{
		SetShapeError(OutError, TEXT("frame session must be the positive integer from ready"));
		return false;
	}
	OutMessage.Session = static_cast<int64>(Session);

	const TArray<TSharedPtr<FJsonValue>>* SubjectValues = nullptr;
	if (!ReadSubjectArray(Object, SubjectValues, OutError))
	{
		return false;
	}
	for (int32 SubjectIndex = 0; SubjectIndex < SubjectValues->Num(); ++SubjectIndex)
	{
		const TSharedPtr<FJsonObject>* SubjectObject = nullptr;
		if (!(*SubjectValues)[SubjectIndex]->TryGetObject(SubjectObject) || SubjectObject == nullptr)
		{
			SetShapeError(OutError, FString::Printf(TEXT("frame subject %d is not an object"), SubjectIndex));
			return false;
		}
		FMtoUFrameSubject Subject;
		if (!ReadRequiredString(*SubjectObject, TEXT("id"), Subject.Id, OutError))
		{
			return false;
		}
		if (OutMessage.Subjects.ContainsByPredicate(
				[&Subject](const FMtoUFrameSubject& Existing) { return Existing.Id == Subject.Id; }))
		{
			SetShapeError(OutError, FString::Printf(TEXT("frame carries subject '%s' twice"), *Subject.Id));
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> TransformValues;
		if (!ReadNumberArray(*SubjectObject, TEXT("transforms"), TransformValues, OutError))
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> CurveValues;
		if (!ReadNumberArray(*SubjectObject, TEXT("curves"), CurveValues, OutError))
		{
			return false;
		}
		// One ten-number row per declared bone, exactly like bind.
		const int32 PoseCount = TransformValues.Num();
		if (PoseCount == 0)
		{
			SetShapeError(OutError, FString::Printf(
				TEXT("subject %s carries no pose rows"), *Subject.Id));
			return false;
		}
		Subject.Transforms.Reserve(PoseCount);
		for (int32 PoseIndex = 0; PoseIndex < PoseCount; ++PoseIndex)
		{
			const TArray<TSharedPtr<FJsonValue>>* Row = nullptr;
			if (TransformValues[PoseIndex].IsValid())
			{
				TransformValues[PoseIndex]->TryGetArray(Row);
			}
			FTransform LocalTransform;
			if (Row == nullptr || !ReadPoseTuple(*Row, LocalTransform))
			{
				SetShapeError(OutError, FString::Printf(
					TEXT("subject %s pose %d is not ten finite numbers with a unit quaternion"),
					*Subject.Id, PoseIndex));
				return false;
			}
			Subject.Transforms.Add(LocalTransform);
		}

		Subject.Curves.Reserve(CurveValues.Num());
		for (const TSharedPtr<FJsonValue>& CurveValue : CurveValues)
		{
			double Value = 0.0;
			if (!CurveValue.IsValid() || !CurveValue->TryGetNumber(Value) || !FMath::IsFinite(Value))
			{
				SetShapeError(OutError, FString::Printf(
					TEXT("subject %s carries a non-finite curve value"), *Subject.Id));
				return false;
			}
			Subject.Curves.Add(static_cast<float>(Value));
		}
		OutMessage.Subjects.Add(MoveTemp(Subject));
	}
	return true;
}

bool FMtoUMultiSubjectProtocol::ParseRemove(
	const TSharedPtr<FJsonObject>& Object,
	FMtoURemoveMessage& OutMessage,
	FMtoUProtocolError& OutError)
{
	OutMessage = FMtoURemoveMessage();
	if (!Object.IsValid())
	{
		SetShapeError(OutError, TEXT("remove is not a JSON object"));
		return false;
	}

	double Session = 0.0;
	if (!ReadRequiredNumber(Object, TEXT("session"), Session, OutError)
		|| FMath::FloorToDouble(Session) != Session || Session < 1.0)
	{
		SetShapeError(OutError, TEXT("remove session must be the positive integer from ready"));
		return false;
	}
	OutMessage.Session = static_cast<int64>(Session);
	return ReadRequiredString(Object, TEXT("id"), OutMessage.Id, OutError);
}

FString FMtoUMultiSubjectProtocol::DescribeTimeDirection(
	double PreviousTime,
	bool bHasPrevious,
	double Time)
{
	if (!bHasPrevious)
	{
		return TEXT("first");
	}
	if (Time > PreviousTime)
	{
		return TEXT("forward");
	}
	return Time < PreviousTime ? TEXT("backward") : TEXT("hold");
}

TSharedRef<FJsonObject> FMtoUMultiSubjectProtocol::MakeReady(int64 Session)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("type"), TEXT("ready"));
	Object->SetNumberField(TEXT("session"), static_cast<double>(Session));
	return Object;
}

TSharedRef<FJsonObject> FMtoUMultiSubjectProtocol::MakeApplied(
	int64 Session,
	int64 Serial,
	double Time,
	const TArray<FMtoUSubjectStatus>& SubjectStatuses)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("type"), TEXT("applied"));
	Object->SetNumberField(TEXT("session"), static_cast<double>(Session));
	Object->SetNumberField(TEXT("serial"), static_cast<double>(Serial));
	Object->SetNumberField(TEXT("time"), Time);
	TArray<TSharedPtr<FJsonValue>> SubjectValues;
	SubjectValues.Reserve(SubjectStatuses.Num());
	for (const FMtoUSubjectStatus& Status : SubjectStatuses)
	{
		const TSharedRef<FJsonObject> SubjectObject = MakeShared<FJsonObject>();
		SubjectObject->SetStringField(TEXT("id"), Status.Id);
		SubjectObject->SetStringField(TEXT("status"), Status.Status);
		SubjectValues.Add(MakeShared<FJsonValueObject>(SubjectObject));
	}
	Object->SetArrayField(TEXT("subjects"), SubjectValues);
	return Object;
}

TSharedRef<FJsonObject> FMtoUMultiSubjectProtocol::MakeError(const FString& Code, const FString& Details)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("type"), TEXT("error"));
	Object->SetStringField(TEXT("code"), Code);
	Object->SetStringField(TEXT("details"), Details);
	return Object;
}

FString FMtoUMultiSubjectProtocol::ToLine(const TSharedRef<FJsonObject>& Object)
{
	FString Line;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);
	FJsonSerializer::Serialize(Object, Writer);
	Line.AppendChar(TEXT('\n'));
	return Line;
}

bool FMtoUMultiSubjectProtocol::ReadPoseTuple(
	const TArray<TSharedPtr<FJsonValue>>& Values,
	FTransform& OutTransform)
{
	if (Values.Num() != 10)
	{
		return false;
	}
	double Numbers[10];
	for (int32 Index = 0; Index < 10; ++Index)
	{
		if (!Values[Index].IsValid() || !Values[Index]->TryGetNumber(Numbers[Index])
			|| !FMath::IsFinite(Numbers[Index]))
		{
			return false;
		}
	}
	const FQuat Rotation(
		static_cast<float>(Numbers[3]), static_cast<float>(Numbers[4]),
		static_cast<float>(Numbers[5]), static_cast<float>(Numbers[6]));
	if (!FMath::IsNearlyEqual(static_cast<double>(Rotation.SizeSquared()), 1.0, PoseTupleQuaternionTolerance))
	{
		return false;
	}
	OutTransform = FTransform(
		Rotation,
		FVector(static_cast<float>(Numbers[0]), static_cast<float>(Numbers[1]), static_cast<float>(Numbers[2])),
		FVector(static_cast<float>(Numbers[7]), static_cast<float>(Numbers[8]), static_cast<float>(Numbers[9])));
	return true;
}

/**
 * Prototype bounds for the rest-pose comparison behind the bind/frame
 * projection: the declared rig and the target must agree up to one constant
 * component-space frame, and a gross disagreement means the two are different
 * rigs whose motion must not be mixed. These are prototype noise bounds, not a
 * deformation budget.
 */
constexpr double RestTranslationBoundCm = 5.0;
constexpr double RestRotationBoundDegrees = 10.0;
constexpr double RestScaleBound = 0.05;
/** A mapped pose scaled below this cannot be inverted for the projection. */
constexpr double RestScaleFloor = 1.0e-4;

bool FMtoUMultiSubjectProtocol::CollectRequiredTargetBones(
	const USkeletalMesh& Mesh,
	TArray<bool>& OutRequired,
	FString& OutProblem)
{
	OutRequired.Reset();
	OutProblem.Reset();
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	OutRequired.Init(false, Skeleton.GetNum());

	// The render sections are the reliable skinning evidence: their bone palette
	// and the per-vertex weights are what actually deforms the mesh. A mesh
	// without readable weights is refused instead of guessed.
	const FSkeletalMeshRenderData* Data = Mesh.GetResourceForRendering();
	if (Data == nullptr || Data->LODRenderData.IsEmpty())
	{
		OutProblem = TEXT("has no readable render data; rebuild or reimport the Skeletal Mesh");
		return false;
	}
	for (int32 LODIndex = 0; LODIndex < Data->LODRenderData.Num(); ++LODIndex)
	{
		const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[LODIndex];
		const FSkinWeightVertexBuffer& Weights = LOD.SkinWeightVertexBuffer;
		if (Weights.GetDataVertexBuffer() == nullptr
			|| Weights.GetDataVertexBuffer()->GetWeightData() == nullptr
			|| LOD.RenderSections.IsEmpty())
		{
			OutProblem = FString::Printf(
				TEXT("LOD %d has no readable CPU skin weights; rebuild with CPU skin data available"),
				LODIndex);
			return false;
		}
		for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
		{
			if (static_cast<uint64>(Section.BaseVertexIndex) + Section.NumVertices
				> Weights.GetNumVertices())
			{
				OutProblem = FString::Printf(TEXT("LOD %d has invalid skin vertex ranges"), LODIndex);
				return false;
			}
			for (uint32 Vertex = Section.BaseVertexIndex;
				Vertex < Section.BaseVertexIndex + Section.NumVertices; ++Vertex)
			{
				bool bWeighted = false;
				for (uint32 Influence = 0; Influence < Weights.GetMaxBoneInfluences(); ++Influence)
				{
					if (Weights.GetBoneWeight(Vertex, Influence) == 0)
					{
						continue;
					}
					const uint32 PaletteIndex = Weights.GetBoneIndex(Vertex, Influence);
					if (!Section.BoneMap.IsValidIndex(PaletteIndex))
					{
						OutProblem = FString::Printf(
							TEXT("LOD %d has an invalid positive skin influence"), LODIndex);
						return false;
					}
					const int32 WeightedBone = Section.BoneMap[PaletteIndex];
					if (!OutRequired.IsValidIndex(WeightedBone))
					{
						OutProblem = FString::Printf(
							TEXT("LOD %d skin influences name a bone outside the skeleton"), LODIndex);
						return false;
					}
					bWeighted = true;
					for (int32 Bone = WeightedBone; Bone != INDEX_NONE;
						Bone = Skeleton.GetParentIndex(Bone))
					{
						OutRequired[Bone] = true;
					}
				}
				if (!bWeighted)
				{
					OutProblem = FString::Printf(
						TEXT("LOD %d has a vertex without positive skin weights"), LODIndex);
					return false;
				}
			}
		}
	}
	bool bAnyRequired = false;
	for (const bool bRequired : OutRequired)
	{
		bAnyRequired |= bRequired;
	}
	if (!bAnyRequired)
	{
		OutProblem = TEXT("has no positive skin influences");
		return false;
	}
	return true;
}

namespace
{
	/**
	 * One negotiated target bone. The negotiated target is the *necessary* set
	 * only: every target bone with a positive skin weight in any LOD plus every
	 * ancestor of one, the same target the product's necessary-dependency
	 * negotiation consumes. A target bone outside it deforms nothing, so it is
	 * never a mapping candidate and its bind can never veto a declaration.
	 */
	struct FMtoUTargetBone
	{
		FName Name;
		/** Parent slot inside the necessary set, or INDEX_NONE for a root. */
		int32 Parent = INDEX_NONE;
	};

	/** The mapped parent's name inside one target scope, or `<root>`. */
	FString TargetScopeParentLabel(const TArray<FMtoUTargetBone>& Target, int32 Slot)
	{
		return Slot == INDEX_NONE || Target[Slot].Parent == INDEX_NONE
			? FString(TEXT("<root>"))
			: Target[Target[Slot].Parent].Name.ToString();
	}

	FString TargetParentLabel(const FReferenceSkeleton& Skeleton, int32 BoneIndex)
	{
		const int32 Parent = BoneIndex == INDEX_NONE ? INDEX_NONE : Skeleton.GetParentIndex(BoneIndex);
		return Parent == INDEX_NONE
			? FString(TEXT("<root>"))
			: Skeleton.GetBoneName(Parent).ToString();
	}

	/** The `_` separator plus the 32 hexadecimal digits of one import hash suffix. */
	constexpr int32 HashSuffixLength = 33;

	/** `joint` -> `joint1`: the complete name plus digits only. */
	bool IsNumericSuffixRename(FName DeclaredName, FName TargetName)
	{
		const FString Declared = DeclaredName.ToString();
		const FString Target = TargetName.ToString();
		if (!Target.StartsWith(Declared, ESearchCase::IgnoreCase)
			|| Target.Len() <= Declared.Len())
		{
			return false;
		}
		for (int32 Index = Declared.Len(); Index < Target.Len(); ++Index)
		{
			if (!FChar::IsDigit(Target[Index]))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * `spine_04` -> `spine_04_<32 hex digits>`: the complete declared name plus
	 * exactly one import hash suffix. A partial name, another separator or a
	 * different hash length never match.
	 */
	bool IsHashSuffixRename(FName DeclaredName, FName TargetName)
	{
		const FString Declared = DeclaredName.ToString();
		const FString Target = TargetName.ToString();
		if (Target.Len() != Declared.Len() + HashSuffixLength
			|| !Target.StartsWith(Declared, ESearchCase::IgnoreCase)
			|| Target[Declared.Len()] != TEXT('_'))
		{
			return false;
		}
		for (int32 Index = Declared.Len() + 1; Index < Target.Len(); ++Index)
		{
			if (!FChar::IsHexDigit(Target[Index]))
			{
				return false;
			}
		}
		return true;
	}

	/** Either rename form the importer generates for a duplicated short name. */
	bool IsImportedRename(FName DeclaredName, FName TargetName)
	{
		return IsNumericSuffixRename(DeclaredName, TargetName)
			|| IsHashSuffixRename(DeclaredName, TargetName);
	}

	/** `root/group/joint`: the declared ancestor chain of one bone, for reports. */
	FString SourcePath(const TArray<FMtoUBoneDeclaration>& Bones, int32 Index)
	{
		TArray<int32> Chain;
		for (int32 Current = Index; Current != INDEX_NONE && Bones.IsValidIndex(Current);
			Current = Bones[Current].Parent)
		{
			Chain.Add(Current);
		}
		FString Path;
		for (int32 Depth = Chain.Num() - 1; Depth >= 0; --Depth)
		{
			if (!Path.IsEmpty())
			{
				Path += TEXT("/");
			}
			Path += Bones[Chain[Depth]].Name.ToString();
		}
		return Path;
	}

	/** One declared bone's complete candidate relation inside one mapped parent scope. */
	struct FMtoUScopeBone
	{
		int32 SourceIndex = INDEX_NONE;
		/** First exact-name target below the mapped parent, when one exists. */
		int32 ExactTarget = INDEX_NONE;
		int32 ExactCount = 0;
		/** Import-rename targets that no exact sibling name reserves. */
		TArray<int32> RenameTargets;
		/** Set once the bone is mapped or left as an ignored export branch. */
		bool bResolved = false;
	};

	/** One mapped parent scope: its children and their complete candidate relation. */
	struct FMtoUScope
	{
		int32 ExpectedParent = INDEX_NONE;
		TArray<int32> Siblings;
		TArray<FMtoUScopeBone> Bones;
		/** Every published short name the children carry. */
		TSet<FName> SiblingNames;
		/** How many children carry each short name. */
		TMap<FName, int32> NameCounts;
		/** First child carrying each short name, for one report per conflict. */
		TMap<FName, int32> FirstWithName;
	};

	/** Everything one mapping pass produces; every index is a necessary-set slot. */
	struct FMtoUMappingPass
	{
		/** Target slot per declared bone; INDEX_NONE marks an ignored export branch. */
		TArray<int32> SourceToTarget;
		TSet<int32> Skipped;
		TSet<int32> Used;
		TArray<FString> MappingAmbiguities;
		TArray<FString> BoneNameMappings;
	};

	void MapScopeBone(
		FMtoUMappingPass& Pass,
		const FMtoUSubjectDeclaration& Declaration,
		const TArray<FMtoUTargetBone>& Target,
		int32 SourceIndex,
		int32 TargetSlot)
	{
		Pass.SourceToTarget[SourceIndex] = TargetSlot;
		Pass.Used.Add(TargetSlot);
		const FName SourceName = Declaration.Bones[SourceIndex].Name;
		if (SourceName != Target[TargetSlot].Name)
		{
			Pass.BoneNameMappings.Add(FString::Printf(TEXT("%s -> %s"),
				*SourceName.ToString(), *Target[TargetSlot].Name.ToString()));
		}
	}

	/** Collects the children of one mapped parent and their complete candidate relation. */
	FMtoUScope BuildScope(
		const FMtoUSubjectDeclaration& Declaration,
		const TArray<FMtoUTargetBone>& Target,
		const TArray<int32>& Siblings,
		int32 ExpectedParent,
		const FMtoUMappingPass& Pass)
	{
		FMtoUScope Scope;
		Scope.ExpectedParent = ExpectedParent;
		Scope.Siblings = Siblings;
		Scope.Bones.SetNum(Siblings.Num());
		for (int32 Slot = 0; Slot < Siblings.Num(); ++Slot)
		{
			const FName Name = Declaration.Bones[Siblings[Slot]].Name;
			Scope.SiblingNames.Add(Name);
			int32& Count = Scope.NameCounts.FindOrAdd(Name);
			if (Count == 0)
			{
				Scope.FirstWithName.Add(Name, Slot);
			}
			++Count;
		}
		for (int32 Slot = 0; Slot < Siblings.Num(); ++Slot)
		{
			const int32 SourceIndex = Siblings[Slot];
			const FMtoUBoneDeclaration& SourceBone = Declaration.Bones[SourceIndex];
			FMtoUScopeBone& Bone = Scope.Bones[Slot];
			Bone.SourceIndex = SourceIndex;
			for (int32 TargetSlot = 0; TargetSlot < Target.Num(); ++TargetSlot)
			{
				if (Pass.Used.Contains(TargetSlot))
				{
					continue;
				}
				const FMtoUTargetBone& TargetBone = Target[TargetSlot];
				if (TargetBone.Name == SourceBone.Name)
				{
					if (TargetBone.Parent == ExpectedParent)
					{
						if (Bone.ExactCount == 0)
						{
							Bone.ExactTarget = TargetSlot;
						}
						++Bone.ExactCount;
					}
				}
				// #44/#46 rename applicability: a numeric suffix, or `_` plus 32
				// hexadecimal digits, of the complete declared name; only inside
				// the mapped parent scope; never for a target bone an exact name
				// owns. The complete relation below decides contested candidates
				// by unique assignment instead of capture order.
				else if (SourceBone.Parent != INDEX_NONE
					&& TargetBone.Parent == ExpectedParent
					&& IsImportedRename(SourceBone.Name, TargetBone.Name)
					&& !Scope.SiblingNames.Contains(TargetBone.Name))
				{
					Bone.RenameTargets.Add(TargetSlot);
				}
			}
		}
		return Scope;
	}

	/**
	 * Exact names claim their targets first. Children sharing one short name are
	 * indistinguishable, so none of them may own the target even when a renamed
	 * target is still free.
	 */
	void ResolveExactNames(
		const FMtoUSubjectDeclaration& Declaration,
		const TArray<FMtoUTargetBone>& Target,
		FMtoUScope& Scope,
		FMtoUMappingPass& Pass)
	{
		for (int32 Slot = 0; Slot < Scope.Bones.Num(); ++Slot)
		{
			FMtoUScopeBone& Bone = Scope.Bones[Slot];
			const FName Name = Declaration.Bones[Bone.SourceIndex].Name;
			const int32 NameCount = Scope.NameCounts.FindChecked(Name);
			if (NameCount > 1)
			{
				if (Bone.ExactCount == 0)
				{
					continue;   // the import renames below decide this name
				}
				if (Scope.FirstWithName.FindChecked(Name) == Slot)
				{
					TArray<FString> Paths;
					Paths.Reserve(NameCount);
					for (const int32 Other : Scope.Siblings)
					{
						if (Declaration.Bones[Other].Name == Name)
						{
							Paths.Add(SourcePath(Declaration.Bones, Other));
						}
					}
					Paths.Sort();
					Pass.MappingAmbiguities.Add(FString::Printf(
						TEXT("'%s' below %s is claimed by %d declared bones: %s"),
						*Name.ToString(),
						*TargetScopeParentLabel(Target, Scope.ExpectedParent),
						NameCount,
						*FString::Join(Paths, TEXT(", "))));
				}
				Bone.bResolved = true;
				continue;
			}
			if (Bone.ExactCount == 1)
			{
				Bone.bResolved = true;
				MapScopeBone(Pass, Declaration, Target, Bone.SourceIndex, Bone.ExactTarget);
				continue;
			}
			if (Bone.ExactCount > 1)
			{
				Bone.bResolved = true;
				Pass.MappingAmbiguities.Add(FString::Printf(
					TEXT("%s below %s has %d candidates"),
					*Name.ToString(),
					*TargetScopeParentLabel(Target, Scope.ExpectedParent),
					Bone.ExactCount));
			}
		}
	}

	/**
	 * True when another assignment of sources to targets covers the same targets.
	 * The alternating digraph of the matching gains one virtual node joined to
	 * every free source and to every target, so a cycle is exactly an alternating
	 * cycle or an alternating path from a free source.
	 */
	bool HasCompetingAssignment(
		const FMtoUScope& Scope,
		const TArray<int32>& SourceSlots,
		const TArray<int32>& TargetSlots,
		const TArray<int32>& SourceOfTarget,
		const TArray<int32>& TargetOfSource,
		const TMap<int32, int32>& TargetSlotByIndex)
	{
		const int32 NodeCount = SourceSlots.Num() + TargetSlots.Num() + 1;
		const int32 VirtualNode = NodeCount - 1;
		TArray<TArray<int32>> Edges;
		Edges.SetNum(NodeCount);
		TArray<int32> InDegree;
		InDegree.Init(0, NodeCount);
		const auto AddEdge = [&](int32 From, int32 To)
		{
			Edges[From].Add(To);
			++InDegree[To];
		};
		for (int32 TargetSlot = 0; TargetSlot < TargetSlots.Num(); ++TargetSlot)
		{
			AddEdge(SourceSlots.Num() + TargetSlot, SourceOfTarget[TargetSlot]);
			AddEdge(SourceSlots.Num() + TargetSlot, VirtualNode);
		}
		for (int32 SourceSlot = 0; SourceSlot < SourceSlots.Num(); ++SourceSlot)
		{
			if (TargetOfSource[SourceSlot] == INDEX_NONE)
			{
				AddEdge(VirtualNode, SourceSlot);
			}
			for (const int32 Candidate : Scope.Bones[SourceSlots[SourceSlot]].RenameTargets)
			{
				const int32* TargetSlot = TargetSlotByIndex.Find(Candidate);
				if (TargetSlot && *TargetSlot != TargetOfSource[SourceSlot])
				{
					AddEdge(SourceSlot, SourceSlots.Num() + *TargetSlot);
				}
			}
		}
		TArray<int32> Ready;
		for (int32 Node = 0; Node < NodeCount; ++Node)
		{
			if (InDegree[Node] == 0)
			{
				Ready.Add(Node);
			}
		}
		int32 Removed = 0;
		for (int32 Cursor = 0; Cursor < Ready.Num(); ++Cursor)
		{
			++Removed;
			for (const int32 Next : Edges[Ready[Cursor]])
			{
				if (--InDegree[Next] == 0)
				{
					Ready.Add(Next);
				}
			}
		}
		return Removed != NodeCount;
	}

	/**
	 * Import renames are resolved as one relation over the whole scope: a target
	 * an exact sibling name reserves is never a rename target, the necessary
	 * targets are driven only by the assignment that is the only one covering
	 * them, and a source that can drive several remaining targets is refused
	 * rather than settled by capture order.
	 */
	void ResolveImportRenames(
		const FMtoUSubjectDeclaration& Declaration,
		const TArray<FMtoUTargetBone>& Target,
		FMtoUScope& Scope,
		FMtoUMappingPass& Pass)
	{
		TArray<int32> SourceSlots;
		for (int32 Slot = 0; Slot < Scope.Bones.Num(); ++Slot)
		{
			if (!Scope.Bones[Slot].bResolved && !Scope.Bones[Slot].RenameTargets.IsEmpty())
			{
				SourceSlots.Add(Slot);
			}
		}
		TArray<int32> TargetSlots;
		TMap<int32, int32> TargetSlotByIndex;
		for (int32 Candidate = 0; Candidate < Target.Num(); ++Candidate)
		{
			if (Target[Candidate].Parent == Scope.ExpectedParent
				&& !Scope.SiblingNames.Contains(Target[Candidate].Name)
				&& !Pass.Used.Contains(Candidate))
			{
				TargetSlotByIndex.Add(Candidate, TargetSlots.Add(Candidate));
			}
		}
		if (SourceSlots.IsEmpty() || TargetSlots.IsEmpty())
		{
			return;
		}

		TArray<TArray<int32>> TargetSources;
		TargetSources.SetNum(TargetSlots.Num());
		TArray<int32> SourceOfTarget;
		SourceOfTarget.Init(INDEX_NONE, TargetSlots.Num());
		TArray<int32> TargetOfSource;
		TargetOfSource.Init(INDEX_NONE, SourceSlots.Num());
		for (int32 SourceSlot = 0; SourceSlot < SourceSlots.Num(); ++SourceSlot)
		{
			for (const int32 Candidate : Scope.Bones[SourceSlots[SourceSlot]].RenameTargets)
			{
				if (const int32* TargetSlot = TargetSlotByIndex.Find(Candidate))
				{
					TargetSources[*TargetSlot].Add(SourceSlot);
				}
			}
		}

		// Maximum matching, targets and sources both in capture order.
		auto TryMatch = [&](auto&& Self, int32 TargetSlot, TArray<bool>& Visited) -> bool
		{
			for (const int32 SourceSlot : TargetSources[TargetSlot])
			{
				if (Visited[SourceSlot])
				{
					continue;
				}
				Visited[SourceSlot] = true;
				const int32 PreviousTarget = TargetOfSource[SourceSlot];
				if (PreviousTarget == INDEX_NONE || Self(Self, PreviousTarget, Visited))
				{
					TargetOfSource[SourceSlot] = TargetSlot;
					SourceOfTarget[TargetSlot] = SourceSlot;
					return true;
				}
			}
			return false;
		};
		for (int32 TargetSlot = 0; TargetSlot < TargetSlots.Num(); ++TargetSlot)
		{
			TArray<bool> Visited;
			Visited.Init(false, SourceSlots.Num());
			TryMatch(TryMatch, TargetSlot, Visited);
		}

		bool bCovered = true;
		for (int32 TargetSlot = 0; TargetSlot < TargetSlots.Num(); ++TargetSlot)
		{
			bCovered &= SourceOfTarget[TargetSlot] != INDEX_NONE;
		}
		if (bCovered
			&& !HasCompetingAssignment(Scope, SourceSlots, TargetSlots,
				SourceOfTarget, TargetOfSource, TargetSlotByIndex))
		{
			for (int32 SourceSlot = 0; SourceSlot < SourceSlots.Num(); ++SourceSlot)
			{
				const int32 TargetSlot = TargetOfSource[SourceSlot];
				if (TargetSlot != INDEX_NONE)
				{
					FMtoUScopeBone& Bone = Scope.Bones[SourceSlots[SourceSlot]];
					Bone.bResolved = true;
					MapScopeBone(Pass, Declaration, Target, Bone.SourceIndex, TargetSlots[TargetSlot]);
				}
			}
			return;
		}

		// Refuse instead of splitting the sources by capture order: report every
		// target two or more sources can drive, then every source that still has
		// a choice between targets. A source with a single, uncontested target is
		// forced and keeps its mapping.
		for (int32 TargetSlot = 0; TargetSlot < TargetSlots.Num(); ++TargetSlot)
		{
			if (TargetSources[TargetSlot].Num() < 2)
			{
				continue;
			}
			TArray<FString> Paths;
			Paths.Reserve(TargetSources[TargetSlot].Num());
			for (const int32 SourceSlot : TargetSources[TargetSlot])
			{
				Paths.Add(SourcePath(Declaration.Bones, Scope.Bones[SourceSlots[SourceSlot]].SourceIndex));
			}
			Paths.Sort();
			Pass.MappingAmbiguities.Add(FString::Printf(
				TEXT("'%s' below %s is claimed by %d declared bones: %s"),
				*Target[TargetSlots[TargetSlot]].Name.ToString(),
				*TargetScopeParentLabel(Target, Scope.ExpectedParent),
				Paths.Num(),
				*FString::Join(Paths, TEXT(", "))));
			for (const int32 SourceSlot : TargetSources[TargetSlot])
			{
				FMtoUScopeBone& Bone = Scope.Bones[SourceSlots[SourceSlot]];
				if (!Bone.bResolved)
				{
					Bone.bResolved = true;
				}
			}
		}
		for (int32 SourceSlot = 0; SourceSlot < SourceSlots.Num(); ++SourceSlot)
		{
			FMtoUScopeBone& Bone = Scope.Bones[SourceSlots[SourceSlot]];
			if (Bone.bResolved)
			{
				continue;
			}
			TArray<int32> Feasible;
			for (const int32 Candidate : Bone.RenameTargets)
			{
				if (TargetSlotByIndex.Contains(Candidate))
				{
					Feasible.Add(Candidate);
				}
			}
			Bone.bResolved = true;
			if (Feasible.Num() == 1)
			{
				// No other source can drive this target, so every covering
				// assignment uses the pair.
				MapScopeBone(Pass, Declaration, Target, Bone.SourceIndex, Feasible[0]);
				continue;
			}
			Pass.MappingAmbiguities.Add(FString::Printf(
				TEXT("%s below %s has %d candidates"),
				*Declaration.Bones[Bone.SourceIndex].Name.ToString(),
				*TargetScopeParentLabel(Target, Scope.ExpectedParent),
				Feasible.Num()));
		}
	}

	/** Children with no candidate at all are ignored export branches. */
	void ResolveUnmatchedSiblings(FMtoUScope& Scope, FMtoUMappingPass& Pass)
	{
		for (FMtoUScopeBone& Bone : Scope.Bones)
		{
			if (Bone.bResolved)
			{
				continue;
			}
			Bone.bResolved = true;
			Pass.Skipped.Add(Bone.SourceIndex);
		}
	}

	/** Resolves every child of one mapped parent as a single scope. */
	void ResolveScope(
		const FMtoUSubjectDeclaration& Declaration,
		const TArray<FMtoUTargetBone>& Target,
		const TArray<int32>& Siblings,
		int32 ExpectedParent,
		FMtoUMappingPass& Pass)
	{
		FMtoUScope Scope = BuildScope(
			Declaration, Target, Siblings, ExpectedParent, Pass);
		ResolveExactNames(Declaration, Target, Scope, Pass);
		ResolveImportRenames(Declaration, Target, Scope, Pass);
		ResolveUnmatchedSiblings(Scope, Pass);
	}

	/**
	 * Maps declared bones parent-first. Every child of one mapped parent is
	 * resolved as a single scope, over its complete candidate relation, so no
	 * mapping decision depends on the order the declaration lists its bones in.
	 */
	FMtoUMappingPass RunMappingPass(
		const FMtoUSubjectDeclaration& Declaration,
		const TArray<FMtoUTargetBone>& Target)
	{
		FMtoUMappingPass Pass;
		Pass.SourceToTarget.Init(INDEX_NONE, Declaration.Bones.Num());

		// Children of every declared bone, and the roots, in declaration order.
		const int32 RootScope = Declaration.Bones.Num();
		TArray<TArray<int32>> Children;
		Children.SetNum(Declaration.Bones.Num() + 1);
		for (int32 SourceIndex = 0; SourceIndex < Declaration.Bones.Num(); ++SourceIndex)
		{
			const int32 ParentIndex = Declaration.Bones[SourceIndex].Parent;
			Children[Declaration.Bones.IsValidIndex(ParentIndex) ? ParentIndex : RootScope].Add(SourceIndex);
		}

		// Scopes are resolved top-down, so a bone is negotiated only after its
		// parent is mapped. Children of a skipped or unmapped parent are ignored
		// export branches too: they cannot address a mapped parent scope.
		TArray<int32> PendingScopes;
		PendingScopes.Add(RootScope);
		for (int32 Cursor = 0; Cursor < PendingScopes.Num(); ++Cursor)
		{
			const int32 ScopeKey = PendingScopes[Cursor];
			if (ScopeKey != RootScope
				&& (Pass.Skipped.Contains(ScopeKey) || Pass.SourceToTarget[ScopeKey] == INDEX_NONE))
			{
				for (const int32 Child : Children[ScopeKey])
				{
					Pass.Skipped.Add(Child);
				}
				PendingScopes.Append(Children[ScopeKey]);
				continue;
			}
			const int32 ExpectedParent = ScopeKey == RootScope
				? INDEX_NONE
				: Pass.SourceToTarget[ScopeKey];
			ResolveScope(Declaration, Target, Children[ScopeKey], ExpectedParent, Pass);
			PendingScopes.Append(Children[ScopeKey]);
		}
		return Pass;
	}
}

FString FMtoUMultiSubjectProtocol::DescribeNegotiationMismatch(
	const FMtoUSubjectDeclaration& Declaration,
	const USkeletalMesh& Mesh,
	FMtoUNegotiationMap& OutMap)
{
	OutMap = FMtoUNegotiationMap();
	if (Declaration.Bind.Num() != Declaration.Bones.Num())
	{
		return FString::Printf(
			TEXT("Maya advertises %d bind poses for %d bones"),
			Declaration.Bind.Num(), Declaration.Bones.Num());
	}
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();

	TArray<bool> Required;
	FString SkinProblem;
	if (!CollectRequiredTargetBones(Mesh, Required, SkinProblem))
	{
		return FString::Printf(TEXT("the Unreal target %s"), *SkinProblem);
	}
	// The negotiated *target* of the necessary-dependency rules is the necessary
	// set: the bones that deform the mesh and their ancestors. It is what the
	// declaration must cover and what the two rigs have to agree on. Other
	// target bones deform nothing, so a matched one only follows its declared
	// motion and a bind difference there never vetoes the declaration.
	TArray<FMtoUTargetBone> Target;
	Target.SetNum(Skeleton.GetNum());
	for (int32 BoneIndex = 0; BoneIndex < Skeleton.GetNum(); ++BoneIndex)
	{
		if (Required[BoneIndex])
		{
			OutMap.RequiredTargetBones.Add(BoneIndex);
		}
		Target[BoneIndex].Name = Skeleton.GetBoneName(BoneIndex);
		Target[BoneIndex].Parent = Skeleton.GetParentIndex(BoneIndex);
	}

	// Every child of one mapped parent is resolved as one scope over its
	// complete candidate relation, and the targets are driven by the one
	// assignment that covers them: the decision never depends on the order the
	// declaration lists its bones in. Only the necessary set above is required
	// and bind-checked; other matched bones follow their declared motion.
	const FMtoUMappingPass Pass = RunMappingPass(Declaration, Target);

	OutMap.SourceToTarget.Init(INDEX_NONE, Declaration.Bones.Num());
	TArray<int32> ClaimedBy;
	ClaimedBy.Init(INDEX_NONE, Skeleton.GetNum());
	for (int32 SourceIndex = 0; SourceIndex < Declaration.Bones.Num(); ++SourceIndex)
	{
		const int32 TargetSlot = Pass.SourceToTarget[SourceIndex];
		if (TargetSlot == INDEX_NONE)
		{
			continue;
		}
		const int32 BoneIndex = TargetSlot;
		OutMap.SourceToTarget[SourceIndex] = BoneIndex;
		OutMap.DrivenTargetBones.Add(BoneIndex);
		ClaimedBy[BoneIndex] = SourceIndex;
	}
	OutMap.ImportRenames = Pass.BoneNameMappings;
	OutMap.SourceOnlyBones = Declaration.Bones.Num() - OutMap.DrivenTargetBones.Num();

	// Two sources claiming one target, or one source with several targets the
	// relation does not settle, are refused before anything is applied instead
	// of being settled by capture order.
	if (!Pass.MappingAmbiguities.IsEmpty())
	{
		TArray<FString> Ambiguities = Pass.MappingAmbiguities;
		Ambiguities.Sort();
		return FString::Printf(TEXT("the declared mapping is ambiguous: %s"), *Ambiguities[0]);
	}

	// Coverage: every bone that deforms the target mesh, and every ancestor of
	// one, has to be driven by a declared bone. Omitting one would leave that
	// branch at its reference pose while the rest of the mesh moves.
	for (const int32 RequiredBone : OutMap.RequiredTargetBones)
	{
		if (ClaimedBy[RequiredBone] != INDEX_NONE)
		{
			continue;
		}
		const FName RequiredName = Skeleton.GetBoneName(RequiredBone);
		// The most likely cause is a same-named source bone under another
		// parent, so the refusal names it instead of only the target bone.
		FString Hint;
		for (int32 SourceIndex = 0; SourceIndex < Declaration.Bones.Num(); ++SourceIndex)
		{
			if (Declaration.Bones[SourceIndex].Name != RequiredName)
			{
				continue;
			}
			const int32 SourceParent = Declaration.Bones[SourceIndex].Parent;
			const int32 MappedParent = SourceParent == INDEX_NONE
				? INDEX_NONE
				: OutMap.SourceToTarget[SourceParent];
			Hint = MappedParent == INDEX_NONE
				? FString::Printf(
					TEXT("; the declaration's '%s' sits in an export branch the target does not drive"),
					*RequiredName.ToString())
				: FString::Printf(
					TEXT("; the declaration's '%s' sits below '%s'"),
					*RequiredName.ToString(), *TargetParentLabel(Skeleton, MappedParent));
			break;
		}
		if (Hint.IsEmpty())
		{
			// #46 diagnostic boundary: declared bones shaped like an importer
			// rename of the uncovered bone, and why the strict rules did not
			// apply them, so a refused rename is never silent.
			TArray<FString> RenameHints;
			for (int32 SourceIndex = 0; SourceIndex < Declaration.Bones.Num(); ++SourceIndex)
			{
				const FMtoUBoneDeclaration& SourceBone = Declaration.Bones[SourceIndex];
				if (SourceBone.Parent == INDEX_NONE
					|| !IsImportedRename(SourceBone.Name, RequiredName))
				{
					continue;
				}
				RenameHints.Add(FString::Printf(
					TEXT("'%s' (it addresses %s)"),
					*SourceBone.Name.ToString(),
					OutMap.SourceToTarget[SourceIndex] == INDEX_NONE
						? TEXT("an ignored export branch")
						: TEXT("another target bone")));
			}
			if (!RenameHints.IsEmpty())
			{
				RenameHints.Sort();
				Hint = FString::Printf(
					TEXT("; declared %s look like importer renames of it and were not applied"),
					*FString::Join(RenameHints, TEXT(", ")));
			}
		}
		return FString::Printf(
			TEXT("target bone '%s' below '%s' deforms the mesh but the declaration drives no bone for it%s"),
			*RequiredName.ToString(), *TargetParentLabel(Skeleton, RequiredBone), *Hint);
	}

	// Bind/frame projection data. The target keeps its own reference pose and
	// receives the declared bone's motion, so the two rigs only have to agree up
	// to one constant component-space frame (a rigging/import convention at the
	// root joint). The poses are composed here, once per negotiation.
	const TArray<FTransform>& TargetRefPose = Skeleton.GetRefBonePose();
	OutMap.TargetRefComponentPose.Init(FTransform::Identity, Declaration.Bones.Num());
	OutMap.SourceBindComponentPose.Init(FTransform::Identity, Declaration.Bones.Num());
	for (int32 SourceIndex = 0; SourceIndex < Declaration.Bones.Num(); ++SourceIndex)
	{
		if (OutMap.SourceToTarget[SourceIndex] == INDEX_NONE)
		{
			continue;
		}
		const int32 Parent = Declaration.Bones[SourceIndex].Parent;
		const FTransform ParentTarget =
			Parent == INDEX_NONE ? FTransform::Identity : OutMap.TargetRefComponentPose[Parent];
		const FTransform ParentSourceBind =
			Parent == INDEX_NONE ? FTransform::Identity : OutMap.SourceBindComponentPose[Parent];
		OutMap.TargetRefComponentPose[SourceIndex] =
			TargetRefPose[OutMap.SourceToTarget[SourceIndex]] * ParentTarget;
		OutMap.SourceBindComponentPose[SourceIndex] =
			Declaration.Bind[SourceIndex] * ParentSourceBind;
	}
	const int32 RootTarget = OutMap.SourceToTarget.IsValidIndex(0)
		? OutMap.SourceToTarget[0]
		: INDEX_NONE;
	if (RootTarget == INDEX_NONE)
	{
		return TEXT("the declaration's first bone does not map to the target's root bone");
	}
	OutMap.RootFrame = OutMap.SourceBindComponentPose[0]
		* OutMap.TargetRefComponentPose[0].Inverse();
	const FTransform InverseRootFrame = OutMap.RootFrame.Inverse();
	FName WorstBone = NAME_None;
	double WorstRatio = 0.0;
	for (int32 SourceIndex = 0; SourceIndex < Declaration.Bones.Num(); ++SourceIndex)
	{
		if (OutMap.SourceToTarget[SourceIndex] == INDEX_NONE)
		{
			continue;
		}
		// Every inverse the projection performs has to exist; a zero-scaled bone
		// would silently corrupt its whole branch.
		const FTransform& SourceBind = OutMap.SourceBindComponentPose[SourceIndex];
		const FTransform& TargetRef = OutMap.TargetRefComponentPose[SourceIndex];
		const FName BoneName = Skeleton.GetBoneName(OutMap.SourceToTarget[SourceIndex]);
		if (SourceBind.GetScale3D().GetAbsMin() < RestScaleFloor
			|| TargetRef.GetScale3D().GetAbsMin() < RestScaleFloor)
		{
			return FString::Printf(
				TEXT("target bone '%s' or its declared source counterpart rests with a "
					 "zero scale, so the pose cannot be projected"),
				*BoneName.ToString());
		}
		// Only the necessary set carries the rig-agreement check: a bone that
		// deforms nothing can rest anywhere, so a non-essential bind difference
		// never vetoes the declaration. A mapped bone outside it is still
		// projected, so its bind only has to stay invertible.
		if (!Required[OutMap.SourceToTarget[SourceIndex]])
		{
			continue;
		}
		const FTransform Aligned = InverseRootFrame * SourceBind;
		const double TranslationDelta = static_cast<double>(
			(Aligned.GetTranslation() - TargetRef.GetTranslation()).GetAbsMax());
		const double RotationDelta = FMath::RadiansToDegrees(
			Aligned.GetRotation().AngularDistance(TargetRef.GetRotation()));
		OutMap.MaxRestTranslationCm = FMath::Max(OutMap.MaxRestTranslationCm, TranslationDelta);
		OutMap.MaxRestRotationDegrees = FMath::Max(OutMap.MaxRestRotationDegrees, RotationDelta);
		const FVector AlignedScale = Aligned.GetScale3D();
		const FVector TargetScale = TargetRef.GetScale3D();
		double ScaleDelta = 0.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Reference = FMath::Max(1.0, FMath::Abs(TargetScale[Axis]));
			ScaleDelta = FMath::Max(ScaleDelta,
				FMath::Abs(AlignedScale[Axis] - TargetScale[Axis]) / Reference);
		}
		OutMap.MaxRestScale = FMath::Max(OutMap.MaxRestScale, ScaleDelta);
		const double Ratio = FMath::Max3(
			TranslationDelta / RestTranslationBoundCm,
			RotationDelta / RestRotationBoundDegrees,
			ScaleDelta / RestScaleBound);
		if (Ratio > WorstRatio)
		{
			WorstRatio = Ratio;
			WorstBone = BoneName;
		}
	}
	// A rig that rests somewhere else is a different rig, not a bind convention:
	// the prototype refuses gross disagreements and reports the measured values.
	if (OutMap.MaxRestTranslationCm > RestTranslationBoundCm
		|| OutMap.MaxRestRotationDegrees > RestRotationBoundDegrees
		|| OutMap.MaxRestScale > RestScaleBound)
	{
		return FString::Printf(
			TEXT("the declared bind of '%s' disagrees with the Unreal target rest pose beyond the ")
			TEXT("prototype bounds (%.4f cm, %.4f deg, %.4f scale; bounds %.2f cm, %.2f deg, %.4f), ")
			TEXT("so this is a different rig"),
			*WorstBone.ToString(), OutMap.MaxRestTranslationCm, OutMap.MaxRestRotationDegrees,
			OutMap.MaxRestScale, RestTranslationBoundCm, RestRotationBoundDegrees, RestScaleBound);
	}

	for (int32 BoneIndex = 0; BoneIndex < Skeleton.GetNum(); ++BoneIndex)
	{
		if (ClaimedBy[BoneIndex] == INDEX_NONE)
		{
			OutMap.UndrivenTargetBones.Add(Skeleton.GetBoneName(BoneIndex));
		}
	}

	// A declared curve the target has no Morph Target for is an export extra:
	// it is not applied, and the evidence says so instead of implying the whole
	// source Morph set reached the mesh.
	for (const FName& Curve : Declaration.Curves)
	{
		bool bFound = false;
		for (const TObjectPtr<UMorphTarget>& Morph : Mesh.GetMorphTargets())
		{
			if (Morph != nullptr && Morph->GetFName() == Curve)
			{
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			OutMap.SourceOnlyCurves.Add(Curve);
		}
	}
	OutMap.bCoversRequiredBones = true;
	return FString();
}

bool FMtoUMultiSubjectProtocol::FindWeightedTargetVertex(
	const USkeletalMesh& Mesh,
	int32 BoneIndex,
	int32& OutLODIndex,
	int32& OutVertexIndex,
	FString& OutProblem)
{
	OutLODIndex = INDEX_NONE;
	OutVertexIndex = INDEX_NONE;
	OutProblem.Reset();
	const FSkeletalMeshRenderData* Data = Mesh.GetResourceForRendering();
	if (Data == nullptr || Data->LODRenderData.IsEmpty())
	{
		OutProblem = TEXT("has no readable render data");
		return false;
	}
	for (int32 LODIndex = 0; LODIndex < Data->LODRenderData.Num(); ++LODIndex)
	{
		const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[LODIndex];
		const FSkinWeightVertexBuffer& Weights = LOD.SkinWeightVertexBuffer;
		for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
		{
			for (uint32 Vertex = Section.BaseVertexIndex;
				Vertex < Section.BaseVertexIndex + Section.NumVertices; ++Vertex)
			{
				for (uint32 Influence = 0; Influence < Weights.GetMaxBoneInfluences(); ++Influence)
				{
					if (Weights.GetBoneWeight(Vertex, Influence) == 0)
					{
						continue;
					}
					const uint32 PaletteIndex = Weights.GetBoneIndex(Vertex, Influence);
					if (!Section.BoneMap.IsValidIndex(PaletteIndex))
					{
						continue;
					}
					if (Section.BoneMap[PaletteIndex] == BoneIndex)
					{
						OutLODIndex = LODIndex;
						OutVertexIndex = static_cast<int32>(Vertex);
						return true;
					}
				}
			}
		}
	}
	OutProblem = FString::Printf(
		TEXT("has no vertex weighted to bone %d"), BoneIndex);
	return false;
}

FString FMtoUMultiSubjectProtocol::DescribeSkeletonSignature(const FReferenceSkeleton& Skeleton)
{
	TArray<FString> Parts;
	Parts.Reserve(Skeleton.GetNum());
	for (int32 BoneIndex = 0; BoneIndex < Skeleton.GetNum(); ++BoneIndex)
	{
		const int32 ParentIndex = Skeleton.GetParentIndex(BoneIndex);
		Parts.Add(FString::Printf(TEXT("%d:%s<-%s"), BoneIndex,
			*Skeleton.GetBoneName(BoneIndex).ToString(),
			ParentIndex == INDEX_NONE ? TEXT("none") : *Skeleton.GetBoneName(ParentIndex).ToString()));
	}
	return FString::Join(Parts, TEXT(", "));
}
