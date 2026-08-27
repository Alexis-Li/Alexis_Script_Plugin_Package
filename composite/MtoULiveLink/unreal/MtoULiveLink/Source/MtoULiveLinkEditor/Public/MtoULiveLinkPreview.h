#pragma once

#include "CoreMinimal.h"
#include "MtoULiveLinkActor.h"

class UMtoULiveLinkBinding;
class USkeletalMesh;

enum class EMtoUPreviewQuality : uint8
{
    Ready,
    Warning,
    Error
};

/**
 * Calibrated quality boundaries; see the project-history calibration records.
 * The Issue #13 garment-only corpus established the original anchors and its
 * recorded numbers remain that baseline. Issue #22 revalidated the full-character
 * Driver workflow (project-history model-preview-full-character-recalibration
 * record): resolution measures only the resolved Driver garment surface against
 * the Preview, normalized by garment/Preview scale rather than complete-character
 * bounds. Automatic resolution enforces two structural boundaries documented in
 * the preparation implementation: a source-mass limit against duplicated or
 * proximity-pulled geometry, and a twin-region rule that rejects a selected
 * region when a mutually coincident unselected alternative remains equally
 * plausible.
 */
struct MTOULIVELINKEDITOR_API FMtoUPreviewQualityThresholds
{
    /** Inpaint low-confidence ratio at or below which the preview is Ready. */
    double MaxReadyInpaintRatio = 0.27;
    /** Inpaint low-confidence ratio above which the transfer is unsafe. */
    double MaxWarningInpaintRatio = 0.75;
    /**
     * Normalized symmetric surface-distance average above which inputs are
     * misaligned. The average keeps local missing-surface holes (a quality
     * warning) distinct from globally shifted inputs (all samples large).
     * Measured anchors: valid inputs <= 0.005, an extreme half-surface hole
     * fixture measures 0.2474, misaligned negatives measure >= 1.7113.
     */
    double MisalignedNormalizedDistanceAverage = 0.60;
};

/**
 * Intentional public result of the Preview preparation seam. Callers and
 * acceptance tests observe build output, source resolution, quality, timing,
 * and diagnostics here without depending on private resolver helpers.
 */
struct MTOULIVELINKEDITOR_API FMtoUPreviewPreparationResult
{
    bool bSucceeded = false;
    USkeletalMesh* GeneratedPreview = nullptr;
    EMtoUPreviewBuildStage FailureStage = EMtoUPreviewBuildStage::None;
    TArray<EMtoUPreviewBuildStage> CompletedStages;
    int32 VertexCount = 0;
    int32 TriangleCount = 0;
    int32 LowConfidenceVertexCount = 0;
    int32 MorphTargetCount = 0;
    int32 SkippedMorphTargetCount = 0;
    int64 SparseMorphDeltaCount = 0;
    /** Connected Driver LOD0 regions that automatic resolution selected as the garment source. */
    int32 GarmentSourceRegionCount = 0;
    /** Driver LOD0 source triangles inside the resolved Driver garment surface. */
    int32 GarmentSourceTriangleCount = 0;
    /** True when the Binding's manual Driver material-slot override chose the source. */
    bool bManualGarmentSource = false;
    /** Fraction of Preview vertices within agreement distance of the resolved Driver garment surface. */
    double MatchedPreviewCoverage = 1.0;
    bool bTransferFallbackToClosest = false;
    double ClosestTransferMilliseconds = 0.0;
    double InpaintTransferMilliseconds = 0.0;
    double MorphProjectionMilliseconds = 0.0;
    double InpaintLowConfidenceRatio = 0.0;
    double SurfaceDistanceMin = 0.0;
    double SurfaceDistanceMax = 0.0;
    double SurfaceDistanceAverage = 0.0;
    double SurfaceDistanceRms = 0.0;
    EMtoUPreviewQuality Quality = EMtoUPreviewQuality::Error;
    FString QualityReason;
    FString Diagnostics;
};

using FMtoUPreviewStageCallback = TFunction<void(EMtoUPreviewBuildStage)>;

/** Pure threshold evaluation so boundary behavior is testable without geometry fixtures. */
MTOULIVELINKEDITOR_API EMtoUPreviewQuality MtoUEvaluatePreviewQuality(
    double InpaintLowConfidenceRatio,
    double NormalizedSurfaceDistanceAverage,
    const FMtoUPreviewQualityThresholds& Thresholds,
    FString& OutReason);

class MTOULIVELINKEDITOR_API FMtoUPreviewPreparation
{
public:
    static FMtoUPreviewPreparationResult Prepare(
        AMtoULiveLinkActor& Owner,
        const UMtoULiveLinkBinding& Binding,
        const FMtoUPreviewStageCallback& OnStage = {},
        const FMtoUPreviewQualityThresholds& Thresholds = FMtoUPreviewQualityThresholds());

    static FMtoUPreviewPreparationResult RefreshActor(
        AMtoULiveLinkActor& Actor,
        const FMtoUPreviewStageCallback& OnStage = {});
};
