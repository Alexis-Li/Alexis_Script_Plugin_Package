#include "MtoUDriverGarmentSurface.h"

#include "MtoULiveLinkBinding.h"

#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "MeshDescription.h"
#include "Selections/MeshConnectedComponents.h"
#include "StaticMeshAttributes.h"

using namespace UE::Geometry;

namespace
{
/**
 * Maximum distance between a Preview vertex and its nearest Driver surface for
 * that vertex to count as spatially agreeing, as a fraction of the Preview
 * scale. Selection itself uses nearest-surface ownership, so unrelated regions
 * cannot be picked; this radius only separates an aligned Preview from a
 * globally misaligned one.
 */
constexpr double GarmentAgreementRadiusFraction = 0.05;

/** Compact position-only copy of one region's triangles for spatial queries. */
FDynamicMesh3 ExtractRegionGeometry(
    const FDynamicMesh3& Mesh,
    const TArray<int32>& TriangleIDs)
{
    FDynamicMesh3 Out;
    TMap<int32, int32> VertexMap;
    for (const int32 TriangleID : TriangleIDs)
    {
        FIndex3i Triangle = Mesh.GetTriangle(TriangleID);
        for (int32* Corner : {&Triangle.A, &Triangle.B, &Triangle.C})
        {
            const int32 SourceVertexID = *Corner;
            if (int32* Mapped = VertexMap.Find(SourceVertexID))
            {
                *Corner = *Mapped;
            }
            else
            {
                *Corner = Out.AppendVertex(Mesh.GetVertex(SourceVertexID));
                VertexMap.Add(SourceVertexID, *Corner);
            }
        }
        Out.AppendTriangle(Triangle, 0);
    }
    return Out;
}

/** Stable imported identity of one Driver material slot for override matching. */
FName GetDriverSlotIdentity(const FSkeletalMaterial& Slot)
{
    return Slot.ImportedMaterialSlotName != NAME_None
        ? Slot.ImportedMaterialSlotName
        : Slot.MaterialSlotName;
}

FString DescribeDriverSlot(
    const TArray<FSkeletalMaterial>& Slots,
    const int32 SlotIndex)
{
    const FSkeletalMaterial& Slot = Slots[SlotIndex];
    return Slot.MaterialSlotName == GetDriverSlotIdentity(Slot)
        ? FString::Printf(TEXT("'%s'"), *Slot.MaterialSlotName.ToString())
        : FString::Printf(TEXT("'%s' (displayed as '%s')"),
            *GetDriverSlotIdentity(Slot).ToString(), *Slot.MaterialSlotName.ToString());
}

constexpr int32 AmbiguousMaterialSlot = -2;

bool BuildPolygonGroupToMaterialSlotMap(
    const USkeletalMesh& DriverAsset,
    TArray<int32>& OutSlotByOrdinal)
{
    FMeshDescription* Description = DriverAsset.GetMeshDescription(0);
    if (!Description
        || !Description->PolygonGroupAttributes().HasAttribute(
            MeshAttribute::PolygonGroup::ImportedMaterialSlotName))
    {
        return false;
    }

    FStaticMeshAttributes DescriptionAttributes(*Description);
    const TPolygonGroupAttributesConstRef<FName> GroupSlotNames =
        DescriptionAttributes.GetPolygonGroupMaterialSlotNames();
    int32 MaxOrdinal = INDEX_NONE;
    for (const FPolygonGroupID GroupID : Description->PolygonGroups().GetElementIDs())
    {
        MaxOrdinal = FMath::Max(MaxOrdinal, GroupID.GetValue());
    }
    OutSlotByOrdinal.Init(INDEX_NONE, MaxOrdinal + 1);
    const TArray<FSkeletalMaterial>& Slots = DriverAsset.GetMaterials();
    for (const FPolygonGroupID GroupID : Description->PolygonGroups().GetElementIDs())
    {
        for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
        {
            if (GetDriverSlotIdentity(Slots[SlotIndex])
                != GroupSlotNames[GroupID])
            {
                continue;
            }
            int32& Mapped = OutSlotByOrdinal[GroupID.GetValue()];
            Mapped = Mapped == INDEX_NONE ? SlotIndex : AmbiguousMaterialSlot;
        }
    }
    return true;
}

bool BuildTriangleGroupToMaterialSlotMap(
    const USkeletalMesh& DriverAsset,
    TArray<int32>& OutSlotByGroup)
{
    FGeometryScriptMeshReadLOD SourceLOD;
    SourceLOD.LODType = EGeometryScriptLODType::SourceModel;
    SourceLOD.LODIndex = 0;
    TArray<UMaterialInterface*> SectionMaterials;
    TArray<FName> SectionSlotNames;
    EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
    UGeometryScriptLibrary_StaticMeshFunctions::GetLODMaterialListFromSkeletalMesh(
        const_cast<USkeletalMesh*>(&DriverAsset),
        SourceLOD,
        SectionMaterials,
        OutSlotByGroup,
        SectionSlotNames,
        Outcome);
    if (Outcome != EGeometryScriptOutcomePins::Success)
    {
        return false;
    }
    const TArray<FSkeletalMaterial>& Slots = DriverAsset.GetMaterials();
    for (int32& SlotIndex : OutSlotByGroup)
    {
        if (!Slots.IsValidIndex(SlotIndex))
        {
            SlotIndex = INDEX_NONE;
        }
    }
    return true;
}

struct FDriverMaterialSlotMaps
{
    TArray<int32> SlotByPolygonGroup;
    TArray<int32> SlotByTriangleGroup;
    bool bHasTriangleGroupMapping = false;
};

bool BuildDriverMaterialSlotMaps(
    const USkeletalMesh& DriverAsset,
    FDriverMaterialSlotMaps& OutMaps)
{
    if (!BuildPolygonGroupToMaterialSlotMap(
            DriverAsset, OutMaps.SlotByPolygonGroup))
    {
        return false;
    }
    OutMaps.bHasTriangleGroupMapping =
        BuildTriangleGroupToMaterialSlotMap(
            DriverAsset, OutMaps.SlotByTriangleGroup)
        && !OutMaps.SlotByTriangleGroup.IsEmpty();
    return true;
}

struct FDriverMaterialSlotMetadata
{
    bool bCollapsedMaterialMetadata = false;
    bool bUseTriangleGroupMetadata = false;
};

FDriverMaterialSlotMetadata AnalyzeDriverMaterialSlotMetadata(
    const FDynamicMesh3& Driver,
    const TArray<FSkeletalMaterial>& Slots,
    const FDynamicMeshMaterialAttribute* MaterialIDs,
    const FDriverMaterialSlotMaps& SlotMaps)
{
    TSet<int32> MaterialOrdinals;
    TSet<int32> PolygonGroupSlots;
    TSet<int32> TriangleGroupSlots;
    for (const int32 TriangleID : Driver.TriangleIndicesItr())
    {
        const int32 MaterialOrdinal = MaterialIDs
            ? MaterialIDs->GetValue(TriangleID)
            : INDEX_NONE;
        MaterialOrdinals.Add(MaterialOrdinal);
        const int32 PolygonGroupSlot = SlotMaps.SlotByPolygonGroup.IsValidIndex(MaterialOrdinal)
            ? SlotMaps.SlotByPolygonGroup[MaterialOrdinal]
            : INDEX_NONE;
        if (Slots.IsValidIndex(PolygonGroupSlot))
        {
            PolygonGroupSlots.Add(PolygonGroupSlot);
        }
        const int32 TriangleGroup = Driver.GetTriangleGroup(TriangleID);
        const int32 TriangleGroupSlot = SlotMaps.SlotByTriangleGroup.IsValidIndex(TriangleGroup)
            ? SlotMaps.SlotByTriangleGroup[TriangleGroup]
            : INDEX_NONE;
        if (Slots.IsValidIndex(TriangleGroupSlot))
        {
            TriangleGroupSlots.Add(TriangleGroupSlot);
        }
    }
    FDriverMaterialSlotMetadata Metadata;
    Metadata.bCollapsedMaterialMetadata = MaterialOrdinals.Num() <= 1;
    Metadata.bUseTriangleGroupMetadata = Metadata.bCollapsedMaterialMetadata
        && TriangleGroupSlots.Num() > PolygonGroupSlots.Num();
    return Metadata;
}

enum class EDriverMaterialSlotResolutionMode : uint8
{
    Automatic,
    Manual,
};

enum class EDriverMaterialSlotResolutionResult : uint8
{
    Resolved,
    Unmapped,
    Conflict,
};

/** Resolves both stable metadata signals for one triangle without ordinal guessing. */
struct FDriverMaterialSlotResolver
{
    const FDynamicMesh3& Driver;
    const TArray<FSkeletalMaterial>& Slots;
    const FDynamicMeshMaterialAttribute* MaterialIDs;
    const TArray<int32>& SlotByPolygonGroup;
    const TArray<int32>& SlotByTriangleGroup;

