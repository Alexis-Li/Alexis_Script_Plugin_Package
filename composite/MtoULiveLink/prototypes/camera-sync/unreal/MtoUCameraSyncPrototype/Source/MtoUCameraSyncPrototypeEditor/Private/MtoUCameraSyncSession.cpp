// MtoU camera sync prototype (Issue 52 verification). Editor-only prototype code.

#include "MtoUCameraSyncSession.h"

#include "Camera/CameraComponent.h"
#include "Common/TcpListener.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "LevelSequencePlayer.h"
#include "MtoUCameraSyncCapture.h"
#include "MovieScene.h"
#include "MovieSceneSequencePlaybackSettings.h"
#include "MovieSceneSequencePlayer.h"
#include "MovieSceneTrack.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Tracks/MovieSceneCameraCutTrack.h"

namespace
{
const TCHAR* ProtocolName = TEXT("MtoUCameraSync");

/** Copies the first complete JSON object out of a line, ignoring anything after it. */
bool ExtractFirstJsonObject(const FString& Line, FString& OutObject)
{
	int32 Start = INDEX_NONE;
	for (int32 Index = 0; Index < Line.Len(); ++Index)
	{
		if (Line[Index] == TEXT('{'))
		{
			Start = Index;
			break;
		}
	}
	if (Start == INDEX_NONE)
	{
		return false;
	}
	int32 Depth = 0;
	bool bInString = false;
	bool bEscaped = false;
	for (int32 Index = Start; Index < Line.Len(); ++Index)
	{
		const TCHAR Character = Line[Index];
		if (bInString)
		{
			if (bEscaped)
			{
				bEscaped = false;
			}
			else if (Character == TEXT('\\'))
			{
				bEscaped = true;
			}
			else if (Character == TEXT('"'))
			{
				bInString = false;
			}
			continue;
		}
		if (Character == TEXT('"'))
		{
			bInString = true;
		}
		else if (Character == TEXT('{'))
		{
			++Depth;
		}
		else if (Character == TEXT('}'))
		{
			--Depth;
			if (Depth == 0)
			{
				OutObject = Line.Mid(Start, Index - Start + 1);
				return true;
			}
		}
	}
	return false;
}
/**
 * The prototype protocol version. Version 2 separates the evaluation identity
 * (target generation) from the transport serial: the frame carries `eval_serial`
 * and `eval_identity`, and only a report answering the current target may move
 * the pose witness.
 */
const int32 ProtocolVersion = 2;
/**
 * Bounded transport work per pump. Complete buffered lines are drained before every
 * receive, so a burst larger than this budget keeps draining on later pumps even when the
 * sender goes silent afterwards.
 */
const int32 MaxClientLinesPerPump = 64;
/** Bytes one pump may receive, so a flooding client cannot turn an editor tick into unbounded work. */
const int32 MaxClientBytesPerPump = 256 * 1024;
/** A client message may not exceed this many UTF-8 bytes; a longer one fails the session closed. */
const int32 MaxClientMessageBytes = 64 * 1024;
/** One socket read; the receive buffer holds at most one maximum message plus a read. */
const int32 MaxClientReceiveChunkBytes = 4096;
/** Bounded evidence queue; evictions are counted instead of growing without limit. */
const int32 MaxStoredAppliedReports = 512;
const uint8 NewlineByte = 0x0A;

FString FrameTimeToJsonLine(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	Text.AppendChar(TEXT('\n'));
	return Text;
}

double DisplayFrameOfTick(const FFrameRate& TickResolution, const FFrameRate& DisplayRate, const FFrameNumber& Tick)
{
	return TickResolution.AsSeconds(FFrameTime(Tick)) * DisplayRate.AsDecimal();
}
}  // namespace

FMtoUCameraSyncSession::FMtoUCameraSyncSession() = default;

FMtoUCameraSyncSession::~FMtoUCameraSyncSession()
{
	Stop(TEXT("session destroyed"));
}

bool FMtoUCameraSyncSession::Start(
	UWorld& InWorld,
	ULevelSequence& InSequence,
	const FConfig& InConfig,
	FString& OutError)
{
	if (!Prepare(InWorld, InSequence, InConfig, OutError))
	{
		return false;
	}

	FMovieSceneSequencePlaybackSettings Settings;
	Settings.bAutoPlay = false;
	Settings.FinishCompletionStateOverride = EMovieSceneCompletionModeOverride::ForceRestoreState;
	Settings.bDisableCameraCuts = false;
	Settings.bPauseAtEnd = true;

	ALevelSequenceActor* SpawnedActor = nullptr;
	ULevelSequencePlayer* NewPlayer = ULevelSequencePlayer::CreateLevelSequencePlayer(
		&InWorld, &InSequence, Settings, SpawnedActor);
	if (!NewPlayer)
	{
		OutError = TEXT("could not create a level sequence player");
		return false;
	}
	if (SpawnedActor)
	{
		SpawnedActor->SetFlags(RF_Transient);
	}
	Player = NewPlayer;
	SequenceActor = SpawnedActor;
	if (!StartListener(OutError))
	{
		NewPlayer->Stop();
		if (SpawnedActor) { SpawnedActor->Destroy(); }
		Player = nullptr;
		SequenceActor = nullptr;
		return false;
	}
	ApplyTimeToPlayer();
	return true;
}

