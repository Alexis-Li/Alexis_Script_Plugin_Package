// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MtoUSceneRefFixture.h"
#include "MtoUSceneRefPeer.h"
#include "MtoUSceneRefTransfer.h"

namespace
{
	FString DescribeTransfer(const FMtoUSceneRefTransferResult& Result)
	{
		const FMtoUSceneRefTextureScan& Scan = Result.Output.TextureScan;
		return FString::Printf(
			TEXT("objects=%d actors=%d components=%d suppressed=%d triangles=%d vertices=%d")
			TEXT(" texture_records=%d texture_references=%d video_records=%d content_records=%d")
			TEXT(" embedded_media_records=%d image_files=%d camera_records=%d light_records=%d"),
			Result.Resolution.ExportedObjectCount(),
			Result.Resolution.Actors.Num(),
			Result.ComponentCount,
			Result.SuppressedComponentCount,
			Result.TriangleCount,
			Result.VertexCount,
			Scan.TextureRecords,
			Scan.TextureReferences,
			Scan.VideoRecords,
			Scan.ContentRecords,
			Scan.EmbeddedMediaRecords,
			Result.Output.ImageFiles.Num(),
			Scan.CameraRecords,
			Scan.LightRecords);
	}

	void LogTransfer(const FMtoUSceneRefTransferResult& Result, const FString& ManifestPath)
	{
		const FMtoUSceneRefTextureScan& Scan = Result.Output.TextureScan;
		UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef[%s]: %s"),
			*Result.Resolution.ScopeName, *DescribeTransfer(Result));
		UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef[%s]: scope loaded=[%s] unloaded=[%s] excluded=[%s]"),
			*Result.Resolution.ScopeName,
			*FString::Join(Result.Resolution.LoadedSublevels, TEXT(", ")),
			*FString::JoinBy(Result.Resolution.UnloadedSublevels, TEXT(", "),
				[](const FMtoUSceneRefUnloadedLevel& Level) { return Level.Package + TEXT("(") + Level.StreamingState + TEXT(")"); }),
			*FString::Join(Result.Resolution.ExcludedSublevels, TEXT(", ")));
		for (const FMtoUSceneRefSkipped& Skipped : Result.Resolution.Unsupported)
		{
			UE_LOG(LogTemp, Warning, TEXT("MtoUSceneRef[%s]: unsupported %s (%s): %s"),
				*Result.Resolution.ScopeName, *Skipped.ActorLabel, *Skipped.ActorClass, *Skipped.Reason);
		}
		for (const FMtoUSceneRefSuppressedComponent& Suppressed : Result.Resolution.SuppressedComponents)
		{
			UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef[%s]: suppressed %s (%s) component %s (%s): %s"),
				*Result.Resolution.ScopeName,
				*Suppressed.ActorLabel,
				*Suppressed.ActorClass,
				*Suppressed.ComponentName,
				*Suppressed.ComponentClass,
				*Suppressed.Reason);
		}
		// The media counts are facts the manifest carries, not a refusal: they say what the
		// file names, what it embeds, and how many image files the export wrote.
		for (const FString& Reference : Scan.FileNames)
		{
			UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef[%s]: media reference recorded: %s%s"),
				*Result.Resolution.ScopeName, *Reference,
				Result.Output.TextureReferenceFilesPresent.Contains(Reference)
					? TEXT(" (present on this machine)")
					: TEXT(" (absent)"));
		}
		for (const FString& Embedded : Scan.EmbeddedMedia)
		{
			UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef[%s]: embedded media: %s"),
				*Result.Resolution.ScopeName, *Embedded);
		}
		if (Result.Output.ImageFiles.Num() > 0 || Scan.EmbeddedMediaRecords > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("MtoUSceneRef[%s]: the handoff carries media: %d image files, %d embedded media records"),
				*Result.Resolution.ScopeName, Result.Output.ImageFiles.Num(), Scan.EmbeddedMediaRecords);
		}
		UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef[%s]: manifest %s"),
			*Result.Resolution.ScopeName, *ManifestPath);
	}

	/** Trims a mayapy value and the quotes a command line or a shell may have kept around it. */
	FString TrimMayapyValue(const FString& Value)
	{
		FString Trimmed = Value;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.Len() >= 2 && Trimmed.StartsWith(TEXT("\"")) && Trimmed.EndsWith(TEXT("\"")))
		{
			Trimmed = Trimmed.Mid(1, Trimmed.Len() - 2);
			Trimmed.TrimStartAndEndInline();
		}
		return Trimmed;
	}

	/**
	 * The mayapy the Maya peer runs under, and where it came from.
	 *
	 * The command's arguments reach it already split on whitespace, so a mayapy path that
	 * contains spaces cannot travel as a positional argument, and the 8.3 short form of such
	 * a path breaks Maya's own plug-in resolution (measured: mayapy exits 2 with
	 * REFUSED FBX_PLUGIN_UNAVAILABLE). The full path therefore comes from the
	 * `-MtoUSceneRefMayapy=` command line switch when the run carries it, else from the
	 * MTOU_SCENEREF_MAYAPY environment variable, and only then from the first positional
	 * argument (kept for a path without spaces).
	 *
	 * OutPeerScriptIndex is the index of the peer script: the argument after the mayapy when
	 * the mayapy was positional, the first argument otherwise. Returns false when no source
	 * names a mayapy at all.
	 */
	bool ResolveMayapyPath(const TArray<FString>& Args, FString& OutMayapy, int32& OutPeerScriptIndex, FString& OutSource)
	{
		OutPeerScriptIndex = 0;

		FString Switch;
		if (FParse::Value(FCommandLine::Get(), TEXT("MtoUSceneRefMayapy="), Switch))
		{
			const FString Value = TrimMayapyValue(Switch);
			if (!Value.IsEmpty())
			{
				OutMayapy = Value;
				OutSource = TEXT("-MtoUSceneRefMayapy=");
				return true;
			}
		}

		const FString Variable = TrimMayapyValue(FPlatformMisc::GetEnvironmentVariable(TEXT("MTOU_SCENEREF_MAYAPY")));
		if (!Variable.IsEmpty())
		{
			OutMayapy = Variable;
			OutSource = TEXT("MTOU_SCENEREF_MAYAPY");
			return true;
		}

		if (Args.Num() > 0)
		{
			OutMayapy = Args[0];
			OutSource = TEXT("the first positional argument");
			OutPeerScriptIndex = 1;
			return true;
		}
		return false;
	}

	/** The export flags every export command shares. */
	struct FMtoUSceneRefCommandOptions
	{
		FMtoUSceneRefTransferOptions Transfer;
		bool bObjProbe = false;
	};

	/** Reads `obj`, `frontx` and `out=` out of one command's argument list. */
	void ReadExportFlags(const TArray<FString>& Args, FMtoUSceneRefCommandOptions& Out)
	{
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("obj"), ESearchCase::IgnoreCase))
			{
				Out.bObjProbe = true;
			}
			else if (Arg.Equals(TEXT("frontx"), ESearchCase::IgnoreCase))
			{
				Out.Transfer.bForceFrontXAxis = true;
			}
			else if (Arg.StartsWith(TEXT("out=")))
			{
				Out.Transfer.OutputDirectory = Arg.RightChop(4);
			}
		}
	}

	bool ExportScope(const FMtoUSceneRefScopeSpec& Spec, const FMtoUSceneRefTransferOptions& Options, bool bObjProbe)
	{
		UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef: no editor world; build the fixture first"));
			return false;
		}

		FMtoUSceneRefTransferResult Result;
		if (!FMtoUSceneRefTransfer::Run(*World, Spec, Options, Result))
		{
			UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef: export refused: %s"), *Result.Error);
			return false;
		}

		const FString ManifestPath = FPaths::Combine(
			Result.Output.Directory, Result.Resolution.ScopeName + TEXT(".manifest.json"));
		FString WriteError;
		if (!Result.WriteManifest(WriteError))
		{
			UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef: %s"), *WriteError);
			return false;
		}
		LogTransfer(Result, ManifestPath);

		if (bObjProbe)
		{
			const FString ObjPath = FPaths::Combine(
				Result.Output.Directory, Result.Resolution.ScopeName + TEXT(".obj"));
			TArray<FMtoUSceneRefProducedFile> ObjFiles;
			TArray<FString> MaterialLines;
			FString ObjError;
			const bool bProbed = FMtoUSceneRefTransfer::ProbeObjExport(*World, Result.Resolution, ObjPath, ObjFiles, MaterialLines, ObjError);
			if (!bProbed)
			{
				UE_LOG(LogTemp, Warning, TEXT("MtoUSceneRef: OBJ probe refused: %s"), *ObjError);
			}
			for (const FMtoUSceneRefProducedFile& File : ObjFiles)
			{
				UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef: OBJ probe wrote %s (%lld bytes)"),
					*File.Name, File.Bytes);
			}
			UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef: OBJ probe material lines: %d"),
				MaterialLines.Num());
		}
		return true;
	}
}

