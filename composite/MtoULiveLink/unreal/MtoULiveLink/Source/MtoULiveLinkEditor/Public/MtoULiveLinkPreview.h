#pragma once

#include "CoreMinimal.h"
#include "MtoULiveLinkActor.h"

class UMtoULiveLinkBinding;
class USkeletalMesh;

struct MTOULIVELINKEDITOR_API FMtoUPreviewPreparationResult
{
    bool bSucceeded = false;
    USkeletalMesh* GeneratedPreview = nullptr;
    EMtoUPreviewBuildStage FailureStage = EMtoUPreviewBuildStage::None;
    TArray<EMtoUPreviewBuildStage> CompletedStages;
    int32 VertexCount = 0;
    int32 LowConfidenceVertexCount = 0;
    double ClosestTransferMilliseconds = 0.0;
    double InpaintTransferMilliseconds = 0.0;
    FString Diagnostics;
};

using FMtoUPreviewStageCallback = TFunction<void(EMtoUPreviewBuildStage)>;

class MTOULIVELINKEDITOR_API FMtoUPreviewPreparation
{
public:
    static FMtoUPreviewPreparationResult Prepare(
        AMtoULiveLinkActor& Owner,
        const UMtoULiveLinkBinding& Binding,
        const FMtoUPreviewStageCallback& OnStage = {});

    static FMtoUPreviewPreparationResult RefreshActor(
        AMtoULiveLinkActor& Actor,
        const FMtoUPreviewStageCallback& OnStage = {});
};