bool FMtoUCameraSyncSession::StartFromEditor(
	UWorld& InWorld,
	const TSharedRef<ISequencer>& Sequencer,
	const FConfig& InConfig,
	FString& OutError)
{
	ULevelSequence* Root = Cast<ULevelSequence>(Sequencer->GetRootMovieSceneSequence());
	if (!Root)
	{
		OutError = TEXT("the open editor Sequencer has no root Level Sequence");
		return false;
	}
	if (!Prepare(InWorld, *Root, InConfig, OutError))
	{
		return false;
	}
	bEditorSource = true;
	EditorSequencer = Sequencer;
	const FQualifiedFrameTime Global = Sequencer->GetGlobalTime();
	CurrentDisplayFrame = FFrameRate::TransformTime(Global.Time, Global.Rate, DisplayRate).AsDecimal();
	if (!StartListener(OutError))
	{
		EditorSequencer.Reset();
		bEditorSource = false;
		return false;
	}
	return true;
}

bool FMtoUCameraSyncSession::Prepare(
	UWorld& InWorld,
	ULevelSequence& InSequence,
	const FConfig& InConfig,
	FString& OutError)
{
	if (bRunning)
	{
		OutError = TEXT("session already running");
		return false;
	}
	UMovieScene* MovieScene = InSequence.GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("sequence has no movie scene");
		return false;
	}

	World = &InWorld;
	Sequence = &InSequence;
	Config = InConfig;

	DisplayRate = MovieScene->GetDisplayRate();
	TickResolution = MovieScene->GetTickResolution();
	const TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
	PlaybackStartTick = Range.HasLowerBound() ? Range.GetLowerBoundValue() : FFrameNumber(0);
	PlaybackEndTick = Range.HasUpperBound() ? Range.GetUpperBoundValue() : FFrameNumber(0);
	CurrentDisplayFrame = PlaybackStartDisplayFrame();
	return true;
}

bool FMtoUCameraSyncSession::StartListener(FString& OutError)
{
	const TSharedRef<FIPv4Endpoint> Endpoint =
		MakeShared<FIPv4Endpoint>(FIPv4Address(127, 0, 0, 1), Config.Port);
	Listener = MakeUnique<FTcpListener>(*Endpoint);
	if (!Listener->IsActive())
	{
		Listener.Reset();
		OutError = FString::Printf(TEXT("could not listen on 127.0.0.1:%u"), Config.Port);
		return false;
	}
	Listener->OnConnectionAccepted().BindRaw(this, &FMtoUCameraSyncSession::AcceptClient);

	bRunning = true;
	LastError.Reset();
	return true;
}

void FMtoUCameraSyncSession::Stop(const FString& Reason)
{
	if (!bRunning)
	{
		return;
	}
	if (ClientSocket)
	{
		const TSharedRef<FJsonObject> End = MakeShared<FJsonObject>();
		End->SetStringField(TEXT("type"), TEXT("end"));
		End->SetStringField(TEXT("reason"), Reason);
		SendJson(End);
		DropClient();
	}
	// Connections the listener queued but the game thread never adopted.
	FPendingClient Pending;
	while (PendingClients.Dequeue(Pending))
	{
		if (Pending.Socket)
		{
			DestroyClientSocket(*Pending.Socket);
		}
	}
	if (Listener)
	{
		Listener->Stop();
		Listener.Reset();
	}
	if (ULevelSequencePlayer* ResolvedPlayer = Player.Get())
	{
		ResolvedPlayer->Stop();
	}
	if (AActor* Actor = SequenceActor.Get())
	{
		Actor->Destroy();
	}
	SequenceActor = nullptr;
	Player = nullptr;
	EditorSequencer.Reset();
	bEditorSource = false;
	bRunning = false;
	bPlaying = false;
	bGreeted = false;
}

void FMtoUCameraSyncSession::SetFallbackCamera(UCameraComponent* Camera)
{
	FallbackCamera = Camera;
}

void FMtoUCameraSyncSession::Pump(double DeltaSeconds)
{
	if (!bRunning)
	{
		return;
	}

	// The Sequencer target is sampled before client reports are processed, so a
	// report for a frame that has already been replaced on the editor timeline
	// is judged against the new target instead of the stale publication.
	if (bEditorSource)
	{
		const TSharedPtr<ISequencer> Sequencer = EditorSequencer.Pin();
		if (!Sequencer.IsValid() || Sequencer->GetRootMovieSceneSequence() != Sequence.Get())
		{
			Stop(TEXT("editor Sequencer closed or changed sequence"));
			return;
		}
		const FQualifiedFrameTime Global = Sequencer->GetGlobalTime();
		CurrentDisplayFrame = FFrameRate::TransformTime(Global.Time, Global.Rate, DisplayRate).AsDecimal();
	}
	else if (bPlaying)
	{
		const double End = PlaybackEndDisplayFrame();
		CurrentDisplayFrame += DeltaSeconds * DisplayRate.AsDecimal() * PlayRate;
		if (CurrentDisplayFrame >= End)
		{
			if (bLoop)
			{
				const double Start = PlaybackStartDisplayFrame();
				const double Span = FMath::Max(End - Start, 1.0);
				CurrentDisplayFrame = Start + FMath::Fmod(CurrentDisplayFrame - Start, Span);
			}
			else
			{
				CurrentDisplayFrame = End;
				bPlaying = false;
			}
		}
	}

	// The evaluation target is made current before any client report is processed:
	// a report for a frame the timeline has already left is judged against the new
	// target instead of the publication it happened to answer.
	if (!bEditorSource)
	{
		ApplyTimeToPlayer();
	}
	RefreshEvalTarget();
	AdoptPendingClients();
	ReadClientLines();

	if (bGreeted)
	{
		SecondsSincePublish += DeltaSeconds;
		const bool bTimeChanged = !FMath::IsNearlyEqual(LastPublishedDisplayFrame, CurrentDisplayFrame, 1e-6);
		if (bTimeChanged || SecondsSincePublish >= Config.PublishIntervalSeconds)
		{
			PublishCurrentFrame();
		}
	}
}

