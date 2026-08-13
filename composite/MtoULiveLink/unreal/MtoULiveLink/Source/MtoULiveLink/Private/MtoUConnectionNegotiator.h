#pragma once

#include "CoreMinimal.h"

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
};

struct FMtoUNegotiationOutcome
{
    bool bUsable = false;
    TArray<FName> PublishBoneNames;
    TArray<FString> BoneNameMappings;
    TArray<int32> AcceptedCurveIndices;
    TArray<FName> AcceptedCurveNames;
    TArray<FName> MayaOnlyMorphNames;
    TArray<FName> UnrealOnlyMorphNames;
    FString FailureCategory;
    TArray<FString> MissingBones;
    TArray<FString> ExtraBones;
    TArray<FString> ParentMismatches;
    TArray<FString> MappingAmbiguities;
    TArray<FString> DescendantsBlockedByParent;

    FString TechnicalDetails() const;
};

class FMtoUConnectionNegotiator
{
public:
    static FMtoUNegotiationOutcome Negotiate(
        const FMtoUCharacterDescription& Character,
        const FMtoUTargetDescription& Target);
};
