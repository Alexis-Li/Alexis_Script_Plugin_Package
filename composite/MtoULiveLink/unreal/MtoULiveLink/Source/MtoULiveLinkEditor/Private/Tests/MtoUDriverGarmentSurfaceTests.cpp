#if WITH_DEV_AUTOMATION_TESTS

#include "MtoUDriverGarmentSurface.h"
#include "MtoULiveLinkEditorTestFixtures.h"

#include "MtoULiveLinkBinding.h"
#include "MtoULiveLinkPreviewDetail.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMeshEditor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UDynamicMesh.h"

namespace
{
using MtoUEditorTest::AppendPartCopy;
using MtoUEditorTest::SetUniformBoneWeights;
using MtoUEditorTest::WriteTransientDriver;

/**
 * Converts both source assets exactly the way Preview preparation does, so
 * the focused tests cross the same private seam with the same converted
 * inputs the public Preview refresh resolves against.
 */
bool ConvertSourceMeshes(
    USkeletalMesh& DriverAsset,
    UStaticMesh& PreviewAsset,
    UE::Geometry::FDynamicMesh3& OutDriver,
    UE::Geometry::FDynamicMesh3& OutPreview)
{
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
        &DriverAsset, DriverDynamic, ReadOptions, SourceLOD, DriverOutcome);
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(
        &PreviewAsset, PreviewDynamic, ReadOptions, SourceLOD, PreviewOutcome, false);
    if (DriverOutcome != EGeometryScriptOutcomePins::Success
        || PreviewOutcome != EGeometryScriptOutcomePins::Success)
    {
        return false;
    }
    OutDriver = DriverDynamic->GetMeshRef();
    OutPreview = PreviewDynamic->GetMeshRef();
    return true;
}

FMtoUDriverGarmentSurfaceResult ResolveSurface(
    const UE::Geometry::FDynamicMesh3& Driver,
    const USkeletalMesh& DriverAsset,
    const UE::Geometry::FDynamicMesh3& Preview,
    const UStaticMesh& PreviewAsset,
    const TArray<FName>& Override)
{
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>();
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    Binding->SkeletalMesh = const_cast<USkeletalMesh*>(&DriverAsset);
    Binding->PreviewStaticMesh = const_cast<UStaticMesh*>(&PreviewAsset);
    Binding->DriverGarmentSlotOverride = Override;
    return MtoUResolveDriverGarmentSurface(
        Driver,
        Preview,
        *Binding,
        FMtoUPreviewQualityThresholds().MisalignedNormalizedDistanceAverage);
}

/** Every resolved-surface vertex must keep its original Driver vertex ID and position. */
bool PreservesDriverVertexCorrespondence(
    const UE::Geometry::FDynamicMesh3& Driver,
    const UE::Geometry::FDynamicMesh3& Surface)
{
    if (Surface.TriangleCount() == 0 || Surface.VertexCount() == 0)
    {
        return false;
    }
    for (const int32 VertexID : Surface.VertexIndicesItr())
    {
        if (!Driver.IsVertex(VertexID)
            || !Surface.GetVertex(VertexID).Equals(Driver.GetVertex(VertexID)))
        {
            return false;
        }
    }
    return true;
}

