#include "MtoUCharacterComposition.h"

#include "MtoULiveLinkBinding.h"

#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace
{
// A part's reference pose must agree with the Primary Driver closely enough
// that the streamed Primary-relative pose deforms both meshes identically. The
// translation and scale tolerance is relative to the Primary's reference-pose
// extent, so the same rule holds for a character rig measured in any unit.
constexpr double ReferencePoseRelativeTolerance = 1.e-3;
constexpr double ReferencePoseRotationToleranceDegrees = 1.0;
// Bones named per problem category before the remainder is summarized.
constexpr int32 MaxReportedBones = 8;

FString ParentName(const FReferenceSkeleton& Skeleton, int32 Index)
{
    return Index == INDEX_NONE ? FString(TEXT("<root>")) : Skeleton.GetBoneName(Index).ToString();
}

int32 ComponentSpacePoses(const FReferenceSkeleton& Skeleton, TArray<FTransform>& OutPoses)
{
    const int32 BoneCount = Skeleton.GetNum();
    OutPoses.SetNumUninitialized(BoneCount);
    const TArray<FTransform>& LocalPoses = Skeleton.GetRefBonePose();
    for (int32 Index = 0; Index < BoneCount; ++Index)
    {
        const int32 ParentIndex = Skeleton.GetParentIndex(Index);
        OutPoses[Index] = ParentIndex == INDEX_NONE
            ? LocalPoses[Index]
            : LocalPoses[Index] * OutPoses[ParentIndex];
    }
    return BoneCount;
}

FString JoinBones(const TArray<FString>& Bones)
{
    return Bones.Num() <= MaxReportedBones
        ? FString::Join(Bones, TEXT(", "))
        : FString::Printf(TEXT("%s, ... and %d more"),
            *FString::Join(TArrayView<const FString>(Bones).Left(MaxReportedBones), TEXT(", ")),
            Bones.Num() - MaxReportedBones);
}

FString ResolvePartLabel(const FMtoUCharacterPartResolution& Part, int32 PartNumber)
{
    if (!Part.Name.IsEmpty())
    {
        return Part.Name;
    }
    return Part.Mesh
        ? Part.Mesh->GetName()
        : FString::Printf(TEXT("Additional Part %d"), PartNumber);
}

TSet<int32> DeformationBones(const USkeletalMesh& Mesh)
{
    const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
    TSet<int32> Bones;
    const FSkeletalMeshRenderData* Data = Mesh.GetResourceForRendering();
    if (Data)
    {
        // Section palettes cover skinning influences, including all LODs.
        // Ancestors matter even when they carry no vertex weights themselves.
        for (const FSkeletalMeshLODRenderData& LOD : Data->LODRenderData)
        {
            for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
            {
                for (const FBoneIndexType Bone : Section.BoneMap)
                {
                    for (int32 Index = Bone; Index != INDEX_NONE
                         && Index < Skeleton.GetNum(); Index = Skeleton.GetParentIndex(Index))
                    {
                        Bones.Add(Index);
                    }
                }
            }
        }
    }
    // Without usable skinning evidence, retain the conservative old check.
    if (Bones.IsEmpty())
    {
        for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index) { Bones.Add(Index); }
    }
    return Bones;
}

/**
 * The one compatibility rule for an enabled Additional Part. Returns true when
 * the part may join the character; otherwise OutProblem names the offending
 * part-level reason, and bOutSkeletonProblem records whether the reason is a
 * skeleton, bone-mapping, or reference-pose conflict.
 */
