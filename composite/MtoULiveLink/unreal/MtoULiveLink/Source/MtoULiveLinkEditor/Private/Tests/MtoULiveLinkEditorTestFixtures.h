#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"

#include "Animation/MorphTarget.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicVertexSkinWeightsAttribute.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "DynamicMeshEditor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Selections/MeshConnectedComponents.h"
#include "SkeletalMeshAttributes.h"
#include "UDynamicMesh.h"

/**
 * Shared synthetic full-character and failure-boundary fixtures for the
 * Editor automation tests. Both the focused Driver garment-surface tests and
 * the outer Preview preparation tests resolve against these fixtures, so the
 * same evidence feeds the private seam and the public integration seam.
 */
namespace MtoUEditorTest
{
using UE::Geometry::FAxisAlignedBox3d;
using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FIndex3i;

/**
 * Appends one transformed copy of Source into Target under GroupID (split into
 * two polygon groups when GroupCount is 2) and reports the appended bounds.
 * When OutVertexMap is provided it receives the source-to-target vertex ID map
 * so callers can weld extra geometry onto the appended copy's real vertices.
 */
inline FAxisAlignedBox3d AppendPartCopy(
    FDynamicMesh3& Target,
    const FDynamicMesh3& Source,
    const FVector3d& Translation,
    const double Scale,
    const int32 GroupID,
    const int32 GroupCount = 1,
    TMap<int32, int32>* OutVertexMap = nullptr)
{
    FAxisAlignedBox3d Bounds;
    TMap<int32, int32> LocalVertexMap;
    TMap<int32, int32>& VertexMap = OutVertexMap ? *OutVertexMap : LocalVertexMap;
    int32 VisitedTriangles = 0;
    const int32 TotalTriangles = Source.TriangleCount();
    for (const int32 TriangleID : Source.TriangleIndicesItr())
    {
        ++VisitedTriangles;
        const int32 PartGroup = (GroupCount == 2 && VisitedTriangles * 2 > TotalTriangles)
            ? GroupID + 1
            : GroupID;
        FIndex3i Triangle = Source.GetTriangle(TriangleID);
        for (int32* Corner : {&Triangle.A, &Triangle.B, &Triangle.C})
        {
            const int32 SourceVertexID = *Corner;
            if (int32* Mapped = VertexMap.Find(SourceVertexID))
            {
                *Corner = *Mapped;
            }
            else
            {
                *Corner = Target.AppendVertex(
                    Source.GetVertex(SourceVertexID) * Scale + Translation);
                Bounds.Contain(Target.GetVertex(*Corner));
                VertexMap.Add(SourceVertexID, *Corner);
            }
        }
        Target.AppendTriangle(Triangle, PartGroup);
    }
    return Bounds;
}

/** Gives every vertex a single full influence on bone 0 so source validation passes. */
inline void SetUniformBoneWeights(FDynamicMesh3& Mesh)
{
    UE::AnimationCore::FBoneWeights Uniform;
    Uniform.SetBoneWeight(0, 1.0f);
    if (!Mesh.HasAttributes() || !Mesh.Attributes()->HasBones())
    {
        return;
    }
    UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* SkinWeights =
        Mesh.Attributes()->GetSkinWeightsAttribute(
            FSkeletalMeshAttributes::DefaultSkinWeightProfileName);
    if (!SkinWeights)
    {
        return;
    }
    for (const int32 VertexID : Mesh.VertexIndicesItr())
    {
        SkinWeights->SetValue(VertexID, Uniform);
    }
}

/** Writes Geometry as a transient Skeletal Mesh carrying SkeletonSource's reference skeleton, one slot per name. */
inline USkeletalMesh* WriteTransientDriver(
    UObject& Outer, USkeletalMesh& SkeletonSource,
    const FDynamicMesh3& Geometry, const TArray<FName>& SlotNames)
{
    USkeletalMesh* Driver = NewObject<USkeletalMesh>(
        &Outer, NAME_None, RF_Transient);
    Driver->SetSkeleton(SkeletonSource.GetSkeleton());
    Driver->SetRefSkeleton(SkeletonSource.GetRefSkeleton());
    Driver->CalculateInvRefMatrices();
    UDynamicMesh* Source = NewObject<UDynamicMesh>(&Outer);
    Source->SetMesh(FDynamicMesh3(Geometry));
    FGeometryScriptCopyMeshToAssetOptions WriteOptions;
    WriteOptions.bEmitTransaction = false;
    WriteOptions.bEnableRecomputeNormals = true;
    WriteOptions.bEnableRecomputeTangents = true;
    WriteOptions.bReplaceMaterials = true;
    WriteOptions.bUseBuildScale = false;
    WriteOptions.BoneHierarchyMismatchHandling =
        EGeometryScriptBoneHierarchyMismatchHandling::RemapGeometryToReferenceSkeleton;
    for (const FName SlotName : SlotNames)
    {
        WriteOptions.NewMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
        WriteOptions.NewMaterialSlotNames.Add(SlotName);
    }
    FGeometryScriptMeshWriteLOD WriteLOD;
    EGeometryScriptOutcomePins WriteOutcome = EGeometryScriptOutcomePins::Failure;
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToSkeletalMesh(
        Source, Driver, WriteOptions, WriteLOD, WriteOutcome);
    return WriteOutcome == EGeometryScriptOutcomePins::Success ? Driver : nullptr;
}

inline bool AddBoxMorph(USkeletalMesh& Driver, FName Name,
    const FAxisAlignedBox3d& Box, const FVector3f& PositionDelta,
    int32* OutMatchedCount = nullptr)
{
    FSkeletalMeshModel* ImportedModel = Driver.GetImportedModel();
    const FMeshDescription* Description = Driver.GetMeshDescription(0);
    if (OutMatchedCount)
    {
        *OutMatchedCount = 0;
    }
    if (!ImportedModel || !ImportedModel->LODModels.IsValidIndex(0) || !Description)
    {
        return false;
    }

    const TVertexAttributesConstRef<FVector3f> Positions = Description->GetVertexPositions();
    const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[0];
    TArray<FMorphTargetDelta> Deltas;
    for (uint32 VertexIndex = 0; VertexIndex < LODModel.NumVertices; ++VertexIndex)
    {
        if (!LODModel.MeshToImportVertexMap.IsValidIndex(VertexIndex))
        {
            return false;
        }
        const FVertexID PointID(LODModel.MeshToImportVertexMap[VertexIndex]);
        if (Description->Vertices().IsValid(PointID)
            && Box.Contains(FVector3d(Positions[PointID])))
        {
            FMorphTargetDelta& Delta = Deltas.AddDefaulted_GetRef();
            Delta.SourceIdx = VertexIndex;
            Delta.PositionDelta = PositionDelta;
        }
    }
    if (OutMatchedCount)
    {
        *OutMatchedCount = Deltas.Num();
    }
    UMorphTarget* Morph = NewObject<UMorphTarget>(&Driver, Name, RF_Transient);
    Morph->PopulateDeltas(Deltas, 0, LODModel.Sections, false, false, 0.0f);
    return !Deltas.IsEmpty() && Driver.RegisterMorphTarget(Morph, false);
}

struct FMtoUFullCharacterFixtures
{
    USkeletalMesh* FullDriver = nullptr;
    USkeletalMesh* GarmentOnlyDriver = nullptr;
    UStaticMesh* Preview = nullptr;
    FAxisAlignedBox3d GarmentBounds;
    FAxisAlignedBox3d GarmentABounds;
    FAxisAlignedBox3d BodyCoreBounds;
    int32 PreviewTriangleCount = 0;