void FMtoUCameraSyncSession::ApplyTimeToPlayer()
{
	if (bEditorSource) { return; }
	ULevelSequencePlayer* ResolvedPlayer = Player.Get();
	if (!ResolvedPlayer)
	{
		return;
	}
	if (FMath::IsNearlyEqual(LastAppliedDisplayFrame, CurrentDisplayFrame, 0.0001))
	{
		return;
	}
	ResolvedPlayer->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(
		FFrameTime::FromDecimal(CurrentDisplayFrame), EUpdatePositionMethod::Jump));
	LastAppliedDisplayFrame = CurrentDisplayFrame;
}

FString FMtoUCameraSyncSession::BuildEvalIdentity(
	const FFrameTime& TickTime, const FString& CameraPath, const FString& ContentDigest) const
{
	// The time in the identity is the tick-resolution sampling the report's time fields are
	// checked against, at milli-tick precision: times closer than one milli-tick are the
	// same target, and the camera content digest decides everything inside a tick.
	const int64 Tick = TickTime.GetFrame().Value;
	const int32 MilliTick = FMath::Clamp(
		FMath::RoundToInt(TickTime.GetSubFrame() * 1000.0f), 0, 999);
	return FString::Printf(TEXT("%s@%lld+%03d/%s#%s"),
		*Sequence->GetName(), Tick, MilliTick,
		CameraPath.IsEmpty() ? TEXT("-") : *CameraPath, *ContentDigest);
}

void FMtoUCameraSyncSession::RefreshEvalTarget()
{
	ULevelSequencePlayer* ResolvedPlayer = Player.Get();
	const TSharedPtr<ISequencer> Sequencer = EditorSequencer.Pin();
	if (!Sequence.IsValid() || (!ResolvedPlayer && !Sequencer.IsValid()))
	{
		return;
	}

	// The editor evaluates on its own tick, so a moved playhead can still report the
	// previous shot's camera. One forced evaluation keeps the resolved camera in step
	// with the sampled time before the target is compared against a client report.
	if (Sequencer.IsValid()
		&& !FMath::IsNearlyEqual(LastEvalTargetDisplayFrame, CurrentDisplayFrame, 1e-6))
	{
		LastEvalTargetDisplayFrame = CurrentDisplayFrame;
		Sequencer->ForceEvaluate();
	}

	const FQualifiedFrameTime Current = Sequencer.IsValid()
		? Sequencer->GetGlobalTime() : ResolvedPlayer->GetCurrentTime();
	UCameraComponent* CutCamera = Sequencer.IsValid()
		? Sequencer->GetLastEvaluatedCameraCut().Get()
		: ResolvedPlayer->GetActiveCameraComponent();
	UCameraComponent* Camera = CutCamera ? CutCamera : FallbackCamera.Get();

	// The target covers the camera *content*, not only its object path: editing the same
	// camera at the same sequence time (transform, focal length, filmback, offsets) has to
	// supersede reports for the previous content. The capture is cached, so the frame
	// published for this pump carries exactly the state the identity was derived from.
	EvalTargetCamera = Camera;
	EvalTargetCaptureError.Reset();
	bEvalTargetCaptured = Camera != nullptr
		&& FMtoUCameraSyncCapture::CaptureCamera(*Camera, Config.OutputResolution,
			EvalTargetCameraSample, EvalTargetView, EvalTargetProjection, EvalTargetCaptureError);
	EvalTargetContentDigest = bEvalTargetCaptured
		? MtoUCameraSyncCameraContentDigest(EvalTargetCameraSample)
		: FString(TEXT("none"));

	// A heartbeat repeats a target and keeps its evaluation serial. Only a real jump, cut,
	// camera change or change of the camera content starts a new generation and supersedes
	// in-flight results.
	const FString Identity = BuildEvalIdentity(
		FFrameRate::TransformTime(Current.Time, Current.Rate, TickResolution),
		Camera ? Camera->GetPathName() : FString(), EvalTargetContentDigest);
	if (Identity != EvalTargetIdentity)
	{
		EvalTargetIdentity = Identity;
		++EvalSerial;
	}
}

void FMtoUCameraSyncSession::SetDisplayFrame(double DisplayFrame)
{
	if (bEditorSource) { return; }
	CurrentDisplayFrame = DisplayFrame;
	bPlaying = false;
}

void FMtoUCameraSyncSession::Play()
{
	if (bEditorSource) { return; }
	if (IsPlaying())
	{
		return;
	}
	if (CurrentDisplayFrame >= PlaybackEndDisplayFrame())
	{
		CurrentDisplayFrame = PlaybackStartDisplayFrame();
	}
	bPlaying = true;
	LastAppliedDisplayFrame = -1.0;
}

