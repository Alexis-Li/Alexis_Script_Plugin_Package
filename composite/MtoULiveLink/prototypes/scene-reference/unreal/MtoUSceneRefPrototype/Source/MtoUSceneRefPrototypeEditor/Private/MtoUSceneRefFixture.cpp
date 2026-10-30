// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#include "MtoUSceneRefFixture.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorScriptingUtilities/Public/EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/PointLight.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Factories/MaterialFactoryNew.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LevelEditorSubsystem.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SubobjectDataSubsystem.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* const CubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* const SphereMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	const TCHAR* const CylinderMeshPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
	const TCHAR* const ConeMeshPath = TEXT("/Engine/BasicShapes/Cone.Cone");
	const TCHAR* const BasicMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	const TCHAR* const GridTexturePath = TEXT("/Engine/EngineMaterials/T_Default_Material_Grid_M.T_Default_Material_Grid_M");

	/** The partition sample's fixed actors; every sample transform is written here. */
	const TCHAR* const PartitionSampleLabels[] = {
		TEXT("SM_Partition_A"),
		TEXT("SM_Partition_B"),
		TEXT("SM_Partition_C"),
	};
	const FTransform PartitionSampleTransforms[] = {
		FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, 0.0), FVector(1.0)),
		FTransform(FRotator(0.0, 25.0, 0.0), FVector(60000.0, 0.0, 0.0), FVector(2.0)),
		FTransform(FRotator(0.0, -40.0, 0.0), FVector(-60000.0, 60000.0, 0.0), FVector(1.5)),
	};
	const TCHAR* const NaniteSampleLabel = TEXT("SM_NaniteCube");
	const TCHAR* const InstanceSampleLabel = TEXT("LI_SceneRefInstance");
	const TCHAR* const LandscapeSampleLabel = TEXT("Landscape_Fixture");

	/** Every fixture transform is written here, so the sample is reproducible by reading this file. */
	AStaticMeshActor* AddMeshActor(
		ULevel& Level,
		const TCHAR* Label,
		const TCHAR* MeshPath,
		const TCHAR* MaterialPath,
		const FTransform& Transform)
	{
		AStaticMeshActor* Actor = Cast<AStaticMeshActor>(GEditor->AddActor(
			&Level,
			AStaticMeshActor::StaticClass(),
			Transform,
			/*bSilent=*/true,
			RF_Transactional,
			/*bSelectActor=*/false));
		if (Actor == nullptr)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		// AddActor applies the location and the rotation but not the scale, so the
		// sample's full transform is set here; the manifest then reports what it set.
		Actor->SetActorTransform(Transform, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, MeshPath));
		if (MaterialPath != nullptr)
		{
			Component->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, MaterialPath));
		}
		return Actor;
	}

	AActor* AddInstancedCluster(ULevel& Level, const TCHAR* Label, const FTransform& ActorTransform, const TArray<FTransform>& Worlds)
	{
		AActor* Actor = GEditor->AddActor(
			&Level,
			AActor::StaticClass(),
			ActorTransform,
			/*bSilent=*/true,
			RF_Transactional,
			/*bSelectActor=*/false);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		Actor->SetActorTransform(ActorTransform, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);

		UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(
			Actor, TEXT("Instances"), RF_Transactional);
		Actor->SetRootComponent(Instances);
		Instances->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, SphereMeshPath));
		Instances->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, BasicMaterialPath));
		Instances->RegisterComponent();
		Actor->AddInstanceComponent(Instances);
		for (const FTransform& World : Worlds)
		{
			Instances->AddInstance(World, /*bWorldSpace=*/true);
		}
		return Actor;
	}

	ULevelStreaming* AddStreamingLevel(UWorld& World, const FString& Package, bool bLoaded)
	{
		ULevelStreamingDynamic* Streaming = NewObject<ULevelStreamingDynamic>(
			&World, ULevelStreamingDynamic::StaticClass(), NAME_None, RF_NoFlags, nullptr);
		Streaming->SetWorldAssetByPackageName(FName(*Package));
		Streaming->LevelColor = FLinearColor::MakeRandomColor();
		Streaming->SetShouldBeLoaded(bLoaded);
		Streaming->SetShouldBeVisible(bLoaded);
		// An editor world loads a sublevel that is visible in the editor, so an unloaded
		// sublevel has to leave both switches off; that is the editor's own model.
		Streaming->SetShouldBeVisibleInEditor(bLoaded);
		World.AddStreamingLevel(Streaming);

		TArray<ULevelStreaming*> ToRefresh;
		ToRefresh.Add(Streaming);
		World.RefreshStreamingLevels(ToRefresh);
		return Streaming;
	}

	/**
	 * A material whose BaseColor comes from a texture sample parameter, so the export
	 * probe can observe what the engine writes for a texture driven material.
	 */
	UMaterialInterface* CreateTexturedMaterial(const FString& PackagePath, TArray<FString>& OutNotes)
	{
		UPackage* Package = CreatePackage(*PackagePath);
		if (Package == nullptr)
		{
			OutNotes.Add(TEXT("textured material: cannot create the package"));
			return nullptr;
		}
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		UMaterial* Material = Cast<UMaterial>(Factory->FactoryCreateNew(
			UMaterial::StaticClass(), Package, TEXT("M_MtoUSceneRefTextured"), RF_Public | RF_Standalone, nullptr, GWarn));
		if (Material == nullptr)
		{
			OutNotes.Add(TEXT("textured material: the material factory failed"));
			return nullptr;
		}
		UMaterialExpressionTextureSampleParameter2D* Sample = Cast<UMaterialExpressionTextureSampleParameter2D>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, 0));
		if (Sample == nullptr)
		{
			OutNotes.Add(TEXT("textured material: cannot add the texture sample"));
			return nullptr;
		}
		Sample->ParameterName = TEXT("BaseColorTexture");
		Sample->Texture = LoadObject<UTexture>(nullptr, GridTexturePath);
		if (!UMaterialEditingLibrary::ConnectMaterialProperty(Sample, TEXT("RGB"), MP_BaseColor))
		{
			OutNotes.Add(TEXT("textured material: cannot connect the sample to BaseColor"));
		}
		UMaterialEditingLibrary::RecompileMaterial(Material);
		FAssetRegistryModule::AssetCreated(Material);
		Package->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(Material->GetPathName(), /*bOnlyIfIsDirty=*/false);
		return Material;
	}

	/** The root a fixture Blueprint's components hang under. */
	FSubobjectDataHandle BlueprintRootHandle(UBlueprint& Blueprint)
	{
		USubobjectDataSubsystem* Subobjects = GEngine->GetEngineSubsystem<USubobjectDataSubsystem>();
		TArray<FSubobjectDataHandle> Handles;
		Subobjects->K2_GatherSubobjectDataForBlueprint(&Blueprint, Handles);
		return Handles.Num() > 0 ? Handles[0] : FSubobjectDataHandle::InvalidHandle;
	}

	/** The template of the component a subobject add produced, or nullptr with a note. */
	UActorComponent* AddBlueprintComponent(
		UBlueprint& Blueprint,
		const FSubobjectDataHandle& RootHandle,
		TSubclassOf<UActorComponent> NewClass,
		TArray<FString>& OutNotes)
	{
		TSet<USCS_Node*> Before;
		for (USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
		{
			Before.Add(Node);
		}

		USubobjectDataSubsystem* Subobjects = GEngine->GetEngineSubsystem<USubobjectDataSubsystem>();
		FAddNewSubobjectParams Params;
		Params.ParentHandle = RootHandle;
		Params.NewClass = NewClass;
		Params.BlueprintContext = &Blueprint;
		FText FailReason;
		Subobjects->AddNewSubobject(Params, FailReason);
		for (USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
		{
			if (!Before.Contains(Node))
			{
				return Node->ComponentTemplate;
			}
		}
		OutNotes.Add(FString::Printf(TEXT("blueprint %s: the %s component was not found after adding (%s)"),
			*Blueprint.GetName(), *NewClass->GetName(), *FailReason.ToString()));
		return nullptr;
	}

	/**
	 * A fixture Blueprint actor whose construction script Populate fills with one component
	 * per call; the samples differ only in their component mix.
	 */
	UClass* CreateFixtureBlueprint(
		const FString& PackagePath,
		const TCHAR* Name,
		TFunctionRef<void(UBlueprint&, const FSubobjectDataHandle&, TArray<FString>&)> Populate,
		TArray<FString>& OutNotes)
	{
		UPackage* Package = CreatePackage(*PackagePath);
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			Name,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		if (Blueprint == nullptr)
		{
			OutNotes.Add(FString::Printf(TEXT("blueprint %s: creation failed"), Name));
			return nullptr;
		}

		Populate(*Blueprint, BlueprintRootHandle(*Blueprint), OutNotes);

		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		Package->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
		return Blueprint->GeneratedClass;
	}

	/** Adds one static mesh component (mesh and material set) to a fixture Blueprint. */
	void AddBlueprintMeshComponent(
		UBlueprint& Blueprint,
		const FSubobjectDataHandle& RootHandle,
		const TCHAR* MeshPath,
		const FTransform& RelativeTransform,
		TArray<FString>& OutNotes)
	{
		if (UStaticMeshComponent* Template = Cast<UStaticMeshComponent>(AddBlueprintComponent(
			Blueprint, RootHandle, UStaticMeshComponent::StaticClass(), OutNotes)))
		{
			Template->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, MeshPath));
			Template->SetRelativeTransform(RelativeTransform);
			if (Template->GetStaticMesh() != nullptr)
			{
				Template->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, BasicMaterialPath));
			}
		}
	}

	/**
	 * An engine skeletal mesh the fixture can use for the skeletal sample.
	 *
	 * Engine content ships one; the asset registry fallback keeps the fixture working on an
	 * install that ships a different one. Without any, the sample is skipped with a note
	 * instead of failing the build.
	 */
	USkeletalMesh* FindFixtureSkeletalMesh(TArray<FString>& OutNotes)
	{
		const TCHAR* const KnownPaths[] = {
			TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"),
			TEXT("/Engine/EditorMeshes/SkeletalMesh/DefaultSkeletalMesh.DefaultSkeletalMesh"),
		};
		for (const TCHAR* Path : KnownPaths)
		{
			if (USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, Path))
			{
				return Mesh;
			}
		}

		TArray<FAssetData> Assets;
		FAssetRegistryModule::GetRegistry().GetAssetsByClass(
			USkeletalMesh::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/false);
		for (const FAssetData& Asset : Assets)
		{
			if (!Asset.PackagePath.ToString().StartsWith(TEXT("/Engine/")))
			{
				continue;
			}
			if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset()))
			{
				OutNotes.Add(FString::Printf(TEXT("mixed blueprint: the skeletal sample uses %s"),
					*Asset.GetObjectPathString()));
				return Mesh;
			}
		}

		OutNotes.Add(TEXT("mixed blueprint: no engine skeletal mesh asset exists, the skeletal sample is skipped"));
		return nullptr;
	}

	/** Places a fixture actor at a fixed transform and labels it. */
	AActor* PlaceFixtureActor(
		ULevel& Level,
		UClass& ActorClass,
		const TCHAR* Label,
		const FTransform& Transform,
		TArray<FString>& OutNotes)
	{
		AActor* Actor = GEditor->AddActor(
			&Level,
			&ActorClass,
			Transform,
			/*bSilent=*/true,
			RF_Transactional,
			/*bSelectActor=*/false);
		if (Actor == nullptr)
		{
			OutNotes.Add(FString::Printf(TEXT("actor %s: placement failed"), Label));
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		// AddActor applies the location and the rotation but not the scale, so the sample's
		// full transform is set here; the manifest then reports what it set.
		Actor->SetActorTransform(Transform, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
		return Actor;
	}

	bool BuildSublevel(
		ULevelEditorSubsystem& Levels,
		const FString& Package,
		TFunctionRef<void(ULevel&)> Populate,
		FString& OutError)
	{
		if (!Levels.NewLevel(Package))
		{
			OutError = FString::Printf(TEXT("cannot create the sublevel %s"), *Package);
			return false;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		ULevel* Level = World != nullptr ? World->GetCurrentLevel() : nullptr;
		if (Level == nullptr)
		{
			OutError = FString::Printf(TEXT("the sublevel %s has no current level"), *Package);
			return false;
		}
		Populate(*Level);
		if (!Levels.SaveCurrentLevel())
		{
			OutError = FString::Printf(TEXT("cannot save the sublevel %s"), *Package);
			return false;
		}
		return true;
	}

	/** The current level of the loaded editor world, or nullptr with OutError set. */
	ULevel* CurrentFixtureLevel(UWorld*& OutWorld, TArray<FString>& OutNotes, FString& OutError)
	{
		OutWorld = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
		ULevel* Level = OutWorld != nullptr ? OutWorld->GetCurrentLevel() : nullptr;
		if (Level == nullptr)
		{
			OutError = TEXT("the loaded fixture level has no current level");
		}
		return Level;
	}

	/**
	 * The Nanite sample mesh: the engine's basic cube duplicated under the fixture folder
	 * with `NaniteSettings.bEnabled` on.
	 *
	 * The engine's own asset is read-only content, so the fixture copies it instead of
	 * editing it; the copy is what the export measures, and it is what the read back in the
	 * test names (measured: `/Engine/BasicShapes/Cube` cannot be saved, so a Nanite flag set
	 * on it would not survive the run).
	 */
	UStaticMesh* CreateNaniteFixtureMesh(TArray<FString>& OutNotes)
	{
		const FString Destination = FMtoUSceneRefFixture::NaniteMeshPackage();
		UStaticMesh* Mesh = Cast<UStaticMesh>(
			UEditorAssetLibrary::DuplicateAsset(TEXT("/Engine/BasicShapes/Cube"), Destination));
		if (Mesh == nullptr)
		{
			OutNotes.Add(FString::Printf(
				TEXT("nanite sample: the engine cube could not be duplicated to %s, the sample is skipped"),
				*Destination));
			return nullptr;
		}

		FMeshNaniteSettings Settings = Mesh->GetNaniteSettings();
		Settings.bEnabled = true;
		Mesh->SetNaniteSettings(Settings);
		Mesh->PostEditChange();
		UEditorAssetLibrary::SaveAsset(Mesh->GetPathName(), /*bOnlyIfIsDirty=*/false);
		OutNotes.Add(FString::Printf(
			TEXT("nanite sample: %s is a copy of the engine cube, NaniteSettings.bEnabled=%s, IsNaniteEnabled()=%s"),
			*Destination,
			Mesh->GetNaniteSettings().bEnabled ? TEXT("true") : TEXT("false"),
			Mesh->IsNaniteEnabled() ? TEXT("true") : TEXT("false")));
		return Mesh;
	}

	/**
	 * The level instance sample: a saved source sublevel and a level that places one
	 * ALevelInstance over it.
	 *
	 * A level instance's world asset has to be a saved level package, so the sample saves the
	 * source sublevel first and the instance is what the transfer then has to report.
	 */
	bool BuildInstanceLevels(ULevelEditorSubsystem& Levels, TArray<FString>& OutNotes, FString& OutError)
	{
		if (!BuildSublevel(Levels, FMtoUSceneRefFixture::InstanceSourceLevelPackage(), [](ULevel& Level)
			{
				AddMeshActor(Level, TEXT("SM_InstanceSourceCube"), CubeMeshPath, BasicMaterialPath,
					FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, 50.0), FVector(1.0)));
				AddMeshActor(Level, TEXT("SM_InstanceSourceCylinder"), CylinderMeshPath, BasicMaterialPath,
					FTransform(FRotator(0.0, 0.0, 90.0), FVector(250.0, 0.0, 75.0), FVector(0.75)));
			}, OutError))
		{
			return false;
		}

		if (!Levels.NewLevel(FMtoUSceneRefFixture::InstanceHostLevelPackage()))
		{
			OutError = FString::Printf(TEXT("cannot create the level instance sample level %s"),
				FMtoUSceneRefFixture::InstanceHostLevelPackage());
			return false;
		}
		UWorld* World = nullptr;
		ULevel* Level = CurrentFixtureLevel(World, OutNotes, OutError);
		if (Level == nullptr)
		{
			return false;
		}

		ALevelInstance* Instance = Cast<ALevelInstance>(PlaceFixtureActor(
			*Level,
			*ALevelInstance::StaticClass(),
			InstanceSampleLabel,
			FTransform(FRotator::ZeroRotator, FVector(1000.0, 1000.0, 0.0), FVector(1.0)),
			OutNotes));
		if (Instance != nullptr)
		{
			const FString SourcePackage = FMtoUSceneRefFixture::InstanceSourceLevelPackage();
			const FSoftObjectPath WorldAssetPath(FString::Printf(TEXT("%s.%s"),
				*SourcePackage, *FPaths::GetBaseFilename(SourcePackage)));
			const TSoftObjectPtr<UWorld> WorldAsset(WorldAssetPath);
			FString Reason;
			if (!ULevelInstanceSubsystem::CanUseWorldAsset(Instance, WorldAsset, &Reason))
			{
				OutNotes.Add(FString::Printf(
					TEXT("level instance sample: the engine refuses the world asset %s (%s)"),
					*WorldAssetPath.ToString(), *Reason));
			}
			else if (!Instance->SetWorldAsset(WorldAsset))
			{
				OutNotes.Add(FString::Printf(
					TEXT("level instance sample: %s does not accept the world asset %s"),
					*Instance->GetActorLabel(), *WorldAssetPath.ToString()));
			}
			else
			{
				OutNotes.Add(FString::Printf(
					TEXT("level instance sample: %s places the saved sublevel %s"),
					*Instance->GetActorLabel(), *SourcePackage));
			}
		}

		if (!Levels.SaveCurrentLevel())
		{
			OutError = FString::Printf(TEXT("cannot save the level instance sample level %s"),
				FMtoUSceneRefFixture::InstanceHostLevelPackage());
			return false;
		}
		return true;
	}

	/**
	 * The landscape sample: one landscape component, built the way the engine's New
	 * Landscape tool builds one (spawn the actor, then `ALandscapeProxy::Import` the
	 * height data), so the engine's own landscape export branch has something to export.
	 */
	bool BuildLandscapeLevel(ULevelEditorSubsystem& Levels, TArray<FString>& OutNotes, FString& OutError)
	{
		if (!Levels.NewLevel(FMtoUSceneRefFixture::LandscapeLevelPackage()))
		{
			OutError = FString::Printf(TEXT("cannot create the landscape sample level %s"),
				FMtoUSceneRefFixture::LandscapeLevelPackage());
			return false;
		}
		UWorld* World = nullptr;
		if (CurrentFixtureLevel(World, OutNotes, OutError) == nullptr)
		{
			return false;
		}

		// ALandscape is not a placeable class (`GEditor->AddActor` refuses it: "Class Landscape
		// isn't placeable"), so the sample spawns it the way the engine's own New Landscape
		// tool does: SpawnActor, then Import the height data.
		ALandscape* Landscape = World->SpawnActor<ALandscape>(ALandscape::StaticClass(), FTransform::Identity);
		if (Landscape != nullptr)
		{
			Landscape->SetActorLabel(LandscapeSampleLabel);
			// One component with one section of 63 quads: 64 height samples per side, flat.
			const int32 NumSubsections = 1;
			const int32 SubsectionSizeQuads = 63;
			const int32 SizeX = NumSubsections * SubsectionSizeQuads + 1;
			const int32 SizeY = SizeX;

			TArray<uint16> HeightData;
			HeightData.Init(32768, SizeX * SizeY);
			TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
			HeightDataPerLayers.Add(FGuid(), MoveTemp(HeightData));
			TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
			MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

			Landscape->LandscapeMaterial = LoadObject<UMaterialInterface>(nullptr, BasicMaterialPath);
			Landscape->Import(
				FGuid::NewGuid(),
				/*InMinX=*/0, /*InMinY=*/0, /*InMaxX=*/SizeX - 1, /*InMaxY=*/SizeY - 1,
				NumSubsections, SubsectionSizeQuads,
				HeightDataPerLayers,
				/*InHeightmapFileName=*/TEXT(""),
				MaterialLayerDataPerLayers,
				ELandscapeImportAlphamapType::Additive,
				TArrayView<const FLandscapeLayer>());
			Landscape->RegisterAllComponents();

			OutNotes.Add(FString::Printf(
				TEXT("landscape sample: %s carries %d component(s) over a %dx%d heightfield, landscape info %s"),
				LandscapeSampleLabel,
				Landscape->LandscapeComponents.Num(),
				SizeX, SizeY,
				Landscape->GetLandscapeInfo() != nullptr ? TEXT("present") : TEXT("absent")));
		}

		if (!Levels.SaveCurrentLevel())
		{
			OutError = FString::Printf(TEXT("cannot save the landscape sample level %s"),
				FMtoUSceneRefFixture::LandscapeLevelPackage());
			return false;
		}
		return true;
	}

	/**
	 * The World Partition sample: a level built from the engine's own OpenWorld template.
	 *
	 * The template ships a 2 km landscape split into 128 streaming proxies, and the engine's
	 * FBX level exporter runs its own landscape branch over the whole terrain
	 * (`FFbxExporter::ExportLandscapeToFbx`), which would dominate this sample's export, so
	 * the template's landscape actors are removed here. The World Partition setup the
	 * template exists for (runtime hash, data layers, external actors) stays.
	 */
	bool BuildPartitionedLevel(ULevelEditorSubsystem& Levels, TArray<FString>& OutNotes, FString& OutError)
	{
		if (!Levels.NewLevelFromTemplate(
			FMtoUSceneRefFixture::PartitionedLevelPackage(),
			FMtoUSceneRefFixture::WorldPartitionTemplatePackage()))
		{
			OutError = FString::Printf(TEXT("cannot create the partitioned level %s from %s"),
				FMtoUSceneRefFixture::PartitionedLevelPackage(),
				FMtoUSceneRefFixture::WorldPartitionTemplatePackage());
			return false;
		}
		UWorld* World = nullptr;
		ULevel* Level = CurrentFixtureLevel(World, OutNotes, OutError);
		if (Level == nullptr)
		{
			return false;
		}
		if (World->GetWorldPartition() == nullptr)
		{
			OutNotes.Add(FString::Printf(
				TEXT("partitioned sample: %s carries no World Partition after the template copy"),
				FMtoUSceneRefFixture::PartitionedLevelPackage()));
		}

		TArray<AActor*> TemplateLandscape;
		for (AActor* Actor : Level->Actors)
		{
			if (Actor != nullptr && Actor->IsA<ALandscapeProxy>())
			{
				TemplateLandscape.Add(Actor);
			}
		}
		for (AActor* Actor : TemplateLandscape)
		{
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
		}
		OutNotes.Add(FString::Printf(
			TEXT("partitioned sample: %s removed the template's %d landscape actors and keeps its World Partition setup"),
			FMtoUSceneRefFixture::PartitionedLevelPackage(),
			TemplateLandscape.Num()));

		for (int32 Index = 0; Index < FMtoUSceneRefFixture::PartitionActorCount(); ++Index)
		{
			AddMeshActor(*Level, FMtoUSceneRefFixture::PartitionActorLabel(Index), CubeMeshPath, BasicMaterialPath,
				PartitionSampleTransforms[Index]);
		}

		if (!Levels.SaveCurrentLevel())
		{
			OutError = FString::Printf(TEXT("cannot save the partitioned level %s"),
				FMtoUSceneRefFixture::PartitionedLevelPackage());
			return false;
		}
		return true;
	}
}

