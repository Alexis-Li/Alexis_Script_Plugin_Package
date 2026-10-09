// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "MtoUMultiSubjectReceiver.h"

#include "Common/TcpSocketBuilder.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MovieSceneTrack.h"
#include "MtoUMultiSubjectDriver.h"
#include "MtoUMultiSubjectProtocol.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace
{
	const TCHAR* ReceiverLogCategory = TEXT("MtoUMultiSubject");

	/** The byte length of a UTF-8 line, which is what the 1 MiB bound applies to. */
	int64 Utf8Length(const FString& Line)
	{
		return static_cast<int64>(FTCHARToUTF8(*Line).Length());
	}

	void AddNumberArray(TArray<TSharedPtr<FJsonValue>>& OutValues, const FTransform& Transform)
	{
		const FVector Translation = Transform.GetTranslation();
		const FQuat Rotation = Transform.GetRotation();
		const FVector Scale = Transform.GetScale3D();
		for (const double Value : {
			Translation.X, Translation.Y, Translation.Z,
			Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
			Scale.X, Scale.Y, Scale.Z })
		{
			OutValues.Add(MakeShared<FJsonValueNumber>(Value));
		}
	}

	TSharedRef<FJsonObject> MakeTransformObject(const FTransform& Transform)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Transform.GetTranslation().X);
		Object->SetNumberField(TEXT("y"), Transform.GetTranslation().Y);
		Object->SetNumberField(TEXT("z"), Transform.GetTranslation().Z);
		return Object;
	}
}

FMtoUMultiSubjectReceiver::FMtoUMultiSubjectReceiver() = default;

FMtoUMultiSubjectReceiver::~FMtoUMultiSubjectReceiver()
{
	Stop(TEXT("receiver destroyed"));
}

bool FMtoUMultiSubjectReceiver::Start(
	UWorld& InWorld,
	const TArray<FMtoUTargetRegistration>& TargetRegistrations,
	const FMtoUMultiSubjectSessionConfig& InConfig,
	FString& OutError)
{
	if (bRunning)
	{
		OutError = TEXT("the receiver is already running");
		return false;
	}
	if (TargetRegistrations.IsEmpty())
	{
		OutError = TEXT("the receiver needs at least one registered target");
		return false;
	}

	World = &InWorld;
	Config = InConfig;
	BoundPort = Config.Port;

	Targets.Reset();
	for (const FMtoUTargetRegistration& Registration : TargetRegistrations)
	{
		TUniquePtr<FMtoUMultiSubjectTarget> Target = MakeUnique<FMtoUMultiSubjectTarget>();
		FString TargetError;
		if (!Target->Initialize(Registration, TargetError))
		{
			OutError = TargetError;
			Targets.Reset();
			World = nullptr;
			return false;
		}
		if (FindTarget(Registration.Id) != nullptr)
		{
			OutError = FString::Printf(
				TEXT("target id '%s' is registered twice"), *Registration.Id);
			Targets.Reset();
			World = nullptr;
			return false;
		}
		// Two ids may never share one target: the same component driven twice
		// would make the two subjects fight over one pose.
		for (const TUniquePtr<FMtoUMultiSubjectTarget>& Existing : Targets)
		{
			if (Existing->GetComponent() == Target->GetComponent()
				|| Existing->GetAnchor() == Target->GetAnchor())
			{
				OutError = FString::Printf(
					TEXT("target ids '%s' and '%s' share one anchor/component; "
						"every subject needs its own Unreal target"),
					*Existing->GetId(), *Target->GetId());
				Targets.Reset();
				World = nullptr;
				return false;
			}
		}
		Targets.Add(MoveTemp(Target));
	}

	if (!StartListener(OutError))
	{
		Targets.Reset();
		World = nullptr;
		return false;
	}

	// A session never outlives the world it drives, whether the world is closed
	// by the editor or destroyed by a test.
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(
		this, &FMtoUMultiSubjectReceiver::HandleWorldCleanup);

	bRunning = true;
	bStarted = true;
	UE_LOG(LogTemp, Display, TEXT("%s: listening 127.0.0.1:%u scenario=%s targets=%d"),
		ReceiverLogCategory, BoundPort, *Config.Scenario, Targets.Num());
	return true;
}

bool FMtoUMultiSubjectReceiver::StartListener(FString& OutError)
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Sockets == nullptr)
	{
		OutError = TEXT("no socket subsystem is available");
		return false;
	}
	const FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), Config.Port);
	ListenSocket = FTcpSocketBuilder(TEXT("MtoUMultiSubject"))
		.AsReusable()
		.BoundToEndpoint(Endpoint)
		.Listening(4)
		.Build();
	if (ListenSocket == nullptr)
	{
		OutError = FString::Printf(TEXT("could not listen on 127.0.0.1:%u"), Config.Port);
		return false;
	}
	ListenSocket->SetNonBlocking(true);

	TSharedRef<FInternetAddr> BoundAddress = Sockets->CreateInternetAddr();
	// FSocket::GetAddress fills the address in place; with port 0 the bound port
	// is assigned by the OS and has to be read back for the Maya peer.
	ListenSocket->GetAddress(*BoundAddress);
	BoundPort = static_cast<uint16>(BoundAddress->GetPort());
	return true;
}

void FMtoUMultiSubjectReceiver::Stop(const FString& Reason)
{
	if (!bStarted)
	{
		return;
	}
	EndSession(Reason);
	CloseClient(Reason);
	if (ListenSocket != nullptr)
	{
		ListenSocket->Close();
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			Sockets->DestroySocket(ListenSocket);
		}
		ListenSocket = nullptr;
	}
	if (WorldCleanupHandle.IsValid())
	{
		FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
		WorldCleanupHandle.Reset();
	}
	bRunning = false;
	bStarted = false;
	bReady = false;
	World = nullptr;
	UE_LOG(LogTemp, Display, TEXT("%s: stopped (%s) after %d applied frames"),
		ReceiverLogCategory, *Reason, AppliedFrames);
}

void FMtoUMultiSubjectReceiver::HandleWorldCleanup(
	UWorld* InWorld,
	bool bSessionEnded,
	bool bCleanupResources)
{
	if (InWorld == nullptr || World == nullptr)
	{
		return;
	}
	if (InWorld != World)
	{
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("%s: world cleanup ends the session"), ReceiverLogCategory);
	Stop(TEXT("world_cleanup"));
}

void FMtoUMultiSubjectReceiver::Pump(double DeltaSeconds)
{
	if (!bRunning)
	{
		return;
	}
	if (ClientSocket == nullptr)
	{
		AcceptPendingClient();
	}
	ReadClientLines();
}