void FMtoUCameraSyncSession::Pause()
{
	if (bEditorSource) { return; }
	bPlaying = false;
}

bool FMtoUCameraSyncSession::IsPlaying() const
{
	return bEditorSource ? ULevelSequenceEditorBlueprintLibrary::IsPlaying() : bPlaying;
}

void FMtoUCameraSyncSession::SetPlayRate(double InPlayRate)
{
	if (bEditorSource) { return; }
	PlayRate = InPlayRate;
}

void FMtoUCameraSyncSession::SetLoop(bool bInLoop)
{
	if (bEditorSource) { return; }
	bLoop = bInLoop;
}

double FMtoUCameraSyncSession::PlaybackStartDisplayFrame() const
{
	return DisplayFrameOfTick(TickResolution, DisplayRate, PlaybackStartTick);
}

double FMtoUCameraSyncSession::PlaybackEndDisplayFrame() const
{
	return DisplayFrameOfTick(TickResolution, DisplayRate, PlaybackEndTick);
}

bool FMtoUCameraSyncSession::HasClient() const
{
	return ClientSocket != nullptr;
}

bool FMtoUCameraSyncSession::AcceptClient(FSocket* Socket, const FIPv4Endpoint& Endpoint)
{
	// The listener thread owns no session state: it hands the accepted socket to the game
	// thread, which adopts it or answers SESSION_BUSY in Pump(). Returning true keeps the
	// socket for that decision instead of letting the listener destroy it.
	FPendingClient Pending;
	Pending.Socket = Socket;
	Pending.Host = Endpoint.ToString();
	PendingClients.Enqueue(MoveTemp(Pending));
	return true;
}

void FMtoUCameraSyncSession::AdoptPendingClients()
{
	FPendingClient Pending;
	while (PendingClients.Dequeue(Pending))
	{
		if (!Pending.Socket)
		{
			continue;
		}
		if (ClientSocket != nullptr)
		{
			RefuseClient(*Pending.Socket, TEXT("SESSION_BUSY"), TEXT("one Maya client at a time"));
			continue;
		}
		ClientSocket = Pending.Socket;
		++ConnectionSessionId;
		LastPairedSerial = 0;
		LastPairedEvalSerial = 0;
		LastPublishedFrame.Reset();
		ClientHost = Pending.Host;
		ReceiveBytes.Reset();
		bGreeted = false;
	}
}

void FMtoUCameraSyncSession::RefuseClient(
	FSocket& Socket, const FString& Category, const FString& Detail)
{
	const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("type"), TEXT("error"));
	Error->SetStringField(TEXT("category"), Category);
	Error->SetStringField(TEXT("detail"), Detail);
	Error->SetStringField(TEXT("protocol"), ProtocolName);
	Error->SetNumberField(TEXT("version"), ProtocolVersion);
	const FString Line = FrameTimeToJsonLine(Error);
	const FTCHARToUTF8 Utf8(*Line);
	int32 Sent = 0;
	Socket.Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Sent);
	DestroyClientSocket(Socket);
}

void FMtoUCameraSyncSession::DestroyClientSocket(FSocket& Socket)
{
	Socket.Close();
	if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
	{
		Sockets->DestroySocket(&Socket);
	}
}

void FMtoUCameraSyncSession::DropClient()
{
	if (!ClientSocket)
	{
		return;
	}
	DestroyClientSocket(*ClientSocket);
	ClientSocket = nullptr;
	bGreeted = false;
	// Partial input belongs to the connection that sent it; a new client must not inherit
	// half a line from the previous one.
	ReceiveBytes.Reset();
}

void FMtoUCameraSyncSession::ReadClientLines()
{
	if (!ClientSocket)
	{
		return;
	}
	if (ClientSocket->GetConnectionState() != SCS_Connected)
	{
		DropClient();
		return;
	}

	// Complete lines that are already buffered are drained before every receive. A burst
	// larger than the per-pump line budget therefore keeps draining on later pumps even
	// when the sender has gone silent, and the receive buffer stays bounded by one
	// message plus one read.
	int32 LinesHandled = 0;
	int32 BytesReceived = 0;
	while (ClientSocket && LinesHandled < MaxClientLinesPerPump)
	{
		if (!DrainBufferedClientLines(LinesHandled))
		{
			return;
		}
		if (LinesHandled >= MaxClientLinesPerPump || BytesReceived >= MaxClientBytesPerPump)
		{
			// The remainder stays buffered for the next pump instead of growing an
			// unbounded backlog of stale targets inside one editor tick.
			break;
		}
		if (!ReceiveClientBytes(BytesReceived))
		{
			break;
		}
	}
}