/**
 * Editor entry point: the console commands an artist or an evidence run uses.
 * The automation tests drive the fixture, the transfer and the peer directly.
 */
class FMtoUSceneRefPrototypeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterCommands();
	void UnregisterCommands();

	TArray<IConsoleObject*> Commands;
};

void FMtoUSceneRefPrototypeModule::StartupModule()
{
	RegisterCommands();
}

void FMtoUSceneRefPrototypeModule::ShutdownModule()
{
	UnregisterCommands();
}

void FMtoUSceneRefPrototypeModule::RegisterCommands()
{
	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MtoUSceneRef.BuildFixture"),
		TEXT("Rebuilds the generated scene reference fixture under /Game/MtoUSceneRefFixture."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			TArray<FString> Notes;
			FString Error;
			if (FMtoUSceneRefFixture::Build(Notes, Error))
			{
				for (const FString& Note : Notes)
				{
					UE_LOG(LogTemp, Warning, TEXT("MtoUSceneRef fixture: %s"), *Note);
				}
				UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef fixture: built and loaded %s"),
					FMtoUSceneRefFixture::HostLevelPackage());
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef fixture: %s"), *Error);
			}
		})));

	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MtoUSceneRef.Export"),
		TEXT("Exports the fixture scope with the engine's FBX level exporter. "
			"Args: [textured] [obj] [frontx] [out=<dir>]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			bool bTextured = false;
			FMtoUSceneRefCommandOptions CommandOptions;
			for (const FString& Arg : Args)
			{
				if (Arg.Equals(TEXT("textured"), ESearchCase::IgnoreCase))
				{
					bTextured = true;
				}
			}
			ReadExportFlags(Args, CommandOptions);

			const FMtoUSceneRefScopeSpec Spec = bTextured
				? FMtoUSceneRefFixture::TexturedScope()
				: FMtoUSceneRefFixture::MainScope();
			if (!ExportScope(Spec, CommandOptions.Transfer, CommandOptions.bObjProbe))
			{
				UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef.Export failed"));
			}
		})));

	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MtoUSceneRef.ExportLevel"),
		TEXT("Exports the scope of a level that is already the loaded editor world; the "
			"command never loads or switches a level. "
			"Args: <level_package> [sublevel=<package>]... [obj] [frontx] [out=<dir>]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FMtoUSceneRefScopeSpec Spec;
			FMtoUSceneRefCommandOptions CommandOptions;
			for (const FString& Arg : Args)
			{
				if (Arg.StartsWith(TEXT("sublevel=")))
				{
					Spec.RequestedSublevels.Add(Arg.RightChop(9));
				}
				else if (!Arg.Contains(TEXT("=")))
				{
					if (Spec.LevelPackage.IsEmpty())
					{
						Spec.LevelPackage = Arg;
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("MtoUSceneRef.ExportLevel: ignoring the extra argument %s"), *Arg);
					}
				}
			}
			ReadExportFlags(Args, CommandOptions);

			if (Spec.LevelPackage.IsEmpty())
			{
				UE_LOG(LogTemp, Error,
					TEXT("MtoUSceneRef.ExportLevel needs the level package: "
						"MtoUSceneRef.ExportLevel /Game/Map [sublevel=/Game/Sub] [obj] [frontx] [out=<dir>]"));
				return;
			}
			if (!ExportScope(Spec, CommandOptions.Transfer, CommandOptions.bObjProbe))
			{
				UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef.ExportLevel failed"));
			}
		})));

	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MtoUSceneRef.Peer"),
		TEXT("Runs the Maya importer/verifier over the last export. "
			"The mayapy that runs the peer is resolved in this order: the "
			"-MtoUSceneRefMayapy=<path> command line switch, then the MTOU_SCENEREF_MAYAPY "
			"environment variable, then the first positional argument. A path that contains "
			"spaces only fits the switch or the variable, because the command's arguments are "
			"split on whitespace (an 8.3 short path is not a substitute: it breaks Maya's own "
			"plug-in resolution). "
			"Args: [<mayapy>] <peer script> [scope=<name>] [out=<dir>] "
			"[world=camera] [shading=material|keep] [dryrun] [allowimagedata]. "
			"The last four reach the Maya CLI as --target-world, --shading, --dry-run "
			"and --allow-image-data."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FString MayapyPath;
			FString MayapySource;
			int32 PeerScriptIndex = 0;
			if (!ResolveMayapyPath(Args, MayapyPath, PeerScriptIndex, MayapySource)
				|| Args.Num() <= PeerScriptIndex)
			{
				UE_LOG(LogTemp, Error,
					TEXT("MtoUSceneRef.Peer needs a mayapy (the -MtoUSceneRefMayapy=<path> switch, "
						"the MTOU_SCENEREF_MAYAPY environment variable, or the first positional "
						"argument) and the peer script"));
				return;
			}

			FString ScopeName = FMtoUSceneRefFixture::MainScope().ScopeName();
			FString Directory;
			TArray<FString> ExtraArguments;
			for (int32 ArgIndex = PeerScriptIndex + 1; ArgIndex < Args.Num(); ++ArgIndex)
			{
				const FString& Arg = Args[ArgIndex];
				if (Arg.StartsWith(TEXT("scope=")))
				{
					ScopeName = Arg.RightChop(6);
				}
				else if (Arg.StartsWith(TEXT("out=")))
				{
					Directory = Arg.RightChop(4);
				}
				else if (Arg.Equals(TEXT("world=camera"), ESearchCase::IgnoreCase))
				{
					ExtraArguments.Add(TEXT("--target-world camera"));
				}
				else if (Arg.StartsWith(TEXT("shading=")))
				{
					ExtraArguments.Add(FString::Printf(TEXT("--shading %s"), *Arg.RightChop(8)));
				}
				else if (Arg.Equals(TEXT("dryrun"), ESearchCase::IgnoreCase))
				{
					ExtraArguments.Add(TEXT("--dry-run"));
				}
				else if (Arg.Equals(TEXT("allowimagedata"), ESearchCase::IgnoreCase))
				{
					ExtraArguments.Add(TEXT("--allow-image-data"));
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("MtoUSceneRef.Peer: unknown argument %s"), *Arg);
				}
			}
			if (Directory.IsEmpty())
			{
				Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MtoUSceneRef"), ScopeName);
			}

			FMtoUSceneRefPeerRequest Request;
			Request.MayapyPath = MayapyPath;
			Request.PeerScriptPath = Args[PeerScriptIndex];
			Request.FbxPath = FPaths::Combine(Directory, ScopeName + TEXT(".fbx"));
			Request.ManifestPath = FPaths::Combine(Directory, ScopeName + TEXT(".manifest.json"));
			Request.ReportPath = FPaths::Combine(Directory, ScopeName + TEXT(".maya-report.json"));
			Request.LogPath = FPaths::Combine(Directory, ScopeName + TEXT(".maya-peer.log"));
			Request.ExtraArguments = ExtraArguments;

			UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef.Peer: mayapy %s (from %s)"),
				*Request.MayapyPath, *MayapySource);

			FMtoUSceneRefPeerResult Result;
			FString Error;
			if (!RunMayaSceneRefPeer(Request, Result, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef.Peer: %s"), *Error);
				return;
			}
			const bool bReportOk = Result.Report.IsValid()
				&& Result.Report->HasTypedField<EJson::Boolean>(TEXT("ok"))
				&& Result.Report->GetBoolField(TEXT("ok"));
			UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef.Peer: exit=%d ok=%s report=%s"),
				Result.ReturnCode, bReportOk ? TEXT("true") : TEXT("false"), *Request.ReportPath);
			UE_LOG(LogTemp, Display, TEXT("%s"), *Result.Output);
		})));
}

void FMtoUSceneRefPrototypeModule::UnregisterCommands()
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

IMPLEMENT_MODULE(FMtoUSceneRefPrototypeModule, MtoUSceneRefPrototypeEditor)