void FMtoUMultiSubjectReceiver::AcceptPendingClient()
{
	if (ListenSocket == nullptr)
	{
		return;
	}
	bool bPending = false;
	if (!ListenSocket->HasPendingConnection(bPending) || !bPending)
	{
		return;
	}
	FSocket* Accepted = ListenSocket->Accept(TEXT("MtoUMultiSubjectClient"));
	if (Accepted == nullptr)
	{
		return;
	}
	Accepted->SetNoDelay(true);
	ClientSocket = Accepted;
	ReceiveBytes.Reset();
	ClientDescription = TEXT("127.0.0.1");
	UE_LOG(LogTemp, Display, TEXT("%s: client connected"), ReceiverLogCategory);
}

void FMtoUMultiSubjectReceiver::ReadClientLines()
{
	if (ClientSocket == nullptr)
	{
		return;
	}
	if (ClientSocket->GetConnectionState() != SCS_Connected)
	{
		CloseClient(TEXT("disconnect"));
		return;
	}

	uint32 Pending = 0;
	while (ClientSocket != nullptr && ClientSocket->HasPendingData(Pending) && Pending > 0)
	{
		uint8 Buffer[4096];
		int32 Read = 0;
		if (!ClientSocket->Recv(Buffer, sizeof(Buffer), Read, ESocketReceiveFlags::None) || Read <= 0)
		{
			break;
		}
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
				// The 1 MiB bound applies to the bytes on the wire, so it is
				// enforced before the line is decoded into text.
				if (static_cast<int64>(Length) > MtoUMultiSubjectProtocol::MaxLineBytes)
				{
					NoteError(MtoUMultiSubjectError::TooLarge,
						FString::Printf(TEXT("client line of %d bytes exceeds the %lld byte bound"),
							Length, MtoUMultiSubjectProtocol::MaxLineBytes), LastAppliedSerial);
					SendError(MtoUMultiSubjectError::TooLarge,
						FString::Printf(TEXT("messages may not exceed %lld bytes"),
							MtoUMultiSubjectProtocol::MaxLineBytes));
					CloseClient(TEXT("oversize line"));
					ReceiveBytes.Reset();
					return;
				}
				const FUTF8ToTCHAR Converted(
					reinterpret_cast<const ANSICHAR*>(ReceiveBytes.GetData() + LineStart), Length);
				// The converter's buffer is length-delimited, not a C string.
				FString Line(Converted.Length(), Converted.Get());
				Line.TrimStartInline();
				if (Line.EndsWith(TEXT("\r")))
				{
					Line.LeftChopInline(1);
				}
				if (!Line.IsEmpty())
				{
					HandleClientLine(Line);
					if (ClientSocket == nullptr)
					{
						// The handler closed the connection (framing error) and
						// reset the receive buffer with it.
						ReceiveBytes.Reset();
						return;
					}
				}
			}
			LineStart = Index + 1;
		}
		if (LineStart > 0)
		{
			ReceiveBytes.RemoveAt(0, LineStart, EAllowShrinking::No);
		}

		// A client that never sends a newline must not grow this buffer forever.
		if (ClientSocket != nullptr && static_cast<int64>(ReceiveBytes.Num()) > MtoUMultiSubjectProtocol::MaxLineBytes)
		{
			NoteError(MtoUMultiSubjectError::TooLarge,
				FString::Printf(TEXT("a client line grew past %lld bytes without a newline"),
					MtoUMultiSubjectProtocol::MaxLineBytes), LastAppliedSerial);
			SendError(MtoUMultiSubjectError::TooLarge,
				FString::Printf(TEXT("messages may not exceed %lld bytes"),
					MtoUMultiSubjectProtocol::MaxLineBytes));
			CloseClient(TEXT("oversize line"));
			return;
		}
	}
	// BSD GetConnectionState caches "connected" for five seconds after the
	// last activity. Read readiness plus an unconsumed peek detects the FIN
	// promptly without eating the next partial JSON line.
	if (ClientSocket != nullptr
		&& ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
	{
		uint8 Probe = 0;
		int32 Peeked = 0;
		if (!ClientSocket->Recv(&Probe, 1, Peeked, ESocketReceiveFlags::Peek) || Peeked == 0)
		{
			CloseClient(TEXT("disconnect"));
		}
	}
}

void FMtoUMultiSubjectReceiver::HandleClientLine(const FString& Line)
{
	++ClientLineCount;
	if (Utf8Length(Line) > MtoUMultiSubjectProtocol::MaxLineBytes)
	{
		NoteError(MtoUMultiSubjectError::TooLarge,
			FString::Printf(TEXT("client line of %lld bytes exceeds the %lld byte bound"),
				Utf8Length(Line), MtoUMultiSubjectProtocol::MaxLineBytes), LastAppliedSerial);
		SendError(MtoUMultiSubjectError::TooLarge,
			FString::Printf(TEXT("messages may not exceed %lld bytes"),
				MtoUMultiSubjectProtocol::MaxLineBytes));
		CloseClient(TEXT("oversize line"));
		return;
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		// Whatever the client meant, the framing is broken: reply once and close.
		NoteError(MtoUMultiSubjectError::MalformedJson,
			FString::Printf(TEXT("client line is not one JSON object: %s"),
				*Reader->GetErrorMessage()), LastAppliedSerial);
		SendError(MtoUMultiSubjectError::MalformedJson,
			TEXT("every line must be exactly one JSON object"));
		CloseClient(TEXT("malformed json"));
		return;
	}

	FString Type;
	FMtoUProtocolError ProtocolError;
	if (!FMtoUMultiSubjectProtocol::PeekType(Object, Type, ProtocolError))
	{
		NoteShapeError(ProtocolError);
		SendError(ProtocolError.Code, ProtocolError.Details);
		return;
	}

	if (Type == TEXT("init"))
	{
		HandleInit(Object);
	}
	else if (Type == TEXT("frame"))
	{
		HandleFrame(Object);
	}
	else if (Type == TEXT("remove"))
	{
		HandleRemove(Object);
	}
	else
	{
		SendError(MtoUMultiSubjectError::MessageShape,
			FString::Printf(TEXT("message type '%s' is not part of this profile"), *Type));
	}
}

