// MtoU camera sync prototype (Issue 52 verification). Editor-only prototype code.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "CineCameraSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Editor.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MtoUCameraSyncCapture.h"
#include "MtoUCameraSyncSession.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneSubTrack.h"

namespace
{
const TCHAR* MarkerTagName = TEXT("MtoUCameraSyncMarker");

struct FPrototypeWorld
{
	UWorld* World = nullptr;
	FWorldContext* Context = nullptr;

	bool Create()
	{
		World = UWorld::CreateWorld(EWorldType::Editor, false);
		if (!World)
		{
			return false;
		}
		Context = &GEngine->CreateNewWorldContext(EWorldType::Editor);
		Context->SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		return true;
	}

	~FPrototypeWorld()
	{
		if (World)
		{
			World->DestroyWorld(false);
			if (GEngine && Context)
			{
				GEngine->DestroyWorldContext(World);
			}
		}
	}
};

ULevelSequence* MakeSequence(
	const FFrameRate& DisplayRate,
	const FFrameRate& TickResolution,
	int32 StartTick,
	int32 EndTick)
{
	ULevelSequence* Sequence = NewObject<ULevelSequence>(GetTransientPackage(), NAME_None, RF_Transient);
	Sequence->Initialize();
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	MovieScene->SetDisplayRate(DisplayRate);
	MovieScene->SetTickResolutionDirectly(TickResolution);
	MovieScene->SetPlaybackRange(TRange<FFrameNumber>(
		TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(StartTick)),
		TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(EndTick))));
	return Sequence;
}

ACineCameraActor* SpawnCamera(
	UWorld& World,
	const FVector& Location,
	const FRotator& Rotation,
	float FocalLengthMm,
	float Aperture,
	float FocusDistanceCm)
{
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACineCameraActor* Actor = World.SpawnActor<ACineCameraActor>(Location, Rotation, Params);
	if (!Actor)
	{
		return nullptr;
	}
	UCineCameraComponent* Camera = Actor->GetCineCameraComponent();
	Camera->SetCurrentFocalLength(FocalLengthMm);
	Camera->SetCurrentAperture(Aperture);
	Camera->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
	Camera->FocusSettings.ManualFocusDistance = FocusDistanceCm;
	return Actor;
}

AActor* SpawnMarker(UWorld& World, const FString& Name, const FVector& Location)
{
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Marker = World.SpawnActor<AActor>(Location, FRotator::ZeroRotator, Params);
	if (Marker)
	{
		// A plain AActor has no root component, so it would report a zero location.
		USceneComponent* Root = NewObject<USceneComponent>(Marker, TEXT("MarkerRoot"));
		Marker->SetRootComponent(Root);
		Root->RegisterComponent();
		Marker->SetActorLocation(Location);
		Marker->SetActorLabel(Name);
		Marker->Tags.Add(FName(MarkerTagName));
	}
	return Marker;
}

FGuid BindActor(ULevelSequence& Sequence, UWorld& World, UObject& Object, const FString& Name)
{
	UMovieScene* MovieScene = Sequence.GetMovieScene();
	const FGuid Guid = MovieScene->AddPossessable(Name, Object.GetClass());
	Sequence.BindPossessableObject(Guid, Object, &World);
	return Guid;
}

UMovieSceneCameraCutTrack* EnsureCutTrack(ULevelSequence& Sequence)
{
	UMovieScene* MovieScene = Sequence.GetMovieScene();
	UMovieSceneTrack* Track = MovieScene->GetCameraCutTrack();
	if (!Track)
	{
		Track = MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
	}
	return Cast<UMovieSceneCameraCutTrack>(Track);
}

UMovieSceneCameraCutSection* AddCut(
	ULevelSequence& Sequence,
	const FGuid& BindingGuid,
	int32 StartTick,
	int32 EndTick)
{
	UMovieSceneCameraCutTrack* Track = EnsureCutTrack(Sequence);
	UMovieSceneCameraCutSection* Section =
		Cast<UMovieSceneCameraCutSection>(Track->CreateNewSection());
	Section->SetRange(TRange<FFrameNumber>(
		TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(StartTick)),
		TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(EndTick))));
	Section->SetCameraGuid(BindingGuid);
	Track->AddSection(*Section);
	return Section;
}

uint16 ReserveLoopbackPort()
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets)
	{
		return 0;
	}
	FSocket* Socket = Sockets->CreateSocket(NAME_Stream, TEXT("MtoU camera sync port probe"));
	if (!Socket)
	{
		return 0;
	}
	const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
	bool bValidAddress = false;
	Address->SetIp(TEXT("127.0.0.1"), bValidAddress);
	Address->SetPort(0);
	uint16 Port = 0;
	if (Socket->Bind(*Address))
	{
		Socket->GetAddress(*Address);
		Port = static_cast<uint16>(Address->GetPort());
	}
	Socket->Close();
	Sockets->DestroySocket(Socket);
	return Port;
}

bool SaveJson(const FString& Path, const FString& Text)
{
	// Force UTF-8: the evidence carries Maya diagnostics that may be non-ASCII, and the
	// default auto-detected encoding would write UTF-16.
	return FFileHelper::SaveStringToFile(
		Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString JsonToText(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	return Text;
}

bool LoadJson(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), OutObject)
		&& OutObject.IsValid();
}

/** Returns the named double field of a nested object, or Def when absent. */
double GetNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double Def)
{
	double Value = Def;
	if (Object.IsValid())
	{
		Object->TryGetNumberField(Field, Value);
	}
	return Value;
}

TSharedPtr<FJsonObject> GetObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	if (Object.IsValid() && Object->TryGetObjectField(Field, Found) && Found)
	{
		return *Found;
	}
	return nullptr;
}

/** Connects one synthetic client to a session's loopback port. */
bool ConnectLoopbackClient(uint16 Port, FSocket*& OutSocket)
{
	OutSocket = nullptr;
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets)
	{
		return false;
	}
	OutSocket = Sockets->CreateSocket(NAME_Stream, TEXT("MtoU synthetic client"));
	if (!OutSocket)
	{
		return false;
	}
	const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
	bool bValidAddress = false;
	Address->SetIp(TEXT("127.0.0.1"), bValidAddress);
	Address->SetPort(Port);
	const bool bConnected = bValidAddress && OutSocket->Connect(*Address);
	if (!bConnected)
	{
		OutSocket->Close();
		Sockets->DestroySocket(OutSocket);
		OutSocket = nullptr;
	}
	return bConnected;
}

void CloseLoopbackClient(FSocket*& Socket)
{
	if (!Socket)
	{
		return;
	}
	Socket->Close();
	if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
	{
		Sockets->DestroySocket(Socket);
	}
	Socket = nullptr;
}

/** Sends raw bytes, retrying while the socket accepts them partially. */
bool SendLoopbackBytes(FSocket& Socket, const uint8* Data, int32 Num)
{
	int32 Offset = 0;
	while (Offset < Num)
	{
		int32 Sent = 0;
		if (!Socket.Send(Data + Offset, Num - Offset, Sent) || Sent <= 0)
		{
			return false;
		}
		Offset += Sent;
	}
	return true;
}

