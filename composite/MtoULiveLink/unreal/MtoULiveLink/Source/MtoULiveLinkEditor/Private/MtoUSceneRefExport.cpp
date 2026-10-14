#include "MtoUSceneRefExport.h"

#include "MtoULinkage.h"

#include "AssetExportTask.h"
#include "Camera/CameraComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshResources.h"
#include "UObject/StrongObjectPtr.h"
#include "UnrealExporter.h"

namespace
{
using FJsonWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

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

/** Strips the whitespace and commas that separate the tokens of an FBX record line. */
FString StripRecordSeparators(FString Value)
{
    Value.TrimStartAndEndInline();
    Value.RemoveFromStart(TEXT(","));
    Value.RemoveFromEnd(TEXT(","));
    Value.TrimStartAndEndInline();
    return Value;
}

/**
 * The characters a `Content:` record's payload holds, or 0 when it holds none.
 *
 * The FBX SDK writes the payload of an embedded media record as a quoted string,
 * either on the `Content:` line itself or on the immediately following non empty
 * line. A record without embedded media carries no `Content:` line at all. The
 * surrounding quotes are the record's syntax, not media, so they are not counted.
 */
int32 ContentPayloadChars(const FString& ContentLine, const FString& FollowingLine)
{
    FString Payload = StripRecordSeparators(
        ContentLine.RightChop(FCString::Strlen(TEXT("Content:"))));
    if (Payload.IsEmpty())
    {
        Payload = StripRecordSeparators(FollowingLine);
        if (!Payload.StartsWith(TEXT("\"")))
        {
            return 0;
        }
    }
    if (Payload.Len() >= 2 && Payload.StartsWith(TEXT("\"")) && Payload.EndsWith(TEXT("\"")))
    {
        return Payload.Len() - 2;
    }
    return Payload.Len();
}

/** The options every handoff export runs with, in one place. */
UFbxExportOption* NewFbxExportOptions(bool bAscii)
{
    UFbxExportOption* Options = NewObject<UFbxExportOption>();
    Options->bASCII = bAscii;
    // Baking material inputs is what would pull image data into the file; the
    // handoff carries geometry only, so the bake stays off and the scan verifies it.
    Options->BakeMaterialInputs = EFbxMaterialBakeMode::Disabled;
    Options->VertexColor = false;
    Options->LevelOfDetail = false;
    Options->Collision = false;
    Options->bExportSourceMesh = false;
    Options->bExportMorphTargets = false;
    Options->bExportPreviewMesh = false;
    Options->bForceFrontXAxis = false;
    return Options;
}

void SelectActors(const TArray<AActor*>& Actors)
{
    GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true,
        /*WarnAboutManyActors=*/false);
    for (AActor* Actor : Actors)
    {
        if (Actor != nullptr)
        {
            GEditor->SelectActor(Actor, /*bInSelected=*/true, /*bNotify=*/false,
                /*bSelectEvenIfHidden=*/true);
        }
    }
}

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

/**
 * Keeps the components that are not static reference geometry out of the engine's
 * FBX export. The engine's level exporter turns every non hidden mesh, camera,
 * light and child actor component of a selected actor into a node, and
 * `bHiddenInGame` is the only caller side switch that stops it. The guard writes
 * the property for the duration of one export and puts every previous value back,
 * including on an early return, without dirtying the package.
 */
class FComponentSuppressionGuard
{
public:
    explicit FComponentSuppressionGuard(const TArray<USceneComponent*>& Components)
    {
        PreviousValues.Reserve(Components.Num());
        for (USceneComponent* Component : Components)
        {
            if (Component == nullptr)
            {
                continue;
            }
            const bool bWasHiddenInGame = Component->bHiddenInGame;
            PreviousValues.Emplace(Component, bWasHiddenInGame);
            Component->bHiddenInGame = true;
            ++SuppressedCount;
        }
    }

    ~FComponentSuppressionGuard()
    {
        for (const TPair<USceneComponent*, bool>& Previous : PreviousValues)
        {
            if (Previous.Key != nullptr)
            {
                Previous.Key->bHiddenInGame = Previous.Value;
            }
        }
    }

    FComponentSuppressionGuard(const FComponentSuppressionGuard&) = delete;
    FComponentSuppressionGuard& operator=(const FComponentSuppressionGuard&) = delete;

    int32 SuppressedCount = 0;

private:
    TArray<TPair<USceneComponent*, bool>> PreviousValues;
};