bool FMtoUCameraSyncSession::DrainBufferedClientLines(int32& LinesHandled)
{
	int32 Consumed = 0;
	while (LinesHandled < MaxClientLinesPerPump)
	{
		int32 NewlineIndex = INDEX_NONE;
		for (int32 Index = Consumed; Index < ReceiveBytes.Num(); ++Index)
		{
			if (ReceiveBytes[Index] == NewlineByte)
			{
				NewlineIndex = Index;
				break;
			}
		}

		// A message that carries no complete line inside the cap fails the session closed:
		// the alternative is a buffer that grows with whatever the client streams.
		const int32 PendingLength =
			(NewlineIndex == INDEX_NONE ? ReceiveBytes.Num() : NewlineIndex) - Consumed;
		if (PendingLength > MaxClientMessageBytes)
		{
			SendError(TEXT("CLIENT_MESSAGE_TOO_LARGE"), FString::Printf(
				TEXT("a client message exceeded %d bytes without a complete line"),
				MaxClientMessageBytes));
			DropClient();
			return false;
		}
		if (NewlineIndex == INDEX_NONE)
		{
			break;
		}

		if (NewlineIndex > Consumed)
		{
			// Frame at the byte level: a multi-byte UTF-8 character can straddle two reads,
			// so only complete lines are converted to text.
			const FUTF8ToTCHAR Converted(
				reinterpret_cast<const ANSICHAR*>(ReceiveBytes.GetData() + Consumed),
				NewlineIndex - Consumed);
			// The converter's buffer is length-delimited, not a C string.
			FString Line(Converted.Length(), Converted.Get());
			Line.TrimStartInline();
			if (Line.EndsWith(TEXT("\r"))) { Line.LeftChopInline(1); }
			if (!Line.IsEmpty())
			{
				HandleClientLine(Line);
				++LinesHandled;
				++ClientLinesProcessed;
				if (!ClientSocket)
				{
					// The line ended the session ("bye" or a refused transport).
					ReceiveBytes.Reset();
					return false;
				}
			}
		}
		Consumed = NewlineIndex + 1;
	}
	if (Consumed > 0)
	{
		ReceiveBytes.RemoveAt(0, Consumed, EAllowShrinking::No);
	}
	return true;
}

