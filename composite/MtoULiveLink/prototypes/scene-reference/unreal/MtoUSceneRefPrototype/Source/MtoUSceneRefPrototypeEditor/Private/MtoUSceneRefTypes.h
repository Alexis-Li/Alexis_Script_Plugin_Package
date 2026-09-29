// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

class AActor;
class UStaticMeshComponent;
class USceneComponent;

/** What a scope member is, for the support report. */
enum class EMtoUSceneRefCategory : uint8
{
	/** A static mesh component without instancing. */
	StaticMesh,
	/** One instance of an instanced static mesh component. */
	InstancedStaticMesh,
	/** A landscape proxy; exported by the engine's own landscape branch. */
	Landscape,
	/** A level instance; the engine's FBX exporter refuses these. */
	LevelInstance,
	/** In scope but not exportable by the engine's level exporter; reported. */
	Unsupported,
	/** In scope but not static geometry (lights, cameras, world settings, ...). */
	SkippedNonGeometry,
};

/** One object the export may place in the file. */
struct FMtoUSceneRefObject
{
	/** Stable identity of this object inside the scope. */
	FString Id;
	/** Node name the engine's FBX exporter is expected to write for this object. */
	FString NodeName;
	FString ActorLabel;
	FString ActorClass;
	FString LevelPackage;
	FString ComponentName;
	FString MeshPath;
	/** INDEX_NONE unless this record is one instance of an instanced component. */
	int32 InstanceIndex = INDEX_NONE;
	EMtoUSceneRefCategory Category = EMtoUSceneRefCategory::Unsupported;
	bool bExported = false;
	FString Note;
	TArray<FString> Materials;
	/** Render geometry this object contributes, at LOD 0. */
	int32 Triangles = 0;
	int32 Vertices = 0;
	/** Component (or instance) world transform, evaluated by the engine. */
	FTransform WorldTransform = FTransform::Identity;
	/** World axis aligned bounds of this object, evaluated by the engine. */
	FBox WorldBounds = FBox(ForceInit);
	/**
	 * Surface (area weighted) centroid of the mesh's LOD 0 triangles in world space.
	 *
	 * Position, bounds size and bounds offset are all blind to a mirrored placement;
	 * this vector is not. It is area weighted on purpose: the engine's render vertex
	 * buffer and the exported file do not hold the same vertex set (measured: 198
	 * positions in the render buffer against 144 in the file for the same cone), while
	 * the triangles are the same, so only a triangle based quantity is comparable.
	 */
	FVector WorldSurfaceCentroid = FVector::ZeroVector;
};

/** An in-scope actor that contributes nothing. */
struct FMtoUSceneRefSkipped
{
	FString ActorLabel;
	FString ActorClass;
	FString Reason;
};

/**
 * One component the transfer keeps out of the produced file.
 *
 * The engine's FBX level exporter turns every non hidden mesh, camera, light and child
 * actor component of a selected actor into a node. The handoff is static reference
 * geometry, so the resolver names the components to suppress (`bHiddenInGame`) and the
 * export applies and restores exactly that list.
 */
struct FMtoUSceneRefSuppressedComponent
{
	FString ActorLabel;
	FString ActorClass;
	FString ComponentName;
	/** Runtime class of the component, e.g. PointLightComponent. */
	FString ComponentClass;
	/** Stable reason this component is not static reference geometry. */
	FString Reason;
	/** Package of the level the owning actor lives in. */
	FString LevelPackage;
};

/** A requested streaming sublevel that is not loaded. */
struct FMtoUSceneRefUnloadedLevel
{
	FString Package;
	FString StreamingState;
	bool bShouldBeVisible = false;
	bool bRequested = true;
};

/** What the caller asked for: one loaded level plus the sublevels it names. */
struct FMtoUSceneRefScopeSpec
{
	/** Package name of the level that must already be the loaded editor world. */
	FString LevelPackage;
	/** Streaming sublevels the caller wants in the transfer. */
	TArray<FString> RequestedSublevels;

	/** A short name used for the output file names. */
	FString ScopeName() const;
};

