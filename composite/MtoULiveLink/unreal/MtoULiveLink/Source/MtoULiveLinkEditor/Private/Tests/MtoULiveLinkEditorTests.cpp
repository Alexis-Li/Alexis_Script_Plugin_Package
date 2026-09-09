#if WITH_DEV_AUTOMATION_TESTS

#include "MtoULiveLinkFactories.h"
#include "MtoULiveLinkPreview.h"
#include "MtoULiveLinkPreviewDetail.h"
#include "MtoULiveLinkEditorTestFixtures.h"

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"

#include "Animation/MorphTarget.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicVertexSkinWeightsAttribute.h"
#include "DynamicMeshEditor.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "Misc/PackageName.h"
#include "PropertyEditorModule.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/ImportSubsystem.h"
#include "Subsystems/PlacementSubsystem.h"
#include "Tests/EnsureScope.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

// Editor-private bridge for the Refresh-during-session integration test
// (Issue #39): real Live Link source negotiation plus public RefreshActor.
// Only this test includes the Runtime Private source; production code
// never does.
#include "MtoULiveLinkSource.h"

#include "Engine/Engine.h"
#include "Features/IModularFeatures.h"
#include "HAL/PlatformProcess.h"
#include "ILiveLinkClient.h"
#include "IPAddress.h"
#include "LiveLinkInstance.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "ReferenceSkeleton.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Input/Reply.h"
#include "Misc/ScopeExit.h"
#include "MtoULiveLinkActorDetails.h"

#include <limits>

// Deterministic observation for the Refresh-ends-session critical window
// (Issue #39). Reads Game Thread session state without disturbing it, so the
// test can prove the synchronous publish gate refuses old items while the
// worker still serves the replaced session. Only this test uses it.
class FMtoURefreshEndsSessionTestAccess
{
public:
    static uint64 Session(const FMtoULiveLinkSource& Source)
    {
        return Source.GameThreadSession;
    }
    static bool IsWorkerCurrent(const FMtoULiveLinkSource& Source, uint64 SessionId)
    {
        return Source.IsCurrentSession(SessionId);
    }
    static bool IsPublishable(FMtoULiveLinkSource& Source, uint64 SessionId)
    {
        return Source.IsSessionPublishableOnGameThread(SessionId);
    }
    static bool HasPendingLiveFrame(const FMtoULiveLinkSource& Source)
    {
        FScopeLock Lock(&Source.PendingMutex);
        return Source.PendingFrame.IsSet();
    }
    static int32 PendingCacheCommandCount(const FMtoULiveLinkSource& Source)
    {
        return Source.GetPendingCacheCommandCount();
    }
    static EMtoUCacheState CacheState(const FMtoULiveLinkSource& Source)
    {
        return Source.GetCacheSessionState();
    }
};

namespace
{
UStaticMesh* MakePreview(
    USkeletalMesh& Driver,
    UObject& Outer,
    bool bLocalRetopology = false,
    bool bDoubleLayer = false,
    bool bRemovePositiveXSurface = false,
    bool bMisaligned = false)
{
    UDynamicMesh* DynamicMesh = NewObject<UDynamicMesh>(&Outer);
    FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
    FGeometryScriptMeshReadLOD ReadLOD;
    ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
    EGeometryScriptOutcomePins ReadOutcome = EGeometryScriptOutcomePins::Failure;
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
        &Driver, DynamicMesh, ReadOptions, ReadLOD, ReadOutcome);
    if (ReadOutcome != EGeometryScriptOutcomePins::Success)
    {
        return nullptr;
    }
    if (bMisaligned)
    {
        DynamicMesh->EditMesh([](UE::Geometry::FDynamicMesh3& Mesh)
        {
            const double Offset = Mesh.GetBounds().DiagonalLength() * 2.0;
            for (const int32 VertexID : Mesh.VertexIndicesItr())
            {
                Mesh.SetVertex(VertexID,
                    Mesh.GetVertex(VertexID) + FVector3d(Offset, 0.0, 0.0));
            }
        });
    }
    if (bRemovePositiveXSurface)
    {
        DynamicMesh->EditMesh([](UE::Geometry::FDynamicMesh3& Mesh)
        {
            const double CenterX = Mesh.GetBounds().Center().X;
            TArray<int32> TrianglesToRemove;
            for (const int32 TriangleID : Mesh.TriangleIndicesItr())
            {
                const UE::Geometry::FIndex3i Triangle = Mesh.GetTriangle(TriangleID);
                if (Mesh.GetVertex(Triangle.A).X > CenterX
                    || Mesh.GetVertex(Triangle.B).X > CenterX
                    || Mesh.GetVertex(Triangle.C).X > CenterX)
                {
                    TrianglesToRemove.Add(TriangleID);
                }
            }
            for (const int32 TriangleID : TrianglesToRemove)
            {
                Mesh.RemoveTriangle(TriangleID);
            }
        });
    }
    if (bLocalRetopology)
    {
        DynamicMesh->EditMesh([bDoubleLayer](UE::Geometry::FDynamicMesh3& Mesh)
        {
            for (const int32 TriangleID : Mesh.TriangleIndicesItr())
            {
                UE::Geometry::FDynamicMesh3::FPokeTriangleInfo PokeInfo;
                Mesh.PokeTriangle(TriangleID, PokeInfo);
                break;
            }
            if (bDoubleLayer)
            {
                const UE::Geometry::FDynamicMesh3 OuterLayer(Mesh);
                UE::Geometry::FDynamicMeshEditor Editor(&Mesh);
                UE::Geometry::FMeshIndexMappings Mappings;
                Editor.AppendMesh(
                    &OuterLayer,
                    Mappings,
                    [](int32, const FVector3d& Position) { return Position * 0.9; },
                    nullptr,
                    true);
            }
        });
    }

    UStaticMesh* Preview = NewObject<UStaticMesh>(&Outer, NAME_None, RF_Transient);
    FGeometryScriptCopyMeshToAssetOptions WriteOptions;
    WriteOptions.bEmitTransaction = false;
    WriteOptions.bReplaceMaterials = true;
    for (const FSkeletalMaterial& Material : Driver.GetMaterials())
    {
        WriteOptions.NewMaterials.Add(Material.MaterialInterface);
        WriteOptions.NewMaterialSlotNames.Add(Material.MaterialSlotName);
    }
    FGeometryScriptMeshWriteLOD WriteLOD;
    EGeometryScriptOutcomePins WriteOutcome = EGeometryScriptOutcomePins::Failure;
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
        DynamicMesh, Preview, WriteOptions, WriteLOD, WriteOutcome, false);
    return WriteOutcome == EGeometryScriptOutcomePins::Success ? Preview : nullptr;
}

enum class EMtoUCorpusVariant : uint8
{
    SameTopology,
    LocalRetopology,
    DoubleLayerSeams,
    Misaligned,
    /** Whole-surface shift by 0.1% of the diagonal: aligned but nonzero distances. */
    SmallMisalignment
};

UStaticMesh* MakeCorpusPreview(
    USkeletalMesh& Driver,
    UStaticMesh* BasePreview,
    UObject& Outer,
    EMtoUCorpusVariant Variant)
{
    const bool bFullCharacterFixture = BasePreview != nullptr;
    UDynamicMesh* DynamicMesh = NewObject<UDynamicMesh>(&Outer);
    FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
    ReadOptions.bApplyBuildSettings = false;
    ReadOptions.bRequestTangents = true;
    FGeometryScriptMeshReadLOD ReadLOD;
    ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
    EGeometryScriptOutcomePins ReadOutcome = EGeometryScriptOutcomePins::Failure;
    if (BasePreview)
    {
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(
            BasePreview, DynamicMesh, ReadOptions, ReadLOD, ReadOutcome, false);
    }
    else
    {
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
            &Driver, DynamicMesh, ReadOptions, ReadLOD, ReadOutcome);
    }
    if (ReadOutcome != EGeometryScriptOutcomePins::Success)
    {
        return nullptr;
    }

    DynamicMesh->EditMesh([Variant, bFullCharacterFixture](UE::Geometry::FDynamicMesh3& Mesh)
    {
        switch (Variant)
        {
        case EMtoUCorpusVariant::Misaligned:
        {
            const double Offset = Mesh.GetBounds().DiagonalLength() * 2.0;
            for (const int32 VertexID : Mesh.VertexIndicesItr())
            {
                Mesh.SetVertex(VertexID,
                    Mesh.GetVertex(VertexID) + FVector3d(Offset, 0.0, 0.0));
            }
            break;
        }
        case EMtoUCorpusVariant::SmallMisalignment:
        {
            const double Offset = Mesh.GetBounds().DiagonalLength() * 0.001;
            for (const int32 VertexID : Mesh.VertexIndicesItr())
            {
                Mesh.SetVertex(VertexID,
                    Mesh.GetVertex(VertexID) + FVector3d(0.0, 0.0, Offset));
            }
            break;
        }
        case EMtoUCorpusVariant::LocalRetopology:
        case EMtoUCorpusVariant::DoubleLayerSeams:
        {
            if (Variant == EMtoUCorpusVariant::LocalRetopology
                && bFullCharacterFixture)
            {
                for (const int32 EdgeID : Mesh.EdgeIndicesItr())
                {
                    if (!Mesh.IsBoundaryEdge(EdgeID))
                    {
                        UE::Geometry::FDynamicMesh3::FEdgeFlipInfo FlipInfo;
                        if (Mesh.FlipEdge(EdgeID, FlipInfo)
                            == UE::Geometry::EMeshResult::Ok)
                        {
                            break;
                        }
                    }
                }
                break;
            }
            UE::Geometry::FDynamicMesh3::FPokeTriangleInfo PokeInfo;
            for (const int32 TriangleID : Mesh.TriangleIndicesItr())
            {
                Mesh.PokeTriangle(TriangleID, PokeInfo);
                break;
            }
            if (Variant == EMtoUCorpusVariant::DoubleLayerSeams)
            {
                const UE::Geometry::FDynamicMesh3 OuterLayer(Mesh);
                UE::Geometry::FDynamicMeshEditor Editor(&Mesh);
                UE::Geometry::FMeshIndexMappings Mappings;
                Editor.AppendMesh(
                    &OuterLayer,
                    Mappings,
                    [](int32, const FVector3d& Position) { return Position * 0.9; },
                    nullptr,
                    true);
            }
            break;
        }
        default:
            break;
        }
    });

    UStaticMesh* Preview = NewObject<UStaticMesh>(&Outer, NAME_None, RF_Transient);
    FGeometryScriptCopyMeshToAssetOptions WriteOptions;
    WriteOptions.bEmitTransaction = false;
    WriteOptions.bReplaceMaterials = true;
    if (BasePreview)
    {
        for (const FStaticMaterial& Material : BasePreview->GetStaticMaterials())
        {
            WriteOptions.NewMaterials.Add(Material.MaterialInterface);
            WriteOptions.NewMaterialSlotNames.Add(Material.MaterialSlotName);
        }
    }
    else
    {
        for (const FSkeletalMaterial& Material : Driver.GetMaterials())
        {
            WriteOptions.NewMaterials.Add(Material.MaterialInterface);
            WriteOptions.NewMaterialSlotNames.Add(Material.MaterialSlotName);
        }
    }
    FGeometryScriptMeshWriteLOD WriteLOD;
    EGeometryScriptOutcomePins WriteOutcome = EGeometryScriptOutcomePins::Failure;
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
        DynamicMesh, Preview, WriteOptions, WriteLOD, WriteOutcome, false);
    return WriteOutcome == EGeometryScriptOutcomePins::Success ? Preview : nullptr;
}

bool AddUniformMorph(USkeletalMesh& Driver, FName Name, const FVector3f& PositionDelta)
{
    FSkeletalMeshModel* ImportedModel = Driver.GetImportedModel();
    if (!ImportedModel || !ImportedModel->LODModels.IsValidIndex(0))
    {
        return false;
    }

    const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[0];
    TArray<FMorphTargetDelta> Deltas;
    Deltas.Reserve(LODModel.NumVertices);
    for (uint32 VertexIndex = 0; VertexIndex < LODModel.NumVertices; ++VertexIndex)
    {
        FMorphTargetDelta& Delta = Deltas.AddDefaulted_GetRef();
        Delta.SourceIdx = VertexIndex;
        Delta.PositionDelta = PositionDelta;
    }
    UMorphTarget* Morph = NewObject<UMorphTarget>(
        &Driver, Name, RF_Transient);
    Morph->PopulateDeltas(Deltas, 0, LODModel.Sections, false, false, 0.0f);
    return Driver.RegisterMorphTarget(Morph, false);
}

bool AddPositiveXMorph(USkeletalMesh& Driver, FName Name, const FVector3f& PositionDelta)
{
    FSkeletalMeshModel* ImportedModel = Driver.GetImportedModel();
    const FMeshDescription* Description = Driver.GetMeshDescription(0);
    if (!ImportedModel || !ImportedModel->LODModels.IsValidIndex(0) || !Description)
    {
        return false;
    }

    const TVertexAttributesConstRef<FVector3f> Positions = Description->GetVertexPositions();
    float MinX = TNumericLimits<float>::Max();
    float MaxX = TNumericLimits<float>::Lowest();
    for (const FVertexID VertexID : Description->Vertices().GetElementIDs())
    {
        MinX = FMath::Min(MinX, Positions[VertexID].X);
        MaxX = FMath::Max(MaxX, Positions[VertexID].X);
    }
    const float CenterX = (MinX + MaxX) * 0.5f;

    const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[0];
    TArray<FMorphTargetDelta> Deltas;
    for (uint32 VertexIndex = 0; VertexIndex < LODModel.NumVertices; ++VertexIndex)
    {
        if (!LODModel.MeshToImportVertexMap.IsValidIndex(VertexIndex))
        {
            return false;
        }
        const FVertexID PointID(LODModel.MeshToImportVertexMap[VertexIndex]);
        if (Description->Vertices().IsValid(PointID) && Positions[PointID].X > CenterX)
        {
            FMorphTargetDelta& Delta = Deltas.AddDefaulted_GetRef();
            Delta.SourceIdx = VertexIndex;
            Delta.PositionDelta = PositionDelta;
        }
    }
    UMorphTarget* Morph = NewObject<UMorphTarget>(&Driver, Name, RF_Transient);
    Morph->PopulateDeltas(Deltas, 0, LODModel.Sections, false, false, 0.0f);
    return !Deltas.IsEmpty() && Driver.RegisterMorphTarget(Morph, false);
}

