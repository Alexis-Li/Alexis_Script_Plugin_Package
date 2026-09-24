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

/** How many Maya sources carry one published short name. */
struct FMtoUSourceNameInfo
{
    int32 Count = 0;
};

/** One Maya bone's candidate relation inside its mapped parent scope. */
struct FMtoUScopeBone
{
    int32 MayaIndex = INDEX_NONE;
    /** First exact-name target below the mapped parent, when one exists. */
    int32 ExactTarget = INDEX_NONE;
    int32 ExactCount = 0;
    /** Import-rename targets that no exact sibling name reserves. */
    TArray<int32> RenameTargets;
    /** Same-name targets that sit below another Unreal parent. */
    TArray<int32> WrongParentExact;
    /** Set once the bone is mapped, skipped, or refused. */
    bool bResolved = false;
};

/** One mapped parent scope: its children and their complete candidate relation. */
struct FMtoUScope
{
    int32 ExpectedUnrealParent = INDEX_NONE;
    TArray<int32> Siblings;
    TArray<FMtoUScopeBone> Bones;
    /** Every published short name the children carry. */
    TSet<FName> SiblingNames;
    /** How many children carry each short name. */
    TMap<FName, int32> NameCounts;
    /** First child carrying each short name, for one report per conflict. */
    TMap<FName, int32> FirstWithName;
};

/** A Maya bone that could not be mapped from its own candidates. */
struct FMtoUDirectFailure
{
    int32 MayaIndex = INDEX_NONE;
    int32 ExpectedUnrealParent = INDEX_NONE;
    FString Reason;
};

/** Everything one mapping pass produces. */
struct FMtoUMappingPass
{
    TArray<int32> MayaToUnreal;
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
    TArray<FMtoUDirectFailure> DirectFailures;
};

void MapScopeBone(
    FMtoUMappingPass& Pass,
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target,
    int32 MayaIndex,
    int32 UnrealIndex)
{
    Pass.MayaToUnreal[MayaIndex] = UnrealIndex;
    Pass.UsedUnreal.Add(UnrealIndex);
    Pass.AccountedUnreal.Add(UnrealIndex);
    Pass.TargetBoneIndices[MayaIndex] = UnrealIndex;
    if (Target.Bones[UnrealIndex].Name != Character.Bones[MayaIndex].Name)
    {
        Pass.BoneNameMappings.Add(FString::Printf(
            TEXT("%s/%s -> %s"),
            *ParentName(Character.Bones, Character.Bones[MayaIndex].ParentIndex),
            *Character.Bones[MayaIndex].Name.ToString(),
            *Target.Bones[UnrealIndex].Name.ToString()));
    }
}

void FailScopeBone(
    FMtoUMappingPass& Pass,
    int32 MayaIndex,
    int32 ExpectedUnrealParent,
    const FString& Reason)
{
    Pass.bMayaMappingFailed[MayaIndex] = true;
    FMtoUDirectFailure& Failure = Pass.DirectFailures.AddDefaulted_GetRef();
    Failure.MayaIndex = MayaIndex;
    Failure.ExpectedUnrealParent = ExpectedUnrealParent;
    Failure.Reason = Reason;
}

/** Collects the children of one mapped parent and their candidate relation. */
FMtoUScope BuildScope(
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target,
    const TMap<FName, FMtoUSourceNameInfo>& SourceNames,
    const TArray<int32>& Siblings,
    int32 ExpectedUnrealParent,
    const FMtoUMappingPass& Pass)
{
    FMtoUScope Scope;
    Scope.ExpectedUnrealParent = ExpectedUnrealParent;
    Scope.Siblings = Siblings;
    Scope.Bones.SetNum(Siblings.Num());
    for (int32 Slot = 0; Slot < Siblings.Num(); ++Slot)
    {
        const FName Name = Character.Bones[Siblings[Slot]].Name;
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
        const int32 MayaIndex = Siblings[Slot];
        const FMtoUDescriptionBone& MayaBone = Character.Bones[MayaIndex];
        const int32 SourceCount = SourceNames.FindChecked(MayaBone.Name).Count;
        FMtoUScopeBone& Bone = Scope.Bones[Slot];
        Bone.MayaIndex = MayaIndex;
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
                    if (Bone.ExactCount == 0)
                    {
                        Bone.ExactTarget = UnrealIndex;
                    }
                    ++Bone.ExactCount;
                }
                else
                {
                    Bone.WrongParentExact.Add(UnrealIndex);
                }
            }
            else if (MayaBone.ParentIndex != INDEX_NONE
                && UnrealBone.ParentIndex == ExpectedUnrealParent
                && SourceCount > 1
                && IsImportedRename(MayaBone.Name, UnrealBone.Name)
                && !Scope.SiblingNames.Contains(UnrealBone.Name))
            {
                Bone.RenameTargets.Add(UnrealIndex);
            }
        }
    }
    return Scope;
}