/** The render geometry one static mesh contributes, at LOD 0. */
void MeasureStaticMesh(const UStaticMesh& Mesh, int32& OutTriangles, int32& OutVertices)
{
    OutTriangles = 0;
    OutVertices = 0;
    if (const FStaticMeshRenderData* RenderData = Mesh.GetRenderData())
    {
        if (RenderData->LODResources.Num() > 0)
        {
            const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
            OutTriangles = LOD.GetNumTriangles();
            OutVertices = LOD.GetNumVertices();
        }
    }
}

/**
 * The area weighted centroid of a mesh's LOD 0 triangles in world space.
 *
 * It is the only sample of the geometry a mirrored placement changes while
 * position, bounds and pivot offset stay equal, and the only one the exported
 * file can reproduce (the file welds duplicated render vertices away), which is
 * why the quantity is triangle based rather than a vertex average.
 */
FVector SurfaceCentroid(const FStaticMeshLODResources& LOD, const FTransform& WorldTransform)
{
    const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
    FVector WeightedSum = FVector::ZeroVector;
    double TotalArea = 0.0;
    const int32 TriangleCount = LOD.GetNumTriangles();
    for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
    {
        const FVector Corners[3] = {
            WorldTransform.TransformPosition(FVector(Positions.VertexPosition(
                LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 0)))),
            WorldTransform.TransformPosition(FVector(Positions.VertexPosition(
                LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 1)))),
            WorldTransform.TransformPosition(FVector(Positions.VertexPosition(
                LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 2)))),
        };
        const double Area =
            0.5 * ((Corners[1] - Corners[0]).Cross(Corners[2] - Corners[0])).Size();
        if (Area <= 0.0)
        {
            continue;
        }
        WeightedSum += Area * (Corners[0] + Corners[1] + Corners[2]) / 3.0;
        TotalArea += Area;
    }
    return TotalArea > 0.0 ? WeightedSum / TotalArea : FVector::ZeroVector;
}

/**
 * One object the manifest records. These are the measurements the Maya side
 * compares against the imported file: the node name it matches after stripping
 * the staging namespace, the world matrix, the world bounds and the surface
 * centroid. A missing centroid is reported as unavailable rather than guessed.
 */
struct FMtoUSceneRefObjectRecord
{
    FString Id;
    FString NodeName;
    FString ActorLabel;
    FString ActorClass;
    FString LevelPackage;
    FString ComponentName;
    FString MeshPath;
    FString Category;
    int32 InstanceIndex = INDEX_NONE;
    TArray<FString> Materials;
    int32 Triangles = 0;
    int32 Vertices = 0;
    FTransform WorldTransform = FTransform::Identity;
    FBox WorldBounds = FBox(ForceInit);
    FVector WorldSurfaceCentroid = FVector::ZeroVector;
    bool bHasCentroid = false;
};
void ScanTextureRecords(const FString& FilePath, FMtoUTextureScan& Out)
{
    Out = FMtoUTextureScan();

    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *FilePath))
    {
        return;
    }
    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

    // The FBX SDK writes `Properties70: { ... }` inside a texture or video record
    // before the name lines, so a record is a brace delimited block: it opens at
    // depth one and only the `}` that brings the depth back to zero ends it.
    const TCHAR* const FileNameHeads[] = {
        TEXT("FileName:"), TEXT("Filename:"), TEXT("RelativeFilename:"),
    };

    bool bInRecord = false;
    bool bIsVideo = false;
    bool bRecordNamesAFile = false;
    int32 RecordDepth = 0;
    FString RecordName;
    int32 RecordContentLines = 0;
    int32 RecordEmbeddedMedia = 0;

    auto FlushRecord = [&]()
    {
        if (!bInRecord)
        {
            return;
        }
        if (bIsVideo)
        {
            ++Out.VideoRecords;
        }
        else
        {
            ++Out.TextureRecords;
            if (bRecordNamesAFile)
            {
                ++Out.TextureReferences;
            }
        }
        Out.ContentRecords += RecordContentLines;
        Out.EmbeddedMediaRecords += RecordEmbeddedMedia;
        bInRecord = false;
        bIsVideo = false;
        bRecordNamesAFile = false;
        RecordDepth = 0;
        RecordName.Reset();
        RecordContentLines = 0;
        RecordEmbeddedMedia = 0;
    };

    for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
    {
        const FString Line = Lines[LineIndex].TrimStartAndEnd();
        if (!bInRecord)
        {
            if (Line.StartsWith(TEXT("Texture:")) || Line.StartsWith(TEXT("Video:")))
            {
                bInRecord = true;
                bIsVideo = Line.StartsWith(TEXT("Video:"));
                RecordDepth = 1;
                RecordName = QuotedValue(Line);
            }
            continue;
        }
        if (Line.EndsWith(TEXT("{")))
        {
            ++RecordDepth;
            continue;
        }
        if (Line == TEXT("}"))
        {
            if (--RecordDepth <= 0)
            {
                FlushRecord();
            }
            continue;
        }
        bool bNamesFile = false;
        for (const TCHAR* Head : FileNameHeads)
        {
            if (Line.StartsWith(Head))
            {
                bNamesFile = true;
                break;
            }
        }
        if (bNamesFile)
        {
            bRecordNamesAFile = true;
            continue;
        }
        if (Line.StartsWith(TEXT("Content:")))
        {
            ++RecordContentLines;
            // The payload may sit on the next non empty line, so the first non
            // empty line after this one is the candidate the payload rule reads.
            FString FollowingLine;
            for (int32 Probe = LineIndex + 1; Probe < Lines.Num(); ++Probe)
            {
                FollowingLine = StripRecordSeparators(Lines[Probe]);
                if (!FollowingLine.IsEmpty())
                {
                    break;
                }
            }
            const int32 PayloadChars = ContentPayloadChars(Line, FollowingLine);
            if (PayloadChars > 0)
            {
                ++RecordEmbeddedMedia;
                Out.EmbeddedMedia.Add(FString::Printf(TEXT("%s (Content %d chars)"),
                    RecordName.IsEmpty() ? TEXT("unnamed media record") : *RecordName,
                    PayloadChars));
            }
        }
    }
    FlushRecord();
}

