#include "MtoUConnectionNegotiator.h"

#include "Algo/Reverse.h"

namespace
{
/** The `_` separator plus the 32 hexadecimal digits of one import hash suffix. */
constexpr int32 HashSuffixLength = 33;

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

/**
 * True for `<complete Maya short name>_<exactly 32 hexadecimal digits>`, the
 * other rename one Unreal import generates for a duplicated short name. A
 * partial original name, another separator before the digits, and a different
 * hash length never match.
 */
bool IsHashSuffixRename(FName MayaName, FName UnrealName)
{
    const FString MayaText = MayaName.ToString();
    const FString UnrealText = UnrealName.ToString();
    if (UnrealText.Len() != MayaText.Len() + HashSuffixLength
        || !UnrealText.StartsWith(MayaText, ESearchCase::IgnoreCase)
        || UnrealText[MayaText.Len()] != TEXT('_'))
    {
        return false;
    }
    for (int32 Index = MayaText.Len() + 1; Index < UnrealText.Len(); ++Index)
    {
        if (!FChar::IsHexDigit(UnrealText[Index]))
        {
            return false;
        }
    }
    return true;
}

/** Either importer-generated rename form for the complete Maya short name. */
bool IsImportedRename(FName MayaName, FName UnrealName)
{
    return IsNumericSuffixRename(MayaName, UnrealName)
        || IsHashSuffixRename(MayaName, UnrealName);
}

/** Complete published path of one description bone, for example
    "root/joints_grp/spine_04". */
FString BonePath(const TArray<FMtoUDescriptionBone>& Bones, int32 Index)
{
    TArray<FString> Segments;
    for (int32 Current = Index;
        Current != INDEX_NONE && Segments.Num() <= Bones.Num();
        Current = Bones[Current].ParentIndex)
    {
        Segments.Add(Bones[Current].Name.ToString());
    }
    Algo::Reverse(Segments);
    return FString::Join(Segments, TEXT("/"));
}

/** Compact index of Maya sources sharing a published short name. */
struct FMtoUSourceNameInfo
{
    int32 Count = 0;
    int32 LastIndex = INDEX_NONE;
};

/** The Maya source one required target was mapped from during a pass. */
struct FMtoUTargetClaim
{
    int32 MayaIndex = INDEX_NONE;
    /** True when an import rename claimed it instead of its exact name. */
    bool bRename = false;
};

/** Everything one mapping pass produces. */
struct FMtoUMappingPass
{
    TArray<int32> MayaToUnreal;
    TArray<FMtoUTargetClaim> TargetClaims;
    TArray<bool> bMayaMappingFailed;
    TSet<int32> SkippedMaya;
    TSet<int32> UsedUnreal;
    TSet<int32> AccountedUnreal;
    TArray<int32> TargetBoneIndices;
    TArray<FString> MissingBones;
    TArray<FString> ExtraBones;
    TArray<FString> UnreachedUnrealBones;
    TArray<FString> ParentMismatches;
    TArray<FString> MappingAmbiguities;
    TArray<FString> DescendantsBlockedByParent;
    TArray<FString> BoneNameMappings;
    int32 FirstFailureIndex = INDEX_NONE;
    int32 FirstFailureUnrealParent = INDEX_NONE;
    FString FirstFailureReason;
};

/**
 * Maps Maya bones parent-first. In a subset, exact-name siblings reserve their
 * targets before import renames are considered. Identical siblings cannot be
 * split across a plain target and an available suffixed target.
 */
FMtoUMappingPass RunMappingPass(
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target,
    const TMap<FName, FMtoUSourceNameInfo>& SourceNames,
    const TArray<int32>& PreviousWithName)
{
    FMtoUMappingPass Pass;
    Pass.MayaToUnreal.Init(INDEX_NONE, Character.Bones.Num());
    Pass.bMayaMappingFailed.Init(false, Character.Bones.Num());
    Pass.TargetBoneIndices.Init(INDEX_NONE, Character.Bones.Num());
    Pass.TargetClaims.SetNum(Target.Bones.Num());

    for (int32 MayaIndex = 0; MayaIndex < Character.Bones.Num(); ++MayaIndex)
    {
        const FMtoUDescriptionBone& MayaBone = Character.Bones[MayaIndex];
        if (Pass.SkippedMaya.Contains(MayaBone.ParentIndex))
        {
            Pass.SkippedMaya.Add(MayaIndex);
            continue;
        }
        if (MayaBone.ParentIndex != INDEX_NONE && Pass.bMayaMappingFailed[MayaBone.ParentIndex])
        {
            Pass.bMayaMappingFailed[MayaIndex] = true;
            Pass.DescendantsBlockedByParent.Add(FString::Printf(
                TEXT("%s: parent %s is not mapped"),
                *MayaBone.Name.ToString(),
                *Character.Bones[MayaBone.ParentIndex].Name.ToString()));
            continue;
        }

        const int32 ExpectedUnrealParent = MayaBone.ParentIndex == INDEX_NONE
            ? INDEX_NONE
            : Pass.MayaToUnreal[MayaBone.ParentIndex];
        TArray<int32> ExactCandidates;
        TArray<int32> RenameCandidates;
        TArray<int32> WrongParentExact;
        for (int32 UnrealIndex = 0; UnrealIndex < Target.Bones.Num(); ++UnrealIndex)
        {
            if (Pass.UsedUnreal.Contains(UnrealIndex))
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
                && SourceNames.FindChecked(MayaBone.Name).Count > 1
                && IsImportedRename(MayaBone.Name, UnrealBone.Name))
            {
                // Reserve exact targets within this Maya parent's scope even
                // if the exact source appears later in the capture.
                bool bReservedByExactSource = false;
                if (Target.bAllowUnusedSourceBones)
                {
                    if (const FMtoUSourceNameInfo* Exact = SourceNames.Find(UnrealBone.Name))
                    {
                        for (int32 SourceIndex = Exact->LastIndex;
                            SourceIndex != INDEX_NONE;
                            SourceIndex = PreviousWithName[SourceIndex])
                        {
                            if (Character.Bones[SourceIndex].ParentIndex == MayaBone.ParentIndex)
                            {
                                bReservedByExactSource = true;
                                break;
                            }
                        }
                    }
                }
                if (!bReservedByExactSource)
                {
                    RenameCandidates.Add(UnrealIndex);
                }
            }
        }

        bool bDuplicateExactSource = false;
        if (Target.bAllowUnusedSourceBones && !ExactCandidates.IsEmpty()
            && SourceNames.FindChecked(MayaBone.Name).Count > 1)
        {
            int32 FirstSibling = MayaIndex;
            int32 SiblingCount = 0;
            for (int32 SourceIndex = SourceNames.FindChecked(MayaBone.Name).LastIndex;
                SourceIndex != INDEX_NONE;
                SourceIndex = PreviousWithName[SourceIndex])
            {
                if (Character.Bones[SourceIndex].ParentIndex == MayaBone.ParentIndex)
                {
                    ++SiblingCount;
                    FirstSibling = FMath::Min(FirstSibling, SourceIndex);
                }
            }
            bDuplicateExactSource = SiblingCount > 1;
            if (bDuplicateExactSource)
            {
                // Neither source can own the exact target; the other must not
                // silently become an import rename just because one is free.
                Pass.AccountedUnreal.Append(ExactCandidates);
                if (MayaIndex == FirstSibling)
                {
                    TArray<FString> Paths;
                    Paths.Reserve(SiblingCount);
                    for (int32 SourceIndex = SourceNames.FindChecked(MayaBone.Name).LastIndex;
                        SourceIndex != INDEX_NONE;
                        SourceIndex = PreviousWithName[SourceIndex])
                    {
                        if (Character.Bones[SourceIndex].ParentIndex == MayaBone.ParentIndex)
                        {
                            Paths.Add(BonePath(Character.Bones, SourceIndex));
                        }
                    }
                    Paths.Sort();
                    for (int32 UnrealIndex : ExactCandidates)
                    {
                        Pass.MappingAmbiguities.Add(FString::Printf(
                            TEXT("'%s' below %s is claimed by %d Maya bones: %s"),
                            *Target.Bones[UnrealIndex].Name.ToString(),
                            *ParentName(Target.Bones, ExpectedUnrealParent),
                            SiblingCount,
                            *FString::Join(Paths, TEXT(", "))));
                    }
                }
            }
        }

        const TArray<int32>& Candidates = ExactCandidates.IsEmpty()
            ? RenameCandidates
            : ExactCandidates;
        if (Candidates.Num() == 1 && !bDuplicateExactSource)
        {
            const int32 UnrealIndex = Candidates[0];
            Pass.MayaToUnreal[MayaIndex] = UnrealIndex;
            Pass.UsedUnreal.Add(UnrealIndex);
            Pass.AccountedUnreal.Add(UnrealIndex);
            Pass.TargetBoneIndices[MayaIndex] = UnrealIndex;
            Pass.TargetClaims[UnrealIndex].MayaIndex = MayaIndex;
            Pass.TargetClaims[UnrealIndex].bRename = Target.Bones[UnrealIndex].Name != MayaBone.Name;
            if (Target.Bones[UnrealIndex].Name != MayaBone.Name)
            {
                Pass.BoneNameMappings.Add(FString::Printf(
                    TEXT("%s/%s -> %s"),
                    *ParentName(Character.Bones, MayaBone.ParentIndex),
                    *MayaBone.Name.ToString(),
                    *Target.Bones[UnrealIndex].Name.ToString()));
            }
            continue;
        }

        // An occupied rename target still counts as competing evidence, not
        // permission to skip a source that could equally drive it.
        TArray<FString> CompetingSources;
        if (Target.bAllowUnusedSourceBones && !bDuplicateExactSource
            && ExactCandidates.IsEmpty())
        {
            for (int32 UnrealIndex = 0; UnrealIndex < Target.Bones.Num(); ++UnrealIndex)
            {
                const FMtoUTargetClaim& Claim = Pass.TargetClaims[UnrealIndex];
                if (Claim.MayaIndex == INDEX_NONE
                    || Target.Bones[UnrealIndex].ParentIndex != ExpectedUnrealParent)
                {
                    continue;
                }
                const bool bExact = Target.Bones[UnrealIndex].Name == MayaBone.Name;
                const bool bRename = MayaBone.ParentIndex != INDEX_NONE
                    && SourceNames.FindChecked(MayaBone.Name).Count > 1
                    && IsImportedRename(MayaBone.Name, Target.Bones[UnrealIndex].Name);
                if (!bExact && !bRename)
                {
                    continue;
                }
                if (bRename && !Claim.bRename)
                {
                    // The exact source owns the target; this duplicate is an
                    // unused branch like any other bone nothing requires.
                    continue;
                }
                TArray<FString> Sources = {
                    BonePath(Character.Bones, Claim.MayaIndex),
                    BonePath(Character.Bones, MayaIndex)};
                Sources.Sort();
                CompetingSources.Add(FString::Printf(TEXT("'%s' below %s is claimed by %d Maya bones: %s"),
                    *Target.Bones[UnrealIndex].Name.ToString(),
                    *ParentName(Target.Bones, Target.Bones[UnrealIndex].ParentIndex),
                    Sources.Num(),
                    *FString::Join(Sources, TEXT(", "))));
            }
        }
        if (CompetingSources.IsEmpty()
            && Target.bAllowUnusedSourceBones && Candidates.IsEmpty())
        {
            // A duplicate on an unused branch must not steal or reject a
            // required name that a later Maya branch can map correctly.
            for (int32 WrongParent : WrongParentExact)
            {
                Pass.ParentMismatches.Add(FString::Printf(TEXT("%s: Maya=%s, Unreal=%s"),
                    *BonePath(Character.Bones, MayaIndex),
                    *ParentName(Character.Bones, MayaBone.ParentIndex),
                    *ParentName(Target.Bones, Target.Bones[WrongParent].ParentIndex)));
            }
            Pass.SkippedMaya.Add(MayaIndex);
            continue;
        }
        Pass.bMayaMappingFailed[MayaIndex] = true;
        FString FailureReason;
        if (bDuplicateExactSource)
        {
            FailureReason = TEXT("a required target has indistinguishable Maya siblings");
        }
        else if (!CompetingSources.IsEmpty())
        {
            CompetingSources.Sort();
            Pass.MappingAmbiguities.Append(CompetingSources);
            FailureReason = TEXT("a required target has competing Maya sources");
        }
        else if (Candidates.Num() > 1)
        {
            FailureReason = FString::Printf(
                TEXT("%d Unreal bones match under the expected parent"), Candidates.Num());
            Pass.MappingAmbiguities.Add(FString::Printf(
                TEXT("%s below %s has %d candidates"),
                *MayaBone.Name.ToString(),
                *ParentName(Character.Bones, MayaBone.ParentIndex),
                Candidates.Num()));
        }
        else if (WrongParentExact.Num() == 1)
        {
            const int32 UnrealIndex = WrongParentExact[0];
            Pass.AccountedUnreal.Add(UnrealIndex);
            const FString UnrealParent = ParentName(Target.Bones, Target.Bones[UnrealIndex].ParentIndex);
            FailureReason = FString::Printf(
                TEXT("the only Unreal bone with this name sits below %s"), *UnrealParent);
            Pass.ParentMismatches.Add(FString::Printf(
                TEXT("%s: Maya=%s, Unreal=%s"),
                *MayaBone.Name.ToString(),
                *ParentName(Character.Bones, MayaBone.ParentIndex),
                *UnrealParent));
        }
        else if (WrongParentExact.Num() > 1)
        {
            FailureReason = FString::Printf(
                TEXT("%d Unreal bones with this name sit under different parents"),
                WrongParentExact.Num());
            Pass.MappingAmbiguities.Add(FString::Printf(
                TEXT("%s has %d candidates under different Unreal parents"),
                *MayaBone.Name.ToString(),
                WrongParentExact.Num()));
            Pass.AccountedUnreal.Append(WrongParentExact);
        }
        else
        {
            FailureReason = TEXT("no Unreal bone matches under the expected parent");
            Pass.MissingBones.Add(MayaBone.Name.ToString());
        }
        if (Pass.FirstFailureIndex == INDEX_NONE)
        {
            Pass.FirstFailureIndex = MayaIndex;
            Pass.FirstFailureUnrealParent = ExpectedUnrealParent;
            Pass.FirstFailureReason = FailureReason;
        }
    }
    return Pass;
}
}