    EDriverMaterialSlotResolutionResult Resolve(
        const int32 TriangleID,
        const EDriverMaterialSlotResolutionMode Mode,
        const bool bUseTriangleGroupMetadata,
        const bool bCollapsedMaterialMetadata,
        int32& OutSlot,
        FString& OutError) const
    {
        const int32 MaterialOrdinal = MaterialIDs
            ? MaterialIDs->GetValue(TriangleID)
            : INDEX_NONE;
        const int32 PolygonGroupSlot = SlotByPolygonGroup.IsValidIndex(MaterialOrdinal)
            ? SlotByPolygonGroup[MaterialOrdinal]
            : INDEX_NONE;
        const int32 TriangleGroup = Driver.GetTriangleGroup(TriangleID);
        const int32 TriangleGroupSlot = SlotByTriangleGroup.IsValidIndex(TriangleGroup)
            ? SlotByTriangleGroup[TriangleGroup]
            : INDEX_NONE;
        const bool bPolygonSlotValid = Slots.IsValidIndex(PolygonGroupSlot);
        const bool bTriangleGroupSlotValid = Slots.IsValidIndex(TriangleGroupSlot);
        const auto ReportConflict = [&]()
        {
            OutError = FString::Printf(
                TEXT("Driver triangle %d has conflicting current Driver material metadata: "
                    "polygon-group %d resolves to material slot %s, while current LOD triangle-group %d "
                    "resolves to %s. Restore unique imported material-slot names or correct the current LOD "
                    "material mapping before Refresh Preview."),
                TriangleID,
                MaterialOrdinal,
                *DescribeDriverSlot(Slots, PolygonGroupSlot),
                TriangleGroup,
                *DescribeDriverSlot(Slots, TriangleGroupSlot));
        };

        // A collapsed source must prove every triangle through its current LOD
        // section metadata. When that metadata is the richer signal, a
        // polygon-group mismatch is expected; otherwise both signals must agree.
        if (bCollapsedMaterialMetadata)
        {
            if (!bTriangleGroupSlotValid)
            {
                return EDriverMaterialSlotResolutionResult::Unmapped;
            }
            if (!bUseTriangleGroupMetadata
                && bPolygonSlotValid
                && PolygonGroupSlot != TriangleGroupSlot)
            {
                ReportConflict();
                return EDriverMaterialSlotResolutionResult::Conflict;
            }
            OutSlot = TriangleGroupSlot;
            return EDriverMaterialSlotResolutionResult::Resolved;
        }

        if (Mode == EDriverMaterialSlotResolutionMode::Manual)
        {
            if (bPolygonSlotValid)
            {
                OutSlot = PolygonGroupSlot;
                return EDriverMaterialSlotResolutionResult::Resolved;
            }
            if (bTriangleGroupSlotValid)
            {
                OutSlot = TriangleGroupSlot;
                return EDriverMaterialSlotResolutionResult::Resolved;
            }
            return EDriverMaterialSlotResolutionResult::Unmapped;
        }

        if (!bPolygonSlotValid)
        {
            return EDriverMaterialSlotResolutionResult::Unmapped;
        }
        OutSlot = PolygonGroupSlot;
        return EDriverMaterialSlotResolutionResult::Resolved;
    }
};

bool MatchManualOverrideSlots(
    const TArray<FSkeletalMaterial>& Slots,
    const TArray<FName>& Override,
    TArray<int32>& OutMatchedSlots,
    TSet<int32>& OutUniqueMatchedSlots,
    FString& OutError)
{
    TSet<FName> AvailableIdentities;
    for (const FSkeletalMaterial& Slot : Slots)
    {
        AvailableIdentities.Add(GetDriverSlotIdentity(Slot));
    }
    OutMatchedSlots.Reserve(Override.Num());
    for (const FName Entry : Override)
    {
        TArray<int32> EntryMatches;
        for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
        {
            if (GetDriverSlotIdentity(Slots[SlotIndex]) == Entry)
            {
                EntryMatches.Add(SlotIndex);
            }
        }
        if (EntryMatches.IsEmpty())
        {
            TArray<FString> SortedIdentities;
            for (const FName Identity : AvailableIdentities)
            {
                SortedIdentities.Add(Identity.ToString());
            }
            SortedIdentities.Sort();
            OutError = FString::Printf(
                TEXT("Manual garment override names unknown Driver material slot '%s'. The current Driver import "
                    "provides these slot names: %s. Reimporting the Driver may have renamed or removed it; refresh "
                    "stays blocked instead of silently returning to automatic resolution."),
                *Entry.ToString(), *FString::Join(SortedIdentities, TEXT(", ")));
            return false;
        }
        if (EntryMatches.Num() > 1)
        {
            TArray<FString> MatchDescriptions;
            for (const int32 SlotIndex : EntryMatches)
            {
                MatchDescriptions.Add(DescribeDriverSlot(Slots, SlotIndex));
            }
            OutError = FString::Printf(
                TEXT("Manual garment override name '%s' matches more than one Driver material slot (%s); "
                    "the persisted identity is no longer unique and cannot select a source region."),
                *Entry.ToString(), *FString::Join(MatchDescriptions, TEXT(", ")));
            return false;
        }
        if (OutUniqueMatchedSlots.Contains(EntryMatches[0]))
        {
            OutError = FString::Printf(
                TEXT("Manual garment override selects Driver material slot %s more than once; remove the "
                    "duplicated entry so every selected region stays unambiguous."),
                *DescribeDriverSlot(Slots, EntryMatches[0]));
            return false;
        }
        OutUniqueMatchedSlots.Add(EntryMatches[0]);
        OutMatchedSlots.Add(EntryMatches[0]);
    }
    return true;
}

bool ValidateManualGarmentCoverage(
    const FDynamicMesh3& Preview,
    const double Scale,
    const double MisalignedBound,
    const TArray<FSkeletalMaterial>& Slots,
    const TArray<int32>& MatchedSlots,
    FMtoUDriverGarmentSurfaceResult& Out,
    FString& OutError)
{
    FMeshConnectedComponents Components(&Out.Surface);
    Components.FindConnectedTriangles();
    Out.RegionCount = Components.Components.Num();

    const double AgreementRadiusSquared =
        FMath::Square(GarmentAgreementRadiusFraction * Scale);
    const FDynamicMeshAABBTree3 ResolvedSpatial(&Out.Surface, true);
    int32 AgreeingCount = 0;
    int32 PreviewVertexCount = 0;
    for (const int32 VertexID : Preview.VertexIndicesItr())
    {
        ++PreviewVertexCount;
        double DistanceSquared = 0.0;
        ResolvedSpatial.FindNearestTriangle(Preview.GetVertex(VertexID), DistanceSquared);
        AgreeingCount += DistanceSquared <= AgreementRadiusSquared ? 1 : 0;
    }
    if (PreviewVertexCount == 0)
    {
        OutError = TEXT("Preview Static Mesh has no LOD0 geometry to match against the Driver.");
        return false;
    }
    Out.MatchedPreviewCoverage =
        static_cast<double>(AgreeingCount) / PreviewVertexCount;

    TArray<FString> IdentitySummaries;
    for (const int32 SlotIndex : MatchedSlots)
    {
        IdentitySummaries.AddUnique(GetDriverSlotIdentity(Slots[SlotIndex]).ToString());
    }
    Out.RegionSummary = FString::Printf(
        TEXT("manual slots %s; %d tris"),
        *FString::Join(IdentitySummaries, TEXT(", ")),
        Out.TriangleCount);
    if (AgreeingCount == PreviewVertexCount)
    {
        return true;
    }
    OutError = FString::Printf(
        TEXT("Manual garment override resolved %d Driver triangle(s) from %d slot(s) [%s], but only %d of %d "
            "Preview vertices lie within %.4f of the Preview scale or the calibrated misalignment bound %.4f on "
            "that surface. The selection does not cover the whole Preview garment: check reference pose, origin, "
            "units, and asset-local import space, or select every Driver material slot of this outfit."),
        Out.TriangleCount,
        MatchedSlots.Num(),
        *FString::Join(IdentitySummaries, TEXT(", ")),
        AgreeingCount,
        PreviewVertexCount,
        GarmentAgreementRadiusFraction,
        MisalignedBound);
    return false;
}

/**
 * Shared admission preflight for both resolution paths: non-empty Driver
 * geometry and a usable Preview scale. Returns the Preview scale, or 0 with
 * OutError set.
 */
double BeginGarmentSurfaceResolution(
    const FDynamicMesh3& Driver,
    const FDynamicMesh3& Preview,
    FString& OutError)
{
    if (Driver.TriangleCount() == 0)
    {
        OutError = TEXT("Driver LOD0 source geometry is empty; no garment surface can be resolved.");
        return 0.0;
    }
    const double Scale = Preview.GetBounds().DiagonalLength();
    if (Scale <= UE_SMALL_NUMBER)
    {
        OutError = TEXT("Preview Static Mesh bounding box has no usable size; scale is invalid.");
        return 0.0;
    }
    return Scale;
}

bool CollectMaterialSlotIndices(
    const FDynamicMesh3& Driver,
    const FDynamicMesh3& Surface,
    const USkeletalMesh& DriverAsset,
    TArray<int32>& OutSlotIndices,
    FString& OutError)
{
    const TArray<FSkeletalMaterial>& Slots = DriverAsset.GetMaterials();
    FDriverMaterialSlotMaps SlotMaps;
    if (!BuildDriverMaterialSlotMaps(DriverAsset, SlotMaps))
    {
        OutError = TEXT("Driver LOD0 source data has no imported material-slot identities for safe Model display composition.");
        return false;
    }
    const FDynamicMeshMaterialAttribute* DriverMaterialIDs =
        Driver.Attributes() ? Driver.Attributes()->GetMaterialID() : nullptr;
    const FDriverMaterialSlotMetadata Metadata =
        AnalyzeDriverMaterialSlotMetadata(
            Driver, Slots, DriverMaterialIDs, SlotMaps);
    const FDriverMaterialSlotResolver SlotResolver{
        Driver,
        Slots,
        DriverMaterialIDs,
        SlotMaps.SlotByPolygonGroup,
        SlotMaps.SlotByTriangleGroup};
    TSet<int32> SelectedSlots;
    TSet<int32> UnselectedSlots;
    TSet<int32> UnmappedOrdinals;
    for (const int32 TriangleID : Driver.TriangleIndicesItr())
    {
        const int32 MaterialOrdinal = DriverMaterialIDs
            ? DriverMaterialIDs->GetValue(TriangleID)
            : INDEX_NONE;
        const int32 TriangleGroup = Driver.GetTriangleGroup(TriangleID);
        int32 SlotIndex = INDEX_NONE;
        const EDriverMaterialSlotResolutionResult Resolution =
            SlotResolver.Resolve(
                TriangleID,
                EDriverMaterialSlotResolutionMode::Automatic,
                Metadata.bUseTriangleGroupMetadata,
                Metadata.bCollapsedMaterialMetadata,
                SlotIndex,
                OutError);
        if (Resolution == EDriverMaterialSlotResolutionResult::Conflict)
        {
            return false;
        }
        if (Resolution == EDriverMaterialSlotResolutionResult::Unmapped)
        {
            UnmappedOrdinals.Add(
                Metadata.bCollapsedMaterialMetadata ? TriangleGroup : MaterialOrdinal);
            continue;
        }
        if (Surface.IsTriangle(TriangleID))
        {
            SelectedSlots.Add(SlotIndex);
        }
        else
        {
            UnselectedSlots.Add(SlotIndex);
        }
    }
    if (!UnmappedOrdinals.IsEmpty() || SelectedSlots.IsEmpty())
    {
        TArray<int32> SortedOrdinals = UnmappedOrdinals.Array();
        SortedOrdinals.Sort();
        TArray<FString> OrdinalLabels;
        for (const int32 Ordinal : SortedOrdinals)
        {
            OrdinalLabels.Add(FString::FromInt(Ordinal));
        }
        OutError = FString::Printf(
            TEXT("Driver source material group(s) [%s] cannot be mapped uniquely through current Driver "
                "material metadata to a material slot. Restore unique imported material-slot "
                "names or split and reimport the Driver before Refresh Preview."),
            *FString::Join(OrdinalLabels, TEXT(", ")));
        return false;
    }
    for (const int32 SlotIndex : SelectedSlots)
    {
        if (UnselectedSlots.Contains(SlotIndex))
        {
            OutError = FString::Printf(
                TEXT("Resolved garment geometry shares material slot %s with visible non-garment Driver geometry. Split the garment into its own material slot before Refresh Preview."),
                *DescribeDriverSlot(Slots, SlotIndex));
            return false;
        }
    }
    OutSlotIndices = SelectedSlots.Array();
    OutSlotIndices.Sort();
    return true;
}

/**
 * Shared removal tail: keep only the selected triangles while removing their
 * isolated vertices, so every resolved-surface vertex ID stays identical to
 * its original Driver LOD0 import vertex.
 */
void FinalizeResolvedGarmentSurface(
    const FDynamicMesh3& Driver,
    const TArray<int32>& UnselectedTriangles,
    FMtoUDriverGarmentSurfaceResult& Out)
{
    Out.Surface = Driver;
    TArray<int32> Sorted(UnselectedTriangles);
    Sorted.Sort(TGreater<int32>());
    for (const int32 TriangleID : Sorted)
    {
        Out.Surface.RemoveTriangle(TriangleID);
    }
}

/** Compact identification of one connected region: triangle count and touched material sections. */
FString DescribeConnectedRegion(
    const FDynamicMesh3& Mesh,
    const FMeshConnectedComponents::FComponent& Region,
    const FDynamicMeshMaterialAttribute* MaterialIDs)
{
    TSet<int32> SectionIDs;
    for (const int32 TriangleID : Region.Indices)
    {
        SectionIDs.Add(MaterialIDs
            ? MaterialIDs->GetValue(TriangleID)
            : Mesh.GetTriangleGroup(TriangleID));
    }
    TArray<int32> SortedSections = SectionIDs.Array();
    SortedSections.Sort();
    FString Text = FString::Printf(
        TEXT("%d tris in section%s %d"),
        Region.Indices.Num(),
        SortedSections.Num() == 1 ? TEXT("") : TEXT("s"),
        SortedSections.Num() > 0 ? SortedSections[0] : 0);
    for (int32 Index = 1; Index < SortedSections.Num(); ++Index)
    {
        Text += FString::Printf(TEXT("/%d"), SortedSections[Index]);
    }
    return Text;
}

/**
 * Failure boundaries for automatic resolution (Issue #22). A single-region
 * whole Driver is the legacy garment-only contract: metrics alone judge it,
 * so this gate never runs on that passthrough. Duplicated candidate garments
 * and foreign geometry welded into garment surfaces both inflate the selected
 * source mass far beyond the Preview garment area, so one numeric boundary
 * rejects both deterministically; the message names both remedies.
 */
// ponytail: fixed 1.7 ceiling between anchors (legit fixture measures ~1.42,
// whole duplicated garments measure 2.0+); refine from the recorded #22
// external-corpus rows if a production revision lands nearby.
constexpr double MaxDriverToPreviewTriangleRatio = 1.7;
/** Preview triangle counts below this floor are exempt from mass accounting. */
constexpr int32 MinTrianglesForMassAccounting = 8;

/** Corner-sample share two selected regions must match within proximity to be twins. */
constexpr double TwinSurfaceAgreementFraction = 0.98;

/** Twin-proximity cut as a fraction of the Preview scale. */
constexpr double TwinProximityFraction = 0.02;

FString NormalizeMaterialEvidenceName(const FName Name)
{
    if (Name.IsNone())
    {
        return FString();
    }
    FString Normalized = Name.ToString().ToLower();
    int32 SuffixStart = Normalized.Len();
    while (SuffixStart > 0 && FChar::IsDigit(Normalized[SuffixStart - 1]))
    {
        --SuffixStart;
    }
    if (SuffixStart < Normalized.Len()
        && SuffixStart > 0
        && Normalized[SuffixStart - 1] == TEXT('_'))
    {
        Normalized.LeftInline(SuffixStart - 1);
    }
    return Normalized;
}

bool HasSupportingMaterialEvidence(
    const FSkeletalMaterial& DriverMaterial,
    const TArray<FStaticMaterial>& PreviewMaterials)
{
    const FString DriverImported = NormalizeMaterialEvidenceName(
        DriverMaterial.ImportedMaterialSlotName);
    const FString DriverDisplayed = NormalizeMaterialEvidenceName(
        DriverMaterial.MaterialSlotName);
    for (const FStaticMaterial& PreviewMaterial : PreviewMaterials)
    {
        if (DriverMaterial.MaterialInterface
            && DriverMaterial.MaterialInterface == PreviewMaterial.MaterialInterface)
        {
            return true;
        }
        const FString PreviewImported = NormalizeMaterialEvidenceName(
            PreviewMaterial.ImportedMaterialSlotName);
        const FString PreviewDisplayed = NormalizeMaterialEvidenceName(
            PreviewMaterial.MaterialSlotName);
        if ((!DriverImported.IsEmpty()
                && (DriverImported == PreviewImported
                    || DriverImported == PreviewDisplayed))
            || (!DriverDisplayed.IsEmpty()
                && (DriverDisplayed == PreviewImported
                    || DriverDisplayed == PreviewDisplayed)))
        {
            return true;
        }
    }
    return false;
}

double SurfaceAgreementShare(
    const FDynamicMesh3& From,
    const FDynamicMesh3& To,
    const double ProximitySquared)
{
    if (From.VertexCount() == 0)
    {
        return 0.0;
    }
    const FDynamicMeshAABBTree3 ToSpatial(&To, true);
    int32 NearCount = 0;
    for (const int32 VertexID : From.VertexIndicesItr())
    {
        double DistanceSquared = 0.0;
        ToSpatial.FindNearestTriangle(From.GetVertex(VertexID), DistanceSquared);
        NearCount += DistanceSquared <= ProximitySquared ? 1 : 0;
    }
    return static_cast<double>(NearCount) / From.VertexCount();
}

bool FindAmbiguousTwinRegion(
    const FDynamicMesh3& Driver,
    const FMeshConnectedComponents& Components,
    const TArray<bool>& RegionSelected,
    const FDynamicMeshMaterialAttribute* MaterialIDs,
    const double Scale,
    FString& OutError)
{
    const double Proximity = TwinProximityFraction * Scale;
    const double ProximitySquared = FMath::Square(Proximity);
    TArray<FAxisAlignedBox3d> Bounds;
    Bounds.Reserve(Components.Components.Num());
    for (const FMeshConnectedComponents::FComponent& Component : Components.Components)
    {
        FAxisAlignedBox3d ComponentBounds = FAxisAlignedBox3d::Empty();
        for (const int32 TriangleID : Component.Indices)
        {
            const FIndex3i Triangle = Driver.GetTriangle(TriangleID);
            ComponentBounds.Contain(Driver.GetVertex(Triangle.A));
            ComponentBounds.Contain(Driver.GetVertex(Triangle.B));
            ComponentBounds.Contain(Driver.GetVertex(Triangle.C));
        }
        Bounds.Add(ComponentBounds);
    }
    const FVector3d Margin(Proximity, Proximity, Proximity);
    for (int32 First = 0; First < Components.Components.Num(); ++First)
    {
        for (int32 Second = First + 1;
            Second < Components.Components.Num(); ++Second)
        {
            // Ambiguity is an equally plausible alternative to the selected
            // source. Two selected pieces are parts of the same resolved
            // garment; source-mass validation handles duplicated selected mass.
            if (RegionSelected[First] == RegionSelected[Second])
            {
                continue;
            }
            if (!FAxisAlignedBox3d(Bounds[First].Min - Margin, Bounds[First].Max + Margin)
                    .Intersects(Bounds[Second]))
            {
                continue;
            }
            const FDynamicMesh3 FirstMesh = ExtractRegionGeometry(
                Driver, Components.Components[First].Indices);
            const FDynamicMesh3 SecondMesh = ExtractRegionGeometry(
                Driver, Components.Components[Second].Indices);
            if (SurfaceAgreementShare(FirstMesh, SecondMesh, ProximitySquared)
                    < TwinSurfaceAgreementFraction
                || SurfaceAgreementShare(SecondMesh, FirstMesh, ProximitySquared)
                    < TwinSurfaceAgreementFraction)
            {
                continue;
            }
            OutError = FString::Printf(
                TEXT("Automatic garment resolution found two indistinguishable source regions [%s] and [%s]. "
                    "Their surfaces coincide within %.4f of the Preview scale, so the choice is ambiguous. Remove "
                    "one duplicate or pin the intended imported slots with Driver Garment Slot Override."),
                *DescribeConnectedRegion(
                    Driver, Components.Components[First], MaterialIDs),
                *DescribeConnectedRegion(
                    Driver, Components.Components[Second], MaterialIDs),
                TwinProximityFraction);
            return true;
        }
    }
    return false;
}

void SelectAutomaticGarmentRegions(
    const FDynamicMesh3& Driver,
    const USkeletalMesh& DriverAsset,
    const FDynamicMesh3& Preview,
    const UStaticMesh& PreviewAsset,
    const TArray<int32>& PreviewVertexIDs,
    const FMeshConnectedComponents& Components,
    const FDynamicMeshMaterialAttribute* DriverMaterialIDs,
    const double AgreementRadiusSquared,
    TArray<bool>& OutRegionSelected,
    int32& OutAgreeingCount,
    bool& OutUsedMaterialEvidence)
{
    TBitArray<> SupportedSlots(false, DriverAsset.GetMaterials().Num());
    for (int32 SlotIndex = 0; SlotIndex < DriverAsset.GetMaterials().Num(); ++SlotIndex)
    {
        SupportedSlots[SlotIndex] = HasSupportingMaterialEvidence(
            DriverAsset.GetMaterials()[SlotIndex], PreviewAsset.GetStaticMaterials());
    }

    TArray<int32> RegionOfTriangle;
    RegionOfTriangle.Init(INDEX_NONE, Driver.MaxTriangleID());
    for (int32 ComponentIndex = 0;
        ComponentIndex < Components.Components.Num(); ++ComponentIndex)
    {
        for (const int32 TriangleID : Components.Components[ComponentIndex].Indices)
        {
            RegionOfTriangle[TriangleID] = ComponentIndex;
        }
    }

    TArray<int32> CompOwnedHits;
    CompOwnedHits.Init(0, Components.Components.Num());
    TArray<bool> CompMaterialSupported;
    CompMaterialSupported.Init(false, Components.Components.Num());
    int32 MaterialSupportedRegionCount = 0;
    for (int32 ComponentIndex = 0;
        ComponentIndex < Components.Components.Num(); ++ComponentIndex)
    {
        for (const int32 TriangleID : Components.Components[ComponentIndex].Indices)
        {
            const int32 MaterialID = DriverMaterialIDs
                ? DriverMaterialIDs->GetValue(TriangleID)
                : Driver.GetTriangleGroup(TriangleID);
            if (SupportedSlots.IsValidIndex(MaterialID) && SupportedSlots[MaterialID])
            {
                CompMaterialSupported[ComponentIndex] = true;
                ++MaterialSupportedRegionCount;
                break;
            }
        }
    }

    const FDynamicMeshAABBTree3 DriverSpatial(&Driver, true);
    OutAgreeingCount = 0;
    for (const int32 PreviewVertexID : PreviewVertexIDs)
    {
        double DistanceSquared = 0.0;
        const int32 TriangleID = DriverSpatial.FindNearestTriangle(
            Preview.GetVertex(PreviewVertexID), DistanceSquared);
        if (TriangleID != INDEX_NONE && DistanceSquared <= AgreementRadiusSquared)
        {
            ++OutAgreeingCount;
            ++CompOwnedHits[RegionOfTriangle[TriangleID]];
        }
    }

    const bool bMaterialEvidenceCanDiscriminate = MaterialSupportedRegionCount > 0
        && MaterialSupportedRegionCount < Components.Components.Num();
    TBitArray<> MaterialEvidenceCoverage(false, PreviewVertexIDs.Num());
    TArray<TBitArray<>> CompCovered;
    CompCovered.Init(TBitArray<>(false, PreviewVertexIDs.Num()),
        Components.Components.Num());
    for (int32 ComponentIndex = 0;
        ComponentIndex < Components.Components.Num(); ++ComponentIndex)
    {
        const FMeshConnectedComponents::FComponent& Component =
            Components.Components[ComponentIndex];
        if (Component.Indices.IsEmpty()
            || (CompOwnedHits[ComponentIndex] == 0
                && !(bMaterialEvidenceCanDiscriminate
                    && CompMaterialSupported[ComponentIndex])))
        {
            continue;
        }
        const FDynamicMesh3 ComponentMesh =
            ExtractRegionGeometry(Driver, Component.Indices);
        const FDynamicMeshAABBTree3 ComponentSpatial(&ComponentMesh, true);
        for (int32 Sample = 0; Sample < PreviewVertexIDs.Num(); ++Sample)
        {
            double DistanceSquared = 0.0;
            ComponentSpatial.FindNearestTriangle(
                Preview.GetVertex(PreviewVertexIDs[Sample]), DistanceSquared);
            if (DistanceSquared <= AgreementRadiusSquared)
            {
                CompCovered[ComponentIndex][Sample] = true;
                if (CompMaterialSupported[ComponentIndex])
                {
                    MaterialEvidenceCoverage[Sample] = true;
                }
            }
        }
    }

    OutUsedMaterialEvidence = bMaterialEvidenceCanDiscriminate;
    for (int32 Sample = 0; Sample < PreviewVertexIDs.Num(); ++Sample)
    {
        OutUsedMaterialEvidence &= MaterialEvidenceCoverage[Sample];
    }
    if (OutUsedMaterialEvidence)
    {
        OutAgreeingCount = PreviewVertexIDs.Num();
        for (int32 ComponentIndex = 0;
            ComponentIndex < Components.Components.Num(); ++ComponentIndex)
        {
            CompOwnedHits[ComponentIndex] =
                CompMaterialSupported[ComponentIndex]
                && CompCovered[ComponentIndex].Contains(true)
                    ? 1
                    : 0;
        }
    }

    OutRegionSelected.Init(false, Components.Components.Num());
    for (int32 ComponentIndex = 0;
        ComponentIndex < Components.Components.Num(); ++ComponentIndex)
    {
        OutRegionSelected[ComponentIndex] = CompOwnedHits[ComponentIndex] > 0;
    }
}

/**
 * Resolve which connected regions of the full-character Driver LOD0 correspond
 * to the garment-only Preview Static Mesh. Geometry connectivity and spatial
 * agreement remain authoritative: every Preview vertex belongs to
 * the region containing its nearest Driver triangle, so ownership is unique
 * and the resolved garment is exactly the set of edge-connected regions that
 * owns at least one spatially agreeing Preview vertex. Two structural failure
 * boundaries guard the automatic result (Issue #22): a selected region must be
 * predominantly reachable from the Preview surface (welded body-and-garment
 * mixes are rejected), and two regions covering nearly the same Preview surface
 * are reported as a deterministic ambiguity instead of an arbitrary choice.
 * Imported slot names and assigned material assets may narrow those geometric
 * candidates when their union covers the whole Preview. They are supporting
 * evidence only: incomplete or replaced material evidence falls back to
 * geometry, and material agreement alone never bypasses coverage validation.
 * A garment-only Driver resolves to its whole single region unchanged. The
 * manual Driver Garment Slot Override path below bypasses this search but
 * keeps the same geometric validation.
 */
bool ResolveDriverGarmentSurface(
    const FDynamicMesh3& Driver,
    const USkeletalMesh& DriverAsset,
    const FDynamicMesh3& Preview,
    const UStaticMesh& PreviewAsset,
    double MisalignedBound,
    FMtoUDriverGarmentSurfaceResult& Out,
    FString& OutError)
{
    // Admission gate: a Preview vertex only agrees when its nearest Driver
    // point lies within GarmentAgreementRadiusFraction of the Preview scale.
    // MisalignedBound is quoted in the failure text so artists see the
    // calibrated distance bound alongside the measured coverage gap; Issue
    // #22 recalibrates both boundaries for the full-character corpus.
    const double Scale = BeginGarmentSurfaceResolution(Driver, Preview, OutError);
    if (Scale <= UE_SMALL_NUMBER)
    {
        return false;
    }
    const double AgreementRadiusSquared =
        FMath::Square(GarmentAgreementRadiusFraction * Scale);

    TArray<int32> PreviewVertexIDs;
    for (const int32 VertexID : Preview.VertexIndicesItr())
    {
        PreviewVertexIDs.Add(VertexID);
    }
    if (PreviewVertexIDs.Num() == 0)
    {
        OutError = TEXT("Preview Static Mesh has no LOD0 geometry to match against the Driver.");
        return false;
    }

    FMeshConnectedComponents Components(&Driver);
    Components.FindConnectedTriangles();

    // Per-triangle material-section identity. The Geometry Script conversion
    // leaves polygon groups empty, so section evidence comes from the
    // DynamicMesh material-ID attribute (the same signal the manual override
    // path resolves imported slot names against).
    const FDynamicMeshMaterialAttribute* DriverMaterialIDs =
        Driver.Attributes() ? Driver.Attributes()->GetMaterialID() : nullptr;

    TArray<bool> RegionSelected;
    int32 AgreeingCount = 0;
    bool bUsedMaterialEvidence = false;
    SelectAutomaticGarmentRegions(
        Driver, DriverAsset, Preview, PreviewAsset, PreviewVertexIDs,
        Components, DriverMaterialIDs, AgreementRadiusSquared,
        RegionSelected, AgreeingCount, bUsedMaterialEvidence);
    for (int32 ComponentIndex = 0;
        ComponentIndex < Components.Components.Num(); ++ComponentIndex)
    {
        if (RegionSelected[ComponentIndex])
        {
            ++Out.RegionCount;
            Out.TriangleCount += Components.Components[ComponentIndex].Indices.Num();
        }
    }

    // Identify each selected region by size and touched material sections so
    // diagnostics expose what Auto chose without persisting anything.
    for (int32 RegionIndex = 0; RegionIndex < RegionSelected.Num(); ++RegionIndex)
    {
        if (!RegionSelected[RegionIndex])
        {
            continue;
        }
        Out.RegionSummary += FString::Printf(
            TEXT("%s%s"),
            Out.RegionSummary.IsEmpty() ? TEXT("") : TEXT(", "),
            *DescribeConnectedRegion(Driver,
                Components.Components[RegionIndex], DriverMaterialIDs));
    }

    Out.MatchedPreviewCoverage =
        static_cast<double>(AgreeingCount) / PreviewVertexIDs.Num();
    if (AgreeingCount < PreviewVertexIDs.Num())
    {
        OutError = FString::Printf(
            TEXT("Auto garment resolution found no unique separable Driver surface covering %d of %d Preview vertices "
                "within %.4f of the Preview scale or the calibrated misalignment bound %.4f. The unmatched surface is "
                "misaligned (check reference pose, origin, units, and asset-local import space), or the garment is "
                "welded to other character surfaces without a distinct source region."),
            PreviewVertexIDs.Num() - AgreeingCount,
            PreviewVertexIDs.Num(),
            GarmentAgreementRadiusFraction,
            MisalignedBound);
        return false;
    }

    // Issue #22 failure boundaries for automatic resolution. A single-region
    // whole Driver is the legacy garment-only contract: metrics alone judge
    // it, so these gates never run on that passthrough.
    if (Components.Components.Num() > 1
        && PreviewVertexIDs.Num() >= MinTrianglesForMassAccounting)
    {
        // Mass bound: duplicated copies or proximity-pulled foreign geometry
        // inflate the selected source far beyond the Preview garment.
        const int32 PreviewTriangleCount = Preview.TriangleCount();
        const double MaxSelectedTriangles =
            static_cast<double>(PreviewTriangleCount)
            * MaxDriverToPreviewTriangleRatio;
        if (Out.TriangleCount > MaxSelectedTriangles)
        {
            OutError = FString::Printf(
                TEXT("Automatic garment resolution failed the source-mass boundary: the resolved surface holds %d "
                    "Driver LOD0 triangles for a Preview garment of %d triangles (%.2fx allowed %.2f); duplicated "
                    "garment copies are indistinguishable sources (ambiguous) or foreign geometry is welded into "
                    "garment source/material sections, and automatic resolution cannot separate them safely. Remove "
                    "duplicates or split the garment into its own material section in the source FBX, or pin an "
                    "explicit selection with the manual Driver Garment Slot Override."),
                Out.TriangleCount,
                PreviewTriangleCount,
                static_cast<double>(Out.TriangleCount) / PreviewTriangleCount,
                MaxDriverToPreviewTriangleRatio);
            return false;
        }

        if (FindAmbiguousTwinRegion(
                Driver, Components, RegionSelected, DriverMaterialIDs, Scale, OutError))
        {
            return false;
        }
    }
    TArray<int32> UnselectedTriangles;
    for (int32 RegionIndex = 0; RegionIndex < RegionSelected.Num(); ++RegionIndex)
    {
        if (!RegionSelected[RegionIndex])
        {
            UnselectedTriangles.Append(Components.Components[RegionIndex].Indices);
        }
    }
    FinalizeResolvedGarmentSurface(Driver, UnselectedTriangles, Out);
    if (!CollectMaterialSlotIndices(
            Driver, Out.Surface, DriverAsset, Out.MaterialSlotIndices, OutError))
    {
        return false;
    }
    if (bUsedMaterialEvidence)
    {
        Out.RegionSummary += TEXT("; material evidence narrowed Auto candidates");
    }
    return true;
}

/**
 * Resolve the Driver garment surface from the Binding's advanced manual
 * material-slot override. Each entry must name exactly one imported Driver
 * material slot by its stable imported slot identity; missing, duplicated, or
 * no-longer-unique identities are hard preflight errors that never fall back
 * to Auto. The selected slots pick whole LOD0 sections, and the manual result
 * must pass the same spatial agreement gate as Auto: every Preview vertex has
 * to lie within the agreement radius of the resolved surface alone, so a
 * wrong or partial selection fails transactionally instead of transferring
 * against unrelated body surfaces.
 */
bool ResolveDriverGarmentSurfaceFromSlots(
    const FDynamicMesh3& Driver,
    const USkeletalMesh& DriverAsset,
    const TArray<FName>& Override,
    const FDynamicMesh3& Preview,
    double MisalignedBound,
    FMtoUDriverGarmentSurfaceResult& Out,
    FString& OutError)
{
    const double Scale = BeginGarmentSurfaceResolution(Driver, Preview, OutError);
    if (Scale <= UE_SMALL_NUMBER)
    {
        return false;
    }

    const TArray<FSkeletalMaterial>& Slots = DriverAsset.GetMaterials();
    TArray<int32> MatchedSlots;
    TSet<int32> UniqueMatchedSlots;
    if (!MatchManualOverrideSlots(
            Slots, Override, MatchedSlots, UniqueMatchedSlots, OutError))
    {
        return false;
    }

    // Select whole LOD0 sections through the shared per-triangle resolver. It
    // checks the stored MeshDescription polygon-group identity and the current
    // LOD triangle-group metadata before either signal can select a slot.
    const FDynamicMeshMaterialAttribute* MaterialIDs = Driver.Attributes()
        ? Driver.Attributes()->GetMaterialID()
        : nullptr;
    FDriverMaterialSlotMaps SlotMaps;
    if (!MaterialIDs || !BuildDriverMaterialSlotMaps(DriverAsset, SlotMaps))
    {
        OutError = TEXT("Driver LOD0 source data has no imported material-slot identities to match the manual "
            "override against.");
        return false;
    }
    if (!SlotMaps.bHasTriangleGroupMapping)
    {
        OutError = TEXT("Driver LOD0 source data has no current material-slot metadata to match the manual "
            "override against.");
        return false;
    }
    const FDriverMaterialSlotMetadata Metadata =
        AnalyzeDriverMaterialSlotMetadata(
            Driver, Slots, MaterialIDs, SlotMaps);

    const auto IsMatchedSlot = [&](int32 SlotIndex)
    {
        return SlotIndex >= 0 && SlotIndex < Slots.Num()
            && UniqueMatchedSlots.Contains(SlotIndex);
    };
    const FDriverMaterialSlotResolver SlotResolver{
        Driver,
        Slots,
        MaterialIDs,
        SlotMaps.SlotByPolygonGroup,
        SlotMaps.SlotByTriangleGroup};
    TBitArray<> SlotSelected(false, Slots.Num());
    TArray<int32> UnselectedTriangles;
    for (const int32 TriangleID : Driver.TriangleIndicesItr())
    {
        int32 SlotIndex = INDEX_NONE;
        const EDriverMaterialSlotResolutionResult Resolution =
            SlotResolver.Resolve(
                TriangleID,
                EDriverMaterialSlotResolutionMode::Manual,
                Metadata.bUseTriangleGroupMetadata,
                Metadata.bCollapsedMaterialMetadata,
                SlotIndex,
                OutError);
        if (Resolution == EDriverMaterialSlotResolutionResult::Conflict)
        {
            return false;
        }
        if (Resolution == EDriverMaterialSlotResolutionResult::Resolved
            && IsMatchedSlot(SlotIndex))
        {
            SlotSelected[SlotIndex] = true;
            continue;
        }
        UnselectedTriangles.Add(TriangleID);
    }
    for (const int32 SlotIndex : MatchedSlots)
    {
        if (!SlotSelected[SlotIndex])
        {
            OutError = FString::Printf(
                TEXT("Manual garment override names Driver material slot %s, which has no LOD0 triangles in the "
                    "current import; the persisted identity is stale and refresh stays blocked instead of silently "
                    "returning to automatic resolution."),
                *DescribeDriverSlot(Slots, SlotIndex));
            return false;
        }
    }
    Out.TriangleCount = Driver.TriangleCount() - UnselectedTriangles.Num();
    FinalizeResolvedGarmentSurface(Driver, UnselectedTriangles, Out);
    Out.MaterialSlotIndices = MatchedSlots;
    Out.MaterialSlotIndices.Sort();

    // Manual selections never skip the same whole-Preview geometry gate.
    return ValidateManualGarmentCoverage(
        Preview, Scale, MisalignedBound, Slots, MatchedSlots, Out, OutError);
}
}