USkeletalMesh* MakeMorphDriver(UObject& Outer, bool bSplitMissingSurface = false)
{
    USkeletalMesh* Base = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    USkeletalMesh* Driver = nullptr;
    if (Base && bSplitMissingSurface)
    {
        UDynamicMesh* Source = NewObject<UDynamicMesh>(&Outer);
        FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
        FGeometryScriptMeshReadLOD ReadLOD;
        ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
            Base, Source, ReadOptions, ReadLOD, Outcome);
        if (Outcome == EGeometryScriptOutcomePins::Success)
        {
            Source->EditMesh([](UE::Geometry::FDynamicMesh3& Mesh)
            {
                const double CenterX = Mesh.GetBounds().Center().X;
                for (const int32 TriangleID : Mesh.TriangleIndicesItr())
                {
                    const UE::Geometry::FIndex3i Triangle = Mesh.GetTriangle(TriangleID);
                    const bool bPositiveX = Mesh.GetVertex(Triangle.A).X > CenterX
                        || Mesh.GetVertex(Triangle.B).X > CenterX
                        || Mesh.GetVertex(Triangle.C).X > CenterX;
                    Mesh.SetTriangleGroup(TriangleID, bPositiveX ? 1 : 0);
                }
            });
            Driver = MtoUEditorTest::WriteTransientDriver(
                Outer, *Base, Source->GetMeshRef(),
                {FName(TEXT("MaterialSlot")), FName(TEXT("NoMatchingSurface"))});
        }
    }
    else if (Base)
    {
        Driver = DuplicateObject<USkeletalMesh>(Base, &Outer);
    }
    if (!Driver
        || !AddUniformMorph(*Driver, FName(TEXT("Corrective")), FVector3f(2.0f, 0.0f, 0.0f))
        || !AddUniformMorph(*Driver, FName(TEXT("CorrectiveNegative")), FVector3f(-2.0f, 0.0f, 0.0f))
        || !AddUniformMorph(*Driver, FName(TEXT("Left")), FVector3f(0.0f, 1.5f, 0.0f))
        || !AddUniformMorph(*Driver, FName(TEXT("Right")), FVector3f(0.0f, -1.5f, 0.0f))
        || !AddUniformMorph(*Driver, FName(TEXT("Stress")), FVector3f(0.0f, 0.0f, 0.75f)))
    {
        return nullptr;
    }
    Driver->InitMorphTargets();
    return Driver;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUDetailsSectionTest,
    "MtoULiveLink.Editor.Details",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUDetailsSectionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FPropertyEditorModule& PropertyEditor =
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
    for (const FName Category : { FName("MtoU_LiveLink"), FName("MtoU Preview") })
    {
        const TArray<TSharedPtr<FPropertySection>> Sections =
            PropertyEditor.FindSectionsForCategory(AMtoULiveLinkActor::StaticClass(), Category);
        TestTrue(FString::Printf(TEXT("%s belongs to the MtoU details section"), *Category.ToString()),
            Sections.ContainsByPredicate([](const TSharedPtr<FPropertySection>& Section)
            {
                return Section.IsValid() && Section->GetName() == "MtoU"
                    && Section->GetOrder() == 1000;
            }));
    }

    for (const FName PropertyName : {
            FName("PreviewState"), FName("PreviewBuildStage"), FName("DisplayTarget"),
            FName("PreviewDiagnostics"), FName("ModelDiagnostics"), FName("ModelDiagnosticLevel"),
            FName("DriverMeshComponent") })
    {
        const FProperty* Property = FindFProperty<FProperty>(
            AMtoULiveLinkActor::StaticClass(), PropertyName);
        TestTrue(FString::Printf(TEXT("%s stays out of the default property rows"), *PropertyName.ToString()),
            Property && !Property->HasAnyPropertyFlags(CPF_Edit));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewPreparationPreflightTest,
    "MtoULiveLink.Editor.Preview.Preflight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewPreparationPreflightTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>();
    TestNotNull(TEXT("preview owner is created"), Actor);

    TArray<EMtoUPreviewBuildStage> ObservedStages;
    const FMtoUPreviewPreparationResult Result = Actor && Binding
        ? MtoUPreparePreview(*Actor, *Binding,
            [&ObservedStages](EMtoUPreviewBuildStage Stage) { ObservedStages.Add(Stage); })
        : FMtoUPreviewPreparationResult();
    TestFalse(TEXT("missing binding inputs fail preparation"), Result.bSucceeded);
    TestNull(TEXT("a failed preparation returns no usable mesh"), Result.GeneratedPreview);
    TestEqual(TEXT("preflight is observable"), ObservedStages.Num(), 1);
    TestTrue(TEXT("the failure is attributed to preflight"),
        Result.FailureStage == EMtoUPreviewBuildStage::Preflight);

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewPreparationSuccessTest,
    "MtoULiveLink.Editor.Preview.Success",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewPreparationSuccessTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FProperty* LegacyProperty = FindFProperty<FProperty>(
        UMtoULiveLinkBinding::StaticClass(), GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, SkeletalMesh));
    const FProperty* PreviewProperty = FindFProperty<FProperty>(
        UMtoULiveLinkBinding::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, PreviewStaticMesh));
    TestNotNull(TEXT("legacy serialized SkeletalMesh field remains present"), LegacyProperty);
    TestEqual(TEXT("legacy field is displayed as Driver Skeletal Mesh"),
        LegacyProperty ? LegacyProperty->GetDisplayNameText().ToString() : FString(),
        FString(TEXT("Driver Skeletal Mesh")));
    TestNotNull(TEXT("optional Preview Static Mesh field is present"), PreviewProperty);

    USkeletalMesh* Driver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    TestTrue(TEXT("stock Driver has LOD0 source data"),
        Driver && Driver->GetNumSourceModels() > 0 && Driver->HasMeshDescription(0));

    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUPreviewSuccessWorld"));
    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        WorldPackage->GetName(), FPackageName::GetAssetPackageExtension());
    TestFalse(TEXT("success fixture has no package file before Refresh"),
        IFileManager::Get().FileExists(*PackageFilename));
    UStaticMesh* Preview = Driver ? MakePreview(*Driver, *WorldPackage) : nullptr;
    TestTrue(TEXT("fixed same-topology Preview has LOD0 source data"),
        Preview && Preview->IsSourceModelValid(0) && Preview->IsMeshDescriptionValid(0));
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUPreviewSuccessWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = Preview;
    UMtoULiveLinkBinding* LegacyRoundTrip = DuplicateObject<UMtoULiveLinkBinding>(
        Binding, WorldPackage, TEXT("LegacyBindingRoundTrip"));
    TestTrue(TEXT("legacy SkeletalMesh serialization survives object load/duplication"),
        LegacyRoundTrip && LegacyRoundTrip->SkeletalMesh == Driver);
    TArray<EMtoUPreviewBuildStage> ObservedStages;
    const FMtoUPreviewPreparationResult Result = Actor
        ? MtoUPreparePreview(*Actor, *Binding,
            [&ObservedStages](EMtoUPreviewBuildStage Stage) { ObservedStages.Add(Stage); })
        : FMtoUPreviewPreparationResult();

    AddInfo(Result.Diagnostics);
    TestTrue(TEXT("stock public APIs build the transient preview"), Result.bSucceeded);
    TestNotNull(TEXT("success returns the complete Generated Preview"), Result.GeneratedPreview);
    TestEqual(TEXT("all five preparation stages are observable"), ObservedStages.Num(), 5);
    TestTrue(TEXT("Generated Preview is actor-owned"),
        Result.GeneratedPreview && Result.GeneratedPreview->GetOuter() == Actor);
    TestTrue(TEXT("Generated Preview is transient"),
        Result.GeneratedPreview && Result.GeneratedPreview->HasAnyFlags(RF_Transient));
    TestFalse(TEXT("Generated Preview is not a persistent asset"),
        Result.GeneratedPreview && Result.GeneratedPreview->IsAsset());
    TestFalse(TEXT("Generated Preview has no public or standalone asset flags"),
        Result.GeneratedPreview
        && Result.GeneratedPreview->HasAnyFlags(RF_Public | RF_Standalone));
    TArray<FAssetData> PackageAssets;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry")
        .Get().GetAssetsByPackageName(WorldPackage->GetFName(), PackageAssets);
    TestFalse(TEXT("Asset Registry does not expose Generated Preview as a package asset"),
        PackageAssets.ContainsByPredicate([&Result](const FAssetData& Asset)
        {
            return Result.GeneratedPreview && Asset.AssetName == Result.GeneratedPreview->GetFName();
        }));
    TestFalse(TEXT("normal Refresh creates no .uasset on the filesystem"),
        IFileManager::Get().FileExists(*PackageFilename));

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewLifecycleTest,
    "MtoULiveLink.Editor.Preview.Lifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewLifecycleTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    USkeletalMesh* Driver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUPreviewLifecycleWorld"));
    UStaticMesh* Preview = Driver ? MakePreview(*Driver, *WorldPackage) : nullptr;
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUPreviewLifecycleWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = Preview;
    if (!Actor || !Driver || !Preview)
    {
        AddError(TEXT("lifecycle fixtures were not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }

    Actor->SetBinding(Binding);
    TestTrue(TEXT("placement marks configured inputs Dirty"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty);
    TestTrue(TEXT("binding selects the Driver display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver);
    TestTrue(TEXT("MtoU display component bypasses post-process animation"),
        Actor->GetSkeletalMeshComponent()->GetDisablePostProcessBlueprint());
    TestFalse(TEXT("placement never invokes Refresh automatically"),
        Actor->GetPreviewReadiness().IsUsable());
    TestTrue(TEXT("legacy Animation target remains visible before first Refresh"),
        Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    Actor->OnConstruction(Actor->GetActorTransform());
    TestFalse(TEXT("PostEdit construction never invokes Refresh automatically"),
        Actor->GetPreviewReadiness().IsUsable());

    const FMtoUPreviewReadiness First =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    USkeletalMesh* FirstMesh = First.GeneratedPreview;
    TestTrue(TEXT("explicit Refresh commits one complete preview"),
        First.IsUsable() && Actor->GetPreviewReadiness().IsUsable());
    TestTrue(TEXT("inpainted vertices become Warning rather than failure"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Ready
        || Actor->GetPreviewReadiness().State == EMtoUPreviewState::Warning);
    TestTrue(TEXT("successful Refresh displays the Generated Preview"),
        Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == FirstMesh);
    TestTrue(TEXT("successful Refresh records the Generated Preview display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview);

    Actor->GetSkeletalMeshComponent()->SetDisablePostProcessBlueprint(false);
    Actor->OnConstruction(Actor->GetActorTransform());
    TestTrue(TEXT("construction preserves the Generated Preview target and isolation"),
        Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == FirstMesh
        && Actor->GetSkeletalMeshComponent()->GetDisablePostProcessBlueprint());
    Actor->GetSkeletalMeshComponent()->SetDisablePostProcessBlueprint(false);
    Actor->PostRegisterAllComponents();
    TestTrue(TEXT("registration preserves the Generated Preview target and isolation"),
        Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == FirstMesh
        && Actor->GetSkeletalMeshComponent()->GetDisablePostProcessBlueprint());

    TWeakObjectPtr<USkeletalMesh> ReplacedMesh = FirstMesh;
    const FMtoUPreviewReadiness Second =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("repeated Refresh replaces the previous transient mesh"),
        Second.IsUsable() && Second.GeneratedPreview != FirstMesh);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("replacement releases the previous transient mesh"), ReplacedMesh.IsValid());
    TestTrue(TEXT("GC retains the actor-owned current preview"),
        Actor->GetPreviewReadiness().IsUsable());

    AMtoULiveLinkActor* ReloadedActor = DuplicateObject<AMtoULiveLinkActor>(
        Actor, World->GetCurrentLevel());
    TestTrue(TEXT("level save/reload drops transient preview and requires Refresh"),
        ReloadedActor
        && !ReloadedActor->GetPreviewReadiness().IsUsable()
        && ReloadedActor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && ReloadedActor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    if (ReloadedActor)
    {
        ReloadedActor->Destroy();
    }

    Actor->SetConnectionStatus(TEXT("Disconnected"));
    TestTrue(TEXT("disconnect keeps the Generated Preview"),
        Actor->GetPreviewReadiness().IsUsable());
    TestTrue(TEXT("disconnect preserves the Generated Preview display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Second.GeneratedPreview);
    if (GEditor)
    {
        GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetReimport(Preview);
    }
    TestTrue(TEXT("Reimport marks Dirty and releases stale preview"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && !Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);

    TestTrue(TEXT("Refresh can recover after Reimport invalidation"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());
    Preview->OnPostMeshBuild().Broadcast(Preview);
    TestTrue(TEXT("source rebuild notification marks Dirty and hides stale preview"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && !Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);
    TestTrue(TEXT("Refresh can recover after source rebuild invalidation"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());

    Binding->PreviewStaticMesh = nullptr;
    FProperty* PreviewInputProperty = FindFProperty<FProperty>(
        UMtoULiveLinkBinding::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, PreviewStaticMesh));
    FPropertyChangedEvent PreviewInputChanged(PreviewInputProperty);
    Actor->ShowDriverMesh();
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestEqual(TEXT("removing a required input produces the unconfigured None state"),
        Actor->GetPreviewReadiness().State, EMtoUPreviewState::None);
    TestTrue(TEXT("Binding input edits preserve the Driver display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    TestFalse(TEXT("Binding input changes release and hide stale preview"),
        Actor->GetPreviewReadiness().IsUsable());
    const FMtoUPreviewReadiness Failed =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("failed Refresh is transactional"),
        !Failed.IsUsable()
        && Failed.GeneratedPreview == nullptr
        && Failed.State == EMtoUPreviewState::Error
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);

    const FProperty* GeneratedProperty = FindFProperty<FProperty>(
        AMtoULiveLinkActor::StaticClass(), TEXT("GeneratedPreviewMesh"));
    TestTrue(TEXT("level save/reload cannot serialize the Generated Preview"),
        GeneratedProperty
        && GeneratedProperty->HasAllPropertyFlags(CPF_Transient | CPF_DuplicateTransient));

    Binding->PreviewStaticMesh = Preview;
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestTrue(TEXT("editor shutdown cleanup seam releases the preview"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());
    TWeakObjectPtr<USkeletalMesh> ShutdownMesh = Actor->GetPreviewReadiness().GeneratedPreview;
    Actor->NotifyTransientPreviewReleased();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("editor shutdown releases transient preview data"),
        ShutdownMesh.IsValid());
    TestTrue(TEXT("manual delete fixture has a ready preview"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());
    TWeakObjectPtr<USkeletalMesh> DeletedMesh = Actor->GetPreviewReadiness().GeneratedPreview;
    Actor->NotifyGeneratedPreviewDeleted();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("manual delete releases the preview and restores the Driver"),
        !DeletedMesh.IsValid()
        && !Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    TestTrue(TEXT("final Refresh succeeds before actor destruction"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());
    TWeakObjectPtr<USkeletalMesh> DestroyedMesh = Actor->GetPreviewReadiness().GeneratedPreview;
    Actor->Destroy();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("actor destruction releases transient preview data"),
        DestroyedMesh.IsValid());

    AMtoULiveLinkActor* WorldUnloadActor = World->SpawnActor<AMtoULiveLinkActor>();
    WorldUnloadActor->SetBinding(Binding);
    TestTrue(TEXT("world-unload fixture has a ready preview"),
        FMtoUPreviewPreparation::RefreshActor(*WorldUnloadActor).IsUsable());
    TWeakObjectPtr<USkeletalMesh> WorldUnloadMesh = WorldUnloadActor->GetPreviewReadiness().GeneratedPreview;
    World->DestroyWorld(false);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("world unload releases transient preview data"),
        WorldUnloadMesh.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewLocalRetopologyTest,
    "MtoULiveLink.Editor.Preview.LocalRetopology",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewLocalRetopologyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    USkeletalMesh* Driver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUPreviewRetopologyWorld"));
    UStaticMesh* Preview = Driver ? MakePreview(*Driver, *WorldPackage, true) : nullptr;
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUPreviewRetopologyWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = Preview;

    const FMeshDescription* DriverDescription = Driver ? Driver->GetMeshDescription(0) : nullptr;
    const FMeshDescription* PreviewDescription = Preview ? Preview->GetMeshDescription(0) : nullptr;
    TestTrue(TEXT("fixed local-retopology garment changes LOD0 topology"),
        DriverDescription && PreviewDescription
        && PreviewDescription->Vertices().Num() == DriverDescription->Vertices().Num() + 1);

    const FMtoUPreviewPreparationResult Result = Actor
        ? MtoUPreparePreview(*Actor, *Binding)
        : FMtoUPreviewPreparationResult();
    AddInfo(Result.Diagnostics);
    TestTrue(TEXT("local-retopology garment builds a skinned LOD0 preview"),
        Result.bSucceeded && Result.GeneratedPreview);
    TestTrue(TEXT("selected V1 Inpaint comparison records both public modes"),
        Result.ClosestTransferMilliseconds > 0.0
        && Result.InpaintTransferMilliseconds > 0.0);

    UDynamicMesh* GeneratedDynamic = NewObject<UDynamicMesh>(WorldPackage);
    FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
    FGeometryScriptMeshReadLOD ReadLOD;
    ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
    EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
    if (Result.GeneratedPreview)
    {
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
            Result.GeneratedPreview, GeneratedDynamic, ReadOptions, ReadLOD, Outcome);
    }
    bool bEveryVertexSkinned = Outcome == EGeometryScriptOutcomePins::Success;
    int32 RepresentativeBoneIndex = INDEX_NONE;
    if (bEveryVertexSkinned)
    {
        GeneratedDynamic->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
        {
            const UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* SkinWeights =
                Mesh.Attributes()->GetSkinWeightsAttribute(
                    FSkeletalMeshAttributes::DefaultSkinWeightProfileName);
            for (const int32 VertexID : Mesh.VertexIndicesItr())
            {
                UE::AnimationCore::FBoneWeights Weights;
                if (!SkinWeights)
                {
                    bEveryVertexSkinned = false;
                    break;
                }
                SkinWeights->GetValue(VertexID, Weights);
                bEveryVertexSkinned &= Weights.Num() > 0;
                for (int32 Index = 0; Index < Weights.Num(); ++Index)
                {
                    if (Weights[Index].GetBoneIndex() > 0 && Weights[Index].GetWeight() > 0.0f)
                    {
                        RepresentativeBoneIndex = Weights[Index].GetBoneIndex();
                    }
                }
            }
        });
    }
    TestTrue(TEXT("every local-retopology vertex receives skin weights"), bEveryVertexSkinned);
    bool bRepresentativeBoneMovesPreview = false;
    UPoseableMeshComponent* Poseable = Actor
        ? NewObject<UPoseableMeshComponent>(Actor)
        : nullptr;
    const FSkeletalMeshRenderData* RenderData = Result.GeneratedPreview
        ? Result.GeneratedPreview->GetResourceForRendering()
        : nullptr;
    if (Poseable && World && Result.GeneratedPreview && RenderData
        && RenderData->LODRenderData.IsValidIndex(0)
        && Result.GeneratedPreview->GetRefSkeleton().IsValidIndex(RepresentativeBoneIndex))
    {
        Poseable->SetSkinnedAssetAndUpdate(Result.GeneratedPreview, true);
        Poseable->RegisterComponentWithWorld(World);
        Poseable->RefreshBoneTransforms();

        const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
        const FSkinWeightVertexBuffer* SkinWeights = LODData.GetSkinWeightVertexBuffer();
        if (SkinWeights)
        {
            TArray<FVector3f> ReferencePositions;
            TArray<FMatrix44f> RefToLocals;
            Poseable->GetCurrentRefToLocalMatrices(RefToLocals, 0);
            USkinnedMeshComponent::ComputeSkinnedPositions(
                Poseable, ReferencePositions, RefToLocals, LODData, *SkinWeights);

            const FName BoneName = Result.GeneratedPreview->GetRefSkeleton()
                .GetBoneName(RepresentativeBoneIndex);
            FTransform PosedTransform = Poseable->GetBoneTransformByName(
                BoneName, EBoneSpaces::ComponentSpace);
            PosedTransform.AddToTranslation(FVector(10.0, 0.0, 0.0));
            Poseable->SetBoneTransformByName(
                BoneName, PosedTransform, EBoneSpaces::ComponentSpace);
            Poseable->RefreshBoneTransforms();

            TArray<FVector3f> PosedPositions;
            RefToLocals.Reset();
            Poseable->GetCurrentRefToLocalMatrices(RefToLocals, 0);
            USkinnedMeshComponent::ComputeSkinnedPositions(
                Poseable, PosedPositions, RefToLocals, LODData, *SkinWeights);
            if (ReferencePositions.Num() == PosedPositions.Num())
            {
                for (int32 Index = 0; Index < ReferencePositions.Num(); ++Index)
                {
                    if (!ReferencePositions[Index].Equals(PosedPositions[Index]))
                    {
                        bRepresentativeBoneMovesPreview = true;
                        break;
                    }
                }
            }
        }
        Poseable->DestroyComponent();
    }
    TestTrue(TEXT("a representative non-root bone pose deforms the preview"),
        bRepresentativeBoneMovesPreview);

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewMorphProjectionTest,
    "MtoULiveLink.Editor.Preview.MorphProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewMorphProjectionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUPreviewMorphWorld"));
    USkeletalMesh* Driver = MakeMorphDriver(*WorldPackage);
    UStaticMesh* Preview = Driver ? MakePreview(*Driver, *WorldPackage, true, true) : nullptr;
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUPreviewMorphWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = Preview;

    const FMtoUPreviewPreparationResult Result = Actor
        ? MtoUPreparePreview(*Actor, *Binding)
        : FMtoUPreviewPreparationResult();
    AddInfo(Result.Diagnostics);
    const TMap<FName, FVector3f> ExpectedDeltas = {
        {FName(TEXT("Corrective")), FVector3f(2.0f, 0.0f, 0.0f)},
        {FName(TEXT("CorrectiveNegative")), FVector3f(-2.0f, 0.0f, 0.0f)},
        {FName(TEXT("Left")), FVector3f(0.0f, 1.5f, 0.0f)},
        {FName(TEXT("Right")), FVector3f(0.0f, -1.5f, 0.0f)},
        {FName(TEXT("Stress")), FVector3f(0.0f, 0.0f, 0.75f)},
    };
    TestEqual(TEXT("Refresh generates the complete Driver Morph library"),
        Result.MorphTargetCount, ExpectedDeltas.Num());
    TestTrue(TEXT("acceptance diagnostics report Preview triangle count"),
        Result.TriangleCount > 0);

    bool bDirectionAndAmplitudePreserved = Result.GeneratedPreview != nullptr;
    const FSkeletalMeshModel* GeneratedModel = Result.GeneratedPreview
        ? Result.GeneratedPreview->GetImportedModel()
        : nullptr;
    const FSkeletalMeshLODModel* GeneratedLOD = GeneratedModel
        && GeneratedModel->LODModels.IsValidIndex(0)
        ? &GeneratedModel->LODModels[0]
        : nullptr;
    const FMeshDescription* GeneratedDescription = Result.GeneratedPreview
        ? Result.GeneratedPreview->GetMeshDescription(0)
        : nullptr;
    bool bHasSplitPosition = false;
    if (GeneratedDescription)
    {
        const TVertexAttributesConstRef<FVector3f> Positions =
            GeneratedDescription->GetVertexPositions();
        for (const FVertexID A : GeneratedDescription->Vertices().GetElementIDs())
        {
            for (const FVertexID B : GeneratedDescription->Vertices().GetElementIDs())
            {
                if (A < B && Positions[A].Equals(Positions[B]))
                {
                    bHasSplitPosition = true;
                }
            }
        }
    }
    bool bSeamsRemainContinuous = GeneratedLOD && bHasSplitPosition;
    for (const TPair<FName, FVector3f>& Expected : ExpectedDeltas)
    {
        UMorphTarget* GeneratedMorph = Result.GeneratedPreview
            ? Result.GeneratedPreview->FindMorphTarget(Expected.Key)
            : nullptr;
        bDirectionAndAmplitudePreserved &= GeneratedMorph != nullptr;
        if (!GeneratedMorph)
        {
            continue;
        }
        for (const FMorphTargetDelta& Delta : GeneratedMorph->GetMorphTargetDeltas(0))
        {
            bDirectionAndAmplitudePreserved &= Delta.PositionDelta.Equals(
                Expected.Value, 1.0e-3f)
                && !Delta.PositionDelta.ContainsNaN();
        }
        bDirectionAndAmplitudePreserved &= GeneratedMorph->GetNumDeltasForLOD(0) > 0;
        bSeamsRemainContinuous &= GeneratedLOD
            && GeneratedMorph->GetNumDeltasForLOD(0) == static_cast<int32>(GeneratedLOD->NumVertices);
    }
    TestTrue(TEXT("positive/negative and left/right Morphs preserve direction and amplitude"),
        bDirectionAndAmplitudePreserved);
    TestTrue(TEXT("dense UV seam duplicates receive continuous projected deltas"),
        bSeamsRemainContinuous);
    TestTrue(TEXT("double-layer thickness geometry is retained"),
        Result.VertexCount == 50 && Result.TriangleCount == 28);

    UMorphTarget* Corrective = Result.GeneratedPreview
        ? Result.GeneratedPreview->FindMorphTarget(FName(TEXT("Corrective")))
        : nullptr;
    const TConstArrayView<FMorphTargetDelta> CorrectiveDeltas = Corrective
        ? Corrective->GetMorphTargetDeltas(0)
        : TConstArrayView<FMorphTargetDelta>();
    TestTrue(TEXT("representative Morph has linear visual progression from weight 0 to 1"),
        !CorrectiveDeltas.IsEmpty()
        && (CorrectiveDeltas[0].PositionDelta * 0.5f).Equals(FVector3f(1.0f, 0.0f, 0.0f))
        && CorrectiveDeltas[0].PositionDelta.Equals(FVector3f(2.0f, 0.0f, 0.0f)));

    Actor->SetBinding(Binding);
    const auto SparseDeltaCount = [](const USkeletalMesh* Mesh)
    {
        int64 Total = 0;
        if (Mesh)
        {
            for (const TObjectPtr<UMorphTarget>& Morph : Mesh->GetMorphTargets())
            {
                if (Morph)
                {
                    Total += Morph->GetNumDeltasForLOD(0);
                }
            }
        }
        return Total;
    };
    const FMtoUPreviewReadiness FirstRefresh = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TWeakObjectPtr<USkeletalMesh> FirstLibrary = FirstRefresh.GeneratedPreview;
    const FMtoUPreviewReadiness SecondRefresh = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TWeakObjectPtr<USkeletalMesh> SecondLibrary = SecondRefresh.GeneratedPreview;
    const int32 FirstMorphCount = FirstRefresh.GeneratedPreview
        ? FirstRefresh.GeneratedPreview->GetMorphTargets().Num() : 0;
    const int64 FirstSparse = SparseDeltaCount(FirstRefresh.GeneratedPreview);
    const int32 SecondMorphCount = SecondRefresh.GeneratedPreview
        ? SecondRefresh.GeneratedPreview->GetMorphTargets().Num() : 0;
    const int64 SecondSparse = SparseDeltaCount(SecondRefresh.GeneratedPreview);
    TestTrue(TEXT("repeated Refresh replaces the complete transient Morph library"),
        FirstRefresh.IsUsable() && SecondRefresh.IsUsable()
        && FirstLibrary != SecondLibrary
        && FirstMorphCount == SecondMorphCount
        && FirstSparse == SecondSparse);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("repeated Refresh releases the previous transient Morph library"),
        FirstLibrary.IsValid());

    Driver->GetMorphTargets().Add(NewObject<UMorphTarget>(
        Driver, FName(TEXT("BrokenRequired")), RF_Transient));
    const FMtoUPreviewReadiness FailedRefresh = FMtoUPreviewPreparation::RefreshActor(*Actor);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("one required Morph failure transactionally discards stale preview and restores the Driver"),
        !FailedRefresh.IsUsable()
        && FailedRefresh.GeneratedPreview == nullptr
        && FailedRefresh.Stage == EMtoUPreviewBuildStage::SkeletalMeshBuild
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
        && !Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver
        && !SecondLibrary.IsValid());

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewMissingMorphSurfaceTest,
    "MtoULiveLink.Editor.Preview.MissingMorphSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewMissingMorphSurfaceTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUMissingMorphSurfaceWorld"));
    USkeletalMesh* Driver = MakeMorphDriver(*WorldPackage, true);
    TestTrue(TEXT("localized Driver Morph fixture is created"), Driver
        && AddPositiveXMorph(
            *Driver, FName(TEXT("NoMatchingSurface")), FVector3f(1.0f, 0.0f, 0.0f)));
    UStaticMesh* Preview = Driver
        ? MakePreview(*Driver, *WorldPackage, false, false, true)
        : nullptr;
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false,
        TEXT("MtoUMissingMorphSurfaceWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = Preview;
    Binding->DriverGarmentSlotOverride.Add(FName(TEXT("MaterialSlot")));
    if (Actor)
    {
        Actor->SetBinding(Binding);
    }

    const FMtoUPreviewPreparationResult Result = Actor
        ? MtoUPreparePreview(*Actor, *Binding)
        : FMtoUPreviewPreparationResult();
    AddInfo(Result.Diagnostics);
    TestTrue(TEXT("Refresh succeeds when a local Driver Morph has no Preview surface"),
        Result.bSucceeded && Result.GeneratedPreview);
    TestEqual(TEXT("only the unsupported local Morph is skipped"),
        Result.SkippedMorphTargetCount, 1);
    TestEqual(TEXT("all supported Morphs remain projected"), Result.MorphTargetCount, 5);
    TestNull(TEXT("the unsupported Morph is absent from the Generated library"),
        Result.GeneratedPreview
            ? Result.GeneratedPreview->FindMorphTarget(FName(TEXT("NoMatchingSurface")))
            : nullptr);

    const FMtoUPreviewReadiness Readiness = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("the warning readiness is usable and displayed"),
        Readiness.State == EMtoUPreviewState::Warning
        && Readiness.IsUsable()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
            == Readiness.GeneratedPreview);

    FMtoUPreviewQualityThresholds UnsafeThresholds;
    UnsafeThresholds.MaxWarningInpaintRatio = Result.InpaintLowConfidenceRatio - 0.01;
    const FMtoUPreviewPreparationResult UnsafeResult = Actor
        ? MtoUPreparePreview(*Actor, *Binding, {}, UnsafeThresholds)
        : FMtoUPreviewPreparationResult();
    TestTrue(TEXT("an Error quality verdict transactionally discards the generated preview"),
        !UnsafeResult.bSucceeded
        && UnsafeResult.GeneratedPreview == nullptr
        && UnsafeResult.FailureStage == EMtoUPreviewBuildStage::Validation
        && UnsafeResult.Quality == EMtoUPreviewQuality::Error);

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewMeshDataTest,
    "MtoULiveLink.Editor.Preview.MeshData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewMeshDataTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    USkeletalMesh* Driver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUPreviewMeshDataWorld"));
    UStaticMesh* Preview = Driver ? MakePreview(*Driver, *WorldPackage) : nullptr;
    FMeshDescription* SourceDescription = Preview ? Preview->GetMeshDescription(0) : nullptr;
    if (!SourceDescription)
    {
        AddError(TEXT("mesh-data fixture has no source description"));
        return false;
    }

    FStaticMeshAttributes SourceAttributes(*SourceDescription);
    TVertexInstanceAttributesRef<FVector4f> SourceColors =
        SourceAttributes.GetVertexInstanceColors();
    TVertexInstanceAttributesRef<FVector2f> SourceUVs =
        SourceAttributes.GetVertexInstanceUVs();
    SourceUVs.SetNumChannels(FMath::Max(2, SourceUVs.GetNumChannels()));
    const FVector4f ExpectedColor(0.25f, 0.5f, 0.75f, 1.0f);
    const FVector2f ExpectedSecondUV(0.125f, 0.875f);
    for (const FVertexInstanceID VertexInstanceID
        : SourceDescription->VertexInstances().GetElementIDs())
    {
        SourceColors.Set(VertexInstanceID, ExpectedColor);
        SourceUVs.Set(VertexInstanceID, 1, ExpectedSecondUV);
    }
    Preview->CommitMeshDescription(0);

    UMaterialInterface* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
    TArray<FStaticMaterial> Materials;
    Materials.Emplace(DefaultMaterial, FName(TEXT("Outer")));
    Materials.Emplace(nullptr, FName(TEXT("Empty")));
    Materials.Emplace(DefaultMaterial, FName(TEXT("Inner")));
    Preview->SetStaticMaterials(Materials);

    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUPreviewMeshDataWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = Preview;
    const FMtoUPreviewPreparationResult Result = Actor
        ? MtoUPreparePreview(*Actor, *Binding)
        : FMtoUPreviewPreparationResult();
    AddInfo(Result.Diagnostics);
    FMeshDescription* GeneratedDescription = Result.GeneratedPreview
        ? Result.GeneratedPreview->GetMeshDescription(0)
        : nullptr;
    TestNotNull(TEXT("mesh-data preview builds LOD0 source data"), GeneratedDescription);
    if (GeneratedDescription)
    {
        FSkeletalMeshAttributes GeneratedAttributes(*GeneratedDescription);
        TVertexInstanceAttributesRef<FVector3f> SourceNormals =
            SourceAttributes.GetVertexInstanceNormals();
        TVertexInstanceAttributesRef<FVector3f> SourceTangents =
            SourceAttributes.GetVertexInstanceTangents();
        TVertexInstanceAttributesRef<float> SourceBinormalSigns =
            SourceAttributes.GetVertexInstanceBinormalSigns();
        TVertexInstanceAttributesRef<FVector3f> GeneratedNormals =
            GeneratedAttributes.GetVertexInstanceNormals();
        TVertexInstanceAttributesRef<FVector3f> GeneratedTangents =
            GeneratedAttributes.GetVertexInstanceTangents();
        TVertexInstanceAttributesRef<float> GeneratedBinormalSigns =
            GeneratedAttributes.GetVertexInstanceBinormalSigns();
        TVertexInstanceAttributesRef<FVector4f> GeneratedColors =
            GeneratedAttributes.GetVertexInstanceColors();
        TVertexInstanceAttributesRef<FVector2f> GeneratedUVs =
            GeneratedAttributes.GetVertexInstanceUVs();
        TestEqual(TEXT("UV channel count is preserved"),
            GeneratedUVs.GetNumChannels(), SourceUVs.GetNumChannels());
        for (const FVertexInstanceID VertexInstanceID
            : SourceDescription->VertexInstances().GetElementIDs())
        {
            TestTrue(TEXT("normal is preserved"),
                GeneratedNormals.Get(VertexInstanceID).Equals(
                    SourceNormals.Get(VertexInstanceID)));
            TestTrue(TEXT("tangent is preserved"),
                GeneratedTangents.Get(VertexInstanceID).Equals(
                    SourceTangents.Get(VertexInstanceID)));
            TestEqual(TEXT("binormal sign is preserved"),
                GeneratedBinormalSigns.Get(VertexInstanceID),
                SourceBinormalSigns.Get(VertexInstanceID));
            TestTrue(TEXT("vertex color is preserved"),
                GeneratedColors.Get(VertexInstanceID).Equals(ExpectedColor));
            TestTrue(TEXT("second UV channel is preserved"),
                GeneratedUVs.Get(VertexInstanceID, 1).Equals(ExpectedSecondUV));
        }
    }

    const TArray<FSkeletalMaterial>& GeneratedMaterials = Result.GeneratedPreview
        ? Result.GeneratedPreview->GetMaterials()
        : TArray<FSkeletalMaterial>();
    TestEqual(TEXT("material slot count and empty slots are preserved"),
        GeneratedMaterials.Num(), Materials.Num());
    for (int32 Index = 0; Index < FMath::Min(GeneratedMaterials.Num(), Materials.Num()); ++Index)
    {
        TestTrue(TEXT("asset material assignment is preserved"),
            GeneratedMaterials[Index].MaterialInterface == Materials[Index].MaterialInterface);
        TestEqual(TEXT("material slot order and names are preserved"),
            GeneratedMaterials[Index].MaterialSlotName, Materials[Index].MaterialSlotName);
    }

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewSourceFailuresTest,
    "MtoULiveLink.Editor.Preview.SourceFailures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewSourceFailuresTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    USkeletalMesh* Driver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUPreviewFailureWorld"));
    UStaticMesh* ValidPreview = Driver ? MakePreview(*Driver, *WorldPackage) : nullptr;
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUPreviewFailureWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = NewObject<UStaticMesh>(WorldPackage);

    const FMtoUPreviewPreparationResult MissingSource = Actor
        ? MtoUPreparePreview(*Actor, *Binding)
        : FMtoUPreviewPreparationResult();
    TestTrue(TEXT("missing LOD0 source data fails without RenderData fallback"),
        !MissingSource.bSucceeded
        && MissingSource.GeneratedPreview == nullptr
        && MissingSource.FailureStage == EMtoUPreviewBuildStage::Preflight
        && MissingSource.Diagnostics.Contains(TEXT("source data is unavailable")));

    USkeletalMesh* MalformedDriver = Driver
        ? NewObject<USkeletalMesh>(WorldPackage, NAME_None, RF_Transient)
        : nullptr;
    if (MalformedDriver && Driver)
    {
        MalformedDriver->SetSkeleton(Driver->GetSkeleton());
        MalformedDriver->SetRefSkeleton(Driver->GetRefSkeleton());
        FMeshDescription IndependentDescription;
        if (Driver->CloneMeshDescription(0, IndependentDescription))
        {
            MalformedDriver->SetNumSourceModels(1);
            MalformedDriver->CreateMeshDescription(0, MoveTemp(IndependentDescription));
        }
    }
    FMeshDescription* MalformedDescription = MalformedDriver
        ? MalformedDriver->GetMeshDescription(0)
        : nullptr;
    if (MalformedDescription)
    {
        FSkeletalMeshAttributes MalformedAttributes(*MalformedDescription);
        MalformedAttributes.GetBoneNames().Set(0, FName(TEXT("MissingInfluenceBone")));
    }
    Binding->SkeletalMesh = MalformedDriver;
    Binding->PreviewStaticMesh = ValidPreview;
    const FMtoUPreviewPreparationResult MissingInfluence = Actor
        ? MtoUPreparePreview(*Actor, *Binding)
        : FMtoUPreviewPreparationResult();
    AddInfo(MissingInfluence.Diagnostics);
    TestTrue(TEXT("a source influence bone absent from target skeleton is blocking"),
        !MissingInfluence.bSucceeded
        && MissingInfluence.GeneratedPreview == nullptr
        && MissingInfluence.FailureStage == EMtoUPreviewBuildStage::GeometryConversion
        && MissingInfluence.Diagnostics.Contains(TEXT("absent from the target skeleton")));

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewQualityBoundaryTest,
    "MtoULiveLink.Editor.Preview.QualityBoundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewQualityBoundaryTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FMtoUPreviewQualityThresholds Thresholds;
    const double Epsilon = 1.0e-6;
    FString Reason;

    TestEqual(TEXT("average distance immediately below the misalignment bound is not an error by itself"),
        static_cast<uint8>(MtoUEvaluatePreviewQuality(
            0.0, Thresholds.MisalignedNormalizedDistanceAverage - Epsilon, Thresholds, Reason)),
        static_cast<uint8>(EMtoUPreviewQuality::Ready));
    TestEqual(TEXT("average distance immediately above the misalignment bound fails"),
        static_cast<uint8>(MtoUEvaluatePreviewQuality(
            0.0, Thresholds.MisalignedNormalizedDistanceAverage + Epsilon, Thresholds, Reason)),
        static_cast<uint8>(EMtoUPreviewQuality::Error));
    TestTrue(TEXT("the misaligned reason names the boundary"),
        Reason.Contains(TEXT("misaligned")));

    TestEqual(TEXT("ratio immediately below the ready boundary stays Ready"),
        static_cast<uint8>(MtoUEvaluatePreviewQuality(
            Thresholds.MaxReadyInpaintRatio - Epsilon, 0.0, Thresholds, Reason)),
        static_cast<uint8>(EMtoUPreviewQuality::Ready));
    TestEqual(TEXT("ratio immediately above the ready boundary warns"),
        static_cast<uint8>(MtoUEvaluatePreviewQuality(
            Thresholds.MaxReadyInpaintRatio + Epsilon, 0.0, Thresholds, Reason)),
        static_cast<uint8>(EMtoUPreviewQuality::Warning));
    TestEqual(TEXT("ratio immediately below the warning boundary still warns"),
        static_cast<uint8>(MtoUEvaluatePreviewQuality(
            Thresholds.MaxWarningInpaintRatio - Epsilon, 0.0, Thresholds, Reason)),
        static_cast<uint8>(EMtoUPreviewQuality::Warning));
    TestEqual(TEXT("ratio immediately above the warning boundary fails"),
        static_cast<uint8>(MtoUEvaluatePreviewQuality(
            Thresholds.MaxWarningInpaintRatio + Epsilon, 0.0, Thresholds, Reason)),
        static_cast<uint8>(EMtoUPreviewQuality::Error));
    TestTrue(TEXT("every verdict carries a measured reason"),
        !Reason.IsEmpty() && Reason.Contains(FString::SanitizeFloat(Thresholds.MaxWarningInpaintRatio)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewMisalignmentTest,
    "MtoULiveLink.Editor.Preview.Misalignment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewMisalignmentTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    USkeletalMesh* Driver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUMisalignedWorld"));
    UStaticMesh* AlignedPreview = Driver ? MakePreview(*Driver, *WorldPackage) : nullptr;
    UStaticMesh* MisalignedPreview = Driver ? MakePreview(*Driver, *WorldPackage, false, false, false, true) : nullptr;
    // The binding input drops each preview mid-test; keep the transient fixtures alive.
    TStrongObjectPtr<UStaticMesh> AlignedGuard(AlignedPreview);
    TStrongObjectPtr<UStaticMesh> MisalignedGuard(MisalignedPreview);
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUMisalignedWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = MisalignedPreview;
    if (!Driver || !AlignedPreview || !MisalignedPreview || !Actor)
    {
        AddError(TEXT("misalignment fixtures were not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }
    Actor->SetBinding(Binding);

    FProperty* PreviewInputProperty = FindFProperty<FProperty>(
        UMtoULiveLinkBinding::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, PreviewStaticMesh));
    FPropertyChangedEvent PreviewInputChanged(PreviewInputProperty);

    const FMtoUPreviewPreparationResult Misaligned =
        MtoUPreparePreview(*Actor, *Binding);
    AddInfo(Misaligned.Diagnostics);
    TestFalse(TEXT("an intentionally misaligned input cannot produce a usable preview"),
        Misaligned.bSucceeded);
    TestNull(TEXT("the misaligned preparation returns no mesh"), Misaligned.GeneratedPreview);
    TestEqual(TEXT("misalignment is rejected at quality validation of the converted inputs"),
        Misaligned.FailureStage, EMtoUPreviewBuildStage::GeometryConversion);
    TestTrue(TEXT("the misaligned reason names the calibrated bound"),
        Misaligned.Diagnostics.Contains(TEXT("misaligned"))
        && Misaligned.Diagnostics.Contains(*FString::SanitizeFloat(
            FMtoUPreviewQualityThresholds().MisalignedNormalizedDistanceAverage)));

    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = AlignedPreview;
    {
        const FMtoUPreviewReadiness Aligned =
            FMtoUPreviewPreparation::RefreshActor(*Actor);
        TestTrue(TEXT("the aligned input recovers with a complete preview"),
            Aligned.IsUsable() && Actor->GetPreviewReadiness().IsUsable());
    }

    Binding->PreviewStaticMesh = MisalignedPreview;
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TWeakObjectPtr<USkeletalMesh> StaleMesh = Actor->GetPreviewReadiness().GeneratedPreview;
    const FMtoUPreviewReadiness Failed =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("a misaligned Refresh transactionally discards stale preview and restores the Driver"),
        !Failed.IsUsable()
        && Failed.GeneratedPreview == nullptr
        && Failed.State == EMtoUPreviewState::Error
        && Failed.Stage == EMtoUPreviewBuildStage::GeometryConversion
        && !StaleMesh.IsValid()
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);

    Binding->PreviewStaticMesh = AlignedPreview;
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestTrue(TEXT("Refresh recovers after a misaligned attempt"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

namespace
{
using MtoUEditorTest::AppendPartCopy;
using MtoUEditorTest::FMtoUFullCharacterFixtures;
using MtoUEditorTest::MakeFullCharacterFixtures;
using MtoUEditorTest::SetUniformBoneWeights;
using MtoUEditorTest::WriteTransientDriver;
using UE::Geometry::FAxisAlignedBox3d;
using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FIndex3i;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewFullCharacterTest,
    "MtoULiveLink.Editor.Preview.FullCharacter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewFullCharacterTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUFullCharacterWorld"));
    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        WorldPackage->GetName(), FPackageName::GetAssetPackageExtension());
    FMtoUFullCharacterFixtures Fixtures;
    TestTrue(TEXT("full-character fixtures are created"),
        MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures));
    if (!Fixtures.IsValid())
    {
        AddError(TEXT("full-character fixtures were not created"));
        return false;
    }
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUFullCharacterWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    Binding->SkeletalMesh = Fixtures.FullDriver;
    Binding->PreviewStaticMesh = Fixtures.Preview;
    if (!Actor || World == nullptr)
    {
        AddError(TEXT("full-character world was not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }
    Actor->SetBinding(Binding);
    TestTrue(TEXT("Animation preview keeps showing the complete Driver before Refresh"),
        Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Fixtures.FullDriver);

    TArray<EMtoUPreviewBuildStage> ObservedStages;
    const FMtoUPreviewPreparationResult Result = MtoUPreparePreview(
        *Actor, *Binding,
        [&ObservedStages](EMtoUPreviewBuildStage Stage) { ObservedStages.Add(Stage); });
    AddInfo(Result.Diagnostics);
    AddInfo(Result.Summary);
    TestTrue(TEXT("full-character Driver builds a garment-only Generated Preview"),
        Result.bSucceeded && Result.GeneratedPreview != nullptr);
    TestTrue(TEXT("resolved garment slots are retained for full-character display"),
        Result.DriverGarmentMaterialSlotIndices == TArray<int32>({3, 4, 5}));
    TestEqual(TEXT("all five preparation stages remain observable"),
        ObservedStages.Num(), 5);
    TestTrue(TEXT("diagnostics identify the resolved regions and verdict"),
        Result.Diagnostics.Contains(TEXT("garment region"))
        && Result.Diagnostics.Contains(TEXT("matched Preview coverage"))
        && Result.Diagnostics.Contains(TEXT("verdict")));
    TestTrue(TEXT("artist summary names the modified Preview parts"),
        Result.Summary.Contains(TEXT("Cloth09_Top_1"))
        && Result.Summary.Contains(TEXT("Cloth09_Bottom_2")));
    TestFalse(TEXT("artist summary omits technical metrics"),
        Result.Summary.Contains(TEXT("tris"))
        || Result.Summary.Contains(TEXT("vertices"))
        || Result.Summary.Contains(TEXT("ms"))
        || Result.Summary.Contains(TEXT("ratio")));
    TestTrue(TEXT("artist summary puts each modified part on its own line"),
        Result.Summary.Contains(TEXT("\n"))
        && !Result.Summary.Contains(TEXT(",")));
    for (const TCHAR* Part : { TEXT("Cloth09_Top_1"), TEXT("Cloth09_Bottom_2") })
    {
        TestEqual(FString::Printf(TEXT("artist summary lists %s once"), Part),
            Result.Summary.Find(Part),
            Result.Summary.Find(Part, ESearchCase::CaseSensitive, ESearchDir::FromEnd));
    }

    // Generated geometry: garment surface only, nothing from other parts.
    const FAxisAlignedBox3d GarmentCheckBounds(
        Fixtures.GarmentBounds.Min - 0.5, Fixtures.GarmentBounds.Max + 0.5);
    bool bGarmentOnlyGeometry = false;
    int32 GeneratedTriangleCount = -1;
    int32 OutsideGarmentCount = 0;
    int32 InsideBodyCount = 0;
    if (Result.GeneratedPreview && Result.GeneratedPreview->HasMeshDescription(0))
    {
        const FMeshDescription* GeneratedDescription =
            Result.GeneratedPreview->GetMeshDescription(0);
        GeneratedTriangleCount = GeneratedDescription->Triangles().Num();
        const TVertexAttributesConstRef<FVector3f> Positions =
            GeneratedDescription->GetVertexPositions();
        bGarmentOnlyGeometry = GeneratedTriangleCount == Fixtures.PreviewTriangleCount;
        for (const FVertexID VertexID : GeneratedDescription->Vertices().GetElementIDs())
        {
            const FVector3d Position(Positions[VertexID]);
            if (!GarmentCheckBounds.Contains(Position))
            {
                ++OutsideGarmentCount;
            }
            if (Fixtures.BodyCoreBounds.Contains(Position))
            {
                ++InsideBodyCount;
            }
        }
        bGarmentOnlyGeometry &= OutsideGarmentCount == 0 && InsideBodyCount == 0;
        AddInfo(FString::Printf(
            TEXT("purity probe: %d generated triangles (preview %d), %d outside garment bounds, %d inside body core"),
            GeneratedTriangleCount, Fixtures.PreviewTriangleCount,
            OutsideGarmentCount, InsideBodyCount));
    }
    TestTrue(TEXT("Generated Preview contains only the Preview garment surface"),
        bGarmentOnlyGeometry);

    TestTrue(TEXT("Generated Preview retains the complete Driver reference skeleton"),
        Result.GeneratedPreview
        && Result.GeneratedPreview->GetRefSkeleton().GetNum()
            == Fixtures.FullDriver->GetRefSkeleton().GetNum()
        && Result.GeneratedPreview->GetSkeleton() == Fixtures.FullDriver->GetSkeleton());

    const TArray<FSkeletalMaterial>& GeneratedMaterials =
        Result.GeneratedPreview ? Result.GeneratedPreview->GetMaterials()
            : TArray<FSkeletalMaterial>();
    TestEqual(TEXT("Generated Preview uses only the Preview material slots"),
        GeneratedMaterials.Num(), 2);
    TestEqual(TEXT("Preview slot names replace the Driver section names"),
        GeneratedMaterials.Num() == 2
            ? GeneratedMaterials[0].MaterialSlotName.ToString()
            : FString(),
        FString(TEXT("Cloth09_Top_1")));
    TestEqual(TEXT("second Preview slot name is preserved"),
        GeneratedMaterials.Num() == 2
            ? GeneratedMaterials[1].MaterialSlotName.ToString()
            : FString(),
        FString(TEXT("Cloth09_Bottom_2")));

    // Morph filtering: only the garment morph reaches the generated library.
    const auto GeneratedMorph = [&Result](const TCHAR* Name) -> UMorphTarget*
    {
        return Result.GeneratedPreview
            ? Result.GeneratedPreview->FindMorphTarget(FName(Name))
            : nullptr;
    };
    TestEqual(TEXT("only nonzero garment-surface Morphs are generated"),
        Result.MorphTargetCount, 1);
    TestEqual(TEXT("body, face, and hair Morphs are skipped entirely"),
        Result.SkippedMorphTargetCount, 3);
    TestNotNull(TEXT("garment Morph is present in the Generated library"),
        GeneratedMorph(TEXT("GarmentFlare")));
    TestNull(TEXT("arm attachment Morph is absent from the Generated library"),
        GeneratedMorph(TEXT("ArmRaise")));
    TestNull(TEXT("face Morph is absent from the Generated library"),
        GeneratedMorph(TEXT("FaceBlink")));
    TestNull(TEXT("hair Morph is absent from the Generated library"),
        GeneratedMorph(TEXT("HairSway")));

    // Model preview layers the generated garment over the Driver's untouched
    // body, face, hair, and attachment material slots.
    Actor->GetSkeletalMeshComponent()->SetLightingChannels(false, true, false);
    Actor->GetSkeletalMeshComponent()->SetCastInsetShadow(true);
    const FMtoUPreviewReadiness RefreshResult =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("explicit Refresh readies the garment preview"),
        RefreshResult.IsUsable()
        && (RefreshResult.State == EMtoUPreviewState::Ready
            || RefreshResult.State == EMtoUPreviewState::Warning)
        && Actor->GetPreviewReadiness().IsUsable());
    TestTrue(TEXT("Model preview displays the Generated Preview garment"),
        Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
            == RefreshResult.GeneratedPreview);
    TArray<USkeletalMeshComponent*> DisplayMeshes;
    Actor->GetComponents(DisplayMeshes);
    USkeletalMeshComponent** DriverDisplayEntry = DisplayMeshes.FindByPredicate(
        [Actor](const USkeletalMeshComponent* Component)
        {
            return Component != Actor->GetSkeletalMeshComponent();
        });
    USkeletalMeshComponent* DriverDisplay =
        DriverDisplayEntry ? *DriverDisplayEntry : nullptr;
    TestTrue(TEXT("Model preview keeps the original Driver as the character display"),
        DriverDisplay && DriverDisplay->GetSkeletalMeshAsset() == Fixtures.FullDriver);
    TestEqual(TEXT("Driver character display stays on the generated garment's LOD0"),
        DriverDisplay ? DriverDisplay->GetForcedLOD() : 0, 1);
    TestTrue(TEXT("hidden Driver display inherits the editable display lighting settings"),
        DriverDisplay
        && !DriverDisplay->LightingChannels.bChannel0
        && DriverDisplay->LightingChannels.bChannel1
        && !DriverDisplay->LightingChannels.bChannel2
        && DriverDisplay->bCastInsetShadow);
    for (const int32 VisibleSlot : {0, 1, 2, 6})
    {
        TestTrue(FString::Printf(TEXT("Driver slot %d remains visible"), VisibleSlot),
            DriverDisplay && DriverDisplay->IsMaterialSectionShown(VisibleSlot, 0));
    }
    for (const int32 HiddenSlot : {3, 4, 5})
    {
        TestFalse(FString::Printf(TEXT("original garment slot %d is hidden"), HiddenSlot),
            DriverDisplay && DriverDisplay->IsMaterialSectionShown(HiddenSlot, 0));
    }

    // Reimport invalidates and requires explicit Refresh again.
    if (GEditor)
    {
        GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetReimport(Fixtures.Preview);
    }
    TestTrue(TEXT("source Reimport marks Dirty and hides the stale garment"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && !Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr
        && DriverDisplay && DriverDisplay->GetSkeletalMeshAsset() == nullptr);
    TestFalse(TEXT("normal Refresh creates no .uasset on the filesystem"),
        IFileManager::Get().FileExists(*PackageFilename));

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewGarmentOverrideTest,
    "MtoULiveLink.Editor.Preview.GarmentOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewGarmentOverrideTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentOverrideWorld"));
    FMtoUFullCharacterFixtures Fixtures;
    TestTrue(TEXT("garment-override fixtures are created"),
        MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures));
    if (!Fixtures.IsValid())
    {
        AddError(TEXT("garment-override fixtures were not created"));
        return false;
    }
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUGarmentOverrideWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    if (!Actor || World == nullptr)
    {
        AddError(TEXT("garment-override world was not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }
    Binding->SkeletalMesh = Fixtures.FullDriver;
    Binding->PreviewStaticMesh = Fixtures.Preview;
    Actor->SetBinding(Binding);

    const FProperty* OverrideProperty = FindFProperty<FProperty>(
        UMtoULiveLinkBinding::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, DriverGarmentSlotOverride));
    TestTrue(TEXT("the advanced Driver garment slot override property exists and is editor-visible"),
        OverrideProperty != nullptr && OverrideProperty->HasAnyPropertyFlags(CPF_Edit));
    FPropertyChangedEvent OverrideChanged(
        const_cast<FProperty*>(OverrideProperty));

    const FName UpperA(TEXT("Garment_Upper_A"));
    const FName UpperB(TEXT("Garment_Upper_B"));
    const FName Lower(TEXT("Garment_Lower"));

    // Serialization: the override persists stable imported slot names, never
    // numeric section indices.
    Binding->DriverGarmentSlotOverride = {UpperA, UpperB, Lower};
    UMtoULiveLinkBinding* RoundTrip = DuplicateObject<UMtoULiveLinkBinding>(
        Binding, WorldPackage, TEXT("GarmentOverrideRoundTrip"));
    TestTrue(TEXT("saving/loading a binding preserves the unambiguous override unchanged"),
        RoundTrip
            && RoundTrip->DriverGarmentSlotOverride == TArray<FName>({UpperA, UpperB, Lower}));

    // Manual success crosses the whole Preview preparation seam: conversion,
    // transfer against the resolved surface, Morph filtering, Generated
    // Preview ownership, and readiness propagation.
    const FMtoUPreviewPreparationResult Manual =
        MtoUPreparePreview(*Actor, *Binding);
    AddInfo(Manual.Diagnostics);
    TestTrue(TEXT("a valid multi-slot override builds the complete preview"),
        Manual.bSucceeded && Manual.GeneratedPreview != nullptr);
    TestTrue(TEXT("diagnostics distinguish manual from automatic selection"),
        Manual.bManualGarmentSource
            && Manual.Diagnostics.Contains(TEXT("Manual"))
            && Manual.Diagnostics.Contains(TEXT("manual slots")));
    TestTrue(TEXT("the manual result keeps full matched coverage"),
        Manual.MatchedPreviewCoverage > 0.999);
    TestTrue(TEXT("the manual result projects only garment Morphs"),
        Manual.MorphTargetCount == 1 && Manual.SkippedMorphTargetCount == 3);
    TestTrue(TEXT("the manual Generated Preview keeps the complete Driver skeleton"),
        Manual.GeneratedPreview
            && Manual.GeneratedPreview->GetRefSkeleton().GetNum()
                == Fixtures.FullDriver->GetRefSkeleton().GetNum());

    const FMtoUPreviewReadiness Ready =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("the ready override preview is displayed"),
        Ready.IsUsable()
        && Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Ready.GeneratedPreview);

    // A partial multi-slot selection fails transactionally across the seam
    // instead of transferring against the full character.
    Binding->DriverGarmentSlotOverride = {UpperA};
    Binding->PostEditChangeProperty(OverrideChanged);
    const FMtoUPreviewReadiness Partial =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    AddInfo(Partial.Diagnostics);
    TestTrue(TEXT("a partial override fails transactionally through the refresh seam"),
        !Partial.IsUsable()
        && Partial.GeneratedPreview == nullptr
        && Partial.State == EMtoUPreviewState::Error
        && Partial.Stage == EMtoUPreviewBuildStage::GeometryConversion
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Fixtures.FullDriver);

    // Driver Reimport invalidates readiness, and a stale override keeps
    // failing through RefreshActor instead of silently returning to Auto.
    Binding->DriverGarmentSlotOverride = {UpperA, UpperB, Lower};
    Binding->PostEditChangeProperty(OverrideChanged);
    for (FSkeletalMaterial& Slot : Fixtures.FullDriver->GetMaterials())
    {
        if (Slot.MaterialSlotName == UpperA || Slot.MaterialSlotName == UpperB
            || Slot.MaterialSlotName == Lower)
        {
            Slot.ImportedMaterialSlotName =
                *(Slot.MaterialSlotName.ToString() + TEXT("_Renamed"));
            Slot.MaterialSlotName = Slot.ImportedMaterialSlotName;
        }
    }
    if (GEditor)
    {
        GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetReimport(
            Fixtures.FullDriver);
    }
    TestTrue(TEXT("Driver Reimport marks readiness Dirty and hides the stale garment"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && !Actor->GetPreviewReadiness().IsUsable());
    const FMtoUPreviewReadiness Stale =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    AddInfo(Stale.Diagnostics);
    TestTrue(TEXT("a stale override keeps failing after Reimport instead of returning to Auto"),
        !Stale.IsUsable()
        && Stale.GeneratedPreview == nullptr
        && Stale.Diagnostics.Contains(TEXT("unknown Driver material slot"))
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Fixtures.FullDriver);

    // Clearing the override recovers the safe automatic preview.
    Binding->DriverGarmentSlotOverride.Reset();
    Binding->PostEditChangeProperty(OverrideChanged);
    TestTrue(TEXT("clearing the override leaves readiness Dirty"),
        Actor->GetPreviewReadiness().State == EMtoUPreviewState::Dirty
        && !Actor->GetPreviewReadiness().IsUsable());
    const FMtoUPreviewReadiness AutoAgain =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    AddInfo(AutoAgain.Diagnostics);
    TestTrue(TEXT("clearing back to Auto recovers the safe automatic preview"),
        AutoAgain.IsUsable()
            && AutoAgain.GeneratedPreview != nullptr
            && AutoAgain.Diagnostics.Contains(TEXT("Auto selection"))
            && Actor->GetPreviewReadiness().IsUsable());

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUFullCharacterQualityCorpusTest,
    "MtoULiveLink.Editor.Preview.FullCharacterQualityCorpus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUFullCharacterQualityCorpusTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUFullCharacterQuality"));
    FMtoUFullCharacterFixtures Fixtures;
    TestTrue(TEXT("full-character quality fixtures are created"),
        MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures));
    if (!Fixtures.IsValid())
    {
        AddError(TEXT("full-character quality fixtures were not created"));
        return false;
    }
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUFullCharacterQuality"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    if (!Actor || World == nullptr)
    {
        AddError(TEXT("full-character quality world was not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }
    Binding->SkeletalMesh = Fixtures.FullDriver;

    struct FMtoUCase { const TCHAR* Name; EMtoUCorpusVariant Variant; bool bExpectFailure; };
    const FMtoUCase Cases[] = {
        {TEXT("SameTopology"), EMtoUCorpusVariant::SameTopology, false},
        {TEXT("LocalRetopology"), EMtoUCorpusVariant::LocalRetopology, false},
        {TEXT("DoubleLayerSeams"), EMtoUCorpusVariant::DoubleLayerSeams, false},
        {TEXT("MisalignedNegative"), EMtoUCorpusVariant::Misaligned, true},
    };
    for (const FMtoUCase& Case : Cases)
    {
        UStaticMesh* VariantPreview = MakeCorpusPreview(
            *Fixtures.FullDriver, Fixtures.Preview, *WorldPackage, Case.Variant);
        TStrongObjectPtr<UStaticMesh> VariantGuard(VariantPreview);
        if (!VariantPreview)
        {
            AddError(FString::Printf(
                TEXT("full-character corpus variant %s was not created"), Case.Name));
            continue;
        }
        Binding->PreviewStaticMesh = VariantPreview;
        FEnsureScope InpaintEnsureScope;
        const double StartSeconds = FPlatformTime::Seconds();
        const FMtoUPreviewPreparationResult Result =
            MtoUPreparePreview(*Actor, *Binding);
        const double TotalMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

        AddInfo(FString::Printf(
            TEXT("{\"corpus\":\"full-character\",\"case\":\"%s\",\"vertices\":%d,"
                "\"inpaint_ratio\":%.5f,\"distance_avg\":%.6f,\"distance_max\":%.6f,"
                "\"garment_source_regions\":%d,\"garment_source_triangles\":%d,"
                "\"matched_preview_coverage\":%.4f,\"total_refresh_ms\":%.1f}"),
            Case.Name,
            Result.VertexCount,
            Result.InpaintLowConfidenceRatio,
            Result.SurfaceDistanceAverage,
            Result.SurfaceDistanceMax,
            Result.GarmentSourceRegionCount,
            Result.GarmentSourceTriangleCount,
            Result.MatchedPreviewCoverage,
            TotalMilliseconds));
        AddInfo(Result.Diagnostics);

        if (Case.bExpectFailure)
        {
            TestTrue(FString::Printf(TEXT("%s fails transactionally through the full character"), Case.Name),
                !Result.bSucceeded
                && Result.GeneratedPreview == nullptr
                && Result.FailureStage == EMtoUPreviewBuildStage::GeometryConversion
                && Result.Diagnostics.Contains(TEXT("misaligned")));
        }
        else
        {
            TestTrue(FString::Printf(TEXT("%s stays inside the calibrated envelope"), Case.Name),
                Result.bSucceeded
                && Result.GeneratedPreview != nullptr
                && Result.Quality != EMtoUPreviewQuality::Error
                && Result.MatchedPreviewCoverage > 0.999
                && Result.SurfaceDistanceAverage
                    <= FMtoUPreviewQualityThresholds().MisalignedNormalizedDistanceAverage);
        }
    }

    // Metric normalization evidence: the full-character and garment-only
    // fixtures contain identical garment pieces, so a whole-surface
    // 0.1%-diagonal shift must yield identical normalized metrics despite the
    // full character's larger bounds.
    {
        TStrongObjectPtr<UStaticMesh> ShiftedPreview(MakeCorpusPreview(
            *Fixtures.FullDriver, Fixtures.Preview, *WorldPackage,
            EMtoUCorpusVariant::SmallMisalignment));
        Binding->DriverGarmentSlotOverride = {
            FName(TEXT("Garment_Upper_A")),
            FName(TEXT("Garment_Upper_B")),
            FName(TEXT("Garment_Lower"))};
        Binding->SkeletalMesh = Fixtures.FullDriver;
        Binding->PreviewStaticMesh = ShiftedPreview.Get();
        const FMtoUPreviewPreparationResult Full =
            MtoUPreparePreview(*Actor, *Binding);
        Binding->DriverGarmentSlotOverride = {
            FName(TEXT("Garment_Upper_A"))};
        Binding->SkeletalMesh = Fixtures.GarmentOnlyDriver;
        const FMtoUPreviewPreparationResult Legacy =
            MtoUPreparePreview(*Actor, *Binding);
        Binding->DriverGarmentSlotOverride.Reset();
        Binding->SkeletalMesh = Fixtures.FullDriver;
        AddInfo(Full.Diagnostics);
        AddInfo(Legacy.Diagnostics);
        AddInfo(FString::Printf(
            TEXT("normalization equivalence (0.001 shift): full avg %.12f rms %.12f vs legacy avg %.12f rms %.12f"),
            Full.SurfaceDistanceAverage, Full.SurfaceDistanceRms,
            Legacy.SurfaceDistanceAverage, Legacy.SurfaceDistanceRms));
        TestTrue(TEXT("resolved-garment normalization is unaffected by complete-character bounds"),
            Full.bSucceeded && Legacy.bSucceeded
            && Full.GarmentSourceTriangleCount == Legacy.GarmentSourceTriangleCount
            && Full.SurfaceDistanceAverage > 0.0
            && FMath::Abs(Full.SurfaceDistanceAverage
                - Legacy.SurfaceDistanceAverage) < 1.0e-9
            && FMath::Abs(Full.SurfaceDistanceRms
                - Legacy.SurfaceDistanceRms) < 1.0e-9);
    }

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUReducedTopologyTest,
    "MtoULiveLink.Editor.Preview.ReducedTopology",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUReducedTopologyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* Package = CreatePackage(TEXT("/Temp/MtoUReducedTopology"));
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUReducedTopology"), Package, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    if (!Actor)
    {
        if (World) { World->DestroyWorld(false); }
        return false;
    }
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(Package);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    Actor->SetBinding(Binding);
    const TArray<FName> GarmentSlots = {
        FName(TEXT("Garment_Upper_A")), FName(TEXT("Garment_Upper_B")),
        FName(TEXT("Garment_Lower"))};
    struct FRow { int32 Pokes; int32 Triangles; bool bAutoAccepted; };
    // Nearest attainable even triangle counts on either side of 72 / 1.7,
    // plus the original and a threefold density difference.
    const FRow Rows[] = {{-1, 72, true}, {10, 44, true}, {9, 42, false}, {0, 24, false}};
    for (const FRow& Row : Rows)
    {
        FMtoUFullCharacterFixtures Fixtures;
        if (!MakeFullCharacterFixtures(*Package, *this, Fixtures, Row.Pokes))
        {
            AddError(TEXT("reduced-topology fixture creation failed"));
            continue;
        }
        TestEqual(TEXT("Preview density is deterministic"), Fixtures.PreviewTriangleCount, Row.Triangles);
        Binding->SkeletalMesh = Fixtures.FullDriver;
        Binding->PreviewStaticMesh = Fixtures.Preview;
        Binding->DriverGarmentSlotOverride.Reset();
        FEnsureScope InpaintEnsureScope;
        const FMtoUPreviewPreparationResult Auto = MtoUPreparePreview(*Actor, *Binding);
        AddInfo(Auto.Diagnostics);
        TestEqual(TEXT("Auto respects the measured reduction boundary"), Auto.bSucceeded, Row.bAutoAccepted);
        if (Row.bAutoAccepted)
        {
            TestTrue(TEXT("Auto selects only the two garment regions and their slots"),
                Auto.GeneratedPreview && Auto.Quality != EMtoUPreviewQuality::Error
                && Auto.GarmentSourceRegionCount == 2 && Auto.GarmentSourceTriangleCount == 72
                && Auto.DriverGarmentMaterialSlotIndices == TArray<int32>({3, 4, 5}));
        }
        else
        {
            TestTrue(TEXT("the reduction limit fails transactionally in the source-mass category"),
                !Auto.GeneratedPreview && Auto.Quality == EMtoUPreviewQuality::Error
                && Auto.FailureStage == EMtoUPreviewBuildStage::GeometryConversion
                && Auto.Diagnostics.Contains(TEXT("source-mass boundary"))
                && Auto.Diagnostics.Contains(TEXT("legitimate same-surface reduction"))
                && Auto.Diagnostics.Contains(TEXT("triangle count alone does not prove"))
                && Auto.Diagnostics.Contains(TEXT("Driver Garment Slot Override")));
        }
        const FMtoUPreviewReadiness AutoRefresh = FMtoUPreviewPreparation::RefreshActor(*Actor);
        TestEqual(TEXT("public readiness agrees with the Auto operation"),
            AutoRefresh.IsUsable(), Row.bAutoAccepted);
        if (!Row.bAutoAccepted)
        {
            TestTrue(TEXT("rejected reduction restores Driver with no stale preview"),
                AutoRefresh.State == EMtoUPreviewState::Error && !AutoRefresh.GeneratedPreview
                && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver);
        }

        Binding->DriverGarmentSlotOverride = GarmentSlots;
        const FMtoUPreviewPreparationResult Manual = MtoUPreparePreview(*Actor, *Binding);
        AddInfo(Manual.Diagnostics);
        TestTrue(TEXT("manual control recovers exactly the garment surface at every density"),
            Manual.bSucceeded && Manual.GeneratedPreview && Manual.bManualGarmentSource
            && Manual.Quality != EMtoUPreviewQuality::Error
            && Manual.GarmentSourceRegionCount == 2 && Manual.GarmentSourceTriangleCount == 72
            && Manual.DriverGarmentMaterialSlotIndices == TArray<int32>({3, 4, 5})
            && Manual.MatchedPreviewCoverage > 0.999
            && Manual.SurfaceDistanceMax < 1.e-6);
        TestTrue(TEXT("manual remedy restores usable public readiness"),
            FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());
        AddInfo(FString::Printf(
            TEXT("reduction corpus: Driver=216 garment=72 Preview=%d ratio=%.6f Auto=%s "
                "ManualQuality=%d regions=%d slots=3,4,5 coverage=%.6f distance_max=%.9f inpaint=%.6f"),
            Row.Triangles, 72.0 / Row.Triangles, Auto.bSucceeded ? TEXT("accepted") : TEXT("rejected"),
            static_cast<int32>(Manual.Quality), Manual.GarmentSourceRegionCount,
            Manual.MatchedPreviewCoverage, Manual.SurfaceDistanceMax, Manual.InpaintLowConfidenceRatio));
    }
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUCorpusMeasurementTest,
    "MtoULiveLink.Editor.Preview.QualityCorpus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCorpusMeasurementTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString OverrideDriverPath
        = FPlatformMisc::GetEnvironmentVariable(TEXT("MTOU_QUALITY_DRIVER"));
    const FString OverridePreviewPath
        = FPlatformMisc::GetEnvironmentVariable(TEXT("MTOU_QUALITY_PREVIEW"));
    // Optional comma-separated Driver Garment Slot Override so external
    // production measurements can exercise the manual resolution path without
    // code changes; an empty value keeps automatic resolution.
    const FString OverrideSlotsRaw
        = FPlatformMisc::GetEnvironmentVariable(TEXT("MTOU_QUALITY_SLOTS"));
    TArray<FName> OverrideSlots;
    if (!OverrideSlotsRaw.IsEmpty())
    {
        TArray<FString> SlotNames;
        OverrideSlotsRaw.ParseIntoArray(SlotNames, TEXT(","), true);
        for (const FString& SlotName : SlotNames)
        {
            OverrideSlots.Add(FName(*SlotName));
        }
    }
    if (!OverrideDriverPath.IsEmpty() || !OverridePreviewPath.IsEmpty())
    {
        // Private production projects reference optional host plugins that
        // headless acceptance hosts may not mount; those loader errors are
        // unrelated to the measured assets and must not fail these rows.
        // Negative occurrence count suppresses every matching loader error.
        AddExpectedError(TEXT("Unknown structure"),
            EAutomationExpectedErrorFlags::Contains, -1);
    }
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUQualityCorpusWorld"));
    USkeletalMesh* Driver = OverrideDriverPath.IsEmpty()
        ? MakeMorphDriver(*WorldPackage)
        : LoadObject<USkeletalMesh>(nullptr, *OverrideDriverPath);
    UStaticMesh* BasePreview = OverridePreviewPath.IsEmpty()
        ? nullptr
        : LoadObject<UStaticMesh>(nullptr, *OverridePreviewPath);
    const FString CorpusName = OverrideDriverPath.IsEmpty()
        ? TEXT("stock-SkeletalCube")
        : FPaths::GetBaseFilename(OverrideDriverPath);
    if (!Driver || (!BasePreview && !OverridePreviewPath.IsEmpty()))
    {
        AddError(TEXT("corpus fixture assets were not loaded"));
        return false;
    }
    TStrongObjectPtr<USkeletalMesh> DriverGuard(Driver);
    TStrongObjectPtr<UStaticMesh> BasePreviewGuard(BasePreview);

    struct FMtoUCorpusCase
    {
        const TCHAR* Name;
        EMtoUCorpusVariant Variant;
        bool bExpectFailure;
    };
    const FMtoUCorpusCase Cases[] = {
        {TEXT("SameTopology"), EMtoUCorpusVariant::SameTopology, false},
        {TEXT("LocalRetopology"), EMtoUCorpusVariant::LocalRetopology, false},
        // The production BasePreview's deliberately inset duplicate layer is
        // outside whole-garment coverage; the stock synthetic shell remains a
        // supported layered warning fixture.
        {TEXT("DoubleLayerSeams"), EMtoUCorpusVariant::DoubleLayerSeams,
            BasePreview != nullptr},
        {TEXT("MisalignedNegative"), EMtoUCorpusVariant::Misaligned, true},
    };

    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUQualityCorpusWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    if (!Actor)
    {
        AddError(TEXT("corpus world was not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }
    Binding->SkeletalMesh = Driver;
    Binding->DriverGarmentSlotOverride = OverrideSlots;

    for (const FMtoUCorpusCase& Case : Cases)
    {
        UStaticMesh* VariantPreview = MakeCorpusPreview(
            *Driver, BasePreview, *WorldPackage, Case.Variant);
        if (!VariantPreview)
        {
            AddError(FString::Printf(TEXT("corpus variant %s was not created"), Case.Name));
            continue;
        }
        Binding->PreviewStaticMesh = VariantPreview;
        // The public inpaint QP solve fails with a handled ensure on large
        // layered inputs; Refresh falls back to closest-point weights.
        FEnsureScope InpaintEnsureScope;
        const double StartSeconds = FPlatformTime::Seconds();
        const FMtoUPreviewPreparationResult Result =
            MtoUPreparePreview(*Actor, *Binding);
        const double TotalMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
        const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();

        AddInfo(FString::Printf(
            TEXT("{\"corpus\":\"%s\",\"case\":\"%s\",\"vertices\":%d,\"triangles\":%d,"
                "\"low_confidence\":%d,\"inpaint_ratio\":%.5f,"
                "\"distance_min\":%.6f,\"distance_max\":%.6f,\"distance_avg\":%.6f,\"distance_rms\":%.6f,"
                "\"garment_source_regions\":%d,\"garment_source_triangles\":%d,"
                "\"matched_preview_coverage\":%.4f,\"manual_source\":%s,"
                "\"morph_count\":%d,\"skipped_morphs\":%d,\"sparse_deltas\":%lld,"
                "\"closest_ms\":%.3f,\"inpaint_ms\":%.3f,\"morph_projection_ms\":%.3f,"
                "\"total_refresh_ms\":%.1f,\"peak_physical_mib\":%.1f,\"status\":\"%s\"}"),
            *CorpusName,
            Case.Name,
            Result.VertexCount,
            Result.TriangleCount,
            Result.LowConfidenceVertexCount,
            Result.InpaintLowConfidenceRatio,
            Result.SurfaceDistanceMin,
            Result.SurfaceDistanceMax,
            Result.SurfaceDistanceAverage,
            Result.SurfaceDistanceRms,
            Result.GarmentSourceRegionCount,
            Result.GarmentSourceTriangleCount,
            Result.MatchedPreviewCoverage,
            Result.bManualGarmentSource ? TEXT("true") : TEXT("false"),
            Result.MorphTargetCount,
            Result.SkippedMorphTargetCount,
            Result.SparseMorphDeltaCount,
            Result.ClosestTransferMilliseconds,
            Result.InpaintTransferMilliseconds,
            Result.MorphProjectionMilliseconds,
            TotalMilliseconds,
            Stats.PeakUsedPhysical / (1024.0 * 1024.0),
            Result.bSucceeded
                ? (Result.Quality == EMtoUPreviewQuality::Ready ? TEXT("Ready") : TEXT("Warning"))
                : TEXT("Error")));
        AddInfo(Result.Diagnostics);

        if (OverrideSlots.Num() > 0)
        {
            // External measurement rows collect evidence only: the manual
            // override paths are asserted by the focused GarmentOverride
            // automation, so this harness stays non-fatal per case.
            TestTrue(FString::Printf(TEXT("%s produced a usable preview or an actionable diagnostic"), Case.Name),
                (Result.bSucceeded && Result.GeneratedPreview != nullptr)
                || (!Result.bSucceeded && !Result.Diagnostics.IsEmpty()));
            continue;
        }
        if (Case.bExpectFailure)
        {
            TestTrue(FString::Printf(TEXT("%s fails rather than producing a misleading preview"), Case.Name),
                !Result.bSucceeded
                && Result.GeneratedPreview == nullptr
                && Result.FailureStage == EMtoUPreviewBuildStage::GeometryConversion
                && Result.Diagnostics.Contains(TEXT("misaligned")));
        }
        else
        {
            TestTrue(FString::Printf(TEXT("%s builds within the calibrated envelope"), Case.Name),
                Result.bSucceeded
                && Result.GeneratedPreview != nullptr
                && Result.Quality != EMtoUPreviewQuality::Error
                && Result.SurfaceDistanceAverage
                    <= FMtoUPreviewQualityThresholds().MisalignedNormalizedDistanceAverage);
        }
    }

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoULiveLinkFactoriesTest,
    "MtoULiveLink.Editor.Factories",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoULiveLinkFactoriesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UMtoULiveLinkBindingFactory* BindingFactory = NewObject<UMtoULiveLinkBindingFactory>();
    UMtoULiveLinkBinding* Binding = Cast<UMtoULiveLinkBinding>(BindingFactory->FactoryCreateNew(
        UMtoULiveLinkBinding::StaticClass(), GetTransientPackage(), NAME_None,
        RF_Transient, nullptr, GWarn));
    TestNotNull(TEXT("binding factory creates its supported asset"), Binding);
    TestTrue(TEXT("new binding participates in editor transactions"),
        Binding && Binding->HasAnyFlags(RF_Transactional));

    UPlacementSubsystem* PlacementSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UPlacementSubsystem>()
        : nullptr;
    TestNotNull(TEXT("placement subsystem is available"), PlacementSubsystem);
    UMtoULiveLinkActorFactory* ActorFactory = PlacementSubsystem
        ? Cast<UMtoULiveLinkActorFactory>(PlacementSubsystem
            ->GetAssetFactoryFromFactoryClass(UMtoULiveLinkActorFactory::StaticClass())
            .GetObject())
        : nullptr;
    TestNotNull(TEXT("actor factory is registered with the placement subsystem"),
        ActorFactory);
    if (!ActorFactory)
    {
        return false;
    }
    FText Error;
    TestFalse(TEXT("an empty binding cannot be placed"),
        Binding && ActorFactory->CanCreateActorFrom(FAssetData(Binding), Error));
    TestEqual(TEXT("empty binding gives the actionable placement error"), Error.ToString(),
        FString(TEXT("Select a Skeletal Mesh on the binding before placing it.")));

    if (!Binding)
    {
        return false;
    }
    USkeletalMesh* TemplateMesh = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    TestNotNull(TEXT("stock skeletal mesh template is available"), TemplateMesh);
    if (!TemplateMesh)
    {
        return false;
    }
    Binding->SkeletalMesh = NewObject<USkeletalMesh>(GetTransientPackage(),
        USkeletalMesh::StaticClass(), NAME_None, RF_Transient, TemplateMesh);
    TestTrue(TEXT("placement fixture is transient"),
        Binding->SkeletalMesh->HasAnyFlags(RF_Transient));
    TestTrue(TEXT("a binding with a skeletal mesh can be placed"),
        ActorFactory->CanCreateActorFrom(FAssetData(Binding), Error));

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    TestNotNull(TEXT("editor world is created"), World);
    const FTransform DropTransform(FRotator(10.0, 20.0, 30.0), FVector(100.0, 200.0, 300.0));
    AddExpectedError(TEXT("has no render data"),
        EAutomationExpectedErrorFlags::Contains, 3);
    AMtoULiveLinkActor* Actor = World
        ? Cast<AMtoULiveLinkActor>(ActorFactory->CreateActor(
            Binding, World->GetCurrentLevel(), DropTransform))
        : nullptr;
    TestNotNull(TEXT("configured binding creates the binding actor"), Actor);
    if (Actor)
    {
        TestTrue(TEXT("spawned actor retains the binding"), Actor->GetBinding() == Binding);
        TestTrue(TEXT("actor factory preserves the drop transform"),
            Actor->GetActorTransform().Equals(DropTransform));
        TestTrue(TEXT("actor factory resolves the asset from the actor"),
            ActorFactory->GetAssetFromActorInstance(Actor) == Binding);

        USkeletalMesh* ReplacementMesh = NewObject<USkeletalMesh>(GetTransientPackage(),
            USkeletalMesh::StaticClass(), NAME_None, RF_Transient, TemplateMesh);
        FProperty* SkeletalMeshProperty = FindFProperty<FProperty>(
            UMtoULiveLinkBinding::StaticClass(),
            GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, SkeletalMesh));
        Binding->PreEditChange(SkeletalMeshProperty);
        Binding->SkeletalMesh = ReplacementMesh;
        FPropertyChangedEvent ChangedEvent(SkeletalMeshProperty);
        Binding->PostEditChangeProperty(ChangedEvent);
        TestTrue(TEXT("placed actor follows skeletal mesh changes on its binding"),
            Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == ReplacementMesh);
    }
    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewInvalidGeometryTest,
    "MtoULiveLink.Editor.Preview.InvalidGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewInvalidGeometryTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    USkeletalMesh* Driver = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUInvalidGeometryWorld"));
    UStaticMesh* ValidPreview = Driver ? MakePreview(*Driver, *WorldPackage) : nullptr;
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUInvalidGeometryWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    FProperty* PreviewInputProperty = FindFProperty<FProperty>(
        UMtoULiveLinkBinding::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, PreviewStaticMesh));
    FPropertyChangedEvent PreviewInputChanged(PreviewInputProperty);
    if (!Driver || !ValidPreview || !Actor)
    {
        AddError(TEXT("invalid-geometry fixtures were not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = ValidPreview;
    Actor->SetBinding(Binding);
    TestTrue(TEXT("valid geometry prepares and readies the preview"),
        MtoUPreparePreview(*Actor, *Binding).bSucceeded
            && FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());

    // NaN Preview coordinates fail at geometry admission, before any weight
    // transfer, Morph projection, or Generated Preview allocation.
    UStaticMesh* NaNPreview = MakePreview(*Driver, *WorldPackage);
    TStrongObjectPtr<UStaticMesh> NaNPreviewGuard(NaNPreview);
    TestNotNull(TEXT("NaN Preview fixture is created"), NaNPreview);
    if (NaNPreview)
    {
        FMeshDescription* Description = NaNPreview->GetMeshDescription(0);
        TVertexAttributesRef<FVector3f> Positions = Description->GetVertexPositions();
        for (const FVertexID VertexID : Description->Vertices().GetElementIDs())
        {
            Positions[VertexID] = FVector3f(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
            break;
        }
        NaNPreview->CommitMeshDescription(0);
    }
    Binding->PreviewStaticMesh = NaNPreview;
    const FMtoUPreviewPreparationResult NaNPreparation = MtoUPreparePreview(*Actor, *Binding);
    AddInfo(NaNPreparation.Diagnostics);
    TestFalse(TEXT("a NaN Preview fails preparation"), NaNPreparation.bSucceeded);
    TestNull(TEXT("a NaN Preview returns no Generated Preview"), NaNPreparation.GeneratedPreview);
    TestEqual(TEXT("a NaN Preview is rejected at geometry conversion"),
        NaNPreparation.FailureStage, EMtoUPreviewBuildStage::GeometryConversion);
    TestTrue(TEXT("the NaN diagnostic names non-finite geometry"),
        NaNPreparation.Diagnostics.Contains(TEXT("non-finite")));

    Binding->PostEditChangeProperty(PreviewInputChanged);
    const FMtoUPreviewReadiness NaNRefresh =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("a NaN Refresh discards stale preview and restores the Driver"),
        !NaNRefresh.IsUsable()
        && NaNRefresh.GeneratedPreview == nullptr
        && NaNRefresh.State == EMtoUPreviewState::Error
        && NaNRefresh.Stage == EMtoUPreviewBuildStage::GeometryConversion
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
        && !Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);

    Binding->PreviewStaticMesh = ValidPreview;
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestTrue(TEXT("Refresh recovers after non-finite geometry rejection"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());

    // NaN Driver coordinates fail the same shared admission gate.
    USkeletalMesh* NaNDriver = NewObject<USkeletalMesh>(
        WorldPackage, NAME_None, RF_Transient);
    TStrongObjectPtr<USkeletalMesh> NaNDriverGuard(NaNDriver);
    NaNDriver->SetSkeleton(Driver->GetSkeleton());
    NaNDriver->SetRefSkeleton(Driver->GetRefSkeleton());
    NaNDriver->CalculateInvRefMatrices();
    FMeshDescription IndependentDescription;
    if (Driver->CloneMeshDescription(0, IndependentDescription))
    {
        NaNDriver->SetNumSourceModels(1);
        NaNDriver->CreateMeshDescription(0, MoveTemp(IndependentDescription));
    }
    FMeshDescription* NaNDriverDescription = NaNDriver->GetMeshDescription(0);
    TestNotNull(TEXT("NaN Driver fixture has LOD0 source data"), NaNDriverDescription);
    if (NaNDriverDescription)
    {
        TVertexAttributesRef<FVector3f> Positions =
            NaNDriverDescription->GetVertexPositions();
        for (const FVertexID VertexID : NaNDriverDescription->Vertices().GetElementIDs())
        {
            Positions[VertexID] = FVector3f(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
            break;
        }
    }
    Binding->SkeletalMesh = NaNDriver;
    Binding->PreviewStaticMesh = ValidPreview;
    const FMtoUPreviewPreparationResult NaNDriverPreparation =
        MtoUPreparePreview(*Actor, *Binding);
    AddInfo(NaNDriverPreparation.Diagnostics);
    TestFalse(TEXT("a NaN Driver fails preparation"), NaNDriverPreparation.bSucceeded);
    TestNull(TEXT("a NaN Driver returns no Generated Preview"),
        NaNDriverPreparation.GeneratedPreview);
    TestEqual(TEXT("a NaN Driver is rejected at geometry conversion"),
        NaNDriverPreparation.FailureStage, EMtoUPreviewBuildStage::GeometryConversion);
    TestTrue(TEXT("the NaN Driver diagnostic names non-finite geometry"),
        NaNDriverPreparation.Diagnostics.Contains(TEXT("non-finite")));
    Binding->SkeletalMesh = Driver;

    // A valid source mesh whose LOD0 has vertices but no triangles fails
    // cleanly instead of entering mass accounting or reporting Ready.
    UStaticMesh* ZeroTrianglePreview = NewObject<UStaticMesh>(
        WorldPackage, NAME_None, RF_Transient);
    TStrongObjectPtr<UStaticMesh> ZeroTriangleGuard(ZeroTrianglePreview);
    FMeshDescription ZeroDescription;
    FStaticMeshAttributes ZeroAttributes(ZeroDescription);
    ZeroAttributes.Register();
    TVertexAttributesRef<FVector3f> ZeroPositions = ZeroAttributes.GetVertexPositions();
    for (const FVector3f Position : {
            FVector3f(0.0f, 0.0f, 0.0f), FVector3f(1.0f, 0.0f, 0.0f),
            FVector3f(0.0f, 1.0f, 0.0f), FVector3f(1.0f, 1.0f, 0.0f) })
    {
        ZeroPositions[ZeroDescription.CreateVertex()] = Position;
    }
    ZeroTrianglePreview->SetNumSourceModels(1);
    ZeroTrianglePreview->CreateMeshDescription(0, MoveTemp(ZeroDescription));
    ZeroTrianglePreview->CommitMeshDescription(0);
    TestTrue(TEXT("zero-triangle Preview exposes LOD0 source data"),
        ZeroTrianglePreview->IsSourceModelValid(0)
            && ZeroTrianglePreview->IsMeshDescriptionValid(0));
    Binding->PreviewStaticMesh = ZeroTrianglePreview;
    const FMtoUPreviewPreparationResult ZeroTrianglePreparation =
        MtoUPreparePreview(*Actor, *Binding);
    AddInfo(ZeroTrianglePreparation.Diagnostics);
    TestFalse(TEXT("a zero-triangle Preview fails preparation"),
        ZeroTrianglePreparation.bSucceeded);
    TestNull(TEXT("a zero-triangle Preview returns no Generated Preview"),
        ZeroTrianglePreparation.GeneratedPreview);
    TestEqual(TEXT("a zero-triangle Preview is rejected at geometry conversion"),
        ZeroTrianglePreparation.FailureStage, EMtoUPreviewBuildStage::GeometryConversion);
    TestTrue(TEXT("the zero-triangle diagnostic names the missing triangles"),
        ZeroTrianglePreparation.Diagnostics.Contains(TEXT("no LOD0 triangles")));

    Binding->PreviewStaticMesh = ValidPreview;
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestTrue(TEXT("Refresh recovers after the zero-triangle rejection"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());

    // A collinear LOD0 triangle keeps a nonzero bounding box, finite
    // coordinates, and a positive triangle count, but spans no surface; the
    // shared admission gate must reject it transactionally instead of letting
    // a degenerate input reach coverage or mass accounting (#32).
    UDynamicMesh* CollinearSource = NewObject<UDynamicMesh>(WorldPackage);
    CollinearSource->EditMesh([](UE::Geometry::FDynamicMesh3& Mesh)
    {
        const int32 A = Mesh.AppendVertex(FVector3d(0.0, 0.0, 0.0));
        const int32 B = Mesh.AppendVertex(FVector3d(1.0, 0.0, 0.0));
        const int32 C = Mesh.AppendVertex(FVector3d(2.0, 0.0, 0.0));
        Mesh.AppendTriangle(A, B, C);
    });
    UStaticMesh* CollinearPreview = NewObject<UStaticMesh>(
        WorldPackage, NAME_None, RF_Transient);
    TStrongObjectPtr<UStaticMesh> CollinearPreviewGuard(CollinearPreview);
    FGeometryScriptCopyMeshToAssetOptions CollinearWriteOptions;
    CollinearWriteOptions.bEmitTransaction = false;
    CollinearWriteOptions.bReplaceMaterials = true;
    CollinearWriteOptions.NewMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
    CollinearWriteOptions.NewMaterialSlotNames.Add(FName(TEXT("Collinear_Preview")));
    FGeometryScriptMeshWriteLOD CollinearWriteLOD;
    EGeometryScriptOutcomePins CollinearOutcome = EGeometryScriptOutcomePins::Failure;
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
        CollinearSource, CollinearPreview, CollinearWriteOptions,
        CollinearWriteLOD, CollinearOutcome, false);
    TestTrue(TEXT("collinear Preview fixture is written with LOD0 source data"),
        CollinearOutcome == EGeometryScriptOutcomePins::Success
            && CollinearPreview->IsSourceModelValid(0)
            && CollinearPreview->IsMeshDescriptionValid(0));
    Binding->PreviewStaticMesh = CollinearPreview;
    const FMtoUPreviewPreparationResult CollinearPreparation =
        MtoUPreparePreview(*Actor, *Binding);
    AddInfo(CollinearPreparation.Diagnostics);
    TestFalse(TEXT("a collinear Preview fails preparation"),
        CollinearPreparation.bSucceeded);
    TestNull(TEXT("a collinear Preview returns no Generated Preview"),
        CollinearPreparation.GeneratedPreview);
    TestEqual(TEXT("a collinear Preview is rejected at geometry conversion"),
        CollinearPreparation.FailureStage, EMtoUPreviewBuildStage::GeometryConversion);
    TestTrue(TEXT("the collinear diagnostic names the missing triangle area"),
        CollinearPreparation.Diagnostics.Contains(TEXT("no usable triangle area")));

    Binding->PostEditChangeProperty(PreviewInputChanged);
    const FMtoUPreviewReadiness CollinearRefresh =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("a collinear Refresh discards stale preview and restores the Driver"),
        !CollinearRefresh.IsUsable()
        && CollinearRefresh.GeneratedPreview == nullptr
        && CollinearRefresh.State == EMtoUPreviewState::Error
        && CollinearRefresh.Stage == EMtoUPreviewBuildStage::GeometryConversion
        && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
        && !Actor->GetPreviewReadiness().IsUsable()
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);

    Binding->PreviewStaticMesh = ValidPreview;
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestTrue(TEXT("Refresh recovers after the collinear rejection"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).IsUsable());

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewRefreshBenchmarkTest,
    "MtoULiveLink.Editor.Preview.RefreshBenchmark",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewRefreshBenchmarkTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUPreviewBenchmarkWorld"));
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUPreviewBenchmarkWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    if (!Actor || !Binding)
    {
        AddError(TEXT("benchmark world was not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }

    // A selector allows each case's first refresh to be measured in a fresh
    // Editor process. Without it, only the first case is process-cold.
    FString SelectedCase;
    FParse::Value(FCommandLine::Get(), TEXT("MtoUBenchmarkCase="), SelectedCase);
    struct FCase
    {
        const TCHAR* Name;
        int32 GarmentCells;
        int32 OtherCells;
        int32 Morphs;
    };
    const FCase Cases[] = {
        {TEXT("Small"), 0, 0, 5},
        {TEXT("Representative"), 0, 0, 4},
        {TEXT("Library64"), 0, 0, 64},
        {TEXT("GeometryMedium4"), 10, 15, 4},
        {TEXT("GeometryMedium32"), 10, 15, 32},
        {TEXT("GeometryMedium64"), 10, 15, 64},
        {TEXT("GeometryLarge4"), 34, 50, 4},
        {TEXT("GeometryLarge32"), 34, 50, 32},
        {TEXT("GeometryLarge64"), 34, 50, 64},
    };
    bool bFirstRefresh = true;
    bool bMatchedCase = false;
    for (const FCase& Case : Cases)
    {
        if (!SelectedCase.IsEmpty() && SelectedCase != Case.Name)
        {
            continue;
        }
        bMatchedCase = true;
        const bool bSmall = FString(Case.Name) == TEXT("Small");
        FMtoUFullCharacterFixtures Fixtures;
        if (bSmall)
        {
            Fixtures.FullDriver = MakeMorphDriver(*WorldPackage);
            Fixtures.Preview = Fixtures.FullDriver
                ? MakePreview(*Fixtures.FullDriver, *WorldPackage) : nullptr;
        }
        else if (!MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures,
            -1, Case.GarmentCells, Case.OtherCells))
        {
            AddError(FString::Printf(TEXT("%s fixture creation failed"), Case.Name));
            continue;
        }
        USkeletalMesh* Driver = Fixtures.FullDriver;
        UStaticMesh* Preview = Fixtures.Preview;
        TStrongObjectPtr<USkeletalMesh> DriverGuard(Driver);
        TStrongObjectPtr<USkeletalMesh> GarmentGuard(Fixtures.GarmentOnlyDriver);
        TStrongObjectPtr<UStaticMesh> PreviewGuard(Preview);
        if (!Driver || !Preview)
        {
            AddError(TEXT("benchmark input assets are missing"));
            continue;
        }
        for (int32 Index = 0; Index < Case.Morphs - (bSmall ? 5 : 4); ++Index)
        {
            TestTrue(TEXT("benchmark uniform Morph is registered"), AddUniformMorph(*Driver,
                FName(*FString::Printf(TEXT("BenchMorph%02d"), Index)),
                FVector3f(0.1f * (Index + 1), 0.05f * (Index + 1), 0.02f * (Index + 1))));
        }
        Driver->InitMorphTargets();
        TestEqual(TEXT("input library size"), Driver->GetMorphTargets().Num(), Case.Morphs);
        // Fix the selected garment independently of dense proximity sampling,
        // matching the explicit garment-slot workflow in the historical C01 run.
        Binding->DriverGarmentSlotOverride.Reset();
        if (Case.GarmentCells > 0)
        {
            Binding->DriverGarmentSlotOverride = {FName(TEXT("Garment_Upper_A")),
                FName(TEXT("Garment_Upper_B")), FName(TEXT("Garment_Lower"))};
        }
        Binding->SkeletalMesh = Driver;
        Binding->PreviewStaticMesh = Preview;
        Actor->SetBinding(nullptr);
        Actor->SetBinding(Binding);
        const FMeshDescription* Input = Preview->GetMeshDescription(0);
        const FMeshDescription* DriverInput = Driver->GetMeshDescription(0);
        if (!Input || !DriverInput)
        {
            AddError(TEXT("benchmark inputs have no LOD0 descriptions"));
            continue;
        }
        if (Case.GarmentCells > 0)
        {
            TestEqual(TEXT("declared garment geometry scale"), Input->Triangles().Num(),
                24 * Case.GarmentCells * Case.GarmentCells);
            TestEqual(TEXT("declared full-character geometry scale"), DriverInput->Triangles().Num(),
                24 * Case.GarmentCells * Case.GarmentCells + 48 * Case.OtherCells * Case.OtherCells);
        }
        for (int32 Run = 0; Run < 4; ++Run)
        {
            // Release abandoned scratch/output from previous runs before sampling.
            // The actor still retains the previous committed output for warmed runs.
            CollectGarbage(RF_NoFlags);
            const FPlatformMemoryStats Before = FPlatformMemory::GetStats();
            double Stages[6] = {};
            EMtoUPreviewBuildStage PreviousStage = EMtoUPreviewBuildStage::None;
            const double Start = FPlatformTime::Seconds();
            double StageStart = Start;
            const FMtoUPreviewReadiness Result = FMtoUPreviewPreparation::RefreshActor(*Actor,
                [&](EMtoUPreviewBuildStage Stage)
                {
                    const double Now = FPlatformTime::Seconds();
                    Stages[static_cast<int32>(PreviousStage)] += (Now - StageStart) * 1000.0;
                    PreviousStage = Stage;
                    StageStart = Now;
                });
            const double End = FPlatformTime::Seconds();
            Stages[static_cast<int32>(PreviousStage)] += (End - StageStart) * 1000.0;
            const FPlatformMemoryStats After = FPlatformMemory::GetStats();
            AddInfo(FString::Printf(TEXT("%s run %d: %s"), Case.Name, Run, *Result.Diagnostics));
            USkeletalMesh* Generated = Result.GeneratedPreview;
            bool bOutputOK = Result.IsUsable() && Generated
                && Generated->HasAnyFlags(RF_Transient) && Generated->GetOuter() == Actor
                && Actor->GetPreviewReadiness().GeneratedPreview == Generated
                && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
                && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Generated;
            const FMeshDescription* Output = Generated ? Generated->GetMeshDescription(0) : nullptr;
            bOutputOK &= Output && Output->Vertices().Num() == Input->Vertices().Num()
                && Output->Triangles().Num() == Input->Triangles().Num();
            if (Output)
            {
                // Compare the complete position multiset, independent of vertex reindexing.
                TArray<FVector3f> ExpectedPositions, ActualPositions;
                for (const FVertexID ID : Input->Vertices().GetElementIDs())
                {
                    ExpectedPositions.Add(Input->GetVertexPositions()[ID]);
                }
                for (const FVertexID ID : Output->Vertices().GetElementIDs())
                {
                    ActualPositions.Add(Output->GetVertexPositions()[ID]);
                }
                const auto PositionLess = [](const FVector3f& A, const FVector3f& B)
                {
                    return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z);
                };
                ExpectedPositions.Sort(PositionLess);
                ActualPositions.Sort(PositionLess);
                for (int32 Index = 0; Index < FMath::Min(ExpectedPositions.Num(), ActualPositions.Num()); ++Index)
                {
                    bOutputOK &= ExpectedPositions[Index].Equals(ActualPositions[Index], 0.001f);
                }
            }
            UDynamicMesh* Readback = NewObject<UDynamicMesh>(GetTransientPackage());
            FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
            FGeometryScriptMeshReadLOD ReadLOD;
            ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
            EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
            if (Generated)
            {
                UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
                    Generated, Readback, ReadOptions, ReadLOD, Outcome);
            }
            bool bWeightsOK = Outcome == EGeometryScriptOutcomePins::Success;
            const FDynamicMesh3& Mesh = Readback->GetMeshRef();
            const auto* Weights = Mesh.HasAttributes() ? Mesh.Attributes()->GetSkinWeightsAttribute(
                FSkeletalMeshAttributes::DefaultSkinWeightProfileName) : nullptr;
            bWeightsOK &= Weights != nullptr;
            if (Weights)
            {
                for (const int32 VertexID : Mesh.VertexIndicesItr())
                {
                    UE::AnimationCore::FBoneWeights Influences;
                    Weights->GetValue(VertexID, Influences);
                    float Sum = 0.0f;
                    bWeightsOK &= Influences.Num() > 0;
                    for (const auto& Influence : Influences)
                    {
                        Sum += Influence.GetWeight();
                        bWeightsOK &= Generated->GetRefSkeleton().IsValidIndex(Influence.GetBoneIndex());
                        if (!bSmall)
                        {
                            bWeightsOK &= Influence.GetBoneIndex() == 0;
                        }
                    }
                    bWeightsOK &= FMath::IsNearlyEqual(Sum, 1.0f, 0.001f);
                }
            }
            int64 SparseDeltas = 0;
            int32 Projected = 0;
            bool bMorphsOK = Generated != nullptr;
            const TSet<FName> Skipped = {FName(TEXT("ArmRaise")), FName(TEXT("FaceBlink")), FName(TEXT("HairSway"))};
            if (Generated)
            {
                Projected = Generated->GetMorphTargets().Num();
                bMorphsOK &= Projected == Case.Morphs - (bSmall ? 0 : 3);
                for (const UMorphTarget* SourceMorph : Driver->GetMorphTargets())
                {
                    const FName Name = SourceMorph->GetFName();
                    const UMorphTarget* Morph = Generated->FindMorphTarget(Name);
                    if (!bSmall && Skipped.Contains(Name))
                    {
                        bMorphsOK &= Morph == nullptr;
                        continue;
                    }
                    if (!Morph || !Morph->HasDataForLOD(0))
                    {
                        bMorphsOK = false;
                        continue;
                    }
                    const FMorphTargetLODModel& LOD = Morph->GetMorphLODModels()[0];
                    const FVector3f ExpectedDelta = SourceMorph->GetMorphLODModels()[0].Vertices[0].PositionDelta;
                    SparseDeltas += LOD.Vertices.Num();
                    bMorphsOK &= !LOD.Vertices.IsEmpty();
                    const FSkeletalMeshLODModel& GeneratedLOD = Generated->GetImportedModel()->LODModels[0];
                    TMap<uint32, FVector3f> ActualDeltas;
                    for (const FMorphTargetDelta& Delta : LOD.Vertices)
                    {
                        bMorphsOK &= Delta.SourceIdx < GeneratedLOD.NumVertices
                            && !ActualDeltas.Contains(Delta.SourceIdx)
                            && !Delta.PositionDelta.ContainsNaN() && !Delta.TangentZDelta.ContainsNaN();
                        ActualDeltas.Add(Delta.SourceIdx, Delta.PositionDelta);
                    }
                    // Same-topology inputs give an independent analytic oracle:
                    // uniform translations everywhere, or +2 X inside Flare's box.
                    // UE also emits normal-only deltas near that box boundary;
                    // those sparse entries legitimately have zero position delta.
                    const FAxisAlignedBox3d FlareBox(
                        Fixtures.GarmentABounds.Min - 2.0, Fixtures.GarmentABounds.Max + 2.0);
                    bool bDeltasOK = Output != nullptr;
                    for (uint32 Index = 0; Output && Index < GeneratedLOD.NumVertices; ++Index)
                    {
                        if (!GeneratedLOD.MeshToImportVertexMap.IsValidIndex(Index))
                        {
                            bDeltasOK = false;
                            continue;
                        }
                        const FVertexID PointID(GeneratedLOD.MeshToImportVertexMap[Index]);
                        if (!Output->Vertices().IsValid(PointID))
                        {
                            bDeltasOK = false;
                            continue;
                        }
                        const FVector3f Expected = Name == FName(TEXT("GarmentFlare"))
                            ? (FlareBox.Contains(FVector3d(Output->GetVertexPositions()[PointID]))
                                ? FVector3f(2.0f, 0.0f, 0.0f) : FVector3f::ZeroVector)
                            : ExpectedDelta;
                        const FVector3f* Actual = ActualDeltas.Find(Index);
                        bDeltasOK &= (Actual ? *Actual : FVector3f::ZeroVector).Equals(Expected, 0.001f);
                    }
                    bMorphsOK &= TestTrue(*FString::Printf(TEXT("%s complete expected position deltas"),
                        *Name.ToString()), bDeltasOK);
                }
            }
            TestTrue(FString::Printf(TEXT("%s run %d geometry, ownership and readiness"), Case.Name, Run), bOutputOK);
            TestTrue(FString::Printf(TEXT("%s run %d normalized skin weights"), Case.Name, Run), bWeightsOK);
            TestTrue(FString::Printf(TEXT("%s run %d Morph names, coverage and deltas"), Case.Name, Run), bMorphsOK);
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("benchmark"), TEXT("PreviewRefresh"));
            Row->SetStringField(TEXT("scale"), Case.Name);
            Row->SetStringField(TEXT("diagnostics"), Result.Diagnostics);
            Row->SetStringField(TEXT("phase"), Run == 0
                ? (bFirstRefresh ? TEXT("process_first_refresh") : TEXT("case_first_refresh")) : TEXT("warmed"));
            Row->SetStringField(TEXT("selection"), Case.GarmentCells > 0 ? TEXT("Manual") : TEXT("Auto"));
            Row->SetNumberField(TEXT("run"), Run);
            Row->SetNumberField(TEXT("garment_grid_cells"), Case.GarmentCells);
            Row->SetNumberField(TEXT("other_grid_cells"), Case.OtherCells);
            Row->SetNumberField(TEXT("driver_vertices"), DriverInput->Vertices().Num());
            Row->SetNumberField(TEXT("driver_triangles"), DriverInput->Triangles().Num());
            Row->SetNumberField(TEXT("vertices"), Input->Vertices().Num());
            Row->SetNumberField(TEXT("triangles"), Input->Triangles().Num());
            Row->SetNumberField(TEXT("output_vertices"), Output ? Output->Vertices().Num() : 0);
            Row->SetNumberField(TEXT("output_triangles"), Output ? Output->Triangles().Num() : 0);
            Row->SetNumberField(TEXT("driver_morphs"), Case.Morphs);
            Row->SetNumberField(TEXT("morph_count"), Projected);
            Row->SetNumberField(TEXT("skipped_morphs"), Case.Morphs - Projected);
            Row->SetNumberField(TEXT("sparse_deltas"), SparseDeltas);
            const TCHAR* StageNames[] = {TEXT("begin_ms"), TEXT("preflight_ms"), TEXT("geometry_resolution_ms"),
                TEXT("weight_transfer_ms"), TEXT("build_and_morph_ms"), TEXT("validation_commit_display_ms")};
            for (int32 Index = 0; Index < 6; ++Index)
            {
                Row->SetNumberField(StageNames[Index], Stages[Index]);
            }
            Row->SetNumberField(TEXT("total_refresh_ms"), (End - Start) * 1000.0);
            Row->SetNumberField(TEXT("before_used_physical_mib"), Before.UsedPhysical / (1024.0 * 1024.0));
            Row->SetNumberField(TEXT("after_used_physical_mib"), After.UsedPhysical / (1024.0 * 1024.0));
            Row->SetNumberField(TEXT("used_physical_change_mib"),
                (static_cast<double>(After.UsedPhysical) - Before.UsedPhysical) / (1024.0 * 1024.0));
            Row->SetNumberField(TEXT("before_process_peak_mib"), Before.PeakUsedPhysical / (1024.0 * 1024.0));
            Row->SetNumberField(TEXT("after_process_peak_mib"), After.PeakUsedPhysical / (1024.0 * 1024.0));
            Row->SetBoolField(TEXT("output_checks_passed"), bOutputOK && bWeightsOK && bMorphsOK);
            Row->SetStringField(TEXT("status"), Result.State == EMtoUPreviewState::Ready ? TEXT("Ready")
                : (Result.State == EMtoUPreviewState::Warning ? TEXT("Warning") : TEXT("Error")));
            FString Json;
            const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
            FJsonSerializer::Serialize(Row, Writer);
            AddInfo(Json);
            bFirstRefresh = false;
        }
        Actor->SetBinding(nullptr);
    }
    TestTrue(TEXT("benchmark selector matches a known fixture"), bMatchedCase);

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewRenamedSlotTest,
    "MtoULiveLink.Editor.Preview.RenamedSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewRenamedSlotTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUPreviewRenamedSlotWorld"));
    MtoUEditorTest::FMtoUFullCharacterFixtures Fixtures;
    TestTrue(TEXT("renamed preview fixtures are created"),
        MtoUEditorTest::MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures));
    if (!Fixtures.IsValid())
    {
        AddError(TEXT("renamed preview fixtures were not created"));
        return false;
    }
    // Production shape: source group _EyeShadow2 with one displayed match and
    // a different non-empty imported identity; all other groups stay 1-1.
    // Body lives at slot/group 0 in the deterministic fixture.
    const FName EyeShadowGroup(TEXT("_EyeShadow2"));
    const FName EyeShadowImported(TEXT("M_C01_EyeShadow1"));
    FMeshDescription* Description = Fixtures.FullDriver->GetMeshDescription(0);
    TestNotNull(TEXT("renamed preview Driver has LOD0 source data"), Description);
    if (!Description)
    {
        return false;
    }
    FStaticMeshAttributes DescAttributes(*Description);
    TPolygonGroupAttributesRef<FName> GroupNames = DescAttributes.GetPolygonGroupMaterialSlotNames();
    FPolygonGroupID BodyGroupID(0);
    bool bFoundBodyGroup = false;
    const FName BodyImported = Fixtures.FullDriver->GetMaterials()[0].ImportedMaterialSlotName != NAME_None
        ? Fixtures.FullDriver->GetMaterials()[0].ImportedMaterialSlotName
        : Fixtures.FullDriver->GetMaterials()[0].MaterialSlotName;
    for (const FPolygonGroupID GroupID : Description->PolygonGroups().GetElementIDs())
    {
        if (GroupNames[GroupID] == BodyImported)
        {
            BodyGroupID = GroupID;
            bFoundBodyGroup = true;
            break;
        }
    }
    TestTrue(TEXT("renamed preview finds the Body source group"), bFoundBodyGroup);
    if (!bFoundBodyGroup)
    {
        return false;
    }
    const FName OriginalGroup0 = GroupNames[BodyGroupID];
    GroupNames[BodyGroupID] = EyeShadowGroup;
    TArray<FSkeletalMaterial> OriginalMaterials = Fixtures.FullDriver->GetMaterials();
    Fixtures.FullDriver->GetMaterials()[0].MaterialSlotName = EyeShadowGroup;
    Fixtures.FullDriver->GetMaterials()[0].ImportedMaterialSlotName = EyeShadowImported;
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUPreviewRenamedSlotWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    Binding->SkeletalMesh = Fixtures.FullDriver;
    Binding->PreviewStaticMesh = Fixtures.Preview;
    if (!Actor)
    {
        AddError(TEXT("renamed preview world was not created"));
        return false;
    }
    Actor->SetBinding(Binding);
    const FMtoUPreviewPreparationResult Prepared = MtoUPreparePreview(*Actor, *Binding);
    AddInfo(Prepared.Diagnostics);
    TestTrue(TEXT("renamed production shape prepares a usable preview"),
        Prepared.bSucceeded && Prepared.GeneratedPreview != nullptr);
    TestTrue(TEXT("renamed preview keeps body/face/hair/garment composition"),
        Prepared.DriverGarmentMaterialSlotIndices == TArray<int32>({3, 4, 5})
            && Prepared.GarmentSourceRegionCount >= 2
            && Prepared.MatchedPreviewCoverage > 0.999);
    const FMtoUPreviewReadiness Ready = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("renamed Refresh completes with a usable Generated Preview"),
        Ready.IsUsable() && Ready.GeneratedPreview != nullptr
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Ready.GeneratedPreview);
    TWeakObjectPtr<USkeletalMesh> StalePreview = Ready.GeneratedPreview;
    UStaticMesh* MisalignedPreview = MakePreview(*Fixtures.FullDriver, *WorldPackage, false, false, false, true);
    TestNotNull(TEXT("misaligned failure preview is created"), MisalignedPreview);
    TStrongObjectPtr<UStaticMesh> MisalignedGuard(MisalignedPreview);
    UStaticMesh* AlignedPreview = Fixtures.Preview;
    TStrongObjectPtr<UStaticMesh> AlignedGuard(AlignedPreview);
    Binding->PreviewStaticMesh = MisalignedPreview;
    const FMtoUPreviewReadiness Failed = FMtoUPreviewPreparation::RefreshActor(*Actor);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("failed Refresh keeps Error, no usable preview, and Driver display"),
        !Failed.IsUsable()
            && Failed.GeneratedPreview == nullptr
            && Failed.State == EMtoUPreviewState::Error
            && !Actor->GetPreviewReadiness().IsUsable()
            && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Fixtures.FullDriver
            && !StalePreview.IsValid());
    TestTrue(TEXT("Binding Driver and Preview references remain intact after failure"),
        Binding->SkeletalMesh == Fixtures.FullDriver && Binding->PreviewStaticMesh == MisalignedPreview);
    TestTrue(TEXT("Model remains blocked without a usable preview"),
        !Actor->GetPreviewReadiness().IsUsable());
    Actor->ShowDriverMesh();
    TestTrue(TEXT("Animation connection continues to select the bound Driver"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Fixtures.FullDriver);
    // Correct the input and retry without recreating the actor.
    Binding->PreviewStaticMesh = AlignedPreview;
    const FMtoUPreviewReadiness Recovered = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("retry after correcting metadata recovers without a new actor"),
        Recovered.IsUsable()
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview);
    GroupNames[BodyGroupID] = OriginalGroup0;
    Fixtures.FullDriver->GetMaterials() = OriginalMaterials;
    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoURefreshEndsSessionTest,
    "MtoULiveLink.Editor.Preview.RefreshEndsSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoURefreshEndsSessionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    auto PollUntil = [](TFunctionRef<bool()> Predicate)
    {
        const double Deadline = FPlatformTime::Seconds() + 2.0;
        do
        {
            if (Predicate())
            {
                return true;
            }
            FPlatformProcess::Sleep(0.005f);
        }
        while (FPlatformTime::Seconds() < Deadline);
        return Predicate();
    };
    auto Utf8 = [](const FString& Text)
    {
        FTCHARToUTF8 Converted(*Text);
        TArray<uint8> Bytes;
        Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
        return Bytes;
    };
    auto FromUtf8 = [](const TArray<uint8>& Bytes)
    {
        FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
        return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
    };
    auto Prefix = [](uint64 Length)
    {
        TArray<uint8> Bytes;
        Bytes.SetNumUninitialized(8);
        for (int32 Index = 0; Index < 8; ++Index)
        {
            Bytes[Index] = static_cast<uint8>(Length >> ((7 - Index) * 8));
        }
        return Bytes;
    };
    auto PacketFor = [&](const FString& Text)
    {
        TArray<uint8> Payload = Utf8(Text);
        TArray<uint8> Bytes = Prefix(Payload.Num());
        Bytes.Append(Payload);
        return Bytes;
    };
    auto TransformJson = [](const FTransform& Transform)
    {
        const FVector Translation = Transform.GetTranslation();
        const FQuat Rotation = Transform.GetRotation();
        const FVector Scale = Transform.GetScale3D();
        return FString::Printf(
            TEXT("[%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g]"),
            Translation.X, Translation.Y, Translation.Z,
            Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
            Scale.X, Scale.Y, Scale.Z);
    };
    auto BonesJsonFor = [&](const FReferenceSkeleton& Skeleton)
    {
        FString Bones;
        for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
        {
            if (Index > 0)
            {
                Bones += TEXT(",");
            }
            Bones += FString::Printf(
                TEXT("[\"%s\",%d,%s]"),
                *Skeleton.GetBoneName(Index).ToString(),
                Skeleton.GetParentIndex(Index),
                *TransformJson(Skeleton.GetRefBonePose()[Index]));
        }
        return Bones;
    };
    auto FrameJsonFor = [&](const FReferenceSkeleton& Skeleton, const FVector& RootTranslation, const FString& CurvesJson)
    {
        FString Transforms;
        for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
        {
            if (Index > 0)
            {
                Transforms += TEXT(",");
            }
            FTransform Pose = Skeleton.GetRefBonePose()[Index];
            if (Index == 0)
            {
                Pose.SetTranslation(RootTranslation);
            }
            Transforms += TransformJson(Pose);
        }
        return FString::Printf(TEXT("{\"type\":\"frame\",\"transforms\":[%s],\"curves\":%s}"), *Transforms, *CurvesJson);
    };
    auto CacheFrameJsonFor = [&](const FReferenceSkeleton& Skeleton, int32 FrameIndex, const FVector& RootTranslation, const FString& CurvesJson)
    {
        FString Transforms;
        for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
        {
            if (Index > 0)
            {
                Transforms += TEXT(",");
            }
            FTransform Pose = Skeleton.GetRefBonePose()[Index];
            if (Index == 0)
            {
                Pose.SetTranslation(RootTranslation);
            }
            Transforms += TransformJson(Pose);
        }
        return FString::Printf(TEXT("{\"type\":\"cache_frame\",\"index\":%d,\"transforms\":[%s],\"curves\":%s}"), FrameIndex, *Transforms, *CurvesJson);
    };
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoURefreshEndsSessionWorld"));
    USkeletalMesh* Driver = MakeMorphDriver(*WorldPackage);
    UStaticMesh* Preview = Driver ? MakePreview(*Driver, *WorldPackage) : nullptr;
    TestNotNull(TEXT("refresh-session Driver with Morphs is created"), Driver);
    TestNotNull(TEXT("refresh-session Preview is created"), Preview);
    if (!Driver || !Preview)
    {
        return false;
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("MtoURefreshEndsSessionWorld"), WorldPackage, true);
    TestNotNull(TEXT("editor world is created"), World);
    if (!World)
    {
        return false;
    }
    if (GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = World->SpawnActor<AMtoULiveLinkActor>();
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = Preview;
    TestNotNull(TEXT("binding actor is created"), Actor);
    if (!Actor)
    {
        return false;
    }
    Actor->SetBinding(Binding);
    const FReferenceSkeleton* TestSkeleton = &Driver->GetRefSkeleton();
    const FString BonesJson = BonesJsonFor(*TestSkeleton);
    const FMtoUPreviewReadiness DisconnectedFirst = FMtoUPreviewPreparation::RefreshActor(*Actor);
    USkeletalMesh* FirstMesh = DisconnectedFirst.GeneratedPreview;
    TestTrue(TEXT("disconnected Refresh shows Generated Preview immediately"),
        DisconnectedFirst.IsUsable() && FirstMesh != nullptr
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == FirstMesh);
    TWeakObjectPtr<USkeletalMesh> ReplacedWeak = FirstMesh;
    const FMtoUPreviewReadiness DisconnectedSecond = FMtoUPreviewPreparation::RefreshActor(*Actor);
    USkeletalMesh* SecondMesh = DisconnectedSecond.GeneratedPreview;
    TestTrue(TEXT("repeated disconnected Refresh replaces the transient mesh"),
        DisconnectedSecond.IsUsable() && SecondMesh != nullptr && SecondMesh != FirstMesh
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("replacement releases the previous transient mesh"), ReplacedWeak.IsValid());
    auto Loopback = [&](uint16 Port)
    {
        TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
        bool bValid = false;
        Address->SetIp(TEXT("127.0.0.1"), bValid);
        check(bValid);
        Address->SetPort(Port);
        return Address;
    };
    auto DestroySocket = [&](FSocket*& Socket)
    {
        if (Socket)
        {
            Socket->Close();
            SocketSubsystem->DestroySocket(Socket);
            Socket = nullptr;
        }
    };
    auto SendBytes = [&](FSocket& Socket, const uint8* Data, int32 Num)
    {
        int32 Offset = 0;
        const double Deadline = FPlatformTime::Seconds() + 2.0;
        while (Offset < Num && FPlatformTime::Seconds() < Deadline)
        {
            int32 Sent = 0;
            if (Socket.Send(Data + Offset, Num - Offset, Sent) && Sent > 0)
            {
                Offset += Sent;
            }
            else
            {
                Socket.Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(5));
            }
        }
        return Offset == Num;
    };
    FSocket* Reservation = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MtoURefreshEndsSession reservation"));
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    Reservation->Bind(*Loopback(0));
    Reservation->Listen(1);
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(Reservation);
    TestTrue(TEXT("ephemeral listening port is reported"), Port > 0);
    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    const FLiveLinkSubjectKey LiveSubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));
    TestTrue(TEXT("source reaches listening state"),
        PollUntil([&]() { return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on")); }));
    auto ReceiveOne = [&](FSocket& Socket, TArray<uint8>& OutPayload, TFunctionRef<void()> Pump)
    {
        // Minimal length-prefix framing without depending on Runtime Private
        // protocol internals: 8-byte big-endian length plus JSON payload.
        TArray<uint8> Pending;
        return PollUntil([&]()
        {
            Pump();
            uint8 Buffer[65536];
            int32 Read = 0;
            if (Socket.Recv(Buffer, UE_ARRAY_COUNT(Buffer), Read) && Read > 0)
            {
                Pending.Append(Buffer, Read);
            }
            if (Pending.Num() < 8)
            {
                return false;
            }
            uint64 Length = 0;
            for (int32 Index = 0; Index < 8; ++Index)
            {
                Length = (Length << 8) | Pending[Index];
            }
            if ((uint64)Pending.Num() < 8 + Length)
            {
                return false;
            }
            OutPayload.SetNumUninitialized((int32)Length);
            if (Length > 0)
            {
                FMemory::Memcpy(OutPayload.GetData(), Pending.GetData() + 8, (int32)Length);
            }
            return true;
        });
    };
    auto WaitForClose = [&](FSocket& Socket)
    {
        return PollUntil([&]()
        {
            uint8 Buffer[1024];
            int32 Read = 0;
            return !Socket.Recv(Buffer, UE_ARRAY_COUNT(Buffer), Read);
        });
    };
    auto TryReceiveOnce = [&](FSocket& Socket, TArray<uint8>& Pending, TArray<uint8>& OutPayload)
    {
        // One non-blocking pass over the same framing: true only when a full
        // packet is already available, so silence is observable without a
        // timing assumption.
        uint8 Buffer[65536];
        int32 Read = 0;
        if (Socket.Recv(Buffer, UE_ARRAY_COUNT(Buffer), Read) && Read > 0)
        {
            Pending.Append(Buffer, Read);
        }
        if (Pending.Num() < 8)
        {
            return false;
        }
        uint64 Length = 0;
        for (int32 Index = 0; Index < 8; ++Index)
        {
            Length = (Length << 8) | Pending[Index];
        }
        if (Length > 1024 * 1024 || (uint64)Pending.Num() < 8 + Length)
        {
            return false;
        }
        OutPayload.SetNumUninitialized((int32)Length);
        if (Length > 0)
        {
            FMemory::Memcpy(OutPayload.GetData(), Pending.GetData() + 8, (int32)Length);
        }
        Pending.RemoveAt(0, (int32)(8 + Length), EAllowShrinking::No);
        return true;
    };
    auto ConnectClient = [&]()
    {
        FSocket* Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MtoURefreshEndsSession client"));
        if (!Socket || !Socket->Connect(*Loopback(Port)))
        {
            DestroySocket(Socket);
            return (FSocket*)nullptr;
        }
        Socket->SetNonBlocking(true);
        return Socket;
    };
    auto EvaluateRoot = [&](FVector& OutRoot)
    {
        FLiveLinkSubjectFrameData Evaluated;
        if (!LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                LiveSubjectKey, ULiveLinkAnimationRole::StaticClass(), Evaluated))
        {
            return false;
        }
        const FLiveLinkAnimationFrameData* Animation =
            Evaluated.FrameData.Cast<FLiveLinkAnimationFrameData>();
        if (!Animation || !Animation->Transforms.IsValidIndex(0))
        {
            return false;
        }
        OutRoot = Animation->Transforms[0].GetTranslation();
        return true;
    };
    FSocket* AnimClient = ConnectClient();
    TestNotNull(TEXT("animation client connects"), AnimClient);
    const TArray<uint8> AnimInit = PacketFor(FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[%s],\"curves\":[]}"),
        *BonesJson));
    TestTrue(TEXT("animation init is sent"), AnimClient && SendBytes(*AnimClient, AnimInit.GetData(), AnimInit.Num()));
    TArray<uint8> Payload;
    TestTrue(TEXT("animation negotiation produces ready"), AnimClient && ReceiveOne(*AnimClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("animation ready selects the Driver workflow"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\""))
            && FromUtf8(Payload).Contains(TEXT("\"workflow\":\"animation\"")));
    TestTrue(TEXT("animation connection selects the Driver display"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver
            && Actor->GetConnectionStatus().Contains(TEXT("Connected")));
    const TArray<uint8> AnimFrame = PacketFor(FrameJsonFor(*TestSkeleton, FVector(11.0, 12.0, 13.0), TEXT("[]")));
    TestTrue(TEXT("animation frame is sent"), AnimClient && SendBytes(*AnimClient, AnimFrame.GetData(), AnimFrame.Num()));
    FVector AnimRoot = FVector::ZeroVector;
    TestTrue(TEXT("animation frame reaches Live Link"),
        PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            return EvaluateRoot(AnimRoot) && AnimRoot.Equals(FVector(11.0, 12.0, 13.0));
        }));
    // Critical window W1: queued old-session items must fail the synchronous
    // publish gate before any worker disconnect cleanup. The test-only hold
    // below keeps the worker serving the old socket and session, so anything
    // refused from here is refused by the gate, not by asynchronous close
    // handling. No Update runs between intake and the public refresh, and no
    // Sleep sizes the window: intake is observed, not waited out.
    MtoUSetDeferStreamingSessionEndCleanup(true);
    const uint64 AnimSession = FMtoURefreshEndsSessionTestAccess::Session(*Source);
    TestTrue(TEXT("animation session is established on the Game Thread"), AnimSession != 0);
    const TArray<uint8> StaleAnimFrame = PacketFor(FrameJsonFor(*TestSkeleton, FVector(21.0, 22.0, 23.0), TEXT("[]")));
    TestTrue(TEXT("stale animation frame is sent before refresh"),
        AnimClient && SendBytes(*AnimClient, StaleAnimFrame.GetData(), StaleAnimFrame.Num()));
    // A real stale cached upload for the same old session, in wire order:
    // enter, begin, one frame, end, then a play outcome request.
    const TArray<uint8> StaleAnimCacheEnter = PacketFor(TEXT("{\"type\":\"cache_enter\"}"));
    TestTrue(TEXT("stale animation cache entry is sent before refresh"),
        AnimClient && SendBytes(*AnimClient, StaleAnimCacheEnter.GetData(), StaleAnimCacheEnter.Num()));
    const TArray<uint8> StaleAnimCacheBegin = PacketFor(
        TEXT("{\"type\":\"cache_begin\",\"upload_id\":1,\"revision\":9,\"fps\":30,\"start_frame\":1001,\"end_frame\":1001,\"frame_count\":1,\"payload_size\":512}"));
    TestTrue(TEXT("stale animation cache begin is sent before refresh"),
        AnimClient && SendBytes(*AnimClient, StaleAnimCacheBegin.GetData(), StaleAnimCacheBegin.Num()));
    const TArray<uint8> StaleAnimCacheFrame = PacketFor(CacheFrameJsonFor(*TestSkeleton, 0, FVector(61.0, 62.0, 63.0), TEXT("[]")));
    TestTrue(TEXT("stale animation cache frame is sent before refresh"),
        AnimClient && SendBytes(*AnimClient, StaleAnimCacheFrame.GetData(), StaleAnimCacheFrame.Num()));
    const TArray<uint8> StaleAnimCacheEnd = PacketFor(TEXT("{\"type\":\"cache_end\"}"));
    TestTrue(TEXT("stale animation cache end is sent before refresh"),
        AnimClient && SendBytes(*AnimClient, StaleAnimCacheEnd.GetData(), StaleAnimCacheEnd.Num()));
    const TArray<uint8> StaleAnimCachePlay = PacketFor(TEXT("{\"type\":\"cache_play\",\"play_id\":1}"));
    TestTrue(TEXT("stale animation cache play is sent before refresh"),
        AnimClient && SendBytes(*AnimClient, StaleAnimCachePlay.GetData(), StaleAnimCachePlay.Num()));
    TestTrue(TEXT("worker admits the stale live frame and cached upload before refresh"),
        PollUntil([&]()
        {
            return FMtoURefreshEndsSessionTestAccess::HasPendingLiveFrame(*Source)
                && Source->GetQueuedCacheFrameCount() == 1
                && FMtoURefreshEndsSessionTestAccess::PendingCacheCommandCount(*Source) == 5;
        }));
    TWeakObjectPtr<USkeletalMesh> AnimReplaced = SecondMesh;
    const FMtoUPreviewReadiness AfterAnimRefresh = FMtoUPreviewPreparation::RefreshActor(*Actor);
    USkeletalMesh* AnimRefreshedMesh = AfterAnimRefresh.GeneratedPreview;
    TestTrue(TEXT("refresh during Animation ends the session but still shows Generated Preview disconnected"),
        AfterAnimRefresh.IsUsable() && AnimRefreshedMesh != nullptr && AnimRefreshedMesh != SecondMesh
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == AnimRefreshedMesh);
    // The distinguishing pair: the worker still serves the replaced session
    // (cleanup held above), yet the session no longer owns Game Thread
    // publication. Without the synchronous epoch gate this would read
    // true/true and every stale item below would publish.
    TestTrue(TEXT("worker still serves the replaced animation session"),
        FMtoURefreshEndsSessionTestAccess::IsWorkerCurrent(*Source, AnimSession));
    TestFalse(TEXT("replaced animation session loses publication at the refresh boundary"),
        FMtoURefreshEndsSessionTestAccess::IsPublishable(*Source, AnimSession));
    TestTrue(TEXT("stale items are still queued at the refresh boundary"),
        FMtoURefreshEndsSessionTestAccess::HasPendingLiveFrame(*Source)
            && FMtoURefreshEndsSessionTestAccess::PendingCacheCommandCount(*Source) == 5);
    // Drive consumption/publication at the boundary: one Update must refuse
    // the stale live frame, the whole cached upload, and its play outcome
    // without publishing anything.
    Source->Update();
    LiveLinkClient.ForceTick();
    FVector AnimHeldRoot = FVector::ZeroVector;
    TestTrue(TEXT("pre-refresh pose is held, never replaced by the stale frame"),
        EvaluateRoot(AnimHeldRoot) && AnimHeldRoot.Equals(FVector(11.0, 12.0, 13.0)));
    TestFalse(TEXT("stale live frame slot is consumed without publishing"),
        FMtoURefreshEndsSessionTestAccess::HasPendingLiveFrame(*Source));
    TestTrue(TEXT("stale cached upload gains no ownership and cache never enters"),
        Source->GetQueuedCacheFrameCount() == 0
            && FMtoURefreshEndsSessionTestAccess::PendingCacheCommandCount(*Source) == 0
            && FMtoURefreshEndsSessionTestAccess::CacheState(*Source) == EMtoUCacheState::Idle);
    // The old socket is still open (cleanup held): prove nothing was queued
    // for it to send.
    TArray<uint8> AnimDrainPending;
    TArray<uint8> AnimDrainPayload;
    TestFalse(TEXT("no reply is produced for the replaced animation session"),
        TryReceiveOnce(*AnimClient, AnimDrainPending, AnimDrainPayload));
    MtoUSetDeferStreamingSessionEndCleanup(false);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("animation refresh releases the replaced preview"), AnimReplaced.IsValid());
    TestTrue(TEXT("animation refresh closes the old socket"), AnimClient && WaitForClose(*AnimClient));
    TestTrue(TEXT("animation refresh returns source to listening"),
        PollUntil([&]() { return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on")); }));
    TestTrue(TEXT("animation refresh requires explicit reconnection"),
        PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            FLiveLinkSubjectFrameData Stale;
            return !LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                LiveSubjectKey, ULiveLinkAnimationRole::StaticClass(), Stale);
        }));
    TestTrue(TEXT("stale animation frame never publishes after refresh"),
        PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            FVector Root = FVector::ZeroVector;
            if (EvaluateRoot(Root))
            {
                return !Root.Equals(FVector(21.0, 22.0, 23.0));
            }
            return true;
        }));
    TestTrue(TEXT("animation refresh keeps usable readiness and Generated Preview display"),
        Actor->GetPreviewReadiness().IsUsable()
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
            && Actor->GetConnectionStatus().Contains(TEXT("Disconnected")));
    DestroySocket(AnimClient);
    FSocket* AnimReconnect = ConnectClient();
    TestNotNull(TEXT("animation reconnects explicitly"), AnimReconnect);
    TestTrue(TEXT("animation reconnect init is sent"),
        AnimReconnect && SendBytes(*AnimReconnect, AnimInit.GetData(), AnimInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("animation reconnect produces ready"), AnimReconnect && ReceiveOne(*AnimReconnect, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("reconnecting in Animation selects the Driver, never generated readiness"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    const TArray<uint8> AnimFrameAfter = PacketFor(FrameJsonFor(*TestSkeleton, FVector(31.0, 32.0, 33.0), TEXT("[]")));
    TestTrue(TEXT("post-refresh animation frame is sent"),
        AnimReconnect && SendBytes(*AnimReconnect, AnimFrameAfter.GetData(), AnimFrameAfter.Num()));
    FVector AnimAfterRoot = FVector::ZeroVector;
    TestTrue(TEXT("post-refresh animation session streams again"),
        PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            return EvaluateRoot(AnimAfterRoot) && AnimAfterRoot.Equals(FVector(31.0, 32.0, 33.0));
        }));
    DestroySocket(AnimReconnect);
    TestTrue(TEXT("source returns to listening after animation disconnect"),
        PollUntil([&]()
        {
            Source->Update();
            return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on"));
        }));
    FSocket* ModelClient = ConnectClient();
    TestNotNull(TEXT("model client connects"), ModelClient);
    const TArray<uint8> ModelInit = PacketFor(FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"model\",\"blendshapes_enabled\":true,\"bones\":[%s],\"curves\":[\"Corrective\"]}"),
        *BonesJson));
    TestTrue(TEXT("model init is sent"), ModelClient && SendBytes(*ModelClient, ModelInit.GetData(), ModelInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("model negotiation produces ready"), ModelClient && ReceiveOne(*ModelClient, Payload, [&]() { Source->Update(); }));
    const FString ModelReady = FromUtf8(Payload);
    TestTrue(TEXT("model ready renegotiates Morph coverage"),
        ModelReady.Contains(TEXT("\"type\":\"ready\""))
            && ModelReady.Contains(TEXT("\"workflow\":\"model\""))
            && ModelReady.Contains(TEXT("\"accepted_morph_count\":1")));
    TestTrue(TEXT("model connection selects Generated Preview with coverage diagnostics"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == AnimRefreshedMesh
            && (Actor->GetModelDiagnosticLevel() == EMtoUModelDiagnosticLevel::Full
                || Actor->GetModelDiagnosticLevel() == EMtoUModelDiagnosticLevel::Partial));
    const FString ModelFrameText = FrameJsonFor(*TestSkeleton, FVector(41.0, 42.0, 43.0), TEXT("[0.5]"));
    const TArray<uint8> ModelFramePacket = PacketFor(ModelFrameText);
    TestTrue(TEXT("model frame is sent"), ModelClient && SendBytes(*ModelClient, ModelFramePacket.GetData(), ModelFramePacket.Num()));
    FVector ModelRoot = FVector::ZeroVector;
    TestTrue(TEXT("model frame reaches Live Link"),
        PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            return EvaluateRoot(ModelRoot) && ModelRoot.Equals(FVector(41.0, 42.0, 43.0));
        }));
    // Critical window W2: an in-flight cached playback must freeze at the
    // refresh boundary. Drive a real upload to Ready, then to Playing, while
    // the session is still live.
    const TArray<uint8> ModelCacheEnter = PacketFor(TEXT("{\"type\":\"cache_enter\"}"));
    TestTrue(TEXT("model cache entry is sent"),
        ModelClient && SendBytes(*ModelClient, ModelCacheEnter.GetData(), ModelCacheEnter.Num()));
    // A three-frame upload: a one-frame replay would complete inside the
    // dispatch Update itself, so Playing would never survive to the refresh
    // boundary this window must freeze.
    const TArray<uint8> ModelCacheBegin = PacketFor(
        TEXT("{\"type\":\"cache_begin\",\"upload_id\":1,\"revision\":9,\"fps\":30,\"start_frame\":2001,\"end_frame\":2003,\"frame_count\":3,\"payload_size\":2048}"));
    TestTrue(TEXT("model cache begin is sent"),
        ModelClient && SendBytes(*ModelClient, ModelCacheBegin.GetData(), ModelCacheBegin.Num()));
    for (int32 CacheFrameIndex = 0; CacheFrameIndex < 3; ++CacheFrameIndex)
    {
        const TArray<uint8> ModelCacheFrame = PacketFor(CacheFrameJsonFor(*TestSkeleton, CacheFrameIndex, FVector(61.0, 62.0, 63.0), TEXT("[0.9]")));
        TestTrue(TEXT("model cache frame is sent before refresh"),
            ModelClient && SendBytes(*ModelClient, ModelCacheFrame.GetData(), ModelCacheFrame.Num()));
    }
    const TArray<uint8> ModelCacheEnd = PacketFor(TEXT("{\"type\":\"cache_end\"}"));
    TestTrue(TEXT("model cache end is sent"),
        ModelClient && SendBytes(*ModelClient, ModelCacheEnd.GetData(), ModelCacheEnd.Num()));
    TestTrue(TEXT("model cache upload reaches Ready before refresh"),
        PollUntil([&]()
        {
            Source->Update();
            return FMtoURefreshEndsSessionTestAccess::CacheState(*Source) == EMtoUCacheState::Ready;
        }));
    TestTrue(TEXT("model cache Ready reply is received"),
        ModelClient && ReceiveOne(*ModelClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("model cache Ready reply is well formed"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"cache_ready\"")));
    const TArray<uint8> ModelCachePlay = PacketFor(TEXT("{\"type\":\"cache_play\",\"play_id\":1}"));
    TestTrue(TEXT("model cache play is sent"),
        ModelClient && SendBytes(*ModelClient, ModelCachePlay.GetData(), ModelCachePlay.Num()));
    TestTrue(TEXT("model cached playback starts before refresh"),
        PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            return FMtoURefreshEndsSessionTestAccess::CacheState(*Source) == EMtoUCacheState::Playing;
        }));
    // Drain pre-refresh progress so the window below observes only
    // post-boundary traffic.
    TArray<uint8> ModelDrainPending;
    TArray<uint8> ModelDrainPayload;
    for (int32 Drain = 0; Drain < 50 && TryReceiveOnce(*ModelClient, ModelDrainPending, ModelDrainPayload); ++Drain)
    {
    }
    // Hold worker disconnect cleanup for the boundary, then deliver a real
    // stale live frame over the still-served old socket. No Update runs
    // until after the public refresh below.
    MtoUSetDeferStreamingSessionEndCleanup(true);
    const uint64 ModelSession = FMtoURefreshEndsSessionTestAccess::Session(*Source);
    TestTrue(TEXT("model session is established on the Game Thread"), ModelSession != 0);
    const TArray<uint8> StaleModelFrame = PacketFor(FrameJsonFor(*TestSkeleton, FVector(51.0, 52.0, 53.0), TEXT("[0.9]")));
    TestTrue(TEXT("stale model frame is sent before refresh"),
        ModelClient && SendBytes(*ModelClient, StaleModelFrame.GetData(), StaleModelFrame.Num()));
    TestTrue(TEXT("worker admits the stale model frame before refresh"),
        PollUntil([&]() { return FMtoURefreshEndsSessionTestAccess::HasPendingLiveFrame(*Source); }));
    TWeakObjectPtr<USkeletalMesh> ModelReplaced = AnimRefreshedMesh;
    const FMtoUPreviewReadiness AfterModelRefresh = FMtoUPreviewPreparation::RefreshActor(*Actor);
    USkeletalMesh* ModelRefreshedMesh = AfterModelRefresh.GeneratedPreview;
    TestTrue(TEXT("refresh during Model ends the old session but still shows Generated Preview disconnected"),
        AfterModelRefresh.IsUsable() && ModelRefreshedMesh != nullptr && ModelRefreshedMesh != AnimRefreshedMesh
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview);
    // Same distinguishing pair as W1: worker still serves the old Model
    // session, yet it no longer owns publication. The in-flight replay must
    // neither advance nor report an outcome from this boundary.
    TestTrue(TEXT("worker still serves the replaced model session"),
        FMtoURefreshEndsSessionTestAccess::IsWorkerCurrent(*Source, ModelSession));
    TestFalse(TEXT("replaced model session loses publication at the refresh boundary"),
        FMtoURefreshEndsSessionTestAccess::IsPublishable(*Source, ModelSession));
    TestTrue(TEXT("stale model frame is still queued at the refresh boundary"),
        FMtoURefreshEndsSessionTestAccess::HasPendingLiveFrame(*Source));
    TestTrue(TEXT("in-flight playback is untouched at the refresh boundary"),
        FMtoURefreshEndsSessionTestAccess::CacheState(*Source) == EMtoUCacheState::Playing);
    // Freeze window: many gated Updates over wall-clock time that would
    // finish a three-frame replay at 30 fps. The replay must stay Playing,
    // the stale live frame must never evaluate, and no completion may reach
    // the still-open old socket. Without the gate the replay would complete
    // here and report cache_complete, so this window is the regression.
    bool bModelStaleEvaluated = false;
    bool bModelCompleteReply = false;
    int32 ModelProgressReplies = 0;
    const double ModelWindowEnd = FPlatformTime::Seconds() + 0.5;
    do
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        FVector WindowRoot = FVector::ZeroVector;
        if (EvaluateRoot(WindowRoot) && WindowRoot.Equals(FVector(51.0, 52.0, 53.0)))
        {
            bModelStaleEvaluated = true;
        }
        while (TryReceiveOnce(*ModelClient, ModelDrainPending, ModelDrainPayload))
        {
            const FString ReplyText = FromUtf8(ModelDrainPayload);
            if (ReplyText.Contains(TEXT("cache_complete")))
            {
                bModelCompleteReply = true;
            }
            else if (ReplyText.Contains(TEXT("cache_progress")))
            {
                // A pre-refresh progress packet can still be in flight from
                // the live replay above; it is reported, not failed on.
                ++ModelProgressReplies;
            }
        }
        FPlatformProcess::Sleep(0.005f);
    }
    while (FPlatformTime::Seconds() < ModelWindowEnd);
    TestFalse(TEXT("stale model frame never publishes during the freeze window"), bModelStaleEvaluated);
    TestFalse(TEXT("in-flight playback reports no completion during the freeze window"), bModelCompleteReply);
    AddInfo(FString::Printf(TEXT("progress packets observed during the freeze window: %d"), ModelProgressReplies));
    TestTrue(TEXT("in-flight playback stays frozen, never completing on the replaced session"),
        FMtoURefreshEndsSessionTestAccess::CacheState(*Source) == EMtoUCacheState::Playing);
    TestTrue(TEXT("frozen upload holds no parsed queue ownership after the window"),
        Source->GetQueuedCacheFrameCount() == 0);
    MtoUSetDeferStreamingSessionEndCleanup(false);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("model refresh releases the replaced preview"), ModelReplaced.IsValid());
    TestTrue(TEXT("model refresh closes the old socket"), ModelClient && WaitForClose(*ModelClient));
    TestTrue(TEXT("model refresh returns source to listening"),
        PollUntil([&]() { return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on")); }));
    TestTrue(TEXT("stale model frame and cached upload never publish after refresh"),
        PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            FVector Root = FVector::ZeroVector;
            if (EvaluateRoot(Root))
            {
                return !Root.Equals(FVector(51.0, 52.0, 53.0));
            }
            return true;
        }));
    TestEqual(TEXT("stale cached upload gains no parsed queue ownership"), Source->GetQueuedCacheFrameCount(), 0);
    DestroySocket(ModelClient);
    FSocket* FailingClient = ConnectClient();
    TestNotNull(TEXT("model session for failed refresh connects"), FailingClient);
    TestTrue(TEXT("failing-session model init is sent"),
        FailingClient && SendBytes(*FailingClient, ModelInit.GetData(), ModelInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("failing-session model ready is received"),
        FailingClient && ReceiveOne(*FailingClient, Payload, [&]() { Source->Update(); }));
    UStaticMesh* MisalignedPreview = MakePreview(*Driver, *WorldPackage, false, false, false, true);
    TestNotNull(TEXT("misaligned failure preview is created"), MisalignedPreview);
    TStrongObjectPtr<UStaticMesh> MisalignedGuard(MisalignedPreview);
    UStaticMesh* AlignedPreview = Preview;
    TStrongObjectPtr<UStaticMesh> AlignedGuard(AlignedPreview);
    Binding->PreviewStaticMesh = MisalignedPreview;
    const FMtoUPreviewReadiness Failed = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("failed Refresh keeps Error readiness and restores the Driver"),
        !Failed.IsUsable() && Failed.GeneratedPreview == nullptr
            && Failed.State == EMtoUPreviewState::Error
            && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    TestTrue(TEXT("failed refresh terminates the Model session"), FailingClient && WaitForClose(*FailingClient));
    TestTrue(TEXT("failed refresh returns source to listening"),
        PollUntil([&]() { return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on")); }));
    TestTrue(TEXT("failed refresh keeps Error and Driver for inspection"),
        PollUntil([&]()
        {
            Source->Update();
            return Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
                && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver;
        }));
    DestroySocket(FailingClient);
    Binding->PreviewStaticMesh = AlignedPreview;
    const FMtoUPreviewReadiness Recovered = FMtoUPreviewPreparation::RefreshActor(*Actor);
    USkeletalMesh* RecoveredMesh = Recovered.GeneratedPreview;
    TestTrue(TEXT("failed-refresh recovery restores Generated Preview"),
        Recovered.IsUsable() && RecoveredMesh != nullptr
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview);
    FSocket* ModelReconnect = ConnectClient();
    TestNotNull(TEXT("model reconnects explicitly after recovery"), ModelReconnect);
    TestTrue(TEXT("model reconnect init is sent"),
        ModelReconnect && SendBytes(*ModelReconnect, ModelInit.GetData(), ModelInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("model reconnect produces ready"), ModelReconnect && ReceiveOne(*ModelReconnect, Payload, [&]() { Source->Update(); }));
    const FString ModelReconnectReady = FromUtf8(Payload);
    TestTrue(TEXT("model reconnect renegotiates Morph coverage on the new display"),
        ModelReconnectReady.Contains(TEXT("\"accepted_morph_count\":1"))
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == RecoveredMesh);
    DestroySocket(ModelReconnect);
    // The previous socket close is observed asynchronously: a new client
    // connecting first would be rejected as a second client.
    TestTrue(TEXT("source returns to listening before the animation failure session"),
        PollUntil([&]()
        {
            Source->Update();
            return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on"));
        }));
    // Failed refresh during an active Animation session: the same
    // termination applies, Error readiness keeps the Driver for inspection,
    // and a later refresh recovers without a new actor.
    FSocket* AnimFailClient = ConnectClient();
    TestNotNull(TEXT("animation session for failed refresh connects"), AnimFailClient);
    TestTrue(TEXT("failing-session animation init is sent"),
        AnimFailClient && SendBytes(*AnimFailClient, AnimInit.GetData(), AnimInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("failing-session animation ready is received"),
        AnimFailClient && ReceiveOne(*AnimFailClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("failing animation session selects the Driver display"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver
            && Actor->GetConnectionStatus().Contains(TEXT("Connected")));
    UStaticMesh* AnimMisalignedPreview = MakePreview(*Driver, *WorldPackage, false, false, false, true);
    TestNotNull(TEXT("misaligned animation-failure preview is created"), AnimMisalignedPreview);
    TStrongObjectPtr<UStaticMesh> AnimMisalignedGuard(AnimMisalignedPreview);
    Binding->PreviewStaticMesh = AnimMisalignedPreview;
    const FMtoUPreviewReadiness AnimFailed = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("failed Refresh during Animation keeps Error readiness and restores the Driver"),
        !AnimFailed.IsUsable() && AnimFailed.GeneratedPreview == nullptr
            && AnimFailed.State == EMtoUPreviewState::Error
            && Actor->GetPreviewReadiness().State == EMtoUPreviewState::Error
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    TestTrue(TEXT("failed Animation refresh terminates the session"), AnimFailClient && WaitForClose(*AnimFailClient));
    TestTrue(TEXT("failed Animation refresh returns source to listening"),
        PollUntil([&]() { return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on")); }));
    DestroySocket(AnimFailClient);
    Binding->PreviewStaticMesh = AlignedPreview;
    const FMtoUPreviewReadiness AnimRecovered = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("animation failed-refresh recovery restores Generated Preview"),
        AnimRecovered.IsUsable() && AnimRecovered.GeneratedPreview != nullptr
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview);
    TestTrue(TEXT("source returns to listening after final disconnect"),
        PollUntil([&]()
        {
            Source->Update();
            return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on"));
        }));
    Source->StopListener();
    LiveLinkClient.RemoveSource(Source);
    if (World)
    {
        World->DestroyWorld(false);
        if (GEngine)
        {
            GEngine->DestroyWorldContext(World);
        }
    }
    return true;
}

// Explicit opt-in: the ordinary Editor suite never assumes Maya is installed.
// A real Maya host runs the real sender transport through an Animation
// session while this test performs the public explicit refresh, so the
// dual-host half of Issue #39 is observed from Maya itself: transport loss,
// no automatic reconnect, and streaming resumed only by an explicit connect.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUMayaRefreshPeerTest,
    "MtoULiveLink.Editor.Preview.MayaRefreshPeer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMayaRefreshPeerTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FString Mayapy;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUMayapy="), Mayapy))
    {
        AddInfo(TEXT("Real Maya peer not requested; supply MtoUMayapy, MtoURefreshPeer and MtoUEvidence."));
        return true;
    }
    FString Peer;
    FString Evidence;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MtoURefreshPeer="), Peer)
        || !FParse::Value(FCommandLine::Get(), TEXT("MtoUEvidence="), Evidence)
        || !FPaths::FileExists(Mayapy) || !FPaths::FileExists(Peer))
    {
        AddError(TEXT("Real Maya peer requires existing mayapy/peer paths and an evidence directory."));
        return false;
    }
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
    // Issue #39 re-review: this host test must not leak its world, source,
    // or Maya process into later tests in the same UE process. The guard
    // covers normal completion and every early return below.
    TSharedPtr<FMtoULiveLinkSource> PeerSource;
    UWorld* PeerWorld = nullptr;
    FProcHandle PeerProcess;
    ON_SCOPE_EXIT
    {
        if (PeerProcess.IsValid())
        {
            if (FPlatformProcess::IsProcRunning(PeerProcess))
            {
                FPlatformProcess::TerminateProc(PeerProcess, true);
            }
            FPlatformProcess::CloseProc(PeerProcess);
        }
        if (PeerSource.IsValid())
        {
            PeerSource->StopListener();
            LiveLinkClient.RemoveSource(PeerSource.ToSharedRef());
        }
        if (PeerWorld)
        {
            PeerWorld->DestroyWorld(false);
            if (GEngine)
            {
                GEngine->DestroyWorldContext(PeerWorld);
            }
        }
    };
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUMayaRefreshPeerWorld"));
    USkeletalMesh* Driver = MakeMorphDriver(*WorldPackage);
    UStaticMesh* Preview = Driver ? MakePreview(*Driver, *WorldPackage) : nullptr;
    TestNotNull(TEXT("peer Driver with Morphs is created"), Driver);
    TestNotNull(TEXT("peer Preview is created"), Preview);
    if (!Driver || !Preview)
    {
        return false;
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("MtoUMayaRefreshPeerWorld"), WorldPackage, true);
    PeerWorld = World;
    TestNotNull(TEXT("editor world is created"), World);
    if (!World)
    {
        return false;
    }
    if (GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = World->SpawnActor<AMtoULiveLinkActor>();
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = Preview;
    TestNotNull(TEXT("binding actor is created"), Actor);
    if (!Actor)
    {
        return false;
    }
    Actor->SetBinding(Binding);
    const FReferenceSkeleton& Skeleton = Driver->GetRefSkeleton();
    FString Bones;
    for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
    {
        if (Index)
        {
            Bones += TEXT(",");
        }
        Bones += FString::Printf(TEXT("[\"%s\",%d]"),
            *Skeleton.GetBoneName(Index).ToString(), Skeleton.GetParentIndex(Index));
    }
    IFileManager::Get().MakeDirectory(*Evidence, true);
    const FString FixturePath = FPaths::Combine(Evidence, TEXT("maya-refresh-fixture.json"));
    const FString ResultPath = FPaths::Combine(Evidence, TEXT("maya-refresh-result.json"));
    const FString CommandPath = FPaths::Combine(Evidence, TEXT("maya-refresh-command.json"));
    FSocket* Reservation = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MtoUMayaRefreshPeer reservation"));
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    auto Loopback = [&](uint16 Port)
    {
        TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
        bool bValid = false;
        Address->SetIp(TEXT("127.0.0.1"), bValid);
        check(bValid);
        Address->SetPort(Port);
        return Address;
    };
    Reservation->Bind(*Loopback(0));
    Reservation->Listen(1);
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    Reservation->Close();
    SocketSubsystem->DestroySocket(Reservation);
    TestTrue(TEXT("ephemeral listening port is reported"), Port > 0);
    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    PeerSource = Source;
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    const FLiveLinkSubjectKey LiveSubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));
    auto WriteCommand = [&](const FString& Action, int32 Time)
    {
        return FFileHelper::SaveStringToFile(
            FString::Printf(TEXT("{\"action\":\"%s\",\"time\":%d}"), *Action, Time), *CommandPath);
    };
    auto ReadResult = [&]() -> TSharedPtr<FJsonObject>
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *ResultPath))
        {
            return nullptr;
        }
        TSharedPtr<FJsonObject> Object;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
        {
            return nullptr;
        }
        return Object;
    };
    auto EvaluateRootX = [&](double& OutX)
    {
        FLiveLinkSubjectFrameData Evaluated;
        if (!LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                LiveSubjectKey, ULiveLinkAnimationRole::StaticClass(), Evaluated))
        {
            return false;
        }
        const FLiveLinkAnimationFrameData* Animation =
            Evaluated.FrameData.Cast<FLiveLinkAnimationFrameData>();
        if (!Animation || !Animation->Transforms.IsValidIndex(0))
        {
            return false;
        }
        OutX = Animation->Transforms[0].GetTranslation().X;
        return true;
    };
    TestTrue(TEXT("peer fixture is written"),
        FFileHelper::SaveStringToFile(
            FString::Printf(TEXT("{\"port\":%d,\"bones\":[%s],\"command\":\"%s\"}"),
                Port, *Bones, *CommandPath.Replace(TEXT("\\"), TEXT("/"))),
            *FixturePath));
    // Empty result so the first poll cannot read a stale file.
    TestTrue(TEXT("peer result is initialized"),
        FFileHelper::SaveStringToFile(TEXT("{\"phase\":\"starting\"}"), *ResultPath));
    TestTrue(TEXT("initial stream command is written"), WriteCommand(TEXT("stream"), 1));
    const FString ProcArgs = FString::Printf(TEXT("\"%s\" --fixture \"%s\" --result \"%s\""),
        *Peer, *FixturePath, *ResultPath);
    FProcHandle Process = FPlatformProcess::CreateProc(*Mayapy, *ProcArgs, false, true, true,
        nullptr, 0, nullptr, nullptr);
    PeerProcess = Process;
    TestTrue(TEXT("real Maya process starts"), Process.IsValid());
    int32 Stage = 0;
    bool bSawFirstPose = false;
    bool bSawSecondPose = false;
    bool bRefreshed = false;
    bool bPeerDisconnected = false;
    bool bReconnectCommanded = false;
    const double Deadline = FPlatformTime::Seconds() + 300.0;
    while (Process.IsValid() && FPlatformProcess::IsProcRunning(Process)
        && FPlatformTime::Seconds() < Deadline && Stage < 5)
    {
        Source->Update();
        LiveLinkClient.ForceTick();
        double RootX = 0.0;
        const bool bEvaluated = EvaluateRootX(RootX);
        TSharedPtr<FJsonObject> ResultObject = ReadResult();
        const FString Phase = ResultObject.IsValid()
            ? ResultObject->GetStringField(TEXT("phase")) : FString();
        switch (Stage)
        {
            case 0:
                if (Phase == TEXT("streaming") && bEvaluated
                    && FMath::IsNearlyEqual(RootX, 10.0, 0.1))
                {
                    bSawFirstPose = true;
                    WriteCommand(TEXT("stream"), 2);
                    Stage = 1;
                }
                break;
            case 1:
                if (bEvaluated && FMath::IsNearlyEqual(RootX, 20.0, 0.1))
                {
                    bSawSecondPose = true;
                    const FMtoUPreviewReadiness Readiness =
                        FMtoUPreviewPreparation::RefreshActor(*Actor);
                    bRefreshed = Readiness.IsUsable() && Readiness.GeneratedPreview != nullptr
                        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
                        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
                            == Readiness.GeneratedPreview;
                    Stage = 2;
                }
                break;
            case 2:
                if (Phase == TEXT("disconnected"))
                {
                    Stage = 3;
                }
                break;
            case 3:
                if (Source->GetSourceStatus().ToString().Contains(TEXT("Listening on")))
                {
                    // The actor's Disconnected status lands on the Update that
                    // drains the ended session, after the refresh itself.
                    Source->Update();
                    LiveLinkClient.ForceTick();
                    bPeerDisconnected = Actor->GetConnectionStatus().Contains(TEXT("Disconnected"));
                    WriteCommand(TEXT("reconnect"), 0);
                    bReconnectCommanded = true;
                    Stage = 4;
                }
                break;
            case 4:
                if (Phase == TEXT("reconnected") && bEvaluated
                    && FMath::IsNearlyEqual(RootX, 30.0, 0.1)
                    && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver)
                {
                    WriteCommand(TEXT("stop"), 0);
                    Stage = 5;
                }
                break;
            default:
                break;
        }
        FPlatformProcess::Sleep(0.005f);
    }
    TestTrue(TEXT("real Maya animation streams before refresh"), bSawFirstPose);
    TestTrue(TEXT("real Maya animation advances while streaming"), bSawSecondPose);
    TestTrue(TEXT("public refresh ends the Maya session and shows Generated Preview"), bRefreshed);
    TestTrue(TEXT("Unreal observes Disconnected after the ended session drains"), bPeerDisconnected);
    TestTrue(TEXT("reconnect was commanded only after Maya observed the disconnect"), bReconnectCommanded);
    const double PeerExitDeadline = FPlatformTime::Seconds() + 60.0;
    while (Process.IsValid() && FPlatformProcess::IsProcRunning(Process)
        && FPlatformTime::Seconds() < PeerExitDeadline)
    {
        FPlatformProcess::Sleep(0.05f);
    }
    int32 ExitCode = -1;
    if (Process.IsValid())
    {
        if (FPlatformProcess::IsProcRunning(Process))
        {
            FPlatformProcess::TerminateProc(Process, true);
            AddError(TEXT("Real Maya peer timed out"));
        }
        FPlatformProcess::GetProcReturnCode(Process, &ExitCode);
        FPlatformProcess::CloseProc(Process);
        PeerProcess.Reset();
    }
    TestEqual(TEXT("real Maya peer exits cleanly"), ExitCode, 0);
    TSharedPtr<FJsonObject> FinalResult = ReadResult();
    TestTrue(TEXT("Maya result is readable"), FinalResult.IsValid());
    if (FinalResult.IsValid())
    {
        TestTrue(TEXT("Maya peer completed all phases"), FinalResult->GetBoolField(TEXT("ok")));
        const TSharedPtr<FJsonObject>* Disconnected = nullptr;
        if (FinalResult->TryGetObjectField(TEXT("disconnected"), Disconnected) && Disconnected)
        {
            TestTrue(TEXT("Maya observed the refresh disconnect"),
                (*Disconnected)->GetBoolField(TEXT("observed")));
            TestFalse(TEXT("Maya never reconnected on its own"),
                (*Disconnected)->GetBoolField(TEXT("auto_reconnect")));
            FString DiagnosticCode;
            (*Disconnected)->TryGetStringField(TEXT("diagnostic_code"), DiagnosticCode);
            AddInfo(FString::Printf(TEXT("Maya terminal diagnostic: %s"), *DiagnosticCode));
        }
        else
        {
            AddError(TEXT("Maya result lacks the disconnect record"));
        }
        const TSharedPtr<FJsonObject>* Reconnected = nullptr;
        if (FinalResult->TryGetObjectField(TEXT("reconnected"), Reconnected) && Reconnected)
        {
            TestTrue(TEXT("Maya reconnected with a new explicit session"),
                (*Reconnected)->GetBoolField(TEXT("new_session")));
        }
        else
        {
            AddError(TEXT("Maya result lacks the reconnect record"));
        }
        FString FinalText;
        FFileHelper::LoadFileToString(FinalText, *ResultPath);
        AddInfo(FString::Printf(TEXT("Maya result: %s"), *FinalText));
    }
    return true;
}

