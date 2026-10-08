// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FMtoUMultiSubjectReceiver;

/**
 * The Maya side of the cross-host check: an optional command-line peer that
 * drives the real receiver over a real socket. These are the flags the Maya
 * agent implements.
 */
struct FMtoUMultiSubjectPeerRequest
{
	FString MayapyPath;
	FString PeerScriptPath;
	FString Scenario = TEXT("character-prop");
	FString Host = TEXT("127.0.0.1");
	int32 Port = 54340;
	int32 Frames = 24;
	double Fps = 30.0;
	int32 StartFrame = 1;
	/** Send `remove` for the second subject before this frame; 0 disables it. */
	int32 RemoveAtFrame = 0;
	/** Close the socket abruptly after this many frames; 0 disables it. */
	int32 DropAfterFrames = 0;
	/**
	 * Comma-separated Maya source times, one per frame. Empty derives them from
	 * `StartFrame`/`Frames`; an explicit list is how a reverse scrub or a
	 * same-frame re-edit is exercised end to end.
	 */
	FString Times;
	FString EvidencePath;
	FString LogPath;
	double TimeoutSeconds = 120.0;

	/** The exact command line, for logs and evidence. */
	FString BuildCommandLine() const;
};

struct FMtoUMultiSubjectPeerResult
{
	bool bStarted = false;
	bool bCompleted = false;
	int32 ReturnCode = -1;
	FString Output;
	TSharedPtr<FJsonObject> Evidence;
	bool bEvidenceOk = false;
	bool bEvidenceOkField = false;
	double FirstLatencyMs = -1.0;
	double MaxLatencyMs = -1.0;
	int32 EvidenceFrameCount = 0;
	FString EvidenceErrorText;
};

/** Starts the peer, pumps the receiver while it runs, and reads its evidence. */
bool RunMayaMultiSubjectPeer(
	const FMtoUMultiSubjectPeerRequest& Request,
	FMtoUMultiSubjectReceiver& Receiver,
	FMtoUMultiSubjectPeerResult& Out,
	FString& OutError);
