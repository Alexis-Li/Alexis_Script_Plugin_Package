#if WITH_DEV_AUTOMATION_TESTS

#include "MtoUDriverGarmentSurface.h"
#include "MtoULiveLinkEditorTestFixtures.h"

#include "MtoULiveLinkBinding.h"
#include "MtoULiveLinkPreviewDetail.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMeshEditor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UDynamicMesh.h"

#include <limits>

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

/**
 * Gives the Driver description one polygon group per material slot, each named
 * with that slot's stable imported identity, the way a fresh FBX import does.
 */
FMeshDescription* WriteImportedSlotIdentities(
    USkeletalMesh& DriverAsset,
    TPolygonGroupAttributesRef<FName>& OutSlotNames)
{
    FMeshDescription* Description = DriverAsset.GetMeshDescription(0);
    if (!Description)
    {
        return nullptr;
    }
    while (Description->PolygonGroups().Num() < DriverAsset.GetMaterials().Num())
    {
        Description->CreatePolygonGroup();
    }
    FStaticMeshAttributes Attributes(*Description);
    OutSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
    for (int32 SlotIndex = 0; SlotIndex < DriverAsset.GetMaterials().Num(); ++SlotIndex)
    {
        const FSkeletalMaterial& Slot = DriverAsset.GetMaterials()[SlotIndex];
        OutSlotNames[FPolygonGroupID(SlotIndex)] = Slot.ImportedMaterialSlotName != NAME_None
            ? Slot.ImportedMaterialSlotName
            : Slot.MaterialSlotName;
    }
    return Description;
}

