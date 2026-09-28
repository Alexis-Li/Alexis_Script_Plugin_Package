// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

#include "MtoUSceneRefTransfer.h"

#include "AssetExportTask.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MtoUSceneRefScope.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "UnrealExporter.h"

namespace
{
	/** Image suffixes a reference handoff must never produce. */
	const TCHAR* const ImageSuffixes[] = {
		TEXT("png"), TEXT("bmp"), TEXT("tga"), TEXT("jpg"), TEXT("jpeg"),
		TEXT("exr"), TEXT("hdr"), TEXT("dds"), TEXT("fbm"),
	};

	bool IsImagePath(const FString& Path)
	{
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		for (const TCHAR* Suffix : ImageSuffixes)
		{
			if (Extension == Suffix)
			{
				return true;
			}
		}
		return Path.Contains(TEXT(".fbm/"), ESearchCase::IgnoreCase)
			|| Path.Contains(TEXT(".fbm\\"), ESearchCase::IgnoreCase);
	}

	/** Index of the next quote at or after From, or INDEX_NONE. */
	int32 IndexOfQuote(const FString& Line, int32 From)
	{
		for (int32 Index = FMath::Max(From, 0); Index < Line.Len(); ++Index)
		{
			if (Line[Index] == TEXT('"'))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** The first quoted token of a line, or an empty string. */
	FString QuotedValue(const FString& Line)
	{
		const int32 Open = IndexOfQuote(Line, 0);
		if (Open == INDEX_NONE)
		{
			return FString();
		}
		const int32 Close = IndexOfQuote(Line, Open + 1);
		if (Close == INDEX_NONE)
		{
			return FString();
		}
		return Line.Mid(Open + 1, Close - Open - 1);
	}

	/** The last quoted token of a line, which is the value of a `FileName: "x"` record. */
	FString LastQuotedValue(const FString& Line)
	{
		FString Value;
		int32 SearchFrom = 0;
		while (SearchFrom < Line.Len())
		{
			const int32 Open = IndexOfQuote(Line, SearchFrom);
			if (Open == INDEX_NONE)
			{
				break;
			}
			const int32 Close = IndexOfQuote(Line, Open + 1);
			if (Close == INDEX_NONE)
			{
				break;
			}
			Value = Line.Mid(Open + 1, Close - Open - 1);
			SearchFrom = Close + 1;
		}
		return Value;
	}

	void SelectActors(const TArray<AActor*>& Actors)
	{
		GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true, /*WarnAboutManyActors=*/false);
		for (AActor* Actor : Actors)
		{
			if (Actor != nullptr)
			{
				GEditor->SelectActor(Actor, /*bInSelected=*/true, /*bNotify=*/false, /*bSelectEvenIfHidden=*/true);
			}
		}
	}

	/** The engine's level exporters read the editor selection; the caller gets it back. */
	TArray<AActor*> CaptureSelection()
	{
		TArray<AActor*> Actors;
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				Actors.Add(Actor);
			}
		}
		return Actors;
	}

	void RestoreSelection(const TArray<AActor*>& Actors)
	{
		SelectActors(Actors);
	}

	void CollectProducedFiles(const FString& Directory, TArray<FMtoUSceneRefProducedFile>& OutFiles, TArray<FString>& OutImageFiles)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *Directory, TEXT("*"), /*Files=*/true, /*Directories=*/false);
		OutFiles.Reset();
		OutImageFiles.Reset();
		for (const FString& File : Found)
		{
			FMtoUSceneRefProducedFile Entry;
			Entry.Name = FPaths::GetCleanFilename(File);
			Entry.Bytes = IFileManager::Get().FileSize(*File);
			OutFiles.Add(Entry);
			if (IsImagePath(File))
			{
				OutImageFiles.Add(Entry.Name);
			}
		}
		OutFiles.Sort([](const FMtoUSceneRefProducedFile& Left, const FMtoUSceneRefProducedFile& Right)
		{
			return Left.Name < Right.Name;
		});
		OutImageFiles.Sort();
	}

	double UsedPhysicalMegabytes()
	{
		const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
		return static_cast<double>(Stats.UsedPhysical) / (1024.0 * 1024.0);
	}

	using FJsonWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

	void WriteStringArray(FJsonWriter& Writer, const TCHAR* Name, const TArray<FString>& Values)
	{
		Writer.WriteArrayStart(Name);
		for (const FString& Value : Values)
		{
			Writer.WriteValue(Value);
		}
		Writer.WriteArrayEnd();
	}

	void WriteObjectRecord(FJsonWriter& Writer, const FMtoUSceneRefObject& Object)
	{
		const FRotator Rotator = Object.WorldTransform.Rotator();
		const FVector Location = Object.WorldTransform.GetLocation();
		const FVector Scale = Object.WorldTransform.GetScale3D();
		const FMatrix Matrix = Object.WorldTransform.ToMatrixWithScale();

		Writer.WriteObjectStart();
		Writer.WriteValue(TEXT("id"), Object.Id);
		Writer.WriteValue(TEXT("node_name"), Object.NodeName);
		Writer.WriteValue(TEXT("actor"), Object.ActorLabel);
		Writer.WriteValue(TEXT("actor_class"), Object.ActorClass);
		Writer.WriteValue(TEXT("level"), Object.LevelPackage);
		Writer.WriteValue(TEXT("category"), FMtoUSceneRefScope::CategoryName(Object.Category));
		Writer.WriteValue(TEXT("component"), Object.ComponentName);
		Writer.WriteValue(TEXT("mesh"), Object.MeshPath);
		if (Object.InstanceIndex == INDEX_NONE)
		{
			Writer.WriteNull(TEXT("instance_index"));
		}
		else
		{
			Writer.WriteValue(TEXT("instance_index"), Object.InstanceIndex);
		}
		Writer.WriteValue(TEXT("exported"), Object.bExported);
		Writer.WriteValue(TEXT("note"), Object.Note);
		Writer.WriteValue(TEXT("triangles"), Object.Triangles);
		Writer.WriteValue(TEXT("vertices"), Object.Vertices);
		WriteStringArray(Writer, TEXT("materials"), Object.Materials);

		Writer.WriteObjectStart(TEXT("world_location_cm"));
		Writer.WriteValue(TEXT("x"), Location.X);
		Writer.WriteValue(TEXT("y"), Location.Y);
		Writer.WriteValue(TEXT("z"), Location.Z);
		Writer.WriteObjectEnd();

		Writer.WriteObjectStart(TEXT("world_rotation_deg"));
		Writer.WriteValue(TEXT("roll"), Rotator.Roll);
		Writer.WriteValue(TEXT("pitch"), Rotator.Pitch);
		Writer.WriteValue(TEXT("yaw"), Rotator.Yaw);
		Writer.WriteObjectEnd();

		Writer.WriteObjectStart(TEXT("world_scale"));
		Writer.WriteValue(TEXT("x"), Scale.X);
		Writer.WriteValue(TEXT("y"), Scale.Y);
		Writer.WriteValue(TEXT("z"), Scale.Z);
		Writer.WriteObjectEnd();

		Writer.WriteArrayStart(TEXT("world_matrix"));
		for (int32 Row = 0; Row < 4; ++Row)
		{
			for (int32 Column = 0; Column < 4; ++Column)
			{
				Writer.WriteValue(Matrix.M[Row][Column]);
			}
		}
		Writer.WriteArrayEnd();

		Writer.WriteObjectStart(TEXT("world_surface_centroid_cm"));
		Writer.WriteValue(TEXT("x"), Object.WorldSurfaceCentroid.X);
		Writer.WriteValue(TEXT("y"), Object.WorldSurfaceCentroid.Y);
		Writer.WriteValue(TEXT("z"), Object.WorldSurfaceCentroid.Z);
		Writer.WriteObjectEnd();

		const FVector Min = Object.WorldBounds.Min;
		const FVector Max = Object.WorldBounds.Max;
		Writer.WriteObjectStart(TEXT("world_bounds_cm"));
		Writer.WriteObjectStart(TEXT("min"));
		Writer.WriteValue(TEXT("x"), Min.X);
		Writer.WriteValue(TEXT("y"), Min.Y);
		Writer.WriteValue(TEXT("z"), Min.Z);
		Writer.WriteObjectEnd();
		Writer.WriteObjectStart(TEXT("max"));
		Writer.WriteValue(TEXT("x"), Max.X);
		Writer.WriteValue(TEXT("y"), Max.Y);
		Writer.WriteValue(TEXT("z"), Max.Z);
		Writer.WriteObjectEnd();
		Writer.WriteObjectStart(TEXT("size"));
		Writer.WriteValue(TEXT("x"), Max.X - Min.X);
		Writer.WriteValue(TEXT("y"), Max.Y - Min.Y);
		Writer.WriteValue(TEXT("z"), Max.Z - Min.Z);
		Writer.WriteObjectEnd();
		Writer.WriteObjectEnd();

		Writer.WriteObjectEnd();
	}

	void WriteSkippedRecords(FJsonWriter& Writer, const TCHAR* Name, const TArray<FMtoUSceneRefSkipped>& Records)
	{
		Writer.WriteArrayStart(Name);
		for (const FMtoUSceneRefSkipped& Record : Records)
		{
			Writer.WriteObjectStart();
			Writer.WriteValue(TEXT("actor"), Record.ActorLabel);
			Writer.WriteValue(TEXT("class"), Record.ActorClass);
			Writer.WriteValue(TEXT("reason"), Record.Reason);
			Writer.WriteObjectEnd();
		}
		Writer.WriteArrayEnd();
	}
}