/** One level of the explicit scope: its package name and the actors it holds. */
struct FMtoUScopeLevel
{
    FString PackageName;
    TArray<AActor*> Actors;
};

/**
 * The explicit scope: the loaded persistent level plus the streaming sublevels the
 * caller named. Nothing is loaded here; a sublevel that is not loaded is reported
 * instead of silently dropped, so a partial scope can never look complete.
 */
void ResolveScopeLevels(UWorld& World, const TArray<FString>& RequestedSublevels,
    TArray<FMtoUScopeLevel>& OutLevels, TArray<FString>& OutUnloaded,
    TArray<FString>& OutLoaded, TArray<FString>& OutExcluded)
{
    OutLevels.Reset();
    OutUnloaded.Reset();
    OutLoaded.Reset();
    OutExcluded.Reset();

    FMtoUScopeLevel Persistent;
    if (ULevel* Level = World.PersistentLevel)
    {
        Persistent.PackageName = Level->GetPackage()->GetName();
        for (const TObjectPtr<AActor>& Actor : Level->Actors)
        {
            if (Actor)
            {
                Persistent.Actors.Add(Actor.Get());
            }
        }
    }
    OutLevels.Add(MoveTemp(Persistent));

    for (const FString& Requested : RequestedSublevels)
    {
        const FString Normalized = FPaths::GetBaseFilename(Requested);
        if (Normalized.IsEmpty())
        {
            continue;
        }
        bool bFound = false;
        for (ULevelStreaming* Streaming : World.GetStreamingLevels())
        {
            if (!Streaming)
            {
                continue;
            }
            const FString AssetPackage = Streaming->GetWorldAsset().ToSoftObjectPath().GetLongPackageName();
            if (FPaths::GetBaseFilename(AssetPackage) != Normalized)
            {
                continue;
            }
            bFound = true;
            ULevel* Loaded = Streaming->GetLoadedLevel();
            if (!Loaded)
            {
                OutUnloaded.Add(AssetPackage);
                continue;
            }
            FMtoUScopeLevel Entry;
            Entry.PackageName = AssetPackage;
            for (const TObjectPtr<AActor>& Actor : Loaded->Actors)
            {
                if (Actor)
                {
                    Entry.Actors.Add(Actor.Get());
                }
            }
            OutLevels.Add(MoveTemp(Entry));
            OutLoaded.Add(AssetPackage);
        }
        if (!bFound)
        {
            // A name the world does not stream at all cannot be part of this scope.
            OutExcluded.Add(Requested);
        }
    }
}

/**
 * Static reference geometry only: an actor contributes when it holds a static mesh
 * component, and every other component of a contributing actor is suppressed for
 * the export's duration. A skeletal, camera, light or child actor component would
 * turn into a node the handoff must never carry, so the resolver names them and
 * the export applies and restores exactly that list.
 */
