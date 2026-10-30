// MtoU camera sync prototype (Issue 52 verification). Editor-only prototype code.

#pragma once

#include "Containers/Queue.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MtoUCameraSyncPayload.h"

class AActor;
struct FIPv4Endpoint;
class FSocket;
class FTcpListener;
class ISequencer;
class UCameraComponent;
class ULevelSequence;
class ULevelSequencePlayer;
class UWorld;

/**
 * One prototype session: Unreal owns the sequence time and the evaluated camera, the
 * Maya client only receives and reports back what it applied. Nothing in this class
 * accepts a time or camera value from the client, which is what makes the prototype
 * free of time feedback loops by construction.
 */
class FMtoUCameraSyncSession : public TSharedFromThis<FMtoUCameraSyncSession>
{
public:
	struct FAppliedReport
	{
		int64 Serial = 0;
		int64 EvalSerial = 0;
		FString EvalIdentity;
		FString Status;
		double MayaFrame = 0.0;
		bool bPosePaired = false;
		FString PairingError;
		double PoseTranslateX = 0.0;
		FString RawJson;
	};

	struct FConfig
	{
		uint16 Port = 54330;
		FIntPoint OutputResolution = FIntPoint(1920, 1080);
		FString ResolutionSource = TEXT("prototype");
		double FarClipFallbackCm = 100000.0;
		double PublishIntervalSeconds = 0.1;
	};

	FMtoUCameraSyncSession();
	~FMtoUCameraSyncSession();

	bool Start(UWorld& World, ULevelSequence& Sequence, const FConfig& InConfig, FString& OutError);
	/** Follow the already open editor Sequencer; never create or control another player. */
	bool StartFromEditor(UWorld& World, const TSharedRef<ISequencer>& Sequencer,
		const FConfig& InConfig, FString& OutError);
	void Stop(const FString& Reason);

	/** Accepts the client, reads its messages, advances time and publishes the current frame. */
	void Pump(double DeltaSeconds);

	/** Builds the frame that would be published right now, without a connected client. */
	bool BuildCurrentFrame(FMtoUCameraSyncFrameSample& OutFrame, FString& OutError) { return BuildFrame(OutFrame, OutError); }

	/** Unreal-owned time control. */
	void SetDisplayFrame(double DisplayFrame);
	void Play();
	void Pause();
	void SetPlayRate(double InPlayRate);
	void SetLoop(bool bInLoop);
	bool IsPlaying() const;
	bool IsEditorSource() const { return bEditorSource; }
	double GetDisplayFrame() const { return CurrentDisplayFrame; }

	void SetFallbackCamera(UCameraComponent* Camera);
	/** A disposable witness actor used only by the cross-host pose pairing test. */
	void SetPoseWitnessTarget(AActor* Actor) { PoseWitnessTarget = Actor; }
	bool HasClient() const;
	bool IsGreeted() const { return bGreeted; }
	int64 GetPublishedFrameCount() const { return PublishedFrames; }
	int64 GetFailedSendCount() const { return FailedSends; }
	int64 GetClientLineAnomalyCount() const { return ClientLineAnomalies; }
	int64 GetClientTrailingByteCount() const { return ClientTrailingBytes; }
	/** Client lines handled so far, including lines drained from the receive buffer. */
	int64 GetClientLinesProcessed() const { return ClientLinesProcessed; }
	const FString& GetLastClientLineAnomaly() const { return LastClientLineAnomaly; }
	const TArray<FAppliedReport>& GetAppliedReports() const { return AppliedReports; }
	int64 GetPairedPoseCount() const { return PairedPoses; }
	int64 GetRejectedPoseCount() const { return RejectedPoses; }
	int64 GetEvalSerial() const { return EvalSerial; }
	int64 GetFrameSerial() const { return FrameSerial; }
	int64 GetLastPairedEvalSerial() const { return LastPairedEvalSerial; }
	int64 GetConnectionSessionId() const { return ConnectionSessionId; }
	/** True once a pose report answered the evaluation target currently sampled. */
	bool IsCurrentTargetPaired() const { return EvalSerial > 0 && LastPairedEvalSerial == EvalSerial; }
	int64 GetAppliedReportsDropped() const { return AppliedReportsDropped; }
	/** Accepted sockets the listener queued and Stop() closed without adopting them. */
	int64 GetPendingClientsClosedOnStop() const { return PendingClientsClosedOnStop; }
	bool IsRunning() const { return bRunning; }
	const TArray<FString>& GetRejectedCommandTypes() const { return RejectedCommandTypes; }
	const FString& GetLastError() const { return LastError; }
	const TSharedPtr<FJsonObject>& GetLastPublishedFrame() const { return LastPublishedFrame; }
	const TSharedPtr<FJsonObject>& GetLastPublishedSession() const { return LastPublishedSession; }