void FMtoUMultiSubjectReceiver::HandleInit(const TSharedPtr<FJsonObject>& Object)
{
	FMtoUInitMessage Init;
	FMtoUProtocolError ProtocolError;
	if (!FMtoUMultiSubjectProtocol::ParseInit(Object, Init, ProtocolError))
	{
		NoteShapeError(ProtocolError);
		SendError(ProtocolError.Code, ProtocolError.Details);
		return;
	}

	// Validation runs before anything changes: a refused init leaves a running
	// session exactly as it was, so a wrong pairing can never half-apply.
	TArray<FMtoUMultiSubjectTarget*> ResolvedTargets;
	TArray<FMtoUNegotiationMap> NegotiatedMaps;
	for (const FMtoUSubjectDeclaration& Declaration : Init.Subjects)
	{
		FMtoUMultiSubjectTarget* Target = FindTarget(Declaration.Id);
		if (Target == nullptr)
		{
			TArray<FString> RegisteredIds;
			for (const TUniquePtr<FMtoUMultiSubjectTarget>& Candidate : Targets)
			{
				RegisteredIds.Add(Candidate->GetId());
			}
			const FString Details = FString::Printf(
				TEXT("subject '%s' is not a registered target; registered ids: %s"),
				*Declaration.Id, *FString::Join(RegisteredIds, TEXT(", ")));
			NoteError(MtoUMultiSubjectError::UnknownSubjectId, Details, LastAppliedSerial);
			SendError(MtoUMultiSubjectError::UnknownSubjectId, Details);
			return;
		}
		// Defensive: registrations are already unique per target, but the check
		// is by target identity, so two ids can never drive one object even if a
		// caller builds the registration list differently.
		if (ResolvedTargets.ContainsByPredicate(
				[Target](const FMtoUMultiSubjectTarget* Claimed)
				{
					if (Claimed == nullptr || Claimed->GetComponent() == nullptr
						|| Target->GetComponent() == nullptr)
					{
						return false;
					}
					return Claimed->GetComponent() == Target->GetComponent()
						|| Claimed->GetAnchor() == Target->GetAnchor();
				}))
		{
			const FString Details = FString::Printf(
				TEXT("subject '%s' resolves to an Unreal target another subject of this init already claimed"),
				*Declaration.Id);
			NoteError(MtoUMultiSubjectError::TargetReused, Details, LastAppliedSerial);
			SendError(MtoUMultiSubjectError::TargetReused, Details);
			return;
		}
		FMtoUNegotiationMap Map;
		const FString Mismatch = Target->DescribeDeclarationMismatch(Declaration, Map);
		if (!Mismatch.IsEmpty())
		{
			const FString Details = FString::Printf(TEXT("subject '%s': %s"), *Declaration.Id, *Mismatch);
			NoteError(MtoUMultiSubjectError::SkeletonMismatch, Details, LastAppliedSerial);
			SendError(MtoUMultiSubjectError::SkeletonMismatch, Details);
			return;
		}
		ResolvedTargets.Add(Target);
		NegotiatedMaps.Add(MoveTemp(Map));
	}

	// The init is usable: a new negotiation always replaces the old session.
	EndSession(TEXT("new negotiation"));

	FString BeginError;
	if (!BeginSession(Init, NegotiatedMaps, BeginError))
	{
		NoteError(MtoUMultiSubjectError::PreviewConflict, BeginError, SessionId);
		SendError(MtoUMultiSubjectError::PreviewConflict, BeginError);
		EndSession(TEXT("takeover failed"));
		return;
	}

	SendJson(FMtoUMultiSubjectProtocol::MakeReady(SessionId));
	UE_LOG(LogTemp, Display, TEXT("%s: session %lld ready (%s)"),
		ReceiverLogCategory, SessionId, *Config.Scenario);
}

bool FMtoUMultiSubjectReceiver::BeginSession(
	const FMtoUInitMessage& Init,
	const TArray<FMtoUNegotiationMap>& Maps,
	FString& OutError)
{
	Subjects.Reset();
	for (int32 SubjectIndex = 0; SubjectIndex < Init.Subjects.Num(); ++SubjectIndex)
	{
		const FMtoUSubjectDeclaration& Declaration = Init.Subjects[SubjectIndex];
		FMtoUSessionSubject Subject;
		Subject.Id = Declaration.Id;
		Subject.Declaration = Declaration;
		Subject.Target = FindTarget(Declaration.Id);
		Subject.bEnabled = true;
		// The map was computed while the init was validated; it carries which
		// target bones this subject drives and which target bones stay at their
		// reference pose, so the evidence never implies a whole-skeleton match.
		if (Maps.IsValidIndex(SubjectIndex))
		{
			Subject.Map = Maps[SubjectIndex];
		}
		Subjects.Add(MoveTemp(Subject));
	}
	// A session is forgotten when it ends, but what it negotiated is evidence:
	// keep the last one so the written report still shows the mapping.
	LastNegotiatedSubjects.Reset();
	for (const FMtoUSessionSubject& Subject : Subjects)
	{
		FMtoUNegotiatedSubject Snapshot;
		Snapshot.Id = Subject.Id;
		Snapshot.Declaration = Subject.Declaration;
		Snapshot.Map = Subject.Map;
		LastNegotiatedSubjects.Add(MoveTemp(Snapshot));
	}

	++SessionId;
	LastAppliedSerial = 0;
	LastAppliedTime = 0.0;
	LastTimeDirection = TEXT("none");
	AppliedFrames = 0;
	FrameRecords.Reset();

	// A ready session always drives its targets through the preview takeover:
	// the prototype has no second way to write a pose.
	TArray<FMtoUMultiSubjectTarget*> TargetsToDrive;
	for (const FMtoUSessionSubject& Subject : Subjects)
	{
		TargetsToDrive.Add(Subject.Target);
	}
	FString PreviewError;
	if (!Preview.TakeOver(TargetsToDrive, Config.PreviewWriters, Config.EditorSequencer, PreviewError))
	{
		OutError = PreviewError;
		bReady = false;
		return false;
	}
	bReady = true;
	return true;
}

bool FMtoUMultiSubjectReceiver::RefuseStaleSession(int64 MessageSession, const TCHAR* What)
{
	if (MessageSession == SessionId)
	{
		return false;
	}
	const FString Details = FString::Printf(
		TEXT("%s belongs to session %lld; the current session is %lld"),
		What, MessageSession, SessionId);
	NoteError(MtoUMultiSubjectError::SessionMismatch, Details, LastAppliedSerial);
	SendError(MtoUMultiSubjectError::SessionMismatch, Details);
	return true;
}