void CollectExportScope(const TArray<FMtoUScopeLevel>& Levels,
    TArray<AActor*>& OutExportable, TArray<USceneComponent*>& OutSuppressed,
    TArray<FMtoUSceneRefObjectRecord>& OutObjects,
    int32& OutExportedObjects, int32& OutSkippedActors, int32& OutTriangles,
    int32& OutVertices)
{
    OutExportable.Reset();
    OutSuppressed.Reset();
    OutObjects.Reset();
    OutExportedObjects = 0;
    OutSkippedActors = 0;
    OutTriangles = 0;
    OutVertices = 0;

    for (const FMtoUScopeLevel& Level : Levels)
    {
        for (AActor* Actor : Level.Actors)
        {
            if (!Actor)
            {
                continue;
            }
            TArray<UStaticMeshComponent*> MeshComponents;
            for (UActorComponent* Component : Actor->GetComponents())
            {
                UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
                if (StaticMeshComponent && StaticMeshComponent->GetStaticMesh())
                {
                    MeshComponents.Add(StaticMeshComponent);
                }
            }
            if (MeshComponents.IsEmpty())
            {
                ++OutSkippedActors;
                continue;
            }

            for (UActorComponent* Component : Actor->GetComponents())
            {
                if (Cast<UStaticMeshComponent>(Component))
                {
                    continue;
                }
                // The order mirrors FFbxExporter::ExportActor's own branch order, so
                // the suppressed list is exactly the components that would otherwise
                // reach the file as a skeletal, camera, light or child actor node. A
                // component that is already hidden or produces no node is not one.
                const USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
                if (!SceneComponent || SceneComponent->bHiddenInGame)
                {
                    continue;
                }
                if (Cast<USkeletalMeshComponent>(SceneComponent)
                    || Cast<UCameraComponent>(SceneComponent)
                    || Cast<ULightComponentBase>(SceneComponent)
                    || Cast<UChildActorComponent>(SceneComponent))
                {
                    OutSuppressed.Add(const_cast<USceneComponent*>(SceneComponent));
                }
            }
            OutExportable.Add(Actor);

            // Predicts the node layout the engine writes for the components the
            // filter leaves: a single static mesh component keeps the actor node
            // (named after the actor), several get one child node each, named after
            // the component, and every instance of an instanced component is its own
            // node named after the instance index.
            const bool bMultipleExportComponents = MeshComponents.Num() > 1;
            const FString ActorNodeName = Actor->GetActorLabel();
            for (UStaticMeshComponent* Component : MeshComponents)
            {
                const UStaticMesh* Mesh = Component->GetStaticMesh();
                const UInstancedStaticMeshComponent* InstancedComponent =
                    Cast<UInstancedStaticMeshComponent>(Component);
                const int32 Instances = InstancedComponent
                    ? FMath::Max(InstancedComponent->GetNumInstances(), 1) : 1;
                const FTransform ComponentTransform = Component->GetComponentTransform();

                int32 Triangles = 0;
                int32 Vertices = 0;
                MeasureStaticMesh(*Mesh, Triangles, Vertices);
                OutTriangles += Triangles * Instances;
                OutVertices += Vertices * Instances;

                for (int32 InstanceIndex = 0; InstanceIndex < Instances; ++InstanceIndex)
                {
                    FTransform WorldTransform = ComponentTransform;
                    if (InstancedComponent
                        && !InstancedComponent->GetInstanceTransform(
                            InstanceIndex, WorldTransform, /*bWorldSpace=*/true))
                    {
                        WorldTransform = ComponentTransform;
                    }
                    FMtoUSceneRefObjectRecord Record;
                    Record.ActorLabel = ActorNodeName;
                    Record.ActorClass = Actor->GetClass()->GetName();
                    Record.LevelPackage = Level.PackageName;
                    Record.ComponentName = Component->GetName();
                    Record.MeshPath = Mesh->GetPathName();
                    Record.Triangles = Triangles;
                    Record.Vertices = Vertices;
                    Record.WorldTransform = WorldTransform;
                    Record.WorldBounds = Mesh->GetBounds().GetBox().TransformBy(WorldTransform);
                    if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
                    {
                        if (RenderData->LODResources.Num() > 0)
                        {
                            Record.WorldSurfaceCentroid =
                                SurfaceCentroid(RenderData->LODResources[0], WorldTransform);
                            Record.bHasCentroid = true;
                        }
                    }
                    if (InstancedComponent)
                    {
                        Record.InstanceIndex = InstanceIndex;
                        Record.Category = TEXT("InstancedStaticMesh");
                        Record.NodeName = FString::Printf(TEXT("%d"), InstanceIndex);
                        Record.Id = FString::Printf(TEXT("%s:%s:component:%s:%d"),
                            *Record.ActorClass, *Record.ActorLabel, *Record.ComponentName,
                            InstanceIndex);
                    }
                    else
                    {
                        Record.Category = TEXT("StaticMesh");
                        Record.NodeName =
                            bMultipleExportComponents ? Record.ComponentName : ActorNodeName;
                        Record.Id = FString::Printf(TEXT("%s:%s:component:%s"),
                            *Record.ActorClass, *Record.ActorLabel, *Record.ComponentName);
                    }
                    for (int32 MaterialIndex = 0; MaterialIndex < Component->GetNumMaterials();
                        ++MaterialIndex)
                    {
                        const UMaterialInterface* Material =
                            Component->GetMaterial(MaterialIndex);
                        Record.Materials.Add(Material != nullptr
                            ? Material->GetPathName() : FString(TEXT("None")));
                    }
                    OutObjects.Add(MoveTemp(Record));
                    ++OutExportedObjects;
                }
            }
        }
    }
}

