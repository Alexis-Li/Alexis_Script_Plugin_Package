// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MtoUMultiSubjectDriver.h"
#include "MtoUMultiSubjectFixture.h"
#include "MtoUMultiSubjectPeer.h"
#include "MtoUMultiSubjectPreview.h"
#include "MtoUMultiSubjectProtocol.h"
#include "MtoUMultiSubjectReceiver.h"
#include "ReferenceSkeleton.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "UObject/Package.h"

namespace
{
	/** Evidence directory for this run; `-MtoUEvidence=` overrides it. */
	FString EvidenceDirectory()
	{
		FString Override;
		if (FParse::Value(FCommandLine::Get(), TEXT("MtoUEvidence="), Override) && !Override.IsEmpty())
		{
			return Override;
		}
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MtoUMultiSubjectTests"));
	}

	uint16 ReserveLoopbackPort()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (Sockets == nullptr)
		{
			return 0;
		}
		FSocket* Socket = Sockets->CreateSocket(NAME_Stream, TEXT("MtoU multi subject port probe"));
		if (Socket == nullptr)
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

	FString PoseNumbers(const FTransform& Transform)
	{
		const FVector Translation = Transform.GetTranslation();
		const FQuat Rotation = Transform.GetRotation();
		const FVector Scale = Transform.GetScale3D();
		return FString::Printf(
			TEXT("[%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g]"),
			Translation.X, Translation.Y, Translation.Z,
			Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
			Scale.X, Scale.Y, Scale.Z);
	}

	/** The wire encoder of the scripted peer: one line per message, exactly as the contract reads. */
	FString EncodeInit(const FMtoUInitMessage& Init)
	{
		TArray<FString> SubjectLines;
		for (const FMtoUSubjectDeclaration& Subject : Init.Subjects)
		{
			TArray<FString> BoneLines;
			for (const FMtoUBoneDeclaration& Bone : Subject.Bones)
			{
				BoneLines.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"parent\":%d}"),
					*Bone.Name.ToString(), Bone.Parent));
			}
			TArray<FString> CurveLines;
			for (const FName& Curve : Subject.Curves)
			{
				CurveLines.Add(FString::Printf(TEXT("\"%s\""), *Curve.ToString()));
			}
			TArray<FString> BindLines;
			for (const FTransform& Bind : Subject.Bind)
			{
				BindLines.Add(PoseNumbers(Bind));
			}
			SubjectLines.Add(FString::Printf(
				TEXT("{\"id\":\"%s\",\"root\":\"%s\",\"bones\":[%s],\"curves\":[%s],\"bind\":[%s]}"),
				*Subject.Id, *Subject.Root,
				*FString::Join(BoneLines, TEXT(",")),
				*FString::Join(CurveLines, TEXT(",")),
				*FString::Join(BindLines, TEXT(","))));
		}
		return FString::Printf(
			TEXT("{\"type\":\"init\",\"version\":%d,\"fps\":%.17g,\"subjects\":[%s]}\n"),
			Init.Version, Init.Fps, *FString::Join(SubjectLines, TEXT(",")));
	}

	FString EncodeFrame(const FMtoUFrameMessage& Frame)
	{
		TArray<FString> SubjectLines;
		for (const FMtoUFrameSubject& Subject : Frame.Subjects)
		{
			TArray<FString> TransformLines;
			for (const FTransform& Transform : Subject.Transforms)
			{
				TransformLines.Add(PoseNumbers(Transform));
			}
			TArray<FString> CurveLines;
			for (const float Value : Subject.Curves)
			{
				CurveLines.Add(FString::Printf(TEXT("%.9g"), Value));
			}
			SubjectLines.Add(FString::Printf(
				TEXT("{\"id\":\"%s\",\"transforms\":[%s],\"curves\":[%s]}"),
				*Subject.Id,
				*FString::Join(TransformLines, TEXT(",")),
				*FString::Join(CurveLines, TEXT(","))));
		}
		return FString::Printf(
			TEXT("{\"type\":\"frame\",\"serial\":%lld,\"time\":%.17g,\"subjects\":[%s]}\n"),
			Frame.Serial, Frame.Time, *FString::Join(SubjectLines, TEXT(",")));
	}

	FString EncodeRemove(const FString& Id)
	{
		return FString::Printf(TEXT("{\"type\":\"remove\",\"id\":\"%s\"}\n"), *Id);
	}

	bool ParseJsonLine(const FString& Line, TSharedPtr<FJsonObject>& OutObject)
	{
		return FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Line), OutObject)
			&& OutObject.IsValid();
	}

	/** A raw TCP client that speaks the prototype profile and pumps the receiver while waiting. */
	class FMtoUScriptedPeer
	{
	public:
		~FMtoUScriptedPeer()
		{
			Close();
		}

		bool Connect(uint16 Port, FString& OutError)
		{
			ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (Sockets == nullptr)
			{
				OutError = TEXT("no socket subsystem");
				return false;
			}
			Socket = Sockets->CreateSocket(NAME_Stream, TEXT("MtoU scripted peer"), false);
			if (Socket == nullptr)
			{
				OutError = TEXT("could not create a client socket");
				return false;
			}
			const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
			bool bValidAddress = false;
			Address->SetIp(TEXT("127.0.0.1"), bValidAddress);
			Address->SetPort(Port);
			if (!Socket->Connect(*Address))
			{
				OutError = FString::Printf(TEXT("could not connect to 127.0.0.1:%u"), Port);
				return false;
			}
			Socket->SetNonBlocking(false);
			ReceiveBytes.Reset();
			return true;
		}

		bool Send(const FString& Text, FString& OutError)
		{
			if (Socket == nullptr)
			{
				OutError = TEXT("the scripted peer is not connected");
				return false;
			}
			const FTCHARToUTF8 Utf8(*Text);
			int32 Total = 0;
			while (Total < Utf8.Length())
			{
				int32 Sent = 0;
				if (!Socket->Send(reinterpret_cast<const uint8*>(Utf8.Get()) + Total,
						Utf8.Length() - Total, Sent)
					|| Sent <= 0)
				{
					OutError = TEXT("the scripted peer could not send");
					return false;
				}
				Total += Sent;
			}
			return true;
		}

		/** Reads one reply line, pumping the receiver so its session advances. */
		bool ReadLine(
			FMtoUMultiSubjectReceiver& Receiver,
			FString& OutLine,
			FString& OutError,
			double TimeoutSeconds = 15.0)
		{
			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
			while (FPlatformTime::Seconds() < Deadline)
			{
				Receiver.Pump(0.0);
				if (Socket != nullptr)
				{
					uint32 Pending = 0;
					while (Socket->HasPendingData(Pending) && Pending > 0)
					{
						uint8 Buffer[4096];
						int32 Read = 0;
						if (!Socket->Recv(Buffer, sizeof(Buffer), Read, ESocketReceiveFlags::None) || Read <= 0)
						{
							break;
						}
						ReceiveBytes.Append(Buffer, Read);
					}
				}
				const int32 NewlineIndex = ReceiveBytes.IndexOfByPredicate(
					[](const uint8 Byte) { return Byte == 0x0A; });
				if (NewlineIndex != INDEX_NONE)
				{
					const FUTF8ToTCHAR Converted(
						reinterpret_cast<const ANSICHAR*>(ReceiveBytes.GetData()), NewlineIndex);
					FString Line(Converted.Length(), Converted.Get());
					Line.TrimStartInline();
					ReceiveBytes.RemoveAt(0, NewlineIndex + 1, EAllowShrinking::No);
					OutLine = Line;
					return true;
				}
				FPlatformProcess::Sleep(0.001f);
			}
			OutError = TEXT("timed out waiting for a reply line");
			return false;
		}

		void Close()
		{
			if (Socket != nullptr)
			{
				Socket->Close();
				if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
				{
					Sockets->DestroySocket(Socket);
				}
				Socket = nullptr;
			}
		}

	private:
		FSocket* Socket = nullptr;
		TArray<uint8> ReceiveBytes;
	};

	FString GetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	double GetNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double Fallback = 0.0)
	{
		double Value = Fallback;
		if (Object.IsValid())
		{
			Object->TryGetNumberField(Field, Value);
		}
		return Value;
	}

	bool GetBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		bool bValue = false;
		if (Object.IsValid())
		{
			Object->TryGetBoolField(Field, bValue);
		}
		return bValue;
	}

	/** Ticks a component the way the editor path does and returns its pose snapshot. */
	void TickComponent(USkeletalMeshComponent& Component)
	{
		Component.TickAnimation(1.0f / 60.0f, false);
		Component.RefreshBoneTransforms();
	}

	FVector BoneTranslation(const USkeletalMeshComponent& Component, FName BoneName)
	{
		const FReferenceSkeleton& Skeleton =
			Component.GetSkeletalMeshAsset()->GetRefSkeleton();
		const int32 BoneIndex = Skeleton.FindBoneIndex(BoneName);
		const TArray<FTransform>& ComponentSpace = Component.GetComponentSpaceTransforms();
		return ComponentSpace.IsValidIndex(BoneIndex)
			? ComponentSpace[BoneIndex].GetTranslation()
			: FVector::ZeroVector;
	}

	/** Asserts one `applied` reply and returns it. */
	bool ExpectApplied(
		FAutomationTestBase& Test,
		const FString& Line,
		const TCHAR* What,
		int64 Session,
		int64 Serial,
		double Time,
		const TArray<TPair<FString, FString>>& ExpectedStatuses)
	{
		TSharedPtr<FJsonObject> Object;
		if (!Test.TestTrue(FString::Printf(TEXT("%s: reply is JSON"), What), ParseJsonLine(Line, Object)))
		{
			return false;
		}
		bool bOk = Test.TestEqual(FString::Printf(TEXT("%s: reply type"), What),
			GetString(Object, TEXT("type")), FString(TEXT("applied")));
		bOk &= Test.TestEqual(FString::Printf(TEXT("%s: session"), What),
			GetNumber(Object, TEXT("session")), static_cast<double>(Session));
		bOk &= Test.TestEqual(FString::Printf(TEXT("%s: serial"), What),
			GetNumber(Object, TEXT("serial")), static_cast<double>(Serial));
		bOk &= Test.TestTrue(FString::Printf(TEXT("%s: time"), What),
			FMath::IsNearlyEqual(GetNumber(Object, TEXT("time"), -1.0), Time, 1e-9));
		const TArray<TSharedPtr<FJsonValue>>* SubjectValues = nullptr;
		if (Test.TestTrue(FString::Printf(TEXT("%s: subjects"), What),
				Object->TryGetArrayField(TEXT("subjects"), SubjectValues) && SubjectValues != nullptr))
		{
			bOk &= Test.TestEqual(FString::Printf(TEXT("%s: subject count"), What),
				SubjectValues->Num(), ExpectedStatuses.Num());
			for (int32 Index = 0; Index < FMath::Min(SubjectValues->Num(), ExpectedStatuses.Num()); ++Index)
			{
				const TSharedPtr<FJsonObject>* SubjectObject = nullptr;
				if ((*SubjectValues)[Index]->TryGetObject(SubjectObject) && SubjectObject != nullptr)
				{
					bOk &= Test.TestEqual(
						FString::Printf(TEXT("%s: subject %d id"), What, Index),
						GetString(*SubjectObject, TEXT("id")), ExpectedStatuses[Index].Key);
					bOk &= Test.TestEqual(
						FString::Printf(TEXT("%s: subject %d status"), What, Index),
						GetString(*SubjectObject, TEXT("status")), ExpectedStatuses[Index].Value);
				}
			}
		}
		return bOk;
	}

	bool ExpectError(
		FAutomationTestBase& Test,
		const FString& Line,
		const TCHAR* What,
		const FString& Code,
		FString* OutDetails = nullptr)
	{
		TSharedPtr<FJsonObject> Object;
		if (!Test.TestTrue(FString::Printf(TEXT("%s: reply is JSON"), What), ParseJsonLine(Line, Object)))
		{
			return false;
		}
		bool bOk = Test.TestEqual(FString::Printf(TEXT("%s: reply type"), What),
			GetString(Object, TEXT("type")), FString(TEXT("error")));
		bOk &= Test.TestEqual(FString::Printf(TEXT("%s: error code"), What),
			GetString(Object, TEXT("code")), Code);
		if (OutDetails != nullptr)
		{
			*OutDetails = GetString(Object, TEXT("details"));
		}
		return bOk;
	}

	/** Max bone delta and root world delta over every recorded frame of one subject. */
	void MaxRecordedDeltas(
		const FMtoUMultiSubjectReceiver& Receiver,
		const FString& SubjectId,
		double& OutMaxBoneDelta,
		double& OutMaxRootWorldDelta)
	{
		OutMaxBoneDelta = 0.0;
		OutMaxRootWorldDelta = 0.0;
		for (const FMtoUFrameRecord& Record : Receiver.GetFrameRecords())
		{
			for (const FMtoUSubjectMeasurement& Measurement : Record.Subjects)
			{
				if (Measurement.Id != SubjectId)
				{
					continue;
				}
				OutMaxRootWorldDelta = FMath::Max(OutMaxRootWorldDelta, Measurement.RootWorldDelta);
				for (const FMtoUBoneMeasurement& Bone : Measurement.Bones)
				{
					OutMaxBoneDelta = FMath::Max(OutMaxBoneDelta, Bone.Delta);
				}
			}
		}
	}

	void CloseSequenceEditor()
	{
		ULevelSequenceEditorBlueprintLibrary::CloseLevelSequence();
	}
}

