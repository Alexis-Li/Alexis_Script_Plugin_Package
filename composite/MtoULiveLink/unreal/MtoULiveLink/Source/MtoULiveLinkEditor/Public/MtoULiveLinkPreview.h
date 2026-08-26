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
 * Calibrated Issue #13 corpus boundaries; see the project-history calibration
 * record. Measured on the stock fixtures plus the external production garment:
 * approved inputs measure at most 0.26532 low-confidence ratio and 0.00468
 * normalized surface-distance average; the seam-heavy diagnostic cubes measure
 * up to 0.66667 ratio; the accepted half-surface fixture measures 0.75000;
 * misaligned negatives measure 1.7113+ distance average.
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
