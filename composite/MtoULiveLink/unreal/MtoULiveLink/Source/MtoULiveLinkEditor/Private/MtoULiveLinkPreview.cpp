#include "MtoULiveLinkPreview.h"

#include "MtoULiveLinkBinding.h"

#include "Animation/MorphTarget.h"
#include "Distance/DistPoint3Triangle3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicBoneAttribute.h"
#include "DynamicMesh/DynamicVertexSkinWeightsAttribute.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "MeshDescription.h"
#include "Operations/TransferBoneWeights.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "Selections/MeshConnectedComponents.h"
#include "StaticMeshAttributes.h"
#include "UDynamicMesh.h"

using namespace UE::Geometry;

EMtoUPreviewQuality MtoUEvaluatePreviewQuality(
    const double InpaintLowConfidenceRatio,
    const double NormalizedSurfaceDistanceAverage,
    const FMtoUPreviewQualityThresholds& Thresholds,
    FString& OutReason)
{
    if (NormalizedSurfaceDistanceAverage > Thresholds.MisalignedNormalizedDistanceAverage)
    {
        OutReason = FString::Printf(
            TEXT("misaligned inputs: normalized surface distance average %.4f exceeds %.4f"),
            NormalizedSurfaceDistanceAverage,
            Thresholds.MisalignedNormalizedDistanceAverage);
        return EMtoUPreviewQuality::Error;
    }
    if (InpaintLowConfidenceRatio > Thresholds.MaxWarningInpaintRatio)
    {
        OutReason = FString::Printf(
            TEXT("unsafe weight transfer: low-confidence ratio %.4f exceeds %.4f"),
            InpaintLowConfidenceRatio,
            Thresholds.MaxWarningInpaintRatio);
        return EMtoUPreviewQuality::Error;
    }
    if (InpaintLowConfidenceRatio > Thresholds.MaxReadyInpaintRatio)
    {
        OutReason = FString::Printf(
            TEXT("low-confidence weight transfer: ratio %.4f is within the calibrated warning band (%.4f, %.4f]"),
            InpaintLowConfidenceRatio,
            Thresholds.MaxReadyInpaintRatio,
            Thresholds.MaxWarningInpaintRatio);
        return EMtoUPreviewQuality::Warning;
    }
    OutReason = FString::Printf(
        TEXT("within the calibrated envelope: low-confidence ratio %.4f <= %.4f"),
        InpaintLowConfidenceRatio,
        Thresholds.MaxReadyInpaintRatio);
    return EMtoUPreviewQuality::Ready;
}

