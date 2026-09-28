// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

class AActor;
class UStaticMeshComponent;

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

	int32 TriangleCount = 0;
	int32 VertexCount = 0;
	int32 ComponentCount = 0;

	/** Writes the manifest next to the produced geometry file. Returns false on I/O failure. */
	bool WriteManifest(FString& OutError) const;
};