const TCHAR* FMtoUSceneRefFixture::RootFolder() { return TEXT("/Game/MtoUSceneRefFixture"); }
const TCHAR* FMtoUSceneRefFixture::HostLevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Host"); }
const TCHAR* FMtoUSceneRefFixture::LoadedSublevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Loaded"); }
const TCHAR* FMtoUSceneRefFixture::UnloadedSublevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Unloaded"); }
const TCHAR* FMtoUSceneRefFixture::TexturedSublevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Textured"); }
const TCHAR* FMtoUSceneRefFixture::NaniteMeshPackage() { return TEXT("/Game/MtoUSceneRefFixture/SM_NaniteCube"); }
const TCHAR* FMtoUSceneRefFixture::InstanceSourceLevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_InstanceSource"); }
const TCHAR* FMtoUSceneRefFixture::InstanceHostLevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_InstanceHost"); }
const TCHAR* FMtoUSceneRefFixture::PartitionedLevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Partitioned"); }
const TCHAR* FMtoUSceneRefFixture::WorldPartitionTemplatePackage() { return TEXT("/Engine/Maps/Templates/OpenWorld"); }
const TCHAR* FMtoUSceneRefFixture::LandscapeLevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Landscape"); }

const TCHAR* FMtoUSceneRefFixture::NaniteActorLabel() { return NaniteSampleLabel; }
const TCHAR* FMtoUSceneRefFixture::InstanceActorLabel() { return InstanceSampleLabel; }
const TCHAR* FMtoUSceneRefFixture::LandscapeActorLabel() { return LandscapeSampleLabel; }

