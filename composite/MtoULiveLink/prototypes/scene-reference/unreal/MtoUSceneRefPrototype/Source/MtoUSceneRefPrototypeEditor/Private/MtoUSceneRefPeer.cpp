// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#include "MtoUSceneRefPeer.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

bool RunMayaSceneRefPeer(
	const FMtoUSceneRefPeerRequest& Request,
	FMtoUSceneRefPeerResult& Out,
	FString& OutError)
{
	Out = FMtoUSceneRefPeerResult();

	if (!FPaths::FileExists(Request.MayapyPath))
	{
		OutError = FString::Printf(TEXT("mayapy not found: %s"), *Request.MayapyPath);
		return false;
	}
	if (!FPaths::FileExists(Request.PeerScriptPath))
	{
		OutError = FString::Printf(TEXT("peer script not found: %s"), *Request.PeerScriptPath);
		return false;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Request.ReportPath), /*Tree=*/true);

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;
	FPlatformProcess::CreatePipe(PipeRead, PipeWrite);

	const FString ProcessArgs = FString::Printf(
		TEXT("\"%s\" --fbx \"%s\" --manifest \"%s\" --report \"%s\""),
		*Request.PeerScriptPath,
		*Request.FbxPath,
		*Request.ManifestPath,
		*Request.ReportPath);

	// Only the child's output pipe is inherited; the peer prints its own progress.
	FProcHandle Process = FPlatformProcess::CreateProc(
		*Request.MayapyPath,
		*ProcessArgs,
		/*bLaunchDetached=*/false,
		/*bLaunchHidden=*/true,
		/*bLaunchReallyHidden=*/true,
		nullptr,
		0,
		*FPaths::GetPath(Request.FbxPath),
		PipeWrite,
		nullptr);
	if (!Process.IsValid())
	{
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
		OutError = TEXT("the Maya process could not be started");
		return false;
	}
	Out.bStarted = true;

	const double Deadline = FPlatformTime::Seconds() + Request.TimeoutSeconds;
	while (FPlatformProcess::IsProcRunning(Process) && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.05f);
		Out.Output += FPlatformProcess::ReadPipe(PipeRead);
	}

	const bool bRunning = FPlatformProcess::IsProcRunning(Process);
	if (bRunning)
	{
		FPlatformProcess::TerminateProc(Process, /*ClearProc=*/true);
		OutError = FString::Printf(TEXT("the Maya process did not finish within %.0f s"), Request.TimeoutSeconds);
	}
	else
	{
		FPlatformProcess::WaitForProc(Process);
		FPlatformProcess::GetProcReturnCode(Process, &Out.ReturnCode);
		Out.bCompleted = true;
	}
	FPlatformProcess::CloseProc(Process);
	Out.Output += FPlatformProcess::ReadPipe(PipeRead);
	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

	if (!Request.LogPath.IsEmpty())
	{
		FFileHelper::SaveStringToFile(Out.Output, *Request.LogPath);
	}

	if (!FFileHelper::LoadFileToString(Out.ReportText, *Request.ReportPath))
	{
		OutError = FString::Printf(TEXT("the Maya peer wrote no report at %s"), *Request.ReportPath);
		return false;
	}
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Out.ReportText);
	if (!FJsonSerializer::Deserialize(Reader, Out.Report) || !Out.Report.IsValid())
	{
		OutError = TEXT("the Maya report is not valid JSON");
		return false;
	}
	return true;
}