FString FMtoUNegotiationOutcome::TechnicalDetails() const
{
    FString Details;
    if (FirstFailure.IsSet())
    {
        const FMtoUNegotiationFailure& Failure = FirstFailure.GetValue();
        Details += FString::Printf(
            TEXT("Root cause: %s is not mapped\n- Reason: %s\n- Expected parent: %s"),
            *Failure.MayaPath,
            *Failure.Reason,
            *Failure.ExpectedParent);
        if (Failure.BlockedDescendants > 0)
        {
            Details += FString::Printf(
                TEXT("\n- Blocked Maya descendants: %d"), Failure.BlockedDescendants);
        }
        if (Failure.UnreachedUnreal > 0)
        {
            Details += FString::Printf(
                TEXT("\n- Unreached Unreal bones: %d"), Failure.UnreachedUnreal);
        }
        for (const FString& Hint : Failure.RenameHints)
        {
            Details += FString::Printf(TEXT("\n- Possible import rename: %s"), *Hint);
        }
    }
    AppendSection(Details, TEXT("Missing in Unreal"), MissingBones);
    AppendSection(Details, TEXT("Extra in Unreal"), ExtraBones);
    AppendSection(Details, TEXT("Unreached in Unreal"), UnreachedUnrealBones);
    AppendSection(Details, TEXT("Parent mismatches"), ParentMismatches);
    AppendSection(Details, TEXT("Mapping ambiguities"), MappingAmbiguities);
    AppendSection(Details, TEXT("Descendants blocked by parent"), DescendantsBlockedByParent);
    return Details;
}