/** Sends one protocol line, terminator included. */
bool SendLoopbackLine(FSocket& Socket, const FString& Line)
{
	const FString Text = Line + TEXT("\n");
	const FTCHARToUTF8 Utf8(*Text);
	return SendLoopbackBytes(Socket, reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

/** Pumps the session until the predicate holds or the attempt budget runs out. */
bool PumpUntil(FMtoUCameraSyncSession& Session, TFunctionRef<bool()> Predicate,
	int32 Attempts = 200, double Delta = 0.02)
{
	for (int32 Attempt = 0; Attempt < Attempts && !Predicate(); ++Attempt)
	{
		Session.Pump(Delta);
		FPlatformProcess::Sleep(0.005f);
	}
	return Predicate();
}

/** Sends the handshake and waits until the session greeted this client. */
bool GreetLoopbackClient(FMtoUCameraSyncSession& Session, FSocket& Socket)
{
	const bool bSent = SendLoopbackLine(Socket,
		TEXT("{\"type\":\"hello\",\"protocol\":\"MtoUCameraSync\",\"version\":2,")
		TEXT("\"host\":\"synthetic-test\",\"scene_fps\":24,\"time_unit\":\"film\"}"));
	return bSent && PumpUntil(Session, [&Session]() { return Session.IsGreeted(); });
}

/**
 * One applied report that answers the given publication, with every field taken from it.
 * The fixtures use equal display and scene rates with a playback start of 1001.
 */
FString MakePoseLine(const ULevelSequence& Sequence, const TSharedPtr<FJsonObject>& Published,
	int64 ReportSession, int64 ReportSerial, double MayaOrigin, double PoseValue,
	double PlaybackStart = 1001.0)
{
	const TSharedPtr<FJsonObject> Time = GetObject(Published, TEXT("time"));
	const TSharedPtr<FJsonObject> Camera = GetObject(Published, TEXT("camera"));
	const double DisplayFrame = GetNumber(Time, TEXT("display_frame"), 0.0);
	const double MayaFrame = MayaOrigin + (DisplayFrame - PlaybackStart);
	return FString::Printf(
		TEXT("{\"type\":\"applied\",\"session\":%lld,\"sequence\":\"%s\",\"frame_serial\":%lld,")
		TEXT("\"eval_serial\":%lld,\"eval_identity\":\"%s\",\"maya_frame\":%.9f,")
		TEXT("\"unreal_display_frame\":%.9f,\"unreal_camera_path\":\"%s\",")
		TEXT("\"maya_origin_frame\":%.9f,\"status\":\"applied\",\"detail\":[],\"camera\":{},")
		TEXT("\"markers\":[],\"pose\":{\"sampled_maya_frame\":%.9f,\"translate_x\":%.9f}}"),
		ReportSession, *Sequence.GetName(), ReportSerial,
		static_cast<int64>(GetNumber(Published, TEXT("eval_serial"), 0.0)),
		*Published->GetStringField(TEXT("eval_identity")),
		MayaFrame, DisplayFrame, *Camera->GetStringField(TEXT("path")),
		MayaOrigin, MayaFrame, PoseValue);
}

/** The `@<tick>` segment of an evaluation identity, without the milli-tick. */
FString EvalIdentityTickPart(const FString& Identity)
{
	int32 At = INDEX_NONE;
	int32 Plus = INDEX_NONE;
	if (!Identity.FindChar(TEXT('@'), At) || !Identity.FindChar(TEXT('+'), Plus) || Plus <= At)
	{
		return FString();
	}
	return Identity.Mid(At, Plus - At);
}

/** The `#<content digest>` segment of an evaluation identity. */
FString EvalIdentityDigestPart(const FString& Identity)
{
	int32 Hash = INDEX_NONE;
	return Identity.FindLastChar(TEXT('#'), Hash) ? Identity.Mid(Hash) : FString();
}
}  // namespace

// The evaluated camera is read through public engine API and reported with the units the
// Maya side consumes: focal length mm, sensor mm, apertures inches, distances cm.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUCameraSyncPayloadTest,
	"MtoUCameraSyncPrototype.CameraPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCameraSyncPayloadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FPrototypeWorld Fixture;
	if (!TestTrue(TEXT("editor world"), Fixture.Create()))
	{
		return false;
	}

	ACineCameraActor* CameraActor = SpawnCamera(
		*Fixture.World, FVector(0.0, 0.0, 100.0), FRotator(0.0, 0.0, 0.0), 35.0f, 2.8f, 500.0f);
	TestNotNull(TEXT("cine camera"), CameraActor);
	if (!CameraActor)
	{
		return false;
	}
	UCineCameraComponent* Camera = CameraActor->GetCineCameraComponent();
	Camera->Filmback.SensorWidth = 36.0f;
	Camera->Filmback.SensorHeight = 24.0f;
	Camera->Filmback.SensorAspectRatio = 1.5f;
	Camera->Filmback.SensorHorizontalOffset = 5.0f;
	Camera->Filmback.SensorVerticalOffset = -3.0f;
	Camera->LensSettings.SqueezeFactor = 1.0f;
	Camera->CropSettings.AspectRatio = 0.0f;

	SpawnMarker(*Fixture.World, TEXT("m_center"), CameraActor->GetActorLocation() + FVector(1000.0, 0.0, 0.0));
	SpawnMarker(*Fixture.World, TEXT("m_left"), CameraActor->GetActorLocation() + FVector(1000.0, -200.0, 120.0));
	SpawnMarker(*Fixture.World, TEXT("m_right"), CameraActor->GetActorLocation() + FVector(1200.0, 400.0, -160.0));

	const FIntPoint Resolution(1920, 1080);
	FMtoUCameraSyncCameraSample CameraSample;
	FMtoUCameraSyncViewSample ViewSample;
	FMtoUCameraSyncProjectionSample ProjectionSample;
	FString Error;
	if (!TestTrue(TEXT("capture succeeds"),
		FMtoUCameraSyncCapture::CaptureCamera(
			*Camera, Resolution, CameraSample, ViewSample, ProjectionSample, Error)))
	{
		AddError(Error);
		return false;
	}

	TestEqual(TEXT("focal length mm"), CameraSample.FocalLengthMm, 35.0);
	TestEqual(TEXT("f-stop"), CameraSample.FStop, 2.8);
	TestEqual(TEXT("sensor width mm"), CameraSample.SensorWidthMm, 36.0);
	TestEqual(TEXT("sensor height mm"), CameraSample.SensorHeightMm, 24.0);
	TestEqual(TEXT("sensor horizontal offset mm"), CameraSample.SensorHorizontalOffsetMm, 5.0);
	TestEqual(TEXT("sensor vertical offset mm"), CameraSample.SensorVerticalOffsetMm, -3.0);
	const double ExpectedHorizontalFov =
		FMath::RadiansToDegrees(2.0 * FMath::Atan(36.0 / (2.0 * 35.0)));
	TestTrue(TEXT("horizontal fov from focal length and sensor width"),
		FMath::IsNearlyEqual(CameraSample.HorizontalFovDeg, ExpectedHorizontalFov, 0.01));
	TestEqual(TEXT("focus method"), CameraSample.FocusMethod, FString(TEXT("Manual")));
	TestTrue(TEXT("depth of field enabled for a manual focus camera"), CameraSample.bDepthOfField);
	TestTrue(TEXT("view transform matches the component transform"),
		!CameraSample.bViewTransformDiffers);
	// Maya rejects a zero clip plane, so the resolved engine value has to be transferred.
	TestTrue(TEXT("near clip plane is resolved to a positive value"), CameraSample.NearClipCm > 0.0);
	TestEqual(TEXT("near clip reports the engine default source"),
		CameraSample.NearClipSource, FString(TEXT("engine_default")));
	// A Cine Camera always constrains the image to its filmback (or plate crop) aspect,
	// so the rendered aspect is the sensor aspect, not the output resolution aspect.
	TestEqual(TEXT("rendered aspect ratio follows the filmback"),
		ViewSample.AspectRatio, 1.5);
	TestEqual(TEXT("constrained view rectangle is letterboxed inside the output resolution"),
		ProjectionSample.ViewRect.Width(), 1620);
	TestEqual(TEXT("constrained view rectangle keeps the full height"),
		ProjectionSample.ViewRect.Height(), 1080);

	// The evaluated depth of field values are what the renderer receives, not the authored ones.
	TestTrue(TEXT("evaluated f-stop override is present"), CameraSample.Dof.bOverrideFStop);
	TestEqual(TEXT("evaluated dof f-stop"), CameraSample.Dof.FStop, 2.8);
	TestTrue(TEXT("evaluated dof focal distance override is present"),
		CameraSample.Dof.bOverrideFocalDistance);
	TestEqual(TEXT("evaluated dof focal distance cm"), CameraSample.Dof.FocalDistanceCm, 500.0);

	TArray<FMtoUCameraSyncMarkerSample> Markers;
	FMtoUCameraSyncCapture::CollectMarkers(*Fixture.World, Markers);
	TestEqual(TEXT("marker count"), Markers.Num(), 3);
	TestEqual(TEXT("markers are sorted by name"), Markers[0].Name, FString(TEXT("m_center")));

	for (FMtoUCameraSyncMarkerSample& Marker : Markers)
	{
		FVector2D Ndc;
		const FVector4 Clip = ProjectionSample.ViewProjection.TransformFVector4(
			FVector4(Marker.LocationCm.X, Marker.LocationCm.Y, Marker.LocationCm.Z, 1.0));
		AddInfo(FString::Printf(
			TEXT("marker %s world=(%.0f, %.0f, %.0f) cm ndc=(%.4f, %.4f) clip_w=%.1f aperture=[%d,%d,%d,%d]"),
			*Marker.Name, Marker.LocationCm.X, Marker.LocationCm.Y, Marker.LocationCm.Z,
			Clip.W > 0.0 ? Clip.X / Clip.W : 0.0, Clip.W > 0.0 ? Clip.Y / Clip.W : 0.0, Clip.W,
			ProjectionSample.ViewRect.Min.X, ProjectionSample.ViewRect.Min.Y,
			ProjectionSample.ViewRect.Max.X, ProjectionSample.ViewRect.Max.Y));
		if (!TestTrue(TEXT("marker projects in front of the camera"),
			FMtoUCameraSyncCapture::ProjectToNdc(ProjectionSample.ViewProjection, Marker.LocationCm, Ndc)))
		{
			continue;
		}
		const FVector2D Pixel = FMtoUCameraSyncCapture::NdcToPixel(Ndc, ProjectionSample.ViewRect);
		TestTrue(TEXT("marker inside the frame"),
			FMath::Abs(Ndc.X) <= 1.0 && FMath::Abs(Ndc.Y) <= 1.0);
		TestTrue(TEXT("marker pixel x matches the normalized position"),
			FMath::IsNearlyEqual(Pixel.X,
				ProjectionSample.ViewRect.Min.X + (Ndc.X * 0.5 + 0.5) * ProjectionSample.ViewRect.Width(),
				0.01));
		TestTrue(TEXT("marker pixel y is measured from the top row"),
			FMath::IsNearlyEqual(Pixel.Y,
				ProjectionSample.ViewRect.Min.Y + (0.5 - Ndc.Y * 0.5) * ProjectionSample.ViewRect.Height(),
				0.01));
	}

	// The film offset moves the projection, so the centered marker must not stay centered.
	const FMtoUCameraSyncMarkerSample* Center = Markers.FindByPredicate(
		[](const FMtoUCameraSyncMarkerSample& Marker)
		{
			return Marker.Name == TEXT("m_center");
		});
	if (Center)
	{
		FVector2D Ndc;
		FMtoUCameraSyncCapture::ProjectToNdc(ProjectionSample.ViewProjection, Center->LocationCm, Ndc);
		TestTrue(TEXT("sensor horizontal offset shifts the image horizontally"), Ndc.X < -0.01);
	}
	return true;
}