    bool IsValid() const
    {
        return FullDriver && GarmentOnlyDriver && Preview && PreviewTriangleCount > 0;
    }
};

/**
 * Builds a deterministic full-character Driver: body cube plus separate face
 * and hair cubes and a two-piece garment whose upper piece carries two
 * material sections. The Preview contains only the garment surface with its
 * own slot names and material assignment.
 */
inline bool MakeFullCharacterFixtures(UObject& Outer, FAutomationTestBase& Test,
    FMtoUFullCharacterFixtures& Fixtures, const int32 ReducedPreviewPokes = -1)
{
    if (ReducedPreviewPokes < -1 || ReducedPreviewPokes > 24)
    {
        Test.AddError(TEXT("reduced Preview requires -1 (original) or 0..24 planar pokes"));
        return false;
    }
    USkeletalMesh* Base = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    if (!Base)
    {
        Test.AddError(TEXT("fixture SkeletalCube was not loaded"));
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
        Test.AddError(TEXT("fixture body conversion failed"));
        return false;
    }
    FDynamicMesh3 Cube(BodySource->GetMeshRef());
    UE::Geometry::FMergeCoincidentMeshEdges WeldCube(&Cube);
    if (!WeldCube.Apply())
    {
        Test.AddError(TEXT("fixture SkeletalCube faces were not welded"));
        return false;
    }
    const FDynamicMesh3 CoarseCube(Cube);
    TArray<int32> CubeTriangles;
    for (const int32 TriangleID : Cube.TriangleIndicesItr())
    {
        CubeTriangles.Add(TriangleID);
    }
    for (const int32 TriangleID : CubeTriangles)
    {
        FDynamicMesh3::FPokeTriangleInfo PokeInfo;
        Cube.PokeTriangle(TriangleID, PokeInfo);
    }
    const FVector3d BodyCenter = Cube.GetBounds().Center();
    const double BodyHeight = Cube.GetBounds().Height();

    // Full character: body (group 0), face (1), hair (2), garment upper shell
    // split across groups 3/4, disconnected lower garment piece (5), and a
    // separate arm attachment piece (6) beside the body.
    const FVector3d GarmentUpperOffset = FVector3d::Zero();
    const double GarmentUpperScale = 1.15;
    const FVector3d GarmentLowerOffset =
        BodyCenter - FVector3d(0.0, 0.0, BodyHeight * 1.05);
    const double GarmentLowerScale = 0.55;
    UDynamicMesh* Merged = NewObject<UDynamicMesh>(&Outer);
    Merged->SetMesh(FDynamicMesh3(Cube));
    FAxisAlignedBox3d FaceBounds;
    FAxisAlignedBox3d HairBounds;
    FAxisAlignedBox3d ArmBounds;
    FAxisAlignedBox3d GarmentBBounds;
    bool bAppendOk = true;
    Merged->EditMesh([&](FDynamicMesh3& Mesh)
    {
        FaceBounds = AppendPartCopy(Mesh, Cube,
            BodyCenter + FVector3d(0.0, 0.0, BodyHeight), 0.35, 1);
        HairBounds = AppendPartCopy(Mesh, Cube,
            BodyCenter + FVector3d(0.0, 0.0, BodyHeight * 1.35), 0.4, 2);
        Fixtures.GarmentABounds = AppendPartCopy(Mesh, Cube,
            GarmentUpperOffset, GarmentUpperScale, 3, 2);
        GarmentBBounds = AppendPartCopy(Mesh, Cube,
            GarmentLowerOffset, GarmentLowerScale, 5);
        ArmBounds = AppendPartCopy(Mesh, Cube,
            BodyCenter + FVector3d(BodyHeight * 0.9, 0.0, BodyHeight * 0.35), 0.3, 6);
        SetUniformBoneWeights(Mesh);
        bAppendOk = Mesh.TriangleCount() == Cube.TriangleCount() * 6;
    });
    if (!bAppendOk)
    {
        Test.AddError(FString::Printf(
            TEXT("fixture merge produced %d triangles instead of %d"),
            Merged->GetMeshRef().TriangleCount(), Cube.TriangleCount() * 6));
        return false;
    }
    Fixtures.GarmentBounds = Fixtures.GarmentABounds;
    Fixtures.GarmentBounds.Contain(GarmentBBounds);
    // Body core inset from the body surface so no shell or piece boundary can
    // touch it; only genuine interior body volume counts as body territory.
    constexpr double CoreInset = 1.5;
    Fixtures.BodyCoreBounds = FAxisAlignedBox3d(
        Cube.GetBounds().Min + CoreInset, Cube.GetBounds().Max - CoreInset);

    Fixtures.FullDriver = WriteTransientDriver(Outer, *Base,
        Merged->GetMeshRef(),
        {
            FName(TEXT("Body")),
            FName(TEXT("Face")),
            FName(TEXT("Hair")),
            FName(TEXT("Garment_Upper_A")),
            FName(TEXT("Garment_Upper_B")),
            FName(TEXT("Garment_Lower")),
            FName(TEXT("Arm")),
        });
    if (!Fixtures.FullDriver)
    {
        Test.AddError(TEXT("fixture full-character Driver was not written"));
        return false;
    }
    if (!Fixtures.FullDriver->GetMeshDescription(0)
        || !Fixtures.FullDriver->GetImportedModel()
        || !Fixtures.FullDriver->GetImportedModel()->LODModels.IsValidIndex(0))
    {
        Test.AddError(TEXT("fixture full-character Driver has no LOD0 source data"));
        return false;
    }

    // Region-targeted Morphs: only the garment upper-shell morph may reach the
    // garment-only Preview; the others must be filtered by resolution.
    const FAxisAlignedBox3d FlapBounds(
        Fixtures.GarmentABounds.Min - 2.0, Fixtures.GarmentABounds.Max + 2.0);
    const auto Grown = [](const FAxisAlignedBox3d& Box, double Margin)
    {
        return FAxisAlignedBox3d(Box.Min - Margin, Box.Max + Margin);
    };
    int32 MatchedCount = 0;
    const auto RegisterPartMorph = [&](FName Name, const FAxisAlignedBox3d& Box,
        const FVector3f& Delta) -> bool
    {
        if (AddBoxMorph(*Fixtures.FullDriver, Name, Box, Delta, &MatchedCount))
        {
            return true;
        }
        Test.AddError(FString::Printf(
            TEXT("fixture %s morph matched %d vertices in box [%.1f %.1f %.1f]..[%.1f %.1f %.1f]"),
            *Name.ToString(), MatchedCount,
            Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z));
        return false;
    };
    if (!RegisterPartMorph(FName(TEXT("GarmentFlare")),
            FlapBounds, FVector3f(2.0f, 0.0f, 0.0f))
        || !RegisterPartMorph(FName(TEXT("ArmRaise")),
            Grown(ArmBounds, 1.0), FVector3f(0.0f, 3.0f, 0.0f))
        || !RegisterPartMorph(FName(TEXT("FaceBlink")),
            Grown(FaceBounds, 1.0), FVector3f(0.0f, 0.0f, 1.0f))
        || !RegisterPartMorph(FName(TEXT("HairSway")),
            Grown(HairBounds, 1.0), FVector3f(1.0f, 0.0f, 0.0f)))
    {
        return false;
    }

    // Legacy garment-only Driver and the garment-only Preview are built from
    // the identical garment piece transforms as the full-character Driver.
    FDynamicMesh3 GarmentOnly;
    AppendPartCopy(GarmentOnly, Cube, GarmentUpperOffset, GarmentUpperScale, 0, 2);
    AppendPartCopy(GarmentOnly, Cube, GarmentLowerOffset, GarmentLowerScale, 2);
    if (GarmentOnly.TriangleCount() == 0)
    {
        Test.AddError(TEXT("fixture garment filter selected no triangles"));
        return false;
    }
    Fixtures.GarmentOnlyDriver = WriteTransientDriver(Outer, *Base,
        GarmentOnly,
        {
            FName(TEXT("Garment_Upper_A")),
            FName(TEXT("Garment_Upper_B")),
            FName(TEXT("Garment_Lower")),
        });
    if (!Fixtures.GarmentOnlyDriver)
    {
        Test.AddError(TEXT("fixture legacy garment-only Driver was not written"));
        return false;
    }
    if (!AddBoxMorph(*Fixtures.GarmentOnlyDriver, FName(TEXT("GarmentFlare")),
            FlapBounds, FVector3f(2.0f, 0.0f, 0.0f)))
    {
        Test.AddError(TEXT("fixture legacy GarmentFlare morph was not registered"));
        return false;
    }

    // Garment-only Preview with its own two slots and different materials so
    // slot-name suffixes and assignments cannot drive resolution.
    UDynamicMesh* PreviewSource = NewObject<UDynamicMesh>(&Outer);
    PreviewSource->EditMesh([&](FDynamicMesh3& Mesh)
    {
        const FDynamicMesh3& PreviewCube = ReducedPreviewPokes < 0 ? Cube : CoarseCube;
        AppendPartCopy(Mesh, PreviewCube, GarmentUpperOffset, GarmentUpperScale, 0);
        AppendPartCopy(Mesh, PreviewCube, GarmentLowerOffset, GarmentLowerScale, 1);
        // A poke replaces one planar triangle with three on exactly the same
        // surface. 24 + 2*N triangles vary density without changing coverage,
        // bounds, layers, material slots, or the two garment components.
        TArray<int32> OriginalTriangles;
        for (const int32 TriangleID : Mesh.TriangleIndicesItr())
        {
            OriginalTriangles.Add(TriangleID);
        }
        for (int32 Index = 0; Index < ReducedPreviewPokes; ++Index)
        {
            FDynamicMesh3::FPokeTriangleInfo PokeInfo;
            Mesh.PokeTriangle(OriginalTriangles[Index], PokeInfo);
        }
    });
    Fixtures.PreviewTriangleCount = PreviewSource->GetMeshRef().TriangleCount();

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
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
        PreviewSource, Fixtures.Preview, PreviewWriteOptions, PreviewWriteLOD, PreviewOutcome, false);
    if (PreviewOutcome != EGeometryScriptOutcomePins::Success)
    {
        Test.AddError(TEXT("fixture Preview Static Mesh was not written"));
        return false;
    }
    return true;
}

