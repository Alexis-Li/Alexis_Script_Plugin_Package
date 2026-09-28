// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#include "MtoUSceneRefFixture.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorScriptingUtilities/Public/EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Factories/MaterialFactoryNew.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelEditorSubsystem.h"
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

	/** A Blueprint actor with two mesh components, to cover Blueprint added components. */
	UClass* CreateMultiMeshBlueprint(const FString& PackagePath, TArray<FString>& OutNotes)
	{
		UPackage* Package = CreatePackage(*PackagePath);
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			TEXT("BP_SceneRefMulti"),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		if (Blueprint == nullptr)
		{
			OutNotes.Add(TEXT("blueprint actor: creation failed"));
			return nullptr;
		}

		USubobjectDataSubsystem* Subobjects = GEngine->GetEngineSubsystem<USubobjectDataSubsystem>();
		TArray<FSubobjectDataHandle> Handles;
		Subobjects->K2_GatherSubobjectDataForBlueprint(Blueprint, Handles);
		const FSubobjectDataHandle RootHandle = Handles.Num() > 0 ? Handles[0] : FSubobjectDataHandle::InvalidHandle;

		auto AddMeshComponent = [&](const TCHAR* MeshPath, const FTransform& RelativeTransform)
		{
			TSet<USCS_Node*> Before;
			for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				Before.Add(Node);
			}
			FAddNewSubobjectParams Params;
			Params.ParentHandle = RootHandle;
			Params.NewClass = UStaticMeshComponent::StaticClass();
			Params.BlueprintContext = Blueprint;
			FText FailReason;
			Subobjects->AddNewSubobject(Params, FailReason);
			for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Before.Contains(Node))
				{
					continue;
				}
				if (UStaticMeshComponent* Template = Cast<UStaticMeshComponent>(Node->ComponentTemplate))
				{
					Template->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, MeshPath));
					Template->SetRelativeTransform(RelativeTransform);
					if (UStaticMesh* Mesh = Template->GetStaticMesh())
					{
						Template->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, BasicMaterialPath));
					}
				}
				return;
			}
			OutNotes.Add(FString::Printf(TEXT("blueprint actor: component %s was not found after adding"), *FailReason.ToString()));
		};

		AddMeshComponent(CubeMeshPath, FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, 60.0), FVector(1.0, 1.0, 2.0)));
		AddMeshComponent(CylinderMeshPath, FTransform(FRotator(0.0, 30.0, 0.0), FVector(0.0, 0.0, 200.0), FVector(0.5)));

		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		Package->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
		return Blueprint->GeneratedClass;
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
}

const TCHAR* FMtoUSceneRefFixture::RootFolder() { return TEXT("/Game/MtoUSceneRefFixture"); }
const TCHAR* FMtoUSceneRefFixture::HostLevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Host"); }
const TCHAR* FMtoUSceneRefFixture::LoadedSublevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Loaded"); }
const TCHAR* FMtoUSceneRefFixture::UnloadedSublevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Unloaded"); }
const TCHAR* FMtoUSceneRefFixture::TexturedSublevelPackage() { return TEXT("/Game/MtoUSceneRefFixture/MtoUSceneRef_Textured"); }

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

bool FMtoUSceneRefFixture::Build(TArray<FString>& OutNotes, FString& OutError)
{
	ULevelEditorSubsystem* Levels = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (Levels == nullptr)
	{
		OutError = TEXT("no level editor subsystem");
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
	AddInstancedCluster(*HostLevel, TEXT("ISM_Cluster"),
		FTransform(FRotator(0.0, -25.0, 0.0), FVector(-200.0, -1200.0, 300.0), FVector(1.0)),
		{
			FTransform::Identity,
			FTransform(FRotator(0.0, 45.0, 0.0), FVector(150.0, 60.0, 0.0), FVector(1.0)),
			FTransform(FRotator(10.0, 0.0, 0.0), FVector(300.0, -40.0, 80.0), FVector(1.5, 0.5, 1.0)),
		});

	const UClass* BlueprintClass = CreateMultiMeshBlueprint(
		FString(RootFolder()) + TEXT("/BP_SceneRefMulti"), OutNotes);
	if (BlueprintClass != nullptr)
	{
		if (AActor* BlueprintActor = GEditor->AddActor(
			HostLevel,
			const_cast<UClass*>(BlueprintClass),
			FTransform(FRotator(0.0, 15.0, 0.0), FVector(400.0, 300.0, 0.0), FVector(1.0)),
			/*bSilent=*/true,
			RF_Transactional,
			/*bSelectActor=*/false))
		{
			BlueprintActor->SetActorLabel(TEXT("BP_SceneRefMulti"));
			BlueprintActor->SetActorTransform(
				FTransform(FRotator(0.0, 15.0, 0.0), FVector(400.0, 300.0, 0.0), FVector(1.0)),
				/*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
		}
		else
		{
			OutNotes.Add(TEXT("blueprint actor: placement failed"));
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