// Camera Cuts, a non-zero playback start and the one-way time rule.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUCameraSyncCameraCutTest,
	"MtoUCameraSyncPrototype.CameraCutsAndTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCameraSyncCameraCutTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FPrototypeWorld Fixture;
	if (!TestTrue(TEXT("editor world"), Fixture.Create()))
	{
		return false;
	}

	const FFrameRate DisplayRate(24, 1);
	const FFrameRate TickResolution(24000, 1);
	const int32 StartTick = 1001 * 1000;
	const int32 EndTick = 1051 * 1000;
	ULevelSequence* Sequence = MakeSequence(DisplayRate, TickResolution, StartTick, EndTick);

	ACineCameraActor* CameraA = SpawnCamera(
		*Fixture.World, FVector(0.0, 0.0, 100.0), FRotator(0.0, 0.0, 0.0), 50.0f, 2.0f, 400.0f);
	ACineCameraActor* CameraB = SpawnCamera(
		*Fixture.World, FVector(300.0, 200.0, 150.0), FRotator(0.0, -10.0, 20.0), 85.0f, 4.0f, 800.0f);
	TestNotNull(TEXT("camera A"), CameraA);
	TestNotNull(TEXT("camera B"), CameraB);
	if (!CameraA || !CameraB)
	{
		return false;
	}
	CameraA->GetCineCameraComponent()->Filmback.SensorWidth = 36.0f;
	CameraA->GetCineCameraComponent()->Filmback.SensorHeight = 24.0f;
	CameraB->GetCineCameraComponent()->Filmback.SensorWidth = 24.96f;
	CameraB->GetCineCameraComponent()->Filmback.SensorHeight = 18.72f;

	SpawnMarker(*Fixture.World, TEXT("m_a"), FVector(1000.0, 0.0, 100.0));
	SpawnMarker(*Fixture.World, TEXT("m_b"), FVector(1800.0, 400.0, 260.0));

	AddCut(*Sequence, BindActor(*Sequence, *Fixture.World, *CameraA, TEXT("CamA")),
		StartTick, 1026 * 1000);
	AddCut(*Sequence, BindActor(*Sequence, *Fixture.World, *CameraB, TEXT("CamB")),
		1026 * 1000, EndTick);

	FMtoUCameraSyncSession::FConfig Config;
	Config.Port = ReserveLoopbackPort();
	TestTrue(TEXT("loopback port reserved"), Config.Port != 0);
	Config.OutputResolution = FIntPoint(1920, 1080);

	FMtoUCameraSyncSession Session;
	FString Error;
	if (!TestTrue(TEXT("session starts"), Session.Start(*Fixture.World, *Sequence, Config, Error)))
	{
		AddError(Error);
		return false;
	}

	FMtoUCameraSyncFrameSample Frame;
	Session.SetDisplayFrame(1001.0);
	Session.Pump(0.0);
	if (!TestTrue(TEXT("frame builds at the range start"), Session.BuildCurrentFrame(Frame, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("playback range start is the non-zero display frame"),
		Frame.Time.PlaybackStart, 1001);
	TestEqual(TEXT("playback range end"), Frame.Time.PlaybackEnd, 1051);
	TestEqual(TEXT("reported display frame"), Frame.Time.DisplayFrame, 1001.0);
	TestEqual(TEXT("seconds are measured from the range start"), Frame.Time.Seconds, 0.0);
	TestEqual(TEXT("first camera cut is active"), Frame.Cut.ActiveIndex, 0);
	TestEqual(TEXT("cut stage is the root sequence"), Frame.Cut.Stage, FString(TEXT("root")));
	TestEqual(TEXT("camera A focal length"), Frame.Camera.FocalLengthMm, 50.0);
	TestEqual(TEXT("camera A f-stop"), Frame.Camera.FStop, 2.0);
	TestEqual(TEXT("markers are published"), Frame.Markers.Num(), 2);

	Session.SetDisplayFrame(1030.0);
	Session.Pump(0.0);
	if (!TestTrue(TEXT("frame builds inside the second cut"), Session.BuildCurrentFrame(Frame, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("second camera cut is active"), Frame.Cut.ActiveIndex, 1);
	TestEqual(TEXT("camera B focal length"), Frame.Camera.FocalLengthMm, 85.0);
	TestEqual(TEXT("camera B f-stop"), Frame.Camera.FStop, 4.0);
	TestEqual(TEXT("reported display frame advances"), Frame.Time.DisplayFrame, 1030.0);
	TestTrue(TEXT("seconds follow the display frame"),
		FMath::IsNearlyEqual(Frame.Time.Seconds, (1030.0 - 1001.0) / 24.0, 1e-9));
	TestTrue(TEXT("camera B location is reported"),
		FMath::IsNearlyEqual(Frame.Camera.LocationCm.X, 300.0, 0.01));

	// Unreal owns time: a client time request is refused and changes nothing.
	const double Before = Session.GetDisplayFrame();
	Session.HandleClientLine(TEXT("{\"type\":\"seek\",\"display_frame\":1001}"));
	TestEqual(TEXT("client time request is recorded as rejected"),
		Session.GetRejectedCommandTypes().Num(), 1);
	TestEqual(TEXT("client time request does not move Unreal time"),
		Session.GetDisplayFrame(), Before);

	// Playback stays inside the range and then pauses.
	Session.SetDisplayFrame(1049.0);
	Session.Play();
	Session.Pump(1.0);
	TestTrue(TEXT("playback advances the display frame"), Session.GetDisplayFrame() > 1049.0);
	TestTrue(TEXT("playback stops at the end of the range"),
		FMath::IsNearlyEqual(Session.GetDisplayFrame(), 1051.0, 0.5));

	// Looping wraps inside the range instead of leaving it.
	Session.SetLoop(true);
	Session.SetDisplayFrame(1050.0);
	Session.Play();
	Session.Pump(0.5);
	TestTrue(TEXT("looping stays inside the range"),
		Session.GetDisplayFrame() >= 1001.0 && Session.GetDisplayFrame() < 1051.0);

	Session.Stop(TEXT("test finished"));
	return true;
}

// A master sequence drives a subsequence; the engine must map the master time into the
// shot's own time before evaluating the shot content.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUCameraSyncSubsequenceTest,
	"MtoUCameraSyncPrototype.SubsequenceTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCameraSyncSubsequenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FPrototypeWorld Fixture;
	if (!TestTrue(TEXT("editor world"), Fixture.Create()))
	{
		return false;
	}

	const FFrameRate DisplayRate(24, 1);
	const FFrameRate TickResolution(24000, 1);
	ULevelSequence* Master = MakeSequence(DisplayRate, TickResolution, 1001 * 1000, 1051 * 1000);
	ULevelSequence* Shot = MakeSequence(DisplayRate, TickResolution, 0, 20 * 1000);

	ACineCameraActor* CameraA = SpawnCamera(
		*Fixture.World, FVector(0.0, 0.0, 100.0), FRotator(0.0, 0.0, 0.0), 50.0f, 2.0f, 400.0f);
	TestNotNull(TEXT("camera A"), CameraA);
	if (!CameraA)
	{
		return false;
	}
	AddCut(*Master, BindActor(*Master, *Fixture.World, *CameraA, TEXT("CamA")),
		1001 * 1000, 1051 * 1000);

	// The shot owns a marker actor that the master does not reference directly.
	AActor* ShotMarker = SpawnMarker(*Fixture.World, TEXT("m_shot"), FVector(0.0, 0.0, 0.0));
	TestNotNull(TEXT("shot marker"), ShotMarker);
	if (!ShotMarker)
	{
		return false;
	}

	UMovieScene* MasterScene = Master->GetMovieScene();
	UMovieSceneSubTrack* SubTrack = MasterScene->AddTrack<UMovieSceneSubTrack>();
	TestNotNull(TEXT("sub track"), SubTrack);
	if (!SubTrack)
	{
		return false;
	}
	UMovieSceneSubSection* SubSection = SubTrack->AddSequence(
		Shot, FFrameNumber(1020 * 1000), 20 * 1000);
	TestNotNull(TEXT("sub section"), SubSection);
	if (!SubSection)
	{
		return false;
	}

	FMtoUCameraSyncSession::FConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.OutputResolution = FIntPoint(1920, 1080);
	FMtoUCameraSyncSession Session;
	FString Error;
	if (!TestTrue(TEXT("session starts"), Session.Start(*Fixture.World, *Master, Config, Error)))
	{
		AddError(Error);
		return false;
	}

	// Inside the shot range the engine evaluates the shot's own content.
	Session.SetDisplayFrame(1030.0);
	Session.Pump(0.0);
	FMtoUCameraSyncFrameSample Frame;
	if (!TestTrue(TEXT("frame builds inside the subsequence"), Session.BuildCurrentFrame(Frame, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("the root cut remains the active camera"), Frame.Camera.FocalLengthMm, 50.0);
	TestEqual(TEXT("reported display frame is the master frame"), Frame.Time.DisplayFrame, 1030.0);
	TestEqual(TEXT("the shot marker is visible in the master sequence"), Frame.Markers.Num(), 1);
	TestEqual(TEXT("the evaluable shot content is reported with its unrealized master mapping"),
		Frame.Cut.SectionCount, 1);

	// Outside the shot range the master continues on its own.
	Session.SetDisplayFrame(1010.0);
	Session.Pump(0.0);
	if (!TestTrue(TEXT("frame builds before the subsequence"), Session.BuildCurrentFrame(Frame, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("the master timeline is still authoritative"), Frame.Time.DisplayFrame, 1010.0);
	TestTrue(TEXT("the shot marker is not part of the master evaluation earlier"),
		Frame.Markers.Num() == 1);

	Session.Stop(TEXT("test finished"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUCameraSyncEvalIdentityTest,
	"MtoUCameraSyncPrototype.EvalIdentityConvergence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * The evaluation identity separates a paused target from the transport serial: a slow
 * report for the still-current target pairs even after a heartbeat superseded its
 * publication, a report the timeline has left cannot move the witness, a replayed
 * publication never pairs twice, and the final target has to converge within the wait.
 */
bool FMtoUCameraSyncEvalIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FPrototypeWorld Fixture;
	if (!TestTrue(TEXT("editor world"), Fixture.Create()))
	{
		return false;
	}

	const FFrameRate DisplayRate(24, 1);
	const FFrameRate TickResolution(24000, 1);
	ULevelSequence* Sequence = MakeSequence(DisplayRate, TickResolution, 1001 * 1000, 1051 * 1000);
	ACineCameraActor* CameraA = SpawnCamera(
		*Fixture.World, FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, 50.0f, 2.0f, 400.0f);
	ACineCameraActor* CameraB = SpawnCamera(
		*Fixture.World, FVector(300.0, 200.0, 150.0), FRotator(0.0, -10.0, 20.0), 85.0f, 4.0f, 800.0f);
	AActor* Witness = SpawnMarker(*Fixture.World, TEXT("witness"), FVector::ZeroVector);
	if (!TestNotNull(TEXT("camera A"), CameraA) || !TestNotNull(TEXT("camera B"), CameraB)
		|| !TestNotNull(TEXT("pose witness"), Witness))
	{
		return false;
	}
	Witness->Tags.Remove(FName(MarkerTagName));
	AddCut(*Sequence, BindActor(*Sequence, *Fixture.World, *CameraA, TEXT("CamA")),
		1001 * 1000, 1026 * 1000);
	AddCut(*Sequence, BindActor(*Sequence, *Fixture.World, *CameraB, TEXT("CamB")),
		1026 * 1000, 1051 * 1000);

	FMtoUCameraSyncSession::FConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.OutputResolution = FIntPoint(1920, 1080);
	if (!TestTrue(TEXT("loopback port reserved"), Config.Port != 0))
	{
		return false;
	}

	FMtoUCameraSyncSession Session;
	FString Error;
	if (!TestTrue(TEXT("session starts"), Session.Start(*Fixture.World, *Sequence, Config, Error)))
	{
		AddError(Error);
		return false;
	}
	Session.SetPoseWitnessTarget(Witness);
	Session.SetDisplayFrame(1001.0);

	FSocket* Client = nullptr;
	if (!TestTrue(TEXT("a synthetic client connects"), ConnectLoopbackClient(Config.Port, Client)))
	{
		Session.Stop(TEXT("test finished"));
		return false;
	}
	if (!TestTrue(TEXT("the client is greeted"), GreetLoopbackClient(Session, *Client)))
	{
		CloseLoopbackClient(Client);
		Session.Stop(TEXT("test finished"));
		return false;
	}

	const TSharedPtr<FJsonObject> FirstFrame = Session.GetLastPublishedFrame();
	if (!TestTrue(TEXT("the first target is published"), FirstFrame.IsValid()))
	{
		Session.Stop(TEXT("test finished"));
		return false;
	}
	const int64 FirstEvalSerial = static_cast<int64>(GetNumber(FirstFrame, TEXT("eval_serial"), -1.0));
	const int64 FirstSerial = static_cast<int64>(GetNumber(FirstFrame, TEXT("frame_serial"), -1.0));
	TestTrue(TEXT("the first target carries an evaluation serial"), FirstEvalSerial >= 1);
	TestFalse(TEXT("the first target starts unpaired"), Session.IsCurrentTargetPaired());

	// A heartbeat republishes the same target: the transport serial advances while the
	// evaluation generation stays, so an in-flight report is not invalidated.
	Session.Pump(0.2);
	TestEqual(TEXT("a heartbeat keeps the evaluation serial"),
		Session.GetEvalSerial(), FirstEvalSerial);
	TestTrue(TEXT("a heartbeat advances the transport serial"),
		Session.GetFrameSerial() > FirstSerial);

	const int64 PairedBeforeSlowReport = Session.GetPairedPoseCount();
	Session.HandleClientLine(
		MakePoseLine(*Sequence, FirstFrame, Session.GetConnectionSessionId(), FirstSerial, 1001.0, 5.0));
	TestEqual(TEXT("a slow report for the current target pairs"),
		Session.GetPairedPoseCount(), PairedBeforeSlowReport + 1);
	TestTrue(TEXT("the witness carries the paired value"),
		FMath::IsNearlyEqual(Witness->GetActorLocation().Y, 5.0, 1e-6));
	TestTrue(TEXT("the current target is paired"), Session.IsCurrentTargetPaired());

	Session.HandleClientLine(
		MakePoseLine(*Sequence, FirstFrame, Session.GetConnectionSessionId(), FirstSerial, 1001.0, 5.0));
	TestEqual(TEXT("a replayed publication never pairs twice"),
		Session.GetPairedPoseCount(), PairedBeforeSlowReport + 1);

	// The timeline moves to the second shot while a fresh, unpaired publication of the
	// first one is still in flight. It is read in the same tick, before the new frame
	// is published, and must still be judged against the target that is current now.
	Session.Pump(0.2);
	const TSharedPtr<FJsonObject> InFlightFrame = Session.GetLastPublishedFrame();
	if (!TestTrue(TEXT("a heartbeat publication of the current target exists"), InFlightFrame.IsValid()))
	{
		Session.Stop(TEXT("test finished"));
		return false;
	}
	const int64 InFlightSerial = static_cast<int64>(GetNumber(InFlightFrame, TEXT("frame_serial"), -1.0));
	TestTrue(TEXT("the in-flight publication carries a fresh serial"), InFlightSerial > FirstSerial);
	TestEqual(TEXT("the in-flight publication repeats the evaluation serial"),
		static_cast<int64>(GetNumber(InFlightFrame, TEXT("eval_serial"), -1.0)), FirstEvalSerial);

	const int64 RejectedBeforeJump = Session.GetRejectedPoseCount();
	Session.SetDisplayFrame(1030.0);
	SendLoopbackLine(*Client, MakePoseLine(*Sequence, InFlightFrame, Session.GetConnectionSessionId(), InFlightSerial, 1001.0, 7.0));
	TestTrue(TEXT("a report the timeline has left is refused before the new frame is published"),
		PumpUntil(Session, [&Session, RejectedBeforeJump]()
		{
			return Session.GetRejectedPoseCount() > RejectedBeforeJump;
		}));
	TestTrue(TEXT("the witness keeps the last converged value"),
		FMath::IsNearlyEqual(Witness->GetActorLocation().Y, 5.0, 1e-6));
	TestFalse(TEXT("the new target is not paired by the old report"),
		Session.IsCurrentTargetPaired());

	const TSharedPtr<FJsonObject> SecondFrame = Session.GetLastPublishedFrame();
	if (!TestTrue(TEXT("the second target is published"), SecondFrame.IsValid()))
	{
		Session.Stop(TEXT("test finished"));
		return false;
	}
	const int64 SecondEvalSerial = static_cast<int64>(GetNumber(SecondFrame, TEXT("eval_serial"), -1.0));
	TestTrue(TEXT("the second target starts its own generation"),
		SecondEvalSerial > FirstEvalSerial);
	TestTrue(TEXT("the second target has a different identity"),
		SecondFrame->GetStringField(TEXT("eval_identity"))
			!= FirstFrame->GetStringField(TEXT("eval_identity")));

	// The final target converges once its own report arrives.
	const int64 PairedBeforeConvergence = Session.GetPairedPoseCount();
	Session.HandleClientLine(MakePoseLine(*Sequence, SecondFrame, Session.GetConnectionSessionId(),
		static_cast<int64>(GetNumber(SecondFrame, TEXT("frame_serial"), -1.0)), 1001.0, 9.0));
	TestEqual(TEXT("the final target converges"), Session.GetPairedPoseCount(),
		PairedBeforeConvergence + 1);
	TestTrue(TEXT("the final target is paired"), Session.IsCurrentTargetPaired());
	TestTrue(TEXT("the witness carries the converged value"),
		FMath::IsNearlyEqual(Witness->GetActorLocation().Y, 9.0, 1e-6));

	// Reconnect: the former connection's report is refused although every other identity
	// condition (generation, serial, time, camera) is valid, and the same report pairs
	// once it carries the current session identity.
	const int64 OldSessionId = Session.GetConnectionSessionId();
	CloseLoopbackClient(Client);
	TestTrue(TEXT("the first connection is dropped"), PumpUntil(Session, [&Session]()
	{
		return !Session.HasClient();
	}));

	FSocket* SecondClient = nullptr;
	if (!TestTrue(TEXT("a second client connects"), ConnectLoopbackClient(Config.Port, SecondClient)))
	{
		Session.Stop(TEXT("test finished"));
		return false;
	}
	SendLoopbackLine(*SecondClient, TEXT("{\"type\":\"hello\",\"protocol\":\"MtoUCameraSync\",\"version\":2,")
		TEXT("\"host\":\"evaluation-test\",\"scene_fps\":24,\"time_unit\":\"film\"}"));
	TestTrue(TEXT("the second connection is greeted and publishes"), PumpUntil(Session, [&Session, OldSessionId]()
	{
		return Session.HasClient() && Session.GetConnectionSessionId() == OldSessionId + 1
			&& Session.GetLastPublishedFrame().IsValid();
	}, 200, 0.12));

	const TSharedPtr<FJsonObject> ReconnectedFrame = Session.GetLastPublishedFrame();
	if (!TestTrue(TEXT("the new connection has a published frame"), ReconnectedFrame.IsValid()))
	{
		CloseLoopbackClient(SecondClient);
		Session.Stop(TEXT("test finished"));
		return false;
	}
	const int64 RejectedBeforeOldSession = Session.GetRejectedPoseCount();
	Session.HandleClientLine(MakePoseLine(*Sequence, ReconnectedFrame, OldSessionId,
		static_cast<int64>(GetNumber(ReconnectedFrame, TEXT("frame_serial"), -1.0)), 1001.0, 3.0));
	TestEqual(TEXT("a former connection's report is refused with every other condition valid"),
		Session.GetRejectedPoseCount(), RejectedBeforeOldSession + 1);
	TestTrue(TEXT("the former connection cannot move the witness"),
		FMath::IsNearlyEqual(Witness->GetActorLocation().Y, 9.0, 1e-6));

	const int64 PairedBeforeCurrentSession = Session.GetPairedPoseCount();
	Session.HandleClientLine(MakePoseLine(*Sequence, ReconnectedFrame, Session.GetConnectionSessionId(),
		static_cast<int64>(GetNumber(ReconnectedFrame, TEXT("frame_serial"), -1.0)), 1001.0, 3.0));
	TestEqual(TEXT("the same report pairs with the current session identity"),
		Session.GetPairedPoseCount(), PairedBeforeCurrentSession + 1);

	CloseLoopbackClient(SecondClient);
	Session.Stop(TEXT("test finished"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUCameraSyncCameraContentTest,
	"MtoUCameraSyncPrototype.CameraContentIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * The evaluation identity covers the camera content, not only the sequence time and the
 * camera object: editing the camera that is already selected (transform, focal length)
 * starts a new generation, so a report for the previous content cannot converge it, and
 * the report that answers the changed content is applied. It also fixes the identity
 * precision inside a tick, and shows that a pose edited at the same frame reaches Unreal
 * on the next heartbeat of an unchanged target.
 */
bool FMtoUCameraSyncCameraContentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FPrototypeWorld Fixture;
	if (!TestTrue(TEXT("editor world"), Fixture.Create()))
	{
		return false;
	}

	const FFrameRate DisplayRate(24, 1);
	const FFrameRate TickResolution(24000, 1);
	ULevelSequence* Sequence = MakeSequence(DisplayRate, TickResolution, 1001 * 1000, 1051 * 1000);
	ACineCameraActor* Camera = SpawnCamera(
		*Fixture.World, FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, 50.0f, 2.0f, 400.0f);
	AActor* Witness = SpawnMarker(*Fixture.World, TEXT("content witness"), FVector::ZeroVector);
	if (!TestNotNull(TEXT("camera"), Camera) || !TestNotNull(TEXT("pose witness"), Witness))
	{
		return false;
	}
	Witness->Tags.Remove(FName(MarkerTagName));
	AddCut(*Sequence, BindActor(*Sequence, *Fixture.World, *Camera, TEXT("CamA")),
		1001 * 1000, 1051 * 1000);

	FMtoUCameraSyncSession::FConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.OutputResolution = FIntPoint(1920, 1080);
	if (!TestTrue(TEXT("loopback port reserved"), Config.Port != 0))
	{
		return false;
	}

	FMtoUCameraSyncSession Session;
	FString Error;
	if (!TestTrue(TEXT("session starts"), Session.Start(*Fixture.World, *Sequence, Config, Error)))
	{
		AddError(Error);
		return false;
	}
	Session.SetPoseWitnessTarget(Witness);
	Session.SetDisplayFrame(1001.0);

	FSocket* Client = nullptr;
	if (!TestTrue(TEXT("a synthetic client connects"), ConnectLoopbackClient(Config.Port, Client)))
	{
		Session.Stop(TEXT("test finished"));
		return false;
	}
	if (!TestTrue(TEXT("the client is greeted"), GreetLoopbackClient(Session, *Client)))
	{
		CloseLoopbackClient(Client);
		Session.Stop(TEXT("test finished"));
		return false;
	}

	const TSharedPtr<FJsonObject> FirstFrame = Session.GetLastPublishedFrame();
	if (!TestTrue(TEXT("the first target is published"), FirstFrame.IsValid()))
	{
		CloseLoopbackClient(Client);
		Session.Stop(TEXT("test finished"));
		return false;
	}
	const int64 FirstEvalSerial =
		static_cast<int64>(GetNumber(FirstFrame, TEXT("eval_serial"), -1.0));
	const FString FirstIdentity = FirstFrame->GetStringField(TEXT("eval_identity"));
	TestTrue(TEXT("the first target carries an evaluation serial"), FirstEvalSerial >= 1);

	// A rebuild of the same target publishes the identity the receipts are judged against:
	// the digest is stable while nothing about the published camera changes.
	FMtoUCameraSyncFrameSample Rebuild;
	if (TestTrue(TEXT("the target rebuilds"), Session.BuildCurrentFrame(Rebuild, Error)))
	{
		TestEqual(TEXT("an unchanged camera keeps the evaluation identity"),
			Rebuild.EvalIdentity, FirstIdentity);
	}

	Session.HandleClientLine(MakePoseLine(*Sequence, FirstFrame,
		Session.GetConnectionSessionId(),
		static_cast<int64>(GetNumber(FirstFrame, TEXT("frame_serial"), -1.0)), 1001.0, 5.0));
	TestTrue(TEXT("the first target converges"), Session.IsCurrentTargetPaired());
	TestTrue(TEXT("the witness carries the first value"),
		FMath::IsNearlyEqual(Witness->GetActorLocation().Y, 5.0, 1e-6));

	// A fresh, still unpaired publication of that unchanged content is in flight when the
	// camera is edited, so the refusal below cannot be explained by a replayed serial.
	Session.Pump(0.2);
	const TSharedPtr<FJsonObject> InFlightFrame = Session.GetLastPublishedFrame();
	if (!TestTrue(TEXT("an in-flight publication exists"), InFlightFrame.IsValid()))
	{
		CloseLoopbackClient(Client);
		Session.Stop(TEXT("test finished"));
		return false;
	}
	const int64 InFlightSerial =
		static_cast<int64>(GetNumber(InFlightFrame, TEXT("frame_serial"), -1.0));
	TestTrue(TEXT("the in-flight publication carries a fresh serial"),
		InFlightSerial > static_cast<int64>(GetNumber(FirstFrame, TEXT("frame_serial"), -1.0)));
	TestEqual(TEXT("the in-flight publication repeats the generation"),
		static_cast<int64>(GetNumber(InFlightFrame, TEXT("eval_serial"), -1.0)), FirstEvalSerial);

	// Same sequence time, same camera object, edited content.
	Camera->GetCineCameraComponent()->SetCurrentFocalLength(85.0f);
	Camera->SetActorLocation(FVector(120.0, 0.0, 100.0));
	Session.Pump(0.2);
	const TSharedPtr<FJsonObject> ChangedFrame = Session.GetLastPublishedFrame();
	if (!TestTrue(TEXT("the changed content is published"), ChangedFrame.IsValid()))
	{
		CloseLoopbackClient(Client);
		Session.Stop(TEXT("test finished"));
		return false;
	}
	const FString ChangedIdentity = ChangedFrame->GetStringField(TEXT("eval_identity"));
	const int64 ChangedEvalSerial =
		static_cast<int64>(GetNumber(ChangedFrame, TEXT("eval_serial"), -1.0));
	TestTrue(TEXT("editing the evaluated camera starts a new generation"),
		ChangedEvalSerial > FirstEvalSerial);
	TestTrue(TEXT("the changed content has its own identity"), ChangedIdentity != FirstIdentity);
	TestFalse(TEXT("the changed target is not the one that converged"),
		Session.IsCurrentTargetPaired());

	const int64 RejectedBeforeStaleContent = Session.GetRejectedPoseCount();
	Session.HandleClientLine(MakePoseLine(*Sequence, InFlightFrame,
		Session.GetConnectionSessionId(), InFlightSerial, 1001.0, 6.0));
	TestEqual(TEXT("a report for the previous camera content is refused"),
		Session.GetRejectedPoseCount(), RejectedBeforeStaleContent + 1);
	TestTrue(TEXT("the refused report cannot move the witness"),
		FMath::IsNearlyEqual(Witness->GetActorLocation().Y, 5.0, 1e-6));

	const int64 PairedBeforeNewContent = Session.GetPairedPoseCount();
	Session.HandleClientLine(MakePoseLine(*Sequence, ChangedFrame,
		Session.GetConnectionSessionId(),
		static_cast<int64>(GetNumber(ChangedFrame, TEXT("frame_serial"), -1.0)), 1001.0, 7.5));
	TestEqual(TEXT("the changed content converges with its own report"),
		Session.GetPairedPoseCount(), PairedBeforeNewContent + 1);
	TestTrue(TEXT("the new value reaches the witness"),
		FMath::IsNearlyEqual(Witness->GetActorLocation().Y, 7.5, 1e-6));
	TestTrue(TEXT("the changed content is paired"), Session.IsCurrentTargetPaired());

	// Identity precision inside a tick: half a tick later (0.0005 display frames at 24 fps)
	// is a different target although the published camera content is unchanged.
	Session.SetDisplayFrame(1001.0005);
	Session.Pump(0.2);
	const TSharedPtr<FJsonObject> SubTickFrame = Session.GetLastPublishedFrame();
	if (!TestTrue(TEXT("the sub-tick position is published"), SubTickFrame.IsValid()))
	{
		CloseLoopbackClient(Client);
		Session.Stop(TEXT("test finished"));
		return false;
	}
	const FString SubTickIdentity = SubTickFrame->GetStringField(TEXT("eval_identity"));
	TestTrue(TEXT("the identity carries the documented time segment"),
		EvalIdentityTickPart(ChangedIdentity) == TEXT("@1001000"));
	TestTrue(TEXT("the identity carries a content digest"),
		EvalIdentityDigestPart(ChangedIdentity).Len() > 20);
	TestTrue(TEXT("a sub-tick move inside one tick starts a new generation"),
		static_cast<int64>(GetNumber(SubTickFrame, TEXT("eval_serial"), -1.0)) > ChangedEvalSerial);
	TestTrue(TEXT("a sub-tick move changes the identity"), SubTickIdentity != ChangedIdentity);
	TestEqual(TEXT("a sub-tick move stays inside the same tick"),
		EvalIdentityTickPart(SubTickIdentity), EvalIdentityTickPart(ChangedIdentity));
	TestEqual(TEXT("a sub-tick move keeps the content digest regardless of the milli-tick"),
		EvalIdentityDigestPart(SubTickIdentity), EvalIdentityDigestPart(ChangedIdentity));

	// A pose edited in Maya at the very same frame: the next heartbeat repeats the target
	// and its generation, and the report that carries the new value has to be applied.
	Session.Pump(0.2);
	const TSharedPtr<FJsonObject> HeartbeatFrame = Session.GetLastPublishedFrame();
	if (!TestTrue(TEXT("a heartbeat of the sub-tick target exists"), HeartbeatFrame.IsValid()))
	{
		CloseLoopbackClient(Client);
		Session.Stop(TEXT("test finished"));
		return false;
	}
	TestEqual(TEXT("the heartbeat keeps the generation of the edited-pose target"),
		static_cast<int64>(GetNumber(HeartbeatFrame, TEXT("eval_serial"), -1.0)),
		static_cast<int64>(GetNumber(SubTickFrame, TEXT("eval_serial"), -1.0)));
	TestTrue(TEXT("the heartbeat carries a fresh serial"),
		static_cast<int64>(GetNumber(HeartbeatFrame, TEXT("frame_serial"), -1.0))
			> static_cast<int64>(GetNumber(SubTickFrame, TEXT("frame_serial"), -1.0)));
	const int64 PairedBeforeEdit = Session.GetPairedPoseCount();
	const int64 RejectedBeforeEdit = Session.GetRejectedPoseCount();
	Session.HandleClientLine(MakePoseLine(*Sequence, HeartbeatFrame,
		Session.GetConnectionSessionId(),
		static_cast<int64>(GetNumber(HeartbeatFrame, TEXT("frame_serial"), -1.0)), 1001.0, 12.5));
	TestEqual(TEXT("the report of an edited pose is accepted"),
		Session.GetPairedPoseCount(), PairedBeforeEdit + 1);
	TestEqual(TEXT("the edited pose is not mistaken for a stale report"),
		Session.GetRejectedPoseCount(), RejectedBeforeEdit);
	TestTrue(TEXT("the edited pose reaches the witness"),
		FMath::IsNearlyEqual(Witness->GetActorLocation().Y, 12.5, 1e-6));

	CloseLoopbackClient(Client);
	Session.Stop(TEXT("test finished"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUCameraSyncTrailingByteTest,
	"MtoUCameraSyncPrototype.TrailingByteCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCameraSyncTrailingByteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FMtoUCameraSyncSession Session;
	Session.HandleClientLine(TEXT("{\"type\":\"bye\"}\u00E9"));
	TestEqual(TEXT("one anomalous client line"), Session.GetClientLineAnomalyCount(), 1ll);
	TestEqual(TEXT("the trailing character is two UTF-8 bytes"),
		Session.GetClientTrailingByteCount(), 2ll);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUCameraSyncClientBufferTest,
	"MtoUCameraSyncPrototype.ClientBufferBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * The receive path has to keep making progress on the lines it already buffered (a burst
 * larger than the per-pump budget drains even after the sender goes silent), hold a line
 * that arrives in pieces until its terminator, bound the bytes it buffers by failing a
 * message that never terminates, and start a new connection with an empty buffer.
 */
bool FMtoUCameraSyncClientBufferTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FPrototypeWorld Fixture;
	if (!TestTrue(TEXT("editor world"), Fixture.Create()))
	{
		return false;
	}

	const FFrameRate DisplayRate(24, 1);
	const FFrameRate TickResolution(24000, 1);
	ULevelSequence* Sequence = MakeSequence(DisplayRate, TickResolution, 1001 * 1000, 1051 * 1000);
	ACineCameraActor* Camera = SpawnCamera(
		*Fixture.World, FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, 50.0f, 2.0f, 400.0f);
	if (!TestNotNull(TEXT("camera"), Camera))
	{
		return false;
	}
	AddCut(*Sequence, BindActor(*Sequence, *Fixture.World, *Camera, TEXT("CamA")),
		1001 * 1000, 1051 * 1000);

	FMtoUCameraSyncSession::FConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.OutputResolution = FIntPoint(1920, 1080);
	if (!TestTrue(TEXT("loopback port reserved"), Config.Port != 0))
	{
		return false;
	}

	FMtoUCameraSyncSession Session;
	FString Error;
	if (!TestTrue(TEXT("session starts"), Session.Start(*Fixture.World, *Sequence, Config, Error)))
	{
		AddError(Error);
		return false;
	}
	Session.SetDisplayFrame(1001.0);

	FSocket* Client = nullptr;
	if (!TestTrue(TEXT("a synthetic client connects"), ConnectLoopbackClient(Config.Port, Client)))
	{
		Session.Stop(TEXT("test finished"));
		return false;
	}
	if (!TestTrue(TEXT("the client is greeted"), GreetLoopbackClient(Session, *Client)))
	{
		CloseLoopbackClient(Client);
		Session.Stop(TEXT("test finished"));
		return false;
	}

	// One burst well past the 64-line budget, followed by silence: the remainder is
	// already buffered and must be handled without any further network input.
	const int32 BurstLines = 65;
	const int64 LinesBeforeBurst = Session.GetClientLinesProcessed();
	const int32 ReportsBeforeBurst = Session.GetAppliedReports().Num();
	FString Burst;
	Burst.Reserve(BurstLines * 13);
	for (int32 Index = 0; Index < BurstLines; ++Index)
	{
		Burst += TEXT("{\"type\":\"applied\"}\n");
	}
	const FTCHARToUTF8 BurstUtf8(*Burst);
	if (TestTrue(TEXT("the burst is sent"), SendLoopbackBytes(*Client,
		reinterpret_cast<const uint8*>(BurstUtf8.Get()), BurstUtf8.Length())))
	{
		PumpUntil(Session, [&Session, LinesBeforeBurst, BurstLines]()
		{
			return Session.GetClientLinesProcessed() >= LinesBeforeBurst + BurstLines;
		});
		TestEqual(TEXT("every line of the silent burst is processed"),
			Session.GetClientLinesProcessed(), LinesBeforeBurst + BurstLines);
		TestEqual(TEXT("every line of the burst reached the session"),
			Session.GetAppliedReports().Num(), ReportsBeforeBurst + BurstLines);
	}

	// A line that arrives in two pieces is held until its terminator.
	const int64 LinesBeforeFragment = Session.GetClientLinesProcessed();
	TestTrue(TEXT("the first fragment is sent"),
		SendLoopbackBytes(*Client, reinterpret_cast<const uint8*>("{\"type\":\"app"), 12));
	Session.Pump(0.02);
	TestEqual(TEXT("an incomplete line is not handled"),
		Session.GetClientLinesProcessed(), LinesBeforeFragment);
	TestTrue(TEXT("the rest of the fragmented line is sent"),
		SendLoopbackLine(*Client, TEXT("lied\"}")));
	PumpUntil(Session, [&Session, LinesBeforeFragment]()
	{
		return Session.GetClientLinesProcessed() > LinesBeforeFragment;
	}, 40);
	TestEqual(TEXT("the fragmented line is handled once it is complete"),
		Session.GetClientLinesProcessed(), LinesBeforeFragment + 1);

	// A message that never terminates inside the cap fails the session closed instead of
	// growing the receive buffer with whatever the client streams.
	int32 Flooded = 0;
	const int32 FloodBytes = 68 * 1024;
	TArray<uint8> FloodChunk;
	FloodChunk.Init(uint8(TEXT('A')), 4096);
	bool bFloodSent = true;
	while (Flooded < FloodBytes && Session.HasClient() && bFloodSent)
	{
		bFloodSent = SendLoopbackBytes(*Client, FloodChunk.GetData(), FloodChunk.Num());
		Flooded += FloodChunk.Num();
		Session.Pump(0.02);
	}
	TestFalse(TEXT("the flood kept the sender connected until the session refused it"),
		bFloodSent && Session.HasClient());
	const bool bDropped = PumpUntil(Session, [&Session]() { return !Session.HasClient(); }, 60);
	TestTrue(TEXT("the flooding client is dropped"), bDropped);
	TestTrue(TEXT("the oversize message is reported"),
		Session.GetLastError().Contains(TEXT("CLIENT_MESSAGE_TOO_LARGE")));
	TestEqual(TEXT("the flood is not mistaken for client lines"),
		Session.GetClientLinesProcessed(), LinesBeforeFragment + 1);
	CloseLoopbackClient(Client);

	// The partial input belonged to the connection that sent it: a new client starts with
	// an empty buffer and its own session identity.
	const int64 SessionIdBefore = Session.GetConnectionSessionId();
	FSocket* Second = nullptr;
	if (TestTrue(TEXT("a second client connects"), ConnectLoopbackClient(Config.Port, Second)))
	{
		TestTrue(TEXT("the second client is greeted"), GreetLoopbackClient(Session, *Second));
		TestEqual(TEXT("the new connection has its own identity"),
			Session.GetConnectionSessionId(), SessionIdBefore + 1);
		const int64 LinesBeforeSecond = Session.GetClientLinesProcessed();
		TestTrue(TEXT("the second client can report"),
			SendLoopbackLine(*Second, TEXT("{\"type\":\"applied\"}")));
		PumpUntil(Session, [&Session, LinesBeforeSecond]()
		{
			return Session.GetClientLinesProcessed() > LinesBeforeSecond;
		}, 40);
		TestEqual(TEXT("a line of the new connection is handled"),
			Session.GetClientLinesProcessed(), LinesBeforeSecond + 1);
		CloseLoopbackClient(Second);
	}

	Session.Stop(TEXT("test finished"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUCameraSyncEditorSequencerTest,
	"MtoUCameraSyncPrototype.EditorSequencer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCameraSyncEditorSequencerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("editor world"), World)) { return false; }
	ULevelSequence* Sequence = MakeSequence(FFrameRate(24, 1), FFrameRate(24000, 1),
		1001 * 1000, 1051 * 1000);
	ACineCameraActor* CameraA = SpawnCamera(*World, FVector(0, 0, 100),
		FRotator::ZeroRotator, 50.0f, 2.0f, 400.0f);
	ACineCameraActor* CameraB = SpawnCamera(*World, FVector(300, 200, 150),
		FRotator::ZeroRotator, 85.0f, 4.0f, 800.0f);
	if (!TestNotNull(TEXT("camera A"), CameraA) || !TestNotNull(TEXT("camera B"), CameraB))
	{
		return false;
	}
	AddCut(*Sequence, BindActor(*Sequence, *World, *CameraA, TEXT("CamA")),
		1001 * 1000, 1026 * 1000);
	AddCut(*Sequence, BindActor(*Sequence, *World, *CameraB, TEXT("CamB")),
		1026 * 1000, 1051 * 1000);
	ULevelSequence* Shot = MakeSequence(FFrameRate(24, 1), FFrameRate(24000, 1),
		0, 20 * 1000);
	UMovieSceneSubTrack* SubTrack = Sequence->GetMovieScene()->AddTrack<UMovieSceneSubTrack>();
	UMovieSceneSubSection* SubSection = SubTrack->AddSequence(
		Shot, FFrameNumber(1020 * 1000), 20 * 1000);

	const bool bOpened = ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Sequence);
	TestTrue(TEXT("the real Level Sequence editor opens"), bOpened);
	TestEqual(TEXT("the editor exposes the opened root sequence"),
		ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence(), Sequence);
	IAssetEditorInstance* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
		->FindEditorForAsset(Sequence, false);
	TSharedPtr<ISequencer> Sequencer = AssetEditor
		&& AssetEditor->GetEditorName() == FName(TEXT("LevelSequenceEditor"))
		? static_cast<ILevelSequenceEditorToolkit*>(AssetEditor)->GetSequencer() : nullptr;
	if (!TestTrue(TEXT("the open toolkit exposes Sequencer"), Sequencer.IsValid()))
	{
		if (bOpened) { ULevelSequenceEditorBlueprintLibrary::CloseLevelSequence(); }
		CameraA->Destroy();
		CameraB->Destroy();
		return false;
	}

	Sequencer->SetGlobalTime(FFrameTime(1001 * 1000));
	Sequencer->ForceEvaluate();
	FMtoUCameraSyncSession Session;
	FMtoUCameraSyncSession::FConfig Config;
	Config.Port = ReserveLoopbackPort();
	FString Error;
	const bool bStarted = Session.StartFromEditor(*World, Sequencer.ToSharedRef(), Config, Error);
	TestTrue(TEXT("session follows the open Sequencer"), bStarted);
	if (bStarted)
	{
		TestTrue(TEXT("editor source creates no second player"), Session.IsEditorSource());
		Session.Pump(0.0);
		FMtoUCameraSyncFrameSample Frame;
		if (TestTrue(TEXT("first editor frame builds"), Session.BuildCurrentFrame(Frame, Error)))
		{
			TestEqual(TEXT("first editor camera cut"), Frame.Camera.FocalLengthMm, 50.0);
			TestEqual(TEXT("first editor root display frame"), Frame.Time.DisplayFrame, 1001.0);
		}
		Sequencer->SetGlobalTime(FFrameTime(1030 * 1000));
		Sequencer->ForceEvaluate();
		Session.Pump(0.0);
		if (TestTrue(TEXT("second editor frame builds"), Session.BuildCurrentFrame(Frame, Error)))
		{
			TestEqual(TEXT("second editor camera cut"), Frame.Camera.FocalLengthMm, 85.0);
			TestEqual(TEXT("second editor root display frame"), Frame.Time.DisplayFrame, 1030.0);
		}
		ULevelSequenceEditorBlueprintLibrary::FocusLevelSequence(SubSection);
		Session.Pump(0.0);
		if (TestTrue(TEXT("focused shot frame builds"), Session.BuildCurrentFrame(Frame, Error)))
		{
			TestEqual(TEXT("focus changed to the shot"),
				ULevelSequenceEditorBlueprintLibrary::GetFocusedLevelSequence(), Shot);
			TestEqual(TEXT("focused shot still publishes root time"),
				Frame.Time.DisplayFrame, 1030.0);
			TestEqual(TEXT("focused shot preserves the evaluated root cut"),
				Frame.Camera.FocalLengthMm, 85.0);
		}
		ULevelSequenceEditorBlueprintLibrary::FocusParentSequence();
		Session.SetDisplayFrame(1010.0);
		Session.Pump(0.0);
		TestEqual(TEXT("the prototype cannot override the editor playhead"),
			Session.GetDisplayFrame(), 1030.0);
		Session.Stop(TEXT("editor test finished"));
		TestEqual(TEXT("stopping follow leaves the editor playhead alone"),
			Sequencer->GetGlobalTime().Time.AsDecimal(), 1030.0 * 1000.0);
	}
	else { AddError(Error); }
	ULevelSequenceEditorBlueprintLibrary::CloseLevelSequence();
	CameraA->Destroy();
	CameraB->Destroy();
	return true;
}

// Opt-in real Maya peer: drives the publisher over a real socket and applies every frame
// in Maya 2024. Requires -MtoUCameraSyncMayapy=, -MtoUCameraSyncPeer= and -MtoUEvidence=.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUCameraSyncMayaPeerTest,
	"MtoUCameraSyncPrototype.RealMayaPeer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUCameraSyncMayaPeerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FString MayapyPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUCameraSyncMayapy="), MayapyPath))
	{
		AddInfo(TEXT("Host check not requested; supply MtoUCameraSyncMayapy, MtoUCameraSyncPeer and MtoUEvidence."));
		return true;
	}
	FString PeerPath;
	FString Evidence;
	if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUCameraSyncPeer="), PeerPath)
		|| !FParse::Value(FCommandLine::Get(), TEXT("MtoUEvidence="), Evidence)
		|| !FPaths::FileExists(MayapyPath) || !FPaths::FileExists(PeerPath))
	{
		AddError(TEXT("host check requires existing mayapy/peer paths and an evidence directory"));
		return false;
	}
	if (!TestTrue(TEXT("evidence directory"), IFileManager::Get().MakeDirectory(*Evidence, true)
		|| IFileManager::Get().DirectoryExists(*Evidence)))
	{
		return false;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("real editor world"), World))
	{
		return false;
	}
	TArray<TWeakObjectPtr<AActor>> OwnedActors;
	bool bEditorOpened = false;
	ON_SCOPE_EXIT
	{
		if (bEditorOpened) { ULevelSequenceEditorBlueprintLibrary::CloseLevelSequence(); }
		for (const TWeakObjectPtr<AActor>& Actor : OwnedActors)
		{
			if (Actor.IsValid()) { Actor->Destroy(); }
		}
	};

	const FFrameRate DisplayRate(24, 1);
	const FFrameRate TickResolution(24000, 1);
	ULevelSequence* Sequence = MakeSequence(DisplayRate, TickResolution, 1001 * 1000, 1051 * 1000);
	ACineCameraActor* CameraA = SpawnCamera(
		*World, FVector(0.0, 0.0, 100.0), FRotator(0.0, 0.0, 0.0), 50.0f, 2.0f, 400.0f);
	ACineCameraActor* CameraB = SpawnCamera(
		*World, FVector(300.0, 200.0, 150.0), FRotator(0.0, -10.0, 20.0), 85.0f, 4.0f, 800.0f);
	if (CameraA) { OwnedActors.Add(CameraA); }
	if (CameraB) { OwnedActors.Add(CameraB); }
	if (!TestNotNull(TEXT("camera A"), CameraA) || !TestNotNull(TEXT("camera B"), CameraB))
	{
		return false;
	}
	ACineCameraActor* Cameras[] = {CameraA, CameraB};
	for (ACineCameraActor* Camera : Cameras)
	{
		UCineCameraComponent* Component = Camera->GetCineCameraComponent();
		Component->Filmback.SensorWidth = 24.96f;
		Component->Filmback.SensorHeight = 18.72f;
		Component->Filmback.SensorAspectRatio = 1.333333f;
	}
	OwnedActors.Add(SpawnMarker(*World, TEXT("m_center"), FVector(900.0, 0.0, 100.0)));
	OwnedActors.Add(SpawnMarker(*World, TEXT("m_upper_left"), FVector(1400.0, -500.0, 300.0)));
	OwnedActors.Add(SpawnMarker(*World, TEXT("m_lower_right"), FVector(1600.0, 420.0, -120.0)));
	AActor* PoseWitness = SpawnMarker(*World, TEXT("pose_witness"), FVector::ZeroVector);
	if (PoseWitness)
	{
		PoseWitness->Tags.Remove(FName(MarkerTagName));
		OwnedActors.Add(PoseWitness);
	}

	AddCut(*Sequence, BindActor(*Sequence, *World, *CameraA, TEXT("CamA")),
		1001 * 1000, 1026 * 1000);
	AddCut(*Sequence, BindActor(*Sequence, *World, *CameraB, TEXT("CamB")),
		1026 * 1000, 1051 * 1000);
	bEditorOpened = ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Sequence);
	if (!TestTrue(TEXT("real Level Sequence editor opens"), bEditorOpened)) { return false; }
	IAssetEditorInstance* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
		->FindEditorForAsset(Sequence, false);
	TSharedPtr<ISequencer> Sequencer = AssetEditor
		&& AssetEditor->GetEditorName() == FName(TEXT("LevelSequenceEditor"))
		? static_cast<ILevelSequenceEditorToolkit*>(AssetEditor)->GetSequencer() : nullptr;
	if (!TestTrue(TEXT("real Sequencer is available"), Sequencer.IsValid())) { return false; }
	Sequencer->SetGlobalTime(FFrameTime(1001 * 1000));
	Sequencer->ForceEvaluate();

	FMtoUCameraSyncSession::FConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.OutputResolution = FIntPoint(1920, 1080);
	if (!TestTrue(TEXT("loopback port reserved"), Config.Port != 0))
	{
		return false;
	}

	FMtoUCameraSyncSession Session;
	FString Error;
	if (!TestTrue(TEXT("session follows real editor"),
		Session.StartFromEditor(*World, Sequencer.ToSharedRef(), Config, Error)))
	{
		AddError(Error);
		return false;
	}
	Session.SetPoseWitnessTarget(PoseWitness);

	const FString FixturePath = FPaths::Combine(Evidence, TEXT("camera-sync-fixture.json"));
	const FString ResultPath = FPaths::Combine(Evidence, TEXT("camera-sync-result.json"));
	const FString ReportPath = FPaths::Combine(Evidence, TEXT("camera-sync-ue-report.json"));
	IFileManager::Get().Delete(*ResultPath);
	const FString FixtureJson = FString::Printf(
		TEXT("{\"host\":\"127.0.0.1\",\"port\":%u,\"scene_fps\":24.0,\"maya_start_frame\":1001.0,"
			"\"camera_name\":\"MtoU_UE_Camera\",\"frames_to_apply\":6,\"render\":false,"
			"\"evidence_dir\":\"%s\",\"protocol\":{\"name\":\"MtoUCameraSync\",\"version\":2}}"),
		static_cast<uint32>(Config.Port), *Evidence.Replace(TEXT("\\"), TEXT("/")));
	if (!TestTrue(TEXT("fixture written"), SaveJson(FixturePath, FixtureJson)))
	{
		return false;
	}

	// The peer runs through a small command file so the exact launch is reproducible, and
	// its output is captured through a pipe this test owns: a child that inherits the
	// editor's handles can otherwise write its console output into the prototype socket.
	const FString PeerLogPath = FPaths::Combine(Evidence, TEXT("camera-sync-peer.log"));
	const FString CommandPath = FPaths::Combine(Evidence, TEXT("camera-sync-peer.cmd"));
	const FString CommandText = FString::Printf(
		TEXT("@echo off\r\n\"%s\" \"%s\" --fixture \"%s\" --result \"%s\"\r\nexit /b %%ERRORLEVEL%%\r\n"),
		*MayapyPath, *PeerPath, *FixturePath, *ResultPath);
	if (!TestTrue(TEXT("peer command written"), SaveJson(CommandPath, CommandText)))
	{
		return false;
	}
	void* PeerPipeRead = nullptr;
	void* PeerPipeWrite = nullptr;
	FPlatformProcess::CreatePipe(PeerPipeRead, PeerPipeWrite);
	// Launch the interpreter directly with an owned pipe: going through a shell lets an
	// inherited console handle reach Maya's own output, which then lands in the socket.
	const FString ProcessArgs = FString::Printf(
		TEXT("\"%s\" --fixture \"%s\" --result \"%s\""),
		*PeerPath, *FixturePath, *ResultPath);
	// Only the child's output pipe is inherited; passing an input pipe as well makes the
	// engine warn that the read end is not inheritable.
	FProcHandle Process = FPlatformProcess::CreateProc(
		*MayapyPath, *ProcessArgs, false, true, true, nullptr, 0, nullptr,
		PeerPipeWrite, nullptr);
	FString PeerOutput;
	if (!TestTrue(TEXT("real Maya process starts"), Process.IsValid()))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonObject>> PublishedFrames;
	TestTrue(TEXT("the session has no second sequence player"), Session.IsEditorSource());
	int32 AppliedReports = 0;
	int32 AppliedCuts = 0;
	int64 LastRecordedPublish = 0;
	bool bSwitchedToSecondCut = false;
	double LastLoopTime = FPlatformTime::Seconds();
	const double Deadline = LastLoopTime + 180.0;
	while (Process.IsValid() && FPlatformProcess::IsProcRunning(Process)
		&& FPlatformTime::Seconds() < Deadline)
	{
		// Real time drives the publisher so the Maya side sees a realistic rate, and the
		// loop is paced instead of spinning: an unpaced loop drowns the client.
		FPlatformProcess::Sleep(0.01f);
		PeerOutput += FPlatformProcess::ReadPipe(PeerPipeRead);
		const double Now = FPlatformTime::Seconds();
		const double Delta = FMath::Clamp(Now - LastLoopTime, 0.001, 0.5);
		LastLoopTime = Now;

		// The first two published frames come from the first camera cut, the rest from
		// the second one, so the peer has to follow a real cut.
		const int32 PublishedSoFar = static_cast<int32>(Session.GetPublishedFrameCount());
		if (PublishedSoFar == 2 && !bSwitchedToSecondCut)
		{
			Sequencer->SetGlobalTime(FFrameTime(1030 * 1000));
			Sequencer->ForceEvaluate();
			bSwitchedToSecondCut = true;
		}
		Session.Pump(Delta);

		if (Session.GetPublishedFrameCount() > LastRecordedPublish
			&& PublishedFrames.Num() < 24)
		{
			LastRecordedPublish = Session.GetPublishedFrameCount();
			if (const TSharedPtr<FJsonObject>& Latest = Session.GetLastPublishedFrame())
			{
				PublishedFrames.Add(Latest);
			}
		}

		TSharedPtr<FJsonObject> PeerResult;
		if (LoadJson(ResultPath, PeerResult))
		{
			const TArray<TSharedPtr<FJsonValue>>* Applied = nullptr;
			if (PeerResult->TryGetArrayField(TEXT("applied"), Applied) && Applied)
			{
				AppliedReports = Applied->Num();
				AppliedCuts = 0;
				for (const TSharedPtr<FJsonValue>& Entry : *Applied)
				{
					const TSharedPtr<FJsonObject> Object = Entry->AsObject();
					const TSharedPtr<FJsonObject> EntryCamera = GetObject(Object, TEXT("camera"));
					if (GetNumber(EntryCamera, TEXT("focal_length_mm"), 0.0) > 60.0)
					{
						++AppliedCuts;
					}
				}
			}
			FString Phase;
			PeerResult->TryGetStringField(TEXT("phase"), Phase);
			if (Phase == TEXT("done") || Phase == TEXT("failed"))
			{
				break;
			}
		}
	}

	int32 ReturnCode = 0;
	if (Process.IsValid())
	{
		FPlatformProcess::WaitForProc(Process);
		FPlatformProcess::GetProcReturnCode(Process, &ReturnCode);
		FPlatformProcess::CloseProc(Process);
	}
	// Drain whatever the peer printed before it exited, then release the pipe.
	PeerOutput += FPlatformProcess::ReadPipe(PeerPipeRead);
	SaveJson(PeerLogPath, PeerOutput);
	FPlatformProcess::ClosePipe(PeerPipeRead, PeerPipeWrite);
	Session.Pump(0.0);
	// The acceptance condition is convergence, not one early pair: the target that is
	// current when the session stops has to have been answered by the real client.
	const double ConvergenceDeadline = FPlatformTime::Seconds() + 15.0;
	while (FPlatformTime::Seconds() < ConvergenceDeadline && !Session.IsCurrentTargetPaired())
	{
		Session.Pump(0.02);
		FPlatformProcess::Sleep(0.005f);
	}
	TestTrue(TEXT("the final evaluation target converged within the wait"),
		Session.IsCurrentTargetPaired());
	TestEqual(TEXT("the converged generation is the latest sampled target"),
		Session.GetLastPairedEvalSerial(), Session.GetEvalSerial());
	// Captured here because the reconnect below starts a fresh connection identity,
	// which resets the pairing history on purpose.
	const bool bConvergedBeforeReplay = Session.IsCurrentTargetPaired();
	const int64 ConvergedEvalSerial = Session.GetEvalSerial();
	const int64 ConvergedPairedEvalSerial = Session.GetLastPairedEvalSerial();
	const int32 PeerAppliedReports = Session.GetAppliedReports().Num();
	const int64 PairedBeforeReplay = Session.GetPairedPoseCount();
	const double WitnessBeforeReplay = PoseWitness ? PoseWitness->GetActorLocation().Y : 0.0;
	FString PairedReportJson;
	for (const FMtoUCameraSyncSession::FAppliedReport& Applied : Session.GetAppliedReports())
	{
		if (Applied.bPosePaired)
		{
			PairedReportJson = Applied.RawJson;
			break;
		}
	}
	if (!PairedReportJson.IsEmpty()) { Session.HandleClientLine(PairedReportJson); }
	TestTrue(TEXT("Maya joint pose was paired with a current editor camera frame"),
		PairedBeforeReplay >= 1);
	if (PairedBeforeReplay >= 1)
	{
		TestEqual(TEXT("duplicate or stale pose does not pair again"),
			Session.GetPairedPoseCount(), PairedBeforeReplay);
		TestTrue(TEXT("duplicate or stale pose does not overwrite the UE witness"),
			PoseWitness && FMath::IsNearlyEqual(PoseWitness->GetActorLocation().Y,
				WitnessBeforeReplay, 1e-6));
	}
	// The witness holds the pose of the converged generation, not of an earlier shot.
	double ConvergedPose = WitnessBeforeReplay;
	for (const FMtoUCameraSyncSession::FAppliedReport& Applied : Session.GetAppliedReports())
	{
		if (Applied.bPosePaired)
		{
			ConvergedPose = Applied.PoseTranslateX;
		}
	}
	TestTrue(TEXT("the witness holds the converged generation's pose"),
		PoseWitness && FMath::IsNearlyEqual(PoseWitness->GetActorLocation().Y, ConvergedPose, 1e-6));
	TestTrue(TEXT("the converged pose is the keyed witness value at the final target"),
		FMath::IsNearlyEqual(ConvergedPose, 9.0, 1e-6));
	// Reconnect the transport and replay a valid pose from the first connection.
	// A new connection gets a new session identity even when it uses the same port.
	for (int32 Attempt = 0; Attempt < 20 && Session.HasClient(); ++Attempt)
	{
		Session.Pump(0.0);
		FPlatformProcess::Sleep(0.005f);
	}
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* ReconnectedClient = Sockets
		? Sockets->CreateSocket(NAME_Stream, TEXT("MtoU camera reconnect test")) : nullptr;
	bool bConnectedAgain = false;
	if (ReconnectedClient)
	{
		const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
		bool bValidAddress = false;
		Address->SetIp(TEXT("127.0.0.1"), bValidAddress);
		Address->SetPort(Config.Port);
		bConnectedAgain = bValidAddress && ReconnectedClient->Connect(*Address);
	}
	TestTrue(TEXT("a second client reconnects after Maya exits"), bConnectedAgain);
	if (bConnectedAgain)
	{
		const FString Hello = TEXT("{\"type\":\"hello\",\"protocol\":\"MtoUCameraSync\","
			"\"version\":2,\"host\":\"reconnect-test\",\"scene_fps\":24,"
			"\"time_unit\":\"film\"}\n");
		const FTCHARToUTF8 HelloBytes(*Hello);
		int32 Sent = 0;
		ReconnectedClient->Send(reinterpret_cast<const uint8*>(HelloBytes.Get()),
			HelloBytes.Length(), Sent);
		const int64 PublishedBeforeReconnect = Session.GetPublishedFrameCount();
		for (int32 Attempt = 0; Attempt < 30
			&& (!Session.IsGreeted() || Session.GetPublishedFrameCount() == PublishedBeforeReconnect);
			++Attempt)
		{
			Session.Pump(0.1);
			FPlatformProcess::Sleep(0.005f);
		}
		TestEqual(TEXT("reconnection has a new identity"),
			Session.GetConnectionSessionId(), 2ll);
		TestTrue(TEXT("reconnection publishes its own evaluated frame"),
			Session.GetPublishedFrameCount() > PublishedBeforeReconnect);
		TestEqual(TEXT("a new connection starts with an empty pairing history"),
			Session.GetLastPairedEvalSerial(), 0ll);
		const int64 RejectedBeforeOldSession = Session.GetRejectedPoseCount();
		const FString OldPoseLine = PairedReportJson + TEXT("\n");
		const FTCHARToUTF8 OldPoseBytes(*OldPoseLine);
		ReconnectedClient->Send(reinterpret_cast<const uint8*>(OldPoseBytes.Get()),
			OldPoseBytes.Length(), Sent);
		for (int32 Attempt = 0; Attempt < 30
			&& Session.GetRejectedPoseCount() == RejectedBeforeOldSession; ++Attempt)
		{
			Session.Pump(0.0);
			FPlatformProcess::Sleep(0.005f);
		}
		TestEqual(TEXT("an old-session pose is refused after reconnect"),
			Session.GetRejectedPoseCount(), RejectedBeforeOldSession + 1);
		TestTrue(TEXT("old-session pose cannot overwrite the UE witness"),
			PoseWitness && FMath::IsNearlyEqual(PoseWitness->GetActorLocation().Y,
				WitnessBeforeReplay, 1e-6));
	}
	Session.Stop(TEXT("peer session finished"));
	if (ReconnectedClient)
	{
		ReconnectedClient->Close();
		Sockets->DestroySocket(ReconnectedClient);
	}

	TSharedPtr<FJsonObject> PeerResult;
	const bool bResultLoaded = LoadJson(ResultPath, PeerResult);
	TestTrue(TEXT("peer wrote a result file"), bResultLoaded);
	TestEqual(TEXT("peer exit code"), ReturnCode, 0);
	TestTrue(TEXT("peer reported success"), bResultLoaded
		&& PeerResult->HasTypedField<EJson::Boolean>(TEXT("ok"))
		&& PeerResult->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("peer applied several frames"), AppliedReports >= 3);
	TestTrue(TEXT("peer applied frames from both camera cuts"), AppliedCuts >= 1);
	TestEqual(TEXT("every Maya application report reached UE intact"),
		PeerAppliedReports, AppliedReports);
	TestTrue(TEXT("Unreal recorded the applied reports"),
		Session.GetAppliedReports().Num() >= 1);
	TestEqual(TEXT("client never sent a time or camera command"),
		Session.GetRejectedCommandTypes().Num(), 0);
	TestEqual(TEXT("client JSON lines have no discarded suffix"),
		Session.GetClientLineAnomalyCount(), 0ll);

	// Every frame the real client applied echoes the evaluation identity it answered.
	bool bEchoedEvaluationIdentity = true;
	int32 AppliedWithIdentity = 0;
	if (bResultLoaded && PeerResult.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Applied = nullptr;
		if (PeerResult->TryGetArrayField(TEXT("applied"), Applied) && Applied)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Applied)
			{
				const TSharedPtr<FJsonObject> Object = Entry->AsObject();
				FString Identity;
				double EvalSerial = 0.0;
				if (!Object->TryGetStringField(TEXT("eval_identity"), Identity) || Identity.IsEmpty()
					|| !Object->TryGetNumberField(TEXT("eval_serial"), EvalSerial)
					|| EvalSerial < 1.0)
				{
					bEchoedEvaluationIdentity = false;
				}
				else
				{
					++AppliedWithIdentity;
				}
			}
		}
	}
	TestTrue(TEXT("every applied frame echoes the evaluation identity"),
		bEchoedEvaluationIdentity);
	TestEqual(TEXT("every applied frame carries the echo"), AppliedWithIdentity, AppliedReports);

	// Per-frame comparison between the published payload and what Maya actually applied.
	double MaxMarkerDelta = 0.0;
	if (bResultLoaded && PeerResult.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Applied = nullptr;
		const bool bHasApplied = PeerResult->TryGetArrayField(TEXT("applied"), Applied) && Applied;
		TestTrue(TEXT("peer result carries applied frames"), bHasApplied);
		if (bHasApplied)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Applied)
			{
				const TSharedPtr<FJsonObject> Object = Entry->AsObject();
				const double Serial = GetNumber(Object, TEXT("frame_serial"), 0.0);
				const TSharedPtr<FJsonObject>* Matching = nullptr;
				for (const TSharedPtr<FJsonObject>& Frame : PublishedFrames)
				{
					if (FMath::IsNearlyEqual(GetNumber(Frame, TEXT("frame_serial"), -1.0), Serial))
					{
						Matching = &Frame;
						break;
					}
				}
				if (!Matching)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Camera = GetObject(*Matching, TEXT("camera"));
				const TSharedPtr<FJsonObject> Dof = GetObject(Camera, TEXT("dof"));
				const TSharedPtr<FJsonObject> AppliedCamera = GetObject(Object, TEXT("camera"));
				const TSharedPtr<FJsonObject> AppliedGate = GetObject(Object, TEXT("gate"));
				const TSharedPtr<FJsonObject> Aperture = GetObject(*Matching, TEXT("aperture_resolution"));
				TestEqual(TEXT("applied focal length matches the payload"),
					GetNumber(AppliedCamera, TEXT("focal_length_mm"), 0.0),
					GetNumber(Camera, TEXT("focal_length_mm"), 0.0));
				TestEqual(TEXT("applied film aperture matches the sensor width"),
					GetNumber(AppliedCamera, TEXT("horizontal_film_aperture_in"), 0.0),
					GetNumber(Camera, TEXT("sensor_width_mm"), 0.0) / 25.4);
				TestEqual(TEXT("applied film aperture matches the sensor height"),
					GetNumber(AppliedCamera, TEXT("vertical_film_aperture_in"), 0.0),
					GetNumber(Camera, TEXT("sensor_height_mm"), 0.0) / 25.4);
				TestTrue(TEXT("applied near clip matches the resolved payload value"),
					FMath::IsNearlyEqual(GetNumber(AppliedCamera, TEXT("near_clip_cm"), 0.0),
						GetNumber(Camera, TEXT("near_clip_cm"), -1.0), 1e-6));
				TestEqual(TEXT("applied f-stop matches the payload"),
					GetNumber(AppliedCamera, TEXT("f_stop"), 0.0),
					GetNumber(Camera, TEXT("f_stop"), 0.0));
				TestEqual(TEXT("applied f-stop matches the evaluated post process value"),
					GetNumber(AppliedCamera, TEXT("f_stop"), 0.0),
					GetNumber(Dof, TEXT("fstop"), 0.0));
				TestEqual(TEXT("applied focus distance matches the payload"),
					GetNumber(AppliedCamera, TEXT("focus_distance_cm"), 0.0),
					GetNumber(Camera, TEXT("focus_distance_cm"), 0.0));
				TestEqual(TEXT("applied resolution gate is the payload aperture extent"),
					GetNumber(AppliedGate, TEXT("width"), 0.0),
					GetNumber(Aperture, TEXT("x"), 0.0));
				TestEqual(TEXT("applied resolution gate height is the payload aperture height"),
					GetNumber(AppliedGate, TEXT("height"), 0.0),
					GetNumber(Aperture, TEXT("y"), 0.0));
				const TSharedPtr<FJsonObject> PayloadTime = GetObject(*Matching, TEXT("time"));
				TestTrue(TEXT("applied Maya frame equals the published display frame"),
					FMath::IsNearlyEqual(
						GetNumber(Object, TEXT("maya_frame"), -1.0),
						GetNumber(PayloadTime, TEXT("display_frame"), -2.0), 1e-6));

				const TArray<TSharedPtr<FJsonValue>>* Markers = nullptr;
				if (Object->TryGetArrayField(TEXT("markers"), Markers) && Markers)
				{
					for (const TSharedPtr<FJsonValue>& MarkerValue : *Markers)
					{
						const TSharedPtr<FJsonObject> Marker = MarkerValue->AsObject();
						const TArray<TSharedPtr<FJsonValue>>* Delta = nullptr;
						if (Marker->TryGetArrayField(TEXT("delta"), Delta) && Delta
							&& Delta->Num() == 2 && (*Delta)[0].IsValid() && (*Delta)[1].IsValid())
						{
							MaxMarkerDelta = FMath::Max(MaxMarkerDelta,
								FMath::Max(FMath::Abs((*Delta)[0]->AsNumber()),
									FMath::Abs((*Delta)[1]->AsNumber())));
						}
					}
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("largest Maya-versus-Unreal marker NDC delta: %.9f"), MaxMarkerDelta));
	TestTrue(TEXT("Maya and Unreal agree on the projected marker position"), MaxMarkerDelta < 1e-6);

	const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetNumberField(TEXT("published_frames"), Session.GetPublishedFrameCount());
	Report->SetNumberField(TEXT("applied_reports"), PeerAppliedReports);
	Report->SetNumberField(TEXT("synthetic_stale_replays"),
		Session.GetAppliedReports().Num() - PeerAppliedReports);
	Report->SetNumberField(TEXT("max_marker_ndc_delta"), MaxMarkerDelta);
	Report->SetNumberField(TEXT("peer_exit_code"), ReturnCode);
	Report->SetStringField(TEXT("session_last_error"), Session.GetLastError());
	Report->SetNumberField(TEXT("client_line_anomalies"), Session.GetClientLineAnomalyCount());
	Report->SetNumberField(TEXT("client_trailing_bytes"), Session.GetClientTrailingByteCount());
	Report->SetStringField(TEXT("last_client_line_anomaly"), Session.GetLastClientLineAnomaly());
	Report->SetStringField(TEXT("peer_output_tail"), PeerOutput.Right(4000));
	Report->SetNumberField(TEXT("failed_sends"), Session.GetFailedSendCount());
	Report->SetNumberField(TEXT("paired_poses"), Session.GetPairedPoseCount());
	Report->SetNumberField(TEXT("rejected_poses"), Session.GetRejectedPoseCount());
	Report->SetNumberField(TEXT("eval_serial"), Session.GetEvalSerial());
	Report->SetNumberField(TEXT("last_paired_eval_serial"), Session.GetLastPairedEvalSerial());
	Report->SetBoolField(TEXT("converged"), bConvergedBeforeReplay);
	Report->SetNumberField(TEXT("converged_eval_serial"), ConvergedEvalSerial);
	Report->SetNumberField(TEXT("converged_paired_eval_serial"), ConvergedPairedEvalSerial);
	Report->SetNumberField(TEXT("applied_reports_dropped"), Session.GetAppliedReportsDropped());
	Report->SetNumberField(TEXT("pose_witness_ue_y"), WitnessBeforeReplay);
	Report->SetNumberField(TEXT("converged_pose"), ConvergedPose);
	TArray<TSharedPtr<FJsonValue>> AppliedValues;
	for (const FMtoUCameraSyncSession::FAppliedReport& Applied : Session.GetAppliedReports())
	{
		TSharedPtr<FJsonObject> AppliedObject;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Applied.RawJson), AppliedObject)
			&& AppliedObject.IsValid())
		{
			AppliedObject->SetBoolField(TEXT("ue_pose_paired"), Applied.bPosePaired);
			AppliedObject->SetStringField(TEXT("ue_pairing_error"), Applied.PairingError);
			AppliedValues.Add(MakeShared<FJsonValueObject>(AppliedObject));
		}
	}
	Report->SetArrayField(TEXT("applied_reports_raw"), AppliedValues);
	TArray<TSharedPtr<FJsonValue>> PayloadValues;
	for (const TSharedPtr<FJsonObject>& Frame : PublishedFrames)
	{
		PayloadValues.Add(MakeShared<FJsonValueObject>(Frame));
	}
	Report->SetArrayField(TEXT("published_frame_payloads"), PayloadValues);
	if (PeerResult.IsValid())
	{
		Report->SetObjectField(TEXT("peer_result"), PeerResult);
	}
	TestTrue(TEXT("report written"), SaveJson(ReportPath, JsonToText(Report)));
	TestTrue(TEXT("the publisher reported no transport problem"),
		Session.GetLastError().IsEmpty());
	AddInfo(FString::Printf(
		TEXT("client lines with trailing bytes: %lld (%lld bytes discarded)"),
		Session.GetClientLineAnomalyCount(), Session.GetClientTrailingByteCount()));
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