void FMtoUMultiSubjectReceiver::HandleFrame(const TSharedPtr<FJsonObject>& Object)
{
	if (!bReady)
	{
		const FString Details = TEXT("no session is ready; send init first");
		NoteError(MtoUMultiSubjectError::NoSession, Details, 0);
		SendError(MtoUMultiSubjectError::NoSession, Details);
		return;
	}

	FMtoUFrameMessage Frame;
	FMtoUProtocolError ProtocolError;
	if (!FMtoUMultiSubjectProtocol::ParseFrame(Object, Frame, ProtocolError))
	{
		NoteShapeError(ProtocolError);
		SendError(ProtocolError.Code, ProtocolError.Details);
		return;
	}
	// A frame that still carries the session of a previous negotiation is
	// refused before anything else, so a renegotiated pair can never be driven
	// by a pose that was sampled for the old one.
	if (RefuseStaleSession(Frame.Session, TEXT("frame")))
	{
		return;
	}
	if (Frame.Serial <= LastAppliedSerial)
	{
		const FString Details = FString::Printf(
			TEXT("frame serial %lld is not greater than the last applied serial %lld"),
			Frame.Serial, LastAppliedSerial);
		NoteError(MtoUMultiSubjectError::FrameOrder, Details, Frame.Serial);
		SendError(MtoUMultiSubjectError::FrameOrder, Details);
		return;
	}
	// A frame must carry exactly the enabled subjects; a partial frame would
	// let two objects drift apart in time, which the profile forbids.
	TArray<FMtoUSessionSubject*> Enabled = EnabledSubjects();
	if (Frame.Subjects.Num() != Enabled.Num())
	{
		const FString Details = FString::Printf(
			TEXT("frame %lld carries %d subjects but %d are enabled"),
			Frame.Serial, Frame.Subjects.Num(), Enabled.Num());
		NoteError(MtoUMultiSubjectError::FrameSubjects, Details, Frame.Serial);
		SendError(MtoUMultiSubjectError::FrameSubjects, Details);
		return;
	}
	struct FFrameSubjectPlan
	{
		FMtoUSessionSubject* Subject = nullptr;
		const FMtoUFrameSubject* Frame = nullptr;
	};
	TArray<FFrameSubjectPlan> Plan;
	for (const FMtoUFrameSubject& FrameSubject : Frame.Subjects)
	{
		FMtoUSessionSubject* const* Found = Enabled.FindByPredicate(
			[&FrameSubject](const FMtoUSessionSubject* Candidate)
			{
				return Candidate != nullptr && Candidate->Id == FrameSubject.Id;
			});
		if (Found == nullptr || *Found == nullptr)
		{
			const FString Details = FString::Printf(
				TEXT("frame %lld carries '%s', which is not an enabled subject of session %lld"),
				Frame.Serial, *FrameSubject.Id, SessionId);
			NoteError(MtoUMultiSubjectError::FrameSubjects, Details, Frame.Serial);
			SendError(MtoUMultiSubjectError::FrameSubjects, Details);
			return;
		}
		if (FrameSubject.Transforms.Num() != (*Found)->Declaration.Bones.Num())
		{
			const FString Details = FString::Printf(
				TEXT("subject '%s' carries %d pose rows but %d bones were negotiated"),
				*FrameSubject.Id, FrameSubject.Transforms.Num(),
				(*Found)->Declaration.Bones.Num());
			NoteError(MtoUMultiSubjectError::FrameShape, Details, Frame.Serial);
			SendError(MtoUMultiSubjectError::FrameShape, Details);
			return;
		}
		if (FrameSubject.Curves.Num() != (*Found)->Declaration.Curves.Num())
		{
			const FString Details = FString::Printf(
				TEXT("subject '%s' carries %d curve values but %d curves were negotiated"),
				*FrameSubject.Id, FrameSubject.Curves.Num(), (*Found)->Declaration.Curves.Num());
			NoteError(MtoUMultiSubjectError::FrameShape, Details, Frame.Serial);
			SendError(MtoUMultiSubjectError::FrameShape, Details);
			return;
		}
		Plan.Add({ *Found, &FrameSubject });
	}

	// Every subject validated: apply the whole frame, or none of it. The record
	// keeps the direction the source time moved, so a reverse scrub or a
	// same-frame re-edit is visible in the evidence instead of looking like
	// ordinary forward playback.
	FMtoUFrameRecord Record;
	Record.Session = Frame.Session;
	Record.Serial = Frame.Serial;
	Record.Time = Frame.Time;
	Record.TimeDirection = FMtoUMultiSubjectProtocol::DescribeTimeDirection(
		LastAppliedTime, AppliedFrames > 0, Frame.Time);
	Record.bPreviewActive = Preview.IsActive();
	const double ApplyStart = FPlatformTime::Seconds();
	for (const FFrameSubjectPlan& Entry : Plan)
	{
		FMtoUSubjectMeasurement Measurement;
		FString TargetError;
		if (!Entry.Subject->Target->ApplyPose(
				Entry.Subject->Declaration, Entry.Subject->Map, *Entry.Frame,
				Measurement, TargetError))
		{
			NoteError(MtoUMultiSubjectError::PreviewConflict, TargetError, Frame.Serial);
			SendError(MtoUMultiSubjectError::PreviewConflict, TargetError);
			EndSession(TEXT("target lost"));
			return;
		}
		double MaxBoneDelta = 0.0;
		for (const FMtoUBoneMeasurement& Bone : Measurement.Bones)
		{
			MaxBoneDelta = FMath::Max(MaxBoneDelta, Bone.Delta);
		}
		Entry.Subject->MaxBoneDelta = FMath::Max(Entry.Subject->MaxBoneDelta, MaxBoneDelta);
		Entry.Subject->MaxRootWorldDelta = FMath::Max(
			Entry.Subject->MaxRootWorldDelta, Measurement.RootWorldDelta);
		++Entry.Subject->AppliedFrames;
		Record.Subjects.Add(MoveTemp(Measurement));
	}
	Record.ApplySeconds = FPlatformTime::Seconds() - ApplyStart;

	LastAppliedSerial = Frame.Serial;
	LastAppliedTime = Frame.Time;
	LastTimeDirection = Record.TimeDirection;
	++AppliedFrames;
	++ReceivedFrames;
	if (FrameRecords.Num() < Config.MaxRecordedFrames)
	{
		FrameRecords.Add(MoveTemp(Record));
	}

	// A frame reply reports exactly the subjects the frame carried, so the
	// Maya side can match the reply against its own active set.
	TArray<FMtoUSubjectStatus> Statuses;
	CollectSubjectStatuses(Statuses, /*bEnabledOnly=*/true);
	SendJson(FMtoUMultiSubjectProtocol::MakeApplied(SessionId, Frame.Serial, Frame.Time, Statuses));
}