/**
 * Exact names claim their targets first. Children sharing one short name are
 * indistinguishable, so none of them may own the target even when a suffixed
 * target is still free.
 */
void ResolveExactNames(
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target,
    FMtoUScope& Scope,
    FMtoUMappingPass& Pass)
{
    for (int32 Slot = 0; Slot < Scope.Bones.Num(); ++Slot)
    {
        FMtoUScopeBone& Bone = Scope.Bones[Slot];
        const FName Name = Character.Bones[Bone.MayaIndex].Name;
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
                for (int32 Other : Scope.Siblings)
                {
                    if (Character.Bones[Other].Name == Name)
                    {
                        Paths.Add(BonePath(Character.Bones, Other));
                    }
                }
                Paths.Sort();
                for (int32 UnrealIndex = 0; UnrealIndex < Target.Bones.Num(); ++UnrealIndex)
                {
                    if (Target.Bones[UnrealIndex].Name == Name
                        && Target.Bones[UnrealIndex].ParentIndex == Scope.ExpectedUnrealParent)
                    {
                        Pass.MappingAmbiguities.Add(FString::Printf(
                            TEXT("'%s' below %s is claimed by %d Maya bones: %s"),
                            *Name.ToString(),
                            *ParentName(Target.Bones, Scope.ExpectedUnrealParent),
                            NameCount,
                            *FString::Join(Paths, TEXT(", "))));
                    }
                }
            }
            for (int32 UnrealIndex = 0; UnrealIndex < Target.Bones.Num(); ++UnrealIndex)
            {
                if (Target.Bones[UnrealIndex].Name == Name
                    && Target.Bones[UnrealIndex].ParentIndex == Scope.ExpectedUnrealParent)
                {
                    Pass.AccountedUnreal.Add(UnrealIndex);
                }
            }
            Bone.bResolved = true;
            FailScopeBone(Pass, Bone.MayaIndex, Scope.ExpectedUnrealParent,
                TEXT("a required target has indistinguishable Maya siblings"));
            continue;
        }
        if (Bone.ExactCount == 1)
        {
            Bone.bResolved = true;
            MapScopeBone(Pass, Character, Target, Bone.MayaIndex, Bone.ExactTarget);
            continue;
        }
        if (Bone.ExactCount > 1)
        {
            Bone.bResolved = true;
            Pass.MappingAmbiguities.Add(FString::Printf(
                TEXT("%s below %s has %d candidates"),
                *Name.ToString(),
                *ParentName(Character.Bones, Character.Bones[Bone.MayaIndex].ParentIndex),
                Bone.ExactCount));
            FailScopeBone(Pass, Bone.MayaIndex, Scope.ExpectedUnrealParent,
                FString::Printf(TEXT("%d Unreal bones match under the expected parent"),
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
    const TArray<int32>& TargetIndices,
    const TArray<int32>& SourceOfTarget,
    const TArray<int32>& TargetOfSource,
    const TMap<int32, int32>& TargetSlotByIndex)
{
    const int32 NodeCount = SourceSlots.Num() + TargetIndices.Num() + 1;
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
    for (int32 TargetSlot = 0; TargetSlot < TargetIndices.Num(); ++TargetSlot)
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
        for (int32 UnrealIndex : Scope.Bones[SourceSlots[SourceSlot]].RenameTargets)
        {
            const int32* TargetSlot = TargetSlotByIndex.Find(UnrealIndex);
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
        for (int32 Next : Edges[Ready[Cursor]])
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
 * an exact sibling name reserves is never a rename target, the required targets
 * are driven only by an assignment that is the only one covering them, and a
 * source that can drive several remaining targets is refused rather than
 * settled by capture order.
 */
void ResolveImportRenames(
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target,
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
    TArray<int32> TargetIndices;
    TMap<int32, int32> TargetSlotByIndex;
    for (int32 UnrealIndex = 0; UnrealIndex < Target.Bones.Num(); ++UnrealIndex)
    {
        if (Target.Bones[UnrealIndex].ParentIndex == Scope.ExpectedUnrealParent
            && !Scope.SiblingNames.Contains(Target.Bones[UnrealIndex].Name)
            && !Pass.UsedUnreal.Contains(UnrealIndex))
        {
            TargetSlotByIndex.Add(UnrealIndex, TargetIndices.Add(UnrealIndex));
        }
    }
    if (SourceSlots.IsEmpty() || TargetIndices.IsEmpty())
    {
        return;
    }

    TArray<TArray<int32>> TargetSources;
    TargetSources.SetNum(TargetIndices.Num());
    TArray<int32> SourceOfTarget;
    SourceOfTarget.Init(INDEX_NONE, TargetIndices.Num());
    TArray<int32> TargetOfSource;
    TargetOfSource.Init(INDEX_NONE, SourceSlots.Num());
    for (int32 SourceSlot = 0; SourceSlot < SourceSlots.Num(); ++SourceSlot)
    {
        for (int32 UnrealIndex : Scope.Bones[SourceSlots[SourceSlot]].RenameTargets)
        {
            if (const int32* TargetSlot = TargetSlotByIndex.Find(UnrealIndex))
            {
                TargetSources[*TargetSlot].Add(SourceSlot);
            }
        }
    }

    // Maximum matching, targets and sources both in capture order.
    auto TryMatch = [&](auto&& Self, int32 TargetSlot, TArray<bool>& Visited) -> bool
    {
        for (int32 SourceSlot : TargetSources[TargetSlot])
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
    for (int32 TargetSlot = 0; TargetSlot < TargetIndices.Num(); ++TargetSlot)
    {
        TArray<bool> Visited;
        Visited.Init(false, SourceSlots.Num());
        TryMatch(TryMatch, TargetSlot, Visited);
    }

    bool bCovered = true;
    for (int32 TargetSlot = 0; TargetSlot < TargetIndices.Num(); ++TargetSlot)
    {
        bCovered &= SourceOfTarget[TargetSlot] != INDEX_NONE;
    }
    if (bCovered
        && !HasCompetingAssignment(Scope, SourceSlots, TargetIndices,
            SourceOfTarget, TargetOfSource, TargetSlotByIndex))
    {
        for (int32 SourceSlot = 0; SourceSlot < SourceSlots.Num(); ++SourceSlot)
        {
            const int32 TargetSlot = TargetOfSource[SourceSlot];
            if (TargetSlot != INDEX_NONE)
            {
                FMtoUScopeBone& Bone = Scope.Bones[SourceSlots[SourceSlot]];
                Bone.bResolved = true;
                MapScopeBone(Pass, Character, Target, Bone.MayaIndex, TargetIndices[TargetSlot]);
            }
        }
        return;
    }

    // Refuse instead of splitting the sources by capture order: report every
    // target two or more sources can drive, then every source that still has a
    // choice between targets. A source with a single, uncontested target is
    // forced and keeps its mapping.
    for (int32 TargetSlot = 0; TargetSlot < TargetIndices.Num(); ++TargetSlot)
    {
        if (TargetSources[TargetSlot].Num() < 2)
        {
            continue;
        }
        TArray<FString> Paths;
        Paths.Reserve(TargetSources[TargetSlot].Num());
        for (int32 SourceSlot : TargetSources[TargetSlot])
        {
            Paths.Add(BonePath(Character.Bones, Scope.Bones[SourceSlots[SourceSlot]].MayaIndex));
        }
        Paths.Sort();
        Pass.MappingAmbiguities.Add(FString::Printf(
            TEXT("'%s' below %s is claimed by %d Maya bones: %s"),
            *Target.Bones[TargetIndices[TargetSlot]].Name.ToString(),
            *ParentName(Target.Bones, Scope.ExpectedUnrealParent),
            Paths.Num(),
            *FString::Join(Paths, TEXT(", "))));
        Pass.AccountedUnreal.Add(TargetIndices[TargetSlot]);
        for (int32 SourceSlot : TargetSources[TargetSlot])
        {
            FMtoUScopeBone& Bone = Scope.Bones[SourceSlots[SourceSlot]];
            if (!Bone.bResolved)
            {
                Bone.bResolved = true;
                FailScopeBone(Pass, Bone.MayaIndex, Scope.ExpectedUnrealParent,
                    TEXT("a required target has competing Maya sources"));
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
        for (int32 UnrealIndex : Bone.RenameTargets)
        {
            if (TargetSlotByIndex.Contains(UnrealIndex))
            {
                Feasible.Add(UnrealIndex);
            }
        }
        Bone.bResolved = true;
        if (Feasible.Num() == 1)
        {
            // No other source can drive this target, so every covering
            // assignment uses the pair.
            MapScopeBone(Pass, Character, Target, Bone.MayaIndex, Feasible[0]);
            continue;
        }
        Pass.MappingAmbiguities.Add(FString::Printf(
            TEXT("%s below %s has %d candidates"),
            *Character.Bones[Bone.MayaIndex].Name.ToString(),
            *ParentName(Character.Bones, Character.Bones[Bone.MayaIndex].ParentIndex),
            Feasible.Num()));
        FailScopeBone(Pass, Bone.MayaIndex, Scope.ExpectedUnrealParent,
            FString::Printf(TEXT("%d Unreal bones match under the expected parent"),
                Feasible.Num()));
    }
}

/** Children with no candidate at all: unused branches in a subset, otherwise a
    reported failure. */
void ResolveUnmatchedSiblings(
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target,
    FMtoUScope& Scope,
    FMtoUMappingPass& Pass)
{
    for (FMtoUScopeBone& Bone : Scope.Bones)
    {
        if (Bone.bResolved)
        {
            continue;
        }
        Bone.bResolved = true;
        const FMtoUDescriptionBone& MayaBone = Character.Bones[Bone.MayaIndex];
        if (Target.bAllowUnusedSourceBones)
        {
            // A duplicate on an unused branch must not reject a required name
            // that another Maya branch can map correctly.
            for (int32 WrongParent : Bone.WrongParentExact)
            {
                Pass.ParentMismatches.Add(FString::Printf(TEXT("%s: Maya=%s, Unreal=%s"),
                    *BonePath(Character.Bones, Bone.MayaIndex),
                    *ParentName(Character.Bones, MayaBone.ParentIndex),
                    *ParentName(Target.Bones, Target.Bones[WrongParent].ParentIndex)));
            }
            Pass.SkippedMaya.Add(Bone.MayaIndex);
            continue;
        }
        if (Bone.WrongParentExact.Num() == 1)
        {
            const int32 UnrealIndex = Bone.WrongParentExact[0];
            const FString UnrealParent = ParentName(Target.Bones, Target.Bones[UnrealIndex].ParentIndex);
            Pass.AccountedUnreal.Add(UnrealIndex);
            Pass.ParentMismatches.Add(FString::Printf(TEXT("%s: Maya=%s, Unreal=%s"),
                *MayaBone.Name.ToString(),
                *ParentName(Character.Bones, MayaBone.ParentIndex),
                *UnrealParent));
            FailScopeBone(Pass, Bone.MayaIndex, Scope.ExpectedUnrealParent,
                FString::Printf(TEXT("the only Unreal bone with this name sits below %s"),
                    *UnrealParent));
            continue;
        }
        if (Bone.WrongParentExact.Num() > 1)
        {
            Pass.AccountedUnreal.Append(Bone.WrongParentExact);
            Pass.MappingAmbiguities.Add(FString::Printf(
                TEXT("%s has %d candidates under different Unreal parents"),
                *MayaBone.Name.ToString(),
                Bone.WrongParentExact.Num()));
            FailScopeBone(Pass, Bone.MayaIndex, Scope.ExpectedUnrealParent,
                FString::Printf(TEXT("%d Unreal bones with this name sit under different parents"),
                    Bone.WrongParentExact.Num()));
            continue;
        }
        Pass.MissingBones.Add(MayaBone.Name.ToString());
        FailScopeBone(Pass, Bone.MayaIndex, Scope.ExpectedUnrealParent,
            TEXT("no Unreal bone matches under the expected parent"));
    }
}

/** Resolves every child of one mapped parent, in one deterministic order. */
void ResolveScope(
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target,
    const TMap<FName, FMtoUSourceNameInfo>& SourceNames,
    const TArray<int32>& Siblings,
    int32 ExpectedUnrealParent,
    FMtoUMappingPass& Pass)
{
    FMtoUScope Scope = BuildScope(
        Character, Target, SourceNames, Siblings, ExpectedUnrealParent, Pass);
    ResolveExactNames(Character, Target, Scope, Pass);
    ResolveImportRenames(Character, Target, Scope, Pass);
    ResolveUnmatchedSiblings(Character, Target, Scope, Pass);
}

/**
 * Maps Maya bones parent-first. Every child of one mapped parent is resolved as
 * a single scope, so no mapping decision depends on sibling capture order.
 */
FMtoUMappingPass RunMappingPass(
    const FMtoUCharacterDescription& Character,
    const FMtoUTargetDescription& Target,
    const TMap<FName, FMtoUSourceNameInfo>& SourceNames)
{
    FMtoUMappingPass Pass;
    Pass.MayaToUnreal.Init(INDEX_NONE, Character.Bones.Num());
    Pass.bMayaMappingFailed.Init(false, Character.Bones.Num());
    Pass.TargetBoneIndices.Init(INDEX_NONE, Character.Bones.Num());

    // Children of every Maya bone, and the roots, in capture order.
    const int32 RootScope = Character.Bones.Num();
    TArray<TArray<int32>> Children;
    Children.SetNum(Character.Bones.Num() + 1);
    for (int32 MayaIndex = 0; MayaIndex < Character.Bones.Num(); ++MayaIndex)
    {
        const int32 ParentIndex = Character.Bones[MayaIndex].ParentIndex;
        Children[Character.Bones.IsValidIndex(ParentIndex) ? ParentIndex : RootScope].Add(MayaIndex);
    }

    // Scopes are resolved top-down, so a bone is negotiated only after its
    // parent is mapped. Children of a skipped or failed parent inherit that
    // outcome instead of being negotiated on their own.
    TArray<int32> PendingScopes;
    PendingScopes.Add(RootScope);
    for (int32 Cursor = 0; Cursor < PendingScopes.Num(); ++Cursor)
    {
        const int32 ScopeKey = PendingScopes[Cursor];
        if (ScopeKey != RootScope)
        {
            if (Pass.SkippedMaya.Contains(ScopeKey))
            {
                for (int32 Child : Children[ScopeKey])
                {
                    Pass.SkippedMaya.Add(Child);
                }
                PendingScopes.Append(Children[ScopeKey]);
                continue;
            }
            if (Pass.bMayaMappingFailed[ScopeKey])
            {
                for (int32 Child : Children[ScopeKey])
                {
                    Pass.bMayaMappingFailed[Child] = true;
                    Pass.DescendantsBlockedByParent.Add(FString::Printf(
                        TEXT("%s: parent %s is not mapped"),
                        *Character.Bones[Child].Name.ToString(),
                        *Character.Bones[ScopeKey].Name.ToString()));
                }
                PendingScopes.Append(Children[ScopeKey]);
                continue;
            }
        }
        const int32 ExpectedUnrealParent = ScopeKey == RootScope
            ? INDEX_NONE
            : Pass.MayaToUnreal[ScopeKey];
        ResolveScope(Character, Target, SourceNames, Children[ScopeKey], ExpectedUnrealParent, Pass);
        PendingScopes.Append(Children[ScopeKey]);
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
    for (int32 Index = 0; Index < Character.Bones.Num(); ++Index)
    {
        ++SourceNames.FindOrAdd(Character.Bones[Index].Name).Count;
    }
    FMtoUMappingPass Pass = RunMappingPass(Character, Target, SourceNames);

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

    if (!Pass.DirectFailures.IsEmpty())
    {
        // The first failure in capture order leads the diagnosis.
        const FMtoUDirectFailure* First = &Pass.DirectFailures[0];
        for (const FMtoUDirectFailure& Candidate : Pass.DirectFailures)
        {
            if (Candidate.MayaIndex < First->MayaIndex)
            {
                First = &Candidate;
            }
        }
        FMtoUNegotiationFailure Failure;
        Failure.MayaPath = BonePath(Character.Bones, First->MayaIndex);
        Failure.ExpectedParent = ParentName(
            Character.Bones, Character.Bones[First->MayaIndex].ParentIndex);
        Failure.Reason = First->Reason;
        Failure.UnreachedUnreal = Outcome.UnreachedUnrealBones.Num();
        for (int32 Index = 0; Index < Character.Bones.Num(); ++Index)
        {
            if (Index == First->MayaIndex || !Pass.bMayaMappingFailed[Index])
            {
                continue;
            }
            for (int32 Parent = Character.Bones[Index].ParentIndex;
                Parent != INDEX_NONE;
                Parent = Character.Bones[Parent].ParentIndex)
            {
                if (Parent == First->MayaIndex)
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
        const FName FailedName = Character.Bones[First->MayaIndex].Name;
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
            if (Character.Bones[First->MayaIndex].ParentIndex == INDEX_NONE)
            {
                Failure.RenameHints.Add(FString::Printf(
                    TEXT("%s (a root bone has no mapped parent to scope the rename)"),
                    *UnrealBone.Name.ToString()));
            }
            else if (UnrealBone.ParentIndex != First->ExpectedUnrealParent)
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
