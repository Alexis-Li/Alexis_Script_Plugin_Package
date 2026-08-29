#pragma once

#include "CoreMinimal.h"
#include "MtoULiveLinkActor.h"

class UMtoULiveLinkBinding;
class USkeletalMesh;

using FMtoUPreviewStageCallback = TFunction<void(EMtoUPreviewBuildStage)>;

/**
 * The Editor module's one explicit Preview refresh interface. It drives the
 * Binding actor's private readiness transitions, selects the Generated
 * Preview for immediate display after a successful commit, and returns the
 * coherent readiness snapshot. Detailed preparation evidence lives in the
 * Editor-private preparation header instead of this interface.
 */
class MTOULIVELINKEDITOR_API FMtoUPreviewPreparation
{
public:
    static FMtoUPreviewReadiness RefreshActor(
        AMtoULiveLinkActor& Actor,
        const FMtoUPreviewStageCallback& OnStage = {});
};
