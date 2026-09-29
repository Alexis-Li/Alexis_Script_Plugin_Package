// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "Containers/Ticker.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MtoUMultiSubjectFixture.h"
#include "MtoUMultiSubjectPeer.h"
#include "MtoUMultiSubjectReceiver.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"

namespace
{
	const TCHAR* LogCategory = TEXT("MtoUMultiSubject");

	/** One manual run: the fixture world plus the receiver driving it. */
	struct FMtoUMultiSubjectRun
	{
		TUniquePtr<FMtoUMultiSubjectFixture> Fixture;
		TUniquePtr<FMtoUMultiSubjectReceiver> Receiver;
		FTSTicker::FDelegateHandle TickerHandle;
		FString Scenario;
		FString EvidenceDirectory;

		void StopTicker()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
		}

		void Shutdown(const FString& Reason)
		{
			if (Receiver.IsValid() && Receiver->IsRunning())
			{
				Receiver->Stop(Reason);
				FString EvidenceError;
				FString EvidencePath;
				if (!EvidenceDirectory.IsEmpty()
					&& Receiver->SaveEvidence(EvidenceDirectory, EvidencePath, EvidenceError))
				{
					UE_LOG(LogTemp, Display, TEXT("%s: evidence written to %s"), LogCategory, *EvidencePath);
				}
				else if (!EvidenceError.IsEmpty())
				{
					UE_LOG(LogTemp, Warning, TEXT("%s: evidence not written: %s"), LogCategory, *EvidenceError);
				}
			}
			StopTicker();
			Receiver.Reset();
			if (Fixture.IsValid())
			{
				Fixture->Destroy();
				Fixture.Reset();
			}
		}
	};

	TUniquePtr<FMtoUMultiSubjectRun> GRun;

	FMtoUMultiSubjectRun& GetRun()
	{
		if (!GRun.IsValid())
		{
			GRun = MakeUnique<FMtoUMultiSubjectRun>();
		}
		return *GRun;
	}

	FString DefaultEvidenceDirectory(const FString& Scenario)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MtoUMultiSubject"),
			Scenario.IsEmpty() ? TEXT("session") : Scenario);
	}

	bool ParseScenario(const TArray<FString>& Args, FString& OutScenario)
	{
		OutScenario = TEXT("character-prop");
		bool bFound = false;
		for (const FString& Arg : Args)
		{
			if (Arg.StartsWith(TEXT("scenario=")))
			{
				OutScenario = Arg.RightChop(9);
				bFound = true;
			}
		}
		(void)bFound;
		return FMtoUMultiSubjectFixtureBuilder::IsKnownScenario(OutScenario);
	}

	int32 ParseIntArgument(const TArray<FString>& Args, const TCHAR* Name, int32 Fallback)
	{
		for (const FString& Arg : Args)
		{
			if (Arg.StartsWith(FString(Name) + TEXT("=")))
			{
				return FCString::Atoi(*Arg.RightChop(FString(Name).Len() + 1));
			}
		}
		return Fallback;
	}

	FString ParseStringArgument(const TArray<FString>& Args, const TCHAR* Name)
	{
		for (const FString& Arg : Args)
		{
			if (Arg.StartsWith(FString(Name) + TEXT("=")))
			{
				return Arg.RightChop(FString(Name).Len() + 1);
			}
		}
		return FString();
	}

	/** The sample Level Sequence track, plus the editor Sequencer when it has the sample open. */
	void CollectSamplePreviewWriters(
		const FMtoUMultiSubjectFixture& Fixture,
		TArray<FMtoUPreviewWriter>& OutWriters,
		TSharedPtr<ISequencer>& OutEditorSequencer)
	{
		OutWriters.Reset();
		FMtoUPreviewWriter Writer;
		Writer.Track = Fixture.CharacterAnimationTrack;
		Writer.TargetId = FMtoUMultiSubjectFixtureBuilder::CharacterId();
		OutWriters.Add(Writer);
		if (GEditor == nullptr)
		{
			return;
		}
		IAssetEditorInstance* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
			->FindEditorForAsset(Fixture.Sequence, false);
		if (AssetEditor != nullptr && AssetEditor->GetEditorName() == FName(TEXT("LevelSequenceEditor")))
		{
			OutEditorSequencer = static_cast<ILevelSequenceEditorToolkit*>(AssetEditor)->GetSequencer();
		}
	}

	bool StartRun(
		const FString& Scenario,
		uint16 Port,
		const FString& EvidenceDirectory,
		FString& OutError)
	{
		FMtoUMultiSubjectRun& Run = GetRun();
		Run.Shutdown(TEXT("restarted"));
		Run.Scenario = Scenario;
		Run.EvidenceDirectory = EvidenceDirectory;

		Run.Fixture = MakeUnique<FMtoUMultiSubjectFixture>();
		if (!FMtoUMultiSubjectFixtureBuilder::Build(Scenario, *Run.Fixture, OutError))
		{
			Run.Fixture.Reset();
			return false;
		}
		FMtoUMultiSubjectSessionConfig Config;
		Config.Port = Port;
		Config.Scenario = Scenario;
		CollectSamplePreviewWriters(*Run.Fixture, Config.PreviewWriters, Config.EditorSequencer);

		Run.Receiver = MakeUnique<FMtoUMultiSubjectReceiver>();
		if (!Run.Receiver->Start(*Run.Fixture->World,
				Run.Fixture->MakeRegistrations(FMtoUMultiSubjectFixtureBuilder::ScenarioSubjects(Scenario)),
				Config, OutError))
		{
			Run.Receiver.Reset();
			Run.Fixture->Destroy();
			Run.Fixture.Reset();
			return false;
		}
		Run.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float DeltaSeconds)
			{
				if (GRun.IsValid() && GRun->Receiver.IsValid())
				{
					GRun->Receiver->Pump(DeltaSeconds);
				}
				return true;
			}));
		UE_LOG(LogTemp, Display, TEXT("%s: run started, scenario=%s port=%u evidence=%s"),
			LogCategory, *Scenario, Run.Receiver->GetBoundPort(), *EvidenceDirectory);
		return true;
	}
}

