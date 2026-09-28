// MtoU camera sync prototype (Issue 52 verification). Editor-only prototype code.

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MtoUCameraSyncSession.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StrongObjectPtr.h"

/**
 * Editor-side entry point of the prototype: owns one optional session and exposes the
 * console commands a user runs while Maya is connected. The automation tests drive the
 * same session object directly.
 */
class FMtoUCameraSyncPrototypeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Starts a session on the given editor world. Returns nullptr and fills OutError on failure. */
	FMtoUCameraSyncSession* StartSession(
		UWorld& World,
		ULevelSequence& Sequence,
		const FMtoUCameraSyncSession::FConfig& Config,
		FString& OutError);
	FMtoUCameraSyncSession* StartEditorSession(
		UWorld& World, const TSharedRef<ISequencer>& Sequencer,
		const FMtoUCameraSyncSession::FConfig& Config, FString& OutError);
	void StopSession(const FString& Reason);
	FMtoUCameraSyncSession* GetSession() const { return Session.Get(); }

private:
	bool Tick(float DeltaSeconds);
	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();

	TSharedPtr<FMtoUCameraSyncSession> Session;
	FTSTicker::FDelegateHandle TickerHandle;
	TArray<IConsoleObject*> ConsoleCommands;
};

void FMtoUCameraSyncPrototypeModule::StartupModule()
{
	RegisterConsoleCommands();
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FMtoUCameraSyncPrototypeModule::Tick));
}

void FMtoUCameraSyncPrototypeModule::ShutdownModule()
{
	StopSession(TEXT("module shutdown"));
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	UnregisterConsoleCommands();
}

bool FMtoUCameraSyncPrototypeModule::Tick(float DeltaSeconds)
{
	if (Session.IsValid())
	{
		Session->Pump(DeltaSeconds);
	}
	return true;
}

FMtoUCameraSyncSession* FMtoUCameraSyncPrototypeModule::StartSession(
	UWorld& World,
	ULevelSequence& Sequence,
	const FMtoUCameraSyncSession::FConfig& Config,
	FString& OutError)
{
	StopSession(TEXT("restart"));

	TSharedPtr<FMtoUCameraSyncSession> NewSession = MakeShared<FMtoUCameraSyncSession>();
	if (!NewSession->Start(World, Sequence, Config, OutError))
	{
		return nullptr;
	}
	Session = NewSession;
	return Session.Get();
}

FMtoUCameraSyncSession* FMtoUCameraSyncPrototypeModule::StartEditorSession(
	UWorld& World,
	const TSharedRef<ISequencer>& Sequencer,
	const FMtoUCameraSyncSession::FConfig& Config,
	FString& OutError)
{
	StopSession(TEXT("restart"));
	TSharedPtr<FMtoUCameraSyncSession> NewSession = MakeShared<FMtoUCameraSyncSession>();
	if (!NewSession->StartFromEditor(World, Sequencer, Config, OutError))
	{
		return nullptr;
	}
	Session = NewSession;
	return Session.Get();
}

void FMtoUCameraSyncPrototypeModule::StopSession(const FString& Reason)
{
	if (Session.IsValid())
	{
		Session->Stop(Reason);
		Session.Reset();
	}
}

namespace
{
UWorld* PrototypeEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

ULevelSequence* LoadSequenceOrReport(const TArray<FString>& Args, int32 Index, FString& OutError)
{
	if (!Args.IsValidIndex(Index))
	{
		OutError = TEXT("sequence asset path argument is required");
		return nullptr;
	}
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *Args[Index]);
	if (!Sequence)
	{
		OutError = FString::Printf(TEXT("could not load sequence '%s'"), *Args[Index]);
	}
	return Sequence;
}
}  // namespace

