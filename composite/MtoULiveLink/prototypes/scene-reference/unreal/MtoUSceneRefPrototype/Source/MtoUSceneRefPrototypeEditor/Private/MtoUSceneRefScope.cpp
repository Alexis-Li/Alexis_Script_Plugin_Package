// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#include "MtoUSceneRefScope.h"

#include "Camera/CameraComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "LandscapeProxy.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "StaticMeshResources.h"
#include "WorldPartition/WorldPartition.h"

namespace
{
	FString NormalizePackage(const FString& Package)
	{
		FString Value = Package;
		Value.TrimStartAndEndInline();
		Value.RemoveFromEnd(TEXT(".umap"));
		return Value;
	}

	bool SamePackage(const FString& Left, const FString& Right)
	{
		return NormalizePackage(Left).Equals(NormalizePackage(Right), ESearchCase::IgnoreCase);
	}

	/**
	 * A static mesh component the engine's `ExportActor` turns into a node: it must carry
	 * a mesh and must not be hidden. Every other branch (skeletal mesh, camera, light,
	 * child actor) is suppressed before the export, so it contributes no node.
	 */
	bool IsExportedStaticMeshComponent(const UStaticMeshComponent& Component)
	{
		return !Component.bHiddenInGame && Component.GetStaticMesh() != nullptr;
	}

	int32 CountExportComponents(const AActor& Actor)
	{
		int32 Count = 0;
		for (UActorComponent* ActorComponent : Actor.GetComponents())
		{
			if (const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(ActorComponent))
			{
				if (IsExportedStaticMeshComponent(*MeshComponent))
				{
					++Count;
				}
			}
		}
		return Count;
	}

	FString CategorySuffixForInstance(int32 InstanceIndex)
	{
		return InstanceIndex == INDEX_NONE
			? FString()
			: FString::Printf(TEXT(":instance:%d"), InstanceIndex);
	}
}

FString FMtoUSceneRefScopeSpec::ScopeName() const
{
	const FString Leaf = FPaths::GetBaseFilename(NormalizePackage(LevelPackage));
	return Leaf.IsEmpty() ? TEXT("Level") : Leaf;
}

int32 FMtoUSceneRefResolution::ExportedObjectCount() const
{
	int32 Count = 0;
	for (const FMtoUSceneRefObject& Object : Objects)
	{
		if (Object.bExported)
		{
			++Count;
		}
	}
	return Count;
}

const TCHAR* FMtoUSceneRefScope::CategoryName(EMtoUSceneRefCategory Category)
{
	switch (Category)
	{
	case EMtoUSceneRefCategory::StaticMesh: return TEXT("static_mesh");
	case EMtoUSceneRefCategory::InstancedStaticMesh: return TEXT("instanced_static_mesh");
	case EMtoUSceneRefCategory::Landscape: return TEXT("landscape");
	case EMtoUSceneRefCategory::LevelInstance: return TEXT("level_instance");
	case EMtoUSceneRefCategory::Unsupported: return TEXT("unsupported");
	case EMtoUSceneRefCategory::SkippedNonGeometry: return TEXT("skipped_non_geometry");
	default: return TEXT("unknown");
	}
}

void FMtoUSceneRefScope::ReportSkipped(
	const AActor& Actor,
	const FString& Reason,
	bool bUnsupported,
	FMtoUSceneRefResolution& Out)
{
	FMtoUSceneRefSkipped Entry;
	Entry.ActorLabel = Actor.GetActorLabel();
	Entry.ActorClass = Actor.GetClass()->GetName();
	Entry.Reason = Reason;
	(bUnsupported ? Out.Unsupported : Out.Skipped).Add(Entry);
}

const TCHAR* FMtoUSceneRefScope::SuppressionReason(const USceneComponent& Component)
{
	// A component the caller hid is out of the file with or without this transfer, so it
	// is not a suppression this run performs.
	if (Component.bHiddenInGame || Component.IsA<UStaticMeshComponent>())
	{
		return nullptr;
	}
	// The order mirrors FFbxExporter::ExportActor's own branch order (FbxMainExport.cpp),
	// so a reason always names the engine branch this component would have reached.
	if (const USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>(&Component))
	{
		return SkeletalComponent->GetSkeletalMeshAsset() != nullptr ? TEXT("skeletal mesh component") : nullptr;
	}
	if (Component.IsA<UCameraComponent>())
	{
		return TEXT("camera component");
	}
	if (Component.IsA<ULightComponent>())
	{
		return TEXT("light component");
	}
	if (const UChildActorComponent* ChildActorComponent = Cast<UChildActorComponent>(&Component))
	{
		return ChildActorComponent->GetChildActor() != nullptr ? TEXT("child actor component") : nullptr;
	}
	return nullptr;
}