void FMtoUMultiSubjectReceiver::HandleRemove(const TSharedPtr<FJsonObject>& Object)
{
	if (!bReady)
	{
		const FString Details = TEXT("no session is ready; send init first");
		NoteError(MtoUMultiSubjectError::NoSession, Details, 0);
		SendError(MtoUMultiSubjectError::NoSession, Details);
		return;
	}

	FMtoURemoveMessage Remove;
	FMtoUProtocolError ProtocolError;
	if (!FMtoUMultiSubjectProtocol::ParseRemove(Object, Remove, ProtocolError))
	{
		NoteShapeError(ProtocolError);
		SendError(ProtocolError.Code, ProtocolError.Details);
		return;
	}
	// Removing a subject of a previous negotiation must not end this pair.
	if (RefuseStaleSession(Remove.Session, TEXT("remove")))
	{
		return;
	}


	FMtoUSessionSubject* Found = nullptr;
	for (FMtoUSessionSubject& Subject : Subjects)
	{
		if (Subject.bEnabled && Subject.Id == Remove.Id)
		{
			Found = &Subject;
			break;
		}
	}
	if (Found == nullptr)
	{
		const bool bKnownButDisabled = Subjects.ContainsByPredicate(
			[&Remove](const FMtoUSessionSubject& Subject) { return Subject.Id == Remove.Id; });
		const FString Details = bKnownButDisabled
			? FString::Printf(TEXT("subject '%s' is already disabled in session %lld"), *Remove.Id, SessionId)
			: FString::Printf(TEXT("subject '%s' is not part of session %lld"), *Remove.Id, SessionId);
		const FString Code = bKnownButDisabled
			? FString(MtoUMultiSubjectError::SubjectNotEnabled)
			: FString(MtoUMultiSubjectError::UnknownSubjectId);
		NoteError(Code, Details, LastAppliedSerial);
		SendError(Code, Details);
		return;
	}

	// Removing never leaves residue: that target goes back to the animation
	// state the takeover saved and the writers of that one subject are enabled
	// again, while the other subject keeps streaming.
	Found->bEnabled = false;
	if (Found->Target != nullptr)
	{
		FString TargetError;
		if (!Preview.RestoreTarget(*Found->Target, Config.EditorSequencer, TargetError))
		{
			NoteError(MtoUMultiSubjectError::PreviewConflict, TargetError, LastAppliedSerial);
		}
	}

	// The removal acknowledgement reports the whole subject set: the removed
	// subject as disabled and every remaining subject as applied.
	TArray<FMtoUSubjectStatus> Statuses;
	CollectSubjectStatuses(Statuses, /*bEnabledOnly=*/false);
	SendJson(FMtoUMultiSubjectProtocol::MakeApplied(SessionId, LastAppliedSerial, LastAppliedTime, Statuses));

	const bool bAnyEnabled = Subjects.ContainsByPredicate(
		[](const FMtoUSessionSubject& Subject) { return Subject.bEnabled; });
	if (!bAnyEnabled)
	{
		EndSession(TEXT("every subject was removed"));
	}
}

void FMtoUMultiSubjectReceiver::EndSession(const FString& Reason)
{
	if (!bReady && Subjects.IsEmpty())
	{
		return;
	}
	FString PreviewError;
	if (Preview.IsActive() && !Preview.Restore(Config.EditorSequencer, PreviewError))
	{
		NoteError(MtoUMultiSubjectError::PreviewConflict, PreviewError, LastAppliedSerial);
	}
	for (FMtoUSessionSubject& Subject : Subjects)
	{
		if (Subject.Target != nullptr && Subject.Target->IsDriving())
		{
			FString TargetError;
			Subject.Target->Restore(TargetError);
		}
	}
	Subjects.Reset();
	bReady = false;
	LastSessionEndReason = Reason;
}

void FMtoUMultiSubjectReceiver::CloseClient(const FString& Reason)
{
	// Any closed transport relinquishes its subjects and preview writers.
	// This includes bad framing and send failure, not just an orderly FIN.
	EndSession(Reason);
	if (ClientSocket != nullptr)
	{
		ClientSocket->Close();
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			Sockets->DestroySocket(ClientSocket);
		}
		ClientSocket = nullptr;
	}
	ReceiveBytes.Reset();
	if (bRunning)
	{
		UE_LOG(LogTemp, Display, TEXT("%s: client closed (%s) after %d applied frames"),
			ReceiverLogCategory, *Reason, AppliedFrames);
	}
}

void FMtoUMultiSubjectReceiver::SendJson(const TSharedRef<FJsonObject>& Object)
{
	if (ClientSocket == nullptr)
	{
		return;
	}
	const FString Line = FMtoUMultiSubjectProtocol::ToLine(Object);
	const FTCHARToUTF8 Utf8(*Line);
	const int32 TotalBytes = Utf8.Length();
	int32 SentBytes = 0;
	while (SentBytes < TotalBytes)
	{
		int32 Sent = 0;
		if (!ClientSocket->Send(
				reinterpret_cast<const uint8*>(Utf8.Get()) + SentBytes,
				TotalBytes - SentBytes, Sent)
			|| Sent <= 0)
		{
			// A reply that cannot be delivered is a failed client: never leave a
			// half-written reply on the wire.
			UE_LOG(LogTemp, Warning, TEXT("%s: a reply could not be sent; closing the client"),
				ReceiverLogCategory);
			CloseClient(TEXT("reply could not be sent"));
			return;
		}
		SentBytes += Sent;
	}
}

void FMtoUMultiSubjectReceiver::SendError(const FString& Code, const FString& Details)
{
	LastErrorCode = Code;
	LastErrorDetails = Details;
	SendJson(FMtoUMultiSubjectProtocol::MakeError(Code, Details));
}

void FMtoUMultiSubjectReceiver::NoteError(const FString& Code, const FString& Details, int64 AtSerial)
{
	if (Errors.Num() >= 512)
	{
		return;
	}
	Errors.Add({ Code, Details, AtSerial });
	LastErrorCode = Code;
	LastErrorDetails = Details;
}

void FMtoUMultiSubjectReceiver::NoteShapeError(const FMtoUProtocolError& Error)
{
	NoteError(Error.Code, Error.Details, LastAppliedSerial);
}

TArray<FMtoUSessionSubject*> FMtoUMultiSubjectReceiver::EnabledSubjects()
{
	TArray<FMtoUSessionSubject*> Enabled;
	for (FMtoUSessionSubject& Subject : Subjects)
	{
		if (Subject.bEnabled)
		{
			Enabled.Add(&Subject);
		}
	}
	return Enabled;
}