/**
 * Editor entry point: the console commands an operator or an evidence run uses.
 * The Automation tests drive the fixture, the receiver and the Maya peer directly.
 */
class FMtoUMultiSubjectPrototypeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterCommands();
	void UnregisterCommands();

	TArray<IConsoleObject*> Commands;
};

void FMtoUMultiSubjectPrototypeModule::StartupModule()
{
	RegisterCommands();
}

void FMtoUMultiSubjectPrototypeModule::ShutdownModule()
{
	if (GRun.IsValid())
	{
		GRun->Shutdown(TEXT("module shutdown"));
		GRun.Reset();
	}
	UnregisterCommands();
}

void FMtoUMultiSubjectPrototypeModule::RegisterCommands()
{
	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MtoUMultiSubject.Listen"),
		TEXT("Builds the sample and listens for one Maya client. "
			"Args: [scenario=character-prop|character-arms] [port=<n>] [out=<dir>]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FString Scenario;
			if (!ParseScenario(Args, Scenario))
			{
				UE_LOG(LogTemp, Error, TEXT("%s: unknown scenario; use character-prop or character-arms"), LogCategory);
				return;
			}
			const int32 Port = ParseIntArgument(Args, TEXT("port"), 54340);
			const FString OutDirectory = ParseStringArgument(Args, TEXT("out")).IsEmpty()
				? DefaultEvidenceDirectory(Scenario)
				: ParseStringArgument(Args, TEXT("out"));
			FString Error;
			if (!StartRun(Scenario, static_cast<uint16>(Port), OutDirectory, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("%s: %s"), LogCategory, *Error);
			}
		})));

	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MtoUMultiSubject.Stop"),
		TEXT("Ends the session, restores every target, and writes the evidence file."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (!GRun.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("%s: no run is active"), LogCategory);
				return;
			}
			GRun->Shutdown(TEXT("requested"));
		})));

	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MtoUMultiSubject.Evidence"),
		TEXT("Writes the evidence of the active run. Args: [out=<dir>]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!GRun.IsValid() || !GRun->Receiver.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("%s: no run is active"), LogCategory);
				return;
			}
			const FString OutDirectory = ParseStringArgument(Args, TEXT("out")).IsEmpty()
				? GRun->EvidenceDirectory
				: ParseStringArgument(Args, TEXT("out"));
			FString EvidencePath;
			FString Error;
			if (GRun->Receiver->SaveEvidence(OutDirectory, EvidencePath, Error))
			{
				UE_LOG(LogTemp, Display, TEXT("%s: evidence %s"), LogCategory, *EvidencePath);
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("%s: %s"), LogCategory, *Error);
			}
		})));

	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MtoUMultiSubject.Peer"),
		TEXT("Runs the Maya peer against a real receiver and reports both evidence files. "
			"Args: <mayapy> <peer script> [scenario=] [port=] [frames=] [start-frame=] "
			"[remove-at=] [drop-after=] [out=]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTemp, Error, TEXT("%s: needs <mayapy> and <peer script>"), LogCategory);
				return;
			}
			FString Scenario;
			if (!ParseScenario(Args, Scenario))
			{
				UE_LOG(LogTemp, Error, TEXT("%s: unknown scenario; use character-prop or character-arms"), LogCategory);
				return;
			}
			const FString OutDirectory = ParseStringArgument(Args, TEXT("out")).IsEmpty()
				? DefaultEvidenceDirectory(Scenario)
				: ParseStringArgument(Args, TEXT("out"));
			FString Error;
			if (!StartRun(Scenario,
					static_cast<uint16>(ParseIntArgument(Args, TEXT("port"), 54340)),
					OutDirectory, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("%s: %s"), LogCategory, *Error);
				return;
			}
			FMtoUMultiSubjectRun& Run = GetRun();
			Run.StopTicker();

			FMtoUMultiSubjectPeerRequest Request;
			Request.MayapyPath = Args[0];
			Request.PeerScriptPath = Args[1];
			Request.Scenario = Scenario;
			Request.Port = Run.Receiver->GetBoundPort();
			Request.Frames = ParseIntArgument(Args, TEXT("frames"), 24);
			Request.StartFrame = ParseIntArgument(Args, TEXT("start-frame"), 1);
			Request.RemoveAtFrame = ParseIntArgument(Args, TEXT("remove-at"), 0);
			Request.DropAfterFrames = ParseIntArgument(Args, TEXT("drop-after"), 0);
			Request.EvidencePath = FPaths::Combine(OutDirectory, TEXT("mtou-multi-subject-maya.json"));
			Request.LogPath = FPaths::Combine(OutDirectory, TEXT("mtou-multi-subject-maya.log"));

			FMtoUMultiSubjectPeerResult Result;
			if (!RunMayaMultiSubjectPeer(Request, *Run.Receiver, Result, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("%s: %s"), LogCategory, *Error);
			}
			UE_LOG(LogTemp, Display, TEXT("%s: maya peer exit=%d evidence_ok=%s frames=%d max_latency_ms=%.2f"),
				LogCategory, Result.ReturnCode,
				Result.bEvidenceOkField && Result.bEvidenceOk ? TEXT("true") : TEXT("false"),
				Result.EvidenceFrameCount, Result.MaxLatencyMs);
			UE_LOG(LogTemp, Display, TEXT("%s"), *Result.Output);

			Run.Shutdown(TEXT("peer finished"));
			UE_LOG(LogTemp, Display, TEXT("%s: receiver applied %d frames"), LogCategory,
				Result.EvidenceFrameCount);
		})));
}

void FMtoUMultiSubjectPrototypeModule::UnregisterCommands()
{
	for (IConsoleObject* Command : Commands)
	{
		if (Command != nullptr)
		{
			IConsoleManager::Get().UnregisterConsoleObject(Command);
		}
	}
	Commands.Reset();
}

IMPLEMENT_MODULE(FMtoUMultiSubjectPrototypeModule, MtoUMultiSubjectPrototypeEditor)