void FMtoUSceneRefScope::CollectSuppressed(const AActor& Actor, const FString& LevelPackage, FMtoUSceneRefResolution& Out)
{
	for (UActorComponent* ActorComponent : Actor.GetComponents())
	{
		USceneComponent* SceneComponent = Cast<USceneComponent>(ActorComponent);
		if (SceneComponent == nullptr)
		{
			continue;
		}
		const TCHAR* Reason = SuppressionReason(*SceneComponent);
		if (Reason == nullptr)
		{
			continue;
		}

		FMtoUSceneRefSuppressedComponent Record;
		Record.ActorLabel = Actor.GetActorLabel();
		Record.ActorClass = Actor.GetClass()->GetName();
		Record.ComponentName = SceneComponent->GetName();
		Record.ComponentClass = SceneComponent->GetClass()->GetName();
		Record.Reason = Reason;
		Record.LevelPackage = LevelPackage;
		Out.SuppressedComponents.Add(Record);
		Out.SuppressedComponentInstances.Add(SceneComponent);
	}
}

void FMtoUSceneRefScope::CollectObject(
	const AActor& Actor,
	const FString& LevelPackage,
	bool bActorHasMultipleExportComponents,
	const FString& ActorNodeName,
	UStaticMeshComponent& Component,
	FMtoUSceneRefResolution& Out)
{
	const UStaticMesh* Mesh = Component.GetStaticMesh();
	if (Mesh == nullptr)
	{
		return;
	}

	const UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(&Component);
	const int32 InstanceCount = InstancedComponent != nullptr ? InstancedComponent->GetInstanceCount() : 1;
	const FBox MeshBounds = Mesh->GetBounds().GetBox();

	for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
	{
		FMtoUSceneRefObject Object;
		Object.ActorLabel = Actor.GetActorLabel();
		Object.ActorClass = Actor.GetClass()->GetName();
		Object.LevelPackage = LevelPackage;
		Object.ComponentName = Component.GetName();
		Object.MeshPath = Mesh->GetPathName();
		Object.bExported = true;

		FTransform InstanceTransform = Component.GetComponentTransform();
		if (InstancedComponent != nullptr)
		{
			Object.InstanceIndex = InstanceIndex;
			Object.Category = EMtoUSceneRefCategory::InstancedStaticMesh;
			if (!InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, /*bWorldSpace=*/true))
			{
				Object.bExported = false;
				Object.Note = TEXT("instance transform unavailable");
			}
			Object.NodeName = FString::Printf(TEXT("%d"), InstanceIndex);
		}
		else
		{
			Object.Category = EMtoUSceneRefCategory::StaticMesh;
			Object.NodeName = bActorHasMultipleExportComponents ? Component.GetName() : ActorNodeName;
		}

		Object.WorldTransform = InstanceTransform;
		Object.WorldBounds = MeshBounds.TransformBy(InstanceTransform);
		if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
		{
			if (RenderData->LODResources.Num() > 0)
			{
				const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
				Object.Triangles = LOD.GetNumTriangles();
				Object.Vertices = LOD.GetNumVertices();

				// Area weighted surface centroid: the only sample of the geometry that a
				// mirrored placement changes while position, bounds and pivot offset stay
				// equal, and the only one the exported file can reproduce (the file welds
				// duplicated render vertices away).
				const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
				FVector WeightedSum = FVector::ZeroVector;
				double TotalArea = 0.0;
				const int32 TriangleCount = LOD.GetNumTriangles();
				for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
				{
					const FVector Corners[3] = {
						InstanceTransform.TransformPosition(FVector(Positions.VertexPosition(LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 0)))),
						InstanceTransform.TransformPosition(FVector(Positions.VertexPosition(LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 1)))),
						InstanceTransform.TransformPosition(FVector(Positions.VertexPosition(LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 2)))),
					};
					const double Area = 0.5 * ((Corners[1] - Corners[0]).Cross(Corners[2] - Corners[0])).Size();
					if (Area <= 0.0)
					{
						continue;
					}
					WeightedSum += Area * (Corners[0] + Corners[1] + Corners[2]) / 3.0;
					TotalArea += Area;
				}
				if (TotalArea > 0.0)
				{
					Object.WorldSurfaceCentroid = WeightedSum / TotalArea;
				}
			}
		}
		for (int32 MaterialIndex = 0; MaterialIndex < Component.GetNumMaterials(); ++MaterialIndex)
		{
			const UMaterialInterface* Material = Component.GetMaterial(MaterialIndex);
			Object.Materials.Add(Material != nullptr ? Material->GetPathName() : FString(TEXT("None")));
		}

		Object.Id = FString::Printf(
			TEXT("%s:%s:component:%s%s"),
			*Actor.GetClass()->GetName(),
			*Actor.GetActorLabel(),
			*Component.GetName(),
			*CategorySuffixForInstance(Object.InstanceIndex));

		Out.Objects.Add(Object);
	}
}