void WriteStringArray(FJsonWriter& Writer, const TCHAR* Name, const TArray<FString>& Values)
{
    Writer.WriteArrayStart(Name);
    for (const FString& Value : Values)
    {
        Writer.WriteValue(Value);
    }
    Writer.WriteArrayEnd();
}

void WriteVector(FJsonWriter& Writer, const TCHAR* Name, const FVector& Value)
{
    Writer.WriteObjectStart(Name);
    Writer.WriteValue(TEXT("x"), Value.X);
    Writer.WriteValue(TEXT("y"), Value.Y);
    Writer.WriteValue(TEXT("z"), Value.Z);
    Writer.WriteObjectEnd();
}

/**
 * One measured object, in the shape the frozen importer reads.
 *
 * `node_name` is what Maya matches after stripping the staging namespace, so it
 * carries the node the engine's FBX level export wrote for this object: the actor
 * node for a single mesh component, the component node for a multi-component
 * actor and the instance index for an instanced component. `world_matrix` is the
 * row-major transform the prototype wrote, `world_bounds_cm` carries min, max and
 * size, and `world_surface_centroid_cm` is the triangle based centroid a mirrored
 * placement changes while position and size stay equal.
 */
void WriteObjectRecord(FJsonWriter& Writer, const FMtoUSceneRefObjectRecord& Object)
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
    Writer.WriteValue(TEXT("category"), Object.Category);
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
    Writer.WriteValue(TEXT("exported"), true);
    Writer.WriteValue(TEXT("triangles"), Object.Triangles);
    Writer.WriteValue(TEXT("vertices"), Object.Vertices);
    WriteStringArray(Writer, TEXT("materials"), Object.Materials);

    WriteVector(Writer, TEXT("world_location_cm"), Location);
    Writer.WriteObjectStart(TEXT("world_rotation_deg"));
    Writer.WriteValue(TEXT("roll"), Rotator.Roll);
    Writer.WriteValue(TEXT("pitch"), Rotator.Pitch);
    Writer.WriteValue(TEXT("yaw"), Rotator.Yaw);
    Writer.WriteObjectEnd();
    WriteVector(Writer, TEXT("world_scale"), Scale);

    Writer.WriteArrayStart(TEXT("world_matrix"));
    for (int32 Row = 0; Row < 4; ++Row)
    {
        for (int32 Column = 0; Column < 4; ++Column)
        {
            Writer.WriteValue(Matrix.M[Row][Column]);
        }
    }
    Writer.WriteArrayEnd();

    WriteVector(Writer, TEXT("world_surface_centroid_cm"), Object.WorldSurfaceCentroid);

    const FVector Min = Object.WorldBounds.Min;
    const FVector Max = Object.WorldBounds.Max;
    Writer.WriteObjectStart(TEXT("world_bounds_cm"));
    WriteVector(Writer, TEXT("min"), Min);
    WriteVector(Writer, TEXT("max"), Max);
    Writer.WriteObjectStart(TEXT("size"));
    Writer.WriteValue(TEXT("x"), Max.X - Min.X);
    Writer.WriteValue(TEXT("y"), Max.Y - Min.Y);
    Writer.WriteValue(TEXT("z"), Max.Z - Min.Z);
    Writer.WriteObjectEnd();
    Writer.WriteObjectEnd();

    Writer.WriteObjectEnd();
}