	/** Test hook: process one client line exactly as a network message would be processed. */
	void HandleClientLine(const FString& Line);

private:
	bool Prepare(UWorld& InWorld, ULevelSequence& InSequence, const FConfig& InConfig, FString& OutError);
	bool StartListener(FString& OutError);
	void ApplyTimeToPlayer();
	bool BuildFrame(FMtoUCameraSyncFrameSample& OutFrame, FString& OutError);
	bool PublishCurrentFrame();
	/** `<sequence>@<tick>+<milli-tick>/<camera path>#<published camera content>`. */
	FString BuildEvalIdentity(
		const FFrameTime& TickTime, const FString& CameraPath, const FString& ContentDigest) const;
	/** Resolves the evaluated camera of the sampled time and refreshes the target generation. */
	void RefreshEvalTarget();
	FMtoUCameraSyncCutSample DescribeCut(double DisplayFrame) const;
	/**
	 * Called on the listener thread. It only queues the connection: every piece of session
	 * state belongs to the game thread, which adopts or refuses the socket in Pump().
	 */
	bool AcceptClient(FSocket* Socket, const FIPv4Endpoint& Endpoint);
	/** Adopts or refuses the queued connections. Game thread only. */
	void AdoptPendingClients();
	/** Answers a connection the session cannot serve, then closes it. */
	void RefuseClient(FSocket& Socket, const FString& Category, const FString& Detail);
	void DestroyClientSocket(FSocket& Socket);
	void ReadClientLines();
	/** Handles the complete lines already buffered, within the remaining line budget. */
	bool DrainBufferedClientLines(int32& LinesHandled);
	/** Receives more input within the per-pump byte budget; false when none is available. */
	bool ReceiveClientBytes(int32& BytesReceived);
	void DropClient();
	/** Sends the closing line, ends the stream and reads what the client already sent. */
	void EndClient();
	void SendJson(const TSharedRef<FJsonObject>& Object);
	void SendError(const FString& Category, const FString& Detail);
	double PlaybackEndDisplayFrame() const;
	double PlaybackStartDisplayFrame() const;

	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<ULevelSequence> Sequence;
	TWeakObjectPtr<ULevelSequencePlayer> Player;
	TWeakPtr<ISequencer> EditorSequencer;
	bool bEditorSource = false;
	TWeakObjectPtr<AActor> SequenceActor;
	TWeakObjectPtr<UCameraComponent> FallbackCamera;
	TWeakObjectPtr<AActor> PoseWitnessTarget;

	FConfig Config;
	TUniquePtr<FTcpListener> Listener;
	/** One connection the listener thread accepted, awaiting the game thread's decision. */
	struct FPendingClient
	{
		FSocket* Socket = nullptr;
		FString Host;
	};
	/** Written only by the listener thread, drained only by the game thread. */
	TQueue<FPendingClient, EQueueMode::Spsc> PendingClients;
	FSocket* ClientSocket = nullptr;
	TArray<uint8> ReceiveBytes;
	FString ClientHost;
	double ClientSceneFps = 0.0;
	FString ClientTimeUnit;
	bool bGreeted = false;
	bool bRunning = false;

	double CurrentDisplayFrame = 0.0;
	double LastAppliedDisplayFrame = -1.0;
	double LastPublishedDisplayFrame = -1.0;
	double PlayRate = 1.0;
	bool bPlaying = false;
	bool bLoop = false;

	double SecondsSincePublish = 0.0;
	int64 FrameSerial = 0;
	int64 EvalSerial = 0;
	double LastEvalTargetDisplayFrame = -1.0;
	FString EvalTargetIdentity;
	/** The evaluated camera of the current target, captured once per target refresh. */
	TWeakObjectPtr<UCameraComponent> EvalTargetCamera;
	FMtoUCameraSyncCameraSample EvalTargetCameraSample;
	FMtoUCameraSyncViewSample EvalTargetView;
	FMtoUCameraSyncProjectionSample EvalTargetProjection;
	FString EvalTargetContentDigest;
	FString EvalTargetCaptureError;
	bool bEvalTargetCaptured = false;
	int64 ConnectionSessionId = 0;
	int64 PublishedFrames = 0;
	int64 FailedSends = 0;
	int64 ClientLineAnomalies = 0;
	int64 ClientTrailingBytes = 0;
	int64 ClientLinesProcessed = 0;
	int64 PairedPoses = 0;
	int64 RejectedPoses = 0;
	int64 LastPairedSerial = 0;
	int64 LastPairedEvalSerial = 0;
	int64 AppliedReportsDropped = 0;
	int64 PendingClientsClosedOnStop = 0;
	FString LastClientLineAnomaly;
	bool bCameraMissingReported = false;

	TArray<FAppliedReport> AppliedReports;
	TArray<FString> RejectedCommandTypes;
	FString LastError;
	TSharedPtr<FJsonObject> LastPublishedFrame;
	TSharedPtr<FJsonObject> LastPublishedSession;

	FFrameRate DisplayRate = FFrameRate(24, 1);
	FFrameRate TickResolution = FFrameRate(24000, 1);
	FFrameNumber PlaybackStartTick = FFrameNumber(0);
	FFrameNumber PlaybackEndTick = FFrameNumber(0);
};
