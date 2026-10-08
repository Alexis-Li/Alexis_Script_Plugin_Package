// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "LandscapeProxy.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MtoUSceneRefFixture.h"
#include "MtoUSceneRefPeer.h"
#include "MtoUSceneRefScope.h"
#include "MtoUSceneRefTransfer.h"
#include "Serialization/JsonSerializer.h"
#include "WorldPartition/WorldPartition.h"

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

	/** The object record one actor label produced, or nullptr. */
	const FMtoUSceneRefObject* FindObject(const FMtoUSceneRefResolution& Resolution, const TCHAR* ActorLabel)
	{
		return Resolution.Objects.FindByPredicate([ActorLabel](const FMtoUSceneRefObject& Object)
		{
			return Object.ActorLabel == ActorLabel;
		});
	}

	/** The actor of the loaded world one label names, or nullptr. */
	const AActor* FindActor(const FMtoUSceneRefResolution& Resolution, const TCHAR* ActorLabel)
	{
		AActor* const* Found = Resolution.Actors.FindByPredicate([ActorLabel](const AActor* Actor)
		{
			return Actor != nullptr && Actor->GetActorLabel() == ActorLabel;
		});
		return Found != nullptr ? *Found : nullptr;
	}

	/** The report entry one label produced in either skip list, or nullptr. */
	const FMtoUSceneRefSkipped* FindSkipped(const TArray<FMtoUSceneRefSkipped>& Records, const TCHAR* ActorLabel)
	{
		return Records.FindByPredicate([ActorLabel](const FMtoUSceneRefSkipped& Entry)
		{
			return Entry.ActorLabel == ActorLabel;
		});
	}

	int32 SumTriangles(const FMtoUSceneRefResolution& Resolution)
	{
		int32 Sum = 0;
		for (const FMtoUSceneRefObject& Object : Resolution.Objects)
		{
			if (Object.bExported)
			{
				Sum += Object.Triangles;
			}
		}
		return Sum;
	}

	int32 SumVertices(const FMtoUSceneRefResolution& Resolution)
	{
		int32 Sum = 0;
		for (const FMtoUSceneRefObject& Object : Resolution.Objects)
		{
			if (Object.bExported)
			{
				Sum += Object.Vertices;
			}
		}
		return Sum;
	}

	/** The manifest's object record for one actor label, or an invalid pointer. */
	TSharedPtr<FJsonObject> FindManifestObject(const TSharedPtr<FJsonObject>& Manifest, const TCHAR* ActorLabel)
	{
		if (!Manifest.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("objects"), Objects) || Objects == nullptr)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Objects)
		{
			const TSharedPtr<FJsonObject> Record = Value->AsObject();
			if (Record.IsValid() && Record->GetStringField(TEXT("actor")) == ActorLabel)
			{
				return Record;
			}
		}
		return nullptr;
	}

	/** Loads one of the sample levels; the caller restores the host world afterwards. */
	bool LoadSampleLevel(FAutomationTestBase& Test, const FString& Package)
	{
		FString Error;
		if (!FMtoUSceneRefFixture::LoadFixtureLevel(Package, Error))
		{
			Test.AddError(Error);
			return false;
		}
		return true;
	}

	/** Puts the host level back so the tests that follow a sample test see the fixture as built. */
	void RestoreHost(FAutomationTestBase& Test)
	{
		TArray<FString> Notes;
		FString Error;
		if (!FMtoUSceneRefFixture::RestoreHostWorld(Notes, Error))
		{
			Test.AddError(FString::Printf(TEXT("cannot restore the host world: %s"), *Error));
			return;
		}
		for (const FString& Note : Notes)
		{
			Test.AddInfo(Note);
		}
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
	FMtoUSceneRefNaniteTest,
	"MtoUSceneRefPrototype.NaniteSourceMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefNaniteTest::RunTest(const FString& Parameters)
{
	TArray<FString> Notes;
	if (!EnsureFixture(*this, Notes))
	{
		return false;
	}
	for (const FString& Note : Notes)
	{
		AddInfo(Note);
	}

	// The flag, read back from the asset the fixture duplicated and placed.
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, FMtoUSceneRefFixture::NaniteMeshPackage());
	TestNotNull(TEXT("the Nanite sample mesh exists under the fixture folder"), Mesh);
	if (Mesh != nullptr)
	{
		TestTrue(TEXT("the Nanite sample is a copy of the engine cube, not the engine asset"),
			Mesh->GetPathName() != TEXT("/Engine/BasicShapes/Cube.Cube"));
		TestTrue(TEXT("the mesh reads back with NaniteSettings.bEnabled"), Mesh->GetNaniteSettings().bEnabled);
		TestTrue(TEXT("the mesh reads back as Nanite enabled"), Mesh->IsNaniteEnabled());
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		AddInfo(FString::Printf(
			TEXT("nanite read back: mesh=%s bEnabled=%s is_nanite_enabled=%s lod0_triangles=%d lod0_vertices=%d source_model=%s hi_res_mesh_description=%s"),
			*Mesh->GetPathName(),
			Mesh->GetNaniteSettings().bEnabled ? TEXT("true") : TEXT("false"),
			Mesh->IsNaniteEnabled() ? TEXT("true") : TEXT("false"),
			RenderData != nullptr && RenderData->LODResources.Num() > 0 ? RenderData->LODResources[0].GetNumTriangles() : 0,
			RenderData != nullptr && RenderData->LODResources.Num() > 0 ? RenderData->LODResources[0].GetNumVertices() : 0,
			Mesh->IsSourceModelValid(0) ? TEXT("valid") : TEXT("absent"),
			Mesh->IsHiResMeshDescriptionValid() ? TEXT("valid") : TEXT("absent")));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	FMtoUSceneRefResolution Resolution;
	TestTrue(TEXT("the fixture scope resolves"),
		FMtoUSceneRefScope::Resolve(*World, FMtoUSceneRefFixture::MainScope(), Resolution));

	const FMtoUSceneRefObject* Object = FindObject(Resolution, FMtoUSceneRefFixture::NaniteActorLabel());
	TestNotNull(TEXT("the scope carries the Nanite sample actor"), FindActor(Resolution, FMtoUSceneRefFixture::NaniteActorLabel()));
	TestNotNull(TEXT("the scope carries a record for the Nanite sample"), Object);
	if (Object == nullptr)
	{
		return false;
	}
	TestTrue(TEXT("the Nanite sample record names the fixture mesh"),
		Object->MeshPath.Contains(TEXT("SM_NaniteCube")));
	TestTrue(TEXT("the scope counts render geometry for the Nanite sample"), Object->Triangles > 0);
	AddInfo(FString::Printf(TEXT("nanite scope record: id=%s node=%s category=%s mesh=%s triangles=%d vertices=%d exported=%s"),
		*Object->Id, *Object->NodeName, FMtoUSceneRefScope::CategoryName(Object->Category),
		*Object->MeshPath, Object->Triangles, Object->Vertices, Object->bExported ? TEXT("true") : TEXT("false")));

	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("nanite"));
	FMtoUSceneRefTransferResult Result;
	if (!RunTransfer(*this, FMtoUSceneRefFixture::MainScope(), Directory, Result))
	{
		return false;
	}

	// What the export actually wrote: the node the manifest predicted, and the warnings.
	const FMtoUSceneRefObject* Exported = FindObject(Result.Resolution, FMtoUSceneRefFixture::NaniteActorLabel());
	TestNotNull(TEXT("the export's resolution carries the Nanite sample"), Exported);
	if (Exported != nullptr)
	{
		TestTrue(TEXT("the file holds the node the manifest predicted for the Nanite sample"),
			Result.Output.NodeNames.Contains(Exported->NodeName));
		TestEqual(TEXT("the object count the run reports does not change between resolve and export"),
			Exported->Triangles, Object->Triangles);
	}
	AddInfo(FString::Printf(TEXT("nanite export: objects=%d triangles=%d vertices=%d nodes=[%s] warnings=%d"),
		Result.Resolution.ExportedObjectCount(), Result.TriangleCount, Result.VertexCount,
		*FString::Join(Result.Output.NodeNames, TEXT(", ")), Result.Warnings.Num()));
	for (const FString& Warning : Result.Warnings)
	{
		AddInfo(FString::Printf(TEXT("nanite export warning: %s"), *Warning));
	}

	// The manifest is the cross-host contract: the same numbers the scope reported have to
	// be in it, because the Maya peer matches its objects against the file by name.
	TSharedPtr<FJsonObject> Manifest;
	const FString ManifestPath = FPaths::Combine(Directory, Result.Resolution.ScopeName + TEXT(".manifest.json"));
	TestTrue(TEXT("the manifest parses"), LoadJson(ManifestPath, Manifest));
	const TSharedPtr<FJsonObject> Record = FindManifestObject(Manifest, FMtoUSceneRefFixture::NaniteActorLabel());
	TestTrue(TEXT("the manifest holds the Nanite sample record"), Record.IsValid());
	if (Record.IsValid() && Object != nullptr)
	{
		TestEqual(TEXT("the manifest records the mesh the scope resolved"),
			Record->GetStringField(TEXT("mesh")), Object->MeshPath);
		TestEqual(TEXT("the manifest records the node the scope predicted"),
			Record->GetStringField(TEXT("node_name")), Object->NodeName);
		TestEqual(TEXT("the manifest records the triangles the scope counted"),
			Record->GetIntegerField(TEXT("triangles")), Object->Triangles);
		TestEqual(TEXT("the manifest records the vertices the scope counted"),
			Record->GetIntegerField(TEXT("vertices")), Object->Vertices);
		TestTrue(TEXT("the manifest marks the Nanite sample exported"), Record->GetBoolField(TEXT("exported")));
		if (const TSharedPtr<FJsonObject> Scale = Manifest->GetObjectField(TEXT("scale")))
		{
			TestEqual(TEXT("the manifest's triangle total is the scope's"),
				Scale->GetIntegerField(TEXT("triangles")), Result.TriangleCount);
			TestEqual(TEXT("the manifest's object total is the scope's"),
				Scale->GetIntegerField(TEXT("objects")), Result.Resolution.ExportedObjectCount());
		}
	}
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
	// The media counts are the handoff's informational facts: the main scope has no
	// material input and no camera or light, and nothing is embedded in the file.
	TestEqual(TEXT("no content record reaches the file"), Result.Output.TextureScan.ContentRecords, 0);
	TestEqual(TEXT("nothing is embedded in the file"), Result.Output.TextureScan.EmbeddedMediaRecords, 0);
	TestEqual(TEXT("the embedded media list is empty"), Result.Output.TextureScan.EmbeddedMedia.Num(), 0);
	TestEqual(TEXT("the file carries no camera record"), Result.Output.TextureScan.CameraRecords, 0);
	TestEqual(TEXT("the file carries no light record"), Result.Output.TextureScan.LightRecords, 0);

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
			TestEqual(TEXT("the manifest records no content record"),
				Output->GetIntegerField(TEXT("content_records")), 0);
			TestEqual(TEXT("the manifest records no embedded media"),
				Output->GetIntegerField(TEXT("embedded_media_records")), 0);
			TestEqual(TEXT("the manifest's embedded media list is empty"),
				Output->GetArrayField(TEXT("embedded_media")).Num(), 0);
			TestEqual(TEXT("the manifest records no camera record"),
				Output->GetIntegerField(TEXT("camera_records")), 0);
			TestEqual(TEXT("the manifest records no light record"),
				Output->GetIntegerField(TEXT("light_records")), 0);
		}
		// The convention block is the cross-host contract for the axis the file is in; the
		// Maya side applies exactly this map, so the manifest has to name it.
		const TSharedPtr<FJsonObject> Conventions = Manifest->GetObjectField(TEXT("conventions"));
		TestTrue(TEXT("the manifest convention block exists"), Conventions.IsValid());
		if (Conventions.IsValid())
		{
			TestEqual(TEXT("the handoff is the engine's FBX level export"),
				Conventions->GetStringField(TEXT("handoff")), FString(TEXT("engine_fbx_level_export")));
			TestEqual(TEXT("the axis option is the engine default"),
				Conventions->GetStringField(TEXT("export_axis_option")), FString(TEXT("bForceFrontXAxis=false")));
			TestEqual(TEXT("the point map is the measured engine handoff map"),
				Conventions->GetStringField(TEXT("engine_to_maya_point_map")),
				FString(TEXT("maya_x=ue_x, maya_y=ue_z, maya_z=ue_y")));
			TestEqual(TEXT("the point map mirrors, so its determinant is -1"),
				Conventions->GetIntegerField(TEXT("engine_to_maya_determinant")), -1);
		}
		// The filter block is the resolver's prediction of the components the export keeps
		// out; the main scope's only non geometry actor has no exportable component, so it
		// names no component at all.
		const TSharedPtr<FJsonObject> Filter = Manifest->GetObjectField(TEXT("filter"));
		TestTrue(TEXT("the manifest filter block exists"), Filter.IsValid());
		if (Filter.IsValid())
		{
			TestEqual(TEXT("the filter policy is static mesh components only"),
				Filter->GetStringField(TEXT("policy")), FString(TEXT("static_mesh_components_only")));
			TestEqual(TEXT("the filter count matches the resolver's list"),
				Filter->GetIntegerField(TEXT("suppressed_count")), Result.Resolution.SuppressedComponents.Num());
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
	FMtoUSceneRefMixedFilterTest,
	"MtoUSceneRefPrototype.MixedBlueprintFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefMixedFilterTest::RunTest(const FString& Parameters)
{
	TArray<FString> Notes;
	if (!EnsureFixture(*this, Notes))
	{
		return false;
	}

	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("mixed"));
	FMtoUSceneRefTransferResult Result;
	if (!RunTransfer(*this, FMtoUSceneRefFixture::MainScope(), Directory, Result))
	{
		return false;
	}

	AActor* const* MixedActorEntry = Result.Resolution.Actors.FindByPredicate([](const AActor* Actor)
	{
		return Actor != nullptr && Actor->GetActorLabel() == TEXT("BP_SceneRefMixed");
	});
	TestTrue(TEXT("the fixture carries the mixed Blueprint actor"),
		MixedActorEntry != nullptr && *MixedActorEntry != nullptr);
	if (MixedActorEntry == nullptr || *MixedActorEntry == nullptr)
	{
		return false;
	}
	const AActor& MixedActor = **MixedActorEntry;
	// The skeletal sample is the one component the fixture may have had to skip, so the
	// expectation follows what the fixture actually built.
	const bool bMixedHasSkeletalMesh = MixedActor.FindComponentByClass<USkeletalMeshComponent>() != nullptr;
	if (!bMixedHasSkeletalMesh)
	{
		AddInfo(TEXT("no engine skeletal mesh asset exists on this install, so the mixed sample has no skeletal mesh component"));
	}

	auto SuppressedCountFor = [&Result](const TCHAR* ActorLabel, const TCHAR* Reason)
	{
		int32 Count = 0;
		for (const FMtoUSceneRefSuppressedComponent& Record : Result.Resolution.SuppressedComponents)
		{
			if (Record.ActorLabel == ActorLabel && Record.Reason == Reason)
			{
				++Count;
			}
		}
		return Count;
	};

	// The resolver names every engine export branch of the mixed actor except its mesh.
	TestEqual(TEXT("the light component is named for suppression"),
		SuppressedCountFor(TEXT("BP_SceneRefMixed"), TEXT("light component")), 1);
	TestEqual(TEXT("the camera component is named for suppression"),
		SuppressedCountFor(TEXT("BP_SceneRefMixed"), TEXT("camera component")), 1);
	TestEqual(TEXT("the child actor component is named for suppression"),
		SuppressedCountFor(TEXT("BP_SceneRefMixed"), TEXT("child actor component")), 1);
	TestEqual(TEXT("the skeletal mesh sample is named for suppression when the fixture built it"),
		SuppressedCountFor(TEXT("BP_SceneRefMixed"), TEXT("skeletal mesh component")), bMixedHasSkeletalMesh ? 1 : 0);

	// Every record names a real component and carries one of the stable reasons; the export
	// applied exactly the resolver's list.
	for (const FMtoUSceneRefSuppressedComponent& Record : Result.Resolution.SuppressedComponents)
	{
		TestTrue(FString::Printf(TEXT("the record %s/%s carries a stable reason"),
				*Record.ActorLabel, *Record.ComponentName),
			Record.Reason == TEXT("skeletal mesh component")
				|| Record.Reason == TEXT("camera component")
				|| Record.Reason == TEXT("light component")
				|| Record.Reason == TEXT("child actor component"));
		TestTrue(FString::Printf(TEXT("the record %s/%s names its actor, component and level"),
				*Record.ActorLabel, *Record.ComponentName),
			!Record.ActorLabel.IsEmpty() && !Record.ComponentName.IsEmpty()
				&& !Record.ComponentClass.IsEmpty() && !Record.LevelPackage.IsEmpty());
	}
	TestTrue(TEXT("the transfer suppressed components"), Result.SuppressedComponentCount > 0);
	TestEqual(TEXT("the export applied exactly the resolver's list"),
		Result.SuppressedComponentCount, Result.Resolution.SuppressedComponents.Num());

	// The produced file: the kept mesh is a node of its own, the suppressed components and
	// the whole child actor subtree are not.
	TestEqual(TEXT("the file carries no camera record"), Result.Output.TextureScan.CameraRecords, 0);
	TestEqual(TEXT("the file carries no light record"), Result.Output.TextureScan.LightRecords, 0);
	TestEqual(TEXT("nothing is embedded in the file"), Result.Output.TextureScan.EmbeddedMediaRecords, 0);
	TestEqual(TEXT("no image file is written"), Result.Output.ImageFiles.Num(), 0);
	TestTrue(TEXT("the mixed actor's kept mesh is a node in the file"),
		Result.Output.NodeNames.Contains(TEXT("BP_SceneRefMixed")));
	for (const FString& NodeName : Result.Output.NodeNames)
	{
		TestFalse(FString::Printf(TEXT("no node carries the child blueprint's mesh (%s)"), *NodeName),
			NodeName.Contains(TEXT("SceneRefChildMesh")));
	}

	// A child actor is exported through its parent's child actor component or not at all;
	// the transfer never selects it as an actor of its own.
	for (const FMtoUSceneRefObject& Object : Result.Resolution.Objects)
	{
		TestFalse(TEXT("no object comes from the child blueprint"),
			Object.ActorClass.Contains(TEXT("SceneRefChildMesh")));
	}
	const AActor* ChildActorEntry = nullptr;
	if (AActor* const* Found = Result.Resolution.Actors.FindByPredicate([](const AActor* Actor)
		{
			return Actor != nullptr && Actor->IsChildActor();
		}))
	{
		ChildActorEntry = *Found;
	}
	TestTrue(TEXT("the world holds the child actor instance the filter proves exists"), ChildActorEntry != nullptr);
	if (ChildActorEntry != nullptr)
	{
		TestTrue(TEXT("the child actor is reported as skipped rather than exported"),
			Result.Resolution.Skipped.ContainsByPredicate([ChildActorEntry](const FMtoUSceneRefSkipped& Entry)
				{
					return Entry.ActorLabel == ChildActorEntry->GetActorLabel()
						&& Entry.Reason.Contains(TEXT("child actor"));
				}));
	}

	// The manifest filter block is the same prediction the export applied.
	TSharedPtr<FJsonObject> Manifest;
	const FString ManifestPath = FPaths::Combine(Directory, Result.Resolution.ScopeName + TEXT(".manifest.json"));
	TestTrue(TEXT("the manifest parses"), LoadJson(ManifestPath, Manifest));
	if (Manifest.IsValid())
	{
		const TSharedPtr<FJsonObject> Filter = Manifest->GetObjectField(TEXT("filter"));
		TestTrue(TEXT("the manifest filter block exists"), Filter.IsValid());
		if (Filter.IsValid())
		{
			TestEqual(TEXT("the filter policy is static mesh components only"),
				Filter->GetStringField(TEXT("policy")), FString(TEXT("static_mesh_components_only")));
			TestEqual(TEXT("the filter count matches the resolver's list"),
				Filter->GetIntegerField(TEXT("suppressed_count")), Result.Resolution.SuppressedComponents.Num());

			const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
			TestTrue(TEXT("the manifest lists the suppressed components"),
				Filter->TryGetArrayField(TEXT("suppressed_components"), Records) && Records != nullptr);
			if (Records != nullptr)
			{
				int32 MixedRecords = 0;
				TSet<FString> MixedReasons;
				for (const TSharedPtr<FJsonValue>& Value : *Records)
				{
					const TSharedPtr<FJsonObject> Record = Value->AsObject();
					if (!Record.IsValid() || Record->GetStringField(TEXT("actor")) != TEXT("BP_SceneRefMixed"))
					{
						continue;
					}
					++MixedRecords;
					MixedReasons.Add(Record->GetStringField(TEXT("reason")));
					TestTrue(TEXT("the manifest record names the component, its class and its level"),
						!Record->GetStringField(TEXT("component")).IsEmpty()
							&& !Record->GetStringField(TEXT("class")).IsEmpty()
							&& !Record->GetStringField(TEXT("level")).IsEmpty());
				}
				TestEqual(TEXT("the manifest lists the mixed actor's suppressed components"),
					MixedRecords, bMixedHasSkeletalMesh ? 4 : 3);
				TestTrue(TEXT("the manifest lists the light component"),
					MixedReasons.Contains(FString(TEXT("light component"))));
				TestTrue(TEXT("the manifest lists the camera component"),
					MixedReasons.Contains(FString(TEXT("camera component"))));
				TestTrue(TEXT("the manifest lists the child actor component"),
					MixedReasons.Contains(FString(TEXT("child actor component"))));
				TestEqual(TEXT("the manifest lists the skeletal mesh component when the fixture built it"),
					MixedReasons.Contains(FString(TEXT("skeletal mesh component"))), bMixedHasSkeletalMesh);
			}
		}
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
	const FString EmbeddedPath = FPaths::Combine(Directory, TEXT("embedded.fbx"));

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
	// The layout the FBX SDK writes: both records hold a nested Properties70 block before
	// the name lines, the video record embeds its payload on the line after `Content: ,`,
	// and cameras and lights are NodeAttribute records carrying their class token.
	const FString EmbeddedSample = TEXT(
		"; FBX 7.7.0 project file\n"
		"Objects:  {\n"
		"\tVideo: 2146348041952, \"Video::probeFile\", \"Clip\" {\n"
		"\t\tType: \"Clip\"\n"
		"\t\tProperties70:  {\n"
		"\t\t\tP: \"Path\", \"KString\", \"XRefUrl\", \"\", \"textures/probe2.png\"\n"
		"\t\t}\n"
		"\t\tUseMipMap: 0\n"
		"\t\tFilename: \"textures/probe2.png\"\n"
		"\t\tRelativeFilename: \"probe2.png\"\n"
		"\t\tContent: ,\n"
		" \"iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAADElEQVR4nGOYgAMAAKjZDYG3L0AqAAAAAElFTkSuQmCC\"\n"
		"\t}\n"
		"\tTexture: 2146348036672, \"Texture::probeFile\", \"\" {\n"
		"\t\tType: \"TextureVideoClip\"\n"
		"\t\tTextureName: \"Texture::probeFile\"\n"
		"\t\tProperties70:  {\n"
		"\t\t\tP: \"UVSet\", \"KString\", \"\", \"\", \"map1\"\n"
		"\t\t}\n"
		"\t\tMedia: \"Video::probeFile\"\n"
		"\t\tFileName: \"textures/probe2.png\"\n"
		"\t\tRelativeFilename: \"probe2.png\"\n"
		"\t}\n"
		"\tNodeAttribute: 2146509044704, \"NodeAttribute::probe_cam1\", \"Camera\" {\n"
		"\t\tTypeFlags: \"Camera\"\n"
		"\t}\n"
		"\tNodeAttribute: 2146493446304, \"NodeAttribute::\", \"Light\" {\n"
		"\t\tTypeFlags: \"Light\"\n"
		"\t}\n"
		"}\n");

	TestTrue(TEXT("the textured sample file is written"),
		FFileHelper::SaveStringToFile(TexturedSample, *TexturedPath));
	TestTrue(TEXT("the plain sample file is written"),
		FFileHelper::SaveStringToFile(PlainSample, *PlainPath));
	TestTrue(TEXT("the embedded sample file is written"),
		FFileHelper::SaveStringToFile(EmbeddedSample, *EmbeddedPath));

	TArray<FString> Files;
	FMtoUSceneRefTextureScan Scan;
	FMtoUSceneRefTransfer::ScanFbxTextureReferences(TexturedPath, Scan);
	TestEqual(TEXT("the detector finds the texture record"), Scan.TextureRecords, 1);
	TestEqual(TEXT("the detector finds the texture reference"), Scan.TextureReferences, 1);
	Files = Scan.FileNames;
	TestTrue(TEXT("the detector reports the recorded file name"),
		Files.Contains(TEXT("Assets/T_Rock.png")) && Files.Contains(TEXT("T_Rock.png")));
	TestEqual(TEXT("a record without a Content line carries no content record"), Scan.ContentRecords, 0);
	TestEqual(TEXT("a record without a Content line embeds no media"), Scan.EmbeddedMediaRecords, 0);

	TArray<FString> Names;
	FMtoUSceneRefTransfer::ScanFbxModelNames(TexturedPath, Names);
	TestTrue(TEXT("the detector finds the model name"), Names.Contains(TEXT("SM_Rock")));

	FMtoUSceneRefTextureScan PlainScan;
	FMtoUSceneRefTransfer::ScanFbxTextureReferences(PlainPath, PlainScan);
	TestEqual(TEXT("a geometry only file reports no texture record"), PlainScan.TextureRecords, 0);
	TestEqual(TEXT("a geometry only file records no file name"), PlainScan.FileNames.Num(), 0);
	TestEqual(TEXT("a geometry only file reports no camera record"), PlainScan.CameraRecords, 0);
	TestEqual(TEXT("a geometry only file reports no light record"), PlainScan.LightRecords, 0);

	FMtoUSceneRefTextureScan EmbeddedScan;
	FMtoUSceneRefTransfer::ScanFbxTextureReferences(EmbeddedPath, EmbeddedScan);
	TestEqual(TEXT("the video record is counted"), EmbeddedScan.VideoRecords, 1);
	TestEqual(TEXT("the texture record is counted"), EmbeddedScan.TextureRecords, 1);
	// Both records name a file below their nested block; only the texture record feeds the
	// reference count, which is what an import would try to load.
	TestEqual(TEXT("the texture record is counted as naming a file"), EmbeddedScan.TextureReferences, 1);
	TestEqual(TEXT("both file name heads are recorded"),
		EmbeddedScan.FileNames.Num(), 2);
	TestEqual(TEXT("the embedded Content line is counted"), EmbeddedScan.ContentRecords, 1);
	TestEqual(TEXT("the Content line with a payload is an embedded media record"),
		EmbeddedScan.EmbeddedMediaRecords, 1);
	TestTrue(TEXT("the descriptor names the record and the payload size"),
		EmbeddedScan.EmbeddedMedia.Contains(FString(TEXT("Video::probeFile (Content 92 chars)"))));
	TestEqual(TEXT("the camera NodeAttribute is counted"), EmbeddedScan.CameraRecords, 1);
	TestEqual(TEXT("the light NodeAttribute is counted"), EmbeddedScan.LightRecords, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUSceneRefWorldPartitionTest,
	"MtoUSceneRefPrototype.WorldPartitionScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefWorldPartitionTest::RunTest(const FString& Parameters)
{
	TArray<FString> Notes;
	if (!EnsureFixture(*this, Notes))
	{
		return false;
	}
	for (const FString& Note : Notes)
	{
		AddInfo(Note);
	}
	if (!LoadSampleLevel(*this, FMtoUSceneRefFixture::PartitionedLevelPackage()))
	{
		RestoreHost(*this);
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	UWorldPartition* Partition = World != nullptr ? World->GetWorldPartition() : nullptr;
	TestNotNull(TEXT("the sample level from the engine's OpenWorld template is a World Partition world"), Partition);
	if (World == nullptr || Partition == nullptr)
	{
		RestoreHost(*this);
		return false;
	}

	// The state the level comes in with: an editor session with streaming enabled and, in a
	// headless run, no streaming source, so the level's cells are not streamed in.
	AddInfo(FString::Printf(
		TEXT("partitioned world: streaming_enabled=%s enabled_in_editor=%s initialized=%s levels=%d streaming_levels=%d"),
		Partition->IsStreamingEnabled() ? TEXT("true") : TEXT("false"),
		Partition->IsStreamingEnabledInEditor() ? TEXT("true") : TEXT("false"),
		Partition->IsInitialized() ? TEXT("true") : TEXT("false"),
		World->GetNumLevels(), World->GetStreamingLevels().Num()));
	for (ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		if (Streaming != nullptr)
		{
			AddInfo(FString::Printf(TEXT("partitioned streaming level: %s class=%s loaded=%s visible=%s"),
				*Streaming->GetWorldAssetPackageName(),
				*Streaming->GetClass()->GetName(),
				Streaming->IsLevelLoaded() ? TEXT("true") : TEXT("false"),
				Streaming->GetShouldBeVisibleFlag() ? TEXT("true") : TEXT("false")));
		}
	}

	const FMtoUSceneRefScopeSpec Spec = FMtoUSceneRefFixture::PartitionedScope();

	// Phase one, as the fixture leaves it: a cell that is not streamed in is simply absent
	// from the loaded world, and the scope can only carry the actors the world holds.
	FMtoUSceneRefResolution AsAuthored;
	TestTrue(TEXT("the partitioned scope resolves"), FMtoUSceneRefScope::Resolve(*World, Spec, AsAuthored));
	TestTrue(TEXT("the scope reports a World Partition level"), AsAuthored.bWorldPartition);
	int32 PlacedInWorld = 0;
	for (int32 Index = 0; Index < FMtoUSceneRefFixture::PartitionActorCount(); ++Index)
	{
		const TCHAR* Label = FMtoUSceneRefFixture::PartitionActorLabel(Index);
		const AActor* Actor = FindActor(AsAuthored, Label);
		const FMtoUSceneRefObject* Record = FindObject(AsAuthored, Label);
		PlacedInWorld += Actor != nullptr ? 1 : 0;
		AddInfo(FString::Printf(TEXT("partitioned cell check: %s in_loaded_world=%s scope_object=%s"),
			Label,
			Actor != nullptr ? TEXT("true") : TEXT("false"),
			Record != nullptr ? TEXT("true") : TEXT("false")));
		TestEqual(FString::Printf(TEXT("the scope carries an object for %s exactly when the world holds it"), Label),
			Record != nullptr, Actor != nullptr);
	}
	AddInfo(FString::Printf(
		TEXT("partitioned scope as streamed: actors=%d placed_actors_in_world=%d objects=%d triangles=%d vertices=%d streaming_levels=%d"),
		AsAuthored.Actors.Num(), PlacedInWorld, AsAuthored.ExportedObjectCount(),
		SumTriangles(AsAuthored), SumVertices(AsAuthored), World->GetStreamingLevels().Num()));
	TestEqual(TEXT("a cell that is not streamed in contributes no actor to the loaded world"),
		PlacedInWorld, 0);

	// The scope states its own completeness instead of leaving an empty unloaded-sublevel list
	// to be read as full coverage, and the inventory behind that statement is read from the
	// partition's actor descriptors without loading anything.
	const FMtoUSceneRefWorldPartitionScope& Inventory = AsAuthored.WorldPartitionScope;
	TestEqual(TEXT("a scope whose cells are not streamed in reports its completeness as unconfirmed"),
		AsAuthored.Completeness, EMtoUSceneRefScopeCompleteness::NotConfirmed);
	TestTrue(TEXT("the partition's actor descriptors were readable without loading anything"),
		Inventory.bInventoryAvailable);
	TestTrue(TEXT("the inventory walked at least the partition's own container"), Inventory.Containers >= 1);
	TestEqual(TEXT("the inventory splits the descriptors into spawned, unspawned authored actors and generated proxies"),
		Inventory.LoadedActorDescriptors + Inventory.UnloadedActorCount +
		Inventory.UnloadedHlodCount, Inventory.ActorDescriptors);
	TestTrue(TEXT("the inventory names at least the placed actors the loaded world is missing"),
		Inventory.UnloadedActorCount >= FMtoUSceneRefFixture::PartitionActorCount());
	TestEqual(TEXT("the recorded path list holds every unspawned descriptor below the cap"),
		Inventory.UnloadedActors.Num(),
		FMath::Min(Inventory.UnloadedActorCount, MtoUSceneRefWorldPartitionInventoryLimit));
	AddInfo(FString::Printf(
		TEXT("partition inventory as streamed: descriptors=%d spawned=%d unspawned_authored=%d unspawned_hlod=%d containers=%d recorded=%d truncated=%s note=%s"),
		Inventory.ActorDescriptors, Inventory.LoadedActorDescriptors, Inventory.UnloadedActorCount,
		Inventory.UnloadedHlodCount, Inventory.Containers, Inventory.UnloadedActors.Num(),
		Inventory.bInventoryTruncated ? TEXT("true") : TEXT("false"), *Inventory.Note));
	for (const FString& Unspawned : Inventory.UnloadedActors)
	{
		AddInfo(FString::Printf(TEXT("partition inventory unspawned: %s"), *Unspawned));
	}

	// The review's own reproduction: with the cells not streamed in, this is the handoff the
	// export produces, and its manifest is the artifact that has to say what it covers.
	{
		const FString AsStreamedDirectory = FPaths::Combine(EvidenceDirectory(), TEXT("partitioned-as-streamed"));
		FMtoUSceneRefTransferResult AsStreamed;
		if (!RunTransfer(*this, Spec, AsStreamedDirectory, AsStreamed))
		{
			RestoreHost(*this);
			return false;
		}
		AddInfo(FString::Printf(TEXT("partitioned-as-streamed export: objects=%d nodes=[%s] warnings=%d"),
			AsStreamed.Resolution.ExportedObjectCount(),
			*FString::Join(AsStreamed.Output.NodeNames, TEXT(", ")), AsStreamed.Warnings.Num()));
		for (const FString& Warning : AsStreamed.Warnings)
		{
			AddInfo(FString::Printf(TEXT("partitioned-as-streamed warning: %s"), *Warning));
		}
		TestEqual(TEXT("the loaded-only scope reports itself as not confirmed"),
			AsStreamed.Resolution.Completeness, EMtoUSceneRefScopeCompleteness::NotConfirmed);
		TestTrue(TEXT("the loaded-only run carries the SCOPE_LOADED_ONLY warning"),
			AsStreamed.Warnings.ContainsByPredicate([](const FString& Warning)
			{
				return Warning.Contains(TEXT("SCOPE_LOADED_ONLY"));
			}));
		TSharedPtr<FJsonObject> AsStreamedManifest;
		const FString AsStreamedManifestPath = FPaths::Combine(
			AsStreamedDirectory, AsStreamed.Resolution.ScopeName + TEXT(".manifest.json"));
		TestTrue(TEXT("the loaded-only manifest parses"), LoadJson(AsStreamedManifestPath, AsStreamedManifest));
		if (AsStreamedManifest.IsValid())
		{
			const TSharedPtr<FJsonObject> ScopeBlock = AsStreamedManifest->GetObjectField(TEXT("scope"));
			TestTrue(TEXT("the loaded-only manifest carries the scope block"), ScopeBlock.IsValid());
			if (ScopeBlock.IsValid())
			{
				TestEqual(TEXT("the loaded-only manifest says its completeness is not confirmed"),
					ScopeBlock->GetStringField(TEXT("completeness")), TEXT("not_confirmed"));
				TestEqual(TEXT("the loaded-only manifest names what it covers"),
					ScopeBlock->GetStringField(TEXT("coverage")), TEXT("partition_loaded_content"));
				const TSharedPtr<FJsonObject> PartitionBlock = ScopeBlock->GetObjectField(TEXT("world_partition"));
				TestTrue(TEXT("the loaded-only manifest carries the partition inventory"), PartitionBlock.IsValid());
				if (PartitionBlock.IsValid())
				{
					TestTrue(TEXT("the inventory marks the partition detected"),
						PartitionBlock->GetBoolField(TEXT("detected")));
					TestTrue(TEXT("the inventory marks itself available"),
						PartitionBlock->GetBoolField(TEXT("inventory_available")));
					TestEqual(TEXT("the inventory's descriptor total is the scope's"),
						PartitionBlock->GetIntegerField(TEXT("actor_descriptors")),
						AsStreamed.Resolution.WorldPartitionScope.ActorDescriptors);
					TestTrue(TEXT("the inventory counts the authored actors that are not spawned"),
						PartitionBlock->GetIntegerField(TEXT("unloaded_actor_count")) >= 1);
					TestTrue(TEXT("the inventory counts the generated proxies that are not spawned"),
						PartitionBlock->GetIntegerField(TEXT("unloaded_hlod_count")) >= 0);
					const TArray<TSharedPtr<FJsonValue>>* Unspawned = nullptr;
					TestTrue(TEXT("the inventory records the unspawned actor paths"),
						PartitionBlock->TryGetArrayField(TEXT("unloaded_actors"), Unspawned) &&
						Unspawned != nullptr && Unspawned->Num() >= 1);
				}
			}
		}
	}

	// Phase two, the state the export needs: with editor streaming off every actor desc
	// becomes non spatially loaded (WorldPartition.cpp: SetForceNonSpatiallyLoaded(
	// !IsStreamingEnabledInEditor())), so the level's content is in the world and the
	// transfer can report and export it.
	Partition->SetEnableStreaming(false);
	World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
	AddInfo(FString::Printf(
		TEXT("partitioned world after SetEnableStreaming(false): streaming_enabled=%s enabled_in_editor=%s levels=%d streaming_levels=%d"),
		Partition->IsStreamingEnabled() ? TEXT("true") : TEXT("false"),
		Partition->IsStreamingEnabledInEditor() ? TEXT("true") : TEXT("false"),
		World->GetNumLevels(), World->GetStreamingLevels().Num()));

	FMtoUSceneRefResolution Resolution;
	TestTrue(TEXT("the partitioned scope resolves with the level's content loaded"),
		FMtoUSceneRefScope::Resolve(*World, Spec, Resolution));
	PlacedInWorld = 0;
	for (int32 Index = 0; Index < FMtoUSceneRefFixture::PartitionActorCount(); ++Index)
	{
		const TCHAR* Label = FMtoUSceneRefFixture::PartitionActorLabel(Index);
		const AActor* Actor = FindActor(Resolution, Label);
		TestNotNull(FString::Printf(TEXT("the placed actor %s is in the loaded world at export time"), Label), Actor);
		const FMtoUSceneRefObject* Record = FindObject(Resolution, Label);
		TestNotNull(FString::Printf(TEXT("the scope carries an object for %s"), Label), Record);
		if (Actor != nullptr)
		{
			++PlacedInWorld;
		}
		if (Record != nullptr)
		{
			TestTrue(FString::Printf(TEXT("%s is exported"), Label), Record->bExported);
			TestEqual(FString::Printf(TEXT("%s is attributed to the partitioned level"), Label),
				Record->LevelPackage, FString(FMtoUSceneRefFixture::PartitionedLevelPackage()));
		}
	}
	AddInfo(FString::Printf(
		TEXT("partitioned scope loaded: actors=%d placed_actors_in_world=%d objects=%d triangles=%d vertices=%d loaded_sublevels=%d excluded_sublevels=%d unloaded_sublevels=%d"),
		Resolution.Actors.Num(), PlacedInWorld, Resolution.ExportedObjectCount(),
		SumTriangles(Resolution), SumVertices(Resolution),
		Resolution.LoadedSublevels.Num(), Resolution.ExcludedSublevels.Num(), Resolution.UnloadedSublevels.Num()));

	// With the level's content in the world the inventory finds nothing unspawned, and the
	// scope states its completeness instead of leaving a reader to infer it.
	const FMtoUSceneRefWorldPartitionScope& LoadedInventory = Resolution.WorldPartitionScope;
	AddInfo(FString::Printf(
		TEXT("partition inventory loaded: descriptors=%d spawned=%d unspawned_authored=%d unspawned_hlod=%d containers=%d truncated=%s note=%s"),
		LoadedInventory.ActorDescriptors, LoadedInventory.LoadedActorDescriptors,
		LoadedInventory.UnloadedActorCount, LoadedInventory.UnloadedHlodCount,
		LoadedInventory.Containers,
		LoadedInventory.bInventoryTruncated ? TEXT("true") : TEXT("false"), *LoadedInventory.Note));
	TestEqual(TEXT("every authored actor descriptor is spawned once the level's content is loaded"),
		LoadedInventory.UnloadedActorCount, 0);
	TestEqual(TEXT("the generated HLOD proxies are counted apart from the authored placements"),
		LoadedInventory.LoadedActorDescriptors + LoadedInventory.UnloadedHlodCount,
		LoadedInventory.ActorDescriptors);
	TestEqual(TEXT("a fully spawned partition scope reports its completeness as confirmed"),
		Resolution.Completeness, EMtoUSceneRefScopeCompleteness::Confirmed);

	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("partitioned"));
	FMtoUSceneRefTransferResult Result;
	if (!RunTransfer(*this, Spec, Directory, Result))
	{
		RestoreHost(*this);
		return false;
	}
	for (int32 Index = 0; Index < FMtoUSceneRefFixture::PartitionActorCount(); ++Index)
	{
		const TCHAR* Label = FMtoUSceneRefFixture::PartitionActorLabel(Index);
		const FMtoUSceneRefObject* Record = FindObject(Result.Resolution, Label);
		if (Record != nullptr)
		{
			TestTrue(FString::Printf(TEXT("the file holds the node the manifest predicted for %s"), Label),
				Result.Output.NodeNames.Contains(Record->NodeName));
		}
	}

	TSharedPtr<FJsonObject> Manifest;
	const FString ManifestPath = FPaths::Combine(Directory, Result.Resolution.ScopeName + TEXT(".manifest.json"));
	TestTrue(TEXT("the manifest parses"), LoadJson(ManifestPath, Manifest));
	if (Manifest.IsValid())
	{
		const TSharedPtr<FJsonObject> ManifestWorld = Manifest->GetObjectField(TEXT("world"));
		TestTrue(TEXT("the manifest carries the world block"), ManifestWorld.IsValid());
		if (ManifestWorld.IsValid())
		{
			TestTrue(TEXT("the manifest marks the world as partitioned"),
				ManifestWorld->GetBoolField(TEXT("world_partition")));
			TestEqual(TEXT("the manifest names the partitioned level"),
				ManifestWorld->GetStringField(TEXT("package")),
				FString(FMtoUSceneRefFixture::PartitionedLevelPackage()));
		}
		const TSharedPtr<FJsonObject> Scale = Manifest->GetObjectField(TEXT("scale"));
		if (Scale.IsValid())
		{
			TestEqual(TEXT("the manifest records the scope's actors"),
				Scale->GetIntegerField(TEXT("actors")), Resolution.Actors.Num());
			TestEqual(TEXT("the manifest records the scope's objects"),
				Scale->GetIntegerField(TEXT("objects")), Result.Resolution.ExportedObjectCount());
			TestEqual(TEXT("the manifest records the scope's triangles"),
				Scale->GetIntegerField(TEXT("triangles")), Result.TriangleCount);
		}
		const TSharedPtr<FJsonObject> ManifestScope = Manifest->GetObjectField(TEXT("scope"));
		TestTrue(TEXT("the manifest carries the scope coverage block"), ManifestScope.IsValid());
		if (ManifestScope.IsValid())
		{
			TestEqual(TEXT("the loaded manifest says its completeness is confirmed"),
				ManifestScope->GetStringField(TEXT("completeness")), TEXT("confirmed"));
			TestEqual(TEXT("the loaded manifest names what the scope covers"),
				ManifestScope->GetStringField(TEXT("coverage")), TEXT("partition_loaded_content"));
			const TSharedPtr<FJsonObject> PartitionBlock = ManifestScope->GetObjectField(TEXT("world_partition"));
			TestTrue(TEXT("the loaded manifest carries the partition inventory"), PartitionBlock.IsValid());
			if (PartitionBlock.IsValid())
			{
				TestTrue(TEXT("the loaded inventory marks the partition detected"),
					PartitionBlock->GetBoolField(TEXT("detected")));
				TestEqual(TEXT("the loaded inventory's descriptor total is the resolution's"),
					PartitionBlock->GetIntegerField(TEXT("actor_descriptors")),
					Resolution.WorldPartitionScope.ActorDescriptors);
				TestEqual(TEXT("the loaded inventory records no unspawned authored actor"),
					PartitionBlock->GetIntegerField(TEXT("unloaded_actor_count")), 0);
				TestEqual(TEXT("the loaded inventory counts the generated proxies apart"),
					PartitionBlock->GetIntegerField(TEXT("unloaded_hlod_count")),
					LoadedInventory.UnloadedHlodCount);
			}
		}
		// A confirmed scope is not reported as a gap: the warning belongs to the loaded-only case.
		TestTrue(TEXT("the loaded run carries no SCOPE_LOADED_ONLY warning"),
			!Result.Warnings.ContainsByPredicate([](const FString& Warning)
			{
				return Warning.Contains(TEXT("SCOPE_LOADED_ONLY"));
			}));
	}
	AddInfo(FString::Printf(TEXT("partitioned export: objects=%d triangles=%d vertices=%d nodes=[%s] warnings=%d"),
		Result.Resolution.ExportedObjectCount(), Result.TriangleCount, Result.VertexCount,
		*FString::Join(Result.Output.NodeNames, TEXT(", ")), Result.Warnings.Num()));

	// The switch is transactional and dirties the level by design. The fixture regenerates the
	// level on the next BuildFixture run, so the run leaves it unsaved and clears the dirty
	// flag the measurement set: the next level load has nothing to ask about.
	Partition->SetEnableStreaming(true);
	World->GetOutermost()->SetDirtyFlag(false);
	AddInfo(FString::Printf(TEXT("partitioned world restored: streaming_enabled=%s dirty=%s"),
		Partition->IsStreamingEnabled() ? TEXT("true") : TEXT("false"),
		World->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false")));

	RestoreHost(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUSceneRefLevelInstanceTest,
	"MtoUSceneRefPrototype.LevelInstanceRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefLevelInstanceTest::RunTest(const FString& Parameters)
{
	TArray<FString> Notes;
	if (!EnsureFixture(*this, Notes))
	{
		return false;
	}
	for (const FString& Note : Notes)
	{
		AddInfo(Note);
	}
	if (!LoadSampleLevel(*this, FMtoUSceneRefFixture::InstanceHostLevelPackage()))
	{
		RestoreHost(*this);
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	FMtoUSceneRefResolution Resolution;
	TestTrue(TEXT("the level instance scope resolves"),
		FMtoUSceneRefScope::Resolve(*World, FMtoUSceneRefFixture::InstanceScope(), Resolution));

	const AActor* InstanceActor = FindActor(Resolution, FMtoUSceneRefFixture::InstanceActorLabel());
	TestNotNull(TEXT("the placed level instance actor is in the loaded level"), InstanceActor);
	TestTrue(TEXT("the placed actor is an ALevelInstance"),
		InstanceActor != nullptr && InstanceActor->IsA<ALevelInstance>());
	if (InstanceActor != nullptr)
	{
		const ALevelInstance* Instance = Cast<ALevelInstance>(InstanceActor);
		TestTrue(TEXT("the level instance names the saved fixture sublevel as its world asset"),
			Instance->GetWorldAsset().GetLongPackageName() == FString(FMtoUSceneRefFixture::InstanceSourceLevelPackage()));
		AddInfo(FString::Printf(TEXT("level instance: actor=%s world_asset=%s"),
			*InstanceActor->GetActorLabel(), *Instance->GetWorldAsset().ToString()));
	}

	// The resolver reports the engine's own refusal instead of silently dropping the actor.
	const FMtoUSceneRefSkipped* Unsupported = FindSkipped(Resolution.Unsupported, FMtoUSceneRefFixture::InstanceActorLabel());
	TestNotNull(TEXT("the instance is reported as unsupported"), Unsupported);
	if (Unsupported != nullptr)
	{
		TestTrue(TEXT("the report carries the engine's own refusal text"),
			Unsupported->Reason.Contains(TEXT("Exporting Level Instances to FBX is not supported")));
		TestTrue(TEXT("the report names the engine as the source of the refusal"),
			Unsupported->Reason.Contains(TEXT("engine")));
		AddInfo(FString::Printf(TEXT("level instance report: class=%s reason=%s"),
			*Unsupported->ActorClass, *Unsupported->Reason));
	}
	TestNull(TEXT("no object is produced from the instance"),
		FindObject(Resolution, FMtoUSceneRefFixture::InstanceActorLabel()));
	TestFalse(TEXT("the transfer never selects a level instance for the export"),
		Resolution.ExportableActors.Contains(const_cast<AActor*>(InstanceActor)));
	TestEqual(TEXT("the instance's own level is not one of the scope's levels"),
		Resolution.LoadedSublevels.Contains(FString(FMtoUSceneRefFixture::InstanceSourceLevelPackage())), false);
	AddInfo(FString::Printf(TEXT("level instance scope: actors=%d objects=%d unsupported=%d excluded_sublevels=%d"),
		Resolution.Actors.Num(), Resolution.ExportedObjectCount(), Resolution.Unsupported.Num(),
		Resolution.ExcludedSublevels.Num()));

	// The scope carries nothing the engine can export, so what the transfer writes has to be
	// checked rather than assumed: whatever the export task answers, the file must not carry
	// the instance. Measured: the export task succeeds with an empty file (the engine's own
	// pre-check, IsSomethingToExport, is asked only whether the *selected* actors can be
	// exported, and the resolver selected none).
	// The engine answers IsSomethingToExport with its own reason string
	// (EditorExporters.cpp:1770-1774) on every path that meets the instance, both in the
	// transfer's own export and in the isolated probe below; one or more answers are expected.
	AddExpectedMessagePlain(TEXT("Exporting Level Instances to FBX is not supported."),
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/0);

	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("level-instance"));
	FMtoUSceneRefTransferOptions Options;
	Options.OutputDirectory = Directory;
	FMtoUSceneRefTransferResult Result;
	const bool bExported = FMtoUSceneRefTransfer::Run(*World, FMtoUSceneRefFixture::InstanceScope(), Options, Result);
	AddInfo(FString::Printf(TEXT("level instance transfer: exported=%s error=%s objects=%d"),
		bExported ? TEXT("true") : TEXT("false"), *Result.Error,
		bExported ? Result.Resolution.ExportedObjectCount() : 0));
	if (bExported)
	{
		TestFalse(TEXT("the file holds no node for the level instance"),
			Result.Output.NodeNames.Contains(FMtoUSceneRefFixture::InstanceActorLabel()));
		TestEqual(TEXT("the transfer carries no object for the level instance"),
			Result.Resolution.ExportedObjectCount(), 0);
	}
	else
	{
		TestTrue(TEXT("a refusal comes from the engine's exporter"),
			Result.Error.Contains(TEXT("exporter reported a failure")));
	}

	if (InstanceActor != nullptr)
	{
		const FString RefusedPath = FPaths::Combine(Directory, TEXT("level-instance-refused.fbx"));
		TArray<AActor*> OnlyTheInstance;
		OnlyTheInstance.Add(const_cast<AActor*>(InstanceActor));
		const bool bInstanceExported = FMtoUSceneRefTransfer::ProbeActorExport(
			*World, OnlyTheInstance, RefusedPath, Options);
		TArray<FString> RefusedNodes;
		if (FPaths::FileExists(RefusedPath))
		{
			FMtoUSceneRefTransfer::ScanFbxModelNames(RefusedPath, RefusedNodes);
		}
		AddInfo(FString::Printf(TEXT("level instance probe: engine_exported=%s file=%s nodes=[%s]"),
			bInstanceExported ? TEXT("true") : TEXT("false"),
			FPaths::FileExists(RefusedPath) ? TEXT("written") : TEXT("absent"),
			*FString::Join(RefusedNodes, TEXT(", "))));
		TestFalse(TEXT("the engine's exporter writes no node for a selected level instance"),
			RefusedNodes.Contains(FString(FMtoUSceneRefFixture::InstanceActorLabel())));
	}

	RestoreHost(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMtoUSceneRefLandscapeTest,
	"MtoUSceneRefPrototype.LandscapeScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUSceneRefLandscapeTest::RunTest(const FString& Parameters)
{
	TArray<FString> Notes;
	if (!EnsureFixture(*this, Notes))
	{
		return false;
	}
	for (const FString& Note : Notes)
	{
		AddInfo(Note);
	}
	if (!LoadSampleLevel(*this, FMtoUSceneRefFixture::LandscapeLevelPackage()))
	{
		RestoreHost(*this);
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	FMtoUSceneRefResolution Resolution;
	TestTrue(TEXT("the landscape scope resolves"),
		FMtoUSceneRefScope::Resolve(*World, FMtoUSceneRefFixture::LandscapeScope(), Resolution));

	const FMtoUSceneRefObject* Object = FindObject(Resolution, FMtoUSceneRefFixture::LandscapeActorLabel());
	TestNotNull(TEXT("the scope carries the landscape actor"), FindActor(Resolution, FMtoUSceneRefFixture::LandscapeActorLabel()));
	TestNotNull(TEXT("the scope carries a landscape record"), Object);
	if (Object == nullptr)
	{
		RestoreHost(*this);
		return false;
	}
	TestEqual(TEXT("the landscape is its own category"), Object->Category, EMtoUSceneRefCategory::Landscape);
	TestTrue(TEXT("the landscape is exported"), Object->bExported);

	// The record has to describe the geometry the engine's landscape branch writes, not the
	// actor's own bounds: the fixture is a flat 63x63 quad heightfield, so its surface has no
	// thickness while the actor's editor bounds are hundreds of centimetres thick. These are
	// the two numbers the first landscape handoff failed its Maya size and centroid checks on.
	const ALandscapeProxy* LandscapeProxy = Cast<ALandscapeProxy>(FindActor(Resolution, FMtoUSceneRefFixture::LandscapeActorLabel()));
	TestNotNull(TEXT("the scope carries the landscape actor itself"), LandscapeProxy);
	const FBox ActorBounds = LandscapeProxy != nullptr
		? LandscapeProxy->GetComponentsBoundingBox(/*bNonColliding=*/true)
		: FBox(ForceInit);
	const FVector MeasuredSize = Object->WorldBounds.GetSize();
	if (LandscapeProxy != nullptr)
	{
		const int32 Quads = LandscapeProxy->ComponentSizeQuads;
		const int32 Components = LandscapeProxy->LandscapeComponents.Num();
		const FVector Scale = LandscapeProxy->GetActorScale3D();
		TestEqual(TEXT("the resolver counts the triangles the landscape export writes"),
			Object->Triangles, 2 * FMath::Square(Quads) * Components);
		TestEqual(TEXT("the resolver counts the vertices the landscape export writes"),
			Object->Vertices, FMath::Square(Quads + 1) * Components);
		TestTrue(TEXT("the measured footprint is the component's own patch size"),
			FMath::IsNearlyEqual(MeasuredSize.X, Quads * FMath::Abs(Scale.X), 1.0) &&
			FMath::IsNearlyEqual(MeasuredSize.Y, Quads * FMath::Abs(Scale.Y), 1.0));
		TestTrue(TEXT("the flat heightfield measures as a surface, thinner than the actor's own bounds"),
			MeasuredSize.Z + 1.0 < ActorBounds.GetSize().Z);
		TestTrue(TEXT("the flat heightfield's surface centroid is the centre of its measured bounds"),
			(Object->WorldSurfaceCentroid - Object->WorldBounds.GetCenter()).Size() < 1.0);
	}
	AddInfo(FString::Printf(TEXT("landscape scope record: id=%s node=%s measured_size=%.1f/%.1f/%.1f actor_bounds=%.1f/%.1f/%.1f centroid=%.1f/%.1f/%.1f triangles=%d vertices=%d note=%s"),
		*Object->Id, *Object->NodeName,
		MeasuredSize.X, MeasuredSize.Y, MeasuredSize.Z,
		ActorBounds.GetSize().X, ActorBounds.GetSize().Y, ActorBounds.GetSize().Z,
		Object->WorldSurfaceCentroid.X, Object->WorldSurfaceCentroid.Y, Object->WorldSurfaceCentroid.Z,
		Object->Triangles, Object->Vertices, *Object->Note));

	const FString Directory = FPaths::Combine(EvidenceDirectory(), TEXT("landscape"));
	FMtoUSceneRefTransferResult Result;
	if (!RunTransfer(*this, FMtoUSceneRefFixture::LandscapeScope(), Directory, Result))
	{
		RestoreHost(*this);
		return false;
	}
	AddInfo(FString::Printf(TEXT("landscape export: files=%d bytes=%lld nodes=[%s] triangles=%d vertices=%d warnings=%d"),
		Result.Output.Files.Num(), Result.Output.GeometryBytes,
		*FString::Join(Result.Output.NodeNames, TEXT(", ")),
		Result.TriangleCount, Result.VertexCount, Result.Warnings.Num()));
	for (const FString& Warning : Result.Warnings)
	{
		AddInfo(FString::Printf(TEXT("landscape export warning: %s"), *Warning));
	}
	// The engine's landscape branch writes a node for the actor it was given.
	TestTrue(TEXT("the file holds the landscape node the manifest predicted"),
		Result.Output.NodeNames.Contains(Object->NodeName));

	// The manifest is the cross-host contract: the numbers the scope measured from the
	// landscape geometry have to be the ones the Maya peer compares against the file.
	TSharedPtr<FJsonObject> Manifest;
	const FString ManifestPath = FPaths::Combine(Directory, Result.Resolution.ScopeName + TEXT(".manifest.json"));
	TestTrue(TEXT("the landscape manifest parses"), LoadJson(ManifestPath, Manifest));
	const TSharedPtr<FJsonObject> Record = FindManifestObject(Manifest, FMtoUSceneRefFixture::LandscapeActorLabel());
	TestTrue(TEXT("the manifest holds the landscape record"), Record.IsValid());
	if (Record.IsValid())
	{
		TestEqual(TEXT("the manifest records the landscape category"),
			Record->GetStringField(TEXT("category")), TEXT("landscape"));
		TestEqual(TEXT("the manifest records the triangles the scope measured"),
			Record->GetIntegerField(TEXT("triangles")), Object->Triangles);
		TestEqual(TEXT("the manifest records the vertices the scope measured"),
			Record->GetIntegerField(TEXT("vertices")), Object->Vertices);
		const TSharedPtr<FJsonObject> Bounds = Record->GetObjectField(TEXT("world_bounds_cm"));
		const TSharedPtr<FJsonObject> Size = Bounds.IsValid() ? Bounds->GetObjectField(TEXT("size")) : nullptr;
		if (Size.IsValid())
		{
			TestTrue(TEXT("the manifest's landscape size is the measured surface size, not the actor bounds"),
				FMath::IsNearlyEqual(Size->GetNumberField(TEXT("z")), MeasuredSize.Z, 1.0) &&
				FMath::IsNearlyEqual(Size->GetNumberField(TEXT("x")), MeasuredSize.X, 1.0));
		}
		const TSharedPtr<FJsonObject> Centroid = Record->GetObjectField(TEXT("world_surface_centroid_cm"));
		if (Centroid.IsValid())
		{
			TestTrue(TEXT("the manifest's landscape centroid is the measured surface centroid"),
				FMath::IsNearlyEqual(Centroid->GetNumberField(TEXT("x")), Object->WorldSurfaceCentroid.X, 1.0) &&
				FMath::IsNearlyEqual(Centroid->GetNumberField(TEXT("y")), Object->WorldSurfaceCentroid.Y, 1.0));
		}
	}

	RestoreHost(*this);
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