namespace
{
constexpr double InpaintSearchRadiusFraction = 0.05;
constexpr double InpaintNormalThresholdRadians = UE_DOUBLE_PI / 6.0;

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

struct FMorphCorrespondence
{
    FIndex3i DriverTriangle = FIndex3i::Invalid();
    FVector3d Barycentric = FVector3d::Zero();
};

struct FMtoUSurfaceDistanceStats
{
    double Min = 0.0;
    double Max = 0.0;
    double Average = 0.0;
    double Rms = 0.0;
};

void AccumulateDistanceSamples(
    const FDynamicMesh3& From,
    const FDynamicMeshAABBTree3& ToSpatial,
    double& Sum,
    double& SumSquares,
    double& Min,
    double& Max,
    int32& Count)
{
    for (const int32 VertexID : From.VertexIndicesItr())
    {
        double DistanceSquared = 0.0;
        ToSpatial.FindNearestTriangle(From.GetVertex(VertexID), DistanceSquared);
        const double Distance = FMath::Sqrt(FMath::Max(0.0, DistanceSquared));
        Sum += Distance;
        SumSquares += Distance * Distance;
        Min = FMath::Min(Min, Distance);
        Max = FMath::Max(Max, Distance);
        ++Count;
    }
}

bool MeasureSurfaceDistances(
    const FDynamicMesh3& Driver,
    const FDynamicMesh3& Preview,
    const double NormalizeLength,
    FMtoUSurfaceDistanceStats& OutStats,
    FString& OutError)
{
    if (NormalizeLength <= UE_SMALL_NUMBER)
    {
        OutError = TEXT("Driver bounding box has no usable size; scale is invalid.");
        return false;
    }
    const FDynamicMeshAABBTree3 DriverSpatial(&Driver, true);
    const FDynamicMeshAABBTree3 PreviewSpatial(&Preview, true);

    double Sum = 0.0;
    double SumSquares = 0.0;
    double Min = TNumericLimits<double>::Max();
    double Max = 0.0;
    int32 Count = 0;
    AccumulateDistanceSamples(Preview, DriverSpatial, Sum, SumSquares, Min, Max, Count);
    AccumulateDistanceSamples(Driver, PreviewSpatial, Sum, SumSquares, Min, Max, Count);
    if (Count == 0)
    {
        OutError = TEXT("Symmetric surface distance requires non-empty Driver and Preview geometry.");
        return false;
    }

    OutStats.Min = Min / NormalizeLength;
    OutStats.Max = Max / NormalizeLength;
    OutStats.Average = Sum / Count / NormalizeLength;
    OutStats.Rms = FMath::Sqrt(SumSquares / Count) / NormalizeLength;
    return true;
}

/**
 * Resolved Driver garment surface: the Driver LOD0 triangles selected as the
 * source for one Preview refresh. The filtered mesh keeps the original Driver
 * vertex IDs so Morph deltas and import-vertex correspondence stay lossless;
 * vertices outside the resolved surface are removed instead of compacted.
 */
struct FMtoUDriverGarmentResolution
{
    FDynamicMesh3 Surface;
    int32 RegionCount = 0;
    int32 TriangleCount = 0;
    double MatchedPreviewCoverage = 1.0;
    /** Compact identification of each selected region for diagnostics. */
    FString RegionSummary;
};

FName GetDriverSlotIdentity(const FSkeletalMaterial& Slot);

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
    FMtoUDriverGarmentResolution& Out,
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
        IdentitySummaries.Add(GetDriverSlotIdentity(Slots[SlotIndex]).ToString());
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

/**
 * Shared removal tail: keep only the selected triangles while removing their
 * isolated vertices, so every resolved-surface vertex ID stays identical to
 * its original Driver LOD0 import vertex.
 */
void FinalizeResolvedGarmentSurface(
    const FDynamicMesh3& Driver,
    const TArray<int32>& UnselectedTriangles,
    FMtoUDriverGarmentResolution& Out)
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

/** Stable imported identity of one Driver material slot for override matching. */
FName GetDriverSlotIdentity(const FSkeletalMaterial& Slot)
{
    return Slot.ImportedMaterialSlotName != NAME_None
        ? Slot.ImportedMaterialSlotName
        : Slot.MaterialSlotName;
}

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
 * A garment-only Driver resolves to its whole single region unchanged. The Binding's advanced
 * manual slot override (ResolveDriverGarmentSurfaceFromSlots) bypasses this
 * search but keeps the same geometric validation.
 */
bool ResolveDriverGarmentSurface(
    const FDynamicMesh3& Driver,
    const USkeletalMesh& DriverAsset,
    const FDynamicMesh3& Preview,
    const UStaticMesh& PreviewAsset,
    double MisalignedBound,
    FMtoUDriverGarmentResolution& Out,
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
    USkeletalMesh& DriverAsset,
    const TArray<FName>& Override,
    const FDynamicMesh3& Preview,
    double MisalignedBound,
    FMtoUDriverGarmentResolution& Out,
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

    // Select whole LOD0 sections. Two engine signals identify a converted
    // triangle's material slot: the stored MeshDescription polygon-group
    // imported slot name reached through the per-triangle material ID, and the
    // triangle-group layer that skeletal builds route sections by. Both stay
    // name- or slot-index based; no transient section arithmetic is persisted.
    FMeshDescription* Description = DriverAsset.GetMeshDescription(0);
    const bool bHasPolygonGroupNames = Description
        && Description->PolygonGroupAttributes().HasAttribute(
            MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
    const FDynamicMeshMaterialAttribute* MaterialIDs = Driver.Attributes()
        ? Driver.Attributes()->GetMaterialID()
        : nullptr;
    if (!bHasPolygonGroupNames || !MaterialIDs)
    {
        OutError = TEXT("Driver LOD0 source data has no imported material-slot identities to match the manual "
            "override against.");
        return false;
    }

    // Resolve each polygon-group ordinal to its unique Driver material slot;
    // an imported name matching several slots stays unresolved on purpose.
    constexpr int32 AmbiguousSlot = -2;
    FStaticMeshAttributes DescriptionAttributes(*Description);
    const TPolygonGroupAttributesConstRef<FName> GroupSlotNames =
        DescriptionAttributes.GetPolygonGroupMaterialSlotNames();
    int32 MaxOrdinal = INDEX_NONE;
    for (const FPolygonGroupID GroupID : Description->PolygonGroups().GetElementIDs())
    {
        MaxOrdinal = FMath::Max(MaxOrdinal, GroupID.GetValue());
    }
    TArray<int32> SlotByOrdinal;
    SlotByOrdinal.Init(INDEX_NONE, MaxOrdinal + 1);
    for (const FPolygonGroupID GroupID : Description->PolygonGroups().GetElementIDs())
    {
        const FName ImportedName = GroupSlotNames[GroupID];
        for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
        {
            if (GetDriverSlotIdentity(Slots[SlotIndex]) != ImportedName)
            {
                continue;
            }
            int32& Mapped = SlotByOrdinal[GroupID.GetValue()];
            Mapped = Mapped == INDEX_NONE ? SlotIndex : AmbiguousSlot;
        }
    }

    const auto IsMatchedSlot = [&](int32 SlotIndex)
    {
        return SlotIndex >= 0 && SlotIndex < Slots.Num()
            && UniqueMatchedSlots.Contains(SlotIndex);
    };
    TBitArray<> SlotSelected(false, Slots.Num());
    TArray<int32> UnselectedTriangles;
    for (const int32 TriangleID : Driver.TriangleIndicesItr())
    {
        const int32 Ordinal = MaterialIDs->GetValue(TriangleID);
        const int32 ByName = SlotByOrdinal.IsValidIndex(Ordinal)
            ? SlotByOrdinal[Ordinal]
            : INDEX_NONE;
        if (IsMatchedSlot(ByName))
        {
            SlotSelected[ByName] = true;
            continue;
        }
        // Descriptions whose polygon-group identities collapsed to one group
        // still record the routed slot in the triangle-group layer, which is
        // what skeletal section builds follow.
        const int32 ByGroup = Driver.GetTriangleGroup(TriangleID);
        if (IsMatchedSlot(ByGroup))
        {
            SlotSelected[ByGroup] = true;
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

    // Manual selections never skip the same whole-Preview geometry gate.
    return ValidateManualGarmentCoverage(
        Preview, Scale, MisalignedBound, Slots, MatchedSlots, Out, OutError);
}

void ObserveStage(
    FMtoUPreviewPreparationResult& Result,
    EMtoUPreviewBuildStage Stage,
    const FMtoUPreviewStageCallback& OnStage)
{
    if (OnStage)
    {
        OnStage(Stage);
    }
    Result.FailureStage = Stage;
}

bool ValidateSourceInfluences(const FDynamicMesh3& Mesh, const FReferenceSkeleton& Skeleton,
    FString& OutError)
{
    if (!Mesh.HasAttributes() || !Mesh.Attributes()->HasBones())
    {
        OutError = TEXT("Driver LOD0 source data has no bone hierarchy.");
        return false;
    }
    const FDynamicMeshVertexSkinWeightsAttribute* SkinWeights =
        Mesh.Attributes()->GetSkinWeightsAttribute(
            FSkeletalMeshAttributes::DefaultSkinWeightProfileName);
    if (!SkinWeights)
    {
        OutError = TEXT("Driver LOD0 source data has no default skin weights.");
        return false;
    }

    const TArray<FName>& BoneNames = Mesh.Attributes()->GetBoneNames()->GetAttribValues();
    for (const int32 VertexID : Mesh.VertexIndicesItr())
    {
        UE::AnimationCore::FBoneWeights Weights;
        SkinWeights->GetValue(VertexID, Weights);
        if (Weights.Num() == 0)
        {
            OutError = FString::Printf(TEXT("Driver vertex %d has no skin influence."), VertexID);
            return false;
        }
        for (int32 WeightIndex = 0; WeightIndex < Weights.Num(); ++WeightIndex)
        {
            const int32 BoneIndex = Weights[WeightIndex].GetBoneIndex();
            if (!BoneNames.IsValidIndex(BoneIndex)
                || Skeleton.FindBoneIndex(BoneNames[BoneIndex]) == INDEX_NONE)
            {
                const FString BoneName = BoneNames.IsValidIndex(BoneIndex)
                    ? BoneNames[BoneIndex].ToString()
                    : FString::Printf(TEXT("index %d"), BoneIndex);
                OutError = FString::Printf(
                    TEXT("Driver vertex %d uses source influence bone '%s', which is absent from the target skeleton."),
                    VertexID, *BoneName);
                return false;
            }
        }
    }
    return true;
}

bool TransferWeights(
    const FDynamicMesh3& Source,
    FDynamicMesh3& Target,
    FTransferBoneWeights::ETransferBoneWeightsMethod Method,
    double& OutMilliseconds,
    TArray<bool>& OutMatchedVertices)
{
    if (!Target.HasAttributes())
    {
        Target.EnableAttributes();
    }
    Target.Attributes()->CopyBoneAttributes(*Source.Attributes());

    FTransferBoneWeights Transfer(
        &Source, FSkeletalMeshAttributes::DefaultSkinWeightProfileName);
    Transfer.TransferMethod = Method;
    Transfer.bUseParallel = true;
    if (Method == FTransferBoneWeights::ETransferBoneWeightsMethod::InpaintWeights)
    {
        Transfer.SearchRadius = InpaintSearchRadiusFraction * Target.GetBounds().DiagonalLength();
        Transfer.NormalThreshold = InpaintNormalThresholdRadians;
        Transfer.LayeredMeshSupport = true;
        Transfer.NumSmoothingIterations = 1;
        Transfer.SmoothingStrength = 0.5f;
    }
    if (Transfer.Validate() != EOperationValidationResult::Ok)
    {
        return false;
    }

    const double Start = FPlatformTime::Seconds();
    const bool bSucceeded = Transfer.TransferWeightsToMesh(
        Target, FSkeletalMeshAttributes::DefaultSkinWeightProfileName);
    OutMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
    OutMatchedVertices = MoveTemp(Transfer.MatchedVertices);
    return bSucceeded;
}

bool BuildMorphCorrespondence(
    const FDynamicMesh3& Driver,
    const FDynamicMesh3& Preview,
    TArray<FMorphCorrespondence>& OutCorrespondence,
    FString& OutError)
{
    if (Driver.TriangleCount() == 0 || Preview.VertexCount() == 0)
    {
        OutError = TEXT("Morph projection requires non-empty Driver triangles and Preview vertices.");
        return false;
    }

    const FDynamicMeshAABBTree3 DriverSpatial(&Driver, true);
    OutCorrespondence.SetNum(Preview.MaxVertexID());
    for (const int32 PreviewVertexID : Preview.VertexIndicesItr())
    {
        double DistanceSquared = 0.0;
        const int32 TriangleID = DriverSpatial.FindNearestTriangle(
            Preview.GetVertex(PreviewVertexID), DistanceSquared);
        if (!Driver.IsTriangle(TriangleID))
        {
            OutError = FString::Printf(
                TEXT("No Driver-surface triangle was found for Preview vertex %d."),
                PreviewVertexID);
            return false;
        }

        const FIndex3i Triangle = Driver.GetTriangle(TriangleID);
        FDistPoint3Triangle3d Distance(
            Preview.GetVertex(PreviewVertexID),
            FTriangle3d(
                Driver.GetVertex(Triangle.A),
                Driver.GetVertex(Triangle.B),
                Driver.GetVertex(Triangle.C)));
        Distance.GetSquared();
        OutCorrespondence[PreviewVertexID] = {Triangle, Distance.TriangleBaryCoords};
    }
    return true;
}

bool GeneratePreviewMorphs(
    const USkeletalMesh& DriverAsset,
    const FDynamicMesh3& DriverMesh,
    const UDynamicMesh& PreviewDynamic,
    const TArray<FMorphCorrespondence>& Correspondence,
    USkeletalMesh& Generated,
    int32& OutMorphCount,
    int32& OutSkippedMorphCount,
    int64& OutSparseDeltaCount,
    FString& OutError)
{
    const FSkeletalMeshModel* DriverModel = DriverAsset.GetImportedModel();
    if (!DriverModel || !DriverModel->LODModels.IsValidIndex(0))
    {
        OutError = TEXT("Morph projection requires the public Driver LOD0 imported vertex map.");
        return false;
    }

    const FSkeletalMeshLODModel& DriverLOD = DriverModel->LODModels[0];
    const FDynamicMesh3& PreviewMesh = PreviewDynamic.GetMeshRef();
    for (const TObjectPtr<UMorphTarget>& DriverMorph : DriverAsset.GetMorphTargets())
    {
        if (!DriverMorph || !DriverMorph->HasDataForLOD(0))
        {
            OutError = FString::Printf(
                TEXT("Driver Morph '%s' has no usable LOD0 data."),
                DriverMorph ? *DriverMorph->GetName() : TEXT("<null>"));
            return false;
        }

        TArray<FVector3f> DriverPointDeltas;
        DriverPointDeltas.SetNumZeroed(DriverMesh.MaxVertexID());
        TBitArray<> AssignedDriverPoints(false, DriverMesh.MaxVertexID());
        for (const FMorphTargetDelta& Delta : DriverMorph->GetMorphTargetDeltas(0))
        {
            if (!DriverLOD.MeshToImportVertexMap.IsValidIndex(Delta.SourceIdx))
            {
                OutError = FString::Printf(
                    TEXT("Driver Morph '%s' references invalid LOD0 vertex %u."),
                    *DriverMorph->GetName(), Delta.SourceIdx);
                return false;
            }
            const int32 DriverPointID = DriverLOD.MeshToImportVertexMap[Delta.SourceIdx];
            if (!DriverMesh.IsVertex(DriverPointID))
            {
                // The resolved Driver garment surface removed this vertex, so
                // the delta lies outside the garment and cannot reach the
                // Preview; only deltas on the resolved surface are projected.
                continue;
            }
            if (AssignedDriverPoints[DriverPointID]
                && !DriverPointDeltas[DriverPointID].Equals(Delta.PositionDelta, 1.0e-4f))
            {
                OutError = FString::Printf(
                    TEXT("Driver Morph '%s' has inconsistent position deltas across a source seam."),
                    *DriverMorph->GetName());
                return false;
            }
            AssignedDriverPoints[DriverPointID] = true;
            DriverPointDeltas[DriverPointID] = Delta.PositionDelta;
        }

        TArray<FVector3f> PreviewPointDeltas;
        PreviewPointDeltas.SetNumZeroed(PreviewMesh.MaxVertexID());
        for (const int32 PreviewVertexID : PreviewMesh.VertexIndicesItr())
        {
            if (!Correspondence.IsValidIndex(PreviewVertexID))
            {
                OutError = FString::Printf(
                    TEXT("Morph correspondence is missing Preview vertex %d."), PreviewVertexID);
                return false;
            }
            const FMorphCorrespondence& Mapping = Correspondence[PreviewVertexID];
            if (!DriverMesh.IsVertex(Mapping.DriverTriangle.A)
                || !DriverMesh.IsVertex(Mapping.DriverTriangle.B)
                || !DriverMesh.IsVertex(Mapping.DriverTriangle.C))
            {
                OutError = FString::Printf(
                    TEXT("Morph correspondence for Preview vertex %d is invalid."), PreviewVertexID);
                return false;
            }
            PreviewPointDeltas[PreviewVertexID] =
                DriverPointDeltas[Mapping.DriverTriangle.A] * Mapping.Barycentric.X
                + DriverPointDeltas[Mapping.DriverTriangle.B] * Mapping.Barycentric.Y
                + DriverPointDeltas[Mapping.DriverTriangle.C] * Mapping.Barycentric.Z;
        }

        UDynamicMesh* MorphMesh = NewObject<UDynamicMesh>(GetTransientPackage());
        MorphMesh->SetMesh(FDynamicMesh3(PreviewMesh));
        int32 ProjectedDeltaCount = 0;
        MorphMesh->EditMesh([&PreviewPointDeltas, &ProjectedDeltaCount](FDynamicMesh3& Mesh)
        {
            for (const int32 VertexID : Mesh.VertexIndicesItr())
            {
                const FVector3f Delta = PreviewPointDeltas[VertexID];
                if (!Delta.IsNearlyZero())
                {
                    Mesh.SetVertex(VertexID, Mesh.GetVertex(VertexID) + FVector3d(Delta));
                    ++ProjectedDeltaCount;
                }
            }
        });
        if (ProjectedDeltaCount == 0)
        {
            ++OutSkippedMorphCount;
            continue;
        }

        FGeometryScriptCopyMorphTargetToAssetOptions MorphOptions;
        MorphOptions.bOverwriteExistingTarget = true;
        MorphOptions.bEmitTransaction = false;
        MorphOptions.bDeferMeshPostEditChange = true;
        FGeometryScriptMeshWriteLOD MorphLOD;
        MorphLOD.LODIndex = 0;
        EGeometryScriptOutcomePins MorphOutcome = EGeometryScriptOutcomePins::Failure;
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMorphTargetToSkeletalMesh(
            MorphMesh,
            &Generated,
            DriverMorph->GetFName(),
            MorphOptions,
            MorphLOD,
            MorphOutcome);
        if (MorphOutcome != EGeometryScriptOutcomePins::Success)
        {
            OutError = FString::Printf(
                TEXT("Generated Morph '%s' failed the public Geometry Scripting write."),
                *DriverMorph->GetName());
            return false;
        }
        ++OutMorphCount;
    }

    if (OutMorphCount > 0)
    {
        Generated.PostEditChange();
        for (const TObjectPtr<UMorphTarget>& GeneratedMorph : Generated.GetMorphTargets())
        {
            if (!GeneratedMorph || !GeneratedMorph->HasDataForLOD(0))
            {
                OutError = TEXT("Generated Preview contains an invalid Morph Target after build.");
                return false;
            }
            OutSparseDeltaCount += GeneratedMorph->GetNumDeltasForLOD(0);
        }
    }
    return OutMorphCount + OutSkippedMorphCount == DriverAsset.GetMorphTargets().Num();
}
}

FMtoUPreviewPreparationResult FMtoUPreviewPreparation::Prepare(
    AMtoULiveLinkActor& Owner,
    const UMtoULiveLinkBinding& Binding,
    const FMtoUPreviewStageCallback& OnStage,
    const FMtoUPreviewQualityThresholds& Thresholds)
{
    FMtoUPreviewPreparationResult Result;
    ObserveStage(Result, EMtoUPreviewBuildStage::Preflight, OnStage);

    USkeletalMesh* Driver = Binding.SkeletalMesh;
    UStaticMesh* Preview = Binding.PreviewStaticMesh;
    if (!IsInGameThread())
    {
        Result.Diagnostics = TEXT("Refresh Preview must run on the Unreal Game Thread.");
        return Result;
    }
    if (!Driver || !Preview)
    {
        Result.Diagnostics = TEXT("Select both Driver Skeletal Mesh and Preview Static Mesh.");
        return Result;
    }
    if (!Driver->GetSkeleton() || Driver->GetRefSkeleton().GetNum() == 0)
    {
        Result.Diagnostics = TEXT("Driver Skeletal Mesh has no usable target skeleton.");
        return Result;
    }
    if (Driver->GetNumSourceModels() < 1 || !Driver->HasMeshDescription(0))
    {
        Result.Diagnostics = TEXT("Driver Skeletal Mesh LOD0 source data is unavailable; RenderData is not used.");
        return Result;
    }
    if (!Preview->IsSourceModelValid(0) || !Preview->IsMeshDescriptionValid(0))
    {
        Result.Diagnostics = TEXT("Preview Static Mesh LOD0 source data is unavailable; RenderData is not used.");
        return Result;
    }
    Result.CompletedStages.Add(EMtoUPreviewBuildStage::Preflight);

    ObserveStage(Result, EMtoUPreviewBuildStage::GeometryConversion, OnStage);
    UDynamicMesh* DriverDynamic = NewObject<UDynamicMesh>(GetTransientPackage());
    UDynamicMesh* PreviewDynamic = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
    ReadOptions.bApplyBuildSettings = false;
    ReadOptions.bRequestTangents = true;
    FGeometryScriptMeshReadLOD SourceLOD;
    SourceLOD.LODType = EGeometryScriptLODType::SourceModel;
    SourceLOD.LODIndex = 0;
    EGeometryScriptOutcomePins DriverOutcome = EGeometryScriptOutcomePins::Failure;
    EGeometryScriptOutcomePins PreviewOutcome = EGeometryScriptOutcomePins::Failure;
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
        Driver, DriverDynamic, ReadOptions, SourceLOD, DriverOutcome);
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(
        Preview, PreviewDynamic, ReadOptions, SourceLOD, PreviewOutcome, false);
    if (DriverOutcome != EGeometryScriptOutcomePins::Success
        || PreviewOutcome != EGeometryScriptOutcomePins::Success)
    {
        Result.Diagnostics = TEXT("LOD0 source geometry conversion failed.");
        return Result;
    }
    FString InfluenceError;
    if (!ValidateSourceInfluences(
            DriverDynamic->GetMeshRef(), Driver->GetRefSkeleton(), InfluenceError))
    {
        Result.Diagnostics = InfluenceError;
        return Result;
    }

    // Resolve the unique separable Driver garment surface. Automatic
    // resolution reads geometry evidence alone; the Binding's advanced
    // manual slot override replaces the search with named imported material
    // slots while keeping every geometric gate. Everything downstream
    // (alignment, weights, Morphs) measures and transfers against this
    // filtered surface only, so body, face, and hair cannot contribute
    // nearest-surface data. The filtered mesh keeps original Driver vertex
    // IDs for lossless Morph projection.
    FMtoUDriverGarmentResolution Garment;
    FString ResolveError;
    Result.bManualGarmentSource = Binding.DriverGarmentSlotOverride.Num() > 0;
    const bool bResolvedGarment = Result.bManualGarmentSource
        ? ResolveDriverGarmentSurfaceFromSlots(
            DriverDynamic->GetMeshRef(),
            *Driver,
            Binding.DriverGarmentSlotOverride,
            PreviewDynamic->GetMeshRef(),
            Thresholds.MisalignedNormalizedDistanceAverage,
            Garment,
            ResolveError)
        : ResolveDriverGarmentSurface(
            DriverDynamic->GetMeshRef(),
            *Driver,
            PreviewDynamic->GetMeshRef(),
            *Preview,
            Thresholds.MisalignedNormalizedDistanceAverage,
            Garment,
            ResolveError);
    if (!bResolvedGarment)
    {
        Result.Diagnostics = ResolveError;
        return Result;
    }
    FDynamicMesh3 ResolvedDriver(MoveTemp(Garment.Surface));
    Result.GarmentSourceRegionCount = Garment.RegionCount;
    Result.GarmentSourceTriangleCount = Garment.TriangleCount;
    Result.MatchedPreviewCoverage = Garment.MatchedPreviewCoverage;

    FMtoUSurfaceDistanceStats DistanceStats;
    FString DistanceError;
    if (!MeasureSurfaceDistances(
            ResolvedDriver,
            PreviewDynamic->GetMeshRef(),
            ResolvedDriver.GetBounds().DiagonalLength(),
            DistanceStats,
            DistanceError))
    {
        Result.Diagnostics = DistanceError;
        return Result;
    }
    Result.SurfaceDistanceMin = DistanceStats.Min;
    Result.SurfaceDistanceMax = DistanceStats.Max;
    Result.SurfaceDistanceAverage = DistanceStats.Average;
    Result.SurfaceDistanceRms = DistanceStats.Rms;
    if (DistanceStats.Average > Thresholds.MisalignedNormalizedDistanceAverage)
    {
        Result.Diagnostics = FString::Printf(
            TEXT("Driver and Preview surfaces are misaligned: normalized surface distance average %.4f exceeds %.4f (min %.4f, max %.4f, rms %.4f). Check reference pose, origin, units, and asset-local import space."),
            DistanceStats.Average,
            Thresholds.MisalignedNormalizedDistanceAverage,
            DistanceStats.Min,
            DistanceStats.Max,
            DistanceStats.Rms);
        return Result;
    }
    Result.CompletedStages.Add(EMtoUPreviewBuildStage::GeometryConversion);

    ObserveStage(Result, EMtoUPreviewBuildStage::WeightTransfer, OnStage);
    FDynamicMesh3 ClosestTarget(PreviewDynamic->GetMeshRef());
    TArray<bool> ClosestMatches;
    if (!TransferWeights(
            ResolvedDriver, ClosestTarget,
            FTransferBoneWeights::ETransferBoneWeightsMethod::ClosestPointOnSurface,
            Result.ClosestTransferMilliseconds, ClosestMatches))
    {
        Result.Diagnostics = TEXT("Closest weight-transfer comparison failed.");
        return Result;
    }

    FDynamicMesh3 InpaintTarget(PreviewDynamic->GetMeshRef());
    TArray<bool> InpaintMatches;
    const bool bUseInpaintResult = TransferWeights(
        ResolvedDriver, InpaintTarget,
        FTransferBoneWeights::ETransferBoneWeightsMethod::InpaintWeights,
        Result.InpaintTransferMilliseconds, InpaintMatches);
    if (!bUseInpaintResult)
    {
        // The public inpaint QP solve can fail on large layered inputs; keep the
        // already-computed closest-point result and warn instead of failing.
        Result.bTransferFallbackToClosest = true;
    }
    const FDynamicMesh3& TransferSource = bUseInpaintResult ? InpaintTarget : ClosestTarget;
    const TArray<bool>& TransferMatches = bUseInpaintResult ? InpaintMatches : ClosestMatches;
    Result.VertexCount = TransferSource.VertexCount();
    Result.TriangleCount = TransferSource.TriangleCount();
    for (const int32 VertexID : TransferSource.VertexIndicesItr())
    {
        if (!TransferMatches.IsValidIndex(VertexID) || !TransferMatches[VertexID])
        {
            ++Result.LowConfidenceVertexCount;
        }
    }
    Result.InpaintLowConfidenceRatio = Result.VertexCount > 0
        ? static_cast<double>(Result.LowConfidenceVertexCount) / Result.VertexCount
        : 0.0;
    PreviewDynamic->SetMesh(bUseInpaintResult
        ? MoveTemp(InpaintTarget)
        : MoveTemp(ClosestTarget));
    Result.CompletedStages.Add(EMtoUPreviewBuildStage::WeightTransfer);

    ObserveStage(Result, EMtoUPreviewBuildStage::SkeletalMeshBuild, OnStage);
    TArray<FMorphCorrespondence> MorphCorrespondence;
    const double MorphStart = FPlatformTime::Seconds();
    if (!BuildMorphCorrespondence(
            ResolvedDriver,
            PreviewDynamic->GetMeshRef(),
            MorphCorrespondence,
            Result.Diagnostics))
    {
        return Result;
    }
    USkeletalMesh* Generated = NewObject<USkeletalMesh>(&Owner, NAME_None, RF_Transient);
    Generated->SetSkeleton(Driver->GetSkeleton());
    Generated->SetRefSkeleton(Driver->GetRefSkeleton());
    Generated->CalculateInvRefMatrices();

    FGeometryScriptCopyMeshToAssetOptions WriteOptions;
    WriteOptions.bEnableRecomputeNormals = false;
    WriteOptions.bEnableRecomputeTangents = false;
    WriteOptions.bEnableRemoveDegenerates = false;
    WriteOptions.bUseOriginalVertexOrder = true;
    WriteOptions.bUseBuildScale = false;
    WriteOptions.bReplaceMaterials = true;
    WriteOptions.bEmitTransaction = false;
    WriteOptions.GenerateLightmapUVs =
        EGeometryScriptGenerateLightmapUVOptions::DoNotGenerateLightmapUVs;
    WriteOptions.BoneHierarchyMismatchHandling =
        EGeometryScriptBoneHierarchyMismatchHandling::RemapGeometryToReferenceSkeleton;
    for (const FStaticMaterial& Material : Preview->GetStaticMaterials())
    {
        WriteOptions.NewMaterials.Add(Material.MaterialInterface);
        WriteOptions.NewMaterialSlotNames.Add(Material.MaterialSlotName);
    }
    FGeometryScriptMeshWriteLOD TargetLOD;
    TargetLOD.LODIndex = 0;
    EGeometryScriptOutcomePins BuildOutcome = EGeometryScriptOutcomePins::Failure;
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToSkeletalMesh(
        PreviewDynamic, Generated, WriteOptions, TargetLOD, BuildOutcome);
    if (BuildOutcome != EGeometryScriptOutcomePins::Success)
    {
        Result.Diagnostics = TEXT("Transient Generated Preview Skeletal Mesh build failed.");
        return Result;
    }
    if (!GeneratePreviewMorphs(
            *Driver,
            ResolvedDriver,
            *PreviewDynamic,
            MorphCorrespondence,
            *Generated,
            Result.MorphTargetCount,
            Result.SkippedMorphTargetCount,
            Result.SparseMorphDeltaCount,
            Result.Diagnostics))
    {
        return Result;
    }
    Result.MorphProjectionMilliseconds =
        (FPlatformTime::Seconds() - MorphStart) * 1000.0;
    Result.CompletedStages.Add(EMtoUPreviewBuildStage::SkeletalMeshBuild);

    ObserveStage(Result, EMtoUPreviewBuildStage::Validation, OnStage);
    if (Generated->GetOuter() != &Owner
        || !Generated->HasAnyFlags(RF_Transient)
        || !Generated->HasMeshDescription(0)
        || Generated->GetRefSkeleton().GetNum() != Driver->GetRefSkeleton().GetNum()
        || Generated->GetMaterials().Num() != Preview->GetStaticMaterials().Num()
        || Generated->GetMorphTargets().Num() != Result.MorphTargetCount
        || Result.MorphTargetCount + Result.SkippedMorphTargetCount
            != Driver->GetMorphTargets().Num())
    {
        Result.Diagnostics = TEXT("Generated Preview failed transient ownership or mesh validation.");
        return Result;
    }
    Result.Quality = MtoUEvaluatePreviewQuality(
        Result.InpaintLowConfidenceRatio,
        Result.SurfaceDistanceAverage,
        Thresholds,
        Result.QualityReason);
    if (Result.bTransferFallbackToClosest && Result.Quality == EMtoUPreviewQuality::Ready)
    {
        Result.Quality = EMtoUPreviewQuality::Warning;
    }
    Result.Diagnostics = FString::Printf(
        TEXT("Resolved %d Driver garment region(s) from %d/%d LOD0 triangles with %.4f matched Preview coverage "
            "[%s; %s selection]. Inpaint selected for V1: %d/%d low-confidence vertices (%.4f ratio) across %d triangles; Closest %.3f ms, Inpaint %.3f ms; projected %d Morph Targets (%lld sparse deltas) and skipped %d without matching Preview surface in %.3f ms; normalized surface distance min %.5f / max %.5f / average %.5f / rms %.5f; verdict %s because %s%s."),
        Result.GarmentSourceRegionCount,
        Result.GarmentSourceTriangleCount,
        Driver->GetNumSourceModels() > 0 && Driver->HasMeshDescription(0)
            ? Driver->GetMeshDescription(0)->Triangles().Num()
            : 0,
        Result.MatchedPreviewCoverage,
        *Garment.RegionSummary,
        Result.bManualGarmentSource ? TEXT("Manual") : TEXT("Auto"),
        Result.LowConfidenceVertexCount,
        Result.VertexCount,
        Result.InpaintLowConfidenceRatio,
        Result.TriangleCount,
        Result.ClosestTransferMilliseconds,
        Result.InpaintTransferMilliseconds,
        Result.MorphTargetCount,
        Result.SparseMorphDeltaCount,
        Result.SkippedMorphTargetCount,
        Result.MorphProjectionMilliseconds,
        Result.SurfaceDistanceMin,
        Result.SurfaceDistanceMax,
        Result.SurfaceDistanceAverage,
        Result.SurfaceDistanceRms,
        Result.Quality == EMtoUPreviewQuality::Ready
            ? TEXT("Ready")
            : (Result.Quality == EMtoUPreviewQuality::Warning ? TEXT("Warning") : TEXT("Error")),
        *Result.QualityReason,
        Result.bTransferFallbackToClosest
            ? TEXT("; inpaint solve failed so closest-point weights are used")
            : TEXT(""));
    if (Result.Quality == EMtoUPreviewQuality::Error)
    {
        return Result;
    }
    Result.CompletedStages.Add(EMtoUPreviewBuildStage::Validation);
    Result.FailureStage = EMtoUPreviewBuildStage::None;
    Result.GeneratedPreview = Generated;
    Result.bSucceeded = true;
    return Result;
}

FMtoUPreviewPreparationResult FMtoUPreviewPreparation::RefreshActor(
    AMtoULiveLinkActor& Actor,
    const FMtoUPreviewStageCallback& OnStage)
{
    Actor.BeginPreviewBuild();
    UMtoULiveLinkBinding* Binding = Actor.GetBinding();
    if (!Binding)
    {
        FMtoUPreviewPreparationResult Result;
        Result.FailureStage = EMtoUPreviewBuildStage::Preflight;
        Result.Diagnostics = TEXT("The actor has no MtoU_LiveLink Binding.");
        Actor.FailPreviewBuild(Result.FailureStage, Result.Diagnostics);
        return Result;
    }

    FMtoUPreviewPreparationResult Result = Prepare(
        Actor,
        *Binding,
        [&Actor, &OnStage](EMtoUPreviewBuildStage Stage)
        {
            Actor.SetPreviewBuildStage(Stage);
            if (OnStage)
            {
                OnStage(Stage);
            }
        });
    if (Result.bSucceeded)
    {
        Actor.CompletePreviewBuild(
            Result.GeneratedPreview,
            Result.Quality == EMtoUPreviewQuality::Warning
                || Result.SkippedMorphTargetCount > 0
                || Result.bTransferFallbackToClosest,
            Result.Diagnostics);
    }
    else
    {
        Actor.FailPreviewBuild(Result.FailureStage, Result.Diagnostics);
    }
    return Result;
}