void FMtoUMultiSubjectReceiver::CollectSubjectStatuses(
	TArray<FMtoUSubjectStatus>& OutStatuses,
	bool bEnabledOnly) const
{
	OutStatuses.Reset();
	for (const FMtoUSessionSubject& Subject : Subjects)
	{
		if (bEnabledOnly && !Subject.bEnabled)
		{
			continue;
		}
		FMtoUSubjectStatus Status;
		Status.Id = Subject.Id;
		Status.Status = Subject.bEnabled ? TEXT("applied") : TEXT("disabled");
		OutStatuses.Add(MoveTemp(Status));
	}
}

FMtoUMultiSubjectTarget* FMtoUMultiSubjectReceiver::FindTarget(const FString& Id) const
{
	for (const TUniquePtr<FMtoUMultiSubjectTarget>& Target : Targets)
	{
		if (Target.IsValid() && Target->GetId() == Id)
		{
			return Target.Get();
		}
	}
	return nullptr;
}

void FMtoUMultiSubjectReceiver::CollectNegotiatedSubjects(
	TArray<FMtoUNegotiatedSubject>& OutSubjects) const
{
	OutSubjects.Reset();
	for (const FMtoUSessionSubject& Subject : Subjects)
	{
		FMtoUNegotiatedSubject Snapshot;
		Snapshot.Id = Subject.Id;
		Snapshot.Declaration = Subject.Declaration;
		Snapshot.Map = Subject.Map;
		OutSubjects.Add(MoveTemp(Snapshot));
	}
}

TSharedRef<FJsonObject> FMtoUMultiSubjectReceiver::DescribeSubjectEvidence(
	const FString& Id,
	const FMtoUSubjectDeclaration& Declaration,
	const FMtoUNegotiationMap& Map,
	bool bEnabled,
	int64 AppliedFrames,
	double MaxBoneDelta,
	double MaxRootWorldDelta)
{
	const TSharedRef<FJsonObject> SubjectObject = MakeShared<FJsonObject>();
	SubjectObject->SetStringField(TEXT("id"), Id);
	SubjectObject->SetStringField(TEXT("root"), Declaration.Root);
	SubjectObject->SetBoolField(TEXT("enabled"), bEnabled);
	SubjectObject->SetNumberField(TEXT("applied_frames"), static_cast<double>(AppliedFrames));
	SubjectObject->SetNumberField(TEXT("max_bone_delta"), MaxBoneDelta);
	SubjectObject->SetNumberField(TEXT("max_root_world_delta"), MaxRootWorldDelta);
	SubjectObject->SetNumberField(TEXT("declared_bones"),
		static_cast<double>(Declaration.Bones.Num()));
	// What the negotiation established: the driven/required coverage of the
	// target, the ignored source branches, and the target bones that keep their
	// reference pose.
	SubjectObject->SetNumberField(TEXT("driven_bones"),
		static_cast<double>(Map.DrivenTargetBones.Num()));
	SubjectObject->SetNumberField(TEXT("required_target_bones"),
		static_cast<double>(Map.RequiredTargetBones.Num()));
	SubjectObject->SetNumberField(TEXT("source_only_bones"),
		static_cast<double>(Map.SourceOnlyBones));
	// How far the two rigs rest apart after the constant root frame: the
	// bind/frame projection absorbs this, and the numbers stay visible.
	SubjectObject->SetNumberField(TEXT("rest_deviation_translation_cm"), Map.MaxRestTranslationCm);
	SubjectObject->SetNumberField(TEXT("rest_deviation_rotation_degrees"), Map.MaxRestRotationDegrees);
	SubjectObject->SetNumberField(TEXT("rest_deviation_scale"), Map.MaxRestScale);
	SubjectObject->SetNumberField(TEXT("undriven_bones"),
		static_cast<double>(Map.UndrivenTargetBones.Num()));
	TArray<TSharedPtr<FJsonValue>> UndrivenValues;
	for (const FName& Bone : Map.UndrivenTargetBones)
	{
		UndrivenValues.Add(MakeShared<FJsonValueString>(Bone.ToString()));
	}
	SubjectObject->SetArrayField(TEXT("undriven_bone_names"), UndrivenValues);
	TArray<TSharedPtr<FJsonValue>> CurveValues;
	for (const FName& Curve : Declaration.Curves)
	{
		CurveValues.Add(MakeShared<FJsonValueString>(Curve.ToString()));
	}
	SubjectObject->SetArrayField(TEXT("curves"), CurveValues);
	TArray<TSharedPtr<FJsonValue>> SourceOnlyCurveValues;
	for (const FName& Curve : Map.SourceOnlyCurves)
	{
		SourceOnlyCurveValues.Add(MakeShared<FJsonValueString>(Curve.ToString()));
	}
	SubjectObject->SetArrayField(TEXT("source_only_curve_names"), SourceOnlyCurveValues);
	TArray<TSharedPtr<FJsonValue>> RenameValues;
	for (const FString& Rename : Map.ImportRenames)
	{
		RenameValues.Add(MakeShared<FJsonValueString>(Rename));
	}
	SubjectObject->SetArrayField(TEXT("import_renames"), RenameValues);
	return SubjectObject;
}

TArray<FString> FMtoUMultiSubjectReceiver::DescribeDriveOwnership() const
{
	// Drive ownership is a question an operator has to be able to answer ("who
	// writes this pose now?"), so it is reported explicitly instead of being
	// left implicit in a hidden local mute. Writers the preview muted are
	// separated from writers an earlier exit already restored, so a restored
	// writer is never mistaken for a write the preview is still suppressing.
	TArray<FString> Lines;
	const TArray<FMtoUSuppressedWriter>& Writers = Preview.GetSuppressedWriters();
	for (const TUniquePtr<FMtoUMultiSubjectTarget>& Target : Targets)
	{
		if (!Target.IsValid())
		{
			continue;
		}
		TArray<FString> SuppressedNames;
		TArray<FString> RestoredNames;
		for (const FMtoUSuppressedWriter& Writer : Writers)
		{
			if (Writer.TargetId != Target->GetId() && !Writer.TargetId.IsEmpty())
			{
				continue;
			}
			const FString Name = FString::Printf(TEXT("%s (saved_asset=%s, muted_now=%s)"),
				Writer.Track.IsValid() ? *Writer.Track->GetName() : TEXT("<no track>"),
				Writer.bRepresentsSavedAsset ? TEXT("true") : TEXT("false"),
				!Writer.bRestored ? TEXT("true") : TEXT("false"));
			(Writer.bRestored ? RestoredNames : SuppressedNames).Add(Name);
		}
		Lines.Add(FString::Printf(
			TEXT("%s: component_driver=%s, suppressed_writers=[%s], restored_writers=[%s], exit=%s"),
			*Target->GetId(),
			Target->GetSnapshot().PriorDriver.IsEmpty()
				? TEXT("not_taken_over")
				: *Target->GetSnapshot().PriorDriver,
			*FString::Join(SuppressedNames, TEXT("; ")),
			*FString::Join(RestoredNames, TEXT("; ")),
			Target->DescribeExitState()));
	}
	return Lines;
}

