// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "MtoUSceneRefTypes.h"

/**
 * Builds the reproducible sample the transfer is measured on.
 *
 * The fixture is generated, not committed: the maps under `/Game/MtoUSceneRefFixture`
 * (a persistent level, a loaded sublevel, a sublevel that stays unloaded, a sublevel
 * whose material samples a texture, a level instance sample, a World Partition level
 * from the engine's own OpenWorld template and a landscape sample) plus the Blueprint
 * actors and the Nanite mesh the export measures. Every actor has a fixed transform,
 * so the Maya side can be compared against the manifest.
 */
class FMtoUSceneRefFixture
{
public:
	static const TCHAR* RootFolder();
	static const TCHAR* HostLevelPackage();
	static const TCHAR* LoadedSublevelPackage();
	static const TCHAR* UnloadedSublevelPackage();
	static const TCHAR* TexturedSublevelPackage();

	/**
	 * The Nanite sample mesh: the engine's basic cube duplicated under the fixture
	 * folder with `NaniteSettings.bEnabled` on, so the read-only engine asset stays
	 * untouched.
	 */
	static const TCHAR* NaniteMeshPackage();
	/** The level the level instance sample places, and the level that carries the instance. */
	static const TCHAR* InstanceSourceLevelPackage();
	static const TCHAR* InstanceHostLevelPackage();
	/** The level built from the engine's World Partition template. */
	static const TCHAR* PartitionedLevelPackage();
	static const TCHAR* WorldPartitionTemplatePackage();
	/** The level that carries the one component landscape sample. */
	static const TCHAR* LandscapeLevelPackage();

	/** The main sample: the persistent level plus the loaded sublevel; the unloaded one is reported. */
	static FMtoUSceneRefScopeSpec MainScope();
	/** The texture probe: the persistent level plus the sublevel whose material samples a texture. */
	static FMtoUSceneRefScopeSpec TexturedScope();
	/** The level instance sample's scope: the level that carries the instance actor. */
	static FMtoUSceneRefScopeSpec InstanceScope();
	/** The World Partition sample's scope. */
	static FMtoUSceneRefScopeSpec PartitionedScope();
	/** The landscape sample's scope. */
	static FMtoUSceneRefScopeSpec LandscapeScope();

	/** The actor labels the samples place, so a test names what the fixture built. */
	static const TCHAR* NaniteActorLabel();
	static const TCHAR* InstanceActorLabel();
	static const TCHAR* LandscapeActorLabel();
	static const TCHAR* PartitionActorLabel(int32 Index);
	/** How many actors the partitioned sample places. */
	static int32 PartitionActorCount();

	/**
	 * Loads one fixture level as the loaded editor world. Returns true when it already is.
	 * The host level comes back through RestoreHostWorld, which is what a sample test owes
	 * the tests that follow it.
	 */
	static bool LoadFixtureLevel(const FString& Package, FString& OutError);
	/** Puts the host level back as the loaded world with its streaming sublevels, as Build leaves it. */
	static bool RestoreHostWorld(TArray<FString>& OutNotes, FString& OutError);

	/**
	 * Deletes any previous fixture and rebuilds it, leaving the host level loaded
	 * with its loaded sublevel streamed in and the unloaded sublevel still out.
	 * Failures that are part of the design (a fixture asset that cannot be built)
	 * are reported in OutNotes instead of aborting the run.
	 */
	static bool Build(TArray<FString>& OutNotes, FString& OutError);
};
