// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

#include "MtoUMultiSubjectPeer.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MtoUMultiSubjectReceiver.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool FMtoUMultiSubjectPeerRequest::TryReadArgumentValue(
	const TCHAR* CommandLine,
	const TCHAR* Name,
	FString& OutValue)
{
	OutValue.Reset();
	if (CommandLine == nullptr || Name == nullptr || *Name == TEXT('\0'))
	{
		return false;
	}
	const int32 NameLength = FCString::Strlen(Name);
	const TCHAR* Cursor = CommandLine;
	while (*Cursor != TEXT('\0'))
	{
		// The name may only match at a token boundary: after the beginning of
		// the line or a space, never in the middle of another argument.
		const bool bTokenStart = Cursor == CommandLine || *(Cursor - 1) == TEXT(' ');
		if (bTokenStart && FCString::Strnicmp(Cursor, Name, NameLength) == 0)
		{
			Cursor += NameLength;
			// A quoted value keeps its spaces; an unquoted one ends at the next
			// space, so the following arguments are never swallowed.
			if (*Cursor == TEXT('"'))
			{
				++Cursor;
				const TCHAR* QuoteEnd = FCString::Strchr(Cursor, TEXT('"'));
				if (QuoteEnd == nullptr)
				{
					return false;
				}
				OutValue = FString(static_cast<int32>(QuoteEnd - Cursor), Cursor);
			}
			else
			{
				const TCHAR* ValueEnd = Cursor;
				while (*ValueEnd != TEXT('\0') && *ValueEnd != TEXT(' '))
				{
					++ValueEnd;
				}
				OutValue = FString(static_cast<int32>(ValueEnd - Cursor), Cursor);
			}
			OutValue.TrimStartAndEndInline();
			return true;
		}
		// Skip to the next token.
		while (*Cursor != TEXT('\0') && *Cursor != TEXT(' '))
		{
			++Cursor;
		}
		while (*Cursor == TEXT(' '))
		{
			++Cursor;
		}
	}
	return false;
}

bool FMtoUMultiSubjectPeerRequest::ParseTimeList(const FString& Value, TArray<double>& OutTimes)
{
	OutTimes.Reset();
	TArray<FString> Parts;
	Value.ParseIntoArray(Parts, TEXT(","), /*CullEmpty=*/false);
	if (Parts.IsEmpty())
	{
		return false;
	}
	for (const FString& Part : Parts)
	{
		const FString Trimmed = Part.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.IsNumeric())
		{
			OutTimes.Reset();
			return false;
		}
		const double Number = FCString::Atod(*Trimmed);
		if (!FMath::IsFinite(Number))
		{
			OutTimes.Reset();
			return false;
		}
		OutTimes.Add(Number);
	}
	return !OutTimes.IsEmpty();
}

FString FMtoUMultiSubjectPeerRequest::BuildCommandLine() const
{
	FString CommandLine = FString::Printf(
		TEXT("\"%s\" --host %s --port %d --scenario %s --frames %d --fps %.6f --start-frame %d --evidence \"%s\""),
		*PeerScriptPath, *Host, Port, *Scenario, Frames, Fps, StartFrame, *EvidencePath);
	if (!Times.IsEmpty())
	{
		// Re-encoded from the parsed numbers, so the Maya argv only ever carries
		// validated time values.
		TArray<FString> TimeValues;
		TimeValues.Reserve(Times.Num());
		for (const double Time : Times)
		{
			TimeValues.Add(FString::Printf(TEXT("%.17g"), Time));
		}
		CommandLine += FString::Printf(TEXT(" --times %s"), *FString::Join(TimeValues, TEXT(",")));
	}
	if (RemoveAtFrame > 0)
	{
		CommandLine += FString::Printf(TEXT(" --remove-at %d"), RemoveAtFrame);
	}
	if (DropAfterFrames > 0)
	{
		CommandLine += FString::Printf(TEXT(" --drop-after %d"), DropAfterFrames);
	}
	if (!RigSpec.IsEmpty())
	{
		CommandLine += FString::Printf(TEXT(" --rig-spec \"%s\""), *RigSpec);
	}
	if (!RigDir.IsEmpty())
	{
		CommandLine += FString::Printf(TEXT(" --rig-dir \"%s\""), *RigDir);
	}
	if (!ScenePath.IsEmpty())
	{
		CommandLine += FString::Printf(TEXT(" --scene \"%s\""), *ScenePath);
	}
	for (const FString& ReferenceRig : ReferenceRigs)
	{
		CommandLine += FString::Printf(TEXT(" --reference-rig %s"), *ReferenceRig);
	}
	for (const FString& SubjectOverride : SubjectOverrides)
	{
		CommandLine += FString::Printf(TEXT(" --subject \"%s\""), *SubjectOverride);
	}
	for (const FString& Extra : ExtraArguments)
	{
		CommandLine += FString::Printf(TEXT(" %s"), *Extra);
	}
	return CommandLine;
}