// ---------------------------------------------------------------------------
// The sample itself: two mesh targets with distinct skeletons, strict identity,
// and the anchor-once preflight that refuses socket or parent motion.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectFixtureTest,
	"MtoUMultiSubjectPrototype.FixtureAndAnchorRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectFixtureTest::RunTest(const FString& Parameters)
{
	FMtoUMultiSubjectFixture Fixture;
	ON_SCOPE_EXIT
	{
		Fixture.Destroy();
	};

	FString Error;
	if (!TestTrue(TEXT("the character-prop fixture builds"),
			FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-prop"), Fixture, Error)))
	{
		AddError(Error);
		return false;
	}

	TestTrue(TEXT("the fixture world exists"), Fixture.World != nullptr);
	TestNotNull(TEXT("the character anchor exists"), Fixture.CharacterAnchor);
	TestNotNull(TEXT("the prop anchor exists"), Fixture.PropAnchor);

	// Exact skeleton identity of each sample rig.
	const FReferenceSkeleton& CharacterSkeleton = Fixture.CharacterMesh->GetRefSkeleton();
	TestEqual(TEXT("full body bone count"), CharacterSkeleton.GetNum(), 4);
	TestEqual(TEXT("Chest belongs to the full body"), CharacterSkeleton.FindBoneIndex(FName(TEXT("Chest"))) != INDEX_NONE, true);
	TestEqual(TEXT("Chest is parented to Spine"),
		CharacterSkeleton.GetBoneName(CharacterSkeleton.GetParentIndex(
			CharacterSkeleton.FindBoneIndex(FName(TEXT("Chest"))))).ToString(), FString(TEXT("Spine")));
	const FReferenceSkeleton& PropSkeleton = Fixture.PropMesh->GetRefSkeleton();
	TestEqual(TEXT("prop bone count"), PropSkeleton.GetNum(), 3);
	TestEqual(TEXT("the prop shares the Root name"),
		PropSkeleton.GetBoneName(0).ToString(), FString(TEXT("Root")));
	TestTrue(TEXT("same-named morphs exist on both meshes"),
		Fixture.CharacterMesh->FindMorphTarget(FName(TEXT("Shared"))) != nullptr
			&& Fixture.PropMesh->FindMorphTarget(FName(TEXT("Shared"))) != nullptr);

	// The declarations the fixture sends are the target skeletons themselves, so
	// they pass the identity check including the bind comparison.
	{
		FMtoUMultiSubjectTarget IdentityTarget;
		FString IdentityError;
		if (TestTrue(TEXT("the character target passes the anchor preflight for identity"),
				IdentityTarget.Initialize(
					{ FMtoUMultiSubjectFixtureBuilder::CharacterId(), Fixture.CharacterAnchor, Fixture.CharacterComponent },
					IdentityError)))
		{
			TestTrue(TEXT("the fixture's own character declaration passes skeleton identity"),
				IdentityTarget.DescribeDeclarationMismatch(Fixture.CharacterDeclaration).IsEmpty());
		}
		FMtoUMultiSubjectTarget PropTarget;
		if (TestTrue(TEXT("the prop target passes the anchor preflight for identity"),
				PropTarget.Initialize(
					{ FMtoUMultiSubjectFixtureBuilder::PropId(), Fixture.PropAnchor, Fixture.PropComponent },
					IdentityError)))
		{
			TestTrue(TEXT("the fixture's own prop declaration passes skeleton identity"),
				PropTarget.DescribeDeclarationMismatch(Fixture.PropDeclaration).IsEmpty());
			// Same skeleton, different rest pose: the character target must
			// refuse it even though names and parents match.
			FMtoUSubjectDeclaration WrongBind = Fixture.CharacterDeclaration;
			WrongBind.Bind[1] = FTransform(FVector(0.0, 0.0, 60.0));
			const FString WrongBindDetails = IdentityTarget.DescribeDeclarationMismatch(WrongBind);
			TestTrue(TEXT("a differently resting rig is refused even with matching names"),
				WrongBindDetails.Contains(TEXT("bind")) && WrongBindDetails.Contains(TEXT("Spine")));
		}
	}

	TestEqual(TEXT("the character declaration root is the Maya DAG path"),
		Fixture.CharacterDeclaration.Root, FString(TEXT("|character:Group|character:Root")));
	TestEqual(TEXT("only the last element of the root path names the root bone"),
		Fixture.CharacterDeclaration.Bones[0].Name.ToString(), FString(TEXT("Root")));
	TestEqual(TEXT("the character declaration bind count"),
		Fixture.CharacterDeclaration.Bind.Num(), CharacterSkeleton.GetNum());
	TestEqual(TEXT("the prop declaration declares the Shared curve"), Fixture.PropDeclaration.Curves.Num(), 1);

	// Anchor-once: the component hangs under its own anchor and nothing else moves it.
	FMtoUMultiSubjectTarget CharacterTarget;
	if (TestTrue(TEXT("the character target passes the anchor preflight"),
			CharacterTarget.Initialize(
				{ FMtoUMultiSubjectFixtureBuilder::CharacterId(), Fixture.CharacterAnchor, Fixture.CharacterComponent },
				Error)))
	{
		TestTrue(TEXT("the anchor transform is off-origin"),
			CharacterTarget.GetAnchorCheck().AnchorTransform.GetLocation().Size() > 100.0);
		TestTrue(TEXT("the anchor is rotated"),
			!CharacterTarget.GetAnchorCheck().AnchorTransform.GetRotation().IsIdentity());
	}
	else
	{
		AddError(Error);
	}

	// A declaration of the full-body skeleton against the arms target is refused.
	{
		FMtoUMultiSubjectFixture ArmsFixture;
		ON_SCOPE_EXIT
		{
			ArmsFixture.Destroy();
		};
		if (TestTrue(TEXT("the character-arms fixture builds"),
				FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-arms"), ArmsFixture, Error)))
		{
			FMtoUMultiSubjectTarget ArmsTarget;
			if (TestTrue(TEXT("the arms target passes the anchor preflight"),
					ArmsTarget.Initialize(
						{ FMtoUMultiSubjectFixtureBuilder::ArmsId(), ArmsFixture.ArmsAnchor, ArmsFixture.ArmsComponent },
						Error)))
			{
				const FString Mismatch =
					ArmsTarget.DescribeDeclarationMismatch(ArmsFixture.CharacterDeclaration);
				TestFalse(TEXT("the full-body declaration does not match the arms target"), Mismatch.IsEmpty());
				TestTrue(TEXT("the refusal names the bone count"),
					Mismatch.Contains(TEXT("bone count differs")));
				TestTrue(TEXT("the arms declaration itself matches"),
					ArmsTarget.DescribeDeclarationMismatch(ArmsFixture.ArmsDeclaration).IsEmpty());
			}
			else
			{
				AddError(Error);
			}
			TestTrue(TEXT("the arms skeleton is a different skeleton asset"),
				ArmsFixture.ArmsSkeleton != Fixture.CharacterSkeleton);
			TestTrue(TEXT("the arms rig keeps both chains"),
				ArmsFixture.ArmsMesh->GetRefSkeleton().FindBoneIndex(FName(TEXT("Forearm_R"))) != INDEX_NONE);
			TestTrue(TEXT("the arms skeleton has no Chest"),
				ArmsFixture.ArmsMesh->GetRefSkeleton().FindBoneIndex(FName(TEXT("Chest"))) == INDEX_NONE);
		}
		else
		{
			AddError(Error);
		}
	}

	// A mesh attached at a socket of another animated mesh is refused: the socket
	// already carries the parent motion, so the Maya world root pose would be
	// applied a second time.
	Fixture.PropComponent->AttachToComponent(
		Fixture.CharacterComponent,
		FAttachmentTransformRules::KeepWorldTransform,
		FName(TEXT("Root")));
	FMtoUMultiSubjectTarget SocketTarget;
	Error.Reset();
	TestFalse(TEXT("a socket-attached target is refused"),
		SocketTarget.Initialize(
			{ FMtoUMultiSubjectFixtureBuilder::PropId(), Fixture.PropAnchor, Fixture.PropComponent },
			Error));
	TestTrue(TEXT("the socket refusal names the socket"),
		Error.Contains(TEXT("socket")) || Error.Contains(TEXT("attached")));
	return true;
}

// ---------------------------------------------------------------------------
// The mandatory path: two subjects applied at one shared time, same-named
// bones and morphs staying apart, single-subject removal, disconnect cleanup.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectCharacterPropTest,
	"MtoUMultiSubjectPrototype.CharacterPropStream",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectCharacterPropTest::RunTest(const FString& Parameters)
{
	FMtoUMultiSubjectFixture Fixture;
	FMtoUMultiSubjectReceiver Receiver;
	FMtoUScriptedPeer Peer;
	ON_SCOPE_EXIT
	{
		Peer.Close();
		Receiver.Stop(TEXT("test finished"));
		Fixture.Destroy();
	};

	FString Error;
	if (!TestTrue(TEXT("the character-prop fixture builds"),
			FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-prop"), Fixture, Error)))
	{
		AddError(Error);
		return false;
	}

	// The two prior drivers the takeover has to restore later.
	const EAnimationMode::Type CharacterPriorMode = Fixture.CharacterComponent->GetAnimationMode();
	const EAnimationMode::Type PropPriorMode = Fixture.PropComponent->GetAnimationMode();
	TestEqual(TEXT("the prop starts on a single-node animation"),
		static_cast<int32>(PropPriorMode), static_cast<int32>(EAnimationMode::AnimationSingleNode));
	TestEqual(TEXT("the prop starts with a non-zero morph"),
		Fixture.PropComponent->GetMorphTarget(FName(TEXT("Shared"))), 0.4f);

	const TArray<FString> SubjectIds = FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(TEXT("character-prop"));
	FMtoUMultiSubjectSessionConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.Scenario = TEXT("character-prop");
	if (!TestTrue(TEXT("a loopback port was reserved"), Config.Port != 0))
	{
		return false;
	}
	FMtoUPreviewWriter Writer;
	Writer.Track = Fixture.CharacterAnimationTrack;
	Writer.TargetId = FMtoUMultiSubjectFixtureBuilder::CharacterId();
	Config.PreviewWriters.Add(Writer);
	if (!TestTrue(TEXT("the receiver starts"),
			Receiver.Start(*Fixture.World, Fixture.MakeRegistrations(SubjectIds), Config, Error)))
	{
		AddError(Error);
		return false;
	}

	if (!TestTrue(TEXT("the scripted peer connects"), Peer.Connect(Config.Port, Error)))
	{
		AddError(Error);
		return false;
	}

	// init -> ready
	if (!TestTrue(TEXT("init is sent"),
			Peer.Send(EncodeInit(Fixture.MakeInit(SubjectIds)), Error)))
	{
		AddError(Error);
		return false;
	}
	FString Line;
	if (!TestTrue(TEXT("ready arrives"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Reply;
	TestTrue(TEXT("ready is JSON"), ParseJsonLine(Line, Reply));
	TestEqual(TEXT("ready type"), GetString(Reply, TEXT("type")), FString(TEXT("ready")));
	const int64 Session = static_cast<int64>(GetNumber(Reply, TEXT("session")));
	TestTrue(TEXT("the session id is positive"), Session > 0);
	TestTrue(TEXT("the receiver reports a live session"), Receiver.HasSession());
	TestTrue(TEXT("the preview took the character track over"),
		Fixture.CharacterAnimationTrack->IsLocalEvalDisabled());
	TestTrue(TEXT("the takeover installed the prototype instance on the character"),
		Fixture.CharacterComponent->GetAnimInstance() != nullptr
			&& Fixture.CharacterComponent->GetAnimInstance()->GetClass()->GetName()
				== FString(TEXT("MtoUMultiSubjectPoseInstance")));

	// Three frames of the shared sample.
	const TArray<TPair<FString, FString>> BothApplied = {
		{ FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("applied") },
		{ FMtoUMultiSubjectFixtureBuilder::PropId(), TEXT("applied") } };
	for (int64 Serial = 1; Serial <= 3; ++Serial)
	{
		const FMtoUFrameMessage Frame = Fixture.MakeFrame(SubjectIds, Serial, static_cast<double>(Serial));
		if (!TestTrue(TEXT("frame is sent"), Peer.Send(EncodeFrame(Frame), Error)))
		{
			AddError(Error);
			return false;
		}
		if (!TestTrue(TEXT("applied arrives"), Peer.ReadLine(Receiver, Line, Error)))
		{
			AddError(Error);
			return false;
		}
		ExpectApplied(*this, Line, TEXT("frame"), Session, Serial, static_cast<double>(Serial), BothApplied);
	}

	TestEqual(TEXT("every frame applied"), Receiver.GetAppliedFrameCount(), 3);
	TestEqual(TEXT("one shared time per frame"), Receiver.GetLastAppliedTime(), 3.0);
	TestEqual(TEXT("the last serial is echoed"), Receiver.GetLastAppliedSerial(), static_cast<int64>(3));

	// The applied pose equals the wire pose on both targets, and the anchor is
	// applied exactly once to the root.
	double CharacterBoneDelta = 0.0;
	double CharacterRootWorldDelta = 0.0;
	double PropBoneDelta = 0.0;
	double PropRootWorldDelta = 0.0;
	MaxRecordedDeltas(Receiver, FMtoUMultiSubjectFixtureBuilder::CharacterId(),
		CharacterBoneDelta, CharacterRootWorldDelta);
	MaxRecordedDeltas(Receiver, FMtoUMultiSubjectFixtureBuilder::PropId(),
		PropBoneDelta, PropRootWorldDelta);
	TestTrue(TEXT("the character pose matches the wire"), CharacterBoneDelta < 1e-3);
	TestTrue(TEXT("the character root is anchored exactly once"), CharacterRootWorldDelta < 1e-3);
	TestTrue(TEXT("the prop pose matches the wire"), PropBoneDelta < 1e-3);
	TestTrue(TEXT("the prop root is anchored exactly once"), PropRootWorldDelta < 1e-3);

	// Same-named bones and morphs stay on their own subject.
	TickComponent(*Fixture.CharacterComponent);
	TickComponent(*Fixture.PropComponent);
	const FVector CharacterRoot = BoneTranslation(*Fixture.CharacterComponent, FName(TEXT("Root")));
	const FVector PropRoot = BoneTranslation(*Fixture.PropComponent, FName(TEXT("Root")));
	TestTrue(TEXT("the two Roots hold different poses"),
		!CharacterRoot.Equals(PropRoot, 1e-3));
	// The third frame is source frame 3, so step 2 of the shared sample.
	TestTrue(TEXT("the character Root follows its own stream"),
		CharacterRoot.Equals(FVector(86.60254, 50.0, 29.0), 1e-3));
	TestTrue(TEXT("the prop Root follows its own stream"),
		PropRoot.Equals(FVector(0.0, 30.0, 13.0), 1e-3));
	const float CharacterShared = Fixture.CharacterComponent->GetMorphTarget(FName(TEXT("Shared")));
	const float PropShared = Fixture.PropComponent->GetMorphTarget(FName(TEXT("Shared")));
	TestTrue(TEXT("the same-named morphs hold different values"),
		!FMath::IsNearlyEqual(CharacterShared, PropShared, 1e-4));
	TestEqual(TEXT("the character drives its own Shared morph"), CharacterShared, 0.27f);
	TestEqual(TEXT("the prop drives its own Shared morph"), PropShared, 0.75f);

	// Single-subject removal: only the prop stops being driven and goes back to
	// the animation state it had, while the character keeps streaming.
	if (!TestTrue(TEXT("remove is sent"),
			Peer.Send(EncodeRemove(FMtoUMultiSubjectFixtureBuilder::PropId()), Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("the removal is acknowledged"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	ExpectApplied(*this, Line, TEXT("remove"), Session, 3, 3.0,
		{ { FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("applied") },
			{ FMtoUMultiSubjectFixtureBuilder::PropId(), TEXT("disabled") } });
	TestEqual(TEXT("the prop is back on its single-node animation"),
		static_cast<int32>(Fixture.PropComponent->GetAnimationMode()),
		static_cast<int32>(EAnimationMode::AnimationSingleNode));
	UAnimSingleNodeInstance* RestoredPropInstance = Fixture.PropComponent->GetSingleNodeInstance();
	TestTrue(TEXT("the prop keeps its own animation after removal"),
		RestoredPropInstance != nullptr
			&& RestoredPropInstance->GetCurrentAsset()
				== static_cast<UAnimationAsset*>(Fixture.PropAnimation));
	TestEqual(TEXT("the prop morph is restored"),
		Fixture.PropComponent->GetMorphTarget(FName(TEXT("Shared"))), 0.4f);
	TestTrue(TEXT("the character instance is still the prototype driver"),
		Fixture.CharacterComponent->GetAnimInstance() != nullptr
			&& Fixture.CharacterComponent->GetAnimInstance()->GetClass()->GetName()
				== FString(TEXT("MtoUMultiSubjectPoseInstance")));
	TestTrue(TEXT("the session is still alive with one subject"), Receiver.HasSession());
	TestTrue(TEXT("the character's own writer stays suppressed while it streams"),
		Fixture.CharacterAnimationTrack->IsLocalEvalDisabled());

	// The remaining subject keeps streaming on its own.
	const FMtoUFrameMessage CharacterOnly =
		Fixture.MakeFrame({ FMtoUMultiSubjectFixtureBuilder::CharacterId() }, 4, 4.0);
	if (!TestTrue(TEXT("the remaining subject's frame is sent"),
			Peer.Send(EncodeFrame(CharacterOnly), Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("the remaining frame is acknowledged"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	ExpectApplied(*this, Line, TEXT("character only"), Session, 4, 4.0,
		{ { FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("applied") } });
	// The remaining subject keeps its own stream, and the removed subject's
	// restore did not touch it.
	TestEqual(TEXT("the remaining subject advanced on its own frame"),
		Fixture.CharacterComponent->GetMorphTarget(FName(TEXT("Shared"))), 0.28f);
	TestEqual(TEXT("the removed subject keeps the animation state it was given back"),
		Fixture.PropComponent->GetMorphTarget(FName(TEXT("Shared"))), 0.4f);

	// A frame that still carries the removed subject is refused, and nothing of
	// it is applied.
	FMtoUFrameMessage Partial = Fixture.MakeFrame(SubjectIds, 5, 5.0);
	if (!TestTrue(TEXT("the partial frame is sent"), Peer.Send(EncodeFrame(Partial), Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("the partial frame is refused"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	ExpectError(*this, Line, TEXT("partial frame"), MtoUMultiSubjectError::FrameSubjects);
	TestEqual(TEXT("no frame was applied after the refusal"), Receiver.GetLastAppliedSerial(), static_cast<int64>(4));

	// Evidence: unchanged asset state, machine-readable measurements.
	{
		const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("character-prop"));
		FString EvidencePath;
		if (TestTrue(TEXT("evidence is written"), Receiver.SaveEvidence(Directory, EvidencePath, Error)))
		{
			TSharedPtr<FJsonObject> Evidence;
			FString EvidenceText;
			TestTrue(TEXT("the evidence reads back"),
				FFileHelper::LoadFileToString(EvidenceText, *EvidencePath)
					&& ParseJsonLine(EvidenceText, Evidence));
			if (Evidence.IsValid())
			{
				TestEqual(TEXT("the evidence reports the applied frames"),
					GetNumber(Evidence, TEXT("applied_frames")), 4.0);
				TestTrue(TEXT("the evidence carries per-frame measurements"),
					Evidence->HasTypedField<EJson::Array>(TEXT("frames")));
				TestTrue(TEXT("the character track stayed suppressed while previewing"),
					GetBool(Evidence, TEXT("preview_writers_suppressed")));
			}
		}
	}

	// Disconnect: the receiver forgets the session and restores the character.
	Peer.Close();
	const double DisconnectDeadline = FPlatformTime::Seconds() + 1.0;
	while (Receiver.HasClient() && FPlatformTime::Seconds() < DisconnectDeadline)
	{
		Receiver.Pump(0.0);
		FPlatformProcess::Sleep(0.001f);
	}
	TestFalse(TEXT("a disconnect ends the session"), Receiver.HasSession());
	TestFalse(TEXT("the client is gone"), Receiver.HasClient());
	TestEqual(TEXT("the disconnect is the recorded reason"),
		Receiver.GetLastSessionEndReason(), FString(TEXT("disconnect")));
	TestEqual(TEXT("the character animation mode is restored"),
		Fixture.CharacterComponent->GetAnimationMode(), CharacterPriorMode);
	TestFalse(TEXT("the character track is enabled again"),
		Fixture.CharacterAnimationTrack->IsLocalEvalDisabled());
	TestTrue(TEXT("the receiver is still listening"), Receiver.IsListening());
	return true;
}

// ---------------------------------------------------------------------------
// Refusals and renegotiation: strict skeleton identity, curve identity, target
// reuse, version and shape errors, framing close semantics.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectNegotiationTest,
	"MtoUMultiSubjectPrototype.NegotiationRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectNegotiationTest::RunTest(const FString& Parameters)
{
	FMtoUMultiSubjectFixture Fixture;
	FMtoUMultiSubjectReceiver Receiver;
	FMtoUScriptedPeer Peer;
	ON_SCOPE_EXIT
	{
		Peer.Close();
		Receiver.Stop(TEXT("test finished"));
		Fixture.Destroy();
	};

	FString Error;
	if (!TestTrue(TEXT("the character-prop fixture builds"),
			FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-prop"), Fixture, Error)))
	{
		AddError(Error);
		return false;
	}
	const TArray<FString> SubjectIds = FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(TEXT("character-prop"));
	const TArray<FMtoUTargetRegistration> Registrations = Fixture.MakeRegistrations(SubjectIds);
	FMtoUMultiSubjectSessionConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.Scenario = TEXT("character-prop-renegotiation");

	// Two ids may never share one target: driving one component from two
	// subjects would let them fight over one pose.
	{
		FMtoUTargetRegistration Duplicate;
		Duplicate.Id = TEXT("prop-copy");
		Duplicate.Anchor = Fixture.CharacterAnchor;
		Duplicate.Component = Fixture.CharacterComponent;
		TArray<FMtoUTargetRegistration> DuplicateRegistrations = Registrations;
		DuplicateRegistrations.Add(Duplicate);
		FMtoUMultiSubjectReceiver DuplicateReceiver;
		FString DuplicateError;
		TestFalse(TEXT("two ids sharing one target are refused at registration"),
			DuplicateReceiver.Start(*Fixture.World, DuplicateRegistrations, Config, DuplicateError));
		TestTrue(TEXT("the refusal names both ids"),
			DuplicateError.Contains(TEXT("prop-copy")) && DuplicateError.Contains(TEXT("character")));
	}

	if (!TestTrue(TEXT("the receiver starts"),
			Receiver.Start(*Fixture.World, Registrations, Config, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("the scripted peer connects"), Peer.Connect(Config.Port, Error)))
	{
		AddError(Error);
		return false;
	}
	FString Line;

	// A frame before any init is refused.
	if (TestTrue(TEXT("a frame before init is sent"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 1, 1.0)), Error))
		&& TestTrue(TEXT("the pre-session frame is answered"), Peer.ReadLine(Receiver, Line, Error)))
	{
		ExpectError(*this, Line, TEXT("frame before init"), MtoUMultiSubjectError::NoSession);
	}

	// A wrong wire version is refused.
	{
		FMtoUInitMessage Init = Fixture.MakeInit(SubjectIds);
		Init.Version = 2;
		if (TestTrue(TEXT("the wrong version is sent"), Peer.Send(EncodeInit(Init), Error))
			&& TestTrue(TEXT("the wrong version is answered"), Peer.ReadLine(Receiver, Line, Error)))
		{
			ExpectError(*this, Line, TEXT("version"), MtoUMultiSubjectError::VersionUnsupported);
		}
	}

	// Three subjects are refused.
	if (TestTrue(TEXT("a three-subject init is sent"),
			Peer.Send(FString::Printf(
				TEXT("{\"type\":\"init\",\"version\":1,\"fps\":30,\"subjects\":[{\"id\":\"character\",\"root\":\"%s\",\"bones\":[{\"name\":\"Root\",\"parent\":-1}],\"curves\":[],\"bind\":[[0,0,0,0,0,0,1,1,1,1]]},{\"id\":\"prop\",\"root\":\"%s\",\"bones\":[{\"name\":\"Root\",\"parent\":-1}],\"curves\":[],\"bind\":[[0,0,0,0,0,0,1,1,1,1]]},{\"id\":\"third\",\"root\":\"|third:Root\",\"bones\":[{\"name\":\"Root\",\"parent\":-1}],\"curves\":[],\"bind\":[[0,0,0,0,0,0,1,1,1,1]]}]}\n"),
				*Fixture.CharacterDeclaration.Root, *Fixture.PropDeclaration.Root), Error))
		&& TestTrue(TEXT("the three-subject init is answered"), Peer.ReadLine(Receiver, Line, Error)))
	{
		ExpectError(*this, Line, TEXT("subjects shape"), MtoUMultiSubjectError::SubjectsShape);
	}

	// An unknown subject id is refused.
	{
		FMtoUInitMessage Init = Fixture.MakeInit(SubjectIds);
		Init.Subjects[1].Id = TEXT("nobody");
		if (TestTrue(TEXT("the unknown id is sent"), Peer.Send(EncodeInit(Init), Error))
			&& TestTrue(TEXT("the unknown id is answered"), Peer.ReadLine(Receiver, Line, Error)))
		{
			FString Details;
			ExpectError(*this, Line, TEXT("unknown id"), MtoUMultiSubjectError::UnknownSubjectId, &Details);
			TestTrue(TEXT("the refusal lists the registered ids"),
				Details.Contains(TEXT("character")) && Details.Contains(TEXT("prop")));
		}
	}

	// A curve that the target mesh does not own is refused with its own code.
	{
		FMtoUInitMessage Init = Fixture.MakeInit(SubjectIds);
		Init.Subjects[1].Curves.Add(FName(TEXT("Missing")));
		if (TestTrue(TEXT("the unknown curve is sent"), Peer.Send(EncodeInit(Init), Error))
			&& TestTrue(TEXT("the unknown curve is answered"), Peer.ReadLine(Receiver, Line, Error)))
		{
			FString Details;
			ExpectError(*this, Line, TEXT("unknown curve"),
				MtoUMultiSubjectError::CurveNotInTarget, &Details);
			TestTrue(TEXT("the refusal names the curve"), Details.Contains(TEXT("Missing")));
		}
	}

	// Same names and parents, deliberately wrong advertised bind: the rig rests
	// differently, so it would deform on the same skeleton.
	{
		FMtoUInitMessage Init = Fixture.MakeInit(SubjectIds);
		Init.Subjects[1].Bind[1] = FTransform(FVector(0.0, 0.0, 60.0));
		if (TestTrue(TEXT("the wrong bind is sent"), Peer.Send(EncodeInit(Init), Error))
			&& TestTrue(TEXT("the wrong bind is answered"), Peer.ReadLine(Receiver, Line, Error)))
		{
			FString Details;
			ExpectError(*this, Line, TEXT("wrong bind"),
				MtoUMultiSubjectError::SkeletonMismatch, &Details);
			TestTrue(TEXT("the refusal names the bone and the bind difference"),
				Details.Contains(TEXT("PropBody")) && Details.Contains(TEXT("bind")));
		}
	}

	// A parent mismatch of the same bone count is refused and names the bone.
	{
		FMtoUInitMessage Init = Fixture.MakeInit(SubjectIds);
		Init.Subjects[1].Bones[2].Parent = 0;
		if (TestTrue(TEXT("the parent mismatch is sent"), Peer.Send(EncodeInit(Init), Error))
			&& TestTrue(TEXT("the parent mismatch is answered"), Peer.ReadLine(Receiver, Line, Error)))
		{
			FString Details;
			ExpectError(*this, Line, TEXT("parent mismatch"),
				MtoUMultiSubjectError::SkeletonMismatch, &Details);
			TestTrue(TEXT("the refusal names the bone and its expected parent"),
				Details.Contains(TEXT("PropTip")) && Details.Contains(TEXT("PropBody")));
		}
	}

	// The mismatch scenario itself: the arms input against the arms target.
	{
		FMtoUMultiSubjectFixture ArmsFixture;
		ON_SCOPE_EXIT
		{
			ArmsFixture.Destroy();
		};
		if (TestTrue(TEXT("the character-arms fixture builds"),
				FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-arms"), ArmsFixture, Error)))
		{
			FMtoUMultiSubjectReceiver ArmsReceiver;
			FMtoUScriptedPeer ArmsPeer;
			ON_SCOPE_EXIT
			{
				ArmsPeer.Close();
				ArmsReceiver.Stop(TEXT("arms test finished"));
			};
			FMtoUMultiSubjectSessionConfig ArmsConfig;
			ArmsConfig.Port = ReserveLoopbackPort();
			ArmsConfig.Scenario = TEXT("character-arms");
			const TArray<FString> ArmsSubjects =
				FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(TEXT("character-arms"));
			if (TestTrue(TEXT("the arms receiver starts"),
					ArmsReceiver.Start(*ArmsFixture.World, ArmsFixture.MakeRegistrations(ArmsSubjects),
						ArmsConfig, Error))
				&& TestTrue(TEXT("the arms peer connects"), ArmsPeer.Connect(ArmsConfig.Port, Error)))
			{
				// init #1: the full-body skeleton against the arms target.
				FString ArmsLine;
				FString Details;
				if (TestTrue(TEXT("the mismatched init is sent"),
						ArmsPeer.Send(EncodeInit(ArmsFixture.MakeMismatchedInit()), Error))
					&& TestTrue(TEXT("the mismatched init is answered"),
						ArmsPeer.ReadLine(ArmsReceiver, ArmsLine, Error)))
				{
					ExpectError(*this, ArmsLine, TEXT("arms mismatch"),
						MtoUMultiSubjectError::SkeletonMismatch, &Details);
					TestTrue(TEXT("the refusal reports the full-body skeleton"),
						Details.Contains(TEXT("bone count differs")));
				}
				// The real Maya sender keeps its own root path while declaring the
				// full-body input; the explicit-root rule refuses that too, with the
				// same stable code.
				FMtoUInitMessage RootMismatch = ArmsFixture.MakeMismatchedInit();
				RootMismatch.Subjects[1].Root = ArmsFixture.ArmsDeclaration.Root;
				FString RootDetails;
				if (TestTrue(TEXT("the mis-rooted init is sent"),
						ArmsPeer.Send(EncodeInit(RootMismatch), Error))
					&& TestTrue(TEXT("the mis-rooted init is answered"),
						ArmsPeer.ReadLine(ArmsReceiver, ArmsLine, Error)))
				{
					ExpectError(*this, ArmsLine, TEXT("arms root mismatch"),
						MtoUMultiSubjectError::SkeletonMismatch, &RootDetails);
					TestTrue(TEXT("the refusal names the declared root"),
						RootDetails.Contains(TEXT("declared root")));
				}
				TestFalse(TEXT("a refused init starts no session"), ArmsReceiver.HasSession());
				TestEqual(TEXT("a refused init drives nothing"),
					static_cast<int32>(ArmsFixture.ArmsComponent->GetAnimationMode()),
					static_cast<int32>(EAnimationMode::AnimationBlueprint));

				// init #2 on the same connection: the matching arms skeleton.
				const FMtoUInitMessage Good = ArmsFixture.MakeInit(ArmsSubjects);
				if (TestTrue(TEXT("the corrected init is sent"),
						ArmsPeer.Send(EncodeInit(Good), Error))
					&& TestTrue(TEXT("the corrected init is answered"),
						ArmsPeer.ReadLine(ArmsReceiver, ArmsLine, Error)))
				{
					TSharedPtr<FJsonObject> Ready;
					TestTrue(TEXT("the corrected reply is JSON"), ParseJsonLine(ArmsLine, Ready));
					TestEqual(TEXT("the corrected init is ready"), GetString(Ready, TEXT("type")),
						FString(TEXT("ready")));
					TestTrue(TEXT("the new negotiation has a session id"),
						static_cast<int64>(GetNumber(Ready, TEXT("session"))) > 0);
				}
				TestTrue(TEXT("the renegotiated session streams"),
					ArmsReceiver.HasSession());

				const FMtoUFrameMessage Frame = ArmsFixture.MakeFrame(ArmsSubjects, 1, 1.0);
				if (TestTrue(TEXT("an arms frame is sent"), ArmsPeer.Send(EncodeFrame(Frame), Error))
					&& TestTrue(TEXT("the arms frame is answered"),
						ArmsPeer.ReadLine(ArmsReceiver, ArmsLine, Error)))
				{
					ExpectApplied(*this, ArmsLine, TEXT("arms frame"),
						ArmsReceiver.GetSessionId(), 1, 1.0,
						{ { FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("applied") },
							{ FMtoUMultiSubjectFixtureBuilder::ArmsId(), TEXT("applied") } });
				}
				double ArmsBoneDelta = 0.0;
				double ArmsRootWorldDelta = 0.0;
				MaxRecordedDeltas(ArmsReceiver, FMtoUMultiSubjectFixtureBuilder::ArmsId(),
					ArmsBoneDelta, ArmsRootWorldDelta);
				TestTrue(TEXT("the arms pose matches the wire"), ArmsBoneDelta < 1e-3);
				TestTrue(TEXT("the arms root is anchored exactly once"), ArmsRootWorldDelta < 1e-3);
				TickComponent(*ArmsFixture.ArmsComponent);
				TestTrue(TEXT("the arms Root follows its own stream"),
					BoneTranslation(*ArmsFixture.ArmsComponent, FName(TEXT("ArmsRoot")))
						.Equals(FVector(0.0, -20.0, 5.0), 1e-3));

				// Framing errors close the connection after one error reply.
				if (TestTrue(TEXT("a malformed line is sent"),
						ArmsPeer.Send(TEXT("{\"type\":\"init\"\n"), Error))
					&& TestTrue(TEXT("the malformed line is answered"),
						ArmsPeer.ReadLine(ArmsReceiver, ArmsLine, Error)))
				{
					ExpectError(*this, ArmsLine, TEXT("malformed"),
						MtoUMultiSubjectError::MalformedJson);
				}
				ArmsReceiver.Pump(0.0);
				TestFalse(TEXT("a malformed line closes the connection"), ArmsReceiver.HasClient());
				TestFalse(TEXT("closing the connection ends the session"),
					ArmsReceiver.HasSession());
			}
			else
			{
				AddError(Error);
			}
		}
		else
		{
			AddError(Error);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Explicit preview takeover against a real Level Sequence: the track stops
// writing while the preview is active, and the pose and animation state return
// when the preview exits.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectSequencerTakeoverTest,
	"MtoUMultiSubjectPrototype.SequencerTakeover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectSequencerTakeoverTest::RunTest(const FString& Parameters)
{
	FMtoUMultiSubjectFixture Fixture;
	FMtoUMultiSubjectReceiver Receiver;
	FMtoUScriptedPeer Peer;
	ON_SCOPE_EXIT
	{
		Peer.Close();
		Receiver.Stop(TEXT("test finished"));
		CloseSequenceEditor();
		Fixture.Destroy();
	};

	FString Error;
	if (!TestTrue(TEXT("the character-prop fixture builds"),
			FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-prop"), Fixture, Error)))
	{
		AddError(Error);
		return false;
	}

	// The sample Level Sequence is opened in the real editor Sequencer.
	if (!TestTrue(TEXT("the sample sequence opens in the Level Sequence editor"),
			ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Fixture.Sequence)))
	{
		return false;
	}
	IAssetEditorInstance* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
		->FindEditorForAsset(Fixture.Sequence, false);
	TSharedPtr<ISequencer> Sequencer = AssetEditor != nullptr
		&& AssetEditor->GetEditorName() == FName(TEXT("LevelSequenceEditor"))
		? static_cast<ILevelSequenceEditorToolkit*>(AssetEditor)->GetSequencer()
		: nullptr;
	if (!TestTrue(TEXT("the open toolkit exposes Sequencer"), Sequencer.IsValid()))
	{
		return false;
	}

	// Establish the sequence-driven pose before preview.
	const FFrameTime SampleTime(Fixture.SequenceStartTick + 10 * 30000 / 30);
	Sequencer->SetGlobalTime(SampleTime);
	Sequencer->ForceEvaluate();
	TickComponent(*Fixture.CharacterComponent);
	FMtoUPoseSnapshot CharacterBefore;
	MtoUCapturePoseSnapshot(*Fixture.CharacterComponent, CharacterBefore);
	const FVector RootBefore = BoneTranslation(*Fixture.CharacterComponent, FName(TEXT("Root")));
	const int32 CharacterPriorMode =
		static_cast<int32>(Fixture.CharacterComponent->GetAnimationMode());
	TestTrue(TEXT("the sequence track moves the character Root"),
		!RootBefore.Equals(FVector::ZeroVector, 1e-3));
	const int32 SectionCountBefore = Fixture.CharacterAnimationTrack->GetAllSections().Num();
	// Baseline for "no asset edit": opening the editor may touch the package, so
	// the takeover must not change the dirty state it found.
	const bool bSequenceDirtyBefore = Fixture.Sequence->GetOutermost()->IsDirty();
	FMtoUPoseSnapshot PropBefore;
	TickComponent(*Fixture.PropComponent);
	MtoUCapturePoseSnapshot(*Fixture.PropComponent, PropBefore);

	// Preview: the receiver takes the track over and drives both targets.
	const TArray<FString> SubjectIds = FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(TEXT("character-prop"));
	FMtoUMultiSubjectSessionConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.Scenario = TEXT("character-prop-sequence");
	FMtoUPreviewWriter Writer;
	Writer.Track = Fixture.CharacterAnimationTrack;
	Writer.TargetId = FMtoUMultiSubjectFixtureBuilder::CharacterId();
	Config.PreviewWriters.Add(Writer);
	Config.EditorSequencer = Sequencer;
	if (!TestTrue(TEXT("the receiver starts"),
			Receiver.Start(*Fixture.World, Fixture.MakeRegistrations(SubjectIds), Config, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("the scripted peer connects"), Peer.Connect(Config.Port, Error)))
	{
		AddError(Error);
		return false;
	}
	FString Line;
	if (!TestTrue(TEXT("init is sent"), Peer.Send(EncodeInit(Fixture.MakeInit(SubjectIds)), Error))
		|| !TestTrue(TEXT("ready arrives"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	const int64 Session = Receiver.GetSessionId();

	TestTrue(TEXT("the track is locally muted while the preview is active"),
		Fixture.CharacterAnimationTrack->IsLocalEvalDisabled());
	TestTrue(TEXT("the preview reports its writers as suppressed"),
		Receiver.GetPreview().IsActive() && Receiver.GetPreview().AreWritersSuppressed());

	// A preview frame with values that cannot come from the animation.
	const FMtoUFrameMessage Frame = Fixture.MakeFrame(SubjectIds, 1, 1.0);
	if (!TestTrue(TEXT("the preview frame is sent"), Peer.Send(EncodeFrame(Frame), Error))
		|| !TestTrue(TEXT("the preview frame is acknowledged"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	ExpectApplied(*this, Line, TEXT("preview frame"), Session, 1, 1.0,
		{ { FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("applied") },
			{ FMtoUMultiSubjectFixtureBuilder::PropId(), TEXT("applied") } });

	// The Sequencer is evaluated again at the same time: with the track muted it
	// must not write, so the streamed pose survives.
	Sequencer->SetGlobalTime(SampleTime);
	Sequencer->ForceEvaluate();
	TickComponent(*Fixture.CharacterComponent);
	FMtoUPoseSnapshot CharacterActive;
	MtoUCapturePoseSnapshot(*Fixture.CharacterComponent, CharacterActive);
	const FVector RootActive = BoneTranslation(*Fixture.CharacterComponent, FName(TEXT("Root")));
	TestTrue(TEXT("the active pose is the streamed pose, not the track pose"),
		RootActive.Equals(FVector(100.0, 0.0, 25.0), 1e-3));
	TestTrue(TEXT("the track pose no longer writes the character"),
		CharacterActive.MaxDelta(CharacterBefore) > 1.0);
	TickComponent(*Fixture.PropComponent);
	TestTrue(TEXT("the prop is driven by the stream, not by its own animation"),
		BoneTranslation(*Fixture.PropComponent, FName(TEXT("Root")))
			.Equals(FVector(0.0, 30.0, 10.0), 1e-3));
	TestTrue(TEXT("the prop's single-node animation is gone while previewing"),
		Fixture.PropComponent->GetSingleNodeInstance() == nullptr);

	// Evidence of the active takeover before the exit.
	{
		const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("character-prop-sequence-active"));
		FString EvidencePath;
		if (TestTrue(TEXT("active evidence is written"),
				Receiver.SaveEvidence(Directory, EvidencePath, Error)))
		{
			TSharedPtr<FJsonObject> Evidence;
			FString EvidenceText;
			TestTrue(TEXT("the active evidence reads back"),
				FFileHelper::LoadFileToString(EvidenceText, *EvidencePath)
					&& ParseJsonLine(EvidenceText, Evidence));
			if (Evidence.IsValid())
			{
				TestTrue(TEXT("the evidence records the active preview"),
					GetBool(Evidence, TEXT("preview_active")));
				TestTrue(TEXT("the evidence records the suppressed writer"),
					GetBool(Evidence, TEXT("preview_writers_suppressed")));
				const TArray<TSharedPtr<FJsonValue>>* Subjects = nullptr;
				if (Evidence->TryGetArrayField(TEXT("subjects"), Subjects) && Subjects != nullptr)
				{
					TestEqual(TEXT("the evidence reports both subjects"), Subjects->Num(), 2);
					for (const TSharedPtr<FJsonValue>& SubjectValue : *Subjects)
					{
						const TSharedPtr<FJsonObject>* SubjectObject = nullptr;
						if (SubjectValue->TryGetObject(SubjectObject) && SubjectObject != nullptr)
						{
							TestTrue(TEXT("the evidence measures no bone drift"),
								GetNumber(*SubjectObject, TEXT("max_bone_delta"), 1.0) < 1e-3);
							TestTrue(TEXT("the evidence measures the root anchored once"),
								GetNumber(*SubjectObject, TEXT("max_root_world_delta"), 1.0) < 1e-3);
						}
					}
				}
			}
		}
	}

	// Removing the subject that owns the Sequencer writer restores that writer
	// and its target while the other subject keeps previewing.
	if (!TestTrue(TEXT("the character removal is sent"),
			Peer.Send(EncodeRemove(FMtoUMultiSubjectFixtureBuilder::CharacterId()), Error))
		|| !TestTrue(TEXT("the character removal is acknowledged"),
			Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	ExpectApplied(*this, Line, TEXT("remove character"), Session, 1, 1.0,
		{ { FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("disabled") },
			{ FMtoUMultiSubjectFixtureBuilder::PropId(), TEXT("applied") } });
	TestFalse(TEXT("the character's writer is enabled again with its subject"),
		Fixture.CharacterAnimationTrack->IsLocalEvalDisabled());
	Sequencer->SetGlobalTime(SampleTime);
	Sequencer->ForceEvaluate();
	TickComponent(*Fixture.CharacterComponent);
	TestTrue(TEXT("the character track drives its target again"),
		BoneTranslation(*Fixture.CharacterComponent, FName(TEXT("Root"))).Equals(RootBefore, 1e-3));
	TestEqual(TEXT("the character is back on its own animation state"),
		static_cast<int32>(Fixture.CharacterComponent->GetAnimationMode()),
		CharacterPriorMode);
	TickComponent(*Fixture.PropComponent);
	TestTrue(TEXT("the other subject keeps its preview pose after the removal"),
		BoneTranslation(*Fixture.PropComponent, FName(TEXT("Root")))
			.Equals(FVector(0.0, 30.0, 10.0), 1e-3));
	TestTrue(TEXT("the other subject is still driven by the prototype instance"),
		Fixture.PropComponent->GetAnimInstance() != nullptr
			&& Fixture.PropComponent->GetAnimInstance()->GetClass()->GetName()
				== FString(TEXT("MtoUMultiSubjectPoseInstance")));

	// The remaining subject keeps streaming under the same session.
	{
		const FMtoUFrameMessage PropOnly =
			Fixture.MakeFrame({ FMtoUMultiSubjectFixtureBuilder::PropId() }, 2, 2.0);
		if (!TestTrue(TEXT("the remaining subject's frame is sent"),
				Peer.Send(EncodeFrame(PropOnly), Error))
			|| !TestTrue(TEXT("the remaining frame is acknowledged"),
				Peer.ReadLine(Receiver, Line, Error)))
		{
			AddError(Error);
			return false;
		}
		ExpectApplied(*this, Line, TEXT("prop only"), Session, 2, 2.0,
			{ { FMtoUMultiSubjectFixtureBuilder::PropId(), TEXT("applied") } });
	}

	// Exit: every writer and every target goes back to its prior state.
	Receiver.Stop(TEXT("preview exit"));
	TestFalse(TEXT("the track is enabled again"),
		Fixture.CharacterAnimationTrack->IsLocalEvalDisabled());
	TestEqual(TEXT("the sequence graph is unchanged"),
		Fixture.CharacterAnimationTrack->GetAllSections().Num(), SectionCountBefore);
	Sequencer->SetGlobalTime(SampleTime);
	Sequencer->ForceEvaluate();
	TickComponent(*Fixture.CharacterComponent);
	FMtoUPoseSnapshot CharacterAfter;
	MtoUCapturePoseSnapshot(*Fixture.CharacterComponent, CharacterAfter);
	TestTrue(TEXT("the prior pose returns after the exit"),
		CharacterAfter.MaxDelta(CharacterBefore) < 1e-3);
	TestTrue(TEXT("the track moves the Root again"),
		BoneTranslation(*Fixture.CharacterComponent, FName(TEXT("Root"))).Equals(RootBefore, 1e-3));
	TestEqual(TEXT("the character animation mode is restored"),
		static_cast<int32>(Fixture.CharacterComponent->GetAnimationMode()),
		CharacterPriorMode);

	TickComponent(*Fixture.PropComponent);
	TestEqual(TEXT("the prop is back on its own animation"),
		static_cast<int32>(Fixture.PropComponent->GetAnimationMode()),
		static_cast<int32>(EAnimationMode::AnimationSingleNode));
	UAnimSingleNodeInstance* RestoredPropInstance = Fixture.PropComponent->GetSingleNodeInstance();
	TestTrue(TEXT("the prop resumes its original animation asset"),
		RestoredPropInstance != nullptr
			&& RestoredPropInstance->GetCurrentAsset()
				== static_cast<UAnimationAsset*>(Fixture.PropAnimation));
	TestEqual(TEXT("the prop morph is restored"),
		Fixture.PropComponent->GetMorphTarget(FName(TEXT("Shared"))), 0.4f);

	// The only flag the takeover touched is the non-serialized local mute, and
	// even that is back: the takeover introduced no asset change, no transaction
	// and no autosave pressure on the sequence package.
	TestEqual(TEXT("the sequence package dirty state is what the takeover found"),
		Fixture.Sequence->GetOutermost()->IsDirty(), bSequenceDirtyBefore);
	TestFalse(TEXT("the track's serialized mute is untouched"),
		Fixture.CharacterAnimationTrack->IsEvalDisabled(/*bInCheckLocal=*/false));
	return true;
}

// ---------------------------------------------------------------------------
// Closing the world ends the session; a cleanup of another world does not.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectWorldCleanupTest,
	"MtoUMultiSubjectPrototype.WorldCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectWorldCleanupTest::RunTest(const FString& Parameters)
{
	FMtoUMultiSubjectFixture Fixture;
	FMtoUMultiSubjectReceiver Receiver;
	FMtoUScriptedPeer Peer;
	ON_SCOPE_EXIT
	{
		Peer.Close();
		Receiver.Stop(TEXT("test finished"));
		Fixture.Destroy();
	};

	FString Error;
	if (!TestTrue(TEXT("the character-prop fixture builds"),
			FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-prop"), Fixture, Error)))
	{
		AddError(Error);
		return false;
	}
	const TArray<FString> SubjectIds = FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(TEXT("character-prop"));
	FMtoUMultiSubjectSessionConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.Scenario = TEXT("character-prop-cleanup");
	if (!TestTrue(TEXT("the receiver starts"),
			Receiver.Start(*Fixture.World, Fixture.MakeRegistrations(SubjectIds), Config, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("the scripted peer connects"), Peer.Connect(Config.Port, Error)))
	{
		AddError(Error);
		return false;
	}
	FString Line;
	if (!TestTrue(TEXT("init is sent"), Peer.Send(EncodeInit(Fixture.MakeInit(SubjectIds)), Error))
		|| !TestTrue(TEXT("ready arrives"), Peer.ReadLine(Receiver, Line, Error))
		|| !TestTrue(TEXT("a frame is sent"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 1, 1.0)), Error))
		|| !TestTrue(TEXT("the frame is acknowledged"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("the session is streaming"), Receiver.HasSession());

	// A cleanup of a different world is ignored.
	UWorld* OtherWorld = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (OtherWorld != nullptr && OtherWorld != Fixture.World)
	{
		FWorldDelegates::OnWorldCleanup.Broadcast(OtherWorld, false, true);
		TestTrue(TEXT("another world's cleanup keeps the session"), Receiver.HasSession());
	}

	// The fixture world itself is destroyed, exactly as closing a map would.
	Fixture.Destroy();
	TestFalse(TEXT("world cleanup ends the session"), Receiver.HasSession());
	TestFalse(TEXT("world cleanup closes the client"), Receiver.HasClient());
	TestFalse(TEXT("world cleanup stops listening"), Receiver.IsListening());
	TestEqual(TEXT("the cleanup reason is recorded"), Receiver.GetLastSessionEndReason(),
		FString(TEXT("world_cleanup")));

	// The receiver is already stopped: a later Stop is harmless.
	Receiver.Stop(TEXT("after cleanup"));
	TestFalse(TEXT("the receiver stays stopped"), Receiver.IsRunning());
	return true;
}

// ---------------------------------------------------------------------------
// The cross-host check: a real mayapy peer drives this receiver. Opt in with
// -MtoUMultiSubjectMayapy= -MtoUMultiSubjectPeer= (-MtoUEvidence= optional).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectMayaPeerTest,
	"MtoUMultiSubjectPrototype.RealMayaPeer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectMayaPeerTest::RunTest(const FString& Parameters)
{
	FString MayapyPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectMayapy="), MayapyPath))
	{
		AddInfo(TEXT("Host check not requested; supply MtoUMultiSubjectMayapy, MtoUMultiSubjectPeer and MtoUEvidence."));
		return true;
	}
	FString PeerPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectPeer="), PeerPath)
		|| !FPaths::FileExists(MayapyPath) || !FPaths::FileExists(PeerPath))
	{
		AddError(TEXT("the host check needs an existing mayapy and peer script"));
		return false;
	}
	int32 Frames = 24;
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectFrames="), Frames);
	FString Scenario = TEXT("character-prop");
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectScenario="), Scenario);

	FMtoUMultiSubjectFixture Fixture;
	FMtoUMultiSubjectReceiver Receiver;
	ON_SCOPE_EXIT
	{
		Receiver.Stop(TEXT("host check finished"));
		Fixture.Destroy();
	};

	FString Error;
	if (!TestTrue(TEXT("the fixture builds"),
			FMtoUMultiSubjectFixtureBuilder::Build(Scenario, Fixture, Error)))
	{
		AddError(Error);
		return false;
	}
	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("maya-peer-") + Scenario);
	IFileManager::Get().MakeDirectory(*Directory, true);
	const TArray<FString> SubjectIds = FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(Scenario);
	FMtoUMultiSubjectSessionConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.Scenario = Scenario;
	FMtoUPreviewWriter Writer;
	Writer.Track = Fixture.CharacterAnimationTrack;
	Writer.TargetId = FMtoUMultiSubjectFixtureBuilder::CharacterId();
	Config.PreviewWriters.Add(Writer);
	if (!TestTrue(TEXT("the receiver starts"),
			Receiver.Start(*Fixture.World, Fixture.MakeRegistrations(SubjectIds), Config, Error)))
	{
		AddError(Error);
		return false;
	}

	FMtoUMultiSubjectPeerRequest Request;
	Request.MayapyPath = MayapyPath;
	Request.PeerScriptPath = PeerPath;
	Request.Scenario = Scenario;
	Request.Port = Receiver.GetBoundPort();
	Request.Frames = Frames;
	Request.EvidencePath = FPaths::Combine(Directory, TEXT("mtou-multi-subject-maya.json"));
	Request.LogPath = FPaths::Combine(Directory, TEXT("mtou-multi-subject-maya.log"));
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectRemove="), Request.RemoveAtFrame);
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectDrop="), Request.DropAfterFrames);

	FMtoUMultiSubjectPeerResult Result;
	const bool bRan = RunMayaMultiSubjectPeer(Request, Receiver, Result, Error);
	TestTrue(TEXT("the Maya peer ran"), bRan);
	if (!bRan)
	{
		AddError(Error);
	}
	TestTrue(TEXT("the Maya peer started"), Result.bStarted);
	TestEqual(TEXT("the Maya peer exited cleanly"), Result.ReturnCode, 0);
	TestTrue(TEXT("the Maya evidence reports success"), Result.bEvidenceOkField && Result.bEvidenceOk);
	TestTrue(TEXT("the Maya evidence covers the frames"), Result.EvidenceFrameCount >= Frames);

	// The receiver's own evidence, cross-read against the Maya one.
	FString EvidencePath;
	TestTrue(TEXT("the receiver evidence is written"),
		Receiver.SaveEvidence(Directory, EvidencePath, Error));
	TSharedPtr<FJsonObject> Evidence;
	FString EvidenceText;
	if (TestTrue(TEXT("the receiver evidence reads back"),
			FFileHelper::LoadFileToString(EvidenceText, *EvidencePath)
				&& ParseJsonLine(EvidenceText, Evidence)))
	{
		TestTrue(TEXT("the receiver applied at least the peer's frames"),
			GetNumber(Evidence, TEXT("applied_frames")) >= Frames);
		TestFalse(TEXT("disconnect leaves no preview"), GetBool(Evidence, TEXT("preview_active")));
		TestFalse(TEXT("disconnect releases the preview writers"),
			GetBool(Evidence, TEXT("preview_writers_suppressed")));
		TestEqual(TEXT("the peer disconnected after the last frame"),
			GetString(Evidence, TEXT("session_end_reason")), FString(TEXT("disconnect")));
		const bool bExpectedRefusal = Scenario == TEXT("character-arms");
		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		if (TestTrue(TEXT("the receiver records negotiation errors"),
				Evidence->TryGetArrayField(TEXT("errors"), Errors) && Errors != nullptr))
		{
			TestEqual(TEXT("only the deliberate arms mismatch is recorded"),
				Errors->Num(), bExpectedRefusal ? 1 : 0);
			if (bExpectedRefusal && Errors->Num() == 1 && (*Errors)[0]->AsObject().IsValid())
			{
				TestEqual(TEXT("the refused full-body input names its error"),
					GetString((*Errors)[0]->AsObject(), TEXT("code")),
					FString(MtoUMultiSubjectError::SkeletonMismatch));
			}
		}
		TestEqual(TEXT("receiver ok reflects only error-free runs"),
			GetBool(Evidence, TEXT("ok")), !bExpectedRefusal);
	}

	// The preview exit restores whichever independent target was selected.
	if (Scenario == TEXT("character-prop"))
	{
		TestEqual(TEXT("the prop animation mode survived the host check"),
			static_cast<int32>(Fixture.PropComponent->GetAnimationMode()),
			static_cast<int32>(EAnimationMode::AnimationSingleNode));
		TestNotNull(TEXT("the prop still owns its single-node instance"),
			Fixture.PropComponent->GetSingleNodeInstance());
	}
	else
	{
		TestEqual(TEXT("the arms animation mode survived the host check"),
			static_cast<int32>(Fixture.ArmsComponent->GetAnimationMode()),
			static_cast<int32>(EAnimationMode::AnimationBlueprint));
		TestNull(TEXT("the arms preview instance is gone"),
			Fixture.ArmsComponent->GetAnimInstance());
	}
	TestFalse(TEXT("the character track is enabled after the host check"),
		Fixture.CharacterAnimationTrack->IsLocalEvalDisabled());
	TestTrue(TEXT("the peer latency was reported"), Result.MaxLatencyMs >= 0.0);
	AddInfo(FString::Printf(TEXT("Maya peer: frames=%d max_latency_ms=%.2f log=%s"),
		Result.EvidenceFrameCount, Result.MaxLatencyMs, *Request.LogPath));
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