void FMtoUSceneRefTransfer::ScanFbxTextureReferences(const FString& FilePath, FMtoUSceneRefTextureScan& Out)
{
	Out = FMtoUSceneRefTextureScan();

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		return;
	}

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

	bool bInBlock = false;
	bool bIsVideo = false;
	bool bBlockNamesAFile = false;

	auto FlushBlock = [&]()
	{
		if (bInBlock)
		{
			if (bIsVideo)
			{
				Out.VideoRecords++;
			}
			else
			{
				Out.TextureRecords++;
				if (bBlockNamesAFile)
				{
					Out.TextureReferences++;
				}
			}
		}
		bInBlock = false;
		bIsVideo = false;
		bBlockNamesAFile = false;
	};

	for (const FString& RawLine : Lines)
	{
		const FString Line = RawLine.TrimStartAndEnd();
		if (Line.StartsWith(TEXT("Texture:")) || Line.StartsWith(TEXT("Video:")))
		{
			FlushBlock();
			bInBlock = true;
			bIsVideo = Line.StartsWith(TEXT("Video:"));
			bBlockNamesAFile = false;
			continue;
		}
		if (bInBlock && (Line.StartsWith(TEXT("FileName:")) || Line.StartsWith(TEXT("RelativeFilename:"))))
		{
			const FString Value = LastQuotedValue(Line);
			if (!Value.IsEmpty())
			{
				bBlockNamesAFile = true;
				Out.FileNames.AddUnique(Value);
			}
			continue;
		}
		if (Line == TEXT("}"))
		{
			FlushBlock();
		}
	}
	FlushBlock();
}

