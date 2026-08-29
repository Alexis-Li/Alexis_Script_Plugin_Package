#include "MtoULiveLinkPreview.h"

#include "MtoUDriverGarmentSurface.h"

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

    // Resolve the complete Driver garment surface through the private
    // resolution module: it owns the automatic/manual choice from the
    // Binding's Driver Garment Slot Override and all source-selection and
    // geometry-validation rules. Everything downstream (alignment, weights,
    // Morphs) measures and transfers against this filtered surface only, so
    // body, face, and hair cannot contribute nearest-surface data. The
    // filtered mesh keeps original Driver vertex IDs for lossless Morph
    // projection.
    FMtoUDriverGarmentSurfaceResult Garment = MtoUResolveDriverGarmentSurface(
        DriverDynamic->GetMeshRef(),
        *Driver,
        PreviewDynamic->GetMeshRef(),
        *Preview,
        Binding,
        Thresholds.MisalignedNormalizedDistanceAverage);
    Result.bManualGarmentSource = Garment.bManualSource;
    if (!Garment.bSucceeded)
    {
        Result.Diagnostics = MoveTemp(Garment.Diagnostics);
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
    TArray<FString> PreviewParts;
    for (const FStaticMaterial& Material : Preview->GetStaticMaterials())
    {
        const FName PartName = Material.ImportedMaterialSlotName != NAME_None
            ? Material.ImportedMaterialSlotName
            : Material.MaterialSlotName;
        if (PartName != NAME_None)
        {
            PreviewParts.AddUnique(PartName.ToString());
        }
    }
    if (PreviewParts.IsEmpty())
    {
        PreviewParts.Add(Preview->GetName());
    }
    Result.Summary = FString::Join(PreviewParts, TEXT("\n"));
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
            Result.Diagnostics,
            Result.Summary);
    }
    else
    {
        Actor.FailPreviewBuild(Result.FailureStage, Result.Diagnostics);
    }
    return Result;
}