FMtoUDriverGarmentSurfaceResult MtoUResolveDriverGarmentSurface(
    const FDynamicMesh3& Driver,
    const FDynamicMesh3& Preview,
    const UMtoULiveLinkBinding& Binding,
    double MisalignedNormalizedDistanceAverage)
{
    FMtoUDriverGarmentSurfaceResult Result;
    // The module owns the Auto/Manual choice; the selection identity stays on
    // failed results as failure evidence.
    Result.bManualSource = Binding.DriverGarmentSlotOverride.Num() > 0;
    if (!Binding.SkeletalMesh || !Binding.PreviewStaticMesh)
    {
        Result.Diagnostics = TEXT(
            "Driver Skeletal Mesh and Preview Static Mesh are required for garment-surface resolution.");
        return Result;
    }
    FString Error;
    const bool bResolved = Result.bManualSource
        ? ResolveDriverGarmentSurfaceFromSlots(
            Driver,
            *Binding.SkeletalMesh,
            Binding.DriverGarmentSlotOverride,
            Preview,
            MisalignedNormalizedDistanceAverage,
            Result,
            Error)
        : ResolveDriverGarmentSurface(
            Driver,
            *Binding.SkeletalMesh,
            Preview,
            *Binding.PreviewStaticMesh,
            MisalignedNormalizedDistanceAverage,
            Result,
            Error);
    Result.bSucceeded = bResolved;
    Result.Diagnostics = MoveTemp(Error);
    if (!bResolved)
    {
        // A failed result contains no usable surface; counts, coverage,
        // identity, and the region summary remain as failure evidence.
        Result.Surface = FDynamicMesh3();
    }
    return Result;
}
