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
#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "StaticMeshResources.h"
#include "WorldPartition/ActorDescContainerInstance.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"

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

	/** How many unloaded actor paths the World Partition inventory records per run. */
	constexpr int32 WorldPartitionInventoryLimit = MtoUSceneRefWorldPartitionInventoryLimit;

	/**
	 * Area weighted surface centroid over world space triangles.
	 *
	 * Position, bounds size and pivot offset are all blind to a mirrored placement;
	 * this quantity is not. It is area weighted on purpose: the engine's render vertex
	 * buffer and the exported file do not hold the same vertex set (measured: 198
	 * positions in the render buffer against 144 in the file for the same cone), while
	 * the triangles are the same, so only a triangle based quantity is comparable.
	 */
	struct FSurfaceCentroid
	{
		FVector WeightedSum = FVector::ZeroVector;
		double TotalArea = 0.0;

		void AddTriangle(const FVector& A, const FVector& B, const FVector& C)
		{
			const double Area = 0.5 * ((B - A).Cross(C - A)).Size();
			if (Area <= 0.0)
			{
				return;
			}
			WeightedSum += Area * (A + B + C) / 3.0;
			TotalArea += Area;
		}

		FVector Centroid() const
		{
			return TotalArea > 0.0 ? WeightedSum / TotalArea : FVector::ZeroVector;
		}
	};

	/**
	 * The landscape's record, measured from the geometry the engine's landscape branch writes.
	 *
	 * `FFbxExporter::ExportLandscapeToFbx` builds one mesh per landscape actor out of
	 * `FLandscapeComponentDataInterface` vertices at `ALandscapeProxy::ExportLOD` -- two
	 * triangles per quad, the component's relative location added, the actor's transform on
	 * the node. This walks exactly that source, so the manifest's bounds, surface centroid,
	 * triangle and vertex counts describe the file the Maya side reads. The actor's own
	 * collision/editor bounds are deliberately not used: they include the terrain's
	 * thickness and are the reason the first landscape handoff failed its Maya size check.
	 */
	FMtoUSceneRefObject CollectLandscape(ALandscapeProxy& Landscape, const FString& LevelPackage)
	{
		FMtoUSceneRefObject Object;
		Object.ActorLabel = Landscape.GetActorLabel();
		Object.ActorClass = Landscape.GetClass()->GetName();
		Object.LevelPackage = LevelPackage;
		Object.ComponentName = Landscape.GetRootComponent() != nullptr
			? Landscape.GetRootComponent()->GetName()
			: FString();
		Object.Category = EMtoUSceneRefCategory::Landscape;
		Object.NodeName = Landscape.GetActorLabel();
		Object.bExported = true;
		Object.WorldTransform = Landscape.GetActorTransform();
		Object.Id = FString::Printf(TEXT("landscape:%s"), *Landscape.GetActorLabel());

		const FTransform LandscapeToWorld = Landscape.GetActorTransform();
		const int32 ExportLOD = FMath::Max(0, Landscape.ExportLOD);
		const int32 ComponentSizeQuads = ((Landscape.ComponentSizeQuads + 1) >> ExportLOD) - 1;
		if (ComponentSizeQuads <= 0)
		{
			Object.Note = FString::Printf(
				TEXT("export LOD %d leaves no quads to measure; the record carries the actor bounds only"),
				ExportLOD);
			Object.WorldBounds = Landscape.GetComponentsBoundingBox(/*bNonColliding=*/true);
			return Object;
		}

		const int32 VertsPerComponent = FMath::Square(ComponentSizeQuads + 1);
		const int32 Stride = ComponentSizeQuads + 1;
		FBox Bounds(ForceInit);
		FSurfaceCentroid Centroid;
		TArray<FVector> Positions;
		Positions.SetNumUninitialized(VertsPerComponent);
		bool bVisibilityLayer = false;
		bool bSkippedComponent = false;
		int32 ComponentsMeasured = 0;
		for (ULandscapeComponent* Component : Landscape.LandscapeComponents)
		{
			if (Component == nullptr)
			{
				bSkippedComponent = true;
				continue;
			}
			// A visibility layer hides quads from the file: the exporter writes only the
			// quads whose stored visibility passes its own threshold, so a landscape that
			// carries one is reported rather than silently measured as the full grid.
			for (const FWeightmapLayerAllocationInfo& Allocation : Component->GetWeightmapLayerAllocations())
			{
				bVisibilityLayer |= Allocation.LayerInfo == ALandscapeProxy::VisibilityLayer;
			}
			FLandscapeComponentDataInterface DataInterface(Component, ExportLOD);
			const FVector RelativeLocation = Component->GetRelativeLocation();
			for (int32 VertexIndex = 0; VertexIndex < VertsPerComponent; ++VertexIndex)
			{
				int32 VertexX = 0;
				int32 VertexY = 0;
				DataInterface.VertexIndexToXY(VertexIndex, VertexX, VertexY);
				Positions[VertexIndex] = LandscapeToWorld.TransformPosition(
					DataInterface.GetLocalVertex(VertexX, VertexY) + RelativeLocation);
				Bounds += Positions[VertexIndex];
			}
			// The exporter's own triangulation: two polygons per quad, wound
			// (x,y) (x+1,y+1) (x+1,y) and (x,y) (x,y+1) (x+1,y+1).
			for (int32 Y = 0; Y < ComponentSizeQuads; ++Y)
			{
				for (int32 X = 0; X < ComponentSizeQuads; ++X)
				{
					const FVector& V00 = Positions[X + Y * Stride];
					const FVector& V10 = Positions[(X + 1) + Y * Stride];
					const FVector& V01 = Positions[X + (Y + 1) * Stride];
					const FVector& V11 = Positions[(X + 1) + (Y + 1) * Stride];
					Centroid.AddTriangle(V00, V11, V10);
					Centroid.AddTriangle(V00, V01, V11);
				}
			}
			Object.Triangles += FMath::Square(ComponentSizeQuads) * 2;
			Object.Vertices += VertsPerComponent;
			++ComponentsMeasured;
		}

		Object.WorldBounds = Bounds;
		Object.WorldSurfaceCentroid = Centroid.Centroid();
		const TCHAR* Caveats = TEXT("");
		if (bVisibilityLayer)
		{
			Caveats = TEXT("; a visibility layer is present, and the file omits hidden quads, so the real triangle count can be lower");
		}
		else if (bSkippedComponent)
		{
			Caveats = TEXT("; a null component entry was skipped");
		}
		Object.Note = FString::Printf(
			TEXT("measured from %d landscape component(s) at export LOD %d, the same source the engine's landscape export writes%s"),
			ComponentsMeasured, ExportLOD, Caveats);
		return Object;
	}

	/**
	 * What a partitioned world holds beyond the loaded cells, read without loading anything.
	 *
	 * Every actor of a World Partition world exists as an actor descriptor, and only the
	 * ones inside loaded cells have spawned an actor. Walking the descriptors is read only,
	 * and it is what turns "the scope holds what is loaded" into a count and a list, so an
	 * empty unloaded-sublevel list cannot be read as complete coverage.
	 */
	void CollectWorldPartitionScope(UWorld& World, FMtoUSceneRefResolution& Out)
	{
		FMtoUSceneRefWorldPartitionScope& Scope = Out.WorldPartitionScope;
		Scope.bDetected = Out.bWorldPartition;
		if (!Out.bWorldPartition)
		{
			Out.Completeness = EMtoUSceneRefScopeCompleteness::Confirmed;
			Scope.Note = TEXT("the world is not partitioned: every requested sublevel is either loaded and traversed or listed as unloaded");
			return;
		}
#if WITH_EDITOR
		UWorldPartition* Partition = World.GetWorldPartition();
		if (Partition == nullptr)
		{
			Out.Completeness = EMtoUSceneRefScopeCompleteness::NotConfirmed;
			Scope.Note = TEXT("the world reports a partition but exposes no partition object, so the descriptor inventory could not be read");
			return;
		}
		Partition->ForEachActorDescContainerInstance(
			[&Scope](UActorDescContainerInstance* Container)
			{
				if (Container == nullptr)
				{
					return;
				}
				++Scope.Containers;
				for (const TPair<FGuid, TUniquePtr<FWorldPartitionActorDescInstance>*>& Entry : Container->GetActorsByGuid())
				{
					const FWorldPartitionActorDescInstance* Descriptor =
						Entry.Value != nullptr ? Entry.Value->Get() : nullptr;
					if (Descriptor == nullptr)
					{
						continue;
					}
					++Scope.ActorDescriptors;
					if (Descriptor->IsLoaded())
					{
						++Scope.LoadedActorDescriptors;
						continue;
					}
					// A generated HLOD proxy is a merged copy of authored content, not content
					// of its own: it is counted apart so the gap the scope cannot carry is the
					// authored placement it is missing.
					const UClass* NativeClass = Descriptor->GetActorNativeClass();
					if (NativeClass != nullptr &&
						NativeClass->IsChildOf(AWorldPartitionHLOD::StaticClass()))
					{
						++Scope.UnloadedHlodCount;
						continue;
					}
					++Scope.UnloadedActorCount;
					if (Scope.UnloadedActors.Num() < WorldPartitionInventoryLimit)
					{
						Scope.UnloadedActors.Add(Descriptor->GetActorSoftPath().ToString());
					}
					else
					{
						Scope.bInventoryTruncated = true;
					}
				}
			},
			/*bRecursive=*/true);
		Scope.bInventoryAvailable = true;
		// The descriptors are the partition's own record of what the world holds, so the
		// scope is complete exactly when every one of them has spawned: a streamed out
		// actor is a gap the level range cannot carry, and the count names its size.
		Out.Completeness = Scope.UnloadedActorCount == 0
			? EMtoUSceneRefScopeCompleteness::Confirmed
			: EMtoUSceneRefScopeCompleteness::NotConfirmed;
		Scope.Note = Scope.UnloadedActorCount == 0
			? FString::Printf(
				TEXT("every authored actor of the partition is spawned in the loaded world: %d of %d descriptor(s) over %d container(s), the remaining %d being generated HLOD proxies"),
				Scope.LoadedActorDescriptors, Scope.ActorDescriptors, Scope.Containers,
				Scope.UnloadedHlodCount)
			: FString::Printf(
				TEXT("only the content the loaded cells have spawned is in scope: %d of %d actor descriptor(s) over %d container(s) are authored actors that are not spawned here (%d further descriptor(s) are generated HLOD proxies), and the transfer never loads content on its own"),
				Scope.UnloadedActorCount, Scope.ActorDescriptors, Scope.Containers,
				Scope.UnloadedHlodCount);
#else
		Out.Completeness = EMtoUSceneRefScopeCompleteness::NotConfirmed;
		Scope.Note = TEXT("the descriptor inventory needs an editor build");
#endif
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
				FSurfaceCentroid Centroid;
				const int32 TriangleCount = LOD.GetNumTriangles();
				for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
				{
					const FVector Corners[3] = {
						InstanceTransform.TransformPosition(FVector(Positions.VertexPosition(LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 0)))),
						InstanceTransform.TransformPosition(FVector(Positions.VertexPosition(LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 1)))),
						InstanceTransform.TransformPosition(FVector(Positions.VertexPosition(LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 2)))),
					};
					Centroid.AddTriangle(Corners[0], Corners[1], Corners[2]);
				}
				Object.WorldSurfaceCentroid = Centroid.Centroid();
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

#if WITH_EDITOR
	Out.bWorldPartition = World.GetWorldPartition() != nullptr;
#endif
	// The scope's completeness is decided here, before the traversal, and carried into
	// the manifest: a partitioned world holds the actors its loaded cells have spawned
	// and nothing else, which the report says instead of leaving an empty unloaded list
	// to be read as complete coverage.
	CollectWorldPartitionScope(World, Out);

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
				Out.Objects.Add(CollectLandscape(*Landscape, LevelPackage));
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
