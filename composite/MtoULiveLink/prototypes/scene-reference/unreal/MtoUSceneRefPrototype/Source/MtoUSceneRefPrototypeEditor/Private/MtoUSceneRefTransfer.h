// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "MtoUSceneRefTypes.h"

class UWorld;

/** Inputs of one transfer run. */
struct FMtoUSceneRefTransferOptions
{
	/** Directory the run owns; it is recreated so a repeat run cannot mix with the previous one. */
	FString OutputDirectory;
	/** ASCII FBX keeps the prototype's own texture check readable. */
	bool bAscii = true;
	/** Keep the engine's own front axis (false) or force the FBX front axis to X. */
	bool bForceFrontXAxis = false;
};

/**
 * Runs one official-level-export transfer and records what it actually produced.
 *
 * The geometry comes from the engine's own `ULevelExporterFBX`; this class only
 * resolves the scope, drives the editor selection the exporter reads, and
 * inspects the produced files so no claim rests on an export flag.
 */
class FMtoUSceneRefTransfer
{
public:
	/** Exports, inspects, and fills Out. Returns false with Out.Error set on refusal. */
	static bool Run(
		UWorld& World,
		const FMtoUSceneRefScopeSpec& Spec,
		const FMtoUSceneRefTransferOptions& Options,
		FMtoUSceneRefTransferResult& Out);

	/**
	 * Runs the engine's FBX level exporter over exactly these actors and nothing else, with
	 * the options the transfer itself uses. Returns what the engine's export task returned:
	 * a refusal the engine alone produces (a Level Instance) is measured here instead of
	 * predicted from the resolver's report.
	 */
	static bool ProbeActorExport(
		UWorld& World,
		const TArray<AActor*>& Actors,
		const FString& FbxPath,
		const FMtoUSceneRefTransferOptions& Options);

	/**
	 * Runs the official OBJ level exporter over the same resolution.
	 *
	 * Used by the capability probe: the OBJ path is the other official level export,
	 * and the probe records what it actually writes (including material sidecars).
	 */
	static bool ProbeObjExport(
		UWorld& World,
		const FMtoUSceneRefResolution& Resolution,
		const FString& ObjPath,
		TArray<FMtoUSceneRefProducedFile>& OutFiles,
		TArray<FString>& OutMaterialLines,
		FString& OutError);

	/** Scans an ASCII FBX for material texture records and reports what it names. */
	static void ScanFbxTextureReferences(const FString& FilePath, FMtoUSceneRefTextureScan& Out);

	/** Collects the model node names an ASCII FBX records, for cross-host node matching. */
	static void ScanFbxModelNames(const FString& FilePath, TArray<FString>& OutNames);
};