// Issue #39 re-review: the Details "Refresh Preview" button body (scoped
// slow task plus public RefreshActor with stage progress) must end the
// active Animation session. Automation executes the exact click handler the
// button invokes and proves the customization stays registered; a synthetic
// Slate click is unreliable headless (an unhosted DetailsView materializes
// no property rows in NullRHI), so one physical click still needs manual
// confirmation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUDetailsRefreshClickTest,
    "MtoULiveLink.Editor.Details.RefreshClick",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUDetailsRefreshClickTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    auto PollUntil = [](TFunctionRef<bool()> Predicate)
    {
        const double Deadline = FPlatformTime::Seconds() + 2.0;
        do
        {
            if (Predicate())
            {
                return true;
            }
            FPlatformProcess::Sleep(0.005f);
        }
        while (FPlatformTime::Seconds() < Deadline);
        return Predicate();
    };
    auto Utf8 = [](const FString& Text)
    {
        FTCHARToUTF8 Converted(*Text);
        TArray<uint8> Bytes;
        Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
        return Bytes;
    };
    auto FromUtf8 = [](const TArray<uint8>& Bytes)
    {
        FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
        return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
    };
    auto Prefix = [](uint64 Length)
    {
        TArray<uint8> Bytes;
        Bytes.SetNumUninitialized(8);
        for (int32 Index = 0; Index < 8; ++Index)
        {
            Bytes[Index] = static_cast<uint8>(Length >> ((7 - Index) * 8));
        }
        return Bytes;
    };
    auto PacketFor = [&](const FString& Text)
    {
        TArray<uint8> Payload = Utf8(Text);
        TArray<uint8> Bytes = Prefix(Payload.Num());
        Bytes.Append(Payload);
        return Bytes;
    };
    auto TransformJson = [](const FTransform& Transform)
    {
        const FVector Translation = Transform.GetTranslation();
        const FQuat Rotation = Transform.GetRotation();
        const FVector Scale = Transform.GetScale3D();
        return FString::Printf(
            TEXT("[%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g]"),
            Translation.X, Translation.Y, Translation.Z,
            Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
            Scale.X, Scale.Y, Scale.Z);
    };
    auto BonesJsonFor = [&](const FReferenceSkeleton& Skeleton)
    {
        FString Bones;
        for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
        {
            if (Index > 0)
            {
                Bones += TEXT(",");
            }
            Bones += FString::Printf(
                TEXT("[\"%s\",%d,%s]"),
                *Skeleton.GetBoneName(Index).ToString(),
                Skeleton.GetParentIndex(Index),
                *TransformJson(Skeleton.GetRefBonePose()[Index]));
        }
        return Bones;
    };
    auto FrameJsonFor = [&](const FReferenceSkeleton& Skeleton, const FVector& RootTranslation)
    {
        FString Transforms;
        for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
        {
            if (Index > 0)
            {
                Transforms += TEXT(",");
            }
            FTransform Pose = Skeleton.GetRefBonePose()[Index];
            if (Index == 0)
            {
                Pose.SetTranslation(RootTranslation);
            }
            Transforms += TransformJson(Pose);
        }
        return FString::Printf(TEXT("{\"type\":\"frame\",\"transforms\":[%s],\"curves\":[]}"), *Transforms);
    };
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem);
    if (!SocketSubsystem)
    {
        return false;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    TestTrue(TEXT("Live Link client feature is available"),
        Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName));
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }
    ILiveLinkClient& LiveLinkClient =
        Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
    TSharedPtr<FMtoULiveLinkSource> GuardSource;
    UWorld* GuardWorld = nullptr;
    ON_SCOPE_EXIT
    {
        if (GuardSource.IsValid())
        {
            GuardSource->StopListener();
            LiveLinkClient.RemoveSource(GuardSource.ToSharedRef());
        }
        if (GuardWorld)
        {
            GuardWorld->DestroyWorld(false);
            if (GEngine)
            {
                GEngine->DestroyWorldContext(GuardWorld);
            }
        }
    };
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUDetailsRefreshClickWorld"));
    USkeletalMesh* Driver = MakeMorphDriver(*WorldPackage);
    UStaticMesh* Preview = Driver ? MakePreview(*Driver, *WorldPackage) : nullptr;
    TestNotNull(TEXT("click Driver with Morphs is created"), Driver);
    TestNotNull(TEXT("click Preview is created"), Preview);
    if (!Driver || !Preview)
    {
        return false;
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("MtoUDetailsRefreshClickWorld"), WorldPackage, true);
    TestNotNull(TEXT("editor world is created"), World);
    if (!World)
    {
        return false;
    }
    GuardWorld = World;
    if (GEngine)
    {
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
        WorldContext.SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    AMtoULiveLinkActor* Actor = World->SpawnActor<AMtoULiveLinkActor>();
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    Binding->SkeletalMesh = Driver;
    Binding->PreviewStaticMesh = Preview;
    TestNotNull(TEXT("binding actor is created"), Actor);
    if (!Actor)
    {
        return false;
    }
    Actor->SetBinding(Binding);
    const FReferenceSkeleton* TestSkeleton = &Driver->GetRefSkeleton();
    const FString BonesJson = BonesJsonFor(*TestSkeleton);
    auto Loopback = [&](uint16 Port)
    {
        TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
        bool bValid = false;
        Address->SetIp(TEXT("127.0.0.1"), bValid);
        check(bValid);
        Address->SetPort(Port);
        return Address;
    };
    auto DestroySocket = [&](FSocket*& Socket)
    {
        if (Socket)
        {
            Socket->Close();
            SocketSubsystem->DestroySocket(Socket);
            Socket = nullptr;
        }
    };
    auto SendBytes = [&](FSocket& Socket, const uint8* Data, int32 Num)
    {
        int32 Offset = 0;
        const double Deadline = FPlatformTime::Seconds() + 2.0;
        while (Offset < Num && FPlatformTime::Seconds() < Deadline)
        {
            int32 Sent = 0;
            if (Socket.Send(Data + Offset, Num - Offset, Sent) && Sent > 0)
            {
                Offset += Sent;
            }
            else
            {
                Socket.Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(5));
            }
        }
        return Offset == Num;
    };
    FSocket* Reservation = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MtoUDetailsRefreshClick reservation"));
    TestNotNull(TEXT("test port can be reserved"), Reservation);
    if (!Reservation)
    {
        return false;
    }
    Reservation->Bind(*Loopback(0));
    Reservation->Listen(1);
    TSharedRef<FInternetAddr> ReservedAddress = SocketSubsystem->CreateInternetAddr();
    Reservation->GetAddress(*ReservedAddress);
    const uint16 Port = static_cast<uint16>(ReservedAddress->GetPort());
    DestroySocket(Reservation);
    TestTrue(TEXT("ephemeral listening port is reported"), Port > 0);
    TSharedRef<FMtoULiveLinkSource> Source = MakeShared<FMtoULiveLinkSource>(Port);
    const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
    GuardSource = Source;
    TestTrue(TEXT("Live Link source receives a guid"), SourceGuid.IsValid());
    const FLiveLinkSubjectKey LiveSubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));
    TestTrue(TEXT("source reaches listening state"),
        PollUntil([&]() { return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on")); }));
    auto ReceiveOne = [&](FSocket& Socket, TArray<uint8>& OutPayload, TFunctionRef<void()> Pump)
    {
        TArray<uint8> Pending;
        return PollUntil([&]()
        {
            Pump();
            uint8 Buffer[65536];
            int32 Read = 0;
            if (Socket.Recv(Buffer, UE_ARRAY_COUNT(Buffer), Read) && Read > 0)
            {
                Pending.Append(Buffer, Read);
            }
            if (Pending.Num() < 8)
            {
                return false;
            }
            uint64 Length = 0;
            for (int32 Index = 0; Index < 8; ++Index)
            {
                Length = (Length << 8) | Pending[Index];
            }
            if ((uint64)Pending.Num() < 8 + Length)
            {
                return false;
            }
            OutPayload.SetNumUninitialized((int32)Length);
            if (Length > 0)
            {
                FMemory::Memcpy(OutPayload.GetData(), Pending.GetData() + 8, (int32)Length);
            }
            return true;
        });
    };
    auto WaitForClose = [&](FSocket& Socket)
    {
        return PollUntil([&]()
        {
            uint8 Buffer[1024];
            int32 Read = 0;
            return !Socket.Recv(Buffer, UE_ARRAY_COUNT(Buffer), Read);
        });
    };
    auto ConnectClient = [&]()
    {
        FSocket* Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MtoUDetailsRefreshClick client"));
        if (!Socket || !Socket->Connect(*Loopback(Port)))
        {
            DestroySocket(Socket);
            return (FSocket*)nullptr;
        }
        Socket->SetNonBlocking(true);
        return Socket;
    };
    auto EvaluateRootX = [&](double& OutX)
    {
        FLiveLinkSubjectFrameData Evaluated;
        if (!LiveLinkClient.EvaluateFrameFromSource_AnyThread(
                LiveSubjectKey, ULiveLinkAnimationRole::StaticClass(), Evaluated))
        {
            return false;
        }
        const FLiveLinkAnimationFrameData* Animation =
            Evaluated.FrameData.Cast<FLiveLinkAnimationFrameData>();
        if (!Animation || !Animation->Transforms.IsValidIndex(0))
        {
            return false;
        }
        OutX = Animation->Transforms[0].GetTranslation().X;
        return true;
    };
    FSocket* AnimClient = ConnectClient();
    TestNotNull(TEXT("animation client connects"), AnimClient);
    const TArray<uint8> AnimInit = PacketFor(FString::Printf(
        TEXT("{\"type\":\"init\",\"revision\":9,\"version\":6,\"workflow\":\"animation\",\"blendshapes_enabled\":true,\"bones\":[%s],\"curves\":[]}"),
        *BonesJson));
    TestTrue(TEXT("animation init is sent"), AnimClient && SendBytes(*AnimClient, AnimInit.GetData(), AnimInit.Num()));
    TArray<uint8> Payload;
    TestTrue(TEXT("animation negotiation produces ready"), AnimClient && ReceiveOne(*AnimClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("animation ready selects the Driver display"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\""))
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
            && Actor->GetConnectionStatus().Contains(TEXT("Connected")));
    const TArray<uint8> AnimFrame = PacketFor(FrameJsonFor(*TestSkeleton, FVector(11.0, 12.0, 13.0)));
    TestTrue(TEXT("animation frame is sent"), AnimClient && SendBytes(*AnimClient, AnimFrame.GetData(), AnimFrame.Num()));
    TestTrue(TEXT("animation frame reaches Live Link"),
        PollUntil([&]()
        {
            Source->Update();
            LiveLinkClient.ForceTick();
            double RootX = 0.0;
            return EvaluateRootX(RootX) && FMath::IsNearlyEqual(RootX, 11.0, 0.1);
        }));
    FPropertyEditorModule& PropertyEditor =
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
    TestTrue(TEXT("Details keeps the MtoU Preview customization registered for the Binding actor"),
        PropertyEditor.GetClassNameToDetailLayoutNameMap().Contains(
            AMtoULiveLinkActor::StaticClass()->GetFName()));
    // Execute the exact code the Details button invokes, including the
    // FScopedSlowTask/stage-callback path a direct RefreshActor call skips.
    FMtoULiveLinkActorDetails::HandleRefreshPreviewClicked(Actor);
    TestTrue(TEXT("Details click terminates the Animation session"), AnimClient && WaitForClose(*AnimClient));
    DestroySocket(AnimClient);
    TestTrue(TEXT("Details click returns source to listening"),
        PollUntil([&]()
        {
            Source->Update();
            return Source->GetSourceStatus().ToString().Contains(TEXT("Listening on"));
        }));
    TestTrue(TEXT("Details click shows Generated Preview while disconnected"),
        PollUntil([&]()
        {
            Source->Update();
            return Actor->GetConnectionStatus().Contains(TEXT("Disconnected"))
                && Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
                && Actor->GetPreviewReadiness().GeneratedPreview != nullptr
                && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Actor->GetPreviewReadiness().GeneratedPreview;
        }));
    FSocket* ReconnectClient = ConnectClient();
    TestNotNull(TEXT("animation reconnects explicitly after the Details click"), ReconnectClient);
    TestTrue(TEXT("reconnect init is sent"),
        ReconnectClient && SendBytes(*ReconnectClient, AnimInit.GetData(), AnimInit.Num()));
    Payload.Reset();
    TestTrue(TEXT("reconnect produces ready"), ReconnectClient && ReceiveOne(*ReconnectClient, Payload, [&]() { Source->Update(); }));
    TestTrue(TEXT("reconnect selects the Driver display"),
        FromUtf8(Payload).Contains(TEXT("\"type\":\"ready\""))
            && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
            && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    DestroySocket(ReconnectClient);
    return true;
}

#endif
