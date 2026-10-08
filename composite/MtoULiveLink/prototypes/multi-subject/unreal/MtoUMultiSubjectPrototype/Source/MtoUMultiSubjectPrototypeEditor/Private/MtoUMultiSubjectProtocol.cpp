// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "MtoUMultiSubjectProtocol.h"

#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
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

FString FMtoUMultiSubjectProtocol::DescribeSkeletonMismatch(
	const FMtoUSubjectDeclaration& Declaration,
	const FReferenceSkeleton& Skeleton)
{
	TSet<FName> Declared;
	Declared.Reserve(Declaration.Bones.Num());
	for (const FMtoUBoneDeclaration& Bone : Declaration.Bones)
	{
		Declared.Add(Bone.Name);
	}
	// The target may legitimately own more bones than the sender drives (a
	// simplified preview, a facial rig, a production branch the input does not
	// move), so only the declared bones are compared; what matters is that the
	// sender never leaves one of its bones under a bone it does not drive.
	for (const FMtoUBoneDeclaration& Bone : Declaration.Bones)
	{
		// A bone the target does not have is reported by the identity loop below;
		// walking its ancestors here would index the skeleton with -1.
		const int32 TargetBone = Skeleton.FindBoneIndex(Bone.Name);
		int32 TargetAncestor = TargetBone == INDEX_NONE
			? INDEX_NONE
			: Skeleton.GetParentIndex(TargetBone);
		while (TargetAncestor != INDEX_NONE)
		{
			const FName AncestorName = Skeleton.GetBoneName(TargetAncestor);
			if (!Declared.Contains(AncestorName))
			{
				return FString::Printf(
					TEXT("Maya bone '%s' needs '%s', which this declaration does not drive, "
						 "so its local pose would hang under an undriven bone"),
					*Bone.Name.ToString(), *AncestorName.ToString());
			}
			TargetAncestor = Skeleton.GetParentIndex(TargetAncestor);
		}
	}
	for (int32 BoneIndex = 0; BoneIndex < Declaration.Bones.Num(); ++BoneIndex)
	{
		const FName DeclaredName = Declaration.Bones[BoneIndex].Name;
		const int32 TargetIndex = Skeleton.FindBoneIndex(DeclaredName);
		if (TargetIndex == INDEX_NONE)
		{
			return FString::Printf(
				TEXT("Maya bone '%s' (parent %d) is not in the Unreal target skeleton (%s)"),
				*DeclaredName.ToString(), Declaration.Bones[BoneIndex].Parent,
				*DescribeSkeletonSignature(Skeleton));
		}
		const int32 DeclaredParent = Declaration.Bones[BoneIndex].Parent;
		const FName DeclaredParentName = DeclaredParent == INDEX_NONE
			? NAME_None
			: Declaration.Bones[DeclaredParent].Name;
		const int32 TargetParent = Skeleton.GetParentIndex(TargetIndex);
		const FName TargetParentName = TargetParent == INDEX_NONE
			? NAME_None
			: Skeleton.GetBoneName(TargetParent);
		if (DeclaredParentName != TargetParentName)
		{
			return FString::Printf(
				TEXT("Maya bone '%s' has parent '%s', but the Unreal target parents it to '%s'"),
				*DeclaredName.ToString(),
				*DeclaredParentName.ToString(),
				*TargetParentName.ToString());
		}
	}
	return FString();
}

void FMtoUMultiSubjectProtocol::CollectUndrivenBones(
	const FMtoUSubjectDeclaration& Declaration,
	const FReferenceSkeleton& Skeleton,
	TArray<FName>& OutBones)
{
	OutBones.Reset();
	TSet<FName> Declared;
	for (const FMtoUBoneDeclaration& Bone : Declaration.Bones)
	{
		Declared.Add(Bone.Name);
	}
	for (int32 BoneIndex = 0; BoneIndex < Skeleton.GetNum(); ++BoneIndex)
	{
		const FName BoneName = Skeleton.GetBoneName(BoneIndex);
		if (!Declared.Contains(BoneName))
		{
			OutBones.Add(BoneName);
		}
	}
}

/** Prototype tolerances for the bind comparison: float and axis-conversion noise, not deformation budget. */
constexpr double BindTranslationToleranceCm = 0.25;
constexpr double BindRotationToleranceDegrees = 0.5;
constexpr double BindScaleTolerance = 0.005;

FString FMtoUMultiSubjectProtocol::DescribeBindMismatch(
	const FMtoUSubjectDeclaration& Declaration,
	const FReferenceSkeleton& Skeleton)
{
	if (Declaration.Bind.Num() != Declaration.Bones.Num())
	{
		return FString::Printf(
			TEXT("Maya advertises %d bind poses for %d bones"),
			Declaration.Bind.Num(), Declaration.Bones.Num());
	}
	const TArray<FTransform>& ReferencePose = Skeleton.GetRefBonePose();
	for (int32 BoneIndex = 0; BoneIndex < Declaration.Bones.Num(); ++BoneIndex)
	{
		const int32 TargetIndex = Skeleton.FindBoneIndex(Declaration.Bones[BoneIndex].Name);
		if (!ReferencePose.IsValidIndex(TargetIndex))
		{
			continue;
		}
		const FTransform& Advertised = Declaration.Bind[BoneIndex];
		const FTransform& Reference = ReferencePose[TargetIndex];
		const double TranslationDelta =
			(Advertised.GetTranslation() - Reference.GetTranslation()).GetAbsMax();
		const double ScaleDelta =
			(Advertised.GetScale3D() - Reference.GetScale3D()).GetAbsMax();
		const double RotationDeltaDegrees = FMath::RadiansToDegrees(
			Advertised.GetRotation().AngularDistance(Reference.GetRotation()));
		if (TranslationDelta > BindTranslationToleranceCm
			|| RotationDeltaDegrees > BindRotationToleranceDegrees
			|| ScaleDelta > BindScaleTolerance)
		{
			return FString::Printf(
				TEXT("Maya bind of '%s' differs from the Unreal target reference pose ")
				TEXT("(translation %.4f cm, rotation %.4f deg, scale %.4f)"),
				*Declaration.Bones[BoneIndex].Name.ToString(),
				TranslationDelta, RotationDeltaDegrees, ScaleDelta);
		}
	}
	return FString();
}

bool FMtoUMultiSubjectProtocol::FindMissingCurve(
	const FMtoUSubjectDeclaration& Declaration,
	const USkeletalMeshComponent& Component,
	FName& OutMissing)
{
	const USkeletalMesh* Mesh = Component.GetSkeletalMeshAsset();
	if (Mesh == nullptr)
	{
		OutMissing = Declaration.Curves.IsEmpty() ? NAME_None : Declaration.Curves[0];
		return !Declaration.Curves.IsEmpty();
	}
	for (const FName& Curve : Declaration.Curves)
	{
		bool bFound = false;
		for (const TObjectPtr<UMorphTarget>& Morph : Mesh->GetMorphTargets())
		{
			if (Morph != nullptr && Morph->GetFName() == Curve)
			{
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			OutMissing = Curve;
			return true;
		}
	}
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