bool FMtoUMultiSubjectReceiver::IsListening() const
{
	return ListenSocket != nullptr;
}

bool FMtoUMultiSubjectReceiver::SaveEvidence(
	const FString& Directory,
	FString& OutPath,
	FString& OutError) const
{
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		OutError = FString::Printf(TEXT("could not create the evidence directory %s"), *Directory);
		return false;
	}
	const FString ScenarioName = Config.Scenario.IsEmpty() ? TEXT("session") : Config.Scenario;
	OutPath = FPaths::Combine(Directory, FString::Printf(TEXT("mtou-multi-subject-%s-unreal.json"), *ScenarioName));

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("mtou-multi-subject-evidence/1"));
	Root->SetStringField(TEXT("runner"), TEXT("unreal"));
	Root->SetStringField(TEXT("scenario"), ScenarioName);
	Root->SetNumberField(TEXT("listening_port"), static_cast<double>(BoundPort));
	Root->SetBoolField(TEXT("session_ready"), bReady);
	Root->SetNumberField(TEXT("session"), static_cast<double>(SessionId));
	Root->SetNumberField(TEXT("client_lines"), static_cast<double>(ClientLineCount));
	Root->SetNumberField(TEXT("received_frames"), static_cast<double>(ReceivedFrames));
	Root->SetNumberField(TEXT("applied_frames"), static_cast<double>(AppliedFrames));
	Root->SetNumberField(TEXT("last_applied_serial"), static_cast<double>(LastAppliedSerial));
	Root->SetNumberField(TEXT("last_applied_time"), LastAppliedTime);
	Root->SetStringField(TEXT("last_time_direction"), LastTimeDirection);
	Root->SetStringField(TEXT("last_error_code"), LastErrorCode);
	Root->SetStringField(TEXT("last_error_details"), LastErrorDetails);
	Root->SetStringField(TEXT("session_end_reason"), LastSessionEndReason);
	Root->SetBoolField(TEXT("listening"), IsListening());
	Root->SetBoolField(TEXT("client_connected"), ClientSocket != nullptr);
	Root->SetBoolField(TEXT("preview_active"), Preview.IsActive());
	Root->SetBoolField(TEXT("preview_writers_suppressed"),
		Preview.IsActive() && Preview.AreWritersSuppressed());
	Root->SetBoolField(TEXT("ok"), Errors.IsEmpty() && AppliedFrames > 0 && !bReady);

	TArray<TSharedPtr<FJsonValue>> TargetValues;
	for (const TUniquePtr<FMtoUMultiSubjectTarget>& Target : Targets)
	{
		if (!Target.IsValid())
		{
			continue;
		}
		const FMtoUAnchorCheck& Anchor = Target->GetAnchorCheck();
		const TSharedRef<FJsonObject> TargetObject = MakeShared<FJsonObject>();
		TargetObject->SetStringField(TEXT("id"), Target->GetId());
		TargetObject->SetStringField(TEXT("anchor_actor"),
			Target->GetAnchor() != nullptr ? Target->GetAnchor()->GetActorNameOrLabel() : FString());
		TargetObject->SetStringField(TEXT("component"),
			Target->GetComponent() != nullptr ? Target->GetComponent()->GetName() : FString());
		TargetObject->SetStringField(TEXT("skeleton"), Target->GetSkeletonSignature());
		TargetObject->SetBoolField(TEXT("anchor_valid"), Anchor.bValid);
		TargetObject->SetBoolField(TEXT("attached_to_own_root"), Anchor.bAttachedToOwnRoot);
		TargetObject->SetBoolField(TEXT("anchor_attached_to_actor"), Anchor.bActorAttached);
		TargetObject->SetStringField(TEXT("socket"), Anchor.SocketName.ToString());
		TargetObject->SetObjectField(TEXT("anchor_transform"), MakeTransformObject(Anchor.AnchorTransform));
		TargetObject->SetStringField(TEXT("saved_animation_state"), Target->GetSnapshot().Describe());
		TargetObject->SetBoolField(TEXT("had_animation_driver"),
			Target->GetSnapshot().bHadAnimationDriver);
		TargetObject->SetStringField(TEXT("prior_driver"),
			Target->GetSnapshot().PriorDriver.IsEmpty()
				? TEXT("not_taken_over")
				: Target->GetSnapshot().PriorDriver);
		// The current ownership state, not a historical flag: `exit` is
		// `preview_active` while the preview drives the target, and
		// `restored_to_reference_pose` can only be true for a completed exit.
		TargetObject->SetBoolField(TEXT("driving"), Target->IsDriving());
		TargetObject->SetStringField(TEXT("exit"), Target->DescribeExitState());
		TargetObject->SetBoolField(TEXT("restored_to_reference_pose"),
			Target->DescribeExitState() == FString(TEXT("reference_pose")));
		TargetValues.Add(MakeShared<FJsonValueObject>(TargetObject));
	}
	Root->SetArrayField(TEXT("targets"), TargetValues);

	TArray<TSharedPtr<FJsonValue>> SubjectValues;
	for (const FMtoUSessionSubject& Subject : Subjects)
	{
		SubjectValues.Add(MakeShared<FJsonValueObject>(
			DescribeSubjectEvidence(Subject.Id, Subject.Declaration, Subject.Map, Subject.bEnabled,
				Subject.AppliedFrames, Subject.MaxBoneDelta, Subject.MaxRootWorldDelta)));
	}
	Root->SetArrayField(TEXT("subjects"), SubjectValues);

	// The last session is gone by the time most reports are written, so what it
	// negotiated is reported separately and survives the session end.
	TArray<TSharedPtr<FJsonValue>> NegotiatedValues;
	for (const FMtoUNegotiatedSubject& Subject : LastNegotiatedSubjects)
	{
		NegotiatedValues.Add(MakeShared<FJsonValueObject>(
			DescribeSubjectEvidence(Subject.Id, Subject.Declaration, Subject.Map,
				/*bEnabled=*/true, 0, 0.0, 0.0)));
	}
	Root->SetArrayField(TEXT("negotiated"), NegotiatedValues);

	TArray<TSharedPtr<FJsonValue>> OwnershipValues;
	for (const FString& Line : DescribeDriveOwnership())
	{
		OwnershipValues.Add(MakeShared<FJsonValueString>(Line));
	}
	Root->SetArrayField(TEXT("drive_ownership"), OwnershipValues);

	TArray<TSharedPtr<FJsonValue>> WriterValues;
	for (const FMtoUSuppressedWriter& Writer : Preview.GetSuppressedWriters())
	{
		const TSharedRef<FJsonObject> WriterObject = MakeShared<FJsonObject>();
		WriterObject->SetStringField(TEXT("track"),
			Writer.Track.IsValid() ? Writer.Track->GetName() : FString());
		WriterObject->SetBoolField(TEXT("track_disabled"), Writer.bTrackDisabled);
		WriterObject->SetBoolField(TEXT("track_was_locally_disabled"), Writer.bTrackWasLocallyDisabled);
		WriterObject->SetBoolField(TEXT("represents_saved_asset"), Writer.bRepresentsSavedAsset);
		WriterObject->SetBoolField(TEXT("player_paused"), Writer.bPlayerPaused);
		// A restored writer was muted by an earlier exit; only an unrestored one
		// is suppressed by the preview right now.
		WriterObject->SetBoolField(TEXT("restored"), Writer.bRestored);
		WriterObject->SetBoolField(TEXT("muted_now"), !Writer.bRestored);
		WriterValues.Add(MakeShared<FJsonValueObject>(WriterObject));
	}
	Root->SetArrayField(TEXT("preview_writers"), WriterValues);

	TArray<TSharedPtr<FJsonValue>> FrameValues;
	for (const FMtoUFrameRecord& Record : FrameRecords)
	{
		const TSharedRef<FJsonObject> FrameObject = MakeShared<FJsonObject>();
		FrameObject->SetNumberField(TEXT("session"), static_cast<double>(Record.Session));
		FrameObject->SetNumberField(TEXT("serial"), static_cast<double>(Record.Serial));
		FrameObject->SetNumberField(TEXT("time"), Record.Time);
		FrameObject->SetStringField(TEXT("time_direction"), Record.TimeDirection);
		FrameObject->SetNumberField(TEXT("apply_ms"), Record.ApplySeconds * 1000.0);
		FrameObject->SetBoolField(TEXT("preview_active"), Record.bPreviewActive);
		TArray<TSharedPtr<FJsonValue>> FrameSubjectValues;
		for (const FMtoUSubjectMeasurement& Measurement : Record.Subjects)
		{
			const TSharedRef<FJsonObject> SubjectObject = MakeShared<FJsonObject>();
			SubjectObject->SetStringField(TEXT("id"), Measurement.Id);
			SubjectObject->SetNumberField(TEXT("root_world_delta"), Measurement.RootWorldDelta);
			SubjectObject->SetNumberField(TEXT("anchor_x"), Measurement.AnchorLocation.X);
			SubjectObject->SetNumberField(TEXT("anchor_y"), Measurement.AnchorLocation.Y);
			SubjectObject->SetNumberField(TEXT("anchor_z"), Measurement.AnchorLocation.Z);
			TArray<TSharedPtr<FJsonValue>> BoneValues;
			double MaxBoneDelta = 0.0;
			for (const FMtoUBoneMeasurement& Bone : Measurement.Bones)
			{
				MaxBoneDelta = FMath::Max(MaxBoneDelta, Bone.Delta);
				const TSharedRef<FJsonObject> BoneObject = MakeShared<FJsonObject>();
				BoneObject->SetStringField(TEXT("bone"), Bone.BoneName.ToString());
				TArray<TSharedPtr<FJsonValue>> Received;
				TArray<TSharedPtr<FJsonValue>> Applied;
				AddNumberArray(Received, Bone.ReceivedLocal);
				AddNumberArray(Applied, Bone.ComponentSpace);
				BoneObject->SetArrayField(TEXT("received_local"), Received);
				BoneObject->SetArrayField(TEXT("component_space"), Applied);
				BoneObject->SetNumberField(TEXT("delta"), Bone.Delta);
				BoneValues.Add(MakeShared<FJsonValueObject>(BoneObject));
			}
			SubjectObject->SetNumberField(TEXT("max_bone_delta"), MaxBoneDelta);
			SubjectObject->SetNumberField(TEXT("root_world_x"), Measurement.RootWorld.GetTranslation().X);
			SubjectObject->SetNumberField(TEXT("root_world_y"), Measurement.RootWorld.GetTranslation().Y);
			SubjectObject->SetNumberField(TEXT("root_world_z"), Measurement.RootWorld.GetTranslation().Z);
			SubjectObject->SetArrayField(TEXT("bones"), BoneValues);
			TArray<TSharedPtr<FJsonValue>> CurveValues;
			for (const TPair<FName, float>& Curve : Measurement.Curves)
			{
				const TSharedRef<FJsonObject> CurveObject = MakeShared<FJsonObject>();
				CurveObject->SetStringField(TEXT("name"), Curve.Key.ToString());
				CurveObject->SetNumberField(TEXT("value"), Curve.Value);
				CurveValues.Add(MakeShared<FJsonValueObject>(CurveObject));
			}
			SubjectObject->SetArrayField(TEXT("curves"), CurveValues);
			FrameSubjectValues.Add(MakeShared<FJsonValueObject>(SubjectObject));
		}
		FrameObject->SetArrayField(TEXT("subjects"), FrameSubjectValues);
		FrameValues.Add(MakeShared<FJsonValueObject>(FrameObject));
	}
	Root->SetArrayField(TEXT("frames"), FrameValues);

	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FMtoUEvidenceError& Error : Errors)
	{
		const TSharedRef<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
		ErrorObject->SetStringField(TEXT("code"), Error.Code);
		ErrorObject->SetStringField(TEXT("details"), Error.Details);
		ErrorObject->SetNumberField(TEXT("at_serial"), static_cast<double>(Error.AtSerial));
		ErrorValues.Add(MakeShared<FJsonValueObject>(ErrorObject));
	}
	Root->SetArrayField(TEXT("errors"), ErrorValues);

	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("could not serialize the evidence");
		return false;
	}
	if (!FFileHelper::SaveStringToFile(Text, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("could not write %s"), *OutPath);
		return false;
	}
	return true;
}
