#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

struct FMtoUDescriptionBone
{
    FName Name;
    int32 ParentIndex = INDEX_NONE;
};

struct FMtoUCharacterDescription
{
    TArray<FMtoUDescriptionBone> Bones;
    TArray<FName> CurveNames;
};

struct FMtoUTargetDescription
{
    TArray<FMtoUDescriptionBone> Bones;
    TArray<FName> MorphTargetNames;
    /**
     * Composition targets require every target bone, but allow unused Maya
     * branches. An unused branch may not claim a required target: an exact Maya
     * name outranks an import rename for it, and two equally valid Maya sources
     * are reported as an ambiguity instead of letting the capture order choose.
     */
    bool bAllowUnusedSourceBones = false;
    TArray<FString> BoneOwners;
};

/**
 * Root cause of the first blocking bone-mapping failure. It is rendered before
 * the detailed lists so the broken branch, its parent, and its impact are
 * visible without reading the descendant lists.
 */
struct FMtoUNegotiationFailure
{
    /** Complete published Maya path of the bone that could not be mapped. */
    FString MayaPath;
    /** Mapped Unreal parent the bone had to match, or `<root>` for a root bone. */
    FString ExpectedParent;
    /** Why the mapping failed: no candidate, parent mismatch, or ambiguity. */
    FString Reason;
    /** Maya descendants that stay unpublished while this bone is unmapped. */
    int32 BlockedDescendants = 0;
    /** Unreal bones below an unmatched ancestor that were never examined. */
    int32 UnreachedUnreal = 0;
    /** Diagnostics only: import-rename shaped Unreal names considered for this
        bone, each with the reason the strict rules did not apply it. */
    TArray<FString> RenameHints;
};

struct FMtoUNegotiationOutcome
{
    bool bUsable = false;
    /** Target bone each published Maya bone maps to, by complete Maya snapshot index. */
    TArray<int32> TargetBoneIndices;
    TArray<FString> BoneNameMappings;
    TArray<int32> AcceptedCurveIndices;
    TArray<FName> AcceptedCurveNames;
    TArray<FName> MayaOnlyMorphNames;
    TArray<FName> UnrealOnlyMorphNames;
    FString FailureCategory;
    TArray<FString> MissingBones;
    TArray<FString> ExtraBones;
    /** Unreal bones below an unmatched ancestor: never examined, so they are
        not confirmed extra bones. */
    TArray<FString> UnreachedUnrealBones;
    TArray<FString> ParentMismatches;
    TArray<FString> MappingAmbiguities;
    TArray<FString> DescendantsBlockedByParent;
    /** Root cause of the first blocking mapping failure. */
    TOptional<FMtoUNegotiationFailure> FirstFailure;

    FString TechnicalDetails() const;
};

class FMtoUConnectionNegotiator
{
public:
    static FMtoUNegotiationOutcome Negotiate(
        const FMtoUCharacterDescription& Character,
        const FMtoUTargetDescription& Target);
};
