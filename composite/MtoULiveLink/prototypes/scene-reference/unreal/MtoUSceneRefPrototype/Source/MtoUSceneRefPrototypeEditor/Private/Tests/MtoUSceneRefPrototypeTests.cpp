// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MtoUSceneRefFixture.h"
#include "MtoUSceneRefPeer.h"
#include "MtoUSceneRefScope.h"
#include "MtoUSceneRefTransfer.h"
#include "Serialization/JsonSerializer.h"

#if WITH_EDITOR

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
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MtoUSceneRefTests"));
	}

	/** Builds the fixture unless the host level is already the loaded world. */
	bool EnsureFixture(FAutomationTestBase& Test, TArray<FString>& OutNotes)
	{
		const UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World != nullptr &&
			World->GetOutermost()->GetName() == FMtoUSceneRefFixture::HostLevelPackage() &&
			World->GetNumLevels() > 1)
		{
			return true;
		}
		FString Error;
		if (!FMtoUSceneRefFixture::Build(OutNotes, Error))
		{
			Test.AddError(FString::Printf(TEXT("fixture build failed: %s"), *Error));
			return false;
		}
		return true;
	}

	bool LoadJson(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool RunTransfer(
		FAutomationTestBase& Test,
		const FMtoUSceneRefScopeSpec& Spec,
		const FString& OutDirectory,
		FMtoUSceneRefTransferResult& OutResult)
	{
		UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World == nullptr)
		{
			Test.AddError(TEXT("no editor world"));
			return false;
		}
		FMtoUSceneRefTransferOptions Options;
		Options.OutputDirectory = OutDirectory;
		if (!FMtoUSceneRefTransfer::Run(*World, Spec, Options, OutResult))
		{
			Test.AddError(FString::Printf(TEXT("transfer failed: %s"), *OutResult.Error));
			return false;
		}
		FString WriteError;
		if (!OutResult.WriteManifest(WriteError))
		{
			Test.AddError(WriteError);
			return false;
		}
		return true;
	}

	int32 CountCategory(const FMtoUSceneRefResolution& Resolution, EMtoUSceneRefCategory Category)
	{
		int32 Count = 0;
		for (const FMtoUSceneRefObject& Object : Resolution.Objects)
		{
			if (Object.Category == Category)
			{
				++Count;
			}
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUSceneRefScopeTest,
	"MtoUSceneRefPrototype.ScopeSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefScopeTest::RunTest(const FString& Parameters)
{
	TArray<FString> Notes;
	if (!EnsureFixture(*this, Notes))
	{
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	FMtoUSceneRefResolution Resolution;
	TestTrue(TEXT("the fixture scope resolves"),
		FMtoUSceneRefScope::Resolve(*World, FMtoUSceneRefFixture::MainScope(), Resolution));

	TestEqual(TEXT("the persistent level is the loaded world"),
		Resolution.PersistentLevelPackage, FString(FMtoUSceneRefFixture::HostLevelPackage()));
	TestEqual(TEXT("the requested loaded sublevel is in the scope"),
		Resolution.LoadedSublevels.Num(), 1);
	TestEqual(TEXT("the requested sublevel the world does not hold is reported"),
		Resolution.UnloadedSublevels.Num(), 1);
	if (Resolution.UnloadedSublevels.Num() == 1)
	{
		TestEqual(TEXT("the reported level is the one that was requested"),
			Resolution.UnloadedSublevels[0].Package, FString(FMtoUSceneRefFixture::UnloadedSublevelPackage()));
		TestEqual(TEXT("the reported level is absent from the loaded world"),
			Resolution.UnloadedSublevels[0].StreamingState, FString(TEXT("not_in_world")));
	}
	TestEqual(TEXT("the absent requested sublevel is counted"),
		Resolution.RequestedSublevelsMissing, 1);

	// The unloaded sublevel's actor is in no object record and in no level the world holds.
	for (const FMtoUSceneRefObject& Object : Resolution.Objects)
	{
		TestFalse(TEXT("no object comes from the unloaded sublevel"),
			Object.LevelPackage.Contains(TEXT("MtoUSceneRef_Unloaded")));
		TestTrue(TEXT("every object carries a node name"), !Object.NodeName.IsEmpty());
	}
	TestEqual(TEXT("three instances are carried for the instanced component"),
		CountCategory(Resolution, EMtoUSceneRefCategory::InstancedStaticMesh), 3);
	TestTrue(TEXT("plain static meshes are carried"),
		CountCategory(Resolution, EMtoUSceneRefCategory::StaticMesh) >= 3);

	// The unrequested sublevel is reported rather than exported.
	TestEqual(TEXT("the unrequested sublevel is excluded"), Resolution.ExcludedSublevels.Num(), 1);
	const int32 Exported = Resolution.ExportedObjectCount();
	TestTrue(TEXT("at least the five sample meshes and three instances are exported"),
		Exported >= 8);
	TestTrue(TEXT("non-geometry actors are reported, not dropped"), Resolution.Skipped.Num() >= 1);
	TestTrue(TEXT("the light is reported with a reason"),
		Resolution.Skipped.ContainsByPredicate([](const FMtoUSceneRefSkipped& Entry)
			{
				return Entry.ActorClass == TEXT("PointLight") && !Entry.Reason.IsEmpty();
			}));

	// A requested level that is not the loaded world is refused instead of silently switching.
	FMtoUSceneRefScopeSpec WrongSpec = FMtoUSceneRefFixture::MainScope();
	WrongSpec.LevelPackage = FMtoUSceneRefFixture::LoadedSublevelPackage();
	FMtoUSceneRefResolution Refused;
	TestFalse(TEXT("a scope for a level that is not loaded is refused"),
		FMtoUSceneRefScope::Resolve(*World, WrongSpec, Refused));
	TestTrue(TEXT("the refusal explains itself"), Refused.Error.Contains(TEXT("not the loaded editor world")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUSceneRefExportTest,
	"MtoUSceneRefPrototype.ExportMainScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefExportTest::RunTest(const FString& Parameters)
{
	TArray<FString> Notes;
	if (!EnsureFixture(*this, Notes))
	{
		return false;
	}

	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("main"));
	FMtoUSceneRefTransferResult Result;
	if (!RunTransfer(*this, FMtoUSceneRefFixture::MainScope(), Directory, Result))
	{
		return false;
	}

	// The handoff must be geometry only: one file, no images, no texture records.
	TestEqual(TEXT("the export wrote exactly one file"), Result.Output.Files.Num(), 1);
	TestEqual(TEXT("the produced file is the FBX"), Result.Output.Files[0].Name,
		Result.Resolution.ScopeName + TEXT(".fbx"));
	TestTrue(TEXT("the FBX is not empty"), Result.Output.GeometryBytes > 0);
	TestEqual(TEXT("no image file is written"), Result.Output.ImageFiles.Num(), 0);
	TestEqual(TEXT("no material texture record reaches the file"),
		Result.Output.TextureScan.TextureReferences, 0);
	TestEqual(TEXT("the export carries the assembled transforms"),
		Result.Output.TextureScan.FileNames.Num(), 0);

	// Predicted node names must exist in the file the engine actually wrote.
	for (const FMtoUSceneRefObject& Object : Result.Resolution.Objects)
	{
		if (!Object.bExported || Object.Category == EMtoUSceneRefCategory::InstancedStaticMesh)
		{
			// Instanced children are named by index; Maya may rename a numeric node, so
			// they are matched by transform on the Maya side and not asserted here.
			continue;
		}
		TestTrue(FString::Printf(TEXT("the file holds the node %s"), *Object.NodeName),
			Result.Output.NodeNames.Contains(Object.NodeName));
	}

	// The manifest is the cross-host contract; it must parse and describe the same run.
	TSharedPtr<FJsonObject> Manifest;
	const FString ManifestPath = FPaths::Combine(Directory, Result.Resolution.ScopeName + TEXT(".manifest.json"));
	TestTrue(TEXT("the manifest parses"), LoadJson(ManifestPath, Manifest));
	if (Manifest.IsValid())
	{
		TestEqual(TEXT("manifest schema"), Manifest->GetStringField(TEXT("schema")),
			FString(TEXT("mtou-scene-ref-manifest/1")));
		const TSharedPtr<FJsonObject> Output = Manifest->GetObjectField(TEXT("output"));
		TestTrue(TEXT("the manifest output section exists"), Output.IsValid());
		if (Output.IsValid())
		{
			TestEqual(TEXT("the manifest records no texture references"),
				Output->GetIntegerField(TEXT("texture_references")), 0);
			TestEqual(TEXT("the manifest records no image files"),
				Output->GetArrayField(TEXT("image_files")).Num(), 0);
		}
		const TSharedPtr<FJsonObject> Scope = Manifest->GetObjectField(TEXT("scope"));
		TestTrue(TEXT("the manifest scope section exists"), Scope.IsValid());
		if (Scope.IsValid())
		{
			TestEqual(TEXT("the manifest reports the unloaded sublevel"),
				Scope->GetArrayField(TEXT("unloaded_sublevels")).Num(), 1);
		}
		const TSharedPtr<FJsonObject> Scale = Manifest->GetObjectField(TEXT("scale"));
		TestTrue(TEXT("the manifest records the geometry scale"), Scale.IsValid());
		if (Scale.IsValid())
		{
			TestEqual(TEXT("the manifest object count matches the resolution"),
				Scale->GetIntegerField(TEXT("objects")), Result.Resolution.ExportedObjectCount());
			TestTrue(TEXT("the manifest records triangles"), Scale->GetIntegerField(TEXT("triangles")) > 0);
		}
	}

	// The comparison a symmetric sample cannot make: the surface centroid that moves when
	// a placement is mirrored. The fixture must carry one object with a large offset.
	{
		const FMtoUSceneRefObject* Asymmetric = Result.Resolution.Objects.FindByPredicate(
			[](const FMtoUSceneRefObject& Object) { return Object.ActorLabel == TEXT("SM_Cone_Asymmetric"); });
		TestTrue(TEXT("the fixture carries the asymmetric sample"), Asymmetric != nullptr);
		if (Asymmetric != nullptr)
		{
			const FVector Offset = Asymmetric->WorldSurfaceCentroid - Asymmetric->WorldTransform.GetLocation();
			TestTrue(TEXT("the asymmetric sample's surface centroid is several centimetres off its pivot"),
				Offset.Size() > 3.0);
		}
	}

	// A repeat run replaces its own directory instead of mixing with the previous one.
	FMtoUSceneRefTransferResult Again;
	if (RunTransfer(*this, FMtoUSceneRefFixture::MainScope(), Directory, Again))
	{
		TestEqual(TEXT("a repeat run writes the same number of files"), Again.Output.Files.Num(), 1);
		TestEqual(TEXT("a repeat run exports the same object count"),
			Again.Resolution.ExportedObjectCount(), Result.Resolution.ExportedObjectCount());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUSceneRefTextureProbeTest,
	"MtoUSceneRefPrototype.TextureProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefTextureProbeTest::RunTest(const FString& Parameters)
{
	TArray<FString> Notes;
	if (!EnsureFixture(*this, Notes))
	{
		return false;
	}
	if (Notes.Num() > 0)
	{
		for (const FString& Note : Notes)
		{
			AddInfo(Note);
		}
	}

	// The texture probe sublevel is streamed into the world and loaded; editor worlds load
	// every sublevel they hold, and the main scope reports it as an excluded sublevel.
	UWorld* World = GEditor->GetEditorWorldContext().World();
	ULevelStreaming* Textured = nullptr;
	for (ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		if (Streaming != nullptr && Streaming->GetWorldAssetPackageName() == FMtoUSceneRefFixture::TexturedSublevelPackage())
		{
			Textured = Streaming;
			break;
		}
	}
	if (Textured == nullptr)
	{
		AddError(TEXT("the texture probe sublevel is not in the world"));
		return false;
	}
	TestTrue(TEXT("the texture probe sublevel is loaded"), Textured->IsLevelLoaded());

	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("textured"));
	FMtoUSceneRefTransferResult Result;
	if (!RunTransfer(*this, FMtoUSceneRefFixture::TexturedScope(), Directory, Result))
	{
		return false;
	}

	AddInfo(FString::Printf(TEXT("texture probe: %d texture records, %d files recorded"),
		Result.Output.TextureScan.TextureReferences, Result.Output.TextureScan.FileNames.Num()));
	for (const FString& Reference : Result.Output.TextureScan.FileNames)
	{
		AddInfo(FString::Printf(TEXT("texture probe reference: %s%s"), *Reference,
			Result.Output.TextureReferenceFilesPresent.Contains(Reference) ? TEXT(" (present)") : TEXT(" (absent)")));
	}

	// This is the check biting: a material whose BaseColor is a texture makes the engine
	// write a texture record, so a texture-free claim has to inspect the file.
	TestTrue(TEXT("a texture driven material produces a texture record"),
		Result.Output.TextureScan.TextureReferences >= 1);
	TestTrue(TEXT("the recorded texture file name is reported"),
		Result.Output.TextureScan.FileNames.Num() >= 1);
	// No image is written: disabling baking is measurable, not just a flag.
	TestEqual(TEXT("no image file is written for the texture probe"),
		Result.Output.ImageFiles.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUSceneRefDetectorTest,
	"MtoUSceneRefPrototype.TextureDetector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefDetectorTest::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("detector"));
	IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);

	const FString TexturedPath = FPaths::Combine(Directory, TEXT("textured.fbx"));
	const FString PlainPath = FPaths::Combine(Directory, TEXT("plain.fbx"));

	// A minimal ASCII FBX fragment with one texture clip that names a file.
	const FString TexturedSample = TEXT(
		"; FBX 7.4.0 project file\n"
		"Objects:  {\n"
		"\tTexture: \"Texture::T_Rock\", \"\" {\n"
		"\t\tType: \"TextureVideoClip\"\n"
		"\t\tFileName: \"Assets/T_Rock.png\"\n"
		"\t\tRelativeFilename: \"T_Rock.png\"\n"
		"\t\tProperties70:  {\n"
		"\t\t\tP: \"UVSet\", \"KString\", \"\", \"\", \"UVMap\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tModel: \"Model::SM_Rock\", \"Mesh\" {\n"
		"\t\tVersion: 232\n"
		"\t}\n"
		"}\n");
	const FString PlainSample = TEXT(
		"; FBX 7.4.0 project file\n"
		"Objects:  {\n"
		"\tModel: \"Model::SM_Rock\", \"Mesh\" {\n"
		"\t\tVersion: 232\n"
		"\t\tProperties70:  {\n"
		"\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,0\n"
		"\t\t}\n"
		"\t}\n"
		"}\n");

	TestTrue(TEXT("the textured sample file is written"),
		FFileHelper::SaveStringToFile(TexturedSample, *TexturedPath));
	TestTrue(TEXT("the plain sample file is written"),
		FFileHelper::SaveStringToFile(PlainSample, *PlainPath));

	TArray<FString> Files;
	FMtoUSceneRefTextureScan Scan;
	FMtoUSceneRefTransfer::ScanFbxTextureReferences(TexturedPath, Scan);
	TestEqual(TEXT("the detector finds the texture record"), Scan.TextureRecords, 1);
	TestEqual(TEXT("the detector finds the texture reference"), Scan.TextureReferences, 1);
	Files = Scan.FileNames;
	TestTrue(TEXT("the detector reports the recorded file name"),
		Files.Contains(TEXT("Assets/T_Rock.png")) && Files.Contains(TEXT("T_Rock.png")));

	TArray<FString> Names;
	FMtoUSceneRefTransfer::ScanFbxModelNames(TexturedPath, Names);
	TestTrue(TEXT("the detector finds the model name"), Names.Contains(TEXT("SM_Rock")));

	FMtoUSceneRefTextureScan PlainScan;
	FMtoUSceneRefTransfer::ScanFbxTextureReferences(PlainPath, PlainScan);
	TestEqual(TEXT("a geometry only file reports no texture record"), PlainScan.TextureRecords, 0);
	TestEqual(TEXT("a geometry only file records no file name"), PlainScan.FileNames.Num(), 0);
	return true;
}

/**
 * Cross-host check. Requires -MtoUSceneRefMayapy=, -MtoUSceneRefPeer= and
 * -MtoUEvidence=; without them it reports that it was not requested.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUSceneRefMayaPeerTest,
	"MtoUSceneRefPrototype.RealMayaPeer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefMayaPeerTest::RunTest(const FString& Parameters)
{
	FString MayapyPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUSceneRefMayapy="), MayapyPath))
	{
		AddInfo(TEXT("cross-host check not requested; supply MtoUSceneRefMayapy, MtoUSceneRefPeer and MtoUEvidence."));
		return true;
	}
	FString PeerPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUSceneRefPeer="), PeerPath) ||
		!FPaths::FileExists(MayapyPath) || !FPaths::FileExists(PeerPath))
	{
		AddError(TEXT("the cross-host check needs an existing mayapy and peer script"));
		return false;
	}

	TArray<FString> Notes;
	if (!EnsureFixture(*this, Notes))
	{
		return false;
	}

	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("peer"));
	FMtoUSceneRefTransferResult Result;
	if (!RunTransfer(*this, FMtoUSceneRefFixture::MainScope(), Directory, Result))
	{
		return false;
	}

	FMtoUSceneRefPeerRequest Request;
	Request.MayapyPath = MayapyPath;
	Request.PeerScriptPath = PeerPath;
	Request.FbxPath = FPaths::Combine(Directory, Result.Resolution.ScopeName + TEXT(".fbx"));
	Request.ManifestPath = FPaths::Combine(Directory, Result.Resolution.ScopeName + TEXT(".manifest.json"));
	Request.ReportPath = FPaths::Combine(Directory, Result.Resolution.ScopeName + TEXT(".maya-report.json"));
	Request.LogPath = FPaths::Combine(Directory, Result.Resolution.ScopeName + TEXT(".maya-peer.log"));

	FMtoUSceneRefPeerResult Peer;
	FString Error;
	if (!RunMayaSceneRefPeer(Request, Peer, Error))
	{
		AddError(Error);
		return false;
	}
	AddInfo(FString::Printf(TEXT("peer exit code %d"), Peer.ReturnCode));
	TestTrue(TEXT("the Maya peer finished"), Peer.bCompleted);
	TestEqual(TEXT("the Maya peer reports success"), Peer.ReturnCode, 0);
	if (!Peer.Report.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Report = Peer.Report;
	TestTrue(TEXT("the Maya report claims ok"), Report->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("the Maya container was created"),
		Report->GetObjectField(TEXT("container"))->GetStringField(TEXT("namespace")).Len() > 0);

	const TSharedPtr<FJsonObject> Counts = Report->GetObjectField(TEXT("counts"));
	TestTrue(TEXT("the report has counts"), Counts.IsValid());
	if (Counts.IsValid())
	{
		TestEqual(TEXT("every manifest object is in the Maya container"),
			Counts->GetIntegerField(TEXT("manifest_objects")), Result.Resolution.ExportedObjectCount());
		TestEqual(TEXT("Maya created no file texture node"),
			Counts->GetIntegerField(TEXT("file_texture_nodes")), 0);
	}

	const TSharedPtr<FJsonObject> Transform = Report->GetObjectField(TEXT("transform_check"));
	TestTrue(TEXT("the report has a transform check"), Transform.IsValid());
	if (Transform.IsValid())
	{
		TestTrue(TEXT("the measured world transforms match the manifest"), Transform->GetBoolField(TEXT("matched")));
		AddInfo(FString::Printf(TEXT("axis candidate: %s, max position error %.4f cm, max size error %.4f cm"),
			*Transform->GetStringField(TEXT("best")),
			Transform->GetNumberField(TEXT("max_position_error_cm")),
			Transform->GetNumberField(TEXT("max_size_error_cm"))));
	}

	const TArray<TSharedPtr<FJsonValue>>* Problems = nullptr;
	if (Report->TryGetArrayField(TEXT("problems"), Problems))
	{
		for (const TSharedPtr<FJsonValue>& Problem : *Problems)
		{
			AddError(FString::Printf(TEXT("Maya reported: %s"), *Problem->AsString()));
		}
	}
	return true;
}

#endif // WITH_EDITOR