int32 FMtoUSceneRefFixture::PartitionActorCount() { return UE_ARRAY_COUNT(PartitionSampleLabels); }

const TCHAR* FMtoUSceneRefFixture::PartitionActorLabel(int32 Index)
{
	return PartitionSampleLabels[Index];
}

FMtoUSceneRefScopeSpec FMtoUSceneRefFixture::MainScope()
{
	FMtoUSceneRefScopeSpec Spec;
	Spec.LevelPackage = HostLevelPackage();
	Spec.RequestedSublevels.Add(LoadedSublevelPackage());
	Spec.RequestedSublevels.Add(UnloadedSublevelPackage());
	return Spec;
}

FMtoUSceneRefScopeSpec FMtoUSceneRefFixture::TexturedScope()
{
	FMtoUSceneRefScopeSpec Spec;
	Spec.LevelPackage = HostLevelPackage();
	Spec.RequestedSublevels.Add(TexturedSublevelPackage());
	return Spec;
}

FMtoUSceneRefScopeSpec FMtoUSceneRefFixture::InstanceScope()
{
	FMtoUSceneRefScopeSpec Spec;
	Spec.LevelPackage = InstanceHostLevelPackage();
	return Spec;
}

FMtoUSceneRefScopeSpec FMtoUSceneRefFixture::PartitionedScope()
{
	FMtoUSceneRefScopeSpec Spec;
	Spec.LevelPackage = PartitionedLevelPackage();
	return Spec;
}

