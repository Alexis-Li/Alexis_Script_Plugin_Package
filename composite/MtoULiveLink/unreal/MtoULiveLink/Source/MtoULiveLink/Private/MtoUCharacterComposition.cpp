#include "MtoUCharacterComposition.h"

#include "MtoULiveLinkBinding.h"

#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"

namespace
{
// A required bone's reference pose must agree with the pose already owned by
// the character's union closely enough that the streamed union-relative pose
// deforms every mesh identically. The translation and scale tolerance is
// relative to the union's reference-pose extent, so the same rule holds for a
// character rig measured in any unit.
constexpr double ReferencePoseRelativeTolerance = 1.e-3;
constexpr double ReferencePoseRotationToleranceDegrees = 1.0;

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

bool DeformationBones(const USkeletalMesh& Mesh, TSet<int32>& Bones, FString& Problem)
{
    const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
    const FSkeletalMeshRenderData* Data = Mesh.GetResourceForRendering();
    if (!Data || Data->LODRenderData.IsEmpty())
    {
        Problem = TEXT("has no reliable skinning data; rebuild or reimport the Skeletal Mesh.");
        return false;
    }
    for (int32 LODIndex = 0; LODIndex < Data->LODRenderData.Num(); ++LODIndex)
    {
        const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[LODIndex];
        const FSkinWeightVertexBuffer& Weights = LOD.SkinWeightVertexBuffer;
        if (!Weights.GetDataVertexBuffer()->GetWeightData() || LOD.RenderSections.IsEmpty())
        {
            Problem = FString::Printf(TEXT("LOD %d has no readable skin weights; rebuild with CPU skin data available."), LODIndex);
            return false;
        }
        for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
        {
            if (uint64(Section.BaseVertexIndex) + Section.NumVertices > Weights.GetNumVertices())
            {
                Problem = FString::Printf(TEXT("LOD %d has invalid skin vertex ranges."), LODIndex);
                return false;
            }
            for (uint32 Vertex = Section.BaseVertexIndex; Vertex < Section.BaseVertexIndex + Section.NumVertices; ++Vertex)
            {
                bool bWeighted = false;
                for (uint32 Influence = 0; Influence < Weights.GetMaxBoneInfluences(); ++Influence)
                {
                    if (Weights.GetBoneWeight(Vertex, Influence) == 0) { continue; }
                    const uint32 PaletteIndex = Weights.GetBoneIndex(Vertex, Influence);
                    if (!Section.BoneMap.IsValidIndex(PaletteIndex) || Section.BoneMap[PaletteIndex] >= Skeleton.GetNum())
                    {
                        Problem = FString::Printf(TEXT("LOD %d has an invalid positive skin influence."), LODIndex);
                        return false;
                    }
                    bWeighted = true;
                    for (int32 Bone = Section.BoneMap[PaletteIndex]; Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone))
                    {
                        Bones.Add(Bone);
                    }
                }
                if (!bWeighted)
                {
                    Problem = FString::Printf(TEXT("LOD %d has a vertex without positive skin weights."), LODIndex);
                    return false;
                }
            }
        }
    }
    if (Bones.IsEmpty()) { Problem = TEXT("has no positive skin influences."); }
    return !Bones.IsEmpty();
}