void FMtoUSceneRefTransfer::ScanFbxModelNames(const FString& FilePath, TArray<FString>& OutNames)
{
	OutNames.Reset();

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		return;
	}

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
	for (const FString& RawLine : Lines)
	{
		const FString Line = RawLine.TrimStartAndEnd();
		if (!Line.StartsWith(TEXT("Model:")))
		{
			continue;
		}
		const FString Raw = QuotedValue(Line);
		const FString Name = Raw.StartsWith(TEXT("Model::"))
			? Raw.RightChop(7)
			: Raw;
		if (!Name.IsEmpty())
		{
			OutNames.AddUnique(Name);
		}
	}
}

bool FMtoUSceneRefTransfer::Run(
	UWorld& World,
	const FMtoUSceneRefScopeSpec& Spec,
	const FMtoUSceneRefTransferOptions& Options,
	FMtoUSceneRefTransferResult& Out)
{
	Out = FMtoUSceneRefTransferResult();
	Out.Spec = Spec;

	const double ScopeStart = FPlatformTime::Seconds();
	if (!FMtoUSceneRefScope::Resolve(World, Spec, Out.Resolution))
	{
		Out.Error = Out.Resolution.Error;
		return false;
	}
	Out.Measurements.ScopeSeconds = FPlatformTime::Seconds() - ScopeStart;

	TSet<FString> ComponentKeys;
	for (const FMtoUSceneRefObject& Object : Out.Resolution.Objects)
	{
		if (!Object.bExported)
		{
			continue;
		}
		Out.TriangleCount += Object.Triangles;
		Out.VertexCount += Object.Vertices;
		ComponentKeys.Add(Object.LevelPackage + TEXT("|") + Object.ActorLabel + TEXT("|") + Object.ComponentName);
	}
	Out.ComponentCount = ComponentKeys.Num();

	FString Directory = Options.OutputDirectory;
	if (Directory.IsEmpty())
	{
		Directory = FPaths::ProjectSavedDir() / TEXT("MtoUSceneRef") / Out.Resolution.ScopeName;
	}
	Directory = FPaths::ConvertRelativePathToFull(Directory);
	IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists=*/false, /*Tree=*/true);
	if (!IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true))
	{
		Out.Error = FString::Printf(TEXT("cannot create the output directory %s"), *Directory);
		return false;
	}

	const FString FbxPath = FPaths::Combine(Directory, Out.Resolution.ScopeName + TEXT(".fbx"));

	const TArray<AActor*> PreviousSelection = CaptureSelection();
	const double ExportStart = FPlatformTime::Seconds();
	SelectActors(Out.Resolution.ExportableActors);

	TStrongObjectPtr<UAssetExportTask> Task(NewObject<UAssetExportTask>());
	TStrongObjectPtr<UFbxExportOption> FbxOptions(NewObject<UFbxExportOption>());
	FbxOptions->bASCII = Options.bAscii;
	FbxOptions->BakeMaterialInputs = EFbxMaterialBakeMode::Disabled;
	FbxOptions->VertexColor = false;
	FbxOptions->LevelOfDetail = false;
	FbxOptions->Collision = false;
	FbxOptions->bExportSourceMesh = false;
	FbxOptions->bExportMorphTargets = false;
	FbxOptions->bExportPreviewMesh = false;
	FbxOptions->bForceFrontXAxis = Options.bForceFrontXAxis;

	Task->Object = &World;
	Task->Filename = FbxPath;
	Task->bSelected = true;
	Task->bReplaceIdentical = false;
	Task->bPrompt = false;
	Task->bAutomated = true;
	Task->bUseFileArchive = false;
	Task->bWriteEmptyFiles = false;
	Task->Options = FbxOptions.Get();

	const bool bExported = UExporter::RunAssetExportTask(Task.Get());
	Out.Warnings.Append(Task->Errors);
	RestoreSelection(PreviousSelection);
	Out.Measurements.ExportSeconds = FPlatformTime::Seconds() - ExportStart;

	if (!bExported)
	{
		Out.Error = TEXT("the engine's FBX level exporter reported a failure");
		return false;
	}

	const double InspectStart = FPlatformTime::Seconds();
	Out.Output.Directory = Directory;
	Out.Output.GeometryFile = FPaths::GetCleanFilename(FbxPath);
	Out.Output.GeometryBytes = IFileManager::Get().FileSize(*FbxPath);
	CollectProducedFiles(Directory, Out.Output.Files, Out.Output.ImageFiles);
	ScanFbxTextureReferences(FbxPath, Out.Output.TextureScan);
	ScanFbxModelNames(FbxPath, Out.Output.NodeNames);
	for (const FString& Reference : Out.Output.TextureScan.FileNames)
	{
		const FString Resolved = FPaths::IsRelative(Reference)
			? FPaths::Combine(Directory, Reference)
			: Reference;
		if (IFileManager::Get().FileExists(*Resolved))
		{
			Out.Output.TextureReferenceFilesPresent.Add(Reference);
		}
	}
	Out.Measurements.InspectSeconds = FPlatformTime::Seconds() - InspectStart;
	Out.Measurements.UsedPhysicalMB = UsedPhysicalMegabytes();

	Out.bSucceeded = true;
	return true;
}