FMtoUSceneRefScopeSpec FMtoUSceneRefFixture::LandscapeScope()
{
	FMtoUSceneRefScopeSpec Spec;
	Spec.LevelPackage = LandscapeLevelPackage();
	return Spec;
}

bool FMtoUSceneRefFixture::LoadFixtureLevel(const FString& Package, FString& OutError)
{
	ULevelEditorSubsystem* Levels = GEditor != nullptr ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
	if (Levels == nullptr)
	{
		OutError = TEXT("no level editor subsystem");
		return false;
	}
	if (const UWorld* World = GEditor->GetEditorWorldContext().World())
	{
		FString Loaded = World->GetOutermost()->GetName();
		Loaded.RemoveFromEnd(TEXT(".umap"));
		if (Loaded.Equals(Package, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	if (Levels->LoadLevel(Package))
	{
		return true;
	}
	OutError = FString::Printf(TEXT("cannot load the fixture level %s"), *Package);
	return false;
}

bool FMtoUSceneRefFixture::RestoreHostWorld(TArray<FString>& OutNotes, FString& OutError)
{
	ULevelEditorSubsystem* Levels = GEditor != nullptr ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
	if (Levels == nullptr)
	{
		OutError = TEXT("no level editor subsystem");
		return false;
	}
	if (!LoadFixtureLevel(HostLevelPackage(), OutError))
	{
		return false;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (World == nullptr)
	{
		OutError = TEXT("the restored host level has no world");
		return false;
	}

	// The host map carries the streaming entries it was saved with. A reload the fixture
	// itself performed brings them back; any that are missing are added here so the state a
	// sample test leaves behind is the state Build leaves behind.
	for (const TCHAR* Package : { LoadedSublevelPackage(), TexturedSublevelPackage() })
	{
		bool bPresent = false;
		for (ULevelStreaming* Streaming : World->GetStreamingLevels())
		{
			if (Streaming != nullptr && Streaming->GetWorldAssetPackageName() == Package)
			{
				bPresent = true;
				break;
			}
		}
		if (!bPresent)
		{
			AddStreamingLevel(*World, Package, /*bLoaded=*/true);
			OutNotes.Add(FString::Printf(TEXT("the host level lost its streaming entry for %s; it was added again"), Package));
		}
	}
	World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
	return true;
}

bool FMtoUSceneRefFixture::Build(TArray<FString>& OutNotes, FString& OutError)
{
	ULevelEditorSubsystem* Levels = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (Levels == nullptr)
	{
		OutError = TEXT("no level editor subsystem");
		return false;
	}

	// A fixture level that is the loaded editor world cannot be deleted from under
	// the editor: the delete would tear down a world the session still holds and
	// crash (measured: EXCEPTION_ACCESS_VIOLATION inside the rebuild). Building once
	// per session is what the tests and the acceptance runs do; a second build needs
	// a fresh editor session.
	const UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (EditorWorld != nullptr &&
		EditorWorld->GetOutermost()->GetName().StartsWith(RootFolder()))
	{
		OutError = FString::Printf(
			TEXT("the fixture is already loaded as the editor world (%s); rebuild it in a fresh "
				 "editor session instead, because deleting a loaded fixture level crashes the editor"),
			*EditorWorld->GetOutermost()->GetName());
		return false;
	}

	if (UEditorAssetLibrary::DoesDirectoryExist(RootFolder()))
	{
		if (!UEditorAssetLibrary::DeleteDirectory(RootFolder()))
		{
			OutError = FString::Printf(TEXT("cannot delete the previous fixture at %s"), RootFolder());
			return false;
		}
	}

	// Persistent level with the plain, rotated, non-uniformly scaled, instance and
	// Blueprint-actor samples, plus one actor that only exists to be reported.
	if (!Levels->NewLevel(HostLevelPackage()))
	{
		OutError = TEXT("cannot create the host level");
		return false;
	}

	// The Nanite sample mesh is an asset, not an actor: it has to exist before the host
	// level that places it is populated.
	UStaticMesh* NaniteMesh = CreateNaniteFixtureMesh(OutNotes);

	UWorld* HostWorld = GEditor->GetEditorWorldContext().World();
	ULevel* HostLevel = HostWorld != nullptr ? HostWorld->GetCurrentLevel() : nullptr;
	if (HostLevel == nullptr)
	{
		OutError = TEXT("the host level has no current level");
		return false;
	}

	AddMeshActor(*HostLevel, TEXT("SM_Pillar_Offset"), CubeMeshPath, BasicMaterialPath,
		FTransform(FRotator(0.0, 35.0, 10.0), FVector(1500.0, -800.0, 250.0), FVector(1.5)));
	AddMeshActor(*HostLevel, TEXT("SM_Plate_NonUniform"), CubeMeshPath, BasicMaterialPath,
		FTransform(FRotator(5.0, -20.0, 0.0), FVector(-900.0, 600.0, 60.0), FVector(2.0, 0.5, 0.25)));
	// The engine's own basic shapes are almost symmetric in their vertex distribution
	// (measured: every one of them has a local vertex centroid under 1 cm from its
	// pivot), so the mirror probe scales this sample up until its centroid offset is
	// several centimetres and a mirrored placement cannot hide inside the tolerance.
	AddMeshActor(*HostLevel, TEXT("SM_Cone_Asymmetric"), ConeMeshPath, BasicMaterialPath,
		FTransform(FRotator(-12.0, 40.0, 0.0), FVector(1200.0, 900.0, 150.0), FVector(5.0)));
	// The Nanite sample: the same engine cube geometry, but the placed asset is the copy with
	// Nanite enabled, so the transfer's render data path is measured on a Nanite mesh.
	if (NaniteMesh != nullptr)
	{
		AddMeshActor(*HostLevel, NaniteSampleLabel, *NaniteMesh->GetPathName(), BasicMaterialPath,
			FTransform(FRotator(0.0, 15.0, 0.0), FVector(-1500.0, 1200.0, 200.0), FVector(1.5)));
	}
	AddInstancedCluster(*HostLevel, TEXT("ISM_Cluster"),
		FTransform(FRotator(0.0, -25.0, 0.0), FVector(-200.0, -1200.0, 300.0), FVector(1.0)),
		{
			FTransform::Identity,
			FTransform(FRotator(0.0, 45.0, 0.0), FVector(150.0, 60.0, 0.0), FVector(1.0)),
			FTransform(FRotator(10.0, 0.0, 0.0), FVector(300.0, -40.0, 80.0), FVector(1.5, 0.5, 1.0)),
		});

	// Blueprint samples: the multi-mesh one keeps its two mesh nodes, the mixed one carries
	// one component per engine export branch the filter has to suppress.
	USkeletalMesh* SkeletalMesh = FindFixtureSkeletalMesh(OutNotes);

	UClass* MultiMeshClass = CreateFixtureBlueprint(
		FString(RootFolder()) + TEXT("/BP_SceneRefMulti"), TEXT("BP_SceneRefMulti"),
		[](UBlueprint& Blueprint, const FSubobjectDataHandle& RootHandle, TArray<FString>& Notes)
		{
			AddBlueprintMeshComponent(Blueprint, RootHandle, CubeMeshPath,
				FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, 60.0), FVector(1.0, 1.0, 2.0)), Notes);
			AddBlueprintMeshComponent(Blueprint, RootHandle, CylinderMeshPath,
				FTransform(FRotator(0.0, 30.0, 0.0), FVector(0.0, 0.0, 200.0), FVector(0.5)), Notes);
		},
		OutNotes);
	if (MultiMeshClass != nullptr)
	{
		PlaceFixtureActor(*HostLevel, *MultiMeshClass, TEXT("BP_SceneRefMulti"),
			FTransform(FRotator(0.0, 15.0, 0.0), FVector(400.0, 300.0, 0.0), FVector(1.0)), OutNotes);
	}

	// The child class owns a sphere the file must never carry: it only reaches the export
	// through the mixed actor's child actor component, which the filter suppresses.
	UClass* ChildMeshClass = CreateFixtureBlueprint(
		FString(RootFolder()) + TEXT("/BP_SceneRefChildMesh"), TEXT("BP_SceneRefChildMesh"),
		[](UBlueprint& Blueprint, const FSubobjectDataHandle& RootHandle, TArray<FString>& Notes)
		{
			AddBlueprintMeshComponent(Blueprint, RootHandle, SphereMeshPath, FTransform::Identity, Notes);
		},
		OutNotes);

	UClass* MixedClass = CreateFixtureBlueprint(
		FString(RootFolder()) + TEXT("/BP_SceneRefMixed"), TEXT("BP_SceneRefMixed"),
		[ChildMeshClass, SkeletalMesh](UBlueprint& Blueprint, const FSubobjectDataHandle& RootHandle, TArray<FString>& Notes)
		{
			// The one static mesh component the filter keeps. It sits at the actor's own
			// transform on purpose: with a single exported component the engine merges the
			// component transform into the actor node only when the actor has no attached
			// actors, and the mixed actor always has its child actor attached.
			AddBlueprintMeshComponent(Blueprint, RootHandle, CubeMeshPath, FTransform::Identity, Notes);
			if (UPointLightComponent* LightTemplate = Cast<UPointLightComponent>(AddBlueprintComponent(
				Blueprint, RootHandle, UPointLightComponent::StaticClass(), Notes)))
			{
				LightTemplate->SetRelativeLocation(FVector(150.0, 0.0, 300.0));
			}
			if (UCameraComponent* CameraTemplate = Cast<UCameraComponent>(AddBlueprintComponent(
				Blueprint, RootHandle, UCameraComponent::StaticClass(), Notes)))
			{
				CameraTemplate->SetRelativeLocation(FVector(-150.0, 0.0, 200.0));
			}
			if (UChildActorComponent* ChildTemplate = Cast<UChildActorComponent>(AddBlueprintComponent(
				Blueprint, RootHandle, UChildActorComponent::StaticClass(), Notes)))
			{
				ChildTemplate->SetRelativeLocation(FVector(200.0, 200.0, 0.0));
				ChildTemplate->SetChildActorClass(ChildMeshClass);
			}
			if (SkeletalMesh != nullptr)
			{
				if (USkeletalMeshComponent* SkeletalTemplate = Cast<USkeletalMeshComponent>(AddBlueprintComponent(
					Blueprint, RootHandle, USkeletalMeshComponent::StaticClass(), Notes)))
				{
					SkeletalTemplate->SetSkeletalMeshAsset(SkeletalMesh);
					SkeletalTemplate->SetRelativeLocation(FVector(0.0, -200.0, 0.0));
				}
			}
		},
		OutNotes);
	if (MixedClass != nullptr)
	{
		if (PlaceFixtureActor(*HostLevel, *MixedClass, TEXT("BP_SceneRefMixed"),
			FTransform(FRotator(0.0, 105.0, 0.0), FVector(-600.0, -450.0, 0.0), FVector(1.0)), OutNotes) != nullptr)
		{
			OutNotes.Add(FString::Printf(
				TEXT("BP_SceneRefMixed carries the filter sample: a static mesh that stays, and a light, a camera, a child actor (%s) and %s that the export suppresses"),
				ChildMeshClass != nullptr ? TEXT("BP_SceneRefChildMesh") : TEXT("no child class"),
				SkeletalMesh != nullptr ? TEXT("a skeletal mesh") : TEXT("no skeletal mesh component")));
		}
	}

	if (APointLight* Light = Cast<APointLight>(GEditor->AddActor(
		HostLevel,
		APointLight::StaticClass(),
		FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, 400.0), FVector(1.0)),
		/*bSilent=*/true,
		RF_Transactional,
		/*bSelectActor=*/false)))
	{
		Light->SetActorLabel(TEXT("Light_ReportedOnly"));
	}

	if (!Levels->SaveCurrentLevel())
	{
		OutError = TEXT("cannot save the host level");
		return false;
	}

	// Loaded sublevel: plain static meshes in a second level package.
	if (!BuildSublevel(*Levels, LoadedSublevelPackage(), [](ULevel& Level)
		{
			AddMeshActor(Level, TEXT("SM_SublevelCube"), CubeMeshPath, BasicMaterialPath,
				FTransform(FRotator(0.0, 45.0, 0.0), FVector(300.0, 1200.0, 100.0), FVector(1.0)));
			AddMeshActor(Level, TEXT("SM_SublevelCylinder"), CylinderMeshPath, BasicMaterialPath,
				FTransform(FRotator(0.0, 0.0, 90.0), FVector(-150.0, 900.0, 50.0), FVector(0.75, 0.75, 1.25)));
		}, OutError))
	{
		return false;
	}

	// Unloaded sublevel: exists and is requested, but must never reach the file.
	if (!BuildSublevel(*Levels, UnloadedSublevelPackage(), [](ULevel& Level)
		{
			AddMeshActor(Level, TEXT("SM_UnloadedCube"), CubeMeshPath, BasicMaterialPath,
				FTransform(FRotator::ZeroRotator, FVector(5000.0, 5000.0, 0.0), FVector(1.0)));
		}, OutError))
	{
		return false;
	}

	// Texture probe sublevel: a material whose BaseColor is driven by a texture.
	UMaterialInterface* TexturedMaterial = CreateTexturedMaterial(
		FString(RootFolder()) + TEXT("/M_MtoUSceneRefTextured"), OutNotes);
	if (!BuildSublevel(*Levels, TexturedSublevelPackage(), [TexturedMaterial](ULevel& Level)
		{
			if (AStaticMeshActor* Actor = AddMeshActor(
				Level,
				TEXT("SM_TexturedSphere"),
				SphereMeshPath,
				/*MaterialPath=*/nullptr,
				FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, 100.0), FVector(1.0))))
			{
				if (TexturedMaterial != nullptr)
				{
					Actor->GetStaticMeshComponent()->SetMaterial(0, TexturedMaterial);
				}
			}
		}, OutError))
	{
		return false;
	}

	// Level instance sample: a saved source sublevel plus the level that places an instance
	// of it. A level instance cannot be exported, so this sample exists to be reported.
	if (!BuildInstanceLevels(*Levels, OutNotes, OutError))
	{
		return false;
	}

	// Landscape sample: one component, built the way the engine's New Landscape tool builds
	// one, so the engine's own landscape export branch has geometry to write.
	if (!BuildLandscapeLevel(*Levels, OutNotes, OutError))
	{
		return false;
	}

	// World Partition sample: the engine's own OpenWorld template, with the sample actors.
	if (!BuildPartitionedLevel(*Levels, OutNotes, OutError))
	{
		return false;
	}

	// Reload the host and stream the sublevels in the state the scope semantics need.
	if (!Levels->LoadLevel(HostLevelPackage()))
	{
		OutError = TEXT("cannot reload the host level");
		return false;
	}
	UWorld* ReloadedWorld = GEditor->GetEditorWorldContext().World();
	if (ReloadedWorld == nullptr)
	{
		OutError = TEXT("the reloaded host level has no world");
		return false;
	}
	AddStreamingLevel(*ReloadedWorld, LoadedSublevelPackage(), /*bLoaded=*/true);
	AddStreamingLevel(*ReloadedWorld, TexturedSublevelPackage(), /*bLoaded=*/true);

	// The unloaded sublevel is deliberately NOT added to the world. An editor world loads
	// every streaming level it owns (ULevelStreaming::DetermineTargetState returns
	// LoadedNotVisible for a non-game world), so "requested but not loaded" is represented
	// by a saved level package the loaded world does not hold. The scope has to report it.
	ReloadedWorld->FlushLevelStreaming(EFlushLevelStreamingType::Full);
	OutNotes.Add(FString::Printf(
		TEXT("%s exists on disk and stays out of the loaded world: requesting it is the unloaded-content case"),
		UnloadedSublevelPackage()));

	if (!Levels->SaveCurrentLevel())
	{
		// The sublevels were saved as they were built; only the host's streaming entries are new.
		OutNotes.Add(TEXT("the host level could not be re-saved after the streaming setup"));
	}
	return true;
}