bool RunMayaMultiSubjectPeer(
	const FMtoUMultiSubjectPeerRequest& Request,
	FMtoUMultiSubjectReceiver& Receiver,
	FMtoUMultiSubjectPeerResult& Out,
	FString& OutError)
{
	Out = FMtoUMultiSubjectPeerResult();
	if (!FPaths::FileExists(Request.MayapyPath))
	{
		OutError = FString::Printf(TEXT("mayapy was not found at %s"), *Request.MayapyPath);
		return false;
	}
	if (!FPaths::FileExists(Request.PeerScriptPath))
	{
		OutError = FString::Printf(TEXT("the Maya peer script was not found at %s"), *Request.PeerScriptPath);
		return false;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Request.EvidencePath), true);

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;
	FPlatformProcess::CreatePipe(PipeRead, PipeWrite);

	const FString ProcessArgs = Request.BuildCommandLine();
	FProcHandle Process = FPlatformProcess::CreateProc(
		*Request.MayapyPath,
		*ProcessArgs,
		/*bLaunchDetached=*/false,
		/*bLaunchHidden=*/true,
		/*bLaunchReallyHidden=*/true,
		nullptr,
		0,
		*FPaths::GetPath(Request.PeerScriptPath),
		PipeWrite,
		nullptr);
	if (!Process.IsValid())
	{
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
		OutError = FString::Printf(TEXT("could not start %s"), *Request.MayapyPath);
		return false;
	}
	Out.bStarted = true;

	// Maya prints its full evidence JSON to stdout. Drain the pipe while
	// pumping replies, or a full pipe can block mayapy before it exits.
	const double Deadline = FPlatformTime::Seconds() + Request.TimeoutSeconds;
	while (FPlatformProcess::IsProcRunning(Process) && FPlatformTime::Seconds() < Deadline)
	{
		Receiver.Pump(0.0);
		// The peer is streaming now: capture the negotiated subjects before it
		// disconnects and the receiver forgets the session.
		if (Out.NegotiatedSubjects.IsEmpty() && Receiver.HasSession())
		{
			Receiver.CollectNegotiatedSubjects(Out.NegotiatedSubjects);
		}
		Out.Output += FPlatformProcess::ReadPipe(PipeRead);
		FPlatformProcess::Sleep(0.001f);
	}
	const bool bRunning = FPlatformProcess::IsProcRunning(Process);
	if (bRunning)
	{
		FPlatformProcess::TerminateProc(Process, true);
		OutError = FString::Printf(
			TEXT("the Maya peer did not finish within %.0f seconds"), Request.TimeoutSeconds);
	}
	// One last pump serves any bytes the child wrote before exiting.
	Receiver.Pump(0.0);

	Out.Output += FPlatformProcess::ReadPipe(PipeRead);
	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
	Out.bCompleted = !bRunning;
	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(Process, &ReturnCode);
	Out.ReturnCode = ReturnCode;
	FPlatformProcess::CloseProc(Process);

	if (!Request.LogPath.IsEmpty())
	{
		// The exact Maya argv is part of the evidence: a launch failure has to
		// be diagnosable from the log alone.
		const FString LogText = FString::Printf(
			TEXT("[unreal] %s\n[unreal] exit=%d\n%s"), *ProcessArgs, Out.ReturnCode, *Out.Output);
		FFileHelper::SaveStringToFile(LogText, *Request.LogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	if (bRunning)
	{
		return false;
	}

	if (!FPaths::FileExists(Request.EvidencePath))
	{
		OutError = FString::Printf(
			TEXT("the Maya peer wrote no evidence at %s (exit code %d)"), *Request.EvidencePath, Out.ReturnCode);
		return false;
	}
	FString EvidenceText;
	if (!FFileHelper::LoadFileToString(EvidenceText, *Request.EvidencePath))
	{
		OutError = FString::Printf(TEXT("could not read %s"), *Request.EvidencePath);
		return false;
	}
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(EvidenceText);
	if (!FJsonSerializer::Deserialize(Reader, Out.Evidence) || !Out.Evidence.IsValid())
	{
		OutError = FString::Printf(TEXT("%s is not a JSON object"), *Request.EvidencePath);
		return false;
	}

	Out.bEvidenceOkField = Out.Evidence->TryGetBoolField(TEXT("ok"), Out.bEvidenceOk);
	Out.EvidenceErrorText = Out.Evidence->HasTypedField<EJson::String>(TEXT("error"))
		? Out.Evidence->GetStringField(TEXT("error"))
		: FString();
	// The Maya CLI records frames and measured round-trip latency inside each
	// session, not as top-level counters. A role replacement has two sessions.
	const TArray<TSharedPtr<FJsonValue>>* Sessions = nullptr;
	if (Out.Evidence->TryGetArrayField(TEXT("sessions"), Sessions) && Sessions != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& SessionValue : *Sessions)
		{
			const TSharedPtr<FJsonObject> Session = SessionValue.IsValid()
				? SessionValue->AsObject() : nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
			if (!Session.IsValid()
				|| !Session->TryGetArrayField(TEXT("frames"), Frames) || Frames == nullptr)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& FrameValue : *Frames)
			{
				const TSharedPtr<FJsonObject> Frame = FrameValue.IsValid()
					? FrameValue->AsObject() : nullptr;
				if (!Frame.IsValid())
				{
					continue;
				}
				double Latency = 0.0;
				if (Frame->TryGetNumberField(TEXT("latency_ms"), Latency))
				{
					if (Out.FirstLatencyMs < 0.0)
					{
						Out.FirstLatencyMs = Latency;
					}
					Out.MaxLatencyMs = FMath::Max(Out.MaxLatencyMs, Latency);
				}
				++Out.EvidenceFrameCount;
			}
		}
	}
	return true;
}