void WriteScopeReport(FJsonWriter& Writer, const FMtoUSceneRefExportResult& Result,
    const TArray<FMtoUSceneRefObjectRecord>& Objects)
{
    Writer.WriteObjectStart(TEXT("scope"));
    Writer.WriteValue(TEXT("level"), Result.LevelPackage);
    WriteStringArray(Writer, TEXT("requested_sublevels"), Result.RequestedSublevels);
    WriteStringArray(Writer, TEXT("loaded_sublevels"), Result.LoadedSublevels);
    WriteStringArray(Writer, TEXT("unloaded_sublevels"), Result.UnloadedSublevels);
    WriteStringArray(Writer, TEXT("excluded_sublevels"), Result.ExcludedSublevels);
    Writer.WriteValue(TEXT("completeness"),
        Result.bScopeComplete ? TEXT("confirmed") : TEXT("loaded_only"));
    Writer.WriteObjectEnd();

    Writer.WriteObjectStart(TEXT("counts"));
    Writer.WriteValue(TEXT("objects"), Result.ExportedObjectCount);
    Writer.WriteValue(TEXT("triangles"), Result.TriangleCount);
    Writer.WriteValue(TEXT("vertices"), Result.VertexCount);
    Writer.WriteValue(TEXT("bytes"), Result.GeometryBytes);
    Writer.WriteValue(TEXT("skipped_actors"), Result.SkippedActorCount);
    Writer.WriteValue(TEXT("suppressed_components"), Result.SuppressedComponentCount);
    Writer.WriteObjectEnd();

    Writer.WriteObjectStart(TEXT("image_data"));
    Writer.WriteValue(TEXT("detected"), Result.bImageDataDetected);
    Writer.WriteValue(TEXT("texture_records"), Result.TextureScan.TextureRecords);
    Writer.WriteValue(TEXT("texture_references"), Result.TextureScan.TextureReferences);
    Writer.WriteValue(TEXT("video_records"), Result.TextureScan.VideoRecords);
    Writer.WriteValue(TEXT("content_records"), Result.TextureScan.ContentRecords);
    Writer.WriteValue(TEXT("embedded_media_records"), Result.TextureScan.EmbeddedMediaRecords);
    WriteStringArray(Writer, TEXT("embedded_media"), Result.TextureScan.EmbeddedMedia);
    WriteStringArray(Writer, TEXT("image_files"), Result.ImageFiles);
    Writer.WriteObjectEnd();

    // The per-object measurements the importer compares against the file it
    // imported: one entry per exported object, in the order the file holds them.
    Writer.WriteArrayStart(TEXT("objects"));
    for (const FMtoUSceneRefObjectRecord& Object : Objects)
    {
        WriteObjectRecord(Writer, Object);
    }
    Writer.WriteArrayEnd();
}
}