bool MergeRequiredBones(FMtoUCharacterComposition& Composition, FMtoUCharacterPartResolution& Part)
{
    TSet<int32> Required;
    if (!DeformationBones(*Part.Mesh, Required, Part.Problem)) { return false; }
    const FReferenceSkeleton& Skeleton = Part.Mesh->GetRefSkeleton();
    TArray<FTransform> Poses;
    ComponentSpacePoses(Skeleton, Poses);
    TArray<FTransform> UnionPoses;
    ComponentSpacePoses(Composition.RequiredSkeleton, UnionPoses);
    double Extent = 1.0;
    for (const FTransform& Pose : UnionPoses) { Extent = FMath::Max(Extent, Pose.GetTranslation().Size()); }
    for (int32 Bone : Required) { Extent = FMath::Max(Extent, Poses[Bone].GetTranslation().Size()); }
    FReferenceSkeletonModifier Modifier(Composition.RequiredSkeleton, nullptr);
    for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
    {
        if (!Required.Contains(Index)) { continue; }
        const FName Name = Skeleton.GetBoneName(Index);
        const int32 Parent = Skeleton.GetParentIndex(Index);
        const int32 UnionParent = Parent == INDEX_NONE ? INDEX_NONE
            : Composition.RequiredSkeleton.FindRawBoneIndex(Skeleton.GetBoneName(Parent));
        const int32 Existing = Composition.RequiredSkeleton.FindRawBoneIndex(Name);
        if (Existing != INDEX_NONE)
        {
            if (Composition.RequiredSkeleton.GetRawParentIndex(Existing) != UnionParent)
            {
                Part.Problem = FString::Printf(TEXT("required bone '%s' has parent '%s', conflicting with %s."),
                    *Name.ToString(), *ParentName(Skeleton, Parent), *Composition.BoneOwners[Existing]);
                return false;
            }
            const FTransform& A = Poses[Index];
            const FTransform& B = UnionPoses[Existing];
            if ((A.GetTranslation() - B.GetTranslation()).Size() > Extent * ReferencePoseRelativeTolerance
                || FMath::RadiansToDegrees(A.GetRotation().AngularDistance(B.GetRotation())) > ReferencePoseRotationToleranceDegrees
                || (A.GetScale3D() - B.GetScale3D()).GetAbsMax() / FMath::Max(1.0, B.GetScale3D().GetAbsMax()) > ReferencePoseRelativeTolerance)
            {
                Part.Problem = FString::Printf(TEXT("required bone '%s' reference pose or import space conflicts with %s."),
                    *Name.ToString(), *Composition.BoneOwners[Existing]);
                return false;
            }
            Composition.BoneOwners[Existing] += TEXT(", ") + Part.Name;
        }
        else
        {
            if (Parent == INDEX_NONE && Composition.RequiredSkeleton.GetRawBoneNum() != 0)
            {
                Part.Problem = FString::Printf(TEXT("required root '%s' differs from the character root."), *Name.ToString());
                return false;
            }
            Modifier.Add(FMeshBoneInfo(Name, Name.ToString(), UnionParent), Skeleton.GetRefBonePose()[Index]);
            UnionPoses.Add(Poses[Index]);
            Composition.BoneOwners.Add(Part.Name);
        }
    }
    return true;
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

    // Canonical processing order makes list reordering inert, including union pose ownership.
    TArray<int32> Order;
    for (int32 Index = 1; Index < Composition.Parts.Num(); ++Index) { Order.Add(Index); }
    Order.Sort([&](int32 A, int32 B) { return Composition.Parts[A].PartId < Composition.Parts[B].PartId; });
    Order.Insert(0, 0);
    bool bSkeletonProblem = false;
    for (int32 Index : Order)
    {
        auto& Part = Composition.Parts[Index];
        if (!Part.bEnabled || !Part.Problem.IsEmpty() || !Composition.Parts[0].Mesh) { continue; }
        if (!Part.bPrimary && !Part.PartId.IsValid())
        {
            Part.Problem = TEXT("has no stable identity; re-save the Binding asset.");
            continue;
        }
        if (!Part.Mesh->GetSkeleton() || Part.Mesh->GetSkeleton() != Composition.Parts[0].Mesh->GetSkeleton())
        {
            Part.Problem = FString::Printf(TEXT("uses Skeleton asset '%s' instead of the Primary Driver Skeleton asset."),
                Part.Mesh->GetSkeleton() ? *Part.Mesh->GetSkeleton()->GetName() : TEXT("<none>"));
            bSkeletonProblem = true;
        }
        else if (!MergeRequiredBones(Composition, Part)) { bSkeletonProblem = true; }
    }
    const FMtoUCharacterPartResolution& Primary = Composition.Parts[0];
    const bool bCharacterPresent = Primary.Mesh != nullptr;
    bool bUsable = Primary.Problem.IsEmpty();
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
