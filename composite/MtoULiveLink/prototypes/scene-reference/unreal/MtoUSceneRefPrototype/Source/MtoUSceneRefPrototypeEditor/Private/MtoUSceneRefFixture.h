// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "MtoUSceneRefTypes.h"

/**
 * Builds the reproducible sample the transfer is measured on.
 *
 * The fixture is generated, not committed: four maps under
 * `/Game/MtoUSceneRefFixture` (a persistent level, one loaded sublevel, one
 * sublevel that stays unloaded, and one sublevel whose material samples a
 * texture) plus a Blueprint actor with two mesh components. Every actor has a
 * fixed transform, so the Maya side can be compared against the manifest.
 */
class FMtoUSceneRefFixture
{
public:
	static const TCHAR* RootFolder();
	static const TCHAR* HostLevelPackage();
	static const TCHAR* LoadedSublevelPackage();
	static const TCHAR* UnloadedSublevelPackage();
	static const TCHAR* TexturedSublevelPackage();

	/** The main sample: the persistent level plus the loaded sublevel; the unloaded one is reported. */
	static FMtoUSceneRefScopeSpec MainScope();
	/** The texture probe: the persistent level plus the sublevel whose material samples a texture. */
	static FMtoUSceneRefScopeSpec TexturedScope();

	/**
	 * Deletes any previous fixture and rebuilds it, leaving the host level loaded
	 * with its loaded sublevel streamed in and the unloaded sublevel still out.
	 * Failures that are part of the design (a fixture asset that cannot be built)
	 * are reported in OutNotes instead of aborting the run.
	 */
	static bool Build(TArray<FString>& OutNotes, FString& OutError);
};