void FMtoUCameraSyncPrototypeModule::RegisterConsoleCommands()
{
	IConsoleManager& Console = IConsoleManager::Get();

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("MtoUCameraSyncPrototype.Start"),
		TEXT("MtoUCameraSyncPrototype.Start <SequenceAssetPath> [Port] [Width] [Height]"),
		FConsoleCommandWithArgsDelegate::CreateLambda(
			[this](const TArray<FString>& Args)
			{
				FString Error;
				ULevelSequence* Sequence = LoadSequenceOrReport(Args, 0, Error);
				UWorld* World = PrototypeEditorWorld();
				if (!Sequence || !World)
				{
					UE_LOG(LogTemp, Error, TEXT("[MtoUCameraSyncPrototype] %s"),
						World ? *Error : TEXT("no editor world available"));
					return;
				}
				FMtoUCameraSyncSession::FConfig Config;
				if (Args.IsValidIndex(1)) { Config.Port = static_cast<uint16>(FCString::Atoi(*Args[1])); }
				if (Args.IsValidIndex(2)) { Config.OutputResolution.X = FCString::Atoi(*Args[2]); }
				if (Args.IsValidIndex(3)) { Config.OutputResolution.Y = FCString::Atoi(*Args[3]); }
				if (!StartSession(*World, *Sequence, Config, Error))
				{
					UE_LOG(LogTemp, Error, TEXT("[MtoUCameraSyncPrototype] start failed: %s"), *Error);
					return;
				}
				UE_LOG(LogTemp, Display,
					TEXT("[MtoUCameraSyncPrototype] listening on 127.0.0.1:%u for sequence %s at %dx%d"),
					Config.Port, *Sequence->GetName(),
					Config.OutputResolution.X, Config.OutputResolution.Y);
			}),
		ECVF_Default));

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("MtoUCameraSyncPrototype.StartEditor"),
		TEXT("MtoUCameraSyncPrototype.StartEditor [Port] [Width] [Height] - follow the open Level Sequence editor"),
		FConsoleCommandWithArgsDelegate::CreateLambda(
			[this](const TArray<FString>& Args)
			{
				ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
				UWorld* World = PrototypeEditorWorld();
				if (!Sequence || !World || !GEditor)
				{
					UE_LOG(LogTemp, Error, TEXT("[MtoUCameraSyncPrototype] open a Level Sequence in the editor first"));
					return;
				}
				IAssetEditorInstance* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
					->FindEditorForAsset(Sequence, false);
				if (!AssetEditor || AssetEditor->GetEditorName() != FName(TEXT("LevelSequenceEditor")))
				{
					UE_LOG(LogTemp, Error, TEXT("[MtoUCameraSyncPrototype] the current sequence has no open editor toolkit"));
					return;
				}
				const TSharedPtr<ISequencer> Sequencer =
					static_cast<ILevelSequenceEditorToolkit*>(AssetEditor)->GetSequencer();
				if (!Sequencer.IsValid())
				{
					UE_LOG(LogTemp, Error, TEXT("[MtoUCameraSyncPrototype] Sequencer is unavailable"));
					return;
				}
				FMtoUCameraSyncSession::FConfig Config;
				if (Args.IsValidIndex(0)) { Config.Port = static_cast<uint16>(FCString::Atoi(*Args[0])); }
				if (Args.IsValidIndex(1)) { Config.OutputResolution.X = FCString::Atoi(*Args[1]); }
				if (Args.IsValidIndex(2)) { Config.OutputResolution.Y = FCString::Atoi(*Args[2]); }
				FString Error;
				if (!StartEditorSession(*World, Sequencer.ToSharedRef(), Config, Error))
				{
					UE_LOG(LogTemp, Error, TEXT("[MtoUCameraSyncPrototype] editor start failed: %s"), *Error);
					return;
				}
				UE_LOG(LogTemp, Display, TEXT("[MtoUCameraSyncPrototype] following editor Sequencer %s on 127.0.0.1:%u"),
					*Sequence->GetName(), Config.Port);
			}), ECVF_Default));

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("MtoUCameraSyncPrototype.Stop"),
		TEXT("MtoUCameraSyncPrototype.Stop - ends the prototype session"),
		FConsoleCommandDelegate::CreateLambda(
			[this]()
			{
				StopSession(TEXT("console stop"));
				UE_LOG(LogTemp, Display, TEXT("[MtoUCameraSyncPrototype] stopped"));
			}),
		ECVF_Default));

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("MtoUCameraSyncPrototype.Status"),
		TEXT("MtoUCameraSyncPrototype.Status - prints the prototype session state"),
		FConsoleCommandDelegate::CreateLambda(
			[this]()
			{
				if (!Session.IsValid())
				{
					UE_LOG(LogTemp, Display, TEXT("[MtoUCameraSyncPrototype] no session"));
					return;
				}
				UE_LOG(LogTemp, Display,
					TEXT("[MtoUCameraSyncPrototype] frame %.3f playing=%d client=%d greeted=%d published=%lld lastError=%s"),
					Session->GetDisplayFrame(), Session->IsPlaying() ? 1 : 0,
					Session->HasClient() ? 1 : 0, Session->IsGreeted() ? 1 : 0,
					Session->GetPublishedFrameCount(), *Session->GetLastError());
			}),
		ECVF_Default));

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("MtoUCameraSyncPrototype.Seek"),
		TEXT("MtoUCameraSyncPrototype.Seek <DisplayFrame> - jumps the sequence time (Unreal stays the time authority)"),
		FConsoleCommandWithArgsDelegate::CreateLambda(
			[this](const TArray<FString>& Args)
			{
				if (!Session.IsValid() || !Args.IsValidIndex(0))
				{
					return;
				}
				Session->SetDisplayFrame(FCString::Atod(*Args[0]));
			}),
		ECVF_Default));

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("MtoUCameraSyncPrototype.Play"),
		TEXT("MtoUCameraSyncPrototype.Play [PlayRate] - advances the sequence time from Unreal"),
		FConsoleCommandWithArgsDelegate::CreateLambda(
			[this](const TArray<FString>& Args)
			{
				if (!Session.IsValid())
				{
					return;
				}
				if (Args.IsValidIndex(0))
				{
					Session->SetPlayRate(FCString::Atod(*Args[0]));
				}
				Session->Play();
			}),
		ECVF_Default));

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("MtoUCameraSyncPrototype.Pause"),
		TEXT("MtoUCameraSyncPrototype.Pause - holds the current sequence time"),
		FConsoleCommandDelegate::CreateLambda(
			[this]()
			{
				if (Session.IsValid())
				{
					Session->Pause();
				}
			}),
		ECVF_Default));
}

void FMtoUCameraSyncPrototypeModule::UnregisterConsoleCommands()
{
	IConsoleManager& Console = IConsoleManager::Get();
	for (IConsoleObject* Command : ConsoleCommands)
	{
		if (Command)
		{
			Console.UnregisterConsoleObject(Command);
		}
	}
	ConsoleCommands.Reset();
}

IMPLEMENT_MODULE(FMtoUCameraSyncPrototypeModule, MtoUCameraSyncPrototypeEditor)