/** What the loaded world can actually offer for that request. */
struct FMtoUSceneRefResolution
{
	bool bResolved = false;
	FString Error;
	FString ScopeName;
	FString PersistentLevelPackage;
	bool bWorldPartition = false;
	TArray<FString> LoadedSublevels;
	TArray<FString> ExcludedSublevels;
	TArray<FMtoUSceneRefUnloadedLevel> UnloadedSublevels;
	/** Every actor in scope, including skipped ones. */
	TArray<AActor*> Actors;
	/** Actors the exporter must select; the engine skips non-geometry on its own. */
	TArray<AActor*> ExportableActors;
	TArray<FMtoUSceneRefObject> Objects;
	/**
	 * Non static mesh components in scope the export must suppress, because the engine's
	 * exporter would otherwise turn each of them into a camera, light, skeletal mesh or
	 * child actor node. The export applies and restores the flag for exactly these.
	 */
	TArray<FMtoUSceneRefSuppressedComponent> SuppressedComponents;
	/**
	 * The components behind SuppressedComponents, in the same order. The export drives
	 * these directly; the records above are what the manifest reports. Transient, like
	 * Actors: it points into the loaded world and is valid only while that world is.
	 */
	TArray<USceneComponent*> SuppressedComponentInstances;
	TArray<FMtoUSceneRefSkipped> Skipped;
	TArray<FMtoUSceneRefSkipped> Unsupported;
	int32 RequestedSublevelsMissing = 0;

	int32 ExportedObjectCount() const;
};

/** Timing and memory readings the prototype records without promising a budget. */
struct FMtoUSceneRefMeasurements
{
	double ScopeSeconds = 0.0;
	double ExportSeconds = 0.0;
	double InspectSeconds = 0.0;
	double UsedPhysicalMB = 0.0;
};

/** What the prototype's own scan of a produced ASCII FBX found. */
struct FMtoUSceneRefTextureScan
{
	/** `Texture:` records, whatever they name. */
	int32 TextureRecords = 0;
	/** Texture records that name a file, which is what an import would try to load. */
	int32 TextureReferences = 0;
	/** `Video:` records, which usually mirror the same media. */
	int32 VideoRecords = 0;
	/** `Content:` lines inside a texture or video record, embedded payload or not. */
	int32 ContentRecords = 0;
	/**
	 * Records whose `Content:` line carries a payload, on the same line or on the
	 * immediately following non empty line. Non zero means the handoff embeds media.
	 */
	int32 EmbeddedMediaRecords = 0;
	/** One descriptor per embedded media record, e.g. `Video::T_Rock (Content 4096 chars)`. */
	TArray<FString> EmbeddedMedia;
	/** `NodeAttribute:` records whose class token is exactly `Camera`. */
	int32 CameraRecords = 0;
	/** `NodeAttribute:` records whose class token is exactly `Light`. */
	int32 LightRecords = 0;
	/** Every file name the records mention, in the order they were found. */
	TArray<FString> FileNames;
};

/** One file the export produced. */
struct FMtoUSceneRefProducedFile
{
	FString Name;
	int64 Bytes = 0;
};

/** What the produced files actually contain, inspected after the export. */
struct FMtoUSceneRefOutput
{
	FString Directory;
	FString GeometryFile;
	int64 GeometryBytes = 0;
	TArray<FMtoUSceneRefProducedFile> Files;
	TArray<FString> ImageFiles;
	/** The prototype's own scan of the produced file; the Maya side repeats it. */
	FMtoUSceneRefTextureScan TextureScan;
	/** Texture reference files that exist on this machine (Maya would load those). */
	TArray<FString> TextureReferenceFilesPresent;
	/** Model names recorded in the produced file, for cross-host node matching. */
	TArray<FString> NodeNames;
};

/** Everything one transfer run produced. */
struct FMtoUSceneRefTransferResult
{
	bool bSucceeded = false;
	FString Error;
	TArray<FString> Warnings;
	FMtoUSceneRefScopeSpec Spec;
	FMtoUSceneRefResolution Resolution;
	FMtoUSceneRefOutput Output;
	FMtoUSceneRefMeasurements Measurements;
	FDateTime GeneratedUtc = FDateTime::UtcNow();

	/** The export option the run used; the manifest's convention block reports it. */
	bool bForceFrontXAxis = false;
	/** Components whose bHiddenInGame the export set and restored for its own duration. */
	int32 SuppressedComponentCount = 0;

	int32 TriangleCount = 0;
	int32 VertexCount = 0;
	int32 ComponentCount = 0;

	/** Writes the manifest next to the produced geometry file. Returns false on I/O failure. */
	bool WriteManifest(FString& OutError) const;
};