int32 FindPolygonGroupOrdinal(const USkeletalMesh& DriverAsset, const int32 SlotIndex)
{
    const FMeshDescription* Description = DriverAsset.GetMeshDescription(0);
    if (!Description || !DriverAsset.GetMaterials().IsValidIndex(SlotIndex))
    {
        return INDEX_NONE;
    }
    const FSkeletalMaterial& Slot = DriverAsset.GetMaterials()[SlotIndex];
    const FName SlotIdentity = Slot.ImportedMaterialSlotName != NAME_None
        ? Slot.ImportedMaterialSlotName
        : Slot.MaterialSlotName;
    FStaticMeshConstAttributes Attributes(*Description);
    const TPolygonGroupAttributesConstRef<FName> SlotNames =
        Attributes.GetPolygonGroupMaterialSlotNames();
    for (const FPolygonGroupID GroupID : Description->PolygonGroups().GetElementIDs())
    {
        if (SlotNames[GroupID] == SlotIdentity)
        {
            return GroupID.GetValue();
        }
    }
    return INDEX_NONE;
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

    TPolygonGroupAttributesRef<FName> DescriptionSlotNames;
    FMeshDescription* Description =
        WriteImportedSlotIdentities(*Fixtures.FullDriver, DescriptionSlotNames);
    TestNotNull(TEXT("material-slot fixtures have a Driver mesh description"), Description);
    if (!Description)
    {
        return false;
    }

    UE::Geometry::FDynamicMesh3 ReorderedDriver(FullDriver);
    ReorderedDriver.Attributes()->EnableMaterialID();
    const int32 BodyGroupOrdinal =
        FindPolygonGroupOrdinal(*Fixtures.FullDriver, 0);
    const int32 GarmentGroupOrdinal =
        FindPolygonGroupOrdinal(*Fixtures.FullDriver, 3);
    TestTrue(TEXT("reordered-slot fixture finds imported polygon groups"),
        BodyGroupOrdinal != INDEX_NONE
            && GarmentGroupOrdinal != INDEX_NONE
            && Fixtures.FullDriver->GetMaterials().IsValidIndex(BodyGroupOrdinal)
            && Fixtures.FullDriver->GetMaterials().IsValidIndex(GarmentGroupOrdinal));
    if (BodyGroupOrdinal == INDEX_NONE
        || GarmentGroupOrdinal == INDEX_NONE
        || !Fixtures.FullDriver->GetMaterials().IsValidIndex(BodyGroupOrdinal)
        || !Fixtures.FullDriver->GetMaterials().IsValidIndex(GarmentGroupOrdinal))
    {
        AddError(TEXT("reordered-slot fixture did not produce safe material-group ordinals"));
        return false;
    }

    UE::Geometry::FDynamicMesh3 ConflictingMetadataDriver(FullDriver);
    ConflictingMetadataDriver.Attributes()->EnableMaterialID();
    for (const int32 TriangleID : ConflictingMetadataDriver.TriangleIndicesItr())
    {
        ConflictingMetadataDriver.Attributes()->GetMaterialID()->SetValue(
            TriangleID, BodyGroupOrdinal);
        ConflictingMetadataDriver.SetTriangleGroup(TriangleID, GarmentGroupOrdinal);
    }
    const FMtoUDriverGarmentSurfaceResult ConflictingMetadata = ResolveSurface(
        ConflictingMetadataDriver,
        *Fixtures.FullDriver,
        PreviewMesh,
        *Fixtures.Preview,
        {});
    TestTrue(TEXT("a single collapsed group with conflicting current mappings fails transactionally"),
        !ConflictingMetadata.bSucceeded
            && HasNoUsableSurface(ConflictingMetadata)
            && ConflictingMetadata.Diagnostics.Contains(TEXT("conflicting current Driver material metadata")));

    UE::Geometry::FDynamicMesh3 PartiallyUnmappableCollapsedDriver(FullDriver);
    PartiallyUnmappableCollapsedDriver.Attributes()->EnableMaterialID();
    bool bInjectedUnmappableGroup = false;
    for (const int32 TriangleID : PartiallyUnmappableCollapsedDriver.TriangleIndicesItr())
    {
        PartiallyUnmappableCollapsedDriver.Attributes()->GetMaterialID()->SetValue(
            TriangleID, BodyGroupOrdinal);
        PartiallyUnmappableCollapsedDriver.SetTriangleGroup(
            TriangleID,
            bInjectedUnmappableGroup ? BodyGroupOrdinal : 99);
        bInjectedUnmappableGroup = true;
    }
    const FMtoUDriverGarmentSurfaceResult PartiallyUnmappableCollapsed = ResolveSurface(
        PartiallyUnmappableCollapsedDriver,
        *Fixtures.FullDriver,
        PreviewMesh,
        *Fixtures.Preview,
        {});
    TestTrue(TEXT("a collapsed group with one unmappable triangle group fails transactionally"),
        bInjectedUnmappableGroup
            && !PartiallyUnmappableCollapsed.bSucceeded
            && HasNoUsableSurface(PartiallyUnmappableCollapsed)
            && PartiallyUnmappableCollapsed.Diagnostics.Contains(TEXT("group(s) [99]")));

    for (const int32 TriangleID : ReorderedDriver.TriangleIndicesItr())
    {
        ReorderedDriver.Attributes()->GetMaterialID()->SetValue(
            TriangleID,
            Auto.Surface.IsTriangle(TriangleID) ? GarmentGroupOrdinal : BodyGroupOrdinal);
    }
    Fixtures.FullDriver->GetMaterials().Swap(0, 3);
    const FMtoUDriverGarmentSurfaceResult ReorderedSlots =
        ResolveSurface(ReorderedDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    Fixtures.FullDriver->GetMaterials().Swap(0, 3);
    TestTrue(TEXT("automatic resolution maps imported polygon groups to reordered material slots"),
        ReorderedSlots.bSucceeded
            && ReorderedSlots.MaterialSlotIndices == TArray<int32>({0}));

    FSkeletalMaterial UnusedSlot;
    UnusedSlot.MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
    UnusedSlot.MaterialSlotName = FName(TEXT("Unused_Before_Garment"));
    UnusedSlot.ImportedMaterialSlotName = UnusedSlot.MaterialSlotName;
    Fixtures.FullDriver->GetMaterials().Insert(UnusedSlot, 0);
    const FMtoUDriverGarmentSurfaceResult UnusedSlotResult =
        ResolveSurface(ReorderedDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    Fixtures.FullDriver->GetMaterials().RemoveAt(0);
    TestTrue(TEXT("an unused material slot cannot shift the resolved garment identity"),
        UnusedSlotResult.bSucceeded
            && UnusedSlotResult.MaterialSlotIndices == TArray<int32>({4}));

    UE::Geometry::FDynamicMesh3 SameOrdinalDriver(FullDriver);
    SameOrdinalDriver.Attributes()->EnableMaterialID();
    for (const int32 TriangleID : SameOrdinalDriver.TriangleIndicesItr())
    {
        SameOrdinalDriver.Attributes()->GetMaterialID()->SetValue(
            TriangleID, GarmentGroupOrdinal);
        SameOrdinalDriver.SetTriangleGroup(TriangleID, GarmentGroupOrdinal);
    }
    const FMtoUDriverGarmentSurfaceResult SameOrdinalSharedSlot =
        ResolveSurface(SameOrdinalDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    TestTrue(TEXT("same-ordinal garment and visible geometry fail transactionally"),
        !SameOrdinalSharedSlot.bSucceeded
            && HasNoUsableSurface(SameOrdinalSharedSlot)
            && SameOrdinalSharedSlot.Diagnostics.Contains(TEXT("shares material slot")));

    TMap<int32, FName> OriginalGroupNames;
    for (const FPolygonGroupID GroupID : Description->PolygonGroups().GetElementIDs())
    {
        OriginalGroupNames.Add(GroupID.GetValue(), DescriptionSlotNames[GroupID]);
        if (GroupID.GetValue() != BodyGroupOrdinal)
        {
            DescriptionSlotNames[GroupID] = NAME_None;
        }
    }
    UE::Geometry::FDynamicMesh3 CollapsedDriver(FullDriver);
    CollapsedDriver.Attributes()->EnableMaterialID();
    for (const int32 TriangleID : CollapsedDriver.TriangleIndicesItr())
    {
        CollapsedDriver.Attributes()->GetMaterialID()->SetValue(
            TriangleID, BodyGroupOrdinal);
    }
    const FMtoUDriverGarmentSurfaceResult CollapsedGroups =
        ResolveSurface(CollapsedDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    FSkeletalMeshLODInfo* LODInfo = Fixtures.FullDriver->GetLODInfo(0);
    const TArray<int32> OriginalLODMaterialMap = LODInfo
        ? LODInfo->LODMaterialMap
        : TArray<int32>();
    if (LODInfo)
    {
        LODInfo->LODMaterialMap.Init(
            INDEX_NONE, Fixtures.FullDriver->GetMaterials().Num());
        LODInfo->LODMaterialMap[BodyGroupOrdinal] = GarmentGroupOrdinal;
    }
    const FMtoUDriverGarmentSurfaceResult CollapsedSharedSlot =
        ResolveSurface(CollapsedDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    if (LODInfo)
    {
        LODInfo->LODMaterialMap = OriginalLODMaterialMap;
    }
    for (const TPair<int32, FName>& Entry : OriginalGroupNames)
    {
        DescriptionSlotNames[FPolygonGroupID(Entry.Key)] = Entry.Value;
    }
    TestTrue(TEXT("collapsed polygon groups use current LOD material metadata"),
        CollapsedGroups.bSucceeded
            && CollapsedGroups.MaterialSlotIndices == TArray<int32>({3, 4, 5}));
    TestTrue(TEXT("triangle-group fallback rejects a shared final material slot"),
        !CollapsedSharedSlot.bSucceeded
            && HasNoUsableSurface(CollapsedSharedSlot)
            && CollapsedSharedSlot.Diagnostics.Contains(TEXT("shares material slot")));

    UE::Geometry::FDynamicMesh3 UnmappableDriver(FullDriver);
    UnmappableDriver.Attributes()->EnableMaterialID();
    for (const int32 TriangleID : UnmappableDriver.TriangleIndicesItr())
    {
        if (Auto.Surface.IsTriangle(TriangleID))
        {
            UnmappableDriver.Attributes()->GetMaterialID()->SetValue(TriangleID, 99);
            UnmappableDriver.SetTriangleGroup(TriangleID, 99);
        }
    }
    const FMtoUDriverGarmentSurfaceResult Unmappable =
        ResolveSurface(UnmappableDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    TestTrue(TEXT("unmappable garment material metadata fails transactionally"),
        !Unmappable.bSucceeded
            && HasNoUsableSurface(Unmappable)
            && Unmappable.Diagnostics.Contains(TEXT("group(s) [99]"))
            && Unmappable.Diagnostics.Contains(TEXT("cannot be mapped uniquely")));

    UE::Geometry::FDynamicMesh3 UnmappableVisibleDriver(FullDriver);
    UnmappableVisibleDriver.Attributes()->EnableMaterialID();
    for (const int32 TriangleID : UnmappableVisibleDriver.TriangleIndicesItr())
    {
        if (!Auto.Surface.IsTriangle(TriangleID))
        {
            UnmappableVisibleDriver.Attributes()->GetMaterialID()->SetValue(TriangleID, 99);
            break;
        }
    }
    const FMtoUDriverGarmentSurfaceResult UnmappableVisible =
        ResolveSurface(UnmappableVisibleDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
    TestTrue(TEXT("unmappable visible material metadata fails transactionally"),
        !UnmappableVisible.bSucceeded
            && HasNoUsableSurface(UnmappableVisible)
            && UnmappableVisible.Diagnostics.Contains(TEXT("group(s) [99]")));

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

    if (Description)
    {
        FStaticMeshAttributes Attributes(*Description);
        TPolygonGroupAttributesRef<FName> SlotNames =
            Attributes.GetPolygonGroupMaterialSlotNames();
        const FPolygonGroupID BodyGroup(BodyGroupOrdinal);
        const FName OriginalBodySlot = SlotNames[BodyGroup];
        SlotNames[BodyGroup] = SlotNames[FPolygonGroupID(GarmentGroupOrdinal)];
        UE::Geometry::FDynamicMesh3 SharedSlotDriver;
        TestTrue(TEXT("shared-slot Driver converts for resolution"),
            ConvertSourceMeshes(
                *Fixtures.FullDriver, *Fixtures.Preview, SharedSlotDriver, PreviewMesh));
        SharedSlotDriver.Attributes()->EnableMaterialID();
        for (const int32 TriangleID : SharedSlotDriver.TriangleIndicesItr())
        {
            SharedSlotDriver.Attributes()->GetMaterialID()->SetValue(
                TriangleID,
                Auto.Surface.IsTriangle(TriangleID) ? GarmentGroupOrdinal : BodyGroupOrdinal);
        }
        const FMtoUDriverGarmentSurfaceResult SharedSlot = ResolveSurface(
            SharedSlotDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
        SlotNames[BodyGroup] = OriginalBodySlot;
        TestFalse(TEXT("automatic resolution rejects a garment slot shared by visible character geometry"),
            SharedSlot.bSucceeded);
        TestTrue(TEXT("shared-slot failure explains why complete display is unsafe"),
            SharedSlot.Diagnostics.Contains(TEXT("shares material slot")));
    }
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

    TPolygonGroupAttributesRef<FName> DescriptionSlotNames;
    FMeshDescription* Description =
        WriteImportedSlotIdentities(*Fixtures.FullDriver, DescriptionSlotNames);
    TestNotNull(TEXT("manual remap fixture has a Driver mesh description"), Description);
    if (!Description)
    {
        return false;
    }

    const int32 BodyGroupOrdinal = FindPolygonGroupOrdinal(*Fixtures.FullDriver, 0);
    const int32 GarmentGroupOrdinal = FindPolygonGroupOrdinal(*Fixtures.FullDriver, 3);
    TestTrue(TEXT("manual remap fixture finds distinct source groups"),
        BodyGroupOrdinal != INDEX_NONE
            && GarmentGroupOrdinal != INDEX_NONE
            && BodyGroupOrdinal != GarmentGroupOrdinal);
    if (BodyGroupOrdinal == INDEX_NONE
        || GarmentGroupOrdinal == INDEX_NONE
        || BodyGroupOrdinal == GarmentGroupOrdinal
        || !Fixtures.FullDriver->GetMaterials().IsValidIndex(BodyGroupOrdinal)
        || !Fixtures.FullDriver->GetMaterials().IsValidIndex(GarmentGroupOrdinal))
    {
        AddError(TEXT("manual remap fixture did not produce safe material-group ordinals"));
        return false;
    }
    FSkeletalMeshLODInfo* LODInfo = Fixtures.FullDriver->GetLODInfo(0);
    if (!LODInfo)
    {
        AddError(TEXT("manual remap fixture has no LOD0 material mapping"));
        return false;
    }
    UE::Geometry::FDynamicMesh3 LODRemappedDriver(Driver);
    LODRemappedDriver.Attributes()->EnableMaterialID();
    for (const int32 TriangleID : LODRemappedDriver.TriangleIndicesItr())
    {
        LODRemappedDriver.Attributes()->GetMaterialID()->SetValue(
            TriangleID, BodyGroupOrdinal);
        LODRemappedDriver.SetTriangleGroup(
            TriangleID,
            Manual.Surface.IsTriangle(TriangleID)
                ? BodyGroupOrdinal
                : GarmentGroupOrdinal);
    }
    const TArray<int32> OriginalLODMaterialMap = LODInfo->LODMaterialMap;
    LODInfo->LODMaterialMap.Init(INDEX_NONE, Fixtures.FullDriver->GetMaterials().Num());
    LODInfo->LODMaterialMap[BodyGroupOrdinal] = 3;
    LODInfo->LODMaterialMap[GarmentGroupOrdinal] = 0;
    const FMtoUDriverGarmentSurfaceResult LODRemappedManual = ResolveSurface(
        LODRemappedDriver,
        *Fixtures.FullDriver,
        PreviewMesh,
        *Fixtures.Preview,
        {UpperA});
    LODInfo->LODMaterialMap = OriginalLODMaterialMap;
    TestTrue(TEXT("manual triangle groups resolve through the current LOD material map"),
        LODRemappedManual.bSucceeded
            && LODRemappedManual.MaterialSlotIndices == TArray<int32>({3})
            && LODRemappedManual.TriangleCount == Manual.TriangleCount);

    UE::Geometry::FDynamicMesh3 ConflictingMetadataDriver(Driver);
    ConflictingMetadataDriver.Attributes()->EnableMaterialID();
    for (const int32 TriangleID : ConflictingMetadataDriver.TriangleIndicesItr())
    {
        ConflictingMetadataDriver.Attributes()->GetMaterialID()->SetValue(
            TriangleID, GarmentGroupOrdinal);
        ConflictingMetadataDriver.SetTriangleGroup(TriangleID, BodyGroupOrdinal);
    }
    const FMtoUDriverGarmentSurfaceResult ConflictingMetadata = ResolveSurface(
        ConflictingMetadataDriver,
        *Fixtures.FullDriver,
        PreviewMesh,
        *Fixtures.Preview,
        {UpperA});
    TestTrue(TEXT("manual override rejects conflicting current material metadata"),
        !ConflictingMetadata.bSucceeded
            && HasNoUsableSurface(ConflictingMetadata)
            && ConflictingMetadata.Diagnostics.Contains(TEXT("conflicting current Driver material metadata")));

    // A collapsed source must also prove every triangle in Manual mode: one
    // unmappable current-LOD triangle group blocks the refresh even though
    // all remaining metadata agrees on the overridden slot.
    const FName OriginalBodyGroupName =
        DescriptionSlotNames[FPolygonGroupID(BodyGroupOrdinal)];
    DescriptionSlotNames[FPolygonGroupID(BodyGroupOrdinal)] =
        DescriptionSlotNames[FPolygonGroupID(GarmentGroupOrdinal)];
    UE::Geometry::FDynamicMesh3 PartiallyUnmappableManualDriver(Driver);
    PartiallyUnmappableManualDriver.Attributes()->EnableMaterialID();
    bool bInjectedUnmappableGroup = false;
    for (const int32 TriangleID : PartiallyUnmappableManualDriver.TriangleIndicesItr())
    {
        PartiallyUnmappableManualDriver.Attributes()->GetMaterialID()->SetValue(
            TriangleID, BodyGroupOrdinal);
        PartiallyUnmappableManualDriver.SetTriangleGroup(
            TriangleID,
            bInjectedUnmappableGroup ? GarmentGroupOrdinal : 99);
        bInjectedUnmappableGroup = true;
    }
    const FMtoUDriverGarmentSurfaceResult PartiallyUnmappableManual = ResolveSurface(
        PartiallyUnmappableManualDriver,
        *Fixtures.FullDriver,
        PreviewMesh,
        *Fixtures.Preview,
        {UpperA});
    DescriptionSlotNames[FPolygonGroupID(BodyGroupOrdinal)] = OriginalBodyGroupName;
    AddInfo(PartiallyUnmappableManual.Diagnostics);
    TestTrue(TEXT("manual override fails closed when one collapsed triangle group cannot be mapped"),
        bInjectedUnmappableGroup
            && !PartiallyUnmappableManual.bSucceeded
            && HasNoUsableSurface(PartiallyUnmappableManual)
            && PartiallyUnmappableManual.Diagnostics.Contains(TEXT("cannot be mapped uniquely"))
            && PartiallyUnmappableManual.Diagnostics.Contains(TEXT("group(s) [99]")));

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

/**
 * Builds the material-evidence disambiguation fixture: a full-character-style
 * Driver whose garment copy A carries the Preview's material-slot identity
 * while a shifted duplicate copy B does not. The Driver's final material
 * slots are reordered AFTER import, so polygon-group and triangle-group
 * ordinals no longer line up with the supported slots; only the shared
 * per-triangle final-slot resolver can feed the evidence pass correctly.
 */
struct FMtoUMaterialEvidenceFixture
{
    USkeletalMesh* Driver = nullptr;
    UStaticMesh* Preview = nullptr;
    /** Final material-slot indices of garment copy A after the reorder. */
    TArray<int32> ExpectedSlotIndices;

    bool IsValid() const { return Driver && Preview; }
};

bool MakeMaterialEvidenceFixtures(
    UObject& Outer, FAutomationTestBase& Test, FMtoUMaterialEvidenceFixture& Fixtures)
{
    using MtoUEditorTest::AppendPartCopy;
    using MtoUEditorTest::SetUniformBoneWeights;
    using MtoUEditorTest::WriteTransientDriver;
    USkeletalMesh* Base = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    if (!Base)
    {
        Test.AddError(TEXT("material-evidence fixture SkeletalCube was not loaded"));
        return false;
    }
    UDynamicMesh* BodySource = NewObject<UDynamicMesh>(&Outer);
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
        Test.AddError(TEXT("material-evidence fixture body conversion failed"));
        return false;
    }
    const UE::Geometry::FDynamicMesh3& CubeSource = BodySource->GetMeshRef();
    // Weld and poke the cube the way the full-character fixture does: every
    // face gets an interior vertex, so nearest-surface ownership cannot drop
    // a face through shared-corner ties, and each cube becomes one region.
    UE::Geometry::FDynamicMesh3 Cube(CubeSource);
    UE::Geometry::FMergeCoincidentMeshEdges WeldCube(&Cube);
    if (!WeldCube.Apply())
    {
        Test.AddError(TEXT("material-evidence fixture SkeletalCube was not welded"));
        return false;
    }
    TArray<int32> CubeTriangles;
    for (const int32 TriangleID : Cube.TriangleIndicesItr())
    {
        CubeTriangles.Add(TriangleID);
    }
    for (const int32 TriangleID : CubeTriangles)
    {
        UE::Geometry::FDynamicMesh3::FPokeTriangleInfo PokeInfo;
        Cube.PokeTriangle(TriangleID, PokeInfo);
    }
    const FVector3d BodyCenter = Cube.GetBounds().Center();
    const double BodyHeight = Cube.GetBounds().Height();
    const double UpperScale = 1.15;
    const double LowerScale = 0.55;
    const FVector3d LowerOffset =
        BodyCenter - FVector3d(0.0, 0.0, BodyHeight * 1.05);

    // The Preview holds only garment copy A's surface.
    UE::Geometry::FDynamicMesh3 PreviewGeometry;
    AppendPartCopy(PreviewGeometry, Cube, FVector3d::Zero(), UpperScale, 0);
    AppendPartCopy(PreviewGeometry, Cube, LowerOffset, LowerScale, 1);
    // Copy B is offset just outside the twin proximity (2% of the scale) but
    // still inside the agreement radius (5%), so geometry alone cannot decide
    // between the copies and only the material evidence can pick the right one.
    const double Shift = 0.03 * PreviewGeometry.GetBounds().DiagonalLength();

    UE::Geometry::FDynamicMesh3 Geometry;
    // Appending pieces with distinct group IDs only persists them when the
    // groups buffer is enabled up front; a default mesh drops every group.
    Geometry.EnableTriangleGroups();
    AppendPartCopy(Geometry, Cube, FVector3d::Zero(), 0.9, 0);
    AppendPartCopy(Geometry, Cube, FVector3d::Zero(), UpperScale, 1);
    AppendPartCopy(Geometry, Cube, LowerOffset, LowerScale, 2);
    AppendPartCopy(Geometry, Cube, FVector3d(Shift, 0.0, 0.0), UpperScale, 3);
    AppendPartCopy(Geometry, Cube, LowerOffset + FVector3d(Shift, 0.0, 0.0), LowerScale, 4);
    SetUniformBoneWeights(Geometry);
    Fixtures.Driver = WriteTransientDriver(Outer, *Base, Geometry,
        {
            FName(TEXT("Body")),
            FName(TEXT("Cloth09_Top_1")),
            FName(TEXT("Cloth09_Bottom_2")),
            FName(TEXT("Dup09_Top_1")),
            FName(TEXT("Dup09_Bottom_2")),
        });
    if (!Fixtures.Driver)
    {
        Test.AddError(TEXT("material-evidence Driver was not written"));
        return false;
    }
    // The cloth identities live at slots 3 and 4, the duplicates at 1 and 2.
    // The final material slots are reordered AFTER import by the test, the way
    // the automatic-resolution reorder case does, so any path that reads a
    // polygon-group or triangle-group ordinal as a slot index selects the
    // wrong copy. Material-asset equality must not mask the name evidence:
    // only the cloth identities of copy A match the Preview slot names.
    Fixtures.ExpectedSlotIndices = {3, 4};
    for (FSkeletalMaterial& Slot : Fixtures.Driver->GetMaterials())
    {
        Slot.MaterialInterface = nullptr;
    }

    Fixtures.Preview = NewObject<UStaticMesh>(&Outer, NAME_None, RF_Transient);
    FGeometryScriptCopyMeshToAssetOptions PreviewWriteOptions;
    PreviewWriteOptions.bEmitTransaction = false;
    PreviewWriteOptions.bEnableRecomputeNormals = true;
    PreviewWriteOptions.bEnableRecomputeTangents = true;
    PreviewWriteOptions.bReplaceMaterials = true;
    PreviewWriteOptions.NewMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
    PreviewWriteOptions.NewMaterialSlotNames.Add(FName(TEXT("Cloth09_Top_1")));
    PreviewWriteOptions.NewMaterials.Add(nullptr);
    PreviewWriteOptions.NewMaterialSlotNames.Add(FName(TEXT("Cloth09_Bottom_2")));
    FGeometryScriptMeshWriteLOD PreviewWriteLOD;
    EGeometryScriptOutcomePins PreviewOutcome = EGeometryScriptOutcomePins::Failure;
    UDynamicMesh* PreviewSource = NewObject<UDynamicMesh>(&Outer);
    PreviewSource->SetMesh(MoveTemp(PreviewGeometry));
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
        PreviewSource, Fixtures.Preview, PreviewWriteOptions, PreviewWriteLOD, PreviewOutcome, false);
    if (PreviewOutcome != EGeometryScriptOutcomePins::Success)
    {
        Test.AddError(TEXT("material-evidence Preview was not written"));
        return false;
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUDriverGarmentSurfaceMaterialEvidenceTest,
    "MtoULiveLink.Editor.GarmentSurface.MaterialEvidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUDriverGarmentSurfaceMaterialEvidenceTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentSurfaceMaterialEvidence"));
    FMtoUMaterialEvidenceFixture Fixtures;
    TestTrue(TEXT("material-evidence fixtures are created"),
        MakeMaterialEvidenceFixtures(*WorldPackage, *this, Fixtures));
    if (!Fixtures.IsValid())
    {
        AddError(TEXT("material-evidence fixtures were not created"));
        return false;
    }
    UE::Geometry::FDynamicMesh3 Driver;
    UE::Geometry::FDynamicMesh3 PreviewMesh;
    TestTrue(TEXT("material-evidence sources convert for resolution"),
        ConvertSourceMeshes(*Fixtures.Driver, *Fixtures.Preview, Driver, PreviewMesh));

    // Rebuild the import-time material evidence the way the automatic
    // resolution reorder case does: the Geometry Script round trip collapses
    // polygon groups, so name the stored polygon groups with the imported
    // slot identities and restore the per-section material ordinals from the
    // preserved triangle groups. The final material slots are then reordered
    // AFTER import, so only the shared per-triangle final-slot resolver can
    // feed the evidence pass correctly.
    TPolygonGroupAttributesRef<FName> DescriptionSlotNames;
    FMeshDescription* Description =
        WriteImportedSlotIdentities(*Fixtures.Driver, DescriptionSlotNames);
    TestNotNull(TEXT("material-evidence Driver has import-time polygon groups"), Description);
    if (!Description)
    {
        return false;
    }
    if (Driver.Attributes() && Driver.Attributes()->GetMaterialID())
    {
        for (const int32 TriangleID : Driver.TriangleIndicesItr())
        {
            Driver.Attributes()->GetMaterialID()->SetValue(
                TriangleID, Driver.GetTriangleGroup(TriangleID));
        }
    }
    Fixtures.Driver->GetMaterials().Swap(1, 3);
    Fixtures.Driver->GetMaterials().Swap(2, 4);
    const FMtoUDriverGarmentSurfaceResult Evidence = ResolveSurface(
        Driver, *Fixtures.Driver, PreviewMesh, *Fixtures.Preview, {});
    AddInfo(Evidence.Diagnostics);
    TestTrue(TEXT("reordered slots select the correct garment copy through final-slot evidence"),
        Evidence.bSucceeded
            && !Evidence.bManualSource
            && Evidence.MaterialSlotIndices == Fixtures.ExpectedSlotIndices
            && Evidence.RegionSummary.Contains(TEXT("material evidence narrowed")));
    TestTrue(TEXT("the evidence-selected surface keeps full Preview coverage"),
        Evidence.MatchedPreviewCoverage > 0.999);
    TestTrue(TEXT("the evidence-selected surface contains only garment copy A"),
        Evidence.TriangleCount == PreviewMesh.TriangleCount());
    TestTrue(TEXT("the evidence-selected surface preserves Driver vertex correspondence"),
        PreservesDriverVertexCorrespondence(Driver, Evidence.Surface));

    // Removing the material evidence (renaming the Preview slots) must fall
    // back to geometry ownership, which still selects copy A alone: the
    // shifted copy owns no Preview vertex and is not a twin. Evidence must
    // never be required, only ever narrow candidates correctly.
    for (FStaticMaterial& Slot : Fixtures.Preview->GetStaticMaterials())
    {
        Slot.ImportedMaterialSlotName = NAME_None;
        Slot.MaterialSlotName = FName(TEXT("Renamed_Cloth"));
    }
    const FMtoUDriverGarmentSurfaceResult NoEvidence = ResolveSurface(
        Driver, *Fixtures.Driver, PreviewMesh, *Fixtures.Preview, {});
    AddInfo(NoEvidence.Diagnostics);
    TestTrue(TEXT("without discriminating evidence geometry still selects the correct copy"),
        NoEvidence.bSucceeded
            && NoEvidence.MaterialSlotIndices == Fixtures.ExpectedSlotIndices
            && NoEvidence.TriangleCount == PreviewMesh.TriangleCount()
            && !NoEvidence.RegionSummary.Contains(TEXT("material evidence narrowed")));
    Fixtures.Driver->GetMaterials().Swap(1, 3);
    Fixtures.Driver->GetMaterials().Swap(2, 4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUDriverGarmentSurfaceGeometryEdgeCasesTest,
    "MtoULiveLink.Editor.GarmentSurface.GeometryEdgeCases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUDriverGarmentSurfaceGeometryEdgeCasesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentSurfaceGeometryEdgeCases"));
    MtoUEditorTest::FMtoUFullCharacterFixtures Fixtures;
    TestTrue(TEXT("geometry-edge-case fixtures are created"),
        MtoUEditorTest::MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures));
    if (!Fixtures.IsValid())
    {
        AddError(TEXT("geometry-edge-case fixtures were not created"));
        return false;
    }
    UE::Geometry::FDynamicMesh3 Driver;
    UE::Geometry::FDynamicMesh3 PreviewMesh;
    TestTrue(TEXT("geometry-edge-case sources convert for resolution"),
        ConvertSourceMeshes(*Fixtures.FullDriver, *Fixtures.Preview, Driver, PreviewMesh));

    // A Preview with vertices but no triangles must fail cleanly before any
    // mass accounting can divide by zero or report a misleading outcome.
    {
        UE::Geometry::FDynamicMesh3 ZeroTrianglePreview;
        for (int32 Index = 0; Index < 12; ++Index)
        {
            ZeroTrianglePreview.AppendVertex(FVector3d(Index * 0.5, 0.0, 0.0));
        }
        const FMtoUDriverGarmentSurfaceResult ZeroTriangle = ResolveSurface(
            Driver, *Fixtures.FullDriver, ZeroTrianglePreview, *Fixtures.Preview, {});
        AddInfo(ZeroTriangle.Diagnostics);
        TestTrue(TEXT("a zero-triangle Preview fails cleanly without mass accounting"),
            !ZeroTriangle.bSucceeded
                && HasNoUsableSurface(ZeroTriangle)
                && ZeroTriangle.Diagnostics.Contains(TEXT("no LOD0 triangles")));
    }

    // A degenerate Preview whose distinct vertices all coincide has no usable
    // scale and fails cleanly instead of reporting a misleading outcome.
    {
        UE::Geometry::FDynamicMesh3 DegeneratePreview;
        const int32 DA = DegeneratePreview.AppendVertex(FVector3d(1.0, 2.0, 3.0));
        const int32 DB = DegeneratePreview.AppendVertex(FVector3d(1.0, 2.0, 3.0));
        const int32 DC = DegeneratePreview.AppendVertex(FVector3d(1.0, 2.0, 3.0));
        DegeneratePreview.AppendTriangle(DA, DB, DC);
        const FMtoUDriverGarmentSurfaceResult Degenerate = ResolveSurface(
            Driver, *Fixtures.FullDriver, DegeneratePreview, *Fixtures.Preview, {});
        AddInfo(Degenerate.Diagnostics);
        TestTrue(TEXT("a degenerate zero-size Preview fails cleanly"),
            !Degenerate.bSucceeded
                && HasNoUsableSurface(Degenerate)
                && Degenerate.Diagnostics.Contains(TEXT("no usable size")));
    }

    // A collinear triangle keeps a nonzero bounding box, finite coordinates,
    // and a positive triangle count, yet spans no surface; it must fail the
    // shared admission gate instead of entering coverage or mass accounting
    // with a false Ready (#32).
    {
        UE::Geometry::FDynamicMesh3 CollinearPreview;
        const int32 CA = CollinearPreview.AppendVertex(FVector3d(0.0, 0.0, 0.0));
        const int32 CB = CollinearPreview.AppendVertex(FVector3d(1.0, 0.0, 0.0));
        const int32 CC = CollinearPreview.AppendVertex(FVector3d(2.0, 0.0, 0.0));
        CollinearPreview.AppendTriangle(CA, CB, CC);
        TestTrue(TEXT("the collinear Preview keeps a nonzero bounding box"),
            CollinearPreview.GetBounds().DiagonalLength() > 1.0);
        const FMtoUDriverGarmentSurfaceResult Collinear = ResolveSurface(
            Driver, *Fixtures.FullDriver, CollinearPreview, *Fixtures.Preview, {});
        AddInfo(Collinear.Diagnostics);
        TestTrue(TEXT("a collinear nonzero-bounds Preview fails cleanly"),
            !Collinear.bSucceeded
                && HasNoUsableSurface(Collinear)
                && Collinear.Diagnostics.Contains(TEXT("no usable triangle area")));
    }
    {
        UE::Geometry::FDynamicMesh3 CollinearDriver;
        const int32 DA = CollinearDriver.AppendVertex(FVector3d(0.0, 0.0, 0.0));
        const int32 DB = CollinearDriver.AppendVertex(FVector3d(1.0, 0.0, 0.0));
        const int32 DC = CollinearDriver.AppendVertex(FVector3d(2.0, 0.0, 0.0));
        CollinearDriver.AppendTriangle(DA, DB, DC);
        const FMtoUDriverGarmentSurfaceResult CollinearDriverResult = ResolveSurface(
            CollinearDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
        AddInfo(CollinearDriverResult.Diagnostics);
        TestTrue(TEXT("a collinear nonzero-bounds Driver fails cleanly"),
            !CollinearDriverResult.bSucceeded
                && HasNoUsableSurface(CollinearDriverResult)
                && CollinearDriverResult.Diagnostics.Contains(TEXT("no usable triangle area")));
    }

    // NaN and infinite coordinates fail closed at the shared admission gate.
    // The public DynamicMesh API refuses non-finite SetVertex writes, so the
    // corrupted inputs are built by appending non-finite vertices directly —
    // the same way a broken asset conversion can produce them.
    const auto AppendNonFiniteMesh = [](const FVector3d& BadPosition)
    {
        UE::Geometry::FDynamicMesh3 Mesh;
        const int32 A = Mesh.AppendVertex(FVector3d(0.0, 0.0, 0.0));
        const int32 B = Mesh.AppendVertex(FVector3d(1.0, 0.0, 0.0));
        const int32 C = Mesh.AppendVertex(FVector3d(0.0, 1.0, 0.0));
        const int32 D = Mesh.AppendVertex(BadPosition);
        Mesh.AppendTriangle(A, B, C);
        Mesh.AppendTriangle(A, C, D);
        return Mesh;
    };
    {
        const UE::Geometry::FDynamicMesh3 NaNDriver = AppendNonFiniteMesh(
            FVector3d(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0));
        const FMtoUDriverGarmentSurfaceResult NaNDriverResult = ResolveSurface(
            NaNDriver, *Fixtures.FullDriver, PreviewMesh, *Fixtures.Preview, {});
        AddInfo(NaNDriverResult.Diagnostics);
        TestTrue(TEXT("a NaN-coordinate Driver fails transactionally"),
            !NaNDriverResult.bSucceeded
                && HasNoUsableSurface(NaNDriverResult)
                && NaNDriverResult.Diagnostics.Contains(TEXT("non-finite")));
    }
    {
        const UE::Geometry::FDynamicMesh3 NaNPreview = AppendNonFiniteMesh(
            FVector3d(0.0, std::numeric_limits<double>::quiet_NaN(), 0.0));
        const FMtoUDriverGarmentSurfaceResult NaNPreviewResult = ResolveSurface(
            Driver, *Fixtures.FullDriver, NaNPreview, *Fixtures.Preview, {});
        AddInfo(NaNPreviewResult.Diagnostics);
        TestTrue(TEXT("a NaN-coordinate Preview fails transactionally"),
            !NaNPreviewResult.bSucceeded
                && HasNoUsableSurface(NaNPreviewResult)
                && NaNPreviewResult.Diagnostics.Contains(TEXT("non-finite")));
    }
    {
        const UE::Geometry::FDynamicMesh3 InfinitePreview = AppendNonFiniteMesh(
            FVector3d(std::numeric_limits<double>::infinity(), 0.0, 0.0));
        const FMtoUDriverGarmentSurfaceResult InfinitePreviewResult = ResolveSurface(
            Driver, *Fixtures.FullDriver, InfinitePreview, *Fixtures.Preview, {});
        AddInfo(InfinitePreviewResult.Diagnostics);
        TestTrue(TEXT("an infinite-coordinate Preview fails transactionally"),
            !InfinitePreviewResult.bSucceeded
                && HasNoUsableSurface(InfinitePreviewResult)
                && InfinitePreviewResult.Diagnostics.Contains(TEXT("non-finite")));
    }

    // The mass-accounting floor is a Preview TRIANGLE count: a multi-region
    // Driver with a sub-floor Preview must resolve by geometry and evidence
    // instead of entering the source-mass gate on its vertex count.
    {
        using MtoUEditorTest::AppendPartCopy;
        using MtoUEditorTest::SetUniformBoneWeights;
        using MtoUEditorTest::WriteTransientDriver;
        USkeletalMesh* Base = LoadObject<USkeletalMesh>(
            nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
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
            AddError(TEXT("low-triangle fixture body conversion failed"));
            return false;
        }
        const UE::Geometry::FDynamicMesh3& Cube = BodySource->GetMeshRef();
        // Weld each cube into one connected region so a piece is selected as
        // a whole: per-face SkeletalCube fragments would leave unselected
        // faces in the garment slot and trip the shared-slot guard.
        UE::Geometry::FDynamicMesh3 WeldedCube(Cube);
        UE::Geometry::FMergeCoincidentMeshEdges Weld(&WeldedCube);
        if (!Weld.Apply())
        {
            AddError(TEXT("low-triangle fixture cube was not welded"));
            return false;
        }
        const FVector3d BodyCenter = WeldedCube.GetBounds().Center();
        const double BodyHeight = WeldedCube.GetBounds().Height();
        const FVector3d LowerOffset =
            BodyCenter - FVector3d(0.0, 0.0, BodyHeight * 1.05);

        // Multi-region Driver: welded garment, body, and lower cubes each
        // form one connected region with its own material slot.
        UE::Geometry::FDynamicMesh3 LowTriGeometry;
        LowTriGeometry.EnableTriangleGroups();
        const UE::Geometry::FAxisAlignedBox3d UpperBounds =
            AppendPartCopy(LowTriGeometry, WeldedCube, FVector3d::Zero(), 1.15, 1);
        AppendPartCopy(LowTriGeometry, WeldedCube, FVector3d::Zero(), 0.9, 0);
        AppendPartCopy(LowTriGeometry, WeldedCube, LowerOffset, 0.55, 2);
        SetUniformBoneWeights(LowTriGeometry);
        USkeletalMesh* LowTriDriver = WriteTransientDriver(*WorldPackage, *Base, LowTriGeometry,
            {
                FName(TEXT("LowTri_Body")),
                FName(TEXT("LowTri_Garment_Upper")),
                FName(TEXT("LowTri_Garment_Lower")),
            });
        TestNotNull(TEXT("low-triangle Driver was written"), LowTriDriver);
        if (!LowTriDriver)
        {
            return false;
        }

        // Sub-floor Preview: 8 vertices and 7 triangles as a fan lying on the
        // upper garment piece's top face. A vertex-count gate would enter the
        // mass check here (8 >= 8) and wrongly reject the 12-triangle piece
        // (12 > 1.7 * 7); the triangle-count gate exempts it.
        UE::Geometry::FDynamicMesh3 FanPreview;
        const double FanRadius = (UpperBounds.Max.X - UpperBounds.Min.X) * 0.4;
        const int32 FanCenter = FanPreview.AppendVertex(
            FVector3d(0.0, 0.0, UpperBounds.Max.Z));
        TArray<int32> FanRim;
        for (int32 Index = 0; Index < 7; ++Index)
        {
            const double Angle = UE_DOUBLE_PI * 2.0 * Index / 7.0;
            FanRim.Add(FanPreview.AppendVertex(FVector3d(
                FMath::Cos(Angle) * FanRadius,
                FMath::Sin(Angle) * FanRadius,
                UpperBounds.Max.Z)));
        }
        for (int32 Index = 0; Index < 7; ++Index)
        {
            FanPreview.AppendTriangle(FanCenter, FanRim[Index], FanRim[(Index + 1) % 7]);
        }
        TestTrue(TEXT("low-triangle Preview converts with 8 vertices and 7 triangles"),
            FanPreview.VertexCount() == 8 && FanPreview.TriangleCount() == 7);
        UStaticMesh* FanPreviewAsset = NewObject<UStaticMesh>(WorldPackage, NAME_None, RF_Transient);
        FGeometryScriptCopyMeshToAssetOptions PreviewWriteOptions;
        PreviewWriteOptions.bEmitTransaction = false;
        PreviewWriteOptions.bEnableRecomputeNormals = true;
        PreviewWriteOptions.bEnableRecomputeTangents = true;
        PreviewWriteOptions.bReplaceMaterials = true;
        PreviewWriteOptions.NewMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
        PreviewWriteOptions.NewMaterialSlotNames.Add(FName(TEXT("Fan_Preview")));
        FGeometryScriptMeshWriteLOD PreviewWriteLOD;
        EGeometryScriptOutcomePins PreviewOutcome = EGeometryScriptOutcomePins::Failure;
        UDynamicMesh* PreviewSource = NewObject<UDynamicMesh>(WorldPackage);
        PreviewSource->SetMesh(MoveTemp(FanPreview));
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
            PreviewSource, FanPreviewAsset, PreviewWriteOptions, PreviewWriteLOD, PreviewOutcome, false);
        if (PreviewOutcome != EGeometryScriptOutcomePins::Success)
        {
            AddError(TEXT("low-triangle Preview was not written"));
            return false;
        }

        UE::Geometry::FDynamicMesh3 LowTriDriverMesh;
        UE::Geometry::FDynamicMesh3 LowTriPreviewMesh;
        TestTrue(TEXT("low-triangle sources convert for resolution"),
            ConvertSourceMeshes(*LowTriDriver, *FanPreviewAsset,
                LowTriDriverMesh, LowTriPreviewMesh));
        TestTrue(TEXT("the converted sub-floor Preview keeps its triangle count"),
            LowTriPreviewMesh.TriangleCount() == 7);
        const FMtoUDriverGarmentSurfaceResult LowTri = ResolveSurface(
            LowTriDriverMesh, *LowTriDriver, LowTriPreviewMesh, *FanPreviewAsset, {});
        AddInfo(LowTri.Diagnostics);
        TestTrue(TEXT("a sub-floor Preview exempts the mass-accounting gate"),
            LowTri.bSucceeded
                && LowTri.MatchedPreviewCoverage > 0.999
                && LowTri.TriangleCount == 12
                && LowTri.MaterialSlotIndices == TArray<int32>({1})
                && !LowTri.Diagnostics.Contains(TEXT("source-mass boundary")));
    }
    return true;
}

#endif