bool FMtoUSceneRefTransfer::ProbeObjExport(
	UWorld& World,
	const FMtoUSceneRefResolution& Resolution,
	const FString& ObjPath,
	TArray<FMtoUSceneRefProducedFile>& OutFiles,
	TArray<FString>& OutMaterialLines,
	FString& OutError)
{
	OutFiles.Reset();
	OutMaterialLines.Reset();

	const TArray<AActor*> PreviousSelection = CaptureSelection();
	SelectActors(Resolution.ExportableActors);

	TStrongObjectPtr<UAssetExportTask> Task(NewObject<UAssetExportTask>());
	Task->Object = &World;
	Task->Filename = ObjPath;
	Task->bSelected = true;
	Task->bReplaceIdentical = false;
	Task->bPrompt = false;
	// The OBJ exporter answers its own material question with "yes" for any automated
	// run; that default is exactly what this probe has to observe.
	Task->bAutomated = true;
	Task->bUseFileArchive = false;
	Task->bWriteEmptyFiles = false;

	const bool bExported = UExporter::RunAssetExportTask(Task.Get());
	RestoreSelection(PreviousSelection);

	if (!bExported)
	{
		OutError = TEXT("the engine's OBJ level exporter reported a failure");
		return false;
	}

	TArray<FString> ImageFiles;
	CollectProducedFiles(FPaths::GetPath(ObjPath), OutFiles, ImageFiles);

	FString Content;
	if (FFileHelper::LoadFileToString(Content, *ObjPath))
	{
		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("mtllib")) || Line.StartsWith(TEXT("usemtl")))
			{
				OutMaterialLines.Add(Line);
			}
		}
	}
	return true;
}