bool ValidatePartAgainstPrimary(
    const USkeletalMesh& Primary,
    const USkeletalMesh& Part,
    FString& OutProblem,
    bool& bOutSkeletonProblem)
{
    OutProblem.Reset();
    bOutSkeletonProblem = false;

    const FReferenceSkeleton& PrimarySkeleton = Primary.GetRefSkeleton();
    const FReferenceSkeleton& PartSkeleton = Part.GetRefSkeleton();
    if (PartSkeleton.GetNum() == 0)
    {
        OutProblem = TEXT("has an empty skeleton.");
        bOutSkeletonProblem = true;
        return false;
    }
    if (Part.GetSkeleton() != Primary.GetSkeleton())
    {
        OutProblem = FString::Printf(
            TEXT("uses Skeleton asset '%s' instead of the Primary Driver Skeleton asset '%s'."),
            Part.GetSkeleton() ? *Part.GetSkeleton()->GetName() : TEXT("<none>"),
            Primary.GetSkeleton() ? *Primary.GetSkeleton()->GetName() : TEXT("<none>"));
        bOutSkeletonProblem = true;
        return false;
    }

    TArray<FTransform> PrimaryComponentPose;
    ComponentSpacePoses(PrimarySkeleton, PrimaryComponentPose);
    TArray<FTransform> PartComponentPose;
    ComponentSpacePoses(PartSkeleton, PartComponentPose);

    double Extent = 1.0;
    for (const FTransform& Pose : PrimaryComponentPose)
    {
        Extent = FMath::Max(Extent, Pose.GetTranslation().Size());
    }
    const double TranslationTolerance = Extent * ReferencePoseRelativeTolerance;

    TArray<FString> ExtraBones;
    TArray<FString> ParentMismatches;
    TArray<FString> PoseConflicts;
    const TSet<int32> PartDeformationBones = DeformationBones(Part);
    for (int32 Index = 0; Index < PartSkeleton.GetNum(); ++Index)
    {
        const FName BoneName = PartSkeleton.GetBoneName(Index);
        const int32 PrimaryIndex = PrimarySkeleton.FindBoneIndex(BoneName);
        if (PrimaryIndex == INDEX_NONE)
        {
            ExtraBones.Add(BoneName.ToString());
            continue;
        }
        const int32 PartParent = PartSkeleton.GetParentIndex(Index);
        const int32 PrimaryParent = PrimarySkeleton.GetParentIndex(PrimaryIndex);
        const bool bSameParent = (PartParent == INDEX_NONE && PrimaryParent == INDEX_NONE)
            || (PartParent != INDEX_NONE && PrimaryParent != INDEX_NONE
                && PartSkeleton.GetBoneName(PartParent) == PrimarySkeleton.GetBoneName(PrimaryParent));
        if (!bSameParent)
        {
            ParentMismatches.Add(FString::Printf(TEXT("%s (parent %s instead of %s)"),
                *BoneName.ToString(),
                *ParentName(PartSkeleton, PartParent),
                *ParentName(PrimarySkeleton, PrimaryParent)));
            continue;
        }

        // Exported complete hierarchies also carry unrelated garment/hair
        // branches. Their bind poses cannot deform this part and must not
        // prevent its display. Name/parent validation above remains strict.
        if (!PartDeformationBones.Contains(Index)) { continue; }
        const FTransform& PartPose = PartComponentPose[Index];
        const FTransform& PrimaryPose = PrimaryComponentPose[PrimaryIndex];
        const double TranslationDelta =
            (PartPose.GetTranslation() - PrimaryPose.GetTranslation()).Size();
        const double RotationDeltaDegrees = FMath::RadiansToDegrees(
            PartPose.GetRotation().AngularDistance(PrimaryPose.GetRotation()));
        const FVector PartScale = PartPose.GetScale3D();
        const FVector PrimaryScale = PrimaryPose.GetScale3D();
        const double ScaleDelta = (PartScale - PrimaryScale).GetAbsMax()
            / FMath::Max(1.0, PrimaryScale.GetAbsMax());
        if (TranslationDelta <= TranslationTolerance
            && RotationDeltaDegrees <= ReferencePoseRotationToleranceDegrees
            && ScaleDelta <= ReferencePoseRelativeTolerance)
        {
            continue;
        }
        PoseConflicts.Add(FString::Printf(
            TEXT("%s (translation %.3f, rotation %.2f deg, scale %.4f)"),
            *BoneName.ToString(), TranslationDelta, RotationDeltaDegrees, ScaleDelta));
    }

    TArray<FString> Reasons;
    if (!ExtraBones.IsEmpty())
    {
        Reasons.Add(FString::Printf(
            TEXT("adds bones that are absent from the Primary Driver Skeletal Mesh: %s"),
            *JoinBones(ExtraBones)));
    }
    if (!ParentMismatches.IsEmpty())
    {
        Reasons.Add(FString::Printf(
            TEXT("maps bones under a different parent than the Primary Driver: %s"),
            *JoinBones(ParentMismatches)));
    }
    if (!PoseConflicts.IsEmpty())
    {
        Reasons.Add(FString::Printf(
            TEXT("reference pose or import space differs from the Primary Driver: %s"),
            *JoinBones(PoseConflicts)));
    }
    if (Reasons.IsEmpty())
    {
        return true;
    }
    OutProblem = FString::Join(Reasons, TEXT("; "));
    bOutSkeletonProblem = true;
    return false;
}
}

void FMtoUCharacterComposition::AppendMorphNames(TArray<FName>& InOut) const
{
    for (const FMtoUCharacterPartResolution& Part : Parts)
    {
        // Disabled parts are absent from the character, and an incompatible
        // part never reaches a streamed character at all.
        if (!Part.Mesh || !Part.Problem.IsEmpty() || (!Part.bPrimary && !Part.bEnabled))
        {
            continue;
        }
        for (const TObjectPtr<UMorphTarget>& MorphTarget : Part.Mesh->GetMorphTargets())
        {
            if (MorphTarget)
            {
                InOut.AddUnique(MorphTarget->GetFName());
            }
        }
    }
}