FMtoUNegotiationOutcome FMtoUConnectionNegotiator::Negotiate(
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target)
{
    TMap<FName, FMtoUSourceNameInfo> SourceNames;
    TArray<int32> PreviousWithName;
    PreviousWithName.Init(INDEX_NONE, Character.Bones.Num());
    for (int32 Index = 0; Index < Character.Bones.Num(); ++Index)
    {
        FMtoUSourceNameInfo& Name = SourceNames.FindOrAdd(Character.Bones[Index].Name);
        PreviousWithName[Index] = Name.LastIndex;
        Name.LastIndex = Index;
        ++Name.Count;
    }
    FMtoUMappingPass Pass = RunMappingPass(Character, Target, SourceNames, PreviousWithName);

    FMtoUNegotiationOutcome Outcome;
    Outcome.TargetBoneIndices = MoveTemp(Pass.TargetBoneIndices);
    Outcome.BoneNameMappings = MoveTemp(Pass.BoneNameMappings);
    Outcome.MissingBones = MoveTemp(Pass.MissingBones);
    Outcome.ParentMismatches = MoveTemp(Pass.ParentMismatches);
    Outcome.MappingAmbiguities = MoveTemp(Pass.MappingAmbiguities);
    Outcome.DescendantsBlockedByParent = MoveTemp(Pass.DescendantsBlockedByParent);

    // A bone is only ever considered while its parent is already mapped, so an
    // unmatched bone whose parent was never mapped was never examined. Reporting
    // it as extra would claim Maya lacks a bone that only sits behind the first
    // failure; only an unmatched bone below a successfully mapped parent is
    // confirmed extra. A parent that is accounted but unmapped - a parent
    // mismatch or an ambiguity - does not make its children extra either, so
    // this test uses the mapped set rather than every explained Unreal bone.
    for (int32 UnrealIndex = 0; UnrealIndex < Target.Bones.Num(); ++UnrealIndex)
    {
        if (Pass.AccountedUnreal.Contains(UnrealIndex))
        {
            continue;
        }
        const int32 ParentIndex = Target.Bones[UnrealIndex].ParentIndex;
        if (ParentIndex != INDEX_NONE && !Pass.UsedUnreal.Contains(ParentIndex))
        {
            Outcome.UnreachedUnrealBones.Add(Target.Bones[UnrealIndex].Name.ToString()
                + (Target.BoneOwners.IsValidIndex(UnrealIndex) ? TEXT(" (required by ") + Target.BoneOwners[UnrealIndex] + TEXT(")") : FString()));
        }
        else
        {
            Outcome.ExtraBones.Add(Target.Bones[UnrealIndex].Name.ToString()
                + (Target.BoneOwners.IsValidIndex(UnrealIndex) ? TEXT(" (required by ") + Target.BoneOwners[UnrealIndex] + TEXT(")") : FString()));
        }
    }

    if (Pass.FirstFailureIndex != INDEX_NONE)
    {
        FMtoUNegotiationFailure Failure;
        Failure.MayaPath = BonePath(Character.Bones, Pass.FirstFailureIndex);
        Failure.ExpectedParent = ParentName(
            Character.Bones, Character.Bones[Pass.FirstFailureIndex].ParentIndex);
        Failure.Reason = Pass.FirstFailureReason;
        Failure.UnreachedUnreal = Outcome.UnreachedUnrealBones.Num();
        for (int32 Index = 0; Index < Character.Bones.Num(); ++Index)
        {
            if (Index == Pass.FirstFailureIndex || !Pass.bMayaMappingFailed[Index])
            {
                continue;
            }
            for (int32 Parent = Character.Bones[Index].ParentIndex;
                Parent != INDEX_NONE;
                Parent = Character.Bones[Parent].ParentIndex)
            {
                if (Parent == Pass.FirstFailureIndex)
                {
                    ++Failure.BlockedDescendants;
                    break;
                }
            }
        }
        // Diagnostic hints only, collected after every mapping decision so they
        // never name a bone this negotiation did use: they list the Unreal names
        // shaped like an import rename of the failed bone and the reason the
        // strict candidate rules did not apply them.
        const FName FailedName = Character.Bones[Pass.FirstFailureIndex].Name;
        const bool bUniqueMayaName = SourceNames.FindChecked(FailedName).Count <= 1;
        for (int32 UnrealIndex = 0; UnrealIndex < Target.Bones.Num(); ++UnrealIndex)
        {
            if (Pass.UsedUnreal.Contains(UnrealIndex) || Pass.AccountedUnreal.Contains(UnrealIndex))
            {
                continue;
            }
            const FMtoUDescriptionBone& UnrealBone = Target.Bones[UnrealIndex];
            if (!IsImportedRename(FailedName, UnrealBone.Name))
            {
                continue;
            }
            if (Character.Bones[Pass.FirstFailureIndex].ParentIndex == INDEX_NONE)
            {
                Failure.RenameHints.Add(FString::Printf(
                    TEXT("%s (a root bone has no mapped parent to scope the rename)"),
                    *UnrealBone.Name.ToString()));
            }
            else if (UnrealBone.ParentIndex != Pass.FirstFailureUnrealParent)
            {
                Failure.RenameHints.Add(FString::Printf(
                    TEXT("%s (parent %s does not match %s)"),
                    *UnrealBone.Name.ToString(),
                    *ParentName(Target.Bones, UnrealBone.ParentIndex),
                    *Failure.ExpectedParent));
            }
            else if (bUniqueMayaName)
            {
                Failure.RenameHints.Add(FString::Printf(
                    TEXT("%s (its Maya short name is not duplicated in this skeleton)"),
                    *UnrealBone.Name.ToString()));
            }
            else
            {
                Failure.RenameHints.Add(UnrealBone.Name.ToString());
            }
        }
        Failure.RenameHints.Sort();
        Outcome.FirstFailure = MoveTemp(Failure);
    }

    if (Target.bAllowUnusedSourceBones && Pass.UsedUnreal.Num() == Target.Bones.Num()
        && Outcome.MappingAmbiguities.IsEmpty())
    {
        Outcome.ParentMismatches.Reset();
    }
    Outcome.MissingBones.Sort();
    Outcome.ExtraBones.Sort();
    Outcome.UnreachedUnrealBones.Sort();
    Outcome.ParentMismatches.Sort();
    Outcome.MappingAmbiguities.Sort();
    Outcome.DescendantsBlockedByParent.Sort();
    Outcome.BoneNameMappings.Sort();
    Outcome.bUsable = Outcome.MissingBones.IsEmpty()
        && Outcome.ExtraBones.IsEmpty()
        && Outcome.UnreachedUnrealBones.IsEmpty()
        && Outcome.ParentMismatches.IsEmpty()
        && Outcome.MappingAmbiguities.IsEmpty()
        && Outcome.DescendantsBlockedByParent.IsEmpty()
        && (Target.bAllowUnusedSourceBones || Pass.UsedUnreal.Num() == Character.Bones.Num())
        && Pass.UsedUnreal.Num() == Target.Bones.Num();
    if (!Outcome.bUsable)
    {
        Outcome.FailureCategory = TEXT("SKELETON_MISMATCH");
        Outcome.TargetBoneIndices.Reset();
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
