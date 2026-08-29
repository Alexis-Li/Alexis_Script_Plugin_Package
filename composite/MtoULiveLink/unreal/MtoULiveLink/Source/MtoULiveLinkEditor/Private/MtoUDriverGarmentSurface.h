#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

class UStaticMesh;
class USkeletalMesh;
class UMtoULiveLinkBinding;

/**
 * Complete outcome of one Driver garment-surface resolution. The resolved
 * surface keeps the original Driver vertex IDs so Morph deltas and
 * import-vertex correspondence stay lossless; vertices outside the resolved
 * surface are removed instead of compacted. A failed result contains no usable
 * surface; it may retain the selection identity, counts, coverage, and
 * diagnostics as failure evidence.
 */
struct FMtoUDriverGarmentSurfaceResult
{
    /** True when resolution produced a validated Driver garment surface. */
    bool bSucceeded = false;
    /** True when the Binding's manual Driver Garment Slot Override chose the source. */
    bool bManualSource = false;
    /** The validated Driver LOD0 garment surface; empty when resolution failed. */
    FDynamicMesh3 Surface;
    /** Connected Driver LOD0 regions that resolution selected as the garment source. */
    int32 RegionCount = 0;
    /** Driver LOD0 source triangles inside the resolved Driver garment surface. */
    int32 TriangleCount = 0;
    /** Fraction of Preview vertices within agreement distance of the resolved surface. */
    double MatchedPreviewCoverage = 1.0;
    /** Compact identification of each selected region for diagnostics. */
    FString RegionSummary;
    /** Actionable failure diagnostics; empty on success. */
    FString Diagnostics;
};

/**
 * Resolves the Driver garment surface for one Preview refresh from the current
 * Binding behind one interface. An empty Driver Garment Slot Override selects
 * automatic resolution; a non-empty override selects manual resolution and
 * never falls back to automatic after an error. Both paths apply the same
 * source-selection and geometry-validation rules and return either a validated
 * surface or an atomic failure. Resolution is side-effect free: it does not
 * mutate the Binding, source assets, or any persistent project content.
 */
FMtoUDriverGarmentSurfaceResult MtoUResolveDriverGarmentSurface(
    const FDynamicMesh3& Driver,
    const USkeletalMesh& DriverAsset,
    const FDynamicMesh3& Preview,
    const UStaticMesh& PreviewAsset,
    const UMtoULiveLinkBinding& Binding,
    double MisalignedNormalizedDistanceAverage);
