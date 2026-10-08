// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "MtoUMultiSubjectDriver.h"
#include "MtoUMultiSubjectPreview.h"
#include "MtoUMultiSubjectTypes.h"

class FJsonObject;
class FSocket;
class ISequencer;
class UWorld;

/** Everything one run of the receiver is configured with. */
struct FMtoUMultiSubjectSessionConfig
{
	/** 127.0.0.1 port; tests reserve an ephemeral one. */
	uint16 Port = 54340;
	/** Scenario name used in logs and evidence ("character-prop", "character-arms"). */
	FString Scenario;
	/**
	 * Writers the preview takes over from as soon as a session becomes ready.
	 * Empty means the receiver drives the targets without taking anything over,
	 * which is what the pure protocol scenarios use.
	 */
	TArray<FMtoUPreviewWriter> PreviewWriters;
	/** Open editor Sequencer, when the writer belongs to one. */
	TSharedPtr<ISequencer> EditorSequencer;
	/** Frames kept in the evidence file; later frames still apply, only the record is bounded. */
	int32 MaxRecordedFrames = 240;
};

/** One negotiated subject of the live session. */
struct FMtoUSessionSubject
{
	FString Id;
	FMtoUSubjectDeclaration Declaration;
	FMtoUMultiSubjectTarget* Target = nullptr;
	bool bEnabled = false;
	int64 AppliedFrames = 0;
	double MaxBoneDelta = 0.0;
	double MaxRootWorldDelta = 0.0;
	/** Target bones this subject does not drive; they keep their reference pose. */
	TArray<FName> UndrivenBones;
};

/**
 * The bounded two-subject receiver: one listener, one client, one session, two
 * independent targets, one shared frame time. It is driven from the game
 * thread (`Pump` from a ticker or from an Automation test step), applies a
 * frame only after every subject of that frame validated, and always puts its
 * targets back before it forgets a session.
 */
class FMtoUMultiSubjectReceiver
{
public:
	FMtoUMultiSubjectReceiver();
	~FMtoUMultiSubjectReceiver();

	/** Registers the targets (anchor-once preflight) and listens on 127.0.0.1. */
	bool Start(
		UWorld& InWorld,
		const TArray<FMtoUTargetRegistration>& TargetRegistrations,
		const FMtoUMultiSubjectSessionConfig& InConfig,
		FString& OutError);

	/** Ends the session, restores every target, and stops listening. */
	void Stop(const FString& Reason);

	/** Accepts a pending client and reads every complete line it sent. */
	void Pump(double DeltaSeconds);

	/** Processes one client line exactly as a network message would be processed. */
	void HandleClientLine(const FString& Line);

	/** Editor world teardown: a session never outlives the world it drives. */
	void HandleWorldCleanup(UWorld* InWorld, bool bSessionEnded, bool bCleanupResources);

	bool IsRunning() const { return bRunning; }
	bool IsListening() const;
	bool HasClient() const { return ClientSocket != nullptr; }
	bool HasSession() const { return bReady; }
	int64 GetSessionId() const { return SessionId; }
	uint16 GetBoundPort() const { return BoundPort; }
	int32 GetClientLineCount() const { return ClientLineCount; }
	int32 GetAppliedFrameCount() const { return AppliedFrames; }
	int64 GetLastAppliedSerial() const { return LastAppliedSerial; }
	double GetLastAppliedTime() const { return LastAppliedTime; }
	/** How the last applied source time moved: first/forward/backward/hold. */
	const FString& GetLastTimeDirection() const { return LastTimeDirection; }


	/** One line per target naming the driver the preview replaced and how it exited. */
	TArray<FString> DescribeDriveOwnership() const;
	const FString& GetLastSessionEndReason() const { return LastSessionEndReason; }
	const FString& GetLastErrorCode() const { return LastErrorCode; }
	const TArray<FMtoUEvidenceError>& GetErrors() const { return Errors; }
	const TArray<FMtoUFrameRecord>& GetFrameRecords() const { return FrameRecords; }
	const TArray<FMtoUSessionSubject>& GetSubjects() const { return Subjects; }
	const FMtoUMultiSubjectPreview& GetPreview() const { return Preview; }
	const TArray<TUniquePtr<FMtoUMultiSubjectTarget>>& GetTargets() const { return Targets; }

	FMtoUMultiSubjectTarget* FindTarget(const FString& Id) const;

	/** Machine-readable evidence of everything this run measured. */
	bool SaveEvidence(const FString& Directory, FString& OutPath, FString& OutError) const;

private:
	bool StartListener(FString& OutError);
	void AcceptPendingClient();
	void ReadClientLines();
	void HandleInit(const TSharedPtr<FJsonObject>& Object);
	void HandleFrame(const TSharedPtr<FJsonObject>& Object);
	void HandleRemove(const TSharedPtr<FJsonObject>& Object);

	bool BeginSession(const FMtoUInitMessage& Init, FString& OutError);
	/** Refuses a message that names another session; returns true when refused. */
	bool RefuseStaleSession(int64 MessageSession, const TCHAR* What);
	void EndSession(const FString& Reason);
	void CloseClient(const FString& Reason);

	void SendJson(const TSharedRef<FJsonObject>& Object);
	void SendError(const FString& Code, const FString& Details);
	void NoteError(const FString& Code, const FString& Details, int64 AtSerial = 0);
	void NoteShapeError(const FMtoUProtocolError& Error);

	TArray<FMtoUSessionSubject*> EnabledSubjects();
	/** `bEnabledOnly` lists the subjects a frame carried; otherwise the whole set. */
	void CollectSubjectStatuses(TArray<FMtoUSubjectStatus>& OutStatuses, bool bEnabledOnly) const;

	UWorld* World = nullptr;
	FMtoUMultiSubjectSessionConfig Config;
	TArray<TUniquePtr<FMtoUMultiSubjectTarget>> Targets;
	FSocket* ListenSocket = nullptr;
	FSocket* ClientSocket = nullptr;
	TArray<uint8> ReceiveBytes;
	FString ClientDescription;
	FDelegateHandle WorldCleanupHandle;

	TArray<FMtoUSessionSubject> Subjects;
	bool bRunning = false;
	bool bReady = false;
	int64 SessionId = 0;
	int64 LastAppliedSerial = 0;
	double LastAppliedTime = 0.0;
	int32 AppliedFrames = 0;
	int64 ReceivedFrames = 0;
	FString LastTimeDirection;
	int32 ClientLineCount = 0;
	uint16 BoundPort = 0;
	bool bOversizeLine = false;

	FMtoUMultiSubjectPreview Preview;
	TArray<FMtoUFrameRecord> FrameRecords;
	TArray<FMtoUEvidenceError> Errors;
	FString LastErrorCode;
	FString LastSessionEndReason;
	FString LastErrorDetails;
	bool bStarted = false;
};