bool FMtoUSceneRefTransferResult::WriteManifest(FString& OutError) const
{
	const FString Path = FPaths::Combine(
		Output.Directory,
		Resolution.ScopeName + TEXT(".manifest.json"));

	FString Text;
	TSharedRef<FJsonWriter> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema"), TEXT("mtou-scene-ref-manifest/1"));
	Writer->WriteValue(TEXT("generated_utc"), GeneratedUtc.ToIso8601());
	Writer->WriteValue(TEXT("engine_version"), FEngineVersion::Current().ToString(EVersionComponent::Patch));

	Writer->WriteObjectStart(TEXT("world"));
	Writer->WriteValue(TEXT("package"), Resolution.PersistentLevelPackage);
	Writer->WriteValue(TEXT("name"), FPaths::GetBaseFilename(Resolution.PersistentLevelPackage));
	Writer->WriteValue(TEXT("up_axis"), TEXT("Z"));
	Writer->WriteValue(TEXT("linear_unit"), TEXT("cm"));
	Writer->WriteValue(TEXT("world_partition"), Resolution.bWorldPartition);
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("scope"));
	Writer->WriteValue(TEXT("kind"), TEXT("level_range"));
	Writer->WriteValue(TEXT("persistent_level"), Resolution.PersistentLevelPackage);
	WriteStringArray(*Writer, TEXT("requested_sublevels"), Spec.RequestedSublevels);
	WriteStringArray(*Writer, TEXT("loaded_sublevels"), Resolution.LoadedSublevels);
	Writer->WriteArrayStart(TEXT("unloaded_sublevels"));
	for (const FMtoUSceneRefUnloadedLevel& Level : Resolution.UnloadedSublevels)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("package"), Level.Package);
		Writer->WriteValue(TEXT("streaming_state"), Level.StreamingState);
		Writer->WriteValue(TEXT("visible"), Level.bShouldBeVisible);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	WriteStringArray(*Writer, TEXT("excluded_sublevels"), Resolution.ExcludedSublevels);
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("scale"));
	Writer->WriteValue(TEXT("actors"), Resolution.Actors.Num());
	Writer->WriteValue(TEXT("components"), ComponentCount);
	Writer->WriteValue(TEXT("objects"), Resolution.ExportedObjectCount());
	Writer->WriteValue(TEXT("triangles"), TriangleCount);
	Writer->WriteValue(TEXT("vertices"), VertexCount);
	Writer->WriteObjectEnd();

	Writer->WriteArrayStart(TEXT("objects"));
	for (const FMtoUSceneRefObject& Object : Resolution.Objects)
	{
		WriteObjectRecord(*Writer, Object);
	}
	Writer->WriteArrayEnd();

	WriteSkippedRecords(*Writer, TEXT("unsupported"), Resolution.Unsupported);
	WriteSkippedRecords(*Writer, TEXT("skipped"), Resolution.Skipped);

	Writer->WriteObjectStart(TEXT("output"));
	Writer->WriteValue(TEXT("directory"), Output.Directory);
	Writer->WriteValue(TEXT("geometry_file"), Output.GeometryFile);
	Writer->WriteValue(TEXT("geometry_bytes"), static_cast<double>(Output.GeometryBytes));
	Writer->WriteArrayStart(TEXT("files"));
	for (const FMtoUSceneRefProducedFile& File : Output.Files)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("name"), File.Name);
		Writer->WriteValue(TEXT("bytes"), static_cast<double>(File.Bytes));
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	WriteStringArray(*Writer, TEXT("image_files"), Output.ImageFiles);
	Writer->WriteValue(TEXT("texture_records"), Output.TextureScan.TextureRecords);
	Writer->WriteValue(TEXT("texture_references"), Output.TextureScan.TextureReferences);
	Writer->WriteValue(TEXT("video_references"), Output.TextureScan.VideoRecords);
	WriteStringArray(*Writer, TEXT("texture_reference_files"), Output.TextureScan.FileNames);
	WriteStringArray(*Writer, TEXT("texture_reference_files_present"), Output.TextureReferenceFilesPresent);
	WriteStringArray(*Writer, TEXT("node_names"), Output.NodeNames);
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("timing"));
	Writer->WriteValue(TEXT("scope_seconds"), Measurements.ScopeSeconds);
	Writer->WriteValue(TEXT("export_seconds"), Measurements.ExportSeconds);
	Writer->WriteValue(TEXT("inspect_seconds"), Measurements.InspectSeconds);
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("memory"));
	Writer->WriteValue(TEXT("used_physical_mb"), Measurements.UsedPhysicalMB);
	Writer->WriteObjectEnd();

	Writer->WriteArrayStart(TEXT("warnings"));
	for (const FString& Warning : Warnings)
	{
		Writer->WriteValue(Warning);
	}
	Writer->WriteArrayEnd();

	Writer->WriteObjectEnd();
	Writer->Close();

	if (!FFileHelper::SaveStringToFile(Text, *Path))
	{
		OutError = FString::Printf(TEXT("cannot write the manifest %s"), *Path);
		return false;
	}
	return true;
}