bool FMtoUCharacterComposition::HasPrimaryDriver(const UMtoULiveLinkBinding* Binding)
{
    return Binding && Binding->SkeletalMesh != nullptr;
}

FMtoUCharacterComposition FMtoUCharacterComposition::Resolve(const UMtoULiveLinkBinding* Binding)
{
    FMtoUCharacterComposition Composition;

    FMtoUCharacterPartResolution PrimaryPart;
    PrimaryPart.bPrimary = true;
    PrimaryPart.bEnabled = true;
    PrimaryPart.Mesh = Binding ? Binding->SkeletalMesh : nullptr;
    PrimaryPart.Name = PrimaryPart.Mesh ? PrimaryPart.Mesh->GetName() : FString();
    if (!PrimaryPart.Mesh)
    {
        PrimaryPart.Problem = TEXT("the Binding has no Primary Driver Skeletal Mesh.");
    }
    Composition.Parts.Add(MoveTemp(PrimaryPart));

    int32 PartNumber = 0;
    if (Binding)
    {
        for (const FMtoUCharacterPart& Part : Binding->AdditionalParts)
        {
            ++PartNumber;
            FMtoUCharacterPartResolution Resolved;
            Resolved.PartId = Part.PartId;
            Resolved.Mesh = Part.SkeletalMesh;
            Resolved.bEnabled = Part.bEnabled;
            Resolved.Name = !Part.PartName.IsEmpty()
                ? Part.PartName
                : (Part.SkeletalMesh ? Part.SkeletalMesh->GetName() : FString());
            if (Part.PartId.IsValid() && Part.bEnabled
                && Part.SkeletalMesh && Part.SkeletalMesh == Composition.Parts[0].Mesh)
            {
                Resolved.Problem = TEXT(
                    "is the Primary Driver Skeletal Mesh; remove the part or assign a different Skeletal Mesh.");
            }
            else if (Part.bEnabled && !Part.SkeletalMesh)
            {
                Resolved.Problem = TEXT("is enabled but has no Skeletal Mesh.");
            }
            Composition.Parts.Add(MoveTemp(Resolved));
        }
    }

    const FMtoUCharacterPartResolution& Primary = Composition.Parts[0];
    const bool bCharacterPresent = Primary.Mesh != nullptr;
    bool bUsable = Primary.Problem.IsEmpty();
    bool bSkeletonProblem = false;
    TArray<FString> Diagnostics;
    TArray<FString> Summary;
    Summary.Add(FString::Printf(TEXT("Primary: %s"),
        Primary.Mesh ? *Primary.Mesh->GetName() : TEXT("<none>")));
    if (!Primary.Problem.IsEmpty())
    {
        Diagnostics.Add(FString::Printf(
            TEXT("The character has no usable Primary Driver Skeletal Mesh: %s"),
            *Primary.Problem));
    }

    for (int32 Index = 1; Index < Composition.Parts.Num(); ++Index)
    {
        FMtoUCharacterPartResolution& Part = Composition.Parts[Index];
        // A disabled part is not part of the character: it is neither validated
        // nor displayed, so an incomplete part stays harmless while parked.
        if (Part.bEnabled && Part.Problem.IsEmpty() && bUsable)
        {
            bool bPartSkeletonProblem = false;
            if (!Part.PartId.IsValid())
            {
                Part.Problem = TEXT("has no stable identity; re-save the Binding asset.");
            }
            else if (!ValidatePartAgainstPrimary(
                *Primary.Mesh, *Part.Mesh, Part.Problem, bPartSkeletonProblem))
            {
                bSkeletonProblem |= bPartSkeletonProblem;
            }
        }
        if (!Part.Problem.IsEmpty())
        {
            bUsable = false;
            Diagnostics.Add(FString::Printf(
                TEXT("Additional Part '%s': %s"), *ResolvePartLabel(Part, Index), *Part.Problem));
        }
        if (Part.bEnabled && bCharacterPresent)
        {
            Composition.EnabledIdentity.Add({Part.PartId, Part.Mesh});
        }
        Summary.Add(Part.bEnabled
            ? ResolvePartLabel(Part, Index)
            : FString::Printf(TEXT("%s (disabled)"), *ResolvePartLabel(Part, Index)));
    }

    if (!bUsable)
    {
        Composition.EnabledIdentity.Reset();
    }
    Composition.EnabledIdentity.Sort();
    Composition.bUsable = bUsable;
    Composition.Diagnostics = FString::Join(Diagnostics, TEXT("\n"));
    Composition.Summary = FString::Join(Summary, TEXT("\n"));
    Composition.FailureCategory = bUsable
        ? FString()
        : (bSkeletonProblem
            ? FMtoUCompositionFailures::SkeletonMismatch
            : FMtoUCompositionFailures::InvalidBinding);
    return Composition;
}