bool FMtoUSceneRefScope::Resolve(UWorld& World, const FMtoUSceneRefScopeSpec& Spec, FMtoUSceneRefResolution& Out)
{
	Out.PersistentLevelPackage = World.GetOutermost() != nullptr ? World.GetOutermost()->GetName() : FString();
	Out.ScopeName = Spec.ScopeName();

	if (!SamePackage(World.GetOutermost()->GetName(), Spec.LevelPackage))
	{
		Out.Error = FString::Printf(
			TEXT("scope level %s is not the loaded editor world (%s); the transfer never switches or loads levels on its own"),
			*Spec.LevelPackage,
			*Out.PersistentLevelPackage);
		return false;
	}

#if WITH_EDITORONLY_DATA
	Out.bWorldPartition = World.GetWorldPartition() != nullptr;
#endif

	// Inventory the loaded levels and the requested sublevels.
	TSet<FString> LevelsInWorld;
	for (int32 LevelIndex = 0; LevelIndex < World.GetNumLevels(); ++LevelIndex)
	{
		if (const ULevel* Level = World.GetLevel(LevelIndex))
		{
			LevelsInWorld.Add(Level->GetOutermost()->GetName());
		}
	}

	TSet<FString> Requested;
	for (const FString& RequestedPackage : Spec.RequestedSublevels)
	{
		Requested.Add(NormalizePackage(RequestedPackage));
	}

	TSet<FString> RequestedSeen;
	for (ULevelStreaming* Streaming : World.GetStreamingLevels())
	{
		if (Streaming == nullptr)
		{
			continue;
		}
		const FString Package = Streaming->GetWorldAssetPackageName();
		const bool bRequested = Requested.Contains(NormalizePackage(Package));
		if (bRequested)
		{
			RequestedSeen.Add(NormalizePackage(Package));
		}
		if (Streaming->IsLevelLoaded())
		{
			if (bRequested)
			{
				Out.LoadedSublevels.Add(Package);
			}
			else
			{
				Out.ExcludedSublevels.Add(Package);
			}
			continue;
		}
		if (bRequested)
		{
			FMtoUSceneRefUnloadedLevel Entry;
			Entry.Package = Package;
			Entry.StreamingState = Streaming->HasLoadRequestPending() ? TEXT("load_pending") : TEXT("not_loaded");
			Entry.bShouldBeVisible = Streaming->GetShouldBeVisibleFlag();
			Out.UnloadedSublevels.Add(Entry);
		}
		else
		{
			Out.ExcludedSublevels.Add(Package);
		}
	}

	for (const FString& RequestedPackage : Spec.RequestedSublevels)
	{
		if (RequestedSeen.Contains(NormalizePackage(RequestedPackage)))
		{
			continue;
		}
		FMtoUSceneRefUnloadedLevel Entry;
		Entry.Package = RequestedPackage;
		Entry.StreamingState = TEXT("not_in_world");
		Out.UnloadedSublevels.Add(Entry);
		++Out.RequestedSublevelsMissing;
	}

	// Collect the actors of the in-scope levels.
	TArray<FString> ScopeLevelPackages;
	ScopeLevelPackages.Add(NormalizePackage(Out.PersistentLevelPackage));
	for (const FString& Loaded : Out.LoadedSublevels)
	{
		ScopeLevelPackages.Add(NormalizePackage(Loaded));
	}

	for (int32 LevelIndex = 0; LevelIndex < World.GetNumLevels(); ++LevelIndex)
	{
		ULevel* Level = World.GetLevel(LevelIndex);
		if (Level == nullptr)
		{
			continue;
		}
		const FString LevelPackage = Level->GetOutermost()->GetName();
		if (!ScopeLevelPackages.Contains(NormalizePackage(LevelPackage)))
		{
			continue;
		}

		for (AActor* Actor : Level->Actors)
		{
			if (Actor == nullptr || Actor->IsA<AWorldSettings>())
			{
				continue;
			}
			Out.Actors.Add(Actor);

			if (Actor->IsChildActor())
			{
				// The engine exports a child actor through its parent's child actor
				// component, never as a node of its own; selecting it here would place its
				// geometry at the top of the file with a node nothing predicts.
				ReportSkipped(*Actor, TEXT("child actor of a child actor component"), /*bUnsupported=*/false, Out);
				continue;
			}

			// Everything this actor could contribute besides its static mesh components is
			// named here, and only here, so the export can keep it out of the file.
			CollectSuppressed(*Actor, LevelPackage, Out);

			if (const ALevelInstance* LevelInstance = Cast<ALevelInstance>(Actor))
			{
				// The engine's FBX level exporter refuses these with its own warning.
				ReportSkipped(
					*Actor,
					TEXT("Exporting Level Instances to FBX is not supported by the engine's level exporter"),
					/*bUnsupported=*/true,
					Out);
				continue;
			}
			if (ALandscapeProxy* Landscape = Cast<ALandscapeProxy>(Actor))
			{
				FMtoUSceneRefObject Object;
				Object.ActorLabel = Actor->GetActorLabel();
				Object.ActorClass = Actor->GetClass()->GetName();
				Object.LevelPackage = LevelPackage;
				Object.ComponentName = Actor->GetRootComponent() != nullptr ? Actor->GetRootComponent()->GetName() : FString();
				Object.Category = EMtoUSceneRefCategory::Landscape;
				Object.NodeName = Actor->GetActorLabel();
				Object.bExported = true;
				Object.WorldTransform = Landscape->GetActorTransform();
				Object.WorldBounds = Landscape->GetComponentsBoundingBox(/*bNonColliding=*/true);
				Object.Id = FString::Printf(TEXT("landscape:%s"), *Actor->GetActorLabel());
				Out.Objects.Add(Object);
				Out.ExportableActors.Add(Actor);
				continue;
			}

			const bool bBlueprintActor = UBlueprint::GetBlueprintFromClass(Actor->GetClass()) != nullptr;
			TArray<UStaticMeshComponent*> MeshComponents;
			if (const AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(Actor))
			{
				if (UStaticMeshComponent* Component = StaticMeshActor->GetStaticMeshComponent())
				{
					MeshComponents.Add(Component);
				}
			}
			else
			{
				// Blueprint actors and every other actor type export their components; the
				// filter keeps only the static mesh components, the ones the node layout
				// below predicts.
				for (UActorComponent* ActorComponent : Actor->GetComponents())
				{
					if (UStaticMeshComponent* Component = Cast<UStaticMeshComponent>(ActorComponent))
					{
						if (IsExportedStaticMeshComponent(*Component))
						{
							MeshComponents.Add(Component);
						}
					}
				}
			}

			if (MeshComponents.Num() == 0)
			{
				const TCHAR* Reason = bBlueprintActor
					? TEXT("blueprint actor without an exportable static mesh component")
					: TEXT("not_static_geometry");
				ReportSkipped(*Actor, Reason, /*bUnsupported=*/false, Out);
				continue;
			}

			// Predicts the node layout the engine writes for the components the filter
			// leaves: a single static mesh component keeps the actor node (named after the
			// actor), several get one child node each, named after the component.
			const bool bMultipleExportComponents = CountExportComponents(*Actor) > 1;
			for (UStaticMeshComponent* Component : MeshComponents)
			{
				CollectObject(*Actor, LevelPackage, bMultipleExportComponents, Actor->GetActorLabel(), *Component, Out);
			}
			Out.ExportableActors.Add(Actor);
		}
	}

	Out.bResolved = true;
	return true;
}
