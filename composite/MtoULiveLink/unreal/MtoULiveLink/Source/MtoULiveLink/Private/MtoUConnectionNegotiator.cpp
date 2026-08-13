#include "MtoUConnectionNegotiator.h"

namespace
{
void AppendSection(FString& Details, const TCHAR* Heading, const TArray<FString>& Values)
{
    if (Values.IsEmpty())
    {
        return;
    }
    if (!Details.IsEmpty())
    {
        Details += TEXT("\n");
    }
    Details += Heading;
    Details += TEXT(":\n- ");
    Details += FString::Join(Values, TEXT("\n- "));
}

bool IsNumericSuffixRename(FName MayaName, FName UnrealName)
{
    const FString MayaText = MayaName.ToString();
    const FString UnrealText = UnrealName.ToString();
    if (!UnrealText.StartsWith(MayaText, ESearchCase::IgnoreCase)
        || UnrealText.Len() <= MayaText.Len())
    {
        return false;
    }
    for (int32 Index = MayaText.Len(); Index < UnrealText.Len(); ++Index)
    {
        if (!FChar::IsDigit(UnrealText[Index]))
        {
            return false;
        }
    }
    return true;
}

FString ParentName(const TArray<FMtoUDescriptionBone>& Bones, int32 Index)
{
    return Index == INDEX_NONE ? TEXT("<root>") : Bones[Index].Name.ToString();
}
}

FString FMtoUNegotiationOutcome::TechnicalDetails() const
{
    FString Details;
    AppendSection(Details, TEXT("Missing in Unreal"), MissingBones);
    AppendSection(Details, TEXT("Extra in Unreal"), ExtraBones);
    AppendSection(Details, TEXT("Parent mismatches"), ParentMismatches);
    AppendSection(Details, TEXT("Mapping ambiguities"), MappingAmbiguities);
    AppendSection(Details, TEXT("Descendants blocked by parent"), DescendantsBlockedByParent);
    return Details;
}