bool FMtoUSceneRefExport::Run(
    UWorld& World,
    const TArray<FString>& RequestedSublevels,
    FMtoUSceneRefExportResult& Out)
{
    Out = FMtoUSceneRefExportResult();
    Out.RequestedSublevels = RequestedSublevels;

    TArray<FMtoUScopeLevel> Levels;
    ResolveScopeLevels(World, RequestedSublevels, Levels,
        Out.UnloadedSublevels, Out.LoadedSublevels, Out.ExcludedSublevels);
    if (Levels.IsEmpty() || Levels[0].PackageName.IsEmpty())
    {
        Out.Error = TEXT("the world has no loaded persistent level to export");
        return false;
    }
    Out.LevelPackage = Levels[0].PackageName;
    Out.ScopeName = FPaths::GetBaseFilename(Out.LevelPackage);
    Out.bScopeComplete = Out.UnloadedSublevels.IsEmpty() && Out.ExcludedSublevels.IsEmpty();
    if (World.IsPartitionedWorld())
    {
        // A partitioned world streams its actors through cells, so a level range
        // holds exactly the actors the loaded cells spawned; the scope says so
        // instead of implying coverage it cannot prove.
        Out.bScopeComplete = false;
        Out.Warnings.Add(TEXT(
            "SCOPE_LOADED_ONLY: the world is partitioned, so the scope holds only the content the loaded cells spawned"));
    }
    if (!Out.UnloadedSublevels.IsEmpty())
    {
        Out.Warnings.Add(FString::Printf(
            TEXT("%d requested sublevel(s) are not loaded and are not part of this handoff"),
            Out.UnloadedSublevels.Num()));
    }
    if (!Out.ExcludedSublevels.IsEmpty())
    {
        Out.Warnings.Add(FString::Printf(
            TEXT("%d requested sublevel(s) are not streamed by this world"),
            Out.ExcludedSublevels.Num()));
    }

    TArray<AActor*> Exportable;
    TArray<USceneComponent*> Suppressed;
    TArray<FMtoUSceneRefObjectRecord> Objects;
    CollectExportScope(Levels, Exportable, Suppressed, Objects, Out.ExportedObjectCount,
        Out.SkippedActorCount, Out.TriangleCount, Out.VertexCount);
    if (Exportable.IsEmpty())
    {
        Out.Error = TEXT("the resolved scope holds no static mesh geometry to export");
        return false;
    }
    Out.SuppressedComponentCount = Suppressed.Num();

    FString Directory = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir() / TEXT("MtoUSceneRef") / Out.ScopeName);
    // The directory belongs to one run: recreating it keeps a repeat run from
    // mixing with a previous one's files.
    IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists=*/false, /*Tree=*/true);
    if (!IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true))
    {
        Out.Error = FString::Printf(TEXT("cannot create the output directory %s"), *Directory);
        return false;
    }
    const FString FbxPath = FPaths::Combine(Directory, Out.ScopeName + TEXT(".fbx"));

    // The engine's level exporters read the editor selection, so the scope the
    // resolver resolved is exactly the selection the export runs over, and the
    // caller's selection comes back even when the export fails.
    const TArray<AActor*> PreviousSelection = CaptureSelection();
    SelectActors(Exportable);

    TStrongObjectPtr<UAssetExportTask> Task(NewObject<UAssetExportTask>());
    TStrongObjectPtr<UFbxExportOption> FbxOptions(NewFbxExportOptions(/*bAscii=*/true));
    Task->Object = &World;
    Task->Filename = FbxPath;
    Task->bSelected = true;
    Task->bReplaceIdentical = false;
    Task->bPrompt = false;
    Task->bAutomated = true;
    Task->bUseFileArchive = false;
    Task->bWriteEmptyFiles = false;
    Task->Options = FbxOptions.Get();

    bool bExported = false;
    {
        FComponentSuppressionGuard Suppression(Suppressed);
        bExported = UExporter::RunAssetExportTask(Task.Get());
        Out.SuppressedComponentCount = Suppression.SuppressedCount;
    }
    Out.Warnings.Append(Task->Errors);
    SelectActors(PreviousSelection);
    if (!bExported)
    {
        Out.Error = TEXT("the engine's FBX level exporter reported a failure");
        return false;
    }

    Out.Directory = Directory;
    Out.GeometryFile = FPaths::GetCleanFilename(FbxPath);
    Out.GeometryBytes = IFileManager::Get().FileSize(*FbxPath);

    // The image-data check runs on the produced file, not on an export flag: a
    // handoff that carries media is refused before it is offered.
    ScanFbxTextureReferences(FbxPath, Out.TextureScan);
    TArray<FString> ProducedFiles;
    IFileManager::Get().FindFilesRecursive(ProducedFiles, *Directory, TEXT("*"),
        /*Files=*/true, /*Directories=*/false);
    for (const FString& File : ProducedFiles)
    {
        if (IsImagePath(File))
        {
            Out.ImageFiles.Add(FPaths::GetCleanFilename(File));
        }
    }
    Out.ImageFiles.Sort();
    Out.bImageDataDetected = FMtoUSceneRefExport::HasImageData(Out);
    if (Out.bImageDataDetected)
    {
        // Fail closed: the produced directory is removed so no image data can be
        // imported from a run that was refused.
        IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists=*/false, /*Tree=*/true);
        Out.Error = TEXT("the produced handoff carries image data; the run was refused");
        return false;
    }

    const FString ManifestPath = FPaths::Combine(Directory, Out.ScopeName + TEXT(".manifest.json"));
    // The identity is decided once, so the manifest and the offer carry the same one.
    Out.TransferId = FString::Printf(TEXT("mtou-scene-ref-%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    FString ManifestText;
    {
        const TSharedRef<FJsonWriter> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ManifestText);
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("schema"), TEXT("mtou-scene-ref-manifest/1"));
        Writer->WriteValue(TEXT("generated_utc"), FDateTime::UtcNow().ToIso8601());
        Writer->WriteValue(TEXT("engine_version"),
            FEngineVersion::Current().ToString(EVersionComponent::Patch));

        Writer->WriteObjectStart(TEXT("world"));
        Writer->WriteValue(TEXT("package"), Out.LevelPackage);
        Writer->WriteValue(TEXT("name"), Out.ScopeName);
        Writer->WriteValue(TEXT("up_axis"), TEXT("Z"));
        Writer->WriteValue(TEXT("linear_unit"), TEXT("cm"));
        Writer->WriteValue(TEXT("world_partition"), World.IsPartitionedWorld());
        Writer->WriteObjectEnd();

        // The handoff's axis convention is the engine's own: the file is written in
        // the engine's coordinates and the Maya side applies the measured map.
        Writer->WriteObjectStart(TEXT("conventions"));
        Writer->WriteValue(TEXT("handoff"), TEXT("engine_fbx_level_export"));
        Writer->WriteValue(TEXT("export_axis_option"), TEXT("bForceFrontXAxis=false"));
        Writer->WriteValue(TEXT("engine_to_maya_point_map"),
            TEXT("maya_x=ue_x, maya_y=ue_z, maya_z=ue_y"));
        Writer->WriteObjectEnd();

        WriteScopeReport(*Writer, Out, Objects);

        Writer->WriteObjectStart(TEXT("files"));
        Writer->WriteValue(TEXT("geometry"), Out.GeometryFile);
        Writer->WriteValue(TEXT("geometry_bytes"), Out.GeometryBytes);
        Writer->WriteValue(TEXT("image_data"), false);
        Writer->WriteObjectEnd();

        Writer->WriteObjectStart(TEXT("identity"));
        Writer->WriteValue(TEXT("transfer_id"), Out.TransferId);
        Writer->WriteObjectEnd();

        Writer->WriteObjectEnd();
        Writer->Close();
    }
    if (!FFileHelper::SaveStringToFile(ManifestText, *ManifestPath))
    {
        Out.Error = TEXT("the manifest could not be written next to the geometry");
        return false;
    }
    Out.ManifestFile = FPaths::GetCleanFilename(ManifestPath);
    Out.ManifestBytes = IFileManager::Get().FileSize(*ManifestPath);

    // The content digest covers the manifest text and the geometry bytes, so a
    // changed handoff is a different handoff and cannot keep the old offer.
    TArray<uint8> DigestInput;
    {
        const FTCHARToUTF8 ManifestUtf8(*ManifestText);
        DigestInput.Append(
            reinterpret_cast<const uint8*>(ManifestUtf8.Get()), ManifestUtf8.Length());
    }
    {
        TArray<uint8> GeometryFileBytes;
        if (FFileHelper::LoadFileToArray(GeometryFileBytes, *FbxPath))
        {
            DigestInput.Append(GeometryFileBytes);
        }
    }
    Out.ManifestDigest = FMD5::HashBytes(DigestInput.GetData(), DigestInput.Num());
    Out.TransferId = FString::Printf(TEXT("mtou-scene-ref-%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));

    const TSharedRef<FJsonObject> Scope = MakeShared<FJsonObject>();
    Scope->SetStringField(TEXT("level"), Out.LevelPackage);
    {
        TArray<TSharedPtr<FJsonValue>> Sublevels;
        for (const FString& Sublevel : Out.LoadedSublevels)
        {
            Sublevels.Add(MakeShared<FJsonValueString>(Sublevel));
        }
        Scope->SetArrayField(TEXT("sublevels"), Sublevels);
    }
    Scope->SetStringField(TEXT("completeness"),
        Out.bScopeComplete ? TEXT("confirmed") : TEXT("loaded_only"));
    {
        TArray<TSharedPtr<FJsonValue>> Unloaded;
        for (const FString& Sublevel : Out.UnloadedSublevels)
        {
            Unloaded.Add(MakeShared<FJsonValueString>(Sublevel));
        }
        Scope->SetArrayField(TEXT("unloaded_sublevels"), Unloaded);
    }

    FString OfferError;
    if (!MtoUOfferReferenceHandoff(
            Out.TransferId,
            Out.Directory,
            Out.ManifestDigest,
            Scope,
            Out.GeometryBytes + Out.ManifestBytes,
            Out.ExportedObjectCount,
            Out.TriangleCount,
            OfferError))
    {
        // No session to offer to is not a failed export: the handoff stays on
        // disk with its manifest and its digest, and the caller sees the reason.
        Out.Warnings.Add(OfferError);
    }
    Out.bSucceeded = true;
    return true;
}

void FMtoUSceneRefExport::ScanFbxTextureReferences(const FString& FilePath, FMtoUTextureScan& Out)
{
    ScanTextureRecords(FilePath, Out);
}

bool FMtoUSceneRefExport::HasImageData(const FMtoUSceneRefExportResult& Result)
{
    return !Result.ImageFiles.IsEmpty() || Result.TextureScan.EmbeddedMediaRecords > 0;
}

