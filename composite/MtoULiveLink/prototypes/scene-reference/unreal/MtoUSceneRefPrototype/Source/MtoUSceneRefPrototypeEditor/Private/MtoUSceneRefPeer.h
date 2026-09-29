// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** What the Unreal side hands to the Maya importer/verifier. */
struct FMtoUSceneRefPeerRequest
{
	FString MayapyPath;
	FString PeerScriptPath;
	FString FbxPath;
	FString ManifestPath;
	FString ReportPath;
	FString LogPath;
	/**
	 * Arguments the caller passes through to the Maya CLI verbatim, appended after the
	 * `--report` argument on its command line, each already spelled the way the peer
	 * script expects it (`--target-world camera`, `--shading keep`, `--dry-run`, ...).
	 */
	TArray<FString> ExtraArguments;
	double TimeoutSeconds = 600.0;
};

/** What came back, including the Maya report the peer wrote. */
struct FMtoUSceneRefPeerResult
{
	bool bStarted = false;
	bool bCompleted = false;
	int32 ReturnCode = -1;
	FString Output;
	FString ReportText;
	TSharedPtr<FJsonObject> Report;
};

/** Runs the Maya side over a produced transfer and reads its report. */
bool RunMayaSceneRefPeer(
	const FMtoUSceneRefPeerRequest& Request,
	FMtoUSceneRefPeerResult& Out,
	FString& OutError);
