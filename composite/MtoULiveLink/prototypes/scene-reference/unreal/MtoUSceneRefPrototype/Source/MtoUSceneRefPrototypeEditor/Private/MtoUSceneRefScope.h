// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "MtoUSceneRefTypes.h"

class AActor;
class ULevel;
class UStaticMeshComponent;
class UWorld;

/**
 * Turns a scope request into the set of objects a transfer can carry.
 *
 * The engine's FBX level exporter exports what the editor selection holds, so the
 * resolution is also the selection the export applies and restores. Everything the
 * request cannot deliver (unloaded sublevels, level instances, non-geometry actors)
 * is reported here instead of being dropped by the engine with a log line only.
 *
 * See prototypes/scene-reference/transfer.md for the selection semantics.
 */
class FMtoUSceneRefScope
{
public:
	/** Resolves Spec against the loaded editor world. Fills Out.Error and returns false on refusal. */
	static bool Resolve(UWorld& World, const FMtoUSceneRefScopeSpec& Spec, FMtoUSceneRefResolution& Out);

	/** Runs the export for exactly the objects this resolution carries. */
	static const TCHAR* CategoryName(EMtoUSceneRefCategory Category);

private:
	/** Appends one object record per exportable mesh component of the actor. */
	static void CollectObject(
		const AActor& Actor,
		const FString& LevelPackage,
		bool bActorHasMultipleExportComponents,
		const FString& ActorNodeName,
		UStaticMeshComponent& Component,
		FMtoUSceneRefResolution& Out);

	/**
	 * The reason this component is not static reference geometry, or nullptr when it is
	 * kept. Every reason names one engine export branch (camera, light, skeletal, child
	 * actor), so a report reader can tell what would have reached the file.
	 */
	static const TCHAR* SuppressionReason(const USceneComponent& Component);

	/** Appends one record per component of the actor the export has to suppress. */
	static void CollectSuppressed(const AActor& Actor, const FString& LevelPackage, FMtoUSceneRefResolution& Out);

	/** Fills the report-only lists for an actor no object is produced from. */
	static void ReportSkipped(const AActor& Actor, const FString& Reason, bool bUnsupported, FMtoUSceneRefResolution& Out);
};