bool FMtoUCameraSyncSession::ReceiveClientBytes(int32& BytesReceived)
{
	// The budget bounds one pump, the capacity bounds the buffer: a read is only issued
	// when both leave room, so no path can grow the buffer past one message plus a read.
	const int32 ChunkSize = FMath::Min3(
		MaxClientReceiveChunkBytes,
		MaxClientBytesPerPump - BytesReceived,
		MaxClientMessageBytes + MaxClientReceiveChunkBytes - ReceiveBytes.Num());
	if (ChunkSize <= 0)
	{
		return false;
	}

	uint32 Pending = 0;
	const bool bHasPendingData = ClientSocket->HasPendingData(Pending) && Pending > 0;
	if (!bHasPendingData
		&& !ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
	{
		return false;
	}

	uint8 Buffer[MaxClientReceiveChunkBytes];
	int32 Read = 0;
	if (!ClientSocket->Recv(Buffer, ChunkSize, Read, ESocketReceiveFlags::None))
	{
		if (!bHasPendingData && Read == 0)
		{
			// Readable with nothing to read is the end of the stream. A client that closes
			// cleanly leaves the socket readable, and the engine's connection state keeps
			// reporting it as connected, so the EOF has to be probed.
			DropClient();
		}
		return false;
	}
	if (Read <= 0)
	{
		return false;
	}
	ReceiveBytes.Append(Buffer, Read);
	BytesReceived += Read;
	return true;
}

void FMtoUCameraSyncSession::HandleClientLine(const FString& Line)
{
	TSharedPtr<FJsonObject> Object;
	FString AcceptedJson = Line;
	bool bLineAnomaly = false;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		// The wire contract is one JSON object per line. A client that appends extra
		// bytes after a complete object is tolerated, but every discarded byte is
		// counted and reported: silently ignoring them would hide a broken client.
		FString ObjectText;
		if (Line.StartsWith(TEXT("{")) && ExtractFirstJsonObject(Line, ObjectText))
		{
			TSharedPtr<FJsonObject> Tolerated;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ObjectText), Tolerated)
				&& Tolerated.IsValid())
			{
				++ClientLineAnomalies;
				bLineAnomaly = true;
				const FString Trailing = Line.Mid(ObjectText.Len());
				const int32 TrailingByteCount = FTCHARToUTF8(*Trailing).Length();
				ClientTrailingBytes += TrailingByteCount;
				LastClientLineAnomaly = FString::Printf(
					TEXT("discarded %d UTF-8 bytes after a complete client message: '%s'"),
					TrailingByteCount, *Trailing.Left(64));
				Object = Tolerated;
				AcceptedJson = ObjectText;
			}
		}
		if (!Object.IsValid())
		{
			SendError(TEXT("MALFORMED_MESSAGE"), FString::Printf(
				TEXT("client message is not a JSON object (%d characters, reader said '%s')"),
				Line.Len(), *Reader->GetErrorMessage()));
			return;
		}
	}

	FString Type;
	if (!Object->TryGetStringField(TEXT("type"), Type))
	{
		SendError(TEXT("MALFORMED_MESSAGE"), TEXT("client message has no type"));
		return;
	}

	if (Type == TEXT("hello"))
	{
		FString Protocol;
		int32 Version = 0;
		Object->TryGetStringField(TEXT("protocol"), Protocol);
		Object->TryGetNumberField(TEXT("version"), Version);
		if (Protocol != ProtocolName || Version != ProtocolVersion)
		{
			SendError(TEXT("PROTOCOL_MISMATCH"), FString::Printf(
				TEXT("expected %s version %d"), ProtocolName, ProtocolVersion));
			return;
		}
		Object->TryGetStringField(TEXT("host"), ClientHost);
		Object->TryGetNumberField(TEXT("scene_fps"), ClientSceneFps);
		Object->TryGetStringField(TEXT("time_unit"), ClientTimeUnit);
		bGreeted = true;

		FMtoUCameraSyncSessionSample Sample;
		Sample.SequenceName = Sequence.IsValid() ? Sequence->GetName() : FString();
		Sample.SequencePath = Sequence.IsValid() ? Sequence->GetPathName() : FString();
		Sample.OutputResolution = Config.OutputResolution;
		Sample.ResolutionSource = Config.ResolutionSource;
		Sample.DisplayRate = DisplayRate;
		Sample.TickResolution = TickResolution;
		Sample.PlaybackStart = FMath::FloorToInt32(PlaybackStartDisplayFrame());
		Sample.PlaybackEnd = FMath::FloorToInt32(PlaybackEndDisplayFrame());
		Sample.FarClipFallbackCm = Config.FarClipFallbackCm;
		Sample.Cut = DescribeCut(CurrentDisplayFrame);
		Sample.Cut.Stage = Sample.Cut.bActive ? Sample.Cut.Stage : TEXT("pending");

		FMtoUCameraSyncFrameSample Probe;
		if (BuildFrame(Probe, LastError))
		{
			Sample.ApertureResolution = FIntPoint(
				Probe.Projection.ViewRect.Width(), Probe.Projection.ViewRect.Height());
			for (const FMtoUCameraSyncMarkerSample& Marker : Probe.Markers)
			{
				Sample.MarkerNames.Add(Marker.Name);
			}
		}
		const TSharedRef<FJsonObject> Session = MtoUCameraSyncSerializeSession(Sample);
		Session->SetNumberField(TEXT("port"), Config.Port);
		LastPublishedSession = Session;
		SendJson(Session);
		return;
	}

	if (Type == TEXT("applied"))
	{
		FAppliedReport Report;
		double Serial = 0.0;
		double EvalSerialValue = 0.0;
		Object->TryGetNumberField(TEXT("frame_serial"), Serial);
		Object->TryGetNumberField(TEXT("eval_serial"), EvalSerialValue);
		Report.Serial = static_cast<int64>(Serial);
		Report.EvalSerial = static_cast<int64>(EvalSerialValue);
		Object->TryGetStringField(TEXT("eval_identity"), Report.EvalIdentity);
		Object->TryGetStringField(TEXT("status"), Report.Status);
		Object->TryGetNumberField(TEXT("maya_frame"), Report.MayaFrame);
		Report.RawJson = AcceptedJson;
		const TSharedPtr<FJsonObject>* Pose = nullptr;
		if (Object->TryGetObjectField(TEXT("pose"), Pose) && Pose && Pose->IsValid())
		{
			double SessionId = 0.0;
			double UnrealFrame = 0.0;
			double MayaOrigin = 0.0;
			double SampledMayaFrame = 0.0;
			double PoseValue = 0.0;
			FString SequenceName;
			FString CameraPath;
			const TSharedPtr<FJsonObject>* PublishedTime = nullptr;
			const TSharedPtr<FJsonObject>* PublishedCamera = nullptr;
			const bool bHasFields = Object->TryGetNumberField(TEXT("session"), SessionId)
				&& Object->TryGetNumberField(TEXT("unreal_display_frame"), UnrealFrame)
				&& Object->TryGetNumberField(TEXT("maya_origin_frame"), MayaOrigin)
				&& Object->TryGetStringField(TEXT("sequence"), SequenceName)
				&& Object->TryGetStringField(TEXT("unreal_camera_path"), CameraPath)
				&& (*Pose)->TryGetNumberField(TEXT("sampled_maya_frame"), SampledMayaFrame)
				&& (*Pose)->TryGetNumberField(TEXT("translate_x"), PoseValue)
				&& LastPublishedFrame.IsValid()
				&& LastPublishedFrame->TryGetObjectField(TEXT("time"), PublishedTime)
				&& LastPublishedFrame->TryGetObjectField(TEXT("camera"), PublishedCamera);
			if (!bHasFields)
			{
				Report.PairingError = TEXT("pose report or published frame lacks an identity field");
			}
			else
			{
				const double PublishedDisplayFrame = (*PublishedTime)->GetNumberField(TEXT("display_frame"));
				const double ExpectedMayaFrame = MayaOrigin
					+ (PublishedDisplayFrame - PlaybackStartDisplayFrame())
					* ClientSceneFps / DisplayRate.AsDecimal();
				if (bLineAnomaly || Report.Status != TEXT("applied")
					|| Serial != static_cast<double>(Report.Serial)
					|| !FMath::IsFinite(ClientSceneFps) || ClientSceneFps <= 0.0
					|| !FMath::IsFinite(MayaOrigin) || !FMath::IsFinite(UnrealFrame)
					|| !FMath::IsFinite(Report.MayaFrame) || !FMath::IsFinite(SampledMayaFrame)
					|| SessionId != static_cast<double>(ConnectionSessionId)
					|| !Sequence.IsValid() || SequenceName != Sequence->GetName()
					|| CameraPath != (*PublishedCamera)->GetStringField(TEXT("path"))
					|| Report.Serial <= 0 || Report.Serial > FrameSerial
					|| Report.Serial <= LastPairedSerial
					|| Report.EvalSerial != EvalSerial
					|| Report.EvalIdentity != EvalTargetIdentity
					|| !FMath::IsNearlyEqual(PublishedDisplayFrame, CurrentDisplayFrame, 1e-5)
					|| !FMath::IsNearlyEqual(UnrealFrame, PublishedDisplayFrame, 1e-5)
					|| !FMath::IsNearlyEqual(Report.MayaFrame, ExpectedMayaFrame, 1e-5)
					|| !FMath::IsNearlyEqual(SampledMayaFrame, ExpectedMayaFrame, 1e-5)
					|| !FMath::IsFinite(PoseValue))
				{
					// A report is judged against the target sampled for this pump, not
					// against whichever publication it happened to answer: a slow report
					// for a still-current target pairs, a report the target has left does
					// not, and a replayed publication never pairs twice.
					Report.PairingError = TEXT("superseded, replayed, or mismatched pose report");
				}
			}
			if (Report.PairingError.IsEmpty())
			{
				Report.bPosePaired = true;
				Report.PoseTranslateX = PoseValue;
				LastPairedSerial = Report.Serial;
				LastPairedEvalSerial = Report.EvalSerial;
				++PairedPoses;
				if (AActor* Target = PoseWitnessTarget.Get())
				{
					// Maya +X is Unreal +Y; only the disposable test actor is changed.
					Target->SetActorLocation(FVector(0.0, PoseValue, 0.0));
				}
			}
			else
			{
				++RejectedPoses;
			}
		}
		if (AppliedReports.Num() >= MaxStoredAppliedReports)
		{
			// The report list is evidence, not session state: the oldest entries are
			// evicted and counted instead of growing without limit on a long session.
			const int32 Evicted = AppliedReports.Num() - MaxStoredAppliedReports + 1;
			AppliedReports.RemoveAt(0, Evicted, EAllowShrinking::No);
			AppliedReportsDropped += Evicted;
		}
		AppliedReports.Add(Report);
		return;
	}

	if (Type == TEXT("bye"))
	{
		DropClient();
		return;
	}

	RejectedCommandTypes.Add(Type);
	SendError(TEXT("CLIENT_MAY_NOT_CONTROL_TIME"), FString::Printf(
		TEXT("client message type '%s' is not accepted: Unreal owns time and camera"), *Type));
}