FMtoUNegotiationOutcome FMtoUConnectionNegotiator::Negotiate(
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target)
{
    FMtoUNegotiationOutcome Outcome;
    Outcome.PublishBoneNames.SetNum(Character.Bones.Num());

    TMap<FName, int32> MayaNameCounts;
    for (const FMtoUDescriptionBone& Bone : Character.Bones)
    {
        MayaNameCounts.FindOrAdd(Bone.Name)++;
    }

    TArray<int32> MayaToUnreal;
    MayaToUnreal.Init(INDEX_NONE, Character.Bones.Num());
    TArray<bool> bMayaMappingFailed;
    bMayaMappingFailed.Init(false, Character.Bones.Num());
    TSet<int32> UsedUnreal;
    TSet<int32> AccountedUnreal;

    for (int32 MayaIndex = 0; MayaIndex < Character.Bones.Num(); ++MayaIndex)
    {
        const FMtoUDescriptionBone& MayaBone = Character.Bones[MayaIndex];
        if (MayaBone.ParentIndex != INDEX_NONE && bMayaMappingFailed[MayaBone.ParentIndex])
        {
            bMayaMappingFailed[MayaIndex] = true;
            Outcome.DescendantsBlockedByParent.Add(FString::Printf(
                TEXT("%s: parent %s is not mapped"),
                *MayaBone.Name.ToString(),
                *Character.Bones[MayaBone.ParentIndex].Name.ToString()));
            continue;
        }

        const int32 ExpectedUnrealParent = MayaBone.ParentIndex == INDEX_NONE
            ? INDEX_NONE
            : MayaToUnreal[MayaBone.ParentIndex];
        TArray<int32> ExactCandidates;
        TArray<int32> SuffixCandidates;
        TArray<int32> WrongParentExact;
        for (int32 UnrealIndex = 0; UnrealIndex < Target.Bones.Num(); ++UnrealIndex)
        {
            if (UsedUnreal.Contains(UnrealIndex))
            {
                continue;
            }
            const FMtoUDescriptionBone& UnrealBone = Target.Bones[UnrealIndex];
            if (UnrealBone.Name == MayaBone.Name)
            {
                if (UnrealBone.ParentIndex == ExpectedUnrealParent)
                {
                    ExactCandidates.Add(UnrealIndex);
                }
                else
                {
                    WrongParentExact.Add(UnrealIndex);
                }
            }
            else if (MayaBone.ParentIndex != INDEX_NONE
                && UnrealBone.ParentIndex == ExpectedUnrealParent
                && MayaNameCounts[MayaBone.Name] > 1
                && IsNumericSuffixRename(MayaBone.Name, UnrealBone.Name))
            {
                SuffixCandidates.Add(UnrealIndex);
            }
        }

        const TArray<int32>& Candidates = ExactCandidates.IsEmpty()
            ? SuffixCandidates
            : ExactCandidates;
        if (Candidates.Num() == 1)
        {
            const int32 UnrealIndex = Candidates[0];
            MayaToUnreal[MayaIndex] = UnrealIndex;
            UsedUnreal.Add(UnrealIndex);
            AccountedUnreal.Add(UnrealIndex);
            Outcome.PublishBoneNames[MayaIndex] = Target.Bones[UnrealIndex].Name;
            if (Target.Bones[UnrealIndex].Name != MayaBone.Name)
            {
                Outcome.BoneNameMappings.Add(FString::Printf(
                    TEXT("%s/%s -> %s"),
                    *ParentName(Character.Bones, MayaBone.ParentIndex),
                    *MayaBone.Name.ToString(),
                    *Target.Bones[UnrealIndex].Name.ToString()));
            }
            continue;
        }

        bMayaMappingFailed[MayaIndex] = true;
        if (Candidates.Num() > 1)
        {
            Outcome.MappingAmbiguities.Add(FString::Printf(
                TEXT("%s below %s has %d candidates"),
                *MayaBone.Name.ToString(),
                *ParentName(Character.Bones, MayaBone.ParentIndex),
                Candidates.Num()));
        }
        else if (WrongParentExact.Num() == 1)
        {
            const int32 UnrealIndex = WrongParentExact[0];
            AccountedUnreal.Add(UnrealIndex);
            Outcome.ParentMismatches.Add(FString::Printf(
                TEXT("%s: Maya=%s, Unreal=%s"),
                *MayaBone.Name.ToString(),
                *ParentName(Character.Bones, MayaBone.ParentIndex),
                *ParentName(Target.Bones, Target.Bones[UnrealIndex].ParentIndex)));
        }
        else if (WrongParentExact.Num() > 1)
        {
            Outcome.MappingAmbiguities.Add(FString::Printf(
                TEXT("%s has %d candidates under different Unreal parents"),
                *MayaBone.Name.ToString(),
                WrongParentExact.Num()));
            AccountedUnreal.Append(WrongParentExact);
        }
        else
        {
            Outcome.MissingBones.Add(MayaBone.Name.ToString());
        }
    }

    for (int32 UnrealIndex = 0; UnrealIndex < Target.Bones.Num(); ++UnrealIndex)
    {
        if (!AccountedUnreal.Contains(UnrealIndex))
        {
            Outcome.ExtraBones.Add(Target.Bones[UnrealIndex].Name.ToString());
        }
    }

    Outcome.MissingBones.Sort();
    Outcome.ExtraBones.Sort();
    Outcome.ParentMismatches.Sort();
    Outcome.MappingAmbiguities.Sort();
    Outcome.DescendantsBlockedByParent.Sort();
    Outcome.BoneNameMappings.Sort();
    Outcome.bUsable = Outcome.MissingBones.IsEmpty()
        && Outcome.ExtraBones.IsEmpty()
        && Outcome.ParentMismatches.IsEmpty()
        && Outcome.MappingAmbiguities.IsEmpty()
        && Outcome.DescendantsBlockedByParent.IsEmpty()
        && UsedUnreal.Num() == Character.Bones.Num()
        && UsedUnreal.Num() == Target.Bones.Num();
    if (!Outcome.bUsable)
    {
        Outcome.FailureCategory = TEXT("SKELETON_MISMATCH");
        Outcome.PublishBoneNames.Reset();
        Outcome.BoneNameMappings.Reset();
        return Outcome;
    }

    TSet<FName> TargetMorphNames(Target.MorphTargetNames);
    TSet<FName> CharacterCurveNames(Character.CurveNames);
    for (int32 Index = 0; Index < Character.CurveNames.Num(); ++Index)
    {
        const FName Name = Character.CurveNames[Index];
        if (TargetMorphNames.Contains(Name))
        {
            Outcome.AcceptedCurveIndices.Add(Index);
            Outcome.AcceptedCurveNames.Add(Name);
        }
        else
        {
            Outcome.MayaOnlyMorphNames.Add(Name);
        }
    }
    for (const FName Name : TargetMorphNames)
    {
        if (!CharacterCurveNames.Contains(Name))
        {
            Outcome.UnrealOnlyMorphNames.Add(Name);
        }
    }
    Outcome.MayaOnlyMorphNames.Sort(FNameLexicalLess());
    Outcome.UnrealOnlyMorphNames.Sort(FNameLexicalLess());
    return Outcome;
}