/** A failed resolution must expose no usable surface geometry at all. */
bool HasNoUsableSurface(const FMtoUDriverGarmentSurfaceResult& Result)
{
    return Result.Surface.TriangleCount() == 0 && Result.Surface.VertexCount() == 0;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUDriverGarmentSurfaceAutoTest,
    "MtoULiveLink.Editor.GarmentSurface.Auto",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUDriverGarmentSurfaceAutoTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentSurfaceAutoWorld"));
    MtoUEditorTest::FMtoUFullCharacterFixtures Fixtures;
    TestTrue(TEXT("automatic-resolution fixtures are created"),
        MtoUEditorTest::MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures));
    if (!Fixtures.IsValid())
    {
        AddError(TEXT("automatic-resolution fixtures were not created"));
        return false;
    }

    // Legacy garment-only compatibility: the whole garment Driver resolves
    // automatically with complete coverage and preserved correspondence. The
    // SkeletalCube-derived fixture keeps per-face vertex runs, so the two
    // disconnected garment cubes surface as several face regions; legacy
    // behavior resolves every region that owns spatially agreeing Preview
    // vertices and never applied the full-character failure boundaries.
    UE::Geometry::FDynamicMesh3 LegacyDriver;
    UE::Geometry::FDynamicMesh3 PreviewMesh;
    TestTrue(TEXT("garment-only sources convert for resolution"),
        ConvertSourceMeshes(
            *Fixtures.GarmentOnlyDriver, *Fixtures.Preview, LegacyDriver, PreviewMesh));
    if (LegacyDriver.TriangleCount() == 0 || PreviewMesh.TriangleCount() == 0)
    {
        AddError(TEXT("garment-only converted sources are empty"));
        return false;
    }
    const FMtoUDriverGarmentSurfaceResult Legacy =
        ResolveSurface(LegacyDriver, *Fixtures.GarmentOnlyDriver, PreviewMesh, *Fixtures.Preview, {});
    AddInfo(Legacy.Diagnostics);
    TestTrue(TEXT("a garment-only Driver resolves its garment surface automatically"),
        Legacy.bSucceeded
            && !Legacy.bManualSource
            && Legacy.RegionCount >= 1
            && Legacy.MatchedPreviewCoverage > 0.999
            && Legacy.TriangleCount > 0);
    TestTrue(TEXT("the garment-only surface preserves Driver vertex correspondence"),
        PreservesDriverVertexCorrespondence(LegacyDriver, Legacy.Surface));

    // Full-character selection: geometry ownership isolates the garment
    // pieces without any usable material evidence.
    UE::Geometry::FDynamicMesh3 FullDriver;
    TestTrue(TEXT("full-character sources convert for resolution"),
        ConvertSourceMeshes(
            *Fixtures.FullDriver, *Fixtures.Preview, FullDriver, PreviewMesh));
    const FMtoUDriverGarmentSurfaceResult Auto =
        ResolveSurface(FullDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    AddInfo(Auto.Diagnostics);
    TestTrue(TEXT("automatic resolution selects the disconnected garment regions"),
        Auto.bSucceeded
            && !Auto.bManualSource
            && Auto.RegionCount >= 2
            && Auto.MatchedPreviewCoverage > 0.999);
    TestTrue(TEXT("the automatic surface stays inside the calibrated source-mass envelope"),
        Auto.TriangleCount > 0
            && Auto.TriangleCount <= FMath::CeilToInt(
                MtoUEditorTest::TestMaxSourceToPreviewRatio * Fixtures.PreviewTriangleCount));
    TestTrue(TEXT("the automatic surface preserves Driver vertex correspondence"),
        PreservesDriverVertexCorrespondence(FullDriver, Auto.Surface));
    TestTrue(TEXT("unselected isolated vertices are removed, not compacted"),
        Auto.Surface.VertexCount() < FullDriver.VertexCount());
    const UE::Geometry::FAxisAlignedBox3d GarmentCheckBounds(
        Fixtures.GarmentBounds.Min - 0.5, Fixtures.GarmentBounds.Max + 0.5);
    bool bGarmentOnlyGeometry = true;
    for (const int32 VertexID : Auto.Surface.VertexIndicesItr())
    {
        const FVector3d Position = Auto.Surface.GetVertex(VertexID);
        bGarmentOnlyGeometry &= GarmentCheckBounds.Contains(Position)
            && !Fixtures.BodyCoreBounds.Contains(Position);
    }
    TestTrue(TEXT("the resolved surface contains only garment geometry"), bGarmentOnlyGeometry);
    TestTrue(TEXT("the region summary identifies every selected region"),
        !Auto.RegionSummary.IsEmpty());

    // Material mimicry: giving the Driver body slot the exact Preview slot
    // name and material cannot change the geometric resolution.
    FSkeletalMaterial& BodyMaterial = Fixtures.FullDriver->GetMaterials()[0];
    const FName BodyName = BodyMaterial.MaterialSlotName;
    BodyMaterial.MaterialSlotName = FName(TEXT("Cloth09_Top_1"));
    BodyMaterial.MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
    const FMtoUDriverGarmentSurfaceResult Mimicry =
        ResolveSurface(FullDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    BodyMaterial.MaterialSlotName = BodyName;
    BodyMaterial.MaterialInterface = Fixtures.FullDriver->GetMaterials()[1].MaterialInterface;
    AddInfo(Mimicry.Diagnostics);
    TestTrue(TEXT("matching material identity does not change geometric resolution"),
        Mimicry.bSucceeded
            && Mimicry.MatchedPreviewCoverage > 0.999
            && Mimicry.TriangleCount == Auto.TriangleCount
            && Mimicry.RegionCount == Auto.RegionCount);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUDriverGarmentSurfaceManualTest,
    "MtoULiveLink.Editor.GarmentSurface.Manual",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUDriverGarmentSurfaceManualTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentSurfaceManualWorld"));
    MtoUEditorTest::FMtoUFullCharacterFixtures Fixtures;
    TestTrue(TEXT("manual-resolution fixtures are created"),
        MtoUEditorTest::MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures));
    if (!Fixtures.IsValid())
    {
        AddError(TEXT("manual-resolution fixtures were not created"));
        return false;
    }
    UE::Geometry::FDynamicMesh3 Driver;
    UE::Geometry::FDynamicMesh3 PreviewMesh;
    TestTrue(TEXT("manual-resolution sources convert for resolution"),
        ConvertSourceMeshes(*Fixtures.FullDriver, *Fixtures.Preview, Driver, PreviewMesh));

    const FName UpperA(TEXT("Garment_Upper_A"));
    const FName UpperB(TEXT("Garment_Upper_B"));
    const FName Lower(TEXT("Garment_Lower"));

    // A valid multi-slot selection covers the whole Preview garment.
    const FMtoUDriverGarmentSurfaceResult Manual = ResolveSurface(
        Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview,
        {UpperA, UpperB, Lower});
    AddInfo(Manual.Diagnostics);
    TestTrue(TEXT("a valid multi-slot override resolves the complete garment surface"),
        Manual.bSucceeded
            && Manual.bManualSource
            && Manual.MatchedPreviewCoverage > 0.999
            && Manual.RegionCount >= 2);
    TestTrue(TEXT("the manual surface stays inside the Preview garment"),
        Manual.TriangleCount > 0
            && Manual.TriangleCount <= Fixtures.PreviewTriangleCount);
    TestTrue(TEXT("the manual surface preserves Driver vertex correspondence"),
        PreservesDriverVertexCorrespondence(Driver, Manual.Surface));
    TestTrue(TEXT("the manual summary names the resolved imported slot identities"),
        Manual.RegionSummary.Contains(TEXT("manual slots"))
            && Manual.RegionSummary.Contains(TEXT("Garment_Upper_A"))
            && Manual.RegionSummary.Contains(TEXT("Garment_Upper_B"))
            && Manual.RegionSummary.Contains(TEXT("Garment_Lower")));

    // Matching keys on the stable imported identity, not the displayed name.
    const FName DisplayedName = Fixtures.FullDriver->GetMaterials()[3].MaterialSlotName;
    Fixtures.FullDriver->GetMaterials()[3].MaterialSlotName = FName(TEXT("Renamed_Display"));
    const FMtoUDriverGarmentSurfaceResult ByImportedName = ResolveSurface(
        Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {UpperA, UpperB, Lower});
    Fixtures.FullDriver->GetMaterials()[3].MaterialSlotName = DisplayedName;
    TestTrue(TEXT("the override matches the stable imported slot identity"),
        ByImportedName.bSucceeded
            && ByImportedName.TriangleCount == Manual.TriangleCount);

    // A partial selection fails whole-Preview coverage validation.
    const FMtoUDriverGarmentSurfaceResult Partial = ResolveSurface(
        Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {UpperA});
    AddInfo(Partial.Diagnostics);
    TestTrue(TEXT("a partial override fails whole-Preview coverage transactionally"),
        !Partial.bSucceeded
            && Partial.bManualSource
            && Partial.MatchedPreviewCoverage < 1.0
            && Partial.Diagnostics.Contains(TEXT("whole Preview garment")));
    TestTrue(TEXT("the failed partial resolution exposes no usable surface"),
        HasNoUsableSurface(Partial));
    TestTrue(TEXT("the failed partial result keeps its counts as failure evidence"),
        Partial.TriangleCount > 0 && Partial.RegionCount >= 1);

    // A missing persisted identity never silently returns to Auto.
    const FMtoUDriverGarmentSurfaceResult Missing = ResolveSurface(
        Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview,
        {UpperA, UpperB, Lower, FName(TEXT("Clothes_09_Missing"))});
    AddInfo(Missing.Diagnostics);
    TestTrue(TEXT("a missing override identity blocks with an actionable diagnostic"),
        !Missing.bSucceeded
            && HasNoUsableSurface(Missing)
            && Missing.Diagnostics.Contains(TEXT("Clothes_09_Missing"))
            && Missing.Diagnostics.Contains(TEXT("unknown Driver material slot"))
            && Missing.Diagnostics.Contains(TEXT("automatic resolution")));
    TestTrue(TEXT("the missing-identity diagnostic lists the available slot names"),
        Missing.Diagnostics.Contains(TEXT("Garment_Upper_A"))
            && Missing.Diagnostics.Contains(TEXT("Garment_Lower")));

    // A no-longer-unique imported identity is a hard error naming both slots.
    TArray<FSkeletalMaterial>& DriverMaterials = Fixtures.FullDriver->GetMaterials();
    const FName BodyName = DriverMaterials[0].MaterialSlotName;
    DriverMaterials[0].ImportedMaterialSlotName = Lower;
    DriverMaterials[0].MaterialSlotName = Lower;
    const FMtoUDriverGarmentSurfaceResult Duplicated = ResolveSurface(
        Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {Lower});
    DriverMaterials[0].ImportedMaterialSlotName = BodyName;
    DriverMaterials[0].MaterialSlotName = BodyName;
    AddInfo(Duplicated.Diagnostics);
    TestTrue(TEXT("a duplicated override identity is a hard error naming both slots"),
        !Duplicated.bSucceeded
            && HasNoUsableSurface(Duplicated)
            && Duplicated.Diagnostics.Contains(TEXT("matches more than one")));

    // A repeated entry in one override is also rejected.
    const FMtoUDriverGarmentSurfaceResult Repeated = ResolveSurface(
        Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {UpperA, UpperA});
    AddInfo(Repeated.Diagnostics);
    TestTrue(TEXT("a repeated override entry is a hard error"),
        !Repeated.bSucceeded
            && HasNoUsableSurface(Repeated)
            && Repeated.Diagnostics.Contains(TEXT("more than once")));

    // A slot with no LOD0 triangles in the current import stays stale.
    FSkeletalMaterial& AddedSlot = DriverMaterials.AddDefaulted_GetRef();
    AddedSlot.MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
    AddedSlot.MaterialSlotName = FName(TEXT("Garment_Empty"));
    AddedSlot.ImportedMaterialSlotName = AddedSlot.MaterialSlotName;
    const FMtoUDriverGarmentSurfaceResult EmptySlot = ResolveSurface(
        Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview,
        {UpperA, UpperB, Lower, AddedSlot.MaterialSlotName});
    AddInfo(EmptySlot.Diagnostics);
    TestTrue(TEXT("an override slot without LOD0 triangles fails as stale"),
        !EmptySlot.bSucceeded
            && HasNoUsableSurface(EmptySlot)
            && EmptySlot.Diagnostics.Contains(TEXT("no LOD0 triangles"))
            && EmptySlot.Diagnostics.Contains(TEXT("Garment_Empty")));

    // Clearing the override selects automatic resolution again.
    const FMtoUDriverGarmentSurfaceResult AutoAgain = ResolveSurface(
        Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    AddInfo(AutoAgain.Diagnostics);
    const FMtoUDriverGarmentSurfaceResult AutoReference = ResolveSurface(
        Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    TestTrue(TEXT("clearing the override returns to automatic resolution"),
        AutoAgain.bSucceeded
            && !AutoAgain.bManualSource
            && AutoAgain.TriangleCount == AutoReference.TriangleCount
            && AutoAgain.MatchedPreviewCoverage > 0.999);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUDriverGarmentSurfaceFailuresTest,
    "MtoULiveLink.Editor.GarmentSurface.Failures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUDriverGarmentSurfaceFailuresTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    {
        UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>();
        TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
        const UE::Geometry::FDynamicMesh3 Empty;
        const FMtoUDriverGarmentSurfaceResult MissingAssets =
            MtoUResolveDriverGarmentSurface(
                Empty,
                Empty,
                *Binding,
                FMtoUPreviewQualityThresholds().MisalignedNormalizedDistanceAverage);
        TestTrue(TEXT("missing Binding assets fail atomically"),
            !MissingAssets.bSucceeded
                && HasNoUsableSurface(MissingAssets)
                && MissingAssets.Diagnostics.Contains(TEXT("required")));
    }

    // Exact duplicates remain equally plausible even when nearest-ownership
    // hands every Preview vertex to one copy, and the diagnostic is
    // deterministic across repeated resolutions.
    {
        UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentSurfaceDuplicate"));
        MtoUEditorTest::FMtoUGarmentFaultFixtures Faults;
        TestTrue(TEXT("duplicate-garment fixtures are created"),
            MtoUEditorTest::MakeGarmentFaultFixtures(*WorldPackage, *this,
                MtoUEditorTest::EMtoUGarmentFaultKind::DuplicateGarment, Faults));
        if (Faults.IsValid())
        {
            UE::Geometry::FDynamicMesh3 Driver;
            UE::Geometry::FDynamicMesh3 PreviewMesh;
            TestTrue(TEXT("duplicate-garment sources convert for resolution"),
                ConvertSourceMeshes(*Faults.Driver, *Faults.Preview, Driver, PreviewMesh));
            const FMtoUDriverGarmentSurfaceResult First = ResolveSurface(
                Driver, *Faults.Driver, PreviewMesh, *Faults.Preview, {});
            AddInfo(First.Diagnostics);
            const FMtoUDriverGarmentSurfaceResult Second = ResolveSurface(
                Driver, *Faults.Driver, PreviewMesh, *Faults.Preview, {});
            TestTrue(TEXT("exact duplicate copies fail with stable ambiguity"),
                !First.bSucceeded
                && HasNoUsableSurface(First)
                && First.Diagnostics.Contains(TEXT("ambiguous")));
            TestTrue(TEXT("the duplicate ambiguity diagnostic is deterministic across resolutions"),
                !Second.bSucceeded
                && Second.Diagnostics == First.Diagnostics);
            TestTrue(TEXT("the ambiguity failure keeps its counts as failure evidence"),
                First.TriangleCount > 0 && First.RegionCount >= 1);
        }
    }

    // Near-twin shifted duplicates stay indistinguishable inside the
    // calibrated twin proximity.
    {
        UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentSurfaceTwin"));
        MtoUEditorTest::FMtoUGarmentFaultFixtures Faults;
        TestTrue(TEXT("near-twin fixtures are created"),
            MtoUEditorTest::MakeGarmentFaultFixtures(*WorldPackage, *this,
                MtoUEditorTest::EMtoUGarmentFaultKind::ShiftedDuplicateGarment, Faults));
        if (Faults.IsValid())
        {
            UE::Geometry::FDynamicMesh3 Driver;
            UE::Geometry::FDynamicMesh3 PreviewMesh;
            TestTrue(TEXT("near-twin sources convert for resolution"),
                ConvertSourceMeshes(*Faults.Driver, *Faults.Preview, Driver, PreviewMesh));
            const FMtoUDriverGarmentSurfaceResult First = ResolveSurface(
                Driver, *Faults.Driver, PreviewMesh, *Faults.Preview, {});
            AddInfo(First.Diagnostics);
            const FMtoUDriverGarmentSurfaceResult Second = ResolveSurface(
                Driver, *Faults.Driver, PreviewMesh, *Faults.Preview, {});
            TestTrue(TEXT("near-twin shifted duplicates fail with deterministic ambiguity"),
                !First.bSucceeded && !Second.bSucceeded
                && HasNoUsableSurface(First)
                && First.Diagnostics.Contains(TEXT("ambiguous"))
                && First.Diagnostics == Second.Diagnostics);
        }
    }

    // Source-mass rejection: a garment shell whose surface is unchanged but
    // subdivided past the calibrated mass boundary fails without any twin.
    {
        UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentSurfaceMass"));
        USkeletalMesh* Base = LoadObject<USkeletalMesh>(
            nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
        if (!Base)
        {
            AddError(TEXT("mass fixture SkeletalCube was not loaded"));
            return false;
        }
        UDynamicMesh* BodySource = NewObject<UDynamicMesh>(WorldPackage);
        FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
        ReadOptions.bApplyBuildSettings = false;
        ReadOptions.bRequestTangents = true;
        FGeometryScriptMeshReadLOD ReadLOD;
        ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
            Base, BodySource, ReadOptions, ReadLOD, Outcome);
        if (Outcome != EGeometryScriptOutcomePins::Success)
        {
            AddError(TEXT("mass fixture body conversion failed"));
            return false;
        }
        const UE::Geometry::FDynamicMesh3& Cube = BodySource->GetMeshRef();
        const FVector3d LowerOffset = Cube.GetBounds().Center()
            - FVector3d(0.0, 0.0, Cube.GetBounds().Height() * 1.05);

        UE::Geometry::FDynamicMesh3 PokedUpper;
        AppendPartCopy(PokedUpper, Cube, FVector3d::Zero(), 1.15, 0, 1);
        TArray<int32> UpperTriangleIDs;
        for (const int32 TriangleID : PokedUpper.TriangleIndicesItr())
        {
            UpperTriangleIDs.Add(TriangleID);
        }
        for (const int32 TriangleID : UpperTriangleIDs)
        {
            UE::Geometry::FDynamicMesh3::FPokeTriangleInfo PokeInfo;
            PokedUpper.PokeTriangle(TriangleID, PokeInfo);
        }
        UE::Geometry::FDynamicMesh3 Geometry;
        AppendPartCopy(Geometry, Cube, FVector3d::Zero(), 0.9, 0, 1);
        {
            UE::Geometry::FDynamicMeshEditor Editor(&Geometry);
            UE::Geometry::FMeshIndexMappings Mappings;
            Editor.AppendMesh(&PokedUpper, Mappings);
        }
        AppendPartCopy(Geometry, Cube, LowerOffset, 0.55, 2, 1);
        SetUniformBoneWeights(Geometry);
        USkeletalMesh* MassDriver = WriteTransientDriver(*WorldPackage, *Base, Geometry,
            {
                FName(TEXT("Mass_Body")),
                FName(TEXT("Mass_Garment_Upper")),
                FName(TEXT("Mass_Garment_Lower")),
            });
        TestNotNull(TEXT("source-mass Driver was written"), MassDriver);

        UE::Geometry::FDynamicMesh3 PreviewGeometry;
        AppendPartCopy(PreviewGeometry, Cube, FVector3d::Zero(), 1.15, 0, 1);
        AppendPartCopy(PreviewGeometry, Cube, LowerOffset, 0.55, 1, 1);
        UStaticMesh* MassPreview = NewObject<UStaticMesh>(WorldPackage, NAME_None, RF_Transient);
        FGeometryScriptCopyMeshToAssetOptions PreviewWriteOptions;
        PreviewWriteOptions.bEmitTransaction = false;
        PreviewWriteOptions.bEnableRecomputeNormals = true;
        PreviewWriteOptions.bEnableRecomputeTangents = true;
        PreviewWriteOptions.bReplaceMaterials = true;
        PreviewWriteOptions.NewMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
        PreviewWriteOptions.NewMaterialSlotNames.Add(FName(TEXT("Mass_Preview_Top")));
        PreviewWriteOptions.NewMaterials.Add(nullptr);
        PreviewWriteOptions.NewMaterialSlotNames.Add(FName(TEXT("Mass_Preview_Bottom")));
        FGeometryScriptMeshWriteLOD PreviewWriteLOD;
        EGeometryScriptOutcomePins PreviewOutcome = EGeometryScriptOutcomePins::Failure;
        UDynamicMesh* PreviewSource = NewObject<UDynamicMesh>(WorldPackage);
        PreviewSource->SetMesh(MoveTemp(PreviewGeometry));
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
            PreviewSource, MassPreview, PreviewWriteOptions, PreviewWriteLOD, PreviewOutcome, false);
        if (!MassDriver || PreviewOutcome != EGeometryScriptOutcomePins::Success)
        {
            AddError(TEXT("source-mass Preview was not written"));
            return false;
        }

        UE::Geometry::FDynamicMesh3 Driver;
        UE::Geometry::FDynamicMesh3 PreviewMesh;
        TestTrue(TEXT("source-mass sources convert for resolution"),
            ConvertSourceMeshes(*MassDriver, *MassPreview, Driver, PreviewMesh));
        const FMtoUDriverGarmentSurfaceResult Mass = ResolveSurface(
            Driver, *MassDriver, PreviewMesh, *MassPreview, {});
        AddInfo(Mass.Diagnostics);
        TestTrue(TEXT("a subdivided garment shell exceeds the calibrated source-mass boundary"),
            !Mass.bSucceeded
            && HasNoUsableSurface(Mass)
            && Mass.Diagnostics.Contains(TEXT("source-mass boundary"))
            && Mass.Diagnostics.Contains(TEXT("Driver Garment Slot Override")));
        TestTrue(TEXT("the mass failure keeps full coverage and counts as failure evidence"),
            Mass.MatchedPreviewCoverage > 0.999
            && Mass.TriangleCount > 0
            && Mass.TriangleCount > FMath::CeilToInt(
                MtoUEditorTest::TestMaxSourceToPreviewRatio * PreviewMesh.TriangleCount()));
    }

    // A globally misaligned Preview cannot reach full coverage, and the
    // failure diagnostic keeps the actionable alignment guidance.
    {
        UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentSurfaceMisaligned"));
        MtoUEditorTest::FMtoUFullCharacterFixtures Fixtures;
        TestTrue(TEXT("misaligned-resolution fixtures are created"),
            MtoUEditorTest::MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures));
        if (Fixtures.IsValid())
        {
            UE::Geometry::FDynamicMesh3 Driver;
            UE::Geometry::FDynamicMesh3 PreviewMesh;
            TestTrue(TEXT("misaligned sources convert for resolution"),
                ConvertSourceMeshes(*Fixtures.FullDriver, *Fixtures.Preview, Driver, PreviewMesh));
            const double Offset = PreviewMesh.GetBounds().DiagonalLength() * 2.0;
            for (const int32 VertexID : PreviewMesh.VertexIndicesItr())
            {
                PreviewMesh.SetVertex(VertexID,
                    PreviewMesh.GetVertex(VertexID) + FVector3d(Offset, 0.0, 0.0));
            }
            const FMtoUDriverGarmentSurfaceResult Misaligned = ResolveSurface(
                Driver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
            AddInfo(Misaligned.Diagnostics);
            TestTrue(TEXT("a misaligned Preview fails automatic coverage"),
                !Misaligned.bSucceeded
                && HasNoUsableSurface(Misaligned)
                && Misaligned.MatchedPreviewCoverage < 0.001
                && Misaligned.Diagnostics.Contains(TEXT("no unique separable Driver surface"))
                && Misaligned.Diagnostics.Contains(TEXT("misaligned")));
        }
    }
    return true;
}

#endif