FMtoUCameraSyncCutSample FMtoUCameraSyncSession::DescribeCut(double DisplayFrame) const
{
	FMtoUCameraSyncCutSample Cut;
	const ULevelSequence* ResolvedSequence = Sequence.Get();
	if (!ResolvedSequence)
	{
		return Cut;
	}
	UMovieScene* MovieScene = ResolvedSequence->GetMovieScene();
	if (!MovieScene)
	{
		return Cut;
	}
	const UMovieSceneTrack* Track = MovieScene->GetCameraCutTrack();
	if (!Track)
	{
		Cut.Stage = TEXT("none");
		return Cut;
	}
	Cut.bHasTrack = true;
	const TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
	Cut.SectionCount = Sections.Num();

	const FFrameNumber Tick = TickResolution.AsFrameNumber(DisplayFrame / DisplayRate.AsDecimal());
	for (int32 Index = 0; Index < Sections.Num(); ++Index)
	{
		if (Sections[Index] && Sections[Index]->GetRange().Contains(Tick))
		{
			Cut.bActive = true;
			Cut.ActiveIndex = Index;
			Cut.Stage = TEXT("root");
			break;
		}
	}
	if (!Cut.bActive)
	{
		// The engine may still resolve a cut owned by a subsequence instance.
		Cut.Stage = TEXT("none");
	}
	return Cut;
}

