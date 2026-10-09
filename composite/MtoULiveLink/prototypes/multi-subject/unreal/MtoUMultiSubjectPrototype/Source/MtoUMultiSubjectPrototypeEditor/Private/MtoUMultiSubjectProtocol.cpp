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
	/** All target bones with one name: a skeleton may repeat a name under different parents. */
	void IndexTargetBonesByName(
		const FReferenceSkeleton& Skeleton,
		TMap<FName, TArray<int32>>& OutIndices)
	{
		for (int32 BoneIndex = 0; BoneIndex < Skeleton.GetNum(); ++BoneIndex)
		{
			OutIndices.FindOrAdd(Skeleton.GetBoneName(BoneIndex)).Add(BoneIndex);
		}
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
	for (int32 BoneIndex = 0; BoneIndex < Skeleton.GetNum(); ++BoneIndex)
	{
		if (Required[BoneIndex])
		{
			OutMap.RequiredTargetBones.Add(BoneIndex);
		}
	}

	TMap<FName, TArray<int32>> TargetByName;
	IndexTargetBonesByName(Skeleton, TargetByName);

	// The assignment is built in declaration order, which the wire contract
	// already requires to be parent-first, so a bone's mapped parent is known
	// before the bone itself is resolved. Each round sweeps exact names and
	// then the importer's rename forms (a numeric suffix, or `_` plus the 32
	// hexadecimal digits of one import hash), and repeats while either sweep
	// claims something: a renamed bone may be the parent scope its own child
	// resolves in. An exact name is only ever claimed for a target bone no
	// exact name owns, so a rename can never displace an exact match.
	OutMap.SourceToTarget.Init(INDEX_NONE, Declaration.Bones.Num());
	TArray<int32> ClaimedBy;
	ClaimedBy.Init(INDEX_NONE, Skeleton.GetNum());
	bool bProgress = true;
	while (bProgress)
	{
		bProgress = false;
		for (int32 Sweep = 0; Sweep < 2; ++Sweep)
		{
			const bool bRenames = Sweep == 1;
			for (int32 SourceIndex = 0; SourceIndex < Declaration.Bones.Num(); ++SourceIndex)
			{
				if (OutMap.SourceToTarget[SourceIndex] != INDEX_NONE)
				{
					continue;
				}
				const FMtoUBoneDeclaration& Bone = Declaration.Bones[SourceIndex];
				int32 MappedParent = INDEX_NONE;
				if (Bone.Parent != INDEX_NONE)
				{
					MappedParent = OutMap.SourceToTarget[Bone.Parent];
					if (MappedParent == INDEX_NONE)
					{
						// The source parent is an ignored export branch, so this
						// bone cannot be addressed in a mapped parent scope either.
						continue;
					}
				}
				int32 Match = INDEX_NONE;
				if (!bRenames)
				{
					const TArray<int32>* const Exact = TargetByName.Find(Bone.Name);
					if (Exact != nullptr)
					{
						for (const int32 Candidate : *Exact)
						{
							if (Skeleton.GetParentIndex(Candidate) != MappedParent)
							{
								continue;
							}
							if (Match != INDEX_NONE)
							{
								return FString::Printf(
									TEXT("the Unreal target skeleton names two bones '%s' below '%s'; "
										 "a declaration cannot address them unambiguously"),
									*Bone.Name.ToString(), *TargetParentLabel(Skeleton, Match));
							}
							Match = Candidate;
						}
					}
				}
				else
				{
					// One rename candidate in the same parent scope; two would be
					// an ambiguity, so a suffix never picks silently.
					for (int32 Candidate = 0; Candidate < Skeleton.GetNum(); ++Candidate)
					{
						if (Skeleton.GetParentIndex(Candidate) != MappedParent
							|| ClaimedBy[Candidate] != INDEX_NONE
							|| !IsImportedRename(Bone.Name, Skeleton.GetBoneName(Candidate)))
						{
							continue;
						}
						if (Match != INDEX_NONE)
						{
							return FString::Printf(
								TEXT("two Unreal target bones ('%s' and '%s') look like imports of '%s' "
									 "below '%s'; the rename is ambiguous"),
								*Skeleton.GetBoneName(Match).ToString(),
								*Skeleton.GetBoneName(Candidate).ToString(),
								*Bone.Name.ToString(), *TargetParentLabel(Skeleton, Match));
						}
						Match = Candidate;
					}
				}
				if (Match == INDEX_NONE)
				{
					// Nothing in this sweep; a later sweep may still resolve it
					// once its parent scope is mapped.
					continue;
				}
				if (ClaimedBy[Match] != INDEX_NONE)
				{
					return FString::Printf(
						TEXT("Maya bones %d and %d both map to target bone '%s' below '%s'; "
							 "the source mapping is ambiguous"),
						ClaimedBy[Match], SourceIndex, *Bone.Name.ToString(),
						*TargetParentLabel(Skeleton, Match));
				}
				ClaimedBy[Match] = SourceIndex;
				OutMap.SourceToTarget[SourceIndex] = Match;
				OutMap.DrivenTargetBones.Add(Match);
				bProgress = true;
				if (bRenames)
				{
					OutMap.ImportRenames.Add(FString::Printf(TEXT("%s -> %s"),
						*Bone.Name.ToString(), *Skeleton.GetBoneName(Match).ToString()));
				}
			}
		}
	}
	OutMap.SourceOnlyBones = Declaration.Bones.Num() - OutMap.DrivenTargetBones.Num();

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
