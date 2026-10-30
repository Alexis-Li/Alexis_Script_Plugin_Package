#pragma once

#include "CoreMinimal.h"
#include "ReferenceSkeleton.h"

class UMtoULiveLinkBinding;
class USkeletalMesh;

/**
 * Stable identity of one enabled character part. It deliberately excludes the
 * display name and the list position, because renaming a part or reordering
 * the Additional Parts list changes nothing that a streaming session or a
 * Preview revision depends on.
 */
struct MTOULIVELINK_API FMtoUCharacterPartIdentity
{
    FGuid PartId;
    USkeletalMesh* Mesh = nullptr;

    bool operator==(const FMtoUCharacterPartIdentity& Other) const
    {
        return PartId == Other.PartId && Mesh == Other.Mesh;
    }

    bool operator<(const FMtoUCharacterPartIdentity& Other) const
    {
        return PartId == Other.PartId
            ? Mesh < Other.Mesh
            : PartId < Other.PartId;
    }
};

/** One resolved character part: the Primary Driver or one Additional Part. */
struct MTOULIVELINK_API FMtoUCharacterPartResolution
{
    FGuid PartId;
    /** User-facing name; falls back to the Skeletal Mesh name when unnamed. */
    FString Name;
    USkeletalMesh* Mesh = nullptr;
    bool bPrimary = false;
    bool bEnabled = true;
    /** Empty while this part is compatible; otherwise the named reason. */
    FString Problem;
};

/** Stable connection-failure categories a composition refusal reports. */
struct MTOULIVELINK_API FMtoUCompositionFailures
{
    /** The Binding names no usable Primary Driver or an enabled part has no mesh. */
    static constexpr TCHAR InvalidBinding[] = TEXT("INVALID_BINDING");
    /** An enabled part's required bones conflict with Skeleton, parent path, or reference pose. */
    static constexpr TCHAR SkeletonMismatch[] = TEXT("SKELETON_MISMATCH");
};

/**
 * The one centralized resolution of a Binding's character composition: which
 * parts form the character, whether every enabled part's required bones are
 * compatible, and which Morph Target library the composed character exposes.
 * Placement, connection negotiation, and diagnostics all consume this result
 * instead of restating its rules.
 *
 * Enabled meshes share a Skeleton asset. Their positive skin influences across
 * every LOD and complete ancestor chains form one deterministic target. Bones
 * another mesh requires must agree in name, parent path and component reference
 * pose; branches no enabled mesh skins impose no requirement.
 */
struct MTOULIVELINK_API FMtoUCharacterComposition
{
    /** The Primary Driver first, then every Additional Part in Binding order. */
    TArray<FMtoUCharacterPartResolution> Parts;

    /** Parent-first union of required bones; never persisted into a mesh asset. */
    FReferenceSkeleton RequiredSkeleton;
    /** Part labels requiring each union bone, for negotiation diagnostics. */
    TArray<FString> BoneOwners;

    /** One named section per incompatible part; empty when the composition is usable. */
    FString Diagnostics;

    /** Human-readable composition for the Details panel. */
    FString Summary;

    /** Stable category of the first blocking problem, for the connection error. */
    FString FailureCategory;

    bool IsUsable() const { return bUsable; }

    /** Enabled parts in canonical identity order, so list edits stay inert. */
    const TArray<FMtoUCharacterPartIdentity>& GetEnabledIdentity() const { return EnabledIdentity; }

    /**
     * Adds the Morph Target names of the Primary Driver and of every enabled
     * compatible part, deduplicated by name. A name that only some parts own
     * stays in the library and simply drives the meshes that have it.
     */
    void AppendMorphNames(TArray<FName>& InOut) const;

    /**
     * The one Primary Driver rule placement needs: a Binding may be placed
     * with incomplete parts, but never without a character.
     */
    static bool HasPrimaryDriver(const UMtoULiveLinkBinding* Binding);

    static FMtoUCharacterComposition Resolve(const UMtoULiveLinkBinding* Binding);

private:
    bool bUsable = false;
    TArray<FMtoUCharacterPartIdentity> EnabledIdentity;
};