bool FMtoUCameraSyncSession::BuildFrame(FMtoUCameraSyncFrameSample& OutFrame, FString& OutError)
{
	ULevelSequencePlayer* ResolvedPlayer = Player.Get();
	const TSharedPtr<ISequencer> Sequencer = EditorSequencer.Pin();
	UWorld* ResolvedWorld = World.Get();
	if ((!ResolvedPlayer && !Sequencer.IsValid()) || !ResolvedWorld || !Sequence.IsValid())
	{
		OutError = TEXT("session is not started");
		return false;
	}

	// The published frame has to carry the identity the receipts are judged against, and
	// the camera content that identity was derived from, so both come from the target
	// sampled for this pump.
	RefreshEvalTarget();
	if (!EvalTargetCamera.IsValid())
	{
		OutError = TEXT("no camera cut is active and no fallback camera is configured");
		return false;
	}
	if (!bEvalTargetCaptured)
	{
		OutError = EvalTargetCaptureError.IsEmpty()
			? TEXT("the evaluated camera could not be read") : EvalTargetCaptureError;
		return false;
	}

	FMtoUCameraSyncFrameSample Frame;
	Frame.Serial = FrameSerial + 1;
	Frame.EvalSerial = EvalSerial;
	Frame.EvalIdentity = EvalTargetIdentity;
	Frame.SequenceName = Sequence->GetName();
	Frame.SequencePath = Sequence->GetPathName();
	Frame.OutputResolution = Config.OutputResolution;
	Frame.ResolutionSource = Config.ResolutionSource;

	const FQualifiedFrameTime Current = Sequencer.IsValid()
		? Sequencer->GetGlobalTime() : ResolvedPlayer->GetCurrentTime();
	const FFrameTime DisplayTime = FFrameRate::TransformTime(Current.Time, Current.Rate, DisplayRate);
	Frame.Time.DisplayRate = DisplayRate;
	Frame.Time.TickResolution = TickResolution;
	Frame.Time.PlaybackStart = FMath::FloorToInt32(PlaybackStartDisplayFrame());
	Frame.Time.PlaybackEnd = FMath::FloorToInt32(PlaybackEndDisplayFrame());
	Frame.Time.DisplayFrame = DisplayTime.AsDecimal();
	Frame.Time.SourceFrame = FMath::FloorToInt32(Frame.Time.DisplayFrame);
	Frame.Time.Seconds = (Frame.Time.DisplayFrame - PlaybackStartDisplayFrame())
		/ FMath::Max(DisplayRate.AsDecimal(), 1.0);
	const FFrameTime TickTime = FFrameRate::TransformTime(Current.Time, Current.Rate, TickResolution);
	Frame.Time.Tick = TickTime.FrameNumber.Value;

	Frame.Cut = DescribeCut(Frame.Time.DisplayFrame);
	if (!Frame.Cut.bActive && EvalTargetCamera.IsValid())
	{
		Frame.Cut.bActive = true;
		Frame.Cut.Stage = TEXT("subsequence");
	}

	Frame.Camera = EvalTargetCameraSample;
	Frame.View = EvalTargetView;
	Frame.Projection = EvalTargetProjection;

	Frame.ApertureResolution = FIntPoint(
		Frame.Projection.ViewRect.Width(), Frame.Projection.ViewRect.Height());
	FMtoUCameraSyncCapture::CollectMarkers(*ResolvedWorld, Frame.Markers);
	const FIntRect ViewRect = Frame.Projection.ViewRect;
	for (FMtoUCameraSyncMarkerSample& Marker : Frame.Markers)
	{
		FVector2D Ndc;
		if (FMtoUCameraSyncCapture::ProjectToNdc(Frame.Projection.ViewProjection, Marker.LocationCm, Ndc))
		{
			Marker.bProjected = true;
			Marker.Ndc = Ndc;
			Marker.Pixel = FMtoUCameraSyncCapture::NdcToPixel(Ndc, ViewRect);
		}
	}

	Frame.EvalIdentity = EvalTargetIdentity;
	OutFrame = Frame;
	return true;
}

bool FMtoUCameraSyncSession::PublishCurrentFrame()
{
	if (!ClientSocket || !bGreeted)
	{
		return false;
	}
	FMtoUCameraSyncFrameSample Frame;
	if (!BuildFrame(Frame, LastError))
	{
		if (!bCameraMissingReported)
		{
			bCameraMissingReported = true;
			SendError(TEXT("NO_EVALUATED_CAMERA"), LastError);
		}
		return false;
	}
	bCameraMissingReported = false;

	FrameSerial = Frame.Serial;
	LastPublishedDisplayFrame = CurrentDisplayFrame;
	SecondsSincePublish = 0.0;
	++PublishedFrames;
	const TSharedRef<FJsonObject> Object = MtoUCameraSyncSerializeFrame(Frame);
	LastPublishedFrame = Object;
	SendJson(Object);
	return true;
}

void FMtoUCameraSyncSession::SendJson(const TSharedRef<FJsonObject>& Object)
{
	if (!ClientSocket)
	{
		return;
	}
	if (ClientSocket->GetConnectionState() != SCS_Connected)
	{
		// The client left: dropping the socket is not a session transport error.
		DropClient();
		return;
	}
	Object->SetStringField(TEXT("protocol"), ProtocolName);
	Object->SetNumberField(TEXT("version"), ProtocolVersion);
	Object->SetNumberField(TEXT("session"), static_cast<double>(ConnectionSessionId));
	const FString Line = FrameTimeToJsonLine(Object);
	const FTCHARToUTF8 Utf8(*Line);
	// The accepted socket is non-blocking, so a send can be refused or partially
	// accepted while the client drains its buffer. A half-written line would be a
	// malformed message for the client, so every line is either fully sent or the
	// session reports the failure.
	int32 Offset = 0;
	int32 Attempts = 0;
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	while (Offset < Utf8.Length() && FPlatformTime::Seconds() < Deadline)
	{
		int32 Sent = 0;
		++Attempts;
		if (!ClientSocket->Send(reinterpret_cast<const uint8*>(Utf8.Get()) + Offset,
			Utf8.Length() - Offset, Sent) || Sent <= 0)
		{
			if (Attempts >= 2000)
			{
				break;
			}
			FPlatformProcess::Sleep(0.001f);
			continue;
		}
		Offset += Sent;
	}
	if (Offset < Utf8.Length())
	{
		++FailedSends;
		LastError = FString::Printf(
			TEXT("TRANSPORT: sent %d of %d bytes in %d attempts"), Offset, Utf8.Length(), Attempts);
	}
}

void FMtoUCameraSyncSession::SendError(const FString& Category, const FString& Detail)
{
	LastError = FString::Printf(TEXT("%s: %s"), *Category, *Detail);
	const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("type"), TEXT("error"));
	Error->SetStringField(TEXT("category"), Category);
	Error->SetStringField(TEXT("detail"), Detail);
	SendJson(Error);
}
