// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MtoUMultiSubjectTypes.h"

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
	 * Maya source times, one per frame. Empty derives them from
	 * `StartFrame`/`Frames`; an explicit list is how a reverse scrub or a
	 * same-frame re-edit is exercised end to end. The peer receives them as a
	 * validated comma-separated list, never as the raw argument text.
	 */
	TArray<double> Times;
	FString EvidencePath;
	FString LogPath;
	double TimeoutSeconds = 120.0;

	// --- real-asset session (the production C01 / Backups pairing) ---------
	/** Open this scene read-only instead of a fresh generated session scene. */
	FString ScenePath;
	/** Fixture rigs to reference into `ScenePath`, in order (`--reference-rig`). */
	TArray<FString> ReferenceRigs;
	/** `id=root` subject root overrides for subjects the scene already holds. */
	TArray<FString> SubjectOverrides;
	/** Fixture directory used with `--rig-spec`. */
	FString RigDir;
	/** Recipe JSON the fixture rigs are built from, instead of the checked-in one. */
	FString RigSpec;
	/** Extra peer arguments, already quoted (for example `--set-curve "..."`). */
	TArray<FString> ExtraArguments;

	/** The exact command line, for logs and evidence. */
	FString BuildCommandLine() const;

	/**
	 * Reads one `-name=value` argument from a command line. The name has to
	 * start a token (after whitespace or the beginning of the line), the value
	 * ends at the next unquoted space, and surrounding quotes are removed. This
	 * is deliberately stricter than `FParse::Value` for string values: it keeps
	 * a comma list such as `-MtoUMultiSubjectTimes=1,3,2,2` whole without
	 * swallowing the arguments that follow it.
	 */
	static bool TryReadArgumentValue(
		const TCHAR* CommandLine,
		const TCHAR* Name,
		FString& OutValue);

	/** Parses a comma-separated list of finite numbers. The list may not be empty. */
	static bool ParseTimeList(const FString& Value, TArray<double>& OutTimes);
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
	/**
	 * The subjects of the last live session, captured while the peer was still
	 * streaming: the session's declarations and their negotiation maps (which
	 * target bones are driven, required, ignored or undriven). The receiver
	 * forgets a session when the peer disconnects, so this snapshot is the only
	 * durable record of what was negotiated.
	 */
	TArray<FMtoUNegotiatedSubject> NegotiatedSubjects;
};

/** Starts the peer, pumps the receiver while it runs, and reads its evidence. */
bool RunMayaMultiSubjectPeer(
	const FMtoUMultiSubjectPeerRequest& Request,
	FMtoUMultiSubjectReceiver& Receiver,
	FMtoUMultiSubjectPeerResult& Out,
	FString& OutError);
