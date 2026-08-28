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
#include "PropertyEditorModule.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Selections/MeshConnectedComponents.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/ImportSubsystem.h"
#include "Subsystems/PlacementSubsystem.h"
#include "Tests/EnsureScope.h"
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

    DynamicMesh->EditMesh([Variant](UE::Geometry::FDynamicMesh3& Mesh)
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
            FName("PreviewDiagnostics"), FName("ModelDiagnostics"), FName("ModelDiagnosticLevel") })
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
    TestTrue(TEXT("binding selects the Driver display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver);
    TestTrue(TEXT("MtoU display component bypasses post-process animation"),
        Actor->GetSkeletalMeshComponent()->GetDisablePostProcessBlueprint());
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
    TestTrue(TEXT("disconnect preserves the Generated Preview display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::GeneratedPreview
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Second.GeneratedPreview);
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
    Actor->ShowDriverMesh();
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestEqual(TEXT("removing an input leaves the preview Dirty"),
        Actor->GetPreviewState(), EMtoUPreviewState::Dirty);
    TestTrue(TEXT("Binding input edits preserve the Driver display target"),
        Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
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
    TestTrue(TEXT("manual delete fixture has a ready preview"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).bSucceeded);
    TWeakObjectPtr<USkeletalMesh> DeletedMesh = Actor->GetGeneratedPreviewMesh();
    Actor->DeleteGeneratedPreview();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("manual delete releases the preview and restores the Driver"),
        !DeletedMesh.IsValid()
        && !Actor->HasReadyGeneratedPreview()
        && Actor->GetPreviewState() == EMtoUPreviewState::Dirty
        && Actor->GetDisplayTarget() == EMtoUDisplayTarget::Driver
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == Driver);
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

    FMtoUPreviewQualityThresholds UnsafeThresholds;
    UnsafeThresholds.MaxWarningInpaintRatio = Result.InpaintLowConfidenceRatio - 0.01;
    const FMtoUPreviewPreparationResult UnsafeResult = Actor
        ? FMtoUPreviewPreparation::Prepare(*Actor, *Binding, {}, UnsafeThresholds)
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
        FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
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
        const FMtoUPreviewPreparationResult Aligned =
            FMtoUPreviewPreparation::RefreshActor(*Actor);
        TestTrue(TEXT("the aligned input recovers with a complete preview"),
            Aligned.bSucceeded && Actor->HasReadyGeneratedPreview());
    }

    Binding->PreviewStaticMesh = MisalignedPreview;
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TWeakObjectPtr<USkeletalMesh> StaleMesh = Actor->GetGeneratedPreviewMesh();
    const FMtoUPreviewPreparationResult Failed =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("a misaligned Refresh transactionally discards and hides the stale preview"),
        !Failed.bSucceeded
        && Failed.GeneratedPreview == nullptr
        && Actor->GetPreviewState() == EMtoUPreviewState::Error
        && !StaleMesh.IsValid()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);

    Binding->PreviewStaticMesh = AlignedPreview;
    Binding->PostEditChangeProperty(PreviewInputChanged);
    TestTrue(TEXT("Refresh recovers after a misaligned attempt"),
        FMtoUPreviewPreparation::RefreshActor(*Actor).bSucceeded);

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

namespace
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
FAxisAlignedBox3d AppendPartCopy(
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
void SetUniformBoneWeights(FDynamicMesh3& Mesh)
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
USkeletalMesh* WriteTransientDriver(
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

bool AddBoxMorph(USkeletalMesh& Driver, FName Name,
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
bool MakeFullCharacterFixtures(UObject& Outer, FAutomationTestBase& Test,
    FMtoUFullCharacterFixtures& Fixtures)
{
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
    const FDynamicMesh3& Cube = BodySource->GetMeshRef();
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
        Test.AddError(TEXT("fixture garment-only Driver was not written"));
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
        AppendPartCopy(Mesh, Cube, GarmentUpperOffset, GarmentUpperScale, 0);
        AppendPartCopy(Mesh, Cube, GarmentLowerOffset, GarmentLowerScale, 1);
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
bool MakeGarmentFaultFixtures(UObject& Outer, FAutomationTestBase& Test,
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
}

namespace
{
// Mirrors the calibrated source-mass boundary documented in
// MtoULiveLinkPreview.cpp (MaxDriverToPreviewTriangleRatio); tests assert the
// resolved surface stays inside the same envelope rather than assuming exact
// Preview parity, since radius coverage legitimately admits nearby trim
// pieces. Keep the two values in sync when recalibrating.
constexpr double TestMaxSourceToPreviewRatio = 1.7;
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
    const FMtoUPreviewPreparationResult Result = FMtoUPreviewPreparation::Prepare(
        *Actor, *Binding,
        [&ObservedStages](EMtoUPreviewBuildStage Stage) { ObservedStages.Add(Stage); });
    AddInfo(Result.Diagnostics);
    AddInfo(Result.Summary);
    TestTrue(TEXT("full-character Driver builds a garment-only Generated Preview"),
        Result.bSucceeded && Result.GeneratedPreview != nullptr);
    TestEqual(TEXT("all five preparation stages remain observable"),
        ObservedStages.Num(), 5);
    TestTrue(TEXT("resolution reports full matched coverage"),
        Result.MatchedPreviewCoverage > 0.999);
    TestTrue(TEXT("the resolved garment stays within the calibrated source-mass envelope"),
        Result.GarmentSourceTriangleCount > 0
        && Result.GarmentSourceTriangleCount <= FMath::CeilToInt(
            TestMaxSourceToPreviewRatio * Fixtures.PreviewTriangleCount));
    TestTrue(TEXT("resolution names multiple disconnected regions"),
        Result.GarmentSourceRegionCount >= 2);
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

    // Model preview displays only the generated garment through explicit Refresh.
    const FMtoUPreviewPreparationResult RefreshResult =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TestTrue(TEXT("explicit Refresh readies the garment preview"),
        RefreshResult.bSucceeded
        && (Actor->GetPreviewState() == EMtoUPreviewState::Ready
            || Actor->GetPreviewState() == EMtoUPreviewState::Warning)
        && Actor->HasReadyGeneratedPreview());
    TestTrue(TEXT("Model preview displays only the Generated Preview"),
        Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()
            == RefreshResult.GeneratedPreview);

    // Reimport invalidates and requires explicit Refresh again.
    if (GEditor)
    {
        GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetReimport(Fixtures.Preview);
    }
    TestTrue(TEXT("source Reimport marks Dirty and hides the stale garment"),
        Actor->GetPreviewState() == EMtoUPreviewState::Dirty
        && !Actor->HasReadyGeneratedPreview()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);
    TestFalse(TEXT("normal Refresh creates no .uasset on the filesystem"),
        IFileManager::Get().FileExists(*PackageFilename));

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUPreviewGarmentResolutionTest,
    "MtoULiveLink.Editor.Preview.GarmentResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUPreviewGarmentResolutionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUGarmentResolutionWorld"));
    FMtoUFullCharacterFixtures Fixtures;
    TestTrue(TEXT("garment-resolution fixtures are created"),
        MakeFullCharacterFixtures(*WorldPackage, *this, Fixtures));
    if (!Fixtures.IsValid())
    {
        AddError(TEXT("garment-resolution fixtures were not created"));
        return false;
    }
    UWorld* World = UWorld::CreateWorld(
        EWorldType::EditorPreview, false, TEXT("MtoUGarmentResolutionWorld"), WorldPackage, true);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    if (!Actor || World == nullptr)
    {
        AddError(TEXT("garment-resolution world was not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }

    // Legacy compatibility: a garment-only Driver follows the same seam.
    Binding->SkeletalMesh = Fixtures.GarmentOnlyDriver;
    Binding->PreviewStaticMesh = Fixtures.Preview;
    Actor->SetBinding(Binding);
    const FMtoUPreviewPreparationResult Legacy = FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
    AddInfo(Legacy.Diagnostics);
    TestTrue(TEXT("legacy garment-only Driver resolves through the same seam"),
        Legacy.bSucceeded
        && Legacy.GeneratedPreview != nullptr
        && Legacy.MorphTargetCount == 1
        && Legacy.SkippedMorphTargetCount == 0
        && Legacy.MatchedPreviewCoverage > 0.999
        && Legacy.GarmentSourceTriangleCount > 0);

    // Material agreement alone cannot authorize the unrelated body: give the
    // Driver body slot the exact same slot name and material as a Preview slot.
    FSkeletalMaterial& BodyMaterial = Fixtures.FullDriver->GetMaterials()[0];
    BodyMaterial.MaterialSlotName = FName(TEXT("Cloth09_Top_1"));
    BodyMaterial.MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
    Binding->SkeletalMesh = Fixtures.FullDriver;
    FPropertyChangedEvent DriverPropertyChanged(FindFProperty<FProperty>(
        UMtoULiveLinkBinding::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UMtoULiveLinkBinding, SkeletalMesh)));
    Binding->PostEditChangeProperty(DriverPropertyChanged);
    const FMtoUPreviewPreparationResult Mimicry = FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
    AddInfo(Mimicry.Diagnostics);
    TestTrue(TEXT("matching material identity does not change geometric resolution"),
        Mimicry.bSucceeded
        && Mimicry.GeneratedPreview != nullptr
        && Mimicry.MatchedPreviewCoverage > 0.999);
    TestTrue(TEXT("material agreement cannot add mass beyond the calibrated envelope"),
        Mimicry.GarmentSourceTriangleCount > 0
        && Mimicry.GarmentSourceTriangleCount <= FMath::CeilToInt(
            TestMaxSourceToPreviewRatio * Fixtures.PreviewTriangleCount));
    TestNull(TEXT("attachment Morph stays excluded despite matching material identity"),
        Mimicry.GeneratedPreview
            ? Mimicry.GeneratedPreview->FindMorphTarget(FName(TEXT("ArmRaise")))
            : nullptr);
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
    bool bPersistedWithoutSectionIndices = RoundTrip != nullptr;
    for (const FName Entry : RoundTrip ? RoundTrip->DriverGarmentSlotOverride
                                       : TArray<FName>())
    {
        bPersistedWithoutSectionIndices &= !Entry.ToString().IsNumeric();
    }
    TestTrue(TEXT("persisted override identities are slot names rather than section indices"),
        bPersistedWithoutSectionIndices);
    UMtoULiveLinkBinding* Fresh = NewObject<UMtoULiveLinkBinding>(WorldPackage);
    TestEqual(TEXT("an empty override keeps automatic resolution"),
        Fresh->DriverGarmentSlotOverride.Num(), 0);

    // A valid multi-slot override selects every required section of the
    // multi-material garment and matches the safe automatic result.
    const FMtoUPreviewPreparationResult Manual =
        FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
    AddInfo(Manual.Diagnostics);
    TestTrue(TEXT("a valid multi-slot override builds the complete preview"),
        Manual.bSucceeded && Manual.GeneratedPreview != nullptr);
    TestTrue(TEXT("diagnostics distinguish manual from automatic selection"),
        Manual.bManualGarmentSource
        && Manual.Diagnostics.Contains(TEXT("Manual"))
        && Manual.Diagnostics.Contains(TEXT("manual slots")));
    TestTrue(TEXT("manual diagnostics list the resolved source identities"),
        Manual.Diagnostics.Contains(TEXT("Garment_Upper_A"))
        && Manual.Diagnostics.Contains(TEXT("Garment_Upper_B"))
        && Manual.Diagnostics.Contains(TEXT("Garment_Lower"))
        && Manual.Diagnostics.Contains(TEXT("matched Preview coverage")));
    TestTrue(TEXT("the manual result keeps full matched coverage"),
        Manual.MatchedPreviewCoverage > 0.999);
    TestTrue(TEXT("the manual surface stays inside the Preview garment"),
        Manual.GarmentSourceTriangleCount > 0
        && Manual.GarmentSourceTriangleCount <= Fixtures.PreviewTriangleCount);
    TestTrue(TEXT("the manual result projects only garment Morphs"),
        Manual.MorphTargetCount == 1 && Manual.SkippedMorphTargetCount == 3);
    TestTrue(TEXT("the manual Generated Preview keeps the complete Driver skeleton"),
        Manual.GeneratedPreview
        && Manual.GeneratedPreview->GetRefSkeleton().GetNum()
            == Fixtures.FullDriver->GetRefSkeleton().GetNum());

    // Changing the override invalidates and releases the previous preview
    // through the existing transactional lifecycle.
    Actor->SetConnectionStatus(TEXT("Disconnected"));
    const FMtoUPreviewPreparationResult Ready =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    TWeakObjectPtr<USkeletalMesh> StaleMesh = Ready.GeneratedPreview;
    TestTrue(TEXT("the ready override preview is displayed"),
        Ready.bSucceeded && Actor->HasReadyGeneratedPreview());
    TestTrue(TEXT("the actor keeps artist summary separate from technical diagnostics"),
        Actor->GetPreviewSummary() == Ready.Summary
        && Actor->GetPreviewSummary() != Actor->GetPreviewDiagnostics()
        && Actor->GetPreviewSummary().Contains(TEXT("Cloth09_Top_1"))
        && Actor->GetPreviewDiagnostics().Contains(TEXT("matched Preview coverage")));
    Binding->DriverGarmentSlotOverride = {UpperA};
    Binding->PostEditChangeProperty(OverrideChanged);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    TestTrue(TEXT("changing the override marks readiness Dirty and releases the stale preview"),
        Actor->GetPreviewState() == EMtoUPreviewState::Dirty
        && !Actor->HasReadyGeneratedPreview()
        && !StaleMesh.IsValid()
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);

    // A partial multi-slot selection cannot authorize unchecked closest-surface
    // transfer: geometric coverage validation rejects it transactionally.
    const FMtoUPreviewPreparationResult Partial =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    AddInfo(Partial.Diagnostics);
    TestTrue(TEXT("a partial override fails geometry coverage instead of transferring against the full character"),
        !Partial.bSucceeded
        && Partial.GeneratedPreview == nullptr
        && Partial.bManualGarmentSource
        && Partial.FailureStage == EMtoUPreviewBuildStage::GeometryConversion
        && Partial.Diagnostics.Contains(TEXT("whole Preview garment"))
        && Actor->GetPreviewState() == EMtoUPreviewState::Error
        && Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() == nullptr);

    // A missing persisted identity is a hard preflight error with actionable
    // diagnostics; Refresh never silently returns to Auto.
    Binding->DriverGarmentSlotOverride = {UpperA, UpperB, Lower,
        FName(TEXT("Clothes_09_Missing"))};
    Binding->PostEditChangeProperty(OverrideChanged);
    const FMtoUPreviewPreparationResult Missing =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    AddInfo(Missing.Diagnostics);
    TestTrue(TEXT("a missing override identity blocks refresh with an actionable diagnostic"),
        !Missing.bSucceeded
        && Missing.FailureStage == EMtoUPreviewBuildStage::GeometryConversion
        && Missing.Diagnostics.Contains(TEXT("Clothes_09_Missing"))
        && Missing.Diagnostics.Contains(TEXT("unknown Driver material slot"))
        && Missing.Diagnostics.Contains(TEXT("automatic resolution")));

    // A no-longer-unique persisted identity is a hard error as well.
    TArray<FSkeletalMaterial>& DriverMaterials = Fixtures.FullDriver->GetMaterials();
    const FName BodyName = DriverMaterials[0].MaterialSlotName;
    DriverMaterials[0].ImportedMaterialSlotName = Lower;
    DriverMaterials[0].MaterialSlotName = Lower;
    const FMtoUPreviewPreparationResult Duplicated =
        FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
    AddInfo(Duplicated.Diagnostics);
    DriverMaterials[0].ImportedMaterialSlotName = BodyName;
    DriverMaterials[0].MaterialSlotName = BodyName;
    TestTrue(TEXT("a duplicated override identity is a hard error naming both slots"),
        !Duplicated.bSucceeded
        && Duplicated.GeneratedPreview == nullptr
        && Duplicated.Diagnostics.Contains(TEXT("matches more than one")));

    // Driver Reimport invalidates readiness; a stale override keeps failing
    // until the artist clears it back to Auto.
    for (FSkeletalMaterial& Slot : DriverMaterials)
    {
        if (Slot.MaterialSlotName == UpperA || Slot.MaterialSlotName == UpperB
            || Slot.MaterialSlotName == Lower)
        {
            Slot.ImportedMaterialSlotName = *(Slot.MaterialSlotName.ToString() + TEXT("_Renamed"));
            Slot.MaterialSlotName = Slot.ImportedMaterialSlotName;
        }
    }
    if (GEditor)
    {
        GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetReimport(Fixtures.FullDriver);
    }
    TestTrue(TEXT("Driver Reimport marks readiness Dirty and hides the stale garment"),
        Actor->GetPreviewState() == EMtoUPreviewState::Dirty
        && !Actor->HasReadyGeneratedPreview());
    const FMtoUPreviewPreparationResult Stale =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    AddInfo(Stale.Diagnostics);
    TestTrue(TEXT("a stale override keeps failing after Reimport instead of returning to Auto"),
        !Stale.bSucceeded
        && Stale.GeneratedPreview == nullptr
        && Stale.Diagnostics.Contains(TEXT("unknown Driver material slot"))
        && Actor->GetPreviewState() == EMtoUPreviewState::Error);

    Binding->DriverGarmentSlotOverride.Reset();
    Binding->PostEditChangeProperty(OverrideChanged);
    TestTrue(TEXT("clearing the override leaves readiness Dirty"),
        Actor->GetPreviewState() == EMtoUPreviewState::Dirty
        && !Actor->HasReadyGeneratedPreview());
    const FMtoUPreviewPreparationResult AutoAgain =
        FMtoUPreviewPreparation::RefreshActor(*Actor);
    AddInfo(AutoAgain.Diagnostics);
    TestTrue(TEXT("clearing back to Auto recovers the safe automatic preview"),
        AutoAgain.bSucceeded
        && AutoAgain.GeneratedPreview != nullptr
        && !AutoAgain.bManualGarmentSource
        && AutoAgain.Diagnostics.Contains(TEXT("Auto")));

    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUGarmentFaultLinesTest,
    "MtoULiveLink.Editor.Preview.GarmentFaultLines",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUGarmentFaultLinesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMtoULiveLinkActor* Actor = World ? World->SpawnActor<AMtoULiveLinkActor>() : nullptr;
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>();
    TStrongObjectPtr<UMtoULiveLinkBinding> BindingGuard(Binding);
    if (!Actor || World == nullptr)
    {
        AddError(TEXT("fault-line world was not created"));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }

    // Exact duplicates remain equally plausible even when nearest-ownership
    // hands every Preview vertex to one copy. Refresh must report ambiguity
    // rather than turning deterministic tie order into a guessed selection.
    {
        UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUFaultDuplicate"));
        FMtoUGarmentFaultFixtures Fixtures;
        TestTrue(TEXT("duplicate-garment fixtures are created"),
            MakeGarmentFaultFixtures(*WorldPackage, *this,
                EMtoUGarmentFaultKind::DuplicateGarment, Fixtures));
        if (Fixtures.IsValid())
        {
            Binding->SkeletalMesh = Fixtures.Driver;
            Binding->PreviewStaticMesh = Fixtures.Preview;
            Binding->DriverGarmentSlotOverride.Reset();
            const FMtoUPreviewPreparationResult First =
                FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
            AddInfo(First.Diagnostics);
            TestTrue(TEXT("exact duplicate copies fail with stable ambiguity"),
                !First.bSucceeded
                && First.GeneratedPreview == nullptr
                && First.Diagnostics.Contains(TEXT("ambiguous")));
            const FMtoUPreviewPreparationResult Second =
                FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
            AddInfo(Second.Diagnostics);
            TestTrue(TEXT("the duplicate ambiguity diagnostic is deterministic across refreshes"),
                !Second.bSucceeded
                && Second.GeneratedPreview == nullptr
                && Second.Diagnostics == First.Diagnostics);
        }
    }

    // Near-twin shifted duplicates are also geometrically indistinguishable
    // inside the calibrated twin proximity even when one wins every ownership
    // tie, so Auto must fail consistently.
    {
        UPackage* WorldPackage = CreatePackage(TEXT("/Temp/MtoUFaultWelded"));
        FMtoUGarmentFaultFixtures Fixtures;
        TestTrue(TEXT("near-twin shifted fixtures are created"),
            MakeGarmentFaultFixtures(*WorldPackage, *this,
                EMtoUGarmentFaultKind::ShiftedDuplicateGarment, Fixtures));
        if (Fixtures.IsValid())
        {
            Binding->SkeletalMesh = Fixtures.Driver;
            Binding->PreviewStaticMesh = Fixtures.Preview;
            Binding->DriverGarmentSlotOverride.Reset();
            const FMtoUPreviewPreparationResult First =
                FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
            AddInfo(First.Diagnostics);
            const FMtoUPreviewPreparationResult Second =
                FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
            AddInfo(Second.Diagnostics);
            TestTrue(TEXT("near-twin shifted duplicates fail with deterministic ambiguity"),
                !First.bSucceeded && !Second.bSucceeded
                && First.GeneratedPreview == nullptr
                && Second.GeneratedPreview == nullptr
                && First.Diagnostics.Contains(TEXT("ambiguous"))
                && First.Diagnostics == Second.Diagnostics);
        }
    }

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
            FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
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

    // Metric normalization evidence: an isolated full-character Driver whose
    // non-garment parts sit far outside the agreement radius resolves exactly
    // the same two garment pieces as the legacy garment-only Driver, so a
    // whole-surface 0.1%-diagonal shift must yield identical normalized
    // metrics for both pairings despite different complete-character bounds.
    {
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
        const double FarShift =
            BodySource->GetMeshRef().GetBounds().DiagonalLength() * 6.0;
        FDynamicMesh3 IsolatedCharacter;
        TMap<int32, int32> Discard;
        AppendPartCopy(IsolatedCharacter, BodySource->GetMeshRef(),
            FVector3d(0.0, 0.0, -FarShift), 0.9, 0, 1, &Discard);
        // Identical garment-piece transforms as the legacy Driver fixtures.
        AppendPartCopy(IsolatedCharacter, BodySource->GetMeshRef(),
            FVector3d::Zero(), 1.15, 3, 2);
        AppendPartCopy(IsolatedCharacter, BodySource->GetMeshRef(),
            BodySource->GetMeshRef().GetBounds().Center()
                - FVector3d(0.0, 0.0,
                    BodySource->GetMeshRef().GetBounds().Height() * 1.05),
            0.55, 5);
        SetUniformBoneWeights(IsolatedCharacter);
        USkeletalMesh* IsolatedDriver = WriteTransientDriver(*WorldPackage,
            *Base, IsolatedCharacter,
            {FName(TEXT("Iso_Body")),
                FName(TEXT("Iso_Unused_A")),
                FName(TEXT("Iso_Unused_B")),
                FName(TEXT("Iso_Upper_A")),
                FName(TEXT("Iso_Upper_B")),
                FName(TEXT("Iso_Lower"))});
        TestNotNull(TEXT("isolated-character equivalence Driver was written"),
            IsolatedDriver);
        if (!Base || Outcome != EGeometryScriptOutcomePins::Success)
        {
            return false;
        }
        if (IsolatedDriver)
        {
            TStrongObjectPtr<UStaticMesh> ShiftedPreview(MakeCorpusPreview(
                *Fixtures.FullDriver, Fixtures.Preview, *WorldPackage,
                EMtoUCorpusVariant::SmallMisalignment));
            Binding->SkeletalMesh = IsolatedDriver;
            Binding->PreviewStaticMesh = ShiftedPreview.Get();
            const FMtoUPreviewPreparationResult Full =
                FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
            Binding->SkeletalMesh = Fixtures.GarmentOnlyDriver;
            const FMtoUPreviewPreparationResult Legacy =
                FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
            Binding->SkeletalMesh = Fixtures.FullDriver;
            AddInfo(Full.Diagnostics);
            AddInfo(Legacy.Diagnostics);
            AddInfo(FString::Printf(
                TEXT("normalization equivalence (isolated, 0.001 shift): full avg %.12f rms %.12f vs legacy avg %.12f rms %.12f"),
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
    }

    if (World)
    {
        World->DestroyWorld(false);
    }
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
            FMtoUPreviewPreparation::Prepare(*Actor, *Binding);
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

#endif
