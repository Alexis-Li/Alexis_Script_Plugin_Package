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
/** Bounded transport work per pump; the remainder is processed on the next tick. */
const int32 MaxClientLinesPerPump = 64;
/** Bounded evidence queue; evictions are counted instead of growing without limit. */
const int32 MaxStoredAppliedReports = 512;

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

FString FMtoUCameraSyncSession::BuildEvalIdentity(const FMtoUCameraSyncFrameSample& Frame) const
{
	return FString::Printf(TEXT("%s@%lld/%s"),
		*Frame.SequenceName, Frame.Time.Tick, *Frame.Camera.Path);
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

	FMtoUCameraSyncFrameSample Target;
	Target.SequenceName = Sequence->GetName();
	Target.Time.Tick = FFrameRate::TransformTime(Current.Time, Current.Rate, TickResolution).FrameNumber.Value;
	Target.Camera.Path = Camera ? Camera->GetPathName() : FString();

	// A heartbeat repeats a target and keeps its evaluation serial. Only a real jump,
	// cut or camera change starts a new generation and supersedes in-flight results.
	const FString Identity = BuildEvalIdentity(Target);
	if (Identity != PendingEvalIdentity)
	{
		PendingEvalIdentity = Identity;
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
	if (!bRunning || ClientSocket != nullptr)
	{
		const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("type"), TEXT("error"));
		Error->SetStringField(TEXT("category"), TEXT("SESSION_BUSY"));
		Error->SetStringField(TEXT("detail"), TEXT("one Maya client at a time"));
		const FString Line = FrameTimeToJsonLine(Error);
		int32 Sent = 0;
		const FTCHARToUTF8 Utf8(*Line);
		Socket->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Sent);
		return false;
	}
	ClientSocket = Socket;
	++ConnectionSessionId;
	LastPairedSerial = 0;
	LastPairedEvalSerial = 0;
	LastPublishedFrame.Reset();
	ClientHost = Endpoint.ToString();
	ReceiveBytes.Reset();
	bGreeted = false;
	return true;
}

void FMtoUCameraSyncSession::DropClient()
{
	if (!ClientSocket)
	{
		return;
	}
	ClientSocket->Close();
	if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
	{
		Sockets->DestroySocket(ClientSocket);
	}
	ClientSocket = nullptr;
	bGreeted = false;
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

	int32 LinesHandled = 0;
	uint32 Pending = 0;
	while (ClientSocket && LinesHandled < MaxClientLinesPerPump)
	{
		const bool bHasPendingData = ClientSocket->HasPendingData(Pending) && Pending > 0;
		if (!bHasPendingData
			&& !ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
		{
			break;
		}

		uint8 Buffer[4096];
		int32 Read = 0;
		if (!ClientSocket->Recv(Buffer, sizeof(Buffer), Read, ESocketReceiveFlags::None))
		{
			if (!bHasPendingData && Read == 0)
			{
				// Readable with nothing to read is the end of the stream. A client that
				// closes cleanly leaves the socket readable, and the engine's connection
				// state keeps reporting it as connected, so the EOF has to be probed.
				DropClient();
				return;
			}
			break;
		}
		if (Read <= 0)
		{
			break;
		}
		// Frame at the byte level: a multi-byte UTF-8 character can straddle two reads,
		// so only complete lines are converted to text.
		ReceiveBytes.Append(Buffer, Read);

		int32 LineStart = 0;
		for (int32 Index = 0; Index < ReceiveBytes.Num(); ++Index)
		{
			if (ReceiveBytes[Index] != 0x0A)
			{
				continue;
			}
			const int32 Length = Index - LineStart;
			if (Length > 0)
			{
				const FUTF8ToTCHAR Converted(
					reinterpret_cast<const ANSICHAR*>(ReceiveBytes.GetData() + LineStart), Length);
				// The converter's buffer is length-delimited, not a C string.
				FString Line(Converted.Length(), Converted.Get());
				Line.TrimStartInline();
				if (Line.EndsWith(TEXT("\r"))) { Line.LeftChopInline(1); }
				if (!Line.IsEmpty())
				{
					HandleClientLine(Line);
					++LinesHandled;
					++ClientLinesProcessed;
				}
			}
			LineStart = Index + 1;
			if (LinesHandled >= MaxClientLinesPerPump)
			{
				// The remainder stays buffered for the next pump instead of growing an
				// unbounded backlog of stale targets inside one editor tick.
				break;
			}
		}
		if (LineStart > 0)
		{
			ReceiveBytes.RemoveAt(0, LineStart, EAllowShrinking::No);
		}
	}
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
					|| Report.EvalIdentity != PendingEvalIdentity
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

	// The published frame must carry the identity of the target the receipts are
	// judged against, so it is derived here through the same path.
	RefreshEvalTarget();

	UCameraComponent* CutCamera = Sequencer.IsValid()
		? Sequencer->GetLastEvaluatedCameraCut().Get()
		: ResolvedPlayer->GetActiveCameraComponent();
	UCameraComponent* Camera = CutCamera ? CutCamera : FallbackCamera.Get();
	if (!Camera)
	{
		OutError = TEXT("no camera cut is active and no fallback camera is configured");
		return false;
	}

	FMtoUCameraSyncFrameSample Frame;
	Frame.Serial = FrameSerial + 1;
	Frame.EvalSerial = EvalSerial;
	Frame.EvalIdentity = PendingEvalIdentity;
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
	if (!Frame.Cut.bActive && CutCamera)
	{
		Frame.Cut.bActive = true;
		Frame.Cut.Stage = TEXT("subsequence");
	}

	if (!FMtoUCameraSyncCapture::CaptureCamera(
		*Camera, Config.OutputResolution, Frame.Camera, Frame.View, Frame.Projection, OutError))
	{
		return false;
	}

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

	Frame.EvalIdentity = BuildEvalIdentity(Frame);
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