/** Failure-boundary scenario exercised by Issue #22 resolution gates. */
enum class EMtoUGarmentFaultKind : uint8
{
    /** Two identical garment copies stacked exactly on top of each other. */
    DuplicateGarment,
    /**
     * Two near-twin garment copies separated by a 0.2%-diagonal shift, small
     * enough to stay inside the twin proximity.
     */
    ShiftedDuplicateGarment,
};

struct FMtoUGarmentFaultFixtures
{
    USkeletalMesh* Driver = nullptr;
    UStaticMesh* Preview = nullptr;
    /** Observed edge-connected region count of the built Driver source geometry. */
    int32 ConnectedRegionCount = 0;

    bool IsValid() const { return Driver && Preview && ConnectedRegionCount > 0; }
};

/**
 * Builds failure-boundary characters over one material section so section
 * evidence never drives the outcome. DuplicateGarment layers a second exact
 * copy on the first: nearest-ownership hands every vertex to one family, but
 * the unowned copy must still cause ambiguity. ShiftedDuplicateGarment rotates
 * the second copy slightly while both remain mutual twins.
 * The Preview always contains only the pure garment surface(s).
 */
inline bool MakeGarmentFaultFixtures(UObject& Outer, FAutomationTestBase& Test,
    const EMtoUGarmentFaultKind Kind, FMtoUGarmentFaultFixtures& Fixtures)
{
    USkeletalMesh* Base = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    if (!Base)
    {
        Test.AddError(TEXT("fault fixture SkeletalCube was not loaded"));
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
        Test.AddError(TEXT("fault fixture body conversion failed"));
        return false;
    }
    const FDynamicMesh3& Cube = BodySource->GetMeshRef();

    const FVector3d LowerOffset(FVector3d::Zero()
        - FVector3d(0.0, 0.0, Cube.GetBounds().Height() * 1.05));
    const double LowerScale = 0.55;

    const auto CountComponents = [](const FDynamicMesh3& Mesh)
    {
        UE::Geometry::FMeshConnectedComponents Parts(&Mesh);
        Parts.FindConnectedTriangles();
        return Parts.Components.Num();
    };

    // Body plus TWO full-garment shell sets; every call keeps its own fresh
    // vertex map so the shells stay disjoint vertex sets and no appended
    // triangle ever duplicates another.
    FDynamicMesh3 Geometry;
    AppendPartCopy(Geometry, Cube, FVector3d::Zero(), 0.9, 0, 1);
    const int32 BaseRegions = CountComponents(Geometry);
    const int32 ExpectedRegions = 5 * BaseRegions;
    AppendPartCopy(Geometry, Cube, FVector3d::Zero(), 1.15, 1, 1);
    AppendPartCopy(Geometry, Cube, LowerOffset, LowerScale, 2, 1);

    FDynamicMesh3 SecondSet;
    AppendPartCopy(SecondSet, Cube, FVector3d::Zero(), 1.15, 3, 1);
    AppendPartCopy(SecondSet, Cube, LowerOffset, LowerScale, 4, 1);
    if (Kind == EMtoUGarmentFaultKind::ShiftedDuplicateGarment)
    {
        // A slight Z-rotation makes the twin shells' surfaces CROSS, so
        // nearest-ownership splits between the two families by region instead
        // of letting one exact overlay win everything.
        const double AngleRadians = FMath::DegreesToRadians(3.0);
        const double CosAngle = FMath::Cos(AngleRadians);
        const double SinAngle = FMath::Sin(AngleRadians);
        const FVector3d Pivot = SecondSet.GetBounds().Center();
        for (const int32 VertexID : SecondSet.VertexIndicesItr())
        {
            const FVector3d Relative = SecondSet.GetVertex(VertexID) - Pivot;
            SecondSet.SetVertex(VertexID, Pivot + FVector3d(
                CosAngle * Relative.X - SinAngle * Relative.Y,
                SinAngle * Relative.X + CosAngle * Relative.Y,
                Relative.Z));
        }
    }
    UE::Geometry::FDynamicMeshEditor Editor(&Geometry);
    UE::Geometry::FMeshIndexMappings MergeMappings;
    Editor.AppendMesh(&SecondSet, MergeMappings);
    SetUniformBoneWeights(Geometry);

    UE::Geometry::FMeshConnectedComponents Components(&Geometry);
    Components.FindConnectedTriangles();
    Fixtures.ConnectedRegionCount = Components.Components.Num();
    Test.AddInfo(FString::Printf(
        TEXT("fault fixture topology: %d connected regions, %d tris, expected=%d"),
        Fixtures.ConnectedRegionCount, Geometry.TriangleCount(), ExpectedRegions));
    if (Fixtures.ConnectedRegionCount != ExpectedRegions)
    {
        Test.AddError(FString::Printf(
            TEXT("fault fixture produced %d connected regions against expected model %d"),
            Fixtures.ConnectedRegionCount, ExpectedRegions));
        return false;
    }

    Fixtures.Driver = WriteTransientDriver(Outer, *Base, Geometry,
        {
            FName(TEXT("Fault_Body")),
            FName(TEXT("Fault_A_Top")),
            FName(TEXT("Fault_A_Bottom")),
            FName(TEXT("Fault_B_Top")),
            FName(TEXT("Fault_B_Bottom"))});
    if (!Fixtures.Driver || !Fixtures.Driver->GetMeshDescription(0))
    {
        Test.AddError(TEXT("fault fixture Driver was not written"));
        return false;
    }

    // The Preview holds only the pure garment surface: upper plus lower pieces
    // mirroring the inner shell set.
    FDynamicMesh3 PreviewGeometry;
    AppendPartCopy(PreviewGeometry, Cube, FVector3d::Zero(), 1.15, 0, 1);
    AppendPartCopy(PreviewGeometry, Cube, LowerOffset, LowerScale, 1, 1);

    Fixtures.Preview = NewObject<UStaticMesh>(&Outer, NAME_None, RF_Transient);
    FGeometryScriptCopyMeshToAssetOptions PreviewWriteOptions;
    PreviewWriteOptions.bEmitTransaction = false;
    PreviewWriteOptions.bEnableRecomputeNormals = true;
    PreviewWriteOptions.bEnableRecomputeTangents = true;
    PreviewWriteOptions.bReplaceMaterials = true;
    PreviewWriteOptions.NewMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
    PreviewWriteOptions.NewMaterialSlotNames.Add(FName(TEXT("Fault_Preview_Top")));
    PreviewWriteOptions.NewMaterials.Add(nullptr);
    PreviewWriteOptions.NewMaterialSlotNames.Add(FName(TEXT("Fault_Preview_Bottom")));
    FGeometryScriptMeshWriteLOD PreviewWriteLOD;
    EGeometryScriptOutcomePins PreviewOutcome = EGeometryScriptOutcomePins::Failure;
    UDynamicMesh* PreviewSource = NewObject<UDynamicMesh>(&Outer);
    PreviewSource->SetMesh(MoveTemp(PreviewGeometry));
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
        PreviewSource, Fixtures.Preview, PreviewWriteOptions, PreviewWriteLOD, PreviewOutcome, false);
    if (PreviewOutcome != EGeometryScriptOutcomePins::Success)
    {
        Test.AddError(TEXT("fault fixture Preview was not written"));
        return false;
    }
    return true;
}

// Mirrors the calibrated source-mass boundary documented in
// MtoUDriverGarmentSurface.cpp (MaxDriverToPreviewTriangleRatio); tests assert
// the resolved surface stays inside the same envelope rather than assuming
// exact Preview parity, since radius coverage legitimately admits nearby trim
// pieces. Keep the two values in sync when recalibrating.
constexpr double TestMaxSourceToPreviewRatio = 1.7;
}

#endif
