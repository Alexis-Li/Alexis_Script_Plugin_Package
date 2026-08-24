#if WITH_DEV_AUTOMATION_TESTS

#include "MtoULiveLinkFactories.h"
#include "MtoULiveLinkPreview.h"

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
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/ImportSubsystem.h"
#include "Subsystems/PlacementSubsystem.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
UStaticMesh* MakePreview(
    USkeletalMesh& Driver,
    UObject& Outer,
    bool bLocalRetopology = false,
    bool bDoubleLayer = false,
    bool bRemovePositiveXSurface = false)
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

USkeletalMesh* MakeMorphDriver(UObject& Outer)
{
    USkeletalMesh* Base = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    USkeletalMesh* Driver = Base
        ? DuplicateObject<USkeletalMesh>(Base, &Outer)
        : nullptr;
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
        ? FMtoUPreviewPreparation::Prepare(*Actor, *Binding,
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
        ? FMtoUPreviewPreparation::Prepare(*Actor, *Binding,
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
        Actor->GetPreviewState() == EMtoUPreviewState::Dirty);
    TestFalse(TEXT("placement never invokes Refresh automatically"),
        Actor->HasReadyGeneratedPreview());
    TestTrue(TEXT("legacy Animation target remains visible before first Refresh"),
        Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    Actor->OnConstruction(Actor->GetActorTransform());
    TestFalse(TEXT("PostEdit construction never invokes Refresh automatically"),
        Actor->HasReadyGeneratedPreview());

    const FMtoUPreviewPreparationResult First =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    USkeletalMesh* FirstMesh = First.GeneratedPreview;
    TestTrue(TEXT("explicit Refresh commits one complete preview"),
        First.bSucceeded && Actor->HasReadyGeneratedPreview());
    TestTrue(TEXT("inpainted vertices become Warning rather than failure"),
        Actor->GetPreviewState() == EMtoUPreviewState::Ready
        || Actor->GetPreviewState() == EMtoUPreviewState::Warning);
    TestTrue(TEXT("successful Refresh displays the Generated Preview"),
        Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == FirstMesh);

    TWeakObjectPtr<USkeletalMesh> ReplacedMesh = FirstMesh;
    const FMtoUPreviewPreparationResult Second =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("repeated Refresh replaces the previous transient mesh"),
        Second.bSucceeded && Second.GeneratedPreview != FirstMesh);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("replacement releases the previous transient mesh"), ReplacedMesh.IsValid());
    TestTrue(TEXT("GC retains the actor-owned current preview"),
        Actor->HasReadyGeneratedPreview());

    AMtoULiveLinkActor* ReloadedActor = DuplicateObject<AMtoULiveLinkActor>(
        Actor, World->GetCurrentLevel());
    TestTrue(TEXT("level save/reload drops transient preview and requires Refresh"),
        ReloadedActor
        && !ReloadedActor->HasReadyGeneratedPreview()
        && ReloadedActor->GetPreviewState() == EMtoUPreviewState::Dirty
        && ReloadedActor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
    if (ReloadedActor)
    {
        ReloadedActor->Destroy();
    }

    Actor->SetConnectionStatus(TEXT("Disconnected"));
    TestTrue(TEXT("disconnect keeps the Generated Preview"),
        Actor->HasReadyGeneratedPreview());
    if (GEditor)
    {
        GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetReimport(Preview);
    }
    TestTrue(TEXT("Reimport marks Dirty and releases stale preview"),
        Actor->GetPreviewState() == EMtoUPreviewState::Dirty
        && !Actor->HasReadyGeneratedPreview()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);

    TestTrue(TEXT("Refresh can recover after Reimport invalidation"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).bSucceeded);
    Preview->OnPostMeshBuild().Broadcast(Preview);
    TestTrue(TEXT("source rebuild notification marks Dirty and hides stale preview"),
        Actor->GetPreviewState() == EMtoUPreviewState::Dirty
        && !Actor->HasReadyGeneratedPreview()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);
    TestTrue(TEXT("Refresh can recover after source rebuild invalidation"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).bSucceeded);

    Binding->PreviewStaticMesh = nullptr;
    FProperty* PreviewInputProperty = FindFProperty<FProperty>(
        UMtoULiveLinkBinding::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, PreviewStaticMesh));
    FPropertyChangedEvent PreviewInputChanged(PreviewInputProperty);
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestEqual(TEXT("removing an input leaves the preview Dirty"),
        Actor->GetPreviewState(), EMtoUPreviewState::Dirty);
    TestFalse(TEXT("Binding input changes release and hide stale preview"),
        Actor->HasReadyGeneratedPreview());
    const FMtoUPreviewPreparationResult Failed =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("failed Refresh is transactional"),
        !Failed.bSucceeded
        && Failed.GeneratedPreview == nullptr
        && Actor->GetPreviewState() == EMtoUPreviewState::Error
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);

    const FProperty* GeneratedProperty = FindFProperty<FProperty>(
        AMtoULiveLinkActor::StaticClass(), TEXT("GeneratedPreviewMesh"));
    TestTrue(TEXT("level save/reload cannot serialize the Generated Preview"),
        GeneratedProperty
        && GeneratedProperty->HasAllPropertyFlags(CPF_Transient | CPF_DuplicateTransient));

    Binding->PreviewStaticMesh = Preview;
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestTrue(TEXT("editor shutdown cleanup seam releases the preview"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).bSucceeded);
    TWeakObjectPtr<USkeletalMesh> ShutdownMesh = Actor->GetGeneratedPreviewMesh();
    Actor->ReleaseGeneratedPreview();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("editor shutdown releases transient preview data"),
        ShutdownMesh.IsValid());
    TestTrue(TEXT("final Refresh succeeds before actor destruction"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).bSucceeded);
    TWeakObjectPtr<USkeletalMesh> DestroyedMesh = Actor->GetGeneratedPreviewMesh();
    Actor->Destroy();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("actor destruction releases transient preview data"),
        DestroyedMesh.IsValid());

    AMtoULiveLinkActor* WorldUnloadActor = World->SpawnActor<AMtoULiveLinkActor>();
    WorldUnloadActor->SetBinding(Binding);
    TestTrue(TEXT("world-unload fixture has a ready preview"),
        FMtoUPreviewPreparation::RefreshActor(*WorldUnloadActor).bSucceeded);
    TWeakObjectPtr<USkeletalMesh> WorldUnloadMesh = WorldUnloadActor->GetGeneratedPreviewMesh();
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
        ? FMtoUPreviewPreparation::Prepare(*Actor, *Binding)
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
        ? FMtoUPreviewPreparation::Prepare(*Actor, *Binding)
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
    FMtoUPreviewPreparationResult FirstRefresh = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TWeakObjectPtr<USkeletalMesh> FirstLibrary = FirstRefresh.GeneratedPreview;
    FMtoUPreviewPreparationResult SecondRefresh = FMtoUPreviewPreparation::RefreshActor(*Actor);
    TWeakObjectPtr<USkeletalMesh> SecondLibrary = SecondRefresh.GeneratedPreview;
    TestTrue(TEXT("repeated Refresh replaces the complete transient Morph library"),
        FirstRefresh.bSucceeded && SecondRefresh.bSucceeded
        && FirstLibrary != SecondLibrary
        && FirstRefresh.MorphTargetCount == SecondRefresh.MorphTargetCount
        && FirstRefresh.SparseMorphDeltaCount == SecondRefresh.SparseMorphDeltaCount);
    FirstRefresh = FMtoUPreviewPreparationResult();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestFalse(TEXT("repeated Refresh releases the previous transient Morph library"),
        FirstLibrary.IsValid());

    Driver->GetMorphTargets().Add(NewObject<UMorphTarget>(
        Driver, FName(TEXT("BrokenRequired")), RF_Transient));
    FMtoUPreviewPreparationResult FailedRefresh = FMtoUPreviewPreparation::RefreshActor(*Actor);
    SecondRefresh = FMtoUPreviewPreparationResult();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("one required Morph failure transactionally discards and hides the preview"),
        !FailedRefresh.bSucceeded
        && FailedRefresh.GeneratedPreview == nullptr
        && FailedRefresh.FailureStage == EMtoUPreviewBuildStage::SkeletalMeshBuild
        && Actor->GetPreviewState() == EMtoUPreviewState::Error
        && !Actor->HasReadyGeneratedPreview()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr
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
    USkeletalMesh* Driver = MakeMorphDriver(*WorldPackage);
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
    if (Actor)
    {
        Actor->SetBinding(Binding);
    }

    const FMtoUPreviewPreparationResult Result = Actor
        ? FMtoUPreviewPreparation::RefreshActor(*Actor)
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
    TestTrue(TEXT("the Generated Preview remains visible with a warning"),
        Actor && Actor->GetPreviewState() == EMtoUPreviewState::Warning
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
            == Result.GeneratedPreview);

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
        ? FMtoUPreviewPreparation::Prepare(*Actor, *Binding)
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
        ? FMtoUPreviewPreparation::Prepare(*Actor, *Binding)
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
        ? FMtoUPreviewPreparation::Prepare(*Actor, *Binding)
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
        EAutomationExpectedErrorFlags::Contains, 2);
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
    }
    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

#endif
