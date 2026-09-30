// MtoU camera sync prototype (Issue 52 verification). Editor-only prototype code.

#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleInterface.h"
#include "MtoUCameraSyncSession.h"

/**
 * Editor-side entry point of the prototype: owns one optional session and exposes the
 * console commands a user runs while Maya is connected. The automation tests drive the
 * same object and the same start/stop entry points, so the editor's own ticker advances
 * the session exactly as it does for a user; they never pump the session themselves.
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
	/** Follows the open Level Sequence editor; the console command uses the same call. */
	FMtoUCameraSyncSession* StartEditorSession(
		UWorld& World, const TSharedRef<ISequencer>& Sequencer,
		const FMtoUCameraSyncSession::FConfig& Config, FString& OutError);
	void StopSession(const FString& Reason);
	/** The running session, or the last stopped one; the module keeps one at a time. */
	FMtoUCameraSyncSession* GetSession() const { return Session.Get(); }

private:
	bool Tick(float DeltaSeconds);
	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();

	TSharedPtr<FMtoUCameraSyncSession> Session;
	FTSTicker::FDelegateHandle TickerHandle;
	TArray<IConsoleObject*> ConsoleCommands;
};
