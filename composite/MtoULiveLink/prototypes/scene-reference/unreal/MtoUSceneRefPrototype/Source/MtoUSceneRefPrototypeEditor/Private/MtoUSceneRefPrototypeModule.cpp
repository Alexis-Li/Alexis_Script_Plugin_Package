// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MtoUSceneRefFixture.h"
#include "MtoUSceneRefPeer.h"
#include "MtoUSceneRefTransfer.h"

namespace
{
	FString DescribeTransfer(const FMtoUSceneRefTransferResult& Result)
	{
		return FString::Printf(
			TEXT("objects=%d actors=%d components=%d triangles=%d vertices=%d textures=%d images=%d"),
			Result.Resolution.ExportedObjectCount(),
			Result.Resolution.Actors.Num(),
			Result.ComponentCount,
			Result.TriangleCount,
			Result.VertexCount,
			Result.Output.TextureScan.TextureReferences,
			Result.Output.ImageFiles.Num());
	}

	void LogTransfer(const FMtoUSceneRefTransferResult& Result, const FString& ManifestPath)
	{
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
		for (const FString& Reference : Result.Output.TextureScan.FileNames)
		{
			UE_LOG(LogTemp, Warning, TEXT("MtoUSceneRef[%s]: texture reference recorded: %s"),
				*Result.Resolution.ScopeName, *Reference);
		}
		UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef[%s]: manifest %s"),
			*Result.Resolution.ScopeName, *ManifestPath);
	}

	bool ExportScope(const FMtoUSceneRefScopeSpec& Spec, const FMtoUSceneRefTransferOptions& Options, const FString& ObjPath)
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

		if (!ObjPath.IsEmpty())
		{
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
			bool bObjProbe = false;
			FMtoUSceneRefTransferOptions Options;
			for (const FString& Arg : Args)
			{
				if (Arg.Equals(TEXT("textured"), ESearchCase::IgnoreCase))
				{
					bTextured = true;
				}
				else if (Arg.Equals(TEXT("obj"), ESearchCase::IgnoreCase))
				{
					bObjProbe = true;
				}
				else if (Arg.Equals(TEXT("frontx"), ESearchCase::IgnoreCase))
				{
					Options.bForceFrontXAxis = true;
				}
				else if (Arg.StartsWith(TEXT("out=")))
				{
					Options.OutputDirectory = Arg.RightChop(4);
				}
			}

			const FMtoUSceneRefScopeSpec Spec = bTextured
				? FMtoUSceneRefFixture::TexturedScope()
				: FMtoUSceneRefFixture::MainScope();
			const FString ObjPath = bObjProbe
				? FPaths::Combine(Options.OutputDirectory.IsEmpty()
						? FPaths::ProjectSavedDir() / TEXT("MtoUSceneRef") / Spec.ScopeName()
						: Options.OutputDirectory,
					Spec.ScopeName() + TEXT(".obj"))
				: FString();
			if (!ExportScope(Spec, Options, ObjPath))
			{
				UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef.Export failed"));
			}
		})));

	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MtoUSceneRef.Peer"),
		TEXT("Runs the Maya importer/verifier over the last export. "
			"Args: <mayapy> <peer script> [out=<dir>]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef.Peer needs <mayapy> and <peer script>"));
				return;
			}
			FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MtoUSceneRef"),
				FMtoUSceneRefFixture::MainScope().ScopeName());
			for (const FString& Arg : Args)
			{
				if (Arg.StartsWith(TEXT("out=")))
				{
					Directory = Arg.RightChop(4);
				}
			}
			const FString ScopeName = FMtoUSceneRefFixture::MainScope().ScopeName();

			FMtoUSceneRefPeerRequest Request;
			Request.MayapyPath = Args[0];
			Request.PeerScriptPath = Args[1];
			Request.FbxPath = FPaths::Combine(Directory, ScopeName + TEXT(".fbx"));
			Request.ManifestPath = FPaths::Combine(Directory, ScopeName + TEXT(".manifest.json"));
			Request.ReportPath = FPaths::Combine(Directory, ScopeName + TEXT(".maya-report.json"));
			Request.LogPath = FPaths::Combine(Directory, ScopeName + TEXT(".maya-peer.log"));

			FMtoUSceneRefPeerResult Result;
			FString Error;
			if (!RunMayaSceneRefPeer(Request, Result, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("MtoUSceneRef.Peer: %s"), *Error);
				return;
			}
			UE_LOG(LogTemp, Display, TEXT("MtoUSceneRef.Peer: exit=%d report=%s"),
				Result.ReturnCode, *Request.ReportPath);
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
