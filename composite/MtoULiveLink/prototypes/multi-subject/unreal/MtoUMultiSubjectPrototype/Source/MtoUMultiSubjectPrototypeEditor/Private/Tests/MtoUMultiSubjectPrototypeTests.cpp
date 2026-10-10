// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
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
#include "LevelEditorViewport.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "MtoUMultiSubjectDriver.h"
#include "MtoUMultiSubjectFixture.h"
#include "MtoUMultiSubjectPeer.h"
#include "MtoUMultiSubjectPreview.h"
#include "MtoUMultiSubjectProtocol.h"
#include "MtoUMultiSubjectReceiver.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "UObject/Package.h"
#include "UnrealClient.h"

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

	/**
	 * Waits, across editor frames, until the requested screenshot has been
	 * written. The image is produced by the renderer on a later frame, so the
	 * check cannot happen inside the test body.
	 */
	DEFINE_LATENT_AUTOMATION_COMMAND_THREE_PARAMETER(
		FMtoUWaitForScreenshot,
		FAutomationTestBase*, Test,
		FString, ScreenshotPath,
		double, DeadlineSeconds);
	bool FMtoUWaitForScreenshot::Update()
	{
		const bool bWritten = FPaths::FileExists(ScreenshotPath)
			&& !FScreenshotRequest::IsScreenshotRequested();
		if (bWritten || FPlatformTime::Seconds() > DeadlineSeconds)
		{
			Test->TestTrue(
				FString::Printf(TEXT("the BaseColor screenshot was written (%s)"), *ScreenshotPath),
				bWritten);
			return true;
		}
		return false;
	}

	/**
	 * Runs one deferred capture step on a later frame. The renderer keeps only
	 * one pending screenshot request, so a second BaseColor capture is planned
	 * only after the first image has been written.
	 */
	DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
		FMtoURunCaptureStep,
		TFunction<void()>, Step);
	bool FMtoURunCaptureStep::Update()
	{
		Step();
		return true;
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

	/** One frame line. The session is explicit: it is the negotiation identity. */
	FString EncodeFrame(const FMtoUFrameMessage& Frame, int64 Session)
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
			TEXT("{\"type\":\"frame\",\"session\":%lld,\"serial\":%lld,\"time\":%.17g,\"subjects\":[%s]}\n"),
			Session, Frame.Serial, Frame.Time, *FString::Join(SubjectLines, TEXT(",")));
	}

	FString EncodeRemove(const FString& Id, int64 Session)
	{
		return FString::Printf(TEXT("{\"type\":\"remove\",\"session\":%lld,\"id\":\"%s\"}\n"),
			Session, *Id);
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

	/**
	 * Largest difference between the component's current component-space pose and
	 * the mesh's reference pose. It answers "is this component back at rest?",
	 * which is what a target without any other driver must be after the exit.
	 */
	double MaxDeltaToReferencePose(const USkeletalMeshComponent& Component)
	{
		const FReferenceSkeleton& Skeleton = Component.GetSkeletalMeshAsset()->GetRefSkeleton();
		const TArray<FTransform>& RefPose = Skeleton.GetRefBonePose();
		TArray<FTransform> Expected;
		Expected.SetNum(RefPose.Num());
		for (int32 BoneIndex = 0; BoneIndex < RefPose.Num(); ++BoneIndex)
		{
			const int32 ParentIndex = Skeleton.GetParentIndex(BoneIndex);
			Expected[BoneIndex] = ParentIndex == INDEX_NONE
				? RefPose[BoneIndex]
				: RefPose[BoneIndex] * Expected[ParentIndex];
		}
		const TArray<FTransform>& ComponentSpace = Component.GetComponentSpaceTransforms();
		double MaxDelta = 0.0;
		for (int32 BoneIndex = 0; BoneIndex < Expected.Num(); ++BoneIndex)
		{
			if (ComponentSpace.IsValidIndex(BoneIndex))
			{
				MaxDelta = FMath::Max(MaxDelta, MtoUSubjectTransformDelta(
					Expected[BoneIndex], ComponentSpace[BoneIndex]));
			}
		}
		return MaxDelta;
	}

	/**
	 * One recorded real Maya sample rebuilt for replay: the driven rows are the
	 * wire rows that sample carried, the rows the target ignores repeat the
	 * declaration's bind (they are never applied), and the curves follow the
	 * recorded values. The lifecycle and the BaseColor hold therefore show a
	 * sampled frame instead of a synthesized pose.
	 */
	bool FrameSubjectFromRecord(
		const FMtoUNegotiatedSubject& Subject,
		const FMtoUFrameRecord& Record,
		FMtoUFrameSubject& OutSubject)
	{
		const FMtoUSubjectMeasurement* Measurement = Record.Subjects.FindByPredicate(
			[&Subject](const FMtoUSubjectMeasurement& Candidate)
			{
				return Candidate.Id == Subject.Id;
			});
		if (Measurement == nullptr)
		{
			return false;
		}
		OutSubject = FMtoUFrameSubject();
		OutSubject.Id = Subject.Id;
		OutSubject.Transforms = Subject.Declaration.Bind;
		OutSubject.Curves.Init(0.0f, Subject.Declaration.Curves.Num());
		int32 Measured = 0;
		for (int32 SourceIndex = 0; SourceIndex < Subject.Map.SourceToTarget.Num(); ++SourceIndex)
		{
			if (Subject.Map.SourceToTarget[SourceIndex] == INDEX_NONE)
			{
				continue;
			}
			if (!Measurement->Bones.IsValidIndex(Measured))
			{
				return false;
			}
			OutSubject.Transforms[SourceIndex] = Measurement->Bones[Measured].ReceivedLocal;
			++Measured;
		}
		for (const TPair<FName, float>& Curve : Measurement->Curves)
		{
			const int32 CurveIndex = Subject.Declaration.Curves.IndexOfByKey(Curve.Key);
			if (Subject.Declaration.Curves.IsValidIndex(CurveIndex))
			{
				OutSubject.Curves[CurveIndex] = Curve.Value;
			}
		}
		return Measured == Measurement->Bones.Num();
	}

	/**
	 * The `-MtoUMultiSubjectTimes=` value of this run, read with the peer's own
	 * parser: the value ends at the next command-line token (so a following
	 * `-abslog=...` cannot leak into it) and it must be a comma-separated list
	 * of finite numbers. Returns false when the argument is absent.
	 */
	bool ReadTimesArgument(TArray<double>& OutTimes)
	{
		FString Value;
		if (!FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
				FCommandLine::Get(), TEXT("-MtoUMultiSubjectTimes="), Value))
		{
			return false;
		}
		return FMtoUMultiSubjectPeerRequest::ParseTimeList(Value, OutTimes);
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
	TestEqual(TEXT("prop bone count"), PropSkeleton.GetNum(), 4);
	TestEqual(TEXT("the prop shares the Root name"),
		PropSkeleton.GetBoneName(0).ToString(), FString(TEXT("Root")));
	TestTrue(TEXT("same-named morphs exist on both meshes"),
		Fixture.CharacterMesh->FindMorphTarget(FName(TEXT("Shared"))) != nullptr
			&& Fixture.PropMesh->FindMorphTarget(FName(TEXT("Shared"))) != nullptr);

	// The declarations the fixture sends are the target skeletons plus their
	// weighted branches, so they map onto the targets.
	{
		FMtoUMultiSubjectTarget IdentityTarget;
		FString IdentityError;
		if (TestTrue(TEXT("the character target passes the anchor preflight for identity"),
				IdentityTarget.Initialize(
					{ FMtoUMultiSubjectFixtureBuilder::CharacterId(), Fixture.CharacterAnchor, Fixture.CharacterComponent },
					IdentityError)))
		{
			FMtoUNegotiationMap IdentityMap;
			TestTrue(TEXT("the fixture's own character declaration negotiates"),
				IdentityTarget.DescribeDeclarationMismatch(Fixture.CharacterDeclaration, IdentityMap).IsEmpty());
			// The mesh skins two vertices to Head, so the required set is the
			// whole Root->Spine->Chest->Head chain.
			TestEqual(TEXT("the required target bones cover the weighted branch"),
				IdentityMap.RequiredTargetBones.Num(), 4);
			TestEqual(TEXT("every required bone is driven by the declaration"),
				IdentityMap.DrivenTargetBones.Num(), 4);
			TestEqual(TEXT("no target bone of the character stays undriven"),
				IdentityMap.UndrivenTargetBones.Num(), 0);
			TestEqual(TEXT("no character source bone is refusable as an export only branch"),
				IdentityMap.SourceOnlyBones, 0);
			// A declaration that drops the weighted leaf must be refused: the
			// remaining declared bones are still an ancestor-closed subset, which
			// is exactly the case the previous rule accepted.
			FMtoUSubjectDeclaration WithoutWeightedLeaf = Fixture.CharacterDeclaration;
			WithoutWeightedLeaf.Bones.Pop();
			WithoutWeightedLeaf.Bind.Pop();
			FMtoUNegotiationMap UnusedMap;
			const FString MissingLeaf = IdentityTarget.DescribeDeclarationMismatch(
				WithoutWeightedLeaf, UnusedMap);
			TestTrue(TEXT("dropping a skin-weighted leaf is refused"), !MissingLeaf.IsEmpty());
			TestTrue(TEXT("the refusal names the weighted bone and its parent"),
				MissingLeaf.Contains(TEXT("Head")) && MissingLeaf.Contains(TEXT("Chest")));
			// An export branch the target does not have is ignored, not refused.
			FMtoUSubjectDeclaration WithExtraBranch = Fixture.CharacterDeclaration;
			WithExtraBranch.Bones.Add({ FName(TEXT("ExportBranch")), 2 });
			WithExtraBranch.Bind.Add(FTransform(FVector(0.0, 0.0, 4.0)));
			WithExtraBranch.Bones.Add({ FName(TEXT("ExportTip")), 4 });
			WithExtraBranch.Bind.Add(FTransform(FVector(0.0, 0.0, 6.0)));
			FMtoUNegotiationMap ExtraMap;
			TestTrue(TEXT("a source-only export branch is accepted"),
				IdentityTarget.DescribeDeclarationMismatch(WithExtraBranch, ExtraMap).IsEmpty());
			TestEqual(TEXT("the export branch is reported as source-only"), ExtraMap.SourceOnlyBones, 2);
			TestEqual(TEXT("the required bones stay covered"), ExtraMap.DrivenTargetBones.Num(), 4);
			// Two declared bones that resolve to one target bone are ambiguous.
			FMtoUSubjectDeclaration Ambiguous = Fixture.CharacterDeclaration;
			Ambiguous.Bones.Add({ FName(TEXT("Spine")), 0 });
			Ambiguous.Bind.Add(Fixture.CharacterDeclaration.Bind[1]);
			FMtoUNegotiationMap AmbiguousMap;
			const FString Ambiguity = IdentityTarget.DescribeDeclarationMismatch(Ambiguous, AmbiguousMap);
			TestTrue(TEXT("two source bones mapping to one target bone are refused"),
				!Ambiguity.IsEmpty() && Ambiguity.Contains(TEXT("ambiguous")));
			// A declaration whose bone sits under another parent cannot drive the
			// target bone of that name, so the required bone is reported missing.
			FMtoUSubjectDeclaration Reparented = Fixture.CharacterDeclaration;
			Reparented.Bones[3].Parent = 0;
			FMtoUNegotiationMap ReparentedMap;
			const FString ReparentRefusal = IdentityTarget.DescribeDeclarationMismatch(
				Reparented, ReparentedMap);
			TestTrue(TEXT("a required bone under the wrong parent is refused"),
				ReparentRefusal.Contains(TEXT("Head")) && ReparentRefusal.Contains(TEXT("Chest")));
			// Same names and parents, different advertised rest pose.
			FMtoUSubjectDeclaration WrongBind = Fixture.CharacterDeclaration;
			WrongBind.Bind[1] = FTransform(FVector(0.0, 0.0, 60.0));
			FMtoUNegotiationMap WrongBindMap;
			const FString WrongBindDetails = IdentityTarget.DescribeDeclarationMismatch(
				WrongBind, WrongBindMap);
			TestTrue(TEXT("a differently resting rig is refused even with matching names"),
				WrongBindDetails.Contains(TEXT("bind")) && WrongBindDetails.Contains(TEXT("Spine")));
		}
		FMtoUMultiSubjectTarget PropTarget;
		if (TestTrue(TEXT("the prop target passes the anchor preflight for identity"),
				PropTarget.Initialize(
					{ FMtoUMultiSubjectFixtureBuilder::PropId(), Fixture.PropAnchor, Fixture.PropComponent },
					IdentityError)))
		{
			FMtoUNegotiationMap PropMap;
			TestTrue(TEXT("the fixture's own prop declaration negotiates"),
				PropTarget.DescribeDeclarationMismatch(Fixture.PropDeclaration, PropMap).IsEmpty());
			// The weighted leaf carries one of the importer's hash suffixes, so a
			// declaration that names it the way Maya does has to map through the
			// rename rule instead of leaving a required bone undriven - and its
			// child, which resolves exactly, only becomes addressable once the
			// renamed parent scope exists.
			FMtoUSubjectDeclaration RenamedLeaf = Fixture.PropDeclaration;
			RenamedLeaf.Bones[2].Name = FName(TEXT("PropTip"));
			FMtoUNegotiationMap RenameMap;
			TestTrue(TEXT("a declared name maps onto the target's import rename"),
				PropTarget.DescribeDeclarationMismatch(RenamedLeaf, RenameMap).IsEmpty());
			TestEqual(TEXT("the rename is reported"), RenameMap.ImportRenames.Num(), 1);
			TestTrue(TEXT("the rename names both sides"),
				RenameMap.ImportRenames.Num() == 1
					&& RenameMap.ImportRenames[0].Contains(TEXT("PropTip -> PropTip_0123")));
			TestEqual(TEXT("the renamed branch drives its whole declared chain"),
				RenameMap.DrivenTargetBones.Num(), 4);
			TestEqual(TEXT("the renamed branch leaves no target bone undriven"),
				RenameMap.UndrivenTargetBones.Num(), 0);
			TestEqual(TEXT("the renamed leaf still covers its required bone"),
				RenameMap.DrivenTargetBones.Num() >= RenameMap.RequiredTargetBones.Num(), true);
			FMtoUSubjectDeclaration PropWrongBind = Fixture.PropDeclaration;
			PropWrongBind.Bind[1] = FTransform(FVector(0.0, 0.0, 60.0));
			FMtoUNegotiationMap PropWrongBindMap;
			const FString PropWrongBindDetails = PropTarget.DescribeDeclarationMismatch(
				PropWrongBind, PropWrongBindMap);
			TestTrue(TEXT("the prop target refuses a differently resting rig"),
				PropWrongBindDetails.Contains(TEXT("bind"))
					&& PropWrongBindDetails.Contains(TEXT("PropBody")));
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
				FMtoUNegotiationMap ArmsMap;
				const FString Mismatch =
					ArmsTarget.DescribeDeclarationMismatch(ArmsFixture.CharacterDeclaration, ArmsMap);
				TestFalse(TEXT("the full-body declaration does not match the arms target"),
					Mismatch.IsEmpty());
				TestTrue(TEXT("the refusal names the required arms bone the declaration misses"),
					Mismatch.Contains(TEXT("ArmsRoot")));
				TestTrue(TEXT("the arms declaration itself matches"),
					ArmsTarget.DescribeDeclarationMismatch(ArmsFixture.ArmsDeclaration, ArmsMap).IsEmpty());
				// Necessary bones, not whole-table equality: the arms mesh skins
				// only UpperArm_L, so a declaration that drops the unweighted
				// Hand_R leaf is accepted and that target bone stays at its
				// reference pose instead of being driven from nowhere.
				FMtoUSubjectDeclaration WithoutLastLeaf = ArmsFixture.ArmsDeclaration;
				WithoutLastLeaf.Bones.Pop();
				WithoutLastLeaf.Bind.Pop();
				FMtoUNegotiationMap WithoutLeafMap;
				TestTrue(TEXT("a declaration without an unweighted leaf is accepted"),
					ArmsTarget.DescribeDeclarationMismatch(WithoutLastLeaf, WithoutLeafMap).IsEmpty());
				TestEqual(TEXT("only the dropped leaf stops being driven"),
					WithoutLeafMap.UndrivenTargetBones.Num(), 1);
				TestTrue(TEXT("the dropped leaf is the reported undriven bone"),
					WithoutLeafMap.UndrivenTargetBones.Contains(FName(TEXT("Hand_R"))));
				TestFalse(TEXT("the dropped leaf is not a required bone"),
					WithoutLeafMap.RequiredTargetBones.Contains(
						ArmsFixture.ArmsMesh->GetRefSkeleton().FindBoneIndex(FName(TEXT("Hand_R")))));
				// The weighted branch, however, is required: dropping it leaves
				// the deforming bone without a driver and must be refused, even
				// though the remaining declaration is still ancestor-closed.
				FMtoUSubjectDeclaration WithoutWeighted = ArmsFixture.ArmsDeclaration;
				{
					const TArray<FMtoUBoneDeclaration> OriginalBones = ArmsFixture.ArmsDeclaration.Bones;
					const TArray<FTransform> OriginalBind = ArmsFixture.ArmsDeclaration.Bind;
					WithoutWeighted.Bones.Reset();
					WithoutWeighted.Bind.Reset();
					TArray<int32> NewIndex;
					NewIndex.Init(INDEX_NONE, OriginalBones.Num());
					for (int32 Index = 0; Index < OriginalBones.Num(); ++Index)
					{
						if (OriginalBones[Index].Name == FName(TEXT("UpperArm_L")))
						{
							continue;
						}
						NewIndex[Index] = WithoutWeighted.Bones.Num();
						WithoutWeighted.Bones.Add(OriginalBones[Index]);
						WithoutWeighted.Bind.Add(OriginalBind[Index]);
					}
					for (int32 Index = 0; Index < OriginalBones.Num(); ++Index)
					{
						const int32 TargetIndex = NewIndex[Index];
						if (TargetIndex == INDEX_NONE)
						{
							continue;
						}
						int32 OldParent = OriginalBones[Index].Parent;
						if (OldParent != INDEX_NONE && NewIndex[OldParent] == INDEX_NONE)
						{
							// The dropped bone's children take its place.
							OldParent = OriginalBones[OldParent].Parent;
						}
						WithoutWeighted.Bones[TargetIndex].Parent =
							OldParent == INDEX_NONE ? INDEX_NONE : NewIndex[OldParent];
					}
				}
				FMtoUNegotiationMap WithoutWeightedMap;
				const FString WeightedRefusal =
					ArmsTarget.DescribeDeclarationMismatch(WithoutWeighted, WithoutWeightedMap);
				TestTrue(TEXT("a declaration that drops a skin-weighted branch is refused"),
					WeightedRefusal.Contains(TEXT("UpperArm_L")));
				// Re-parenting an unweighted bone changes which target bone it
				// addresses, so the target bone of that name becomes undriven and
				// is reported instead of being driven through the wrong parent.
				FMtoUSubjectDeclaration WithoutForearm = ArmsFixture.ArmsDeclaration;
				WithoutForearm.Bones.RemoveAt(2);
				WithoutForearm.Bind.RemoveAt(2);
				WithoutForearm.Bones[2].Parent = 1;
				FMtoUNegotiationMap ReparentedMap;
				TestTrue(TEXT("re-parenting an unweighted branch is accepted"),
					ArmsTarget.DescribeDeclarationMismatch(WithoutForearm, ReparentedMap).IsEmpty());
				TestTrue(TEXT("the target bones that lost their driver are reported"),
					ReparentedMap.UndrivenTargetBones.Contains(FName(TEXT("Forearm_L")))
						&& ReparentedMap.UndrivenTargetBones.Contains(FName(TEXT("Hand_L"))));
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
// A required secondary bone is not merely accepted: driving it moves the
// vertices skinned to it, while a target bone outside the declaration stays at
// its reference pose instead of being driven from nowhere.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectWeightedBoneTest,
	"MtoUMultiSubjectPrototype.WeightedBoneMotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectWeightedBoneTest::RunTest(const FString& Parameters)
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

	FMtoUMultiSubjectTarget Target;
	if (!TestTrue(TEXT("the character target initializes"),
			Target.Initialize(
				{ FMtoUMultiSubjectFixtureBuilder::CharacterId(),
					Fixture.CharacterAnchor, Fixture.CharacterComponent },
				Error)))
	{
		AddError(Error);
		return false;
	}
	FMtoUNegotiationMap Map;
	if (!TestTrue(TEXT("the character declaration negotiates"),
			Target.DescribeDeclarationMismatch(Fixture.CharacterDeclaration, Map).IsEmpty()))
	{
		return false;
	}
	USkeletalMesh* Mesh = Fixture.CharacterMesh;
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	const int32 HeadIndex = Skeleton.FindBoneIndex(FName(TEXT("Head")));
	TestTrue(TEXT("the head bone is a real target bone"), HeadIndex != INDEX_NONE);
	int32 LODIndex = INDEX_NONE;
	int32 VertexIndex = INDEX_NONE;
	TestTrue(TEXT("the head bone skins a target vertex"),
		FMtoUMultiSubjectProtocol::FindWeightedTargetVertex(
			*Mesh, HeadIndex, LODIndex, VertexIndex, Error));

	// A vertex skinned to Head; its CPU-skinned position is the visible result
	// of driving that bone.
	FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!TestNotNull(TEXT("the fixture mesh has render data"), RenderData)
		|| !TestTrue(TEXT("the weighted vertex's LOD exists"),
			RenderData->LODRenderData.IsValidIndex(LODIndex)))
	{
		return false;
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
	// GetSkinnedVertexPosition only reads the weights, but takes the buffer by
	// non-const reference.
	FSkinWeightVertexBuffer& Weights = const_cast<FSkinWeightVertexBuffer&>(LOD.SkinWeightVertexBuffer);

	// The bind pose is the rest frame; the driven frame rotates Head 30 degrees
	// about its local X axis and moves only that branch.
	auto MakeFrame = [&Fixture](bool bRotateHead)
	{
		FMtoUFrameSubject Frame;
		Frame.Id = FMtoUMultiSubjectFixtureBuilder::CharacterId();
		Frame.Transforms = Fixture.CharacterDeclaration.Bind;
		Frame.Curves.Add(0.0f);
		if (bRotateHead)
		{
			const int32 Index = Fixture.CharacterDeclaration.Bones.IndexOfByPredicate(
				[](const FMtoUBoneDeclaration& Bone) { return Bone.Name == FName(TEXT("Head")); });
			if (Frame.Transforms.IsValidIndex(Index))
			{
				Frame.Transforms[Index] = FTransform(
					FQuat(FRotator(30.0, 0.0, 0.0)), FVector(0.0, 0.0, 15.0));
			}
		}
		return Frame;
	};

	if (!TestTrue(TEXT("the target is taken over"), Target.TakeOver(Error)))
	{
		AddError(Error);
		return false;
	}

	// Rest frame: the weighted vertex has not moved.
	FMtoUSubjectMeasurement RestMeasurement;
	if (!TestTrue(TEXT("the rest frame applies"),
			Target.ApplyPose(Fixture.CharacterDeclaration, Map, MakeFrame(false),
				RestMeasurement, Error)))
	{
		AddError(Error);
		return false;
	}
	const FVector3f Before = USkinnedMeshComponent::GetSkinnedVertexPosition(
		Fixture.CharacterComponent, VertexIndex, LOD, Weights);

	// Driven frame: the head bone is driven and its skinned vertex moves.
	FMtoUSubjectMeasurement DrivenMeasurement;
	if (!TestTrue(TEXT("the driven frame applies"),
			Target.ApplyPose(Fixture.CharacterDeclaration, Map, MakeFrame(true),
				DrivenMeasurement, Error)))
	{
		AddError(Error);
		return false;
	}
	const FVector3f After = USkinnedMeshComponent::GetSkinnedVertexPosition(
		Fixture.CharacterComponent, VertexIndex, LOD, Weights);
	TestTrue(TEXT("driving the weighted bone moves its skinned vertex"),
		(After - Before).Size() > 1.0f);
	double DrivenHeadDelta = 0.0;
	for (const FMtoUBoneMeasurement& Bone : DrivenMeasurement.Bones)
	{
		if (Bone.BoneName == FName(TEXT("Head")))
		{
			DrivenHeadDelta = Bone.Delta;
		}
	}
	TestTrue(TEXT("the driven head matches the wire pose"), DrivenHeadDelta < 1e-3);
	// The same frame without the rotation leaves the vertex where it was: the
	// movement above came from the driven bone, not from the takeover itself.
	FMtoUSubjectMeasurement RepeatMeasurement;
	Target.ApplyPose(Fixture.CharacterDeclaration, Map, MakeFrame(false), RepeatMeasurement, Error);
	const FVector3f RestAgain = USkinnedMeshComponent::GetSkinnedVertexPosition(
		Fixture.CharacterComponent, VertexIndex, LOD, Weights);
	TestTrue(TEXT("the unrotated frame returns the vertex to its rest position"),
		(RestAgain - Before).Size() < 0.01f);

	// The arms fixture: a declaration that drops the unweighted Hand_R leaf
	// still drives the weighted branch, and the dropped target bone stays at
	// its reference pose instead of following the stream.
	FMtoUMultiSubjectFixture ArmsFixture;
	ON_SCOPE_EXIT
	{
		ArmsFixture.Destroy();
	};
	if (!TestTrue(TEXT("the character-arms fixture builds"),
			FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-arms"), ArmsFixture, Error)))
	{
		AddError(Error);
		return false;
	}
	FMtoUMultiSubjectTarget ArmsTarget;
	if (!TestTrue(TEXT("the arms target initializes"),
			ArmsTarget.Initialize(
				{ FMtoUMultiSubjectFixtureBuilder::ArmsId(),
					ArmsFixture.ArmsAnchor, ArmsFixture.ArmsComponent },
				Error)))
	{
		AddError(Error);
		return false;
	}
	FMtoUSubjectDeclaration ArmsDeclaration = ArmsFixture.ArmsDeclaration;
	ArmsDeclaration.Bones.Pop();
	ArmsDeclaration.Bind.Pop();
	FMtoUNegotiationMap ArmsMap;
	if (!TestTrue(TEXT("the arms declaration without Hand_R negotiates"),
			ArmsTarget.DescribeDeclarationMismatch(ArmsDeclaration, ArmsMap).IsEmpty()))
	{
		return false;
	}
	if (!TestTrue(TEXT("the arms target is taken over"), ArmsTarget.TakeOver(Error)))
	{
		AddError(Error);
		return false;
	}
	FMtoUFrameSubject ArmsFrame;
	ArmsFrame.Id = FMtoUMultiSubjectFixtureBuilder::ArmsId();
	ArmsFrame.Transforms = ArmsDeclaration.Bind;
	ArmsFrame.Curves.Add(0.0f);
	const int32 UpperArmIndex = ArmsDeclaration.Bones.IndexOfByPredicate(
		[](const FMtoUBoneDeclaration& Bone) { return Bone.Name == FName(TEXT("UpperArm_L")); });
	TestTrue(TEXT("the arms declaration drives UpperArm_L"), UpperArmIndex != INDEX_NONE);
	ArmsFrame.Transforms[UpperArmIndex] = FTransform(
		FQuat(FRotator(0.0, 0.0, 40.0)), FVector(25.0, 0.0, 30.0));
	FMtoUSubjectMeasurement ArmsMeasurement;
	if (!TestTrue(TEXT("the arms frame applies"),
			ArmsTarget.ApplyPose(ArmsDeclaration, ArmsMap, ArmsFrame, ArmsMeasurement, Error)))
	{
		AddError(Error);
		return false;
	}
	const FReferenceSkeleton& ArmsSkeleton = ArmsFixture.ArmsMesh->GetRefSkeleton();
	const int32 HandRIndex = ArmsSkeleton.FindBoneIndex(FName(TEXT("Hand_R")));
	TestTrue(TEXT("the undriven hand is a real target bone"), HandRIndex != INDEX_NONE);
	const TArray<FTransform>& ArmsComponentSpace =
		ArmsFixture.ArmsComponent->GetComponentSpaceTransforms();
	TArray<FTransform> ArmsReference;
	const TArray<FTransform>& ArmsReferencePose = ArmsSkeleton.GetRefBonePose();
	ArmsReference.SetNum(ArmsReferencePose.Num());
	for (int32 BoneIndex = 0; BoneIndex < ArmsReferencePose.Num(); ++BoneIndex)
	{
		const int32 ParentIndex = ArmsSkeleton.GetParentIndex(BoneIndex);
		ArmsReference[BoneIndex] = ParentIndex == INDEX_NONE
			? ArmsReferencePose[BoneIndex]
			: ArmsReferencePose[BoneIndex] * ArmsReference[ParentIndex];
	}
	TestTrue(TEXT("the undriven target bone stays at its reference pose"),
		ArmsComponentSpace.IsValidIndex(HandRIndex)
			&& MtoUSubjectTransformDelta(ArmsReference[HandRIndex], ArmsComponentSpace[HandRIndex]) < 1e-3);
	for (const FMtoUBoneMeasurement& Bone : ArmsMeasurement.Bones)
	{
		TestFalse(TEXT("an ignored declaration bone is not measured"),
			Bone.BoneName == FName(TEXT("Hand_R")));
	}
	TestTrue(TEXT("only mapped bones are measured"),
		ArmsMeasurement.Bones.Num() == ArmsMap.DrivenTargetBones.Num());
	return true;
}


// ---------------------------------------------------------------------------
// Bind/frame projection: a rig whose root joint carries an import convention
// (the whole rest pose rotated by one constant frame) drives the target to the
// same pose as the convention-free rig instead of tilting it.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectBindProjectionTest,
	"MtoUMultiSubjectPrototype.BindFrameProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectBindProjectionTest::RunTest(const FString& Parameters)
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

	// The rotated-root convention: the same rig whose every component pose is
	// left-multiplied by one constant frame. On the wire that means the root's
	// local carries the frame and every other local is conjugated by it -
	// exactly what an import that keeps a rotated skeleton root looks like.
	const FTransform RootConvention(FQuat(FVector(1.0, 0.0, 0.0), PI / 2.0));
	auto RotateConvention = [&RootConvention](int32 BoneIndex, const FTransform& Local)
	{
		return BoneIndex == 0
			? RootConvention * Local
			: RootConvention * Local * RootConvention.Inverse();
	};
	FMtoUSubjectDeclaration Rotated = Fixture.CharacterDeclaration;
	for (int32 BoneIndex = 0; BoneIndex < Rotated.Bind.Num(); ++BoneIndex)
	{
		Rotated.Bind[BoneIndex] = RotateConvention(BoneIndex, Rotated.Bind[BoneIndex]);
	}

	FMtoUMultiSubjectTarget Target;
	if (!TestTrue(TEXT("the character target initializes"),
			Target.Initialize(
				{ FMtoUMultiSubjectFixtureBuilder::CharacterId(),
					Fixture.CharacterAnchor, Fixture.CharacterComponent },
				Error)))
	{
		AddError(Error);
		return false;
	}
	FMtoUNegotiationMap PlainMap;
	TestTrue(TEXT("the plain declaration negotiates"),
		Target.DescribeDeclarationMismatch(Fixture.CharacterDeclaration, PlainMap).IsEmpty());
	FMtoUNegotiationMap RotatedMap;
	TestTrue(TEXT("the rotated-root convention negotiates"),
		Target.DescribeDeclarationMismatch(Rotated, RotatedMap).IsEmpty());
	TestTrue(TEXT("the rotated convention rests within one root frame"),
		RotatedMap.MaxRestRotationDegrees < 1e-3 && RotatedMap.MaxRestTranslationCm < 1e-3);

	// One shared sample pose; the rotated declaration carries the same motion in
	// its own frame.
	const FMtoUFrameMessage Frame = Fixture.MakeFrame(
		{ FMtoUMultiSubjectFixtureBuilder::CharacterId() }, 1, 2.0);
	const FMtoUFrameSubject& FrameSubject = Frame.Subjects[0];
	FMtoUFrameSubject RotatedFrameSubject;
	RotatedFrameSubject.Id = FrameSubject.Id;
	RotatedFrameSubject.Curves = FrameSubject.Curves;
	for (int32 BoneIndex = 0; BoneIndex < FrameSubject.Transforms.Num(); ++BoneIndex)
	{
		RotatedFrameSubject.Transforms.Add(
			RotateConvention(BoneIndex, FrameSubject.Transforms[BoneIndex]));
	}

	const FReferenceSkeleton& Skeleton = Fixture.CharacterMesh->GetRefSkeleton();
	auto CaptureComponentPose = [&Fixture, &Skeleton](TArray<FTransform>& OutPose)
	{
		OutPose = Fixture.CharacterComponent->GetComponentSpaceTransforms();
		OutPose.SetNum(Skeleton.GetNum());
	};

	FMtoUSubjectMeasurement PlainMeasurement;
	TestTrue(TEXT("the target is taken over"), Target.TakeOver(Error));
	TestTrue(TEXT("the plain frame applies"),
		Target.ApplyPose(Fixture.CharacterDeclaration, PlainMap, FrameSubject,
			PlainMeasurement, Error));
	TArray<FTransform> PlainPose;
	CaptureComponentPose(PlainPose);
	Target.Restore(Error);

	FMtoUSubjectMeasurement RotatedMeasurement;
	TestTrue(TEXT("the target is taken over again"), Target.TakeOver(Error));
	TestTrue(TEXT("the rotated-convention frame applies"),
		Target.ApplyPose(Rotated, RotatedMap, RotatedFrameSubject, RotatedMeasurement, Error));
	TArray<FTransform> RotatedPose;
	CaptureComponentPose(RotatedPose);

	double WorstDelta = 0.0;
	for (int32 BoneIndex = 0; BoneIndex < PlainPose.Num() && BoneIndex < RotatedPose.Num(); ++BoneIndex)
	{
		WorstDelta = FMath::Max(WorstDelta,
			MtoUSubjectTransformDelta(PlainPose[BoneIndex], RotatedPose[BoneIndex]));
	}
	AddInfo(FString::Printf(TEXT("rotated-root projection delta: %.9f"), WorstDelta));
	TestTrue(TEXT("the rotated-root rig drives the target to the same pose"),
		WorstDelta < 1e-3);
	double RotatedBoneDelta = 0.0;
	for (const FMtoUBoneMeasurement& Bone : RotatedMeasurement.Bones)
	{
		RotatedBoneDelta = FMath::Max(RotatedBoneDelta, Bone.Delta);
	}
	TestTrue(TEXT("the projected pose matches the wire's motion"), RotatedBoneDelta < 1e-3);
	TestTrue(TEXT("the rotated root is still anchored exactly once"),
		RotatedMeasurement.RootWorldDelta < 1e-3);
	Target.Restore(Error);
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
		if (!TestTrue(TEXT("frame is sent"), Peer.Send(EncodeFrame(Frame, Session), Error)))
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
			Peer.Send(EncodeRemove(FMtoUMultiSubjectFixtureBuilder::PropId(), Session), Error)))
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
			Peer.Send(EncodeFrame(CharacterOnly, Session), Error)))
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
	if (!TestTrue(TEXT("the partial frame is sent"), Peer.Send(EncodeFrame(Partial, Session), Error)))
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
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 1, 1.0), 0), Error))
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

	// A curve the target mesh does not own is an export extra: it is ignored,
	// reported as source-only and does not block the negotiation.
	{
		FMtoUInitMessage Init = Fixture.MakeInit(SubjectIds);
		Init.Subjects[1].Curves.Add(FName(TEXT("Missing")));
		if (TestTrue(TEXT("the extra curve is sent"), Peer.Send(EncodeInit(Init), Error))
			&& TestTrue(TEXT("the extra curve is answered"), Peer.ReadLine(Receiver, Line, Error)))
		{
			TSharedPtr<FJsonObject> Ready;
			TestTrue(TEXT("the reply is JSON"), ParseJsonLine(Line, Ready));
			TestEqual(TEXT("a declaration with an extra curve is ready"),
				GetString(Ready, TEXT("type")), FString(TEXT("ready")));
			bool bReported = false;
			for (const FMtoUSessionSubject& Subject : Receiver.GetSubjects())
			{
				if (Subject.Id == FMtoUMultiSubjectFixtureBuilder::PropId())
				{
					bReported = Subject.Map.SourceOnlyCurves.Contains(FName(TEXT("Missing")));
				}
			}
			TestTrue(TEXT("the ignored curve is reported as source-only"), bReported);
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
					TestTrue(TEXT("the refusal reports the required arms bone the full-body input misses"),
						Details.Contains(TEXT("ArmsRoot")));
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
				if (TestTrue(TEXT("an arms frame is sent"),
						ArmsPeer.Send(EncodeFrame(Frame, ArmsReceiver.GetSessionId()), Error))
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
	if (!TestTrue(TEXT("the preview frame is sent"), Peer.Send(EncodeFrame(Frame, Session), Error))
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
			Peer.Send(EncodeRemove(FMtoUMultiSubjectFixtureBuilder::CharacterId(), Session), Error))
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
				Peer.Send(EncodeFrame(PropOnly, Session), Error))
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
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 1, 1.0), 0), Error))
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
// Session identity and evaluated time: a frame carries the negotiation it was
// sampled for, the serial identifies the evaluation, and the source time may
// move backwards or repeat when the same frame is edited again.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectSessionIdentityTest,
	"MtoUMultiSubjectPrototype.SessionAndTimeIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectSessionIdentityTest::RunTest(const FString& Parameters)
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
	Config.Scenario = TEXT("character-prop-identity");
	if (!TestTrue(TEXT("a loopback port was reserved"), Config.Port != 0))
	{
		return false;
	}
	if (!TestTrue(TEXT("the receiver starts"),
			Receiver.Start(*Fixture.World, Fixture.MakeRegistrations(SubjectIds), Config, Error))
		|| !TestTrue(TEXT("the scripted peer connects"), Peer.Connect(Config.Port, Error)))
	{
		AddError(Error);
		return false;
	}

	FString Line;
	if (!TestTrue(TEXT("the first init is sent"),
			Peer.Send(EncodeInit(Fixture.MakeInit(SubjectIds)), Error))
		|| !TestTrue(TEXT("the first session is ready"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Ready;
	TestTrue(TEXT("ready is JSON"), ParseJsonLine(Line, Ready));
	const int64 FirstSession = static_cast<int64>(GetNumber(Ready, TEXT("session")));

	const TArray<TPair<FString, FString>> BothApplied = {
		{ FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("applied") },
		{ FMtoUMultiSubjectFixtureBuilder::PropId(), TEXT("applied") } };
	if (!TestTrue(TEXT("a first-session frame is sent"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 1, 1.0), FirstSession), Error))
		|| !TestTrue(TEXT("the first frame is acknowledged"),
			Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	ExpectApplied(*this, Line, TEXT("first session frame"), FirstSession, 1, 1.0, BothApplied);

	// Renegotiating on the same connection replaces the session; the pose that
	// was sampled for the previous one must no longer apply.
	if (!TestTrue(TEXT("a second init is sent"),
			Peer.Send(EncodeInit(Fixture.MakeInit(SubjectIds)), Error))
		|| !TestTrue(TEXT("the second session is ready"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("ready is JSON again"), ParseJsonLine(Line, Ready));
	const int64 SecondSession = static_cast<int64>(GetNumber(Ready, TEXT("session")));
	TestTrue(TEXT("the renegotiation handed out a larger session"),
		SecondSession > FirstSession);

	// A renegotiation restarts the session's own counters, so the invariant is
	// that a stale message changes none of them.
	const int32 AppliedBeforeStale = Receiver.GetAppliedFrameCount();
	const int64 SerialBeforeStale = Receiver.GetLastAppliedSerial();
	if (TestTrue(TEXT("a stale-session frame is sent"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 2, 2.0), FirstSession), Error))
		&& TestTrue(TEXT("the stale frame is answered"), Peer.ReadLine(Receiver, Line, Error)))
	{
		FString Details;
		ExpectError(*this, Line, TEXT("stale frame"), MtoUMultiSubjectError::SessionMismatch, &Details);
		TestTrue(TEXT("the refusal names both sessions"),
			Details.Contains(FString::FromInt(FirstSession))
				&& Details.Contains(FString::FromInt(SecondSession)));
	}
	TestEqual(TEXT("a stale frame applies nothing"),
		Receiver.GetAppliedFrameCount(), AppliedBeforeStale);
	TestEqual(TEXT("a stale frame does not advance the serial"),
		Receiver.GetLastAppliedSerial(), SerialBeforeStale);

	if (TestTrue(TEXT("a stale-session remove is sent"),
			Peer.Send(EncodeRemove(FMtoUMultiSubjectFixtureBuilder::PropId(), FirstSession), Error))
		&& TestTrue(TEXT("the stale remove is answered"), Peer.ReadLine(Receiver, Line, Error)))
	{
		ExpectError(*this, Line, TEXT("stale remove"), MtoUMultiSubjectError::SessionMismatch);
	}
	if (TestTrue(TEXT("a current-session frame still carries both subjects"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 2, 2.0), SecondSession), Error))
		&& TestTrue(TEXT("the current frame is acknowledged"),
			Peer.ReadLine(Receiver, Line, Error)))
	{
		ExpectApplied(*this, Line, TEXT("current session frame"), SecondSession, 2, 2.0, BothApplied);
	}

	// The serial is the evaluation identity; the source time may go backwards or
	// repeat when the same frame is edited again.
	struct FPlannedFrame
	{
		int64 Serial;
		double Time;
		const TCHAR* Direction;
	};
	const FPlannedFrame Planned[] = {
		{ 3, 4.0, TEXT("forward") },
		{ 4, 2.0, TEXT("backward") },
		{ 5, 2.0, TEXT("hold") },
	};
	for (const FPlannedFrame& Entry : Planned)
	{
		if (!TestTrue(FString::Printf(TEXT("frame serial %lld is sent"), Entry.Serial),
				Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, Entry.Serial, Entry.Time),
					SecondSession), Error))
			|| !TestTrue(FString::Printf(TEXT("frame serial %lld is acknowledged"), Entry.Serial),
				Peer.ReadLine(Receiver, Line, Error)))
		{
			AddError(Error);
			return false;
		}
		ExpectApplied(*this, Line,
			*FString::Printf(TEXT("serial %lld at time %.1f"), Entry.Serial, Entry.Time),
			SecondSession, Entry.Serial, Entry.Time, BothApplied);
		TestEqual(FString::Printf(TEXT("serial %lld is recorded as %s"),
			Entry.Serial, Entry.Direction),
			Receiver.GetLastTimeDirection(), FString(Entry.Direction));
	}
	// The serial still has to increase inside the session, whatever the time did.
	const int32 AppliedBeforeRepeat = Receiver.GetAppliedFrameCount();
	if (TestTrue(TEXT("a repeated serial is sent"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 5, 5.0), SecondSession), Error))
		&& TestTrue(TEXT("the repeated serial is answered"), Peer.ReadLine(Receiver, Line, Error)))
	{
		ExpectError(*this, Line, TEXT("repeated serial"), MtoUMultiSubjectError::FrameOrder);
	}
	TestEqual(TEXT("a refused serial applies nothing"),
		Receiver.GetAppliedFrameCount(), AppliedBeforeRepeat);

	// A frame whose session the receiver never handed out is refused as well.
	if (TestTrue(TEXT("an unknown session is sent"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 6, 3.0), SecondSession + 41), Error))
		&& TestTrue(TEXT("the unknown session is answered"), Peer.ReadLine(Receiver, Line, Error)))
	{
		ExpectError(*this, Line, TEXT("unknown session"), MtoUMultiSubjectError::SessionMismatch);
	}

	// Evidence: every applied frame is attributed to the session it belongs to.
	{
		const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("session-identity"));
		FString EvidencePath;
		if (TestTrue(TEXT("evidence is written"), Receiver.SaveEvidence(Directory, EvidencePath, Error)))
		{
			TSharedPtr<FJsonObject> Evidence;
			FString EvidenceText;
			if (TestTrue(TEXT("the evidence reads back"),
					FFileHelper::LoadFileToString(EvidenceText, *EvidencePath)
						&& ParseJsonLine(EvidenceText, Evidence)))
			{
				const int32 AppliedThisSession = Receiver.GetAppliedFrameCount();
				TestEqual(TEXT("the evidence counts the applied frames"),
					GetNumber(Evidence, TEXT("applied_frames")),
					static_cast<double>(AppliedThisSession));
				TestEqual(TEXT("the evidence reports the last direction"),
					GetString(Evidence, TEXT("last_time_direction")), FString(TEXT("hold")));
				const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
				if (TestTrue(TEXT("the evidence carries the frames"),
						Evidence->TryGetArrayField(TEXT("frames"), Frames) && Frames != nullptr))
				{
					bool bAllCurrent = Frames->Num() == AppliedThisSession;
					for (const TSharedPtr<FJsonValue>& Value : *Frames)
					{
						bAllCurrent &= static_cast<int64>(GetNumber(Value->AsObject(), TEXT("session")))
							== SecondSession;
					}
					TestTrue(TEXT("every applied frame names the current session"), bAllCurrent);
				}
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Drive ownership: what drove a target before, what the preview muted, and the
// pose a target is left with when nothing else ever drove it.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectDriverExitTest,
	"MtoUMultiSubjectPrototype.DriverExitOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectDriverExitTest::RunTest(const FString& Parameters)
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
	// The arms component carries no animation driver at all: it is the case the
	// exit has to handle, because nothing will write the pose back for it.
	if (!TestTrue(TEXT("the character-arms fixture builds"),
			FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-arms"), Fixture, Error)))
	{
		AddError(Error);
		return false;
	}
	TestNull(TEXT("the arms component starts without an animation driver"),
		Fixture.ArmsComponent->AnimClass.Get());

	const TArray<FString> SubjectIds = FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(TEXT("character-arms"));
	FMtoUMultiSubjectSessionConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.Scenario = TEXT("character-arms-exit");
	if (!TestTrue(TEXT("the receiver starts"),
			Receiver.Start(*Fixture.World, Fixture.MakeRegistrations(SubjectIds), Config, Error))
		|| !TestTrue(TEXT("the scripted peer connects"), Peer.Connect(Config.Port, Error)))
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
	if (!TestTrue(TEXT("a preview frame is sent"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 1, 1.0), Session), Error))
		|| !TestTrue(TEXT("the preview frame is acknowledged"),
			Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	TickComponent(*Fixture.ArmsComponent);
	TestTrue(TEXT("the streamed pose is on the arms target"),
		MaxDeltaToReferencePose(*Fixture.ArmsComponent) > 1.0);
	const TArray<FString> WhileDriving = Receiver.DescribeDriveOwnership();
	TestEqual(TEXT("ownership reports every registered target"),
		WhileDriving.Num(), SubjectIds.Num());
	TestTrue(TEXT("ownership names the undriven target's missing driver"),
		[&WhileDriving]()
		{
			for (const FString& Entry : WhileDriving)
			{
				if (Entry.StartsWith(FMtoUMultiSubjectFixtureBuilder::ArmsId()))
				{
					return Entry.Contains(TEXT("none"))
						&& Entry.Contains(TEXT("preview_active"));
				}
			}
			return false;
		}());

	// Exit: the undriven target must not keep the preview's last pose.
	Receiver.Stop(TEXT("preview exit"));
	TickComponent(*Fixture.ArmsComponent);
	TestNull(TEXT("the prototype pose instance is gone"),
		Fixture.ArmsComponent->GetAnimInstance());
	TestTrue(TEXT("the undriven target is back at its reference pose"),
		MaxDeltaToReferencePose(*Fixture.ArmsComponent) < 1e-3);
	TestTrue(TEXT("the ownership report says the target left at rest"),
		[&Receiver]()
		{
			for (const FString& Entry : Receiver.DescribeDriveOwnership())
			{
				if (Entry.StartsWith(FMtoUMultiSubjectFixtureBuilder::ArmsId()))
				{
					return Entry.Contains(TEXT("reference_pose"));
				}
			}
			return false;
		}());

	{
		const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("driver-exit"));
		FString EvidencePath;
		if (TestTrue(TEXT("evidence is written"), Receiver.SaveEvidence(Directory, EvidencePath, Error)))
		{
			TSharedPtr<FJsonObject> Evidence;
			FString EvidenceText;
			if (TestTrue(TEXT("the evidence reads back"),
					FFileHelper::LoadFileToString(EvidenceText, *EvidencePath)
						&& ParseJsonLine(EvidenceText, Evidence)))
			{
				const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
				if (TestTrue(TEXT("the evidence lists the targets"),
						Evidence->TryGetArrayField(TEXT("targets"), Targets) && Targets != nullptr))
				{
					bool bFoundUndriven = false;
					for (const TSharedPtr<FJsonValue>& Value : *Targets)
					{
						const TSharedPtr<FJsonObject> Target = Value->AsObject();
						if (Target.IsValid()
							&& GetString(Target, TEXT("id")) == FMtoUMultiSubjectFixtureBuilder::ArmsId())
						{
							bFoundUndriven = !GetBool(Target, TEXT("had_animation_driver"))
								&& GetBool(Target, TEXT("restored_to_reference_pose"));
						}
					}
					TestTrue(TEXT("the evidence records the undriven target's exit"), bFoundUndriven);
				}
				TestTrue(TEXT("the evidence carries the ownership report"),
					Evidence->HasTypedField<EJson::Array>(TEXT("drive_ownership")));
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Ownership across renegotiation: the report describes who drives a target
// *now*. An exit result from an earlier session must never appear next to an
// active preview, and a restored writer must not look like a live mute.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectOwnershipRenegotiationTest,
	"MtoUMultiSubjectPrototype.OwnershipRenegotiation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectOwnershipRenegotiationTest::RunTest(const FString& Parameters)
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
	const TArray<FString> SubjectIds =
		FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(TEXT("character-prop"));
	const FString CharacterId = FMtoUMultiSubjectFixtureBuilder::CharacterId();
	const FString PropId = FMtoUMultiSubjectFixtureBuilder::PropId();

	FMtoUMultiSubjectSessionConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.Scenario = TEXT("character-prop-ownership");
	FMtoUPreviewWriter Writer;
	Writer.Track = Fixture.CharacterAnimationTrack;
	Writer.TargetId = CharacterId;
	Config.PreviewWriters.Add(Writer);
	if (!TestTrue(TEXT("a loopback port was reserved"), Config.Port != 0)
		|| !TestTrue(TEXT("the receiver starts"),
			Receiver.Start(*Fixture.World, Fixture.MakeRegistrations(SubjectIds), Config, Error))
		|| !TestTrue(TEXT("the scripted peer connects"), Peer.Connect(Config.Port, Error)))
	{
		AddError(Error);
		return false;
	}

	// One helper: the ownership line of one target.
	auto OwnershipLine = [&Receiver](const FString& Id) -> FString
	{
		for (const FString& Entry : Receiver.DescribeDriveOwnership())
		{
			if (Entry.StartsWith(Id + TEXT(":")))
			{
				return Entry;
			}
		}
		return FString();
	};

	FString Line;
	if (!TestTrue(TEXT("the first init is sent"),
			Peer.Send(EncodeInit(Fixture.MakeInit(SubjectIds)), Error))
		|| !TestTrue(TEXT("the first session is ready"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	const int64 FirstSession = Receiver.GetSessionId();
	if (!TestTrue(TEXT("a frame is sent"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 1, 1.0), FirstSession), Error))
		|| !TestTrue(TEXT("the frame is acknowledged"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	// While the preview drives both targets, the ownership report says so, and
	// the character's writer is currently muted by the preview.
	{
		const FString CharacterLine = OwnershipLine(CharacterId);
		const FString PropLine = OwnershipLine(PropId);
		TestTrue(TEXT("the driving character reports preview_active"),
			CharacterLine.Contains(TEXT("preview_active")));
		TestTrue(TEXT("the driving character's writer is listed as currently muted"),
			!CharacterLine.Contains(TEXT("suppressed_writers=[]"))
				&& CharacterLine.Contains(TEXT("muted_now=true"))
				&& CharacterLine.Contains(TEXT("restored_writers=[]")));
		TestTrue(TEXT("the driving prop reports preview_active"),
			PropLine.Contains(TEXT("preview_active")));
	}

	// Remove the prop: only it goes back to its own driver; the character keeps
	// previewing and its writer stays muted.
	if (!TestTrue(TEXT("the prop removal is sent"),
			Peer.Send(EncodeRemove(PropId, FirstSession), Error))
		|| !TestTrue(TEXT("the removal is acknowledged"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	{
		const FString CharacterLine = OwnershipLine(CharacterId);
		const FString PropLine = OwnershipLine(PropId);
		TestTrue(TEXT("the removed prop reports its own driver restored"),
			PropLine.Contains(TEXT("own_driver_restored")));
		TestFalse(TEXT("the removed prop is no longer preview_active"),
			PropLine.Contains(TEXT("preview_active")));
		TestTrue(TEXT("the character keeps previewing"),
			CharacterLine.Contains(TEXT("preview_active")));
	}

	// Renegotiating on the same connection takes both targets over again: the
	// report and the evidence must describe the active preview, not the earlier
	// exit of the prop/character.
	if (!TestTrue(TEXT("the second init is sent"),
			Peer.Send(EncodeInit(Fixture.MakeInit(SubjectIds)), Error))
		|| !TestTrue(TEXT("the second session is ready"), Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	const int64 SecondSession = Receiver.GetSessionId();
	TestTrue(TEXT("the renegotiation handed out a larger session"), SecondSession > FirstSession);
	if (!TestTrue(TEXT("a frame of the new session is sent"),
			Peer.Send(EncodeFrame(Fixture.MakeFrame(SubjectIds, 1, 2.0), SecondSession), Error))
		|| !TestTrue(TEXT("the new session's frame is acknowledged"),
			Peer.ReadLine(Receiver, Line, Error)))
	{
		AddError(Error);
		return false;
	}
	for (const FString& Id : { CharacterId, PropId })
	{
		const FString ReportLine = OwnershipLine(Id);
		TestTrue(FString::Printf(TEXT("after renegotiation '%s' reports preview_active"), *Id),
			ReportLine.Contains(TEXT("preview_active")));
		TestFalse(FString::Printf(TEXT("after renegotiation '%s' does not report an exit state"), *Id),
			ReportLine.Contains(TEXT("reference_pose")) || ReportLine.Contains(TEXT("own_driver_restored")));
	}

	// The machine-readable evidence must not carry the historical exit flag of
	// an actively driven target: this is the exact contradiction of R-003.
	{
		const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("ownership-renegotiation"));
		FString EvidencePath;
		if (TestTrue(TEXT("the evidence is written"), Receiver.SaveEvidence(Directory, EvidencePath, Error)))
		{
			TSharedPtr<FJsonObject> Evidence;
			FString EvidenceText;
			if (TestTrue(TEXT("the evidence reads back"),
					FFileHelper::LoadFileToString(EvidenceText, *EvidencePath)
						&& ParseJsonLine(EvidenceText, Evidence)))
			{
				TestTrue(TEXT("the preview is active"), GetBool(Evidence, TEXT("preview_active")));
				const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
				if (TestTrue(TEXT("the evidence lists the targets"),
						Evidence->TryGetArrayField(TEXT("targets"), Targets) && Targets != nullptr))
				{
					TestEqual(TEXT("both targets are recorded"), Targets->Num(), 2);
					for (const TSharedPtr<FJsonValue>& Value : *Targets)
					{
						const TSharedPtr<FJsonObject> Target = Value->AsObject();
						if (!Target.IsValid())
						{
							continue;
						}
						const FString Id = GetString(Target, TEXT("id"));
						TestTrue(FString::Printf(TEXT("'%s' is driving"), *Id),
							GetBool(Target, TEXT("driving")));
						TestEqual(FString::Printf(TEXT("'%s' reports preview_active"), *Id),
							GetString(Target, TEXT("exit")), FString(TEXT("preview_active")));
						TestFalse(FString::Printf(TEXT("'%s' does not claim a restored reference pose"), *Id),
							GetBool(Target, TEXT("restored_to_reference_pose")));
					}
				}
				const TArray<TSharedPtr<FJsonValue>>* Writers = nullptr;
				if (TestTrue(TEXT("the evidence lists preview writers"),
						Evidence->TryGetArrayField(TEXT("preview_writers"), Writers)
							&& Writers != nullptr && Writers->Num() == 1))
				{
					const TSharedPtr<FJsonObject> WriterObject = (*Writers)[0]->AsObject();
					TestTrue(TEXT("the character's writer is currently muted"),
						WriterObject.IsValid() && GetBool(WriterObject, TEXT("muted_now"))
							&& !GetBool(WriterObject, TEXT("restored")));
				}
			}
		}
	}

	// Session exit: the character had no driver of its own, so it goes back to
	// its reference pose; the prop had one, so it is restored. The report states
	// each result once, and the two outcomes stay distinguishable.
	Receiver.Stop(TEXT("ownership test finished"));
	{
		const FString CharacterLine = OwnershipLine(CharacterId);
		const FString PropLine = OwnershipLine(PropId);
		TestTrue(TEXT("the undriven character reports reference_pose after the exit"),
			CharacterLine.Contains(TEXT("exit=reference_pose")));
		TestTrue(TEXT("the driven prop reports own_driver_restored after the exit"),
			PropLine.Contains(TEXT("exit=own_driver_restored")));
		TestTrue(TEXT("the character's writer is reported as restored, not muted"),
			CharacterLine.Contains(TEXT("suppressed_writers=[]"))
				&& !CharacterLine.Contains(TEXT("restored_writers=[]"))
				&& CharacterLine.Contains(TEXT("muted_now=false")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// The production pairing (Issue 54 R-007): a real Maya character rig from the
// supplied scene and real Skeletal Meshes from the production project, driven
// in a disposable editor scene. Opt in with the -MtoUMultiSubjectReal* flags.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectRealAssetTest,
	"MtoUMultiSubjectPrototype.RealAssetPair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectRealAssetTest::RunTest(const FString& Parameters)
{
	FString MayapyPath;
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectMayapy="), MayapyPath);
	FString PeerPath;
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectPeer="), PeerPath);
	FString ScenePath;
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectMayaScene="), ScenePath);
	if (MayapyPath.IsEmpty() || PeerPath.IsEmpty() || ScenePath.IsEmpty())
	{
		AddInfo(TEXT("Real-asset check not requested; supply MtoUMultiSubjectMayapy, "
			"MtoUMultiSubjectPeer, MtoUMultiSubjectMayaScene, the two mesh flags and "
			"MtoUMultiSubjectCharacterRoot."));
		return true;
	}
	FString CharacterMeshPath;
	FString PropMeshPath;
	FString CharacterRoot;
	if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectCharacterMesh="), CharacterMeshPath)
		|| !FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectPropMesh="), PropMeshPath)
		|| !FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectCharacterRoot="), CharacterRoot))
	{
		AddError(TEXT("the real-asset check needs MtoUMultiSubjectCharacterMesh, "
			"MtoUMultiSubjectPropMesh and MtoUMultiSubjectCharacterRoot"));
		return false;
	}
	// The production C01 meshes reference an old Anim Blueprint whose
	// KawaiiPhysics nodes need a plug-in this host does not have: loading the
	// user's asset logs property errors that are a documented input limitation,
	// not a prototype failure. They are expected here so the run's own
	// assertions stay visible, and the boundary is recorded with the evidence.
	AddExpectedError(TEXT("AnimGraphNode_KawaiiPhysics"),
		EAutomationExpectedErrorFlags::Contains, 0);
	FString Scenario = TEXT("real-body");
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectRealScenario="), Scenario);
	FString PropRigSpec;
	FString PropRigDir;
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectPropRigSpec="), PropRigSpec);
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectPropRigDir="), PropRigDir);
	int32 Frames = 4;
	FParse::Value(FCommandLine::Get(), TEXT("MtoUMultiSubjectFrames="), Frames);
	TArray<double> Times;
	{
		FString TimesValue;
		if (FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
				FCommandLine::Get(), TEXT("-MtoUMultiSubjectTimes="), TimesValue))
		{
			TestTrue(TEXT("the times argument is a comma-separated list of finite numbers"),
				FMtoUMultiSubjectPeerRequest::ParseTimeList(TimesValue, Times));
		}
	}
	auto ReadRepeatedArgument = [](const TCHAR* Prefix, TArray<FString>& OutValues)
	{
		const FString CommandLine = FCommandLine::Get();
		int32 SearchFrom = 0;
		FString Value;
		while (SearchFrom < CommandLine.Len()
			&& FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
				*CommandLine.Mid(SearchFrom), Prefix, Value))
		{
			OutValues.Add(Value);
			SearchFrom += CommandLine.Mid(SearchFrom).Find(Prefix)
				+ FCString::Strlen(Prefix) + Value.Len();
		}
	};
	TArray<FString> SetCurves;
	TArray<FString> ForceCurves;
	ReadRepeatedArgument(TEXT("-MtoUMultiSubjectSetCurve="), SetCurves);
	ReadRepeatedArgument(TEXT("-MtoUMultiSubjectForceCurve="), ForceCurves);
	const bool bWantSequence =
		FParse::Param(FCommandLine::Get(), TEXT("MtoUMultiSubjectSequence"));

	USkeletalMesh* CharacterMesh = LoadObject<USkeletalMesh>(nullptr, *CharacterMeshPath);
	USkeletalMesh* PropMesh = LoadObject<USkeletalMesh>(nullptr, *PropMeshPath);
	if (!TestNotNull(TEXT("the real character mesh loads"), CharacterMesh)
		|| !TestNotNull(TEXT("the real prop mesh loads"), PropMesh))
	{
		return false;
	}
	FString Error;

	// The production assets are opened read-only: their packages and source
	// files must be exactly as they were before and after the run.
	auto FileDigest = [](const FString& Path) -> FString
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			return FString();
		}
		return FMD5::HashBytes(Bytes.GetData(), Bytes.Num());
	};
	auto AssetFile = [](const UObject& Asset) -> FString
	{
		return FPackageName::LongPackageNameToFilename(
			Asset.GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	};
	const FString CharacterAssetFile = AssetFile(*CharacterMesh);
	const FString PropAssetFile = AssetFile(*PropMesh);
	const FString CharacterDigestBefore = FileDigest(CharacterAssetFile);
	const FString PropDigestBefore = FileDigest(PropAssetFile);
	const FString SceneDigestBefore = FileDigest(ScenePath);
	TestTrue(TEXT("the real character mesh source file is readable"), !CharacterDigestBefore.IsEmpty());
	TestTrue(TEXT("the real prop mesh source file is readable"), !PropDigestBefore.IsEmpty());
	TestTrue(TEXT("the supplied Maya scene is readable"), !SceneDigestBefore.IsEmpty());
	const bool bCharacterDirtyBefore = CharacterMesh->GetOutermost()->IsDirty();
	const bool bPropDirtyBefore = PropMesh->GetOutermost()->IsDirty();

	UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("an editor world is available"), World))
	{
		return false;
	}

	const FString Directory = FPaths::Combine(EvidenceDirectory(), Scenario);
	IFileManager::Get().MakeDirectory(*Directory, true);

	// The two targets, at distinct non-origin placements. The prop anchor is
	// placed so that the pair's relative transform matches the Maya scene's, so
	// contact between the objects is preserved rather than assumed.
	FMtoUMultiSubjectSessionConfig Config;
	Config.Port = ReserveLoopbackPort();
	Config.Scenario = Scenario;
	if (!TestTrue(TEXT("a loopback port was reserved"), Config.Port != 0))
	{
		return false;
	}

	// First pass: a read-only probe tells us where the two Maya roots sit
	// relative to each other, so the Unreal anchors can reproduce that space.
	const FString ProbeEvidencePath =
		FPaths::Combine(Directory, TEXT("mtou-multi-subject-probe.json"));
	FTransform ProbeCharacterWorld = FTransform::Identity;
	FTransform ProbePropWorld = FTransform::Identity;
	{
		// The probe talks to nobody; it is started directly instead of through
		// the peer runner, and it never streams or saves.
		FMtoUMultiSubjectPeerRequest Probe;
		Probe.PeerScriptPath = PeerPath;
		Probe.ScenePath = ScenePath;
		Probe.ReferenceRigs.Add(FMtoUMultiSubjectFixtureBuilder::PropId());
		Probe.SubjectOverrides.Add(
			FString::Printf(TEXT("character=%s"), *CharacterRoot));
		Probe.RigSpec = PropRigSpec;
		Probe.RigDir = PropRigDir;
		Probe.EvidencePath = ProbeEvidencePath;
		// The script path has to come first: the flags that follow it must not
		// be read as mayapy's own options.
		Probe.ExtraArguments.Add(TEXT("--probe"));
		Probe.ExtraArguments.Add(TEXT("--probe-times 1"));
		Probe.ExtraArguments.Add(TEXT("--pair character+prop"));
		int32 ReturnCode = -1;
		FString ProbeStdOut;
		FString ProbeStdErr;
		(void)FPlatformProcess::ExecProcess(
			*MayapyPath, *Probe.BuildCommandLine(),
			&ReturnCode, &ProbeStdOut, &ProbeStdErr);
		FFileHelper::SaveStringToFile(
			FString::Printf(TEXT("[unreal] exit=%d\n%s\n%s"), ReturnCode, *ProbeStdOut, *ProbeStdErr),
			*FPaths::Combine(Directory, TEXT("mtou-multi-subject-probe.log")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TestTrue(TEXT("the read-only Maya probe runs"), ReturnCode == 0);
		TSharedPtr<FJsonObject> ProbeJson;
		FString ProbeText;
		if (TestTrue(TEXT("the probe evidence reads back"),
				FFileHelper::LoadFileToString(ProbeText, *ProbeEvidencePath)
					&& ParseJsonLine(ProbeText, ProbeJson)))
		{
			const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
			const TSharedPtr<FJsonObject>* Subjects = nullptr;
			if (ProbeJson->TryGetArrayField(TEXT("samples"), Samples) && Samples != nullptr
				&& !Samples->IsEmpty()
				&& (*Samples)[0]->AsObject()->TryGetObjectField(TEXT("subjects"), Subjects))
			{
				const TSharedPtr<FJsonObject>* CharacterSubject = nullptr;
				const TSharedPtr<FJsonObject>* PropSubject = nullptr;
				(*Subjects)->TryGetObjectField(TEXT("character"), CharacterSubject);
				(*Subjects)->TryGetObjectField(TEXT("prop"), PropSubject);
				const TArray<TSharedPtr<FJsonValue>>* CharacterRootRow = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* PropRootRow = nullptr;
				if (CharacterSubject != nullptr && PropSubject != nullptr
					&& (*CharacterSubject)->TryGetArrayField(TEXT("root"), CharacterRootRow)
					&& (*PropSubject)->TryGetArrayField(TEXT("root"), PropRootRow)
					&& CharacterRootRow->Num() == 10 && PropRootRow->Num() == 10)
				{
					auto Row = [](const TArray<TSharedPtr<FJsonValue>>& Values, double* Out)
					{
						for (int32 Index = 0; Index < 10; ++Index)
						{
							Out[Index] = (*Values[Index]).AsNumber();
						}
					};
					double CharacterRow[10];
					double PropRow[10];
					Row(*CharacterRootRow, CharacterRow);
					Row(*PropRootRow, PropRow);
					const FTransform CharacterWorld(
						FQuat(CharacterRow[3], CharacterRow[4], CharacterRow[5], CharacterRow[6]),
						FVector(CharacterRow[0], CharacterRow[1], CharacterRow[2]),
						FVector(CharacterRow[7], CharacterRow[8], CharacterRow[9]));
					const FTransform PropWorld(
						FQuat(PropRow[3], PropRow[4], PropRow[5], PropRow[6]),
						FVector(PropRow[0], PropRow[1], PropRow[2]),
						FVector(PropRow[7], PropRow[8], PropRow[9]));
					ProbeCharacterWorld = CharacterWorld;
					ProbePropWorld = PropWorld;
					AddInfo(FString::Printf(
						TEXT("Maya pair placement: character root (%s), prop root (%s)"),
						*CharacterWorld.GetTranslation().ToCompactString(),
						*PropWorld.GetTranslation().ToCompactString()));
				}
			}
		}
	}

	// Both anchors carry one common placement applied to the pose each object
	// has in the scene: the pair therefore sits in the disposable world the way
	// the scene shows it (their relative placement is the scene's), and each
	// target's own rest frame is what the projection cancels at run time. The
	// character anchor is the chosen common placement, off-origin and rotated.
	const FVector CharacterLocation(320.0, -180.0, 40.0);
	const FRotator CharacterRotation(0.0, 25.0, 0.0);
	const FTransform CommonPlacement(CharacterRotation, CharacterLocation);
	// One common world translation on both anchors: the rig roots carry the
	// scene's own import convention, which lands the pair under the level's
	// floor plane at the chosen placement, and the floor would hide it from an
	// elevated capture camera. A common translation leaves the pair's relative
	// transform exactly as it was, so the placement contract above is
	// unchanged; only the world position of the disposable pair moves.
	const FTransform WorldLift(FVector(0.0, 0.0, 400.0));
	const FTransform CharacterAnchorTransform =
		(CommonPlacement * ProbeCharacterWorld) * WorldLift;
	const FTransform PropAnchorTransform =
		(CommonPlacement * ProbePropWorld) * WorldLift;
	const FTransform SceneRelative = ProbeCharacterWorld.Inverse() * ProbePropWorld;

	AActor* CharacterAnchor = nullptr;
	AActor* PropAnchor = nullptr;
	USkeletalMeshComponent* CharacterComponent = nullptr;
	USkeletalMeshComponent* PropComponent = nullptr;
	FMtoUMultiSubjectReceiver Receiver;
	ON_SCOPE_EXIT
	{
		Receiver.Stop(TEXT("real-asset check finished"));
	};
	if (!TestTrue(TEXT("the real character target spawns"),
			FMtoUMultiSubjectFixtureBuilder::SpawnAssetTarget(*World,
				TEXT("MtoU_RealCharacterAnchor"), CharacterAnchorTransform.GetTranslation(),
				CharacterAnchorTransform.Rotator(), *CharacterMesh,
				CharacterAnchor, CharacterComponent))
		|| !TestTrue(TEXT("the real prop target spawns"),
			FMtoUMultiSubjectFixtureBuilder::SpawnAssetTarget(*World,
				TEXT("MtoU_RealPropAnchor"), PropAnchorTransform.GetTranslation(),
				PropAnchorTransform.Rotator(), *PropMesh,
				PropAnchor, PropComponent)))
	{
		return false;
	}
	// The anchors reproduce the scene's own placement of the two objects, which
	// is what "the pair keeps the scene's contact" means for a placement anchor.
	const FTransform AnchorRelative = CharacterAnchorTransform.Inverse() * PropAnchorTransform;
	AddInfo(FString::Printf(TEXT("scene relation: anchors %s vs Maya %s"),
		*AnchorRelative.GetTranslation().ToCompactString(),
		*SceneRelative.GetTranslation().ToCompactString()));
	TestTrue(TEXT("the disposable scene reproduces the Maya pair's relative placement (contact)"),
		MtoUSubjectTransformDelta(AnchorRelative, SceneRelative) < 1e-3);

	// The real targets' own skeletons, so the run's negotiation can be read
	// against what the meshes actually need (and a matching Maya rig can be
	// generated for a target the scene does not already reference).
	{
		const TSharedRef<FJsonObject> Targets = MakeShared<FJsonObject>();
		auto DescribeTarget = [](const FString& Id, USkeletalMesh& Mesh) -> TSharedRef<FJsonObject>
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), Id);
			Object->SetStringField(TEXT("mesh"), Mesh.GetPathName());
			const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
			Object->SetNumberField(TEXT("bones"), Skeleton.GetNum());
			TArray<bool> Required;
			FString Problem;
			Object->SetBoolField(TEXT("skin_weights_readable"),
				FMtoUMultiSubjectProtocol::CollectRequiredTargetBones(Mesh, Required, Problem));
			Object->SetStringField(TEXT("skin_weight_problem"), Problem);
			int32 RequiredCount = 0;
			for (const bool bRequired : Required)
			{
				RequiredCount += bRequired ? 1 : 0;
			}
			Object->SetNumberField(TEXT("required_bones"), RequiredCount);
			TArray<TSharedPtr<FJsonValue>> BoneValues;
			auto NumberArray = [](const FVector& Vector)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				Values.Add(MakeShared<FJsonValueNumber>(Vector.X));
				Values.Add(MakeShared<FJsonValueNumber>(Vector.Y));
				Values.Add(MakeShared<FJsonValueNumber>(Vector.Z));
				return Values;
			};
			for (int32 BoneIndex = 0; BoneIndex < Skeleton.GetNum(); ++BoneIndex)
			{
				const FTransform& Pose = Skeleton.GetRefBonePose()[BoneIndex];
				const TSharedRef<FJsonObject> BoneObject = MakeShared<FJsonObject>();
				BoneObject->SetNumberField(TEXT("index"), BoneIndex);
				BoneObject->SetStringField(TEXT("name"), Skeleton.GetBoneName(BoneIndex).ToString());
				const int32 ParentIndex = Skeleton.GetParentIndex(BoneIndex);
				BoneObject->SetStringField(TEXT("parent"),
					ParentIndex == INDEX_NONE
						? TEXT("none")
						: Skeleton.GetBoneName(ParentIndex).ToString());
				BoneObject->SetArrayField(TEXT("translation"), NumberArray(Pose.GetTranslation()));
				const FQuat Rotation = Pose.GetRotation();
				TArray<TSharedPtr<FJsonValue>> RotationValues;
				RotationValues.Add(MakeShared<FJsonValueNumber>(Rotation.X));
				RotationValues.Add(MakeShared<FJsonValueNumber>(Rotation.Y));
				RotationValues.Add(MakeShared<FJsonValueNumber>(Rotation.Z));
				RotationValues.Add(MakeShared<FJsonValueNumber>(Rotation.W));
				BoneObject->SetArrayField(TEXT("rotation"), RotationValues);
				BoneObject->SetArrayField(TEXT("scale"), NumberArray(Pose.GetScale3D()));
				BoneValues.Add(MakeShared<FJsonValueObject>(BoneObject));
			}
			Object->SetArrayField(TEXT("skeleton"), BoneValues);
			TArray<TSharedPtr<FJsonValue>> MorphValues;
			for (const TObjectPtr<UMorphTarget>& Morph : Mesh.GetMorphTargets())
			{
				if (Morph != nullptr)
				{
					MorphValues.Add(MakeShared<FJsonValueString>(Morph->GetFName().ToString()));
				}
			}
			Object->SetNumberField(TEXT("morph_targets"), MorphValues.Num());
			Object->SetArrayField(TEXT("morph_names"), MorphValues);
			return Object;
		};
		Targets->SetObjectField(TEXT("character"),
			DescribeTarget(FMtoUMultiSubjectFixtureBuilder::CharacterId(), *CharacterMesh));
		Targets->SetObjectField(TEXT("prop"),
			DescribeTarget(FMtoUMultiSubjectFixtureBuilder::PropId(), *PropMesh));
		FString TargetsText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> TargetsWriter =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&TargetsText);
		FJsonSerializer::Serialize(Targets, TargetsWriter);
		FFileHelper::SaveStringToFile(TargetsText,
			*FPaths::Combine(Directory, TEXT("mtou-multi-subject-real-targets-unreal.json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// An optional transient animation track on the real character, so the
	// preview takeover is verified against a Sequencer writer on a production
	// skeleton instead of the fixture one. Nothing is saved.
	UAnimSequence* CharacterAnimation = nullptr;
	ULevelSequence* Sequence = nullptr;
	UMovieSceneSkeletalAnimationTrack* CharacterTrack = nullptr;
	FMtoUPoseSnapshot SequencePose;
	if (bWantSequence)
	{
		CharacterAnimation = NewObject<UAnimSequence>(GetTransientPackage(),
			TEXT("MtoUReal_CharacterAnim"), RF_Transient);
		CharacterAnimation->SetSkeleton(CharacterMesh->GetSkeleton());
		{
			IAnimationDataController& Controller = CharacterAnimation->GetController();
			Controller.InitializeModel();
			Controller.OpenBracket(FText::FromString(TEXT("MtoU multi-subject real asset")), false);
			Controller.SetFrameRate(FFrameRate(30, 1), false);
			Controller.SetNumberOfFrames(FFrameNumber(20), false);
			const FName RootBone = CharacterMesh->GetRefSkeleton().GetBoneName(0);
			Controller.AddBoneCurve(RootBone, false);
			TArray<FVector3f> Positions;
			TArray<FQuat4f> Rotations;
			TArray<FVector3f> Scales;
			for (int32 Frame = 0; Frame <= 20; ++Frame)
			{
				Positions.Add(FVector3f(20.0f * Frame / 20.0f, 0.0f, 0.0f));
				Rotations.Add(FQuat4f::Identity);
				Scales.Add(FVector3f::OneVector);
			}
			Controller.SetBoneTrackKeys(RootBone, Positions, Rotations, Scales, false);
			Controller.CloseBracket(false);
			Controller.NotifyPopulated();
		}
		Sequence = NewObject<ULevelSequence>(GetTransientPackage(), NAME_None, RF_Transient);
		Sequence->Initialize();
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		MovieScene->SetDisplayRate(FFrameRate(30, 1));
		MovieScene->SetTickResolutionDirectly(FFrameRate(30000, 1));
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(
			TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(30000)),
			TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(30000 + 200000))));
		const FGuid Binding = MovieScene->AddPossessable(
			TEXT("RealCharacter"), USkeletalMeshComponent::StaticClass());
		Sequence->BindPossessableObject(Binding, *CharacterComponent, World);
		CharacterTrack = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(Binding);
		if (TestNotNull(TEXT("the transient real-skeleton animation track exists"), CharacterTrack))
		{
			CharacterTrack->SetEvalDisabled(false);
			UMovieSceneSection* Section =
				CharacterTrack->AddNewAnimation(FFrameNumber(30000), CharacterAnimation);
			if (TestNotNull(TEXT("the transient real-skeleton section exists"), Section))
			{
				Section->SetRange(TRange<FFrameNumber>(
					TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(30000)),
					TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(230000))));
			}
			FMtoUPreviewWriter Writer;
			Writer.Track = CharacterTrack;
			Writer.TargetId = FMtoUMultiSubjectFixtureBuilder::CharacterId();
			Config.PreviewWriters.Add(Writer);
			MtoUCapturePoseSnapshot(*CharacterComponent, SequencePose);
		}
	}

	const TArray<FMtoUTargetRegistration> Registrations = {
		{ FMtoUMultiSubjectFixtureBuilder::CharacterId(), CharacterAnchor, CharacterComponent },
		{ FMtoUMultiSubjectFixtureBuilder::PropId(), PropAnchor, PropComponent } };
	const TArray<FString> SubjectIds = {
		FMtoUMultiSubjectFixtureBuilder::CharacterId(),
		FMtoUMultiSubjectFixtureBuilder::PropId() };
	if (!TestTrue(TEXT("the receiver starts on the real targets"),
			Receiver.Start(*World, Registrations, Config, Error)))
	{
		AddError(Error);
		return false;
	}

	FMtoUMultiSubjectPeerRequest Request;
	Request.MayapyPath = MayapyPath;
	Request.PeerScriptPath = PeerPath;
	Request.Scenario = TEXT("character-prop");
	Request.Port = Receiver.GetBoundPort();
	Request.Frames = Frames;
	Request.Times = Times;
	Request.ScenePath = ScenePath;
	Request.ReferenceRigs.Add(FMtoUMultiSubjectFixtureBuilder::PropId());
	Request.SubjectOverrides.Add(
		FString::Printf(TEXT("character=%s"), *CharacterRoot));
	Request.RigSpec = PropRigSpec;
	Request.RigDir = PropRigDir;
	Request.EvidencePath = FPaths::Combine(Directory, TEXT("mtou-multi-subject-maya.json"));
	Request.LogPath = FPaths::Combine(Directory, TEXT("mtou-multi-subject-maya.log"));
	for (const FString& SetCurve : SetCurves)
	{
		Request.ExtraArguments.Add(FString::Printf(TEXT("--set-curve \"%s\""), *SetCurve));
	}
	for (const FString& ForceCurve : ForceCurves)
	{
		Request.ExtraArguments.Add(FString::Printf(TEXT("--force-curve \"%s\""), *ForceCurve));
	}

	FMtoUMultiSubjectPeerResult Result;
	const bool bRan = RunMayaMultiSubjectPeer(Request, Receiver, Result, Error);
	if (!TestTrue(TEXT("the Maya peer ran against the real targets"), bRan))
	{
		AddError(Error);
	}
	TestTrue(TEXT("the Maya peer started"), Result.bStarted);
	TestEqual(TEXT("the Maya peer exited cleanly"), Result.ReturnCode, 0);
	TestTrue(TEXT("the Maya evidence reports success"),
		Result.bEvidenceOkField && Result.bEvidenceOk);
	TestTrue(TEXT("the Maya evidence covers the requested frames"),
		Result.EvidenceFrameCount >= Frames);
	TestTrue(TEXT("the session streamed on the real pair"),
		Receiver.GetAppliedFrameCount() >= Frames);

	// The negotiation against the production meshes. The session is over by now
	// (the peer disconnected), so the snapshot captured while it streamed is the
	// record of what was negotiated.
	const TArray<FMtoUNegotiatedSubject>& Subjects = Result.NegotiatedSubjects;
	TestEqual(TEXT("two subjects were negotiated"), Subjects.Num(), 2);
	for (const FMtoUNegotiatedSubject& Subject : Subjects)
	{
		AddInfo(FString::Printf(
			TEXT("%s: declared=%d driven=%d required=%d source_only=%d undriven=%d"),
			*Subject.Id, Subject.Declaration.Bones.Num(), Subject.Map.DrivenTargetBones.Num(),
			Subject.Map.RequiredTargetBones.Num(), Subject.Map.SourceOnlyBones,
			Subject.Map.UndrivenTargetBones.Num()));
		TestTrue(FString::Printf(TEXT("'%s' covers every required target bone"), *Subject.Id),
			Subject.Map.bCoversRequiredBones);
		TestTrue(FString::Printf(TEXT("'%s' drives at least the required bones"), *Subject.Id),
			Subject.Map.DrivenTargetBones.Num() >= Subject.Map.RequiredTargetBones.Num());
	}
	{
		const FMtoUNegotiatedSubject* Character = Subjects.FindByPredicate(
			[](const FMtoUNegotiatedSubject& Subject)
			{
				return Subject.Id == FMtoUMultiSubjectFixtureBuilder::CharacterId();
			});
		if (TestNotNull(TEXT("the character subject is present"), Character))
		{
			TestTrue(TEXT("the real character mesh needs a bone subset of the rig"),
				Character->Map.RequiredTargetBones.Num() > 0
					&& Character->Map.RequiredTargetBones.Num()
						<= Character->Declaration.Bones.Num());
			// Every declared bone is either driven or reported as an ignored
			// export branch: a target that owns the whole rig drives all of it,
			// a smaller one ignores the rest instead of refusing it.
			TestEqual(TEXT("every declared bone is driven or reported as an export branch"),
				Character->Map.DrivenTargetBones.Num() + Character->Map.SourceOnlyBones,
				Character->Declaration.Bones.Num());
		}
	}

	// Per-frame numbers: the wire pose is what the real components hold.
	double MaxBoneDelta = 0.0;
	double MaxRootDelta = 0.0;
	for (const FMtoUFrameRecord& Record : Receiver.GetFrameRecords())
	{
		for (const FMtoUSubjectMeasurement& Measurement : Record.Subjects)
		{
			MaxRootDelta = FMath::Max(MaxRootDelta, Measurement.RootWorldDelta);
			for (const FMtoUBoneMeasurement& Bone : Measurement.Bones)
			{
				MaxBoneDelta = FMath::Max(MaxBoneDelta, Bone.Delta);
			}
		}
	}
	AddInfo(FString::Printf(TEXT("real pair deltas: max_bone=%.6f max_root_world=%.6f"),
		MaxBoneDelta, MaxRootDelta));
	// A production rig at centimetre scale is evaluated in float, so the
	// prototype bound is looser than the fixture's; the measured values are the
	// evidence, and they stay two orders of magnitude below it.
	TestTrue(TEXT("every driven real bone holds the projected pose"), MaxBoneDelta < 0.01);
	TestTrue(TEXT("the real root is anchored exactly once"), MaxRootDelta < 0.01);

	// The two objects are driven in one world at one shared time; each root
	// world pose is its own wire pose applied through its own anchor exactly
	// once (measured above). The per-frame relation of the two targets is
	// reported so the scene's placement can be compared across hosts.
	{
		bool bEveryFrameCarriesBothSubjects = Receiver.GetFrameRecords().Num() > 0;
		TArray<FString> Relations;
		for (const FMtoUFrameRecord& Record : Receiver.GetFrameRecords())
		{
			const FMtoUSubjectMeasurement* CharacterMeasurement = Record.Subjects.FindByPredicate(
				[](const FMtoUSubjectMeasurement& Measurement)
				{
					return Measurement.Id == FMtoUMultiSubjectFixtureBuilder::CharacterId();
				});
			const FMtoUSubjectMeasurement* PropMeasurement = Record.Subjects.FindByPredicate(
				[](const FMtoUSubjectMeasurement& Measurement)
				{
					return Measurement.Id == FMtoUMultiSubjectFixtureBuilder::PropId();
				});
			bEveryFrameCarriesBothSubjects &= CharacterMeasurement != nullptr
				&& PropMeasurement != nullptr;
			if (CharacterMeasurement != nullptr && PropMeasurement != nullptr)
			{
				const FTransform Relative = CharacterMeasurement->RootWorld.Inverse()
					* PropMeasurement->RootWorld;
				Relations.Add(FString::Printf(TEXT("t=%.3f %s"), Record.Time,
					*Relative.GetTranslation().ToCompactString()));
			}
		}
		TestTrue(TEXT("every applied frame carried both objects at one time"),
			bEveryFrameCarriesBothSubjects);
		AddInfo(TEXT("per-frame pair relation: ") + FString::Join(Relations, TEXT("; ")));
	}
	// The scene the anchors reproduce is the one the peer actually streamed:
	// its first applied frame's own relative placement has to be the relation
	// the read-only probe measured.
	{
		const TArray<TSharedPtr<FJsonValue>>* MayaSessions = nullptr;
		bool bProbeAgrees = false;
		if (Result.Evidence.IsValid()
			&& Result.Evidence->TryGetArrayField(TEXT("sessions"), MayaSessions)
			&& MayaSessions != nullptr && !MayaSessions->IsEmpty())
		{
			const TSharedPtr<FJsonObject> MayaSession = (*MayaSessions)[0]->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* MayaFrames = nullptr;
			if (MayaSession.IsValid()
				&& MayaSession->TryGetArrayField(TEXT("frames"), MayaFrames)
				&& MayaFrames != nullptr && !MayaFrames->IsEmpty())
			{
				const TSharedPtr<FJsonObject> MayaFrame = (*MayaFrames)[0]->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* MayaSubjects = nullptr;
				if (MayaFrame.IsValid()
					&& MayaFrame->TryGetArrayField(TEXT("subjects"), MayaSubjects)
					&& MayaSubjects != nullptr)
				{
					auto RootRow = [](const TSharedPtr<FJsonObject>& Subject)
					{
						FTransform Transform = FTransform::Identity;
						const TArray<TSharedPtr<FJsonValue>>* Row = nullptr;
						if (Subject.IsValid()
							&& Subject->TryGetArrayField(TEXT("root"), Row)
							&& Row != nullptr && Row->Num() == 10)
						{
							Transform = FTransform(
								FQuat((*Row)[3]->AsNumber(), (*Row)[4]->AsNumber(),
									(*Row)[5]->AsNumber(), (*Row)[6]->AsNumber()),
								FVector((*Row)[0]->AsNumber(), (*Row)[1]->AsNumber(),
									(*Row)[2]->AsNumber()),
								FVector((*Row)[7]->AsNumber(), (*Row)[8]->AsNumber(),
									(*Row)[9]->AsNumber()));
						}
						return Transform;
					};
					FTransform CharacterWorld = FTransform::Identity;
					FTransform PropWorld = FTransform::Identity;
					for (const TSharedPtr<FJsonValue>& SubjectValue : *MayaSubjects)
					{
						const TSharedPtr<FJsonObject> Subject = SubjectValue->AsObject();
						if (!Subject.IsValid())
						{
							continue;
						}
						const FString Id = Subject->GetStringField(TEXT("id"));
						if (Id == FMtoUMultiSubjectFixtureBuilder::CharacterId())
						{
							CharacterWorld = RootRow(Subject);
						}
						else if (Id == FMtoUMultiSubjectFixtureBuilder::PropId())
						{
							PropWorld = RootRow(Subject);
						}
					}
					bProbeAgrees = MtoUSubjectTransformDelta(
						CharacterWorld.Inverse() * PropWorld, SceneRelative) < 1e-3;
				}
			}
		}
		TestTrue(TEXT("the streamed scene's own relation is the probed relation"),
			bProbeAgrees);
	}

	// Morph values: the curve values the Maya peer sent are the values the real
	// mesh holds, for every curve the target actually owns.
	{
		const TSharedPtr<FJsonObject> MayaEvidence = Result.Evidence;
		const TArray<TSharedPtr<FJsonValue>>* MayaSessions = nullptr;
		int32 ComparedCurves = 0;
		double MaxCurveDelta = 0.0;
		if (MayaEvidence.IsValid()
			&& MayaEvidence->TryGetArrayField(TEXT("sessions"), MayaSessions)
			&& MayaSessions != nullptr && !MayaSessions->IsEmpty())
		{
			const TSharedPtr<FJsonObject> MayaSession = (*MayaSessions)[0]->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* MayaFrames = nullptr;
			if (MayaSession.IsValid()
				&& MayaSession->TryGetArrayField(TEXT("frames"), MayaFrames)
				&& MayaFrames != nullptr)
			{
				for (int32 FrameIndex = 0;
					FrameIndex < MayaFrames->Num() && FrameIndex < Receiver.GetFrameRecords().Num();
					++FrameIndex)
				{
					const TSharedPtr<FJsonObject> MayaFrame = (*MayaFrames)[FrameIndex]->AsObject();
					const FMtoUFrameRecord& Record = Receiver.GetFrameRecords()[FrameIndex];
					const TArray<TSharedPtr<FJsonValue>>* MayaSubjects = nullptr;
					if (!MayaFrame.IsValid()
						|| !MayaFrame->TryGetArrayField(TEXT("subjects"), MayaSubjects)
						|| MayaSubjects == nullptr)
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& MayaSubjectValue : *MayaSubjects)
					{
						const TSharedPtr<FJsonObject> MayaSubject = MayaSubjectValue->AsObject();
						if (!MayaSubject.IsValid())
						{
							continue;
						}
						const FString SubjectId = MayaSubject->GetStringField(TEXT("id"));
						const FMtoUSubjectMeasurement* Measurement = Record.Subjects.FindByPredicate(
							[&SubjectId](const FMtoUSubjectMeasurement& Candidate)
							{
								return Candidate.Id == SubjectId;
							});
						const FMtoUNegotiatedSubject* Subject = Subjects.FindByPredicate(
							[&SubjectId](const FMtoUNegotiatedSubject& Candidate)
							{
								return Candidate.Id == SubjectId;
							});
						const TArray<TSharedPtr<FJsonValue>>* MayaCurves = nullptr;
						if (Measurement == nullptr || Subject == nullptr
							|| !MayaSubject->TryGetArrayField(TEXT("curves"), MayaCurves)
							|| MayaCurves == nullptr)
						{
							continue;
						}
						for (int32 CurveIndex = 0;
							CurveIndex < MayaCurves->Num()
								&& CurveIndex < Subject->Declaration.Curves.Num();
							++CurveIndex)
						{
							if (Subject->Map.SourceOnlyCurves.Contains(
									Subject->Declaration.Curves[CurveIndex]))
							{
								continue;
							}
							const TPair<FName, float>* Applied = Measurement->Curves.FindByPredicate(
								[&Subject, CurveIndex](const TPair<FName, float>& Candidate)
								{
									return Candidate.Key == Subject->Declaration.Curves[CurveIndex];
								});
							if (Applied == nullptr)
							{
								continue;
							}
							MaxCurveDelta = FMath::Max(MaxCurveDelta,
								FMath::Abs(Applied->Value
									- static_cast<float>((*MayaCurves)[CurveIndex]->AsNumber())));
							++ComparedCurves;
						}
					}
				}
			}
		}
		TestTrue(TEXT("at least one Morph curve was compared on the real mesh"),
			ComparedCurves > 0);
		TestTrue(TEXT("the real mesh holds the Morph values Maya sent"),
			MaxCurveDelta < 1e-5);
		AddInfo(FString::Printf(TEXT("Morph comparison: %d curve values, max delta %.9f"),
			ComparedCurves, MaxCurveDelta));
	}
	if (!SetCurves.IsEmpty() || !ForceCurves.IsEmpty())
	{
		bool bNonZero = false;
		for (const FMtoUFrameRecord& Record : Receiver.GetFrameRecords())
		{
			for (const FMtoUSubjectMeasurement& Measurement : Record.Subjects)
			{
				for (const TPair<FName, float>& Curve : Measurement.Curves)
				{
					bNonZero |= FMath::Abs(Curve.Value) > 1e-6f;
				}
			}
		}
		TestTrue(TEXT("the requested Morph override produced a non-zero applied value"), bNonZero);
	}
	if (!ForceCurves.IsEmpty())
	{
		// A forced override is a scene manipulation, and the evidence has to say
		// what it disconnected: the value is only meaningful with that record.
		const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
		bool bRecorded = false;
		if (Result.Evidence.IsValid()
			&& Result.Evidence->TryGetArrayField(TEXT("curve_overrides"), Overrides)
			&& Overrides != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& OverrideValue : *Overrides)
			{
				const TSharedPtr<FJsonObject> Override = OverrideValue->AsObject();
				if (!Override.IsValid() || !GetBool(Override, TEXT("forced")))
				{
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* Plugs = nullptr;
				if (Override->TryGetArrayField(TEXT("plugs"), Plugs) && Plugs != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& PlugValue : *Plugs)
					{
						const TSharedPtr<FJsonObject> Plug = PlugValue->AsObject();
						if (Plug.IsValid()
							&& (Plug->HasField(TEXT("disconnected"))
								|| GetBool(Plug, TEXT("unlocked"))))
						{
							bRecorded = true;
						}
					}
				}
			}
		}
		TestTrue(TEXT("the forced Morph override records what it disconnected"), bRecorded);
	}

	// The exit: no target keeps the preview pose, and the writer state returns.
	Receiver.Stop(TEXT("real-asset check finished"));
	TestTrue(TEXT("the character target left the preview at its reference pose"),
		MaxDeltaToReferencePose(*CharacterComponent) < 1e-3);
	TestTrue(TEXT("the prop target left the preview at its reference pose"),
		MaxDeltaToReferencePose(*PropComponent) < 1e-3);
	if (bWantSequence)
	{
		TestNotNull(TEXT("the transient writer track still exists"), CharacterTrack);
		TestFalse(TEXT("the transient writer track is enabled again"),
			CharacterTrack->IsLocalEvalDisabled());
	}

	// The real samples this run streamed: the lifecycle phases and the BaseColor
	// hold replay them, so every picture and check below shows a Maya-sampled
	// frame instead of a synthesized pose.
	const FMtoUFrameRecord* SampleMax = nullptr;
	const FMtoUFrameRecord* SampleMin = nullptr;
	for (const FMtoUFrameRecord& Record : Receiver.GetFrameRecords())
	{
		if (SampleMax == nullptr || Record.Time > SampleMax->Time)
		{
			SampleMax = &Record;
		}
		if (SampleMin == nullptr || Record.Time < SampleMin->Time)
		{
			SampleMin = &Record;
		}
	}

	// A different skeleton's input must not silently drive this target: the
	// character's own declaration against the prop target is refused, and a
	// fresh negotiation with the target's own input is accepted.
	{
		const FMtoUSubjectDeclaration* CharacterDeclaration = nullptr;
		const FMtoUSubjectDeclaration* PropDeclaration = nullptr;
		for (const FMtoUNegotiatedSubject& Negotiated : Result.NegotiatedSubjects)
		{
			if (Negotiated.Id == FMtoUMultiSubjectFixtureBuilder::CharacterId())
			{
				CharacterDeclaration = &Negotiated.Declaration;
			}
			else if (Negotiated.Id == FMtoUMultiSubjectFixtureBuilder::PropId())
			{
				PropDeclaration = &Negotiated.Declaration;
			}
		}
		if (TestNotNull(TEXT("the character declaration was captured"), CharacterDeclaration)
			&& TestNotNull(TEXT("the prop declaration was captured"), PropDeclaration))
		{
			FMtoUMultiSubjectReceiver LocalReceiver;
			FMtoUScriptedPeer LocalPeer;
			const FString LocalScenario = Scenario + TEXT("-renegotiation");
			FMtoUMultiSubjectSessionConfig LocalConfig;
			LocalConfig.Port = ReserveLoopbackPort();
			LocalConfig.Scenario = LocalScenario;
			AActor* LocalCharacterAnchor = nullptr;
			AActor* LocalPropAnchor = nullptr;
			USkeletalMeshComponent* LocalCharacterComponent = nullptr;
			USkeletalMeshComponent* LocalPropComponent = nullptr;
			if (!TestTrue(TEXT("the local renegotiation targets spawn"),
					FMtoUMultiSubjectFixtureBuilder::SpawnAssetTarget(*World,
						TEXT("MtoU_RealRenegotiationCharacter"), CharacterAnchorTransform.GetTranslation(),
						CharacterAnchorTransform.Rotator(), *CharacterMesh,
						LocalCharacterAnchor, LocalCharacterComponent))
				|| !TestTrue(TEXT("the local prop target spawns"),
					FMtoUMultiSubjectFixtureBuilder::SpawnAssetTarget(*World,
						TEXT("MtoU_RealRenegotiationProp"), PropAnchorTransform.GetTranslation(),
						PropAnchorTransform.Rotator(), *PropMesh,
						LocalPropAnchor, LocalPropComponent)))
			{
				LocalReceiver.Stop(TEXT("setup failed"));
				return false;
			}
			const TArray<FMtoUTargetRegistration> LocalRegistrations = {
				{ FMtoUMultiSubjectFixtureBuilder::CharacterId(),
					LocalCharacterAnchor, LocalCharacterComponent },
				{ FMtoUMultiSubjectFixtureBuilder::PropId(),
					LocalPropAnchor, LocalPropComponent } };
			if (TestTrue(TEXT("the local receiver starts"),
					LocalReceiver.Start(*World, LocalRegistrations, LocalConfig, Error))
				&& TestTrue(TEXT("the local peer connects"),
					LocalPeer.Connect(LocalConfig.Port, Error)))
			{
				FMtoUInitMessage Mismatched;
				Mismatched.Version = MtoUMultiSubjectProtocol::Version;
				Mismatched.Fps = 30.0;
				Mismatched.Subjects.Add(*CharacterDeclaration);
				Mismatched.Subjects.Add(*CharacterDeclaration);
				Mismatched.Subjects[1].Id = FMtoUMultiSubjectFixtureBuilder::PropId();
				FString Line;
				if (TestTrue(TEXT("the mismatched real init is sent"),
						LocalPeer.Send(EncodeInit(Mismatched), Error))
					&& TestTrue(TEXT("the mismatched real init is answered"),
						LocalPeer.ReadLine(LocalReceiver, Line, Error)))
				{
					FString Details;
					ExpectError(*this, Line, TEXT("mismatched real init"),
						MtoUMultiSubjectError::SkeletonMismatch, &Details);
					TestTrue(TEXT("the refusal names the prop target's required bone"),
						Details.Contains(TEXT("Box023_Jnt")) || Details.Contains(TEXT("Root")));
				}
				FMtoUInitMessage Correct;
				Correct.Version = MtoUMultiSubjectProtocol::Version;
				Correct.Fps = 30.0;
				Correct.Subjects.Add(*CharacterDeclaration);
				Correct.Subjects.Add(*PropDeclaration);
				if (TestTrue(TEXT("the correct real init is sent"),
						LocalPeer.Send(EncodeInit(Correct), Error))
					&& TestTrue(TEXT("the correct real init is answered"),
						LocalPeer.ReadLine(LocalReceiver, Line, Error)))
				{
					TSharedPtr<FJsonObject> Ready;
					TestTrue(TEXT("the correct reply is JSON"), ParseJsonLine(Line, Ready));
					TestEqual(TEXT("the renegotiated real init is ready"),
						GetString(Ready, TEXT("type")), FString(TEXT("ready")));
					FMtoUFrameMessage FirstFrame;
					FirstFrame.Serial = 1;
					FirstFrame.Time = SampleMax != nullptr ? SampleMax->Time : 1.0;
					FMtoUFrameMessage CharacterFrame;
					CharacterFrame.Serial = 2;
					CharacterFrame.Time = SampleMin != nullptr ? SampleMin->Time : 2.0;
					for (const FMtoUNegotiatedSubject& Subject : Result.NegotiatedSubjects)
					{
						FMtoUFrameSubject FrameSubject;
						if (SampleMax == nullptr
							|| !FrameSubjectFromRecord(Subject, *SampleMax, FrameSubject))
						{
							FrameSubject.Id = Subject.Id;
							FrameSubject.Transforms = Subject.Declaration.Bind;
							FrameSubject.Curves.Init(0.0f, Subject.Declaration.Curves.Num());
						}
						FirstFrame.Subjects.Add(MoveTemp(FrameSubject));
						if (Subject.Id == FMtoUMultiSubjectFixtureBuilder::CharacterId())
						{
							FMtoUFrameSubject CharacterSubject;
							if (SampleMin == nullptr
								|| !FrameSubjectFromRecord(Subject, *SampleMin, CharacterSubject))
							{
								CharacterSubject.Id = Subject.Id;
								CharacterSubject.Transforms = Subject.Declaration.Bind;
								CharacterSubject.Curves.Init(0.0f, Subject.Declaration.Curves.Num());
							}
							CharacterFrame.Subjects.Add(MoveTemp(CharacterSubject));
						}
					}
					if (TestTrue(TEXT("the renegotiated rest frame is sent"),
							LocalPeer.Send(EncodeFrame(FirstFrame, LocalReceiver.GetSessionId()), Error))
						&& TestTrue(TEXT("the renegotiated frame is acknowledged"),
							LocalPeer.ReadLine(LocalReceiver, Line, Error)))
					{
						ExpectApplied(*this, Line, TEXT("renegotiated real frame"),
							LocalReceiver.GetSessionId(), 1, FirstFrame.Time,
							{ { FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("applied") },
								{ FMtoUMultiSubjectFixtureBuilder::PropId(), TEXT("applied") } });
					}
					// Independent removal on the real pair: only the C02 prop
					// leaves and returns to its reference pose, while the C01
					// character keeps its own preview and stream.
					if (TestTrue(TEXT("the real prop remove is sent"),
							LocalPeer.Send(EncodeRemove(
								FMtoUMultiSubjectFixtureBuilder::PropId(),
								LocalReceiver.GetSessionId()), Error))
						&& TestTrue(TEXT("the real prop remove is answered"),
							LocalPeer.ReadLine(LocalReceiver, Line, Error)))
					{
						ExpectApplied(*this, Line, TEXT("real prop remove"),
							LocalReceiver.GetSessionId(), 1, FirstFrame.Time,
							{ { FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("applied") },
								{ FMtoUMultiSubjectFixtureBuilder::PropId(), TEXT("disabled") } });
					}
					TestTrue(TEXT("the removed real prop is back at its reference pose"),
						MaxDeltaToReferencePose(*LocalPropComponent) < 1e-3);
					TestTrue(TEXT("the removed real prop leaves the preview"),
						LocalPropComponent->GetAnimInstance() == nullptr);
					if (TestTrue(TEXT("the remaining real frame is sent"),
							LocalPeer.Send(EncodeFrame(CharacterFrame, LocalReceiver.GetSessionId()), Error))
						&& TestTrue(TEXT("the remaining real frame is acknowledged"),
							LocalPeer.ReadLine(LocalReceiver, Line, Error)))
					{
						ExpectApplied(*this, Line, TEXT("real character only"),
							LocalReceiver.GetSessionId(), 2, CharacterFrame.Time,
							{ { FMtoUMultiSubjectFixtureBuilder::CharacterId(), TEXT("applied") } });
					}
					// Per-object numbers for the two lifecycle frames: the wire
					// rows the real samples carried are what the mesh holds, and
					// the two samples pose the character differently.
					const TArray<FMtoUFrameRecord>& LifecycleRecords =
						LocalReceiver.GetFrameRecords();
					TestEqual(TEXT("the two real lifecycle frames were applied"),
						LifecycleRecords.Num(), 2);
					double LifecycleMaxDelta = 0.0;
					double CharacterPoseChange = 0.0;
					if (LifecycleRecords.Num() == 2)
					{
						for (const FMtoUFrameRecord& Record : LifecycleRecords)
						{
							for (const FMtoUSubjectMeasurement& Measurement : Record.Subjects)
							{
								for (const FMtoUBoneMeasurement& Bone : Measurement.Bones)
								{
									LifecycleMaxDelta = FMath::Max(LifecycleMaxDelta, Bone.Delta);
								}
							}
						}
						const FMtoUSubjectMeasurement* FirstCharacter =
							LifecycleRecords[0].Subjects.FindByPredicate(
								[](const FMtoUSubjectMeasurement& Candidate)
								{
									return Candidate.Id == FMtoUMultiSubjectFixtureBuilder::CharacterId();
								});
						const FMtoUSubjectMeasurement* SecondCharacter =
							LifecycleRecords[1].Subjects.FindByPredicate(
								[](const FMtoUSubjectMeasurement& Candidate)
								{
									return Candidate.Id == FMtoUMultiSubjectFixtureBuilder::CharacterId();
								});
						if (FirstCharacter != nullptr && SecondCharacter != nullptr
							&& FirstCharacter->Bones.Num() == SecondCharacter->Bones.Num())
						{
							for (int32 Index = 0; Index < FirstCharacter->Bones.Num(); ++Index)
							{
								CharacterPoseChange = FMath::Max(CharacterPoseChange,
									MtoUSubjectTransformDelta(
										FirstCharacter->Bones[Index].ComponentSpace,
										SecondCharacter->Bones[Index].ComponentSpace));
							}
						}
					}
					AddInfo(FString::Printf(
						TEXT("real lifecycle deltas: max=%.6f character_pose_change=%.6f"),
						LifecycleMaxDelta, CharacterPoseChange));
					TestTrue(TEXT("the lifecycle frames hold the projected pose"),
						LifecycleMaxDelta < 0.01);
					TestTrue(TEXT("the two real samples pose the C01 character differently"),
						CharacterPoseChange > 1e-4);
				}
			}
			else
			{
				AddError(Error);
			}
			// The disconnect ends the session, and every real target returns to
			// its reference pose on the way out.
			LocalPeer.Close();
			const double LocalDisconnectDeadline = FPlatformTime::Seconds() + 1.0;
			while (LocalReceiver.HasClient()
				&& FPlatformTime::Seconds() < LocalDisconnectDeadline)
			{
				LocalReceiver.Pump(0.0);
				FPlatformProcess::Sleep(0.001f);
			}
			TestFalse(TEXT("the real disconnect ends the session"), LocalReceiver.HasSession());
			TestEqual(TEXT("the real disconnect is the recorded reason"),
				LocalReceiver.GetLastSessionEndReason(), FString(TEXT("disconnect")));
			TestTrue(TEXT("the C02 prop left at its reference pose"),
				MaxDeltaToReferencePose(*LocalPropComponent) < 1e-3);
			TestTrue(TEXT("the C01 character left at its reference pose"),
				MaxDeltaToReferencePose(*LocalCharacterComponent) < 1e-3);
			{
				const FString LifecycleDirectory = FPaths::Combine(Directory, TEXT("lifecycle"));
				FString LifecycleEvidencePath;
				TestTrue(TEXT("the real lifecycle evidence is written"),
					LocalReceiver.SaveEvidence(LifecycleDirectory, LifecycleEvidencePath, Error));
				AddInfo(FString::Printf(TEXT("real lifecycle evidence: %s"),
					*LifecycleEvidencePath));
			}
			LocalReceiver.Stop(TEXT("local renegotiation finished"));
		}
	}

	// BaseColor viewport evidence: the real meshes in a disposable level with
	// the material's own colours, driven by a held preview frame. The session is
	// kept alive past the test body (this run is one disposable editor process)
	// so the renderer still sees the driven pose when it writes the image, and
	// the request is verified by a latent command on a later frame.
	if (!FParse::Param(FCommandLine::Get(), TEXT("NullRHI")) && GEditor != nullptr)
	{
		static TUniquePtr<FMtoUMultiSubjectReceiver> ScreenshotReceiver;
		FMtoUMultiSubjectSessionConfig ScreenshotConfig;
		ScreenshotConfig.Port = ReserveLoopbackPort();
		ScreenshotConfig.Scenario = Scenario + TEXT("-viewport");
		AActor* ScreenshotCharacterAnchor = nullptr;
		AActor* ScreenshotPropAnchor = nullptr;
		USkeletalMeshComponent* ScreenshotCharacterComponent = nullptr;
		USkeletalMeshComponent* ScreenshotPropComponent = nullptr;
		if (ScreenshotReceiver.IsValid())
		{
			ScreenshotReceiver->Stop(TEXT("previous viewport run"));
			ScreenshotReceiver.Reset();
		}
		ScreenshotReceiver = MakeUnique<FMtoUMultiSubjectReceiver>();
		const TArray<FMtoUTargetRegistration> ScreenshotRegistrations = {
			{ FMtoUMultiSubjectFixtureBuilder::CharacterId(), CharacterAnchor, CharacterComponent },
			{ FMtoUMultiSubjectFixtureBuilder::PropId(), PropAnchor, PropComponent } };
		TestTrue(TEXT("the viewport session starts on the real targets"),
			ScreenshotReceiver->Start(*World, ScreenshotRegistrations, ScreenshotConfig, Error));
		if (ScreenshotReceiver->IsRunning()
			&& TestNotNull(TEXT("the character declaration was captured for the viewport hold"),
				Result.NegotiatedSubjects.FindByPredicate(
					[](const FMtoUNegotiatedSubject& Subject)
					{
						return Subject.Id == FMtoUMultiSubjectFixtureBuilder::CharacterId();
					}))
			&& TestNotNull(TEXT("the prop declaration was captured for the viewport hold"),
				Result.NegotiatedSubjects.FindByPredicate(
					[](const FMtoUNegotiatedSubject& Subject)
					{
						return Subject.Id == FMtoUMultiSubjectFixtureBuilder::PropId();
					})))
		{
			// The held frame is one real Maya sample of this run (the C01
			// animation at its sampled time and the C02 pose), rebuilt from the
			// recorded wire rows: the image shows a sampled frame, and the
			// numbers that produced it are the per-object evidence above.
			FMtoUInitMessage HoldInit;
			HoldInit.Version = MtoUMultiSubjectProtocol::Version;
			HoldInit.Fps = 30.0;
			FMtoUFrameMessage HoldFrame;
			HoldFrame.Serial = 1;
			HoldFrame.Time = SampleMax != nullptr ? SampleMax->Time : 1.0;
			for (const FMtoUNegotiatedSubject& Subject : Result.NegotiatedSubjects)
			{
				HoldInit.Subjects.Add(Subject.Declaration);
				FMtoUFrameSubject FrameSubject;
				if (SampleMax == nullptr
					|| !FrameSubjectFromRecord(Subject, *SampleMax, FrameSubject))
				{
					FrameSubject.Id = Subject.Id;
					FrameSubject.Transforms = Subject.Declaration.Bind;
					FrameSubject.Curves.Init(0.0f, Subject.Declaration.Curves.Num());
				}
				HoldFrame.Subjects.Add(MoveTemp(FrameSubject));
			}
			ScreenshotReceiver->HandleClientLine(EncodeInit(HoldInit).TrimEnd());
			const int64 HoldSession = ScreenshotReceiver->GetSessionId();
			ScreenshotReceiver->HandleClientLine(EncodeFrame(HoldFrame, HoldSession).TrimEnd());
			TestTrue(TEXT("the viewport hold frame was applied"),
				ScreenshotReceiver->GetAppliedFrameCount() > 0);
		}
		// One capture step: fit the camera to the pair's world bounds, check
		// both targets fit the view cone completely, and request the image. The
		// bounds are read once, before either capture, so the reposition offset
		// is applied exactly once even if the component bounds go stale.
		const FString CapturePath =
			FPaths::Combine(Directory, TEXT("basecolor-") + Scenario + TEXT(".png"));
		const FString SeparatedPath =
			FPaths::Combine(Directory, TEXT("basecolor-") + Scenario + TEXT("-separated.png"));
		const FVector PairSeparation(0.0, 260.0, 0.0);
		const FBoxSphereBounds CharacterBounds = CharacterComponent->Bounds;
		const FBoxSphereBounds PropBoundsBase = PropComponent->Bounds;
		const auto CaptureStep = [this](
			const FString& Path, const FString& Label,
			const FBoxSphereBounds& InCharacterBounds,
			const FBoxSphereBounds& InPropBounds,
			FVector PropOffset)
		{
			FBoxSphereBounds PropBounds = InPropBounds;
			PropBounds.Origin += PropOffset;
			const FBoxSphereBounds PairBounds = InCharacterBounds + PropBounds;
			const FVector Center = PairBounds.Origin;
			const double Radius = FMath::Max<double>(PairBounds.SphereRadius, 10.0);
			const FVector Eye = Center + FVector(-Radius * 1.7, -Radius * 1.7, Radius * 0.85);
			AddInfo(FString::Printf(
				TEXT("BaseColor framing%s: center %s radius %.1f eye %s"),
				*Label, *Center.ToCompactString(), Radius, *Eye.ToCompactString()));
			for (FLevelEditorViewportClient* View : GEditor->GetLevelViewportClients())
			{
				if (View == nullptr || !View->IsPerspective())
				{
					continue;
				}
				View->SetViewLocation(Eye);
				View->SetViewRotation((Center - Eye).Rotation());
				View->SetRealtime(true);
				View->ChangeBufferVisualizationMode(FName(TEXT("BaseColor")));
				View->Invalidate();
				const FVector Forward = (Center - Eye).GetSafeNormal();
				const double HalfFov = FMath::DegreesToRadians(
					FMath::Clamp<double>(View->ViewFOV, 30.0, 120.0) * 0.5);
				for (const FBoxSphereBounds& Bounds : { InCharacterBounds, PropBounds })
				{
					const FVector ToCenter = Bounds.Origin - Eye;
					const double Distance = FMath::Max<double>(ToCenter.Size(), 1.0);
					const double Angle = FMath::Acos(FMath::Clamp(
						FVector::DotProduct(Forward, ToCenter.GetSafeNormal()), -1.0, 1.0));
					const double SphereAngle = FMath::Asin(FMath::Clamp(
						Bounds.SphereRadius / Distance, -1.0, 1.0));
					TestTrue(FString::Printf(
							TEXT("a real target fits completely in the BaseColor framing%s"), *Label),
						Angle + SphereAngle <= HalfFov);
				}
			}
			FScreenshotRequest::RequestScreenshot(Path, false, false);
			AddInfo(FString::Printf(TEXT("BaseColor screenshot requested: %s"), *Path));
		};
		CaptureStep(CapturePath, FString(), CharacterBounds, PropBoundsBase, FVector::ZeroVector);
		ADD_LATENT_AUTOMATION_COMMAND(FMtoUWaitForScreenshot(
			this, CapturePath, FPlatformTime::Seconds() + 60.0));
		// The supplied scene references both rigs at the world origin, so the two
		// targets coincide exactly as they do in Maya. A second capture moves the
		// disposable prop anchor along one axis - a documented legibility
		// reposition of the throwaway target only - so the evidence also shows
		// two complete characters side by side. The poses stay the real sampled
		// frame and every per-object number above is unchanged.
		ADD_LATENT_AUTOMATION_COMMAND(FMtoURunCaptureStep(
			[this, CaptureStep, SeparatedPath, CharacterBounds, PropBoundsBase,
				PropAnchor, PropComponent, PairSeparation]()
			{
				PropAnchor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
				const FVector MovedLocation =
					PropAnchor->GetActorLocation() + PairSeparation;
				PropAnchor->SetActorLocation(
					MovedLocation, false, nullptr, ETeleportType::TeleportPhysics);
				TestTrue(TEXT("the documented reposition moved the disposable prop target"),
					(PropComponent->GetComponentLocation() - MovedLocation).Size() < 1.0);
				CaptureStep(SeparatedPath, TEXT(" (separated pair)"),
					CharacterBounds, PropBoundsBase, PairSeparation);
			}));
		ADD_LATENT_AUTOMATION_COMMAND(FMtoUWaitForScreenshot(
			this, SeparatedPath, FPlatformTime::Seconds() + 90.0));
	}

	// The production assets must be exactly as they were: source files unchanged,
	// packages not dirtied by the run.
	TestEqual(TEXT("the real character mesh file is unchanged"),
		FileDigest(CharacterAssetFile), CharacterDigestBefore);
	TestEqual(TEXT("the real prop mesh file is unchanged"), FileDigest(PropAssetFile), PropDigestBefore);
	TestEqual(TEXT("the supplied Maya scene file is unchanged"), FileDigest(ScenePath), SceneDigestBefore);
	TestEqual(TEXT("the character mesh package is not dirtied by the run"),
		CharacterMesh->GetOutermost()->IsDirty(), bCharacterDirtyBefore);
	TestEqual(TEXT("the prop mesh package is not dirtied by the run"),
		PropMesh->GetOutermost()->IsDirty(), bPropDirtyBefore);

	// The receiver's machine-readable evidence for the real pair.
	{
		FString EvidencePath;
		TestTrue(TEXT("the real-pair receiver evidence is written"),
			Receiver.SaveEvidence(Directory, EvidencePath, Error));
		TSharedPtr<FJsonObject> Evidence;
		FString EvidenceText;
		if (TestTrue(TEXT("the real-pair evidence reads back"),
				FFileHelper::LoadFileToString(EvidenceText, *EvidencePath)
					&& ParseJsonLine(EvidenceText, Evidence)))
		{
			TestEqual(TEXT("the real-pair evidence records the applied frames"),
				GetNumber(Evidence, TEXT("applied_frames")),
				static_cast<double>(Receiver.GetAppliedFrameCount()));
			TestFalse(TEXT("the real-pair evidence has no preview left"),
				GetBool(Evidence, TEXT("preview_active")));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// The peer launch contract: a comma-list argument must not swallow the tokens
// that follow it, and only validated time values reach the Maya argv.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectCommandLineTest,
	"MtoUMultiSubjectPrototype.CommandLineArguments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectCommandLineTest::RunTest(const FString& Parameters)
{
	const TCHAR* const TimesName = TEXT("-MtoUMultiSubjectTimes=");
	FString Value;

	// The value ends at the next token, wherever the times argument sits.
	TestTrue(TEXT("a trailing argument is not swallowed"),
		FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
			TEXT("-MtoUMultiSubjectTimes=1,3,2,2 -abslog=logs/run.log"),
			TimesName, Value)
			&& Value == TEXT("1,3,2,2"));
	TestTrue(TEXT("an argument in the middle only takes its own value"),
		FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
			TEXT("-MtoUEvidence=out -MtoUMultiSubjectTimes=1,3,2,2 -MtoUMultiSubjectFrames=4"),
			TimesName, Value)
			&& Value == TEXT("1,3,2,2"));
	TestTrue(TEXT("a leading argument only takes its own value"),
		FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
			TEXT("-MtoUMultiSubjectTimes=1,3,2,2"),
			TimesName, Value)
			&& Value == TEXT("1,3,2,2"));
	TestTrue(TEXT("a quoted list is read whole"),
		FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
			TEXT("-MtoUMultiSubjectTimes=\"1,3,2,2\" -abslog=x.log"),
			TimesName, Value)
			&& Value == TEXT("1,3,2,2"));
	TestFalse(TEXT("a name inside another token does not match"),
		FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
			TEXT("x-MtoUMultiSubjectTimes=9,9"), TimesName, Value));
	TestFalse(TEXT("an absent argument reports absence"),
		FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
			TEXT("-MtoUEvidence=out -abslog=x.log"), TimesName, Value));

	TArray<double> Times;
	TestTrue(TEXT("a comma list parses into its values"),
		FMtoUMultiSubjectPeerRequest::ParseTimeList(TEXT("1,3,2,2"), Times)
			&& Times.Num() == 4
			&& Times[0] == 1.0 && Times[1] == 3.0 && Times[2] == 2.0 && Times[3] == 2.0);
	TestTrue(TEXT("spaces around entries are tolerated"),
		FMtoUMultiSubjectPeerRequest::ParseTimeList(TEXT(" 1 , 2.5 "), Times) && Times.Num() == 2);
	TestFalse(TEXT("an empty entry is refused"),
		FMtoUMultiSubjectPeerRequest::ParseTimeList(TEXT("1,,2"), Times));
	TestFalse(TEXT("a non-numeric entry is refused"),
		FMtoUMultiSubjectPeerRequest::ParseTimeList(TEXT("1,3,oops"), Times));
	TestFalse(TEXT("an empty list is refused"),
		FMtoUMultiSubjectPeerRequest::ParseTimeList(TEXT(""), Times));

	// The exact peer command line carries the validated values and none of the
	// host-side arguments that followed them.
	FMtoUMultiSubjectPeerRequest Request;
	Request.PeerScriptPath = TEXT("maya/maya_peer.py");
	Request.EvidencePath = TEXT("out/maya.json");
	Request.Times = { 1.0, 3.0, 2.0, 2.0 };
	const FString PeerCommandLine = Request.BuildCommandLine();
	TestTrue(TEXT("the peer command line carries the four times"),
		PeerCommandLine.Contains(TEXT("--times 1,3,2,2")));
	TestFalse(TEXT("no host-side argument reaches the Maya command line"),
		PeerCommandLine.Contains(TEXT("abslog")));
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
	// An explicit time list is how a reverse scrub or a same-frame re-edit is
	// exercised end to end. It is read with the peer's own token-boundary
	// parser and re-encoded from validated numbers, so a later UE argument
	// (`-abslog=...` and the like) can never reach the Maya argv.
	{
		FString TimesValue;
		if (FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
				FCommandLine::Get(), TEXT("-MtoUMultiSubjectTimes="), TimesValue))
		{
			if (!TestTrue(TEXT("the times argument is a comma-separated list of finite numbers"),
					FMtoUMultiSubjectPeerRequest::ParseTimeList(TimesValue, Request.Times)))
			{
				AddError(FString::Printf(
					TEXT("the times argument '%s' is not a comma-separated list of finite numbers"),
					*TimesValue));
				return false;
			}
		}
	}

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

// ---------------------------------------------------------------------------
// The mapping is decided by the complete candidate relation and its unique
// assignment, never by the order the declaration lists its bones in: a renamed
// target two sources can drive is refused in both orders, a uniquely assignable
// rename resolves identically in both orders, and an exact name always wins
// over a rename. Both subject targets negotiate the contested shape the same
// way, so the two subjects of the dual-subject adapter cannot decide by
// different rules.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectRenameContractTest,
	"MtoUMultiSubjectPrototype.RenameCandidateContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectRenameContractTest::RunTest(const FString& Parameters)
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

	// The review's counterexample: two same-parent sources, `Joint` and
	// `Joint1`, both shaped like importer renames of the weighted target bone
	// `Joint12`. Both see the complete candidate, so the two sources compete
	// for one target and the negotiation refuses in either order instead of
	// routing whichever source the declaration listed first to the target.
	const auto ContestedRefusal = [](
		FAutomationTestBase& Test,
		FMtoUMultiSubjectTarget& Target,
		const FMtoUSubjectDeclaration& Base,
		int32 RenamedBoneIndex,
		const TCHAR* TargetName,
		const TCHAR* Label)
	{
		for (int32 Order = 0; Order < 2; ++Order)
		{
			FMtoUSubjectDeclaration Contested = Base;
			Contested.Bones[RenamedBoneIndex].Name =
				FName(Order == 0 ? TEXT("Joint") : TEXT("Joint1"));
			FMtoUBoneDeclaration Other = Contested.Bones[RenamedBoneIndex];
			Other.Name = FName(Order == 0 ? TEXT("Joint1") : TEXT("Joint"));
			Contested.Bones.Add(Other);
			Contested.Bind.Add(Base.Bind[RenamedBoneIndex]);
			FMtoUNegotiationMap Map;
			const FString Refusal = Target.DescribeDeclarationMismatch(Contested, Map);
			Test.TestFalse(FString::Printf(TEXT("%s: order %d is refused"), Label, Order),
				Refusal.IsEmpty());
			Test.TestTrue(FString::Printf(TEXT("%s: order %d reports the two-source ambiguity"),
					Label, Order),
				Refusal.Contains(TEXT("ambiguous")) && Refusal.Contains(TargetName)
					&& Refusal.Contains(TEXT("Joint")));
		}
	};

	{
		FReferenceSkeletonModifier Rename(
			Fixture.CharacterMesh->GetRefSkeleton(), Fixture.CharacterSkeleton);
		Rename.Rename(FName(TEXT("Head")), FName(TEXT("Joint12")));
	}
	FMtoUMultiSubjectTarget CharacterTarget;
	if (!TestTrue(TEXT("the character target passes the anchor preflight"),
			CharacterTarget.Initialize(
				{ FMtoUMultiSubjectFixtureBuilder::CharacterId(), Fixture.CharacterAnchor, Fixture.CharacterComponent },
				Error)))
	{
		AddError(Error);
		return false;
	}
	ContestedRefusal(*this, CharacterTarget, Fixture.CharacterDeclaration, 3,
		TEXT("Joint12"), TEXT("character"));

	// The same contested shape on the second subject target: the dual-subject
	// adapter has one negotiation, so both subjects refuse identically.
	{
		FReferenceSkeletonModifier Rename(
			Fixture.PropMesh->GetRefSkeleton(), Fixture.PropSkeleton);
		Rename.Rename(
			FName(TEXT("PropTip_0123456789abcdef0123456789abcdef")), FName(TEXT("Joint12")));
	}
	FMtoUSubjectDeclaration PropContested = Fixture.PropDeclaration;
	PropContested.Bones[2].Name = FName(TEXT("Joint"));
	PropContested.Bones[3].Parent = 1;   // the renamed leaf is no longer its parent
	FMtoUMultiSubjectTarget PropTarget;
	if (!TestTrue(TEXT("the prop target passes the anchor preflight"),
			PropTarget.Initialize(
				{ FMtoUMultiSubjectFixtureBuilder::PropId(), Fixture.PropAnchor, Fixture.PropComponent },
				Error)))
	{
		AddError(Error);
		return false;
	}
	ContestedRefusal(*this, PropTarget, PropContested, 2,
		TEXT("Joint12"), TEXT("prop"));

	// A uniquely assignable rename resolves the same way in both sibling
	// orders: `joint1` can only become `joint11`, which leaves `joint2` for
	// `joint`, and the two swapped captures must agree.
	FMtoUMultiSubjectFixture ArmsFixture;
	ON_SCOPE_EXIT
	{
		ArmsFixture.Destroy();
	};
	if (!TestTrue(TEXT("the character-arms fixture builds"),
			FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-arms"), ArmsFixture, Error)))
	{
		AddError(Error);
		return false;
	}
	{
		FReferenceSkeletonModifier Rename(
			ArmsFixture.ArmsMesh->GetRefSkeleton(), ArmsFixture.ArmsSkeleton);
		Rename.Rename(FName(TEXT("UpperArm_L")), FName(TEXT("joint11")));
		Rename.Rename(FName(TEXT("UpperArm_R")), FName(TEXT("joint2")));
	}
	FMtoUMultiSubjectTarget ArmsTarget;
	if (!TestTrue(TEXT("the arms target passes the anchor preflight"),
			ArmsTarget.Initialize(
				{ FMtoUMultiSubjectFixtureBuilder::ArmsId(), ArmsFixture.ArmsAnchor, ArmsFixture.ArmsComponent },
				Error)))
	{
		AddError(Error);
		return false;
	}
	const FMtoUSubjectDeclaration& ArmsBase = ArmsFixture.ArmsDeclaration;
	FMtoUSubjectDeclaration Chain;
	Chain.Root = ArmsBase.Root;
	Chain.Curves = ArmsBase.Curves;
	Chain.Bones.Add({ FName(TEXT("ArmsRoot")), INDEX_NONE });
	Chain.Bind.Add(ArmsBase.Bind[0]);
	Chain.Bones.Add({ FName(TEXT("joint")), 0 });
	Chain.Bind.Add(ArmsBase.Bind[4]);
	Chain.Bones.Add({ FName(TEXT("joint1")), 0 });
	Chain.Bind.Add(ArmsBase.Bind[1]);
	TArray<FString> Mappings[2];
	for (int32 Order = 0; Order < 2; ++Order)
	{
		FMtoUSubjectDeclaration Ordered = Chain;
		if (Order == 1)
		{
			Swap(Ordered.Bones[1], Ordered.Bones[2]);
			Swap(Ordered.Bind[1], Ordered.Bind[2]);
			Ordered.Bones[1].Parent = 0;
			Ordered.Bones[2].Parent = 0;
		}
		FMtoUNegotiationMap Map;
		const FString Error2 = ArmsTarget.DescribeDeclarationMismatch(Ordered, Map);
		TestTrue(FString::Printf(TEXT("the unique assignment is accepted in order %d"), Order),
			Error2.IsEmpty());
		TestEqual(FString::Printf(TEXT("both rename reports appear in order %d"), Order),
			Map.ImportRenames.Num(), 2);
		Mappings[Order] = Map.ImportRenames;
		Mappings[Order].Sort();
	}
	TestTrue(TEXT("both sibling orders produce the same mapping"),
		Mappings[0].Num() == 2 && Mappings[0] == Mappings[1]
			&& Mappings[0].Contains(FString(TEXT("joint -> joint2")))
			&& Mappings[0].Contains(FString(TEXT("joint1 -> joint11"))));

	// Two indistinguishable sources for both renamed targets cover them in two
	// ways, so every sibling order refuses instead of guessing.
	for (int32 Order = 0; Order < 2; ++Order)
	{
		FMtoUSubjectDeclaration Ambiguous = Chain;
		Ambiguous.Bones[2].Name = FName(TEXT("joint"));
		if (Order == 1)
		{
			Swap(Ambiguous.Bones[1], Ambiguous.Bones[2]);
			Swap(Ambiguous.Bind[1], Ambiguous.Bind[2]);
			Ambiguous.Bones[1].Parent = 0;
			Ambiguous.Bones[2].Parent = 0;
		}
		FMtoUNegotiationMap Map;
		const FString Refusal = ArmsTarget.DescribeDeclarationMismatch(Ambiguous, Map);
		TestFalse(FString::Printf(TEXT("two indistinguishable sources are refused in order %d"), Order),
			Refusal.IsEmpty());
		TestTrue(FString::Printf(TEXT("order %d reports the competing sources"), Order),
			Refusal.Contains(TEXT("ambiguous")) && Refusal.Contains(TEXT("joint")));
	}

	// An exact name always wins over a rename: the target's own hash name takes
	// the pairing, and the rename-shaped sibling stays an ignored export branch
	// instead of displacing it.
	FMtoUSubjectDeclaration ExactWins = PropContested;
	ExactWins.Bones[2].Name = FName(TEXT("Joint"));
	FMtoUBoneDeclaration Exact;
	Exact.Name = FName(TEXT("Joint12"));
	Exact.Parent = 1;
	ExactWins.Bones.Add(Exact);
	ExactWins.Bind.Add(Fixture.PropDeclaration.Bind[2]);
	FMtoUNegotiationMap ExactMap;
	TestTrue(TEXT("the exact name wins the target the rename-shaped sibling also matches"),
		PropTarget.DescribeDeclarationMismatch(ExactWins, ExactMap).IsEmpty());
	TestTrue(TEXT("the exact pairing is used, without a rename"),
		ExactMap.ImportRenames.IsEmpty()
			&& ExactMap.SourceToTarget[4]
				== Fixture.PropMesh->GetRefSkeleton().FindBoneIndex(FName(TEXT("Joint12"))));
	TestTrue(TEXT("the rename-shaped sibling is reported as an export branch"),
		ExactMap.SourceToTarget[2] == INDEX_NONE && ExactMap.SourceOnlyBones == 2);
	return true;
}

// ---------------------------------------------------------------------------
// A bind difference on a bone that deforms nothing is not a different rig: the
// necessary set is what the two rigs have to agree on, so the non-essential
// branch is accepted and reported while the same difference on a necessary
// bone is still refused.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUMultiSubjectNonEssentialBindTest,
	"MtoUMultiSubjectPrototype.NonEssentialBindDifferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMultiSubjectNonEssentialBindTest::RunTest(const FString& Parameters)
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
	FMtoUMultiSubjectTarget PropTarget;
	if (!TestTrue(TEXT("the prop target passes the anchor preflight"),
			PropTarget.Initialize(
				{ FMtoUMultiSubjectFixtureBuilder::PropId(), Fixture.PropAnchor, Fixture.PropComponent },
				Error)))
	{
		AddError(Error);
		return false;
	}

	// `PropTipEnd` carries no skin weight: the same-name bind difference is a
	// non-essential disagreement and must not veto the declaration.
	const FTransform WrongBind(FQuat(FRotator(25.0, -40.0, 70.0)), FVector(0.0, 0.0, 120.0));
	FMtoUSubjectDeclaration NonEssential = Fixture.PropDeclaration;
	NonEssential.Bind[3] = WrongBind;
	FMtoUNegotiationMap NonEssentialMap;
	TestTrue(TEXT("a non-essential same-name bind difference is accepted"),
		PropTarget.DescribeDeclarationMismatch(NonEssential, NonEssentialMap).IsEmpty());
	TestTrue(TEXT("the non-essential deviation is not part of the rig-agreement check"),
		NonEssentialMap.MaxRestTranslationCm < 1.0 && NonEssentialMap.MaxRestRotationDegrees < 1.0);
	TestEqual(TEXT("the non-essential branch is still driven"),
		NonEssentialMap.DrivenTargetBones.Num(), 4);

	// The same difference on `PropBody`, which deforms the mesh, is a different
	// rig and stays refused.
	FMtoUSubjectDeclaration Essential = Fixture.PropDeclaration;
	Essential.Bind[1] = WrongBind;
	FMtoUNegotiationMap EssentialMap;
	const FString Refusal = PropTarget.DescribeDeclarationMismatch(Essential, EssentialMap);
	TestTrue(TEXT("the same difference on a necessary bone is refused"),
		!Refusal.IsEmpty() && Refusal.Contains(TEXT("bind")) && Refusal.Contains(TEXT("PropBody")));
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
