#pragma once

#include "CoreMinimal.h"

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
    /** An enabled part cannot map onto the Primary Driver. */
    static constexpr TCHAR SkeletonMismatch[] = TEXT("SKELETON_MISMATCH");
};

/**
 * The one centralized resolution of a Binding's character composition: which
 * parts form the character, whether each enabled part is compatible with the
 * Primary Driver, and which Morph Target library the composed character
 * exposes. Placement, connection negotiation, and diagnostics all consume this
 * result instead of restating its rules.
 *
 * The Primary Driver defines the skeleton baseline. Every enabled Additional
 * Part must share the Primary's Skeleton asset, map every bone it contains onto
 * the Primary by name and parent path, and match the Primary's reference pose
 * for its skinning bones across all LODs and their ancestors; unrelated branches
 * may differ in reference pose. A part may use fewer bones and different geometry.
 */
struct MTOULIVELINK_API FMtoUCharacterComposition
{
    /** The Primary Driver first, then every Additional Part in Binding order. */
    TArray<FMtoUCharacterPartResolution> Parts;

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
