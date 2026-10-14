#pragma once

#include "CoreMinimal.h"

class UWorld;

/** What one scan of a produced ASCII FBX found, judged by the handoff's own rules. */
struct FMtoUTextureScan
{
    /** `Texture:` records, whatever they name. */
    int32 TextureRecords = 0;
    /** Texture records that name a file, which is what an import would try to load. */
    int32 TextureReferences = 0;
    /** `Video:` records, which usually mirror the same media. */
    int32 VideoRecords = 0;
    /** `Content:` lines inside a texture or video record, embedded payload or not. */
    int32 ContentRecords = 0;
    /** Records whose `Content:` line carries a payload; non zero means embedded media. */
    int32 EmbeddedMediaRecords = 0;
    /** One descriptor per embedded media record, e.g. `Video::T_Rock (Content 4096 chars)`. */
    TArray<FString> EmbeddedMedia;
};

/** Everything one scene-reference handoff run produced and checked. */
struct FMtoUSceneRefExportResult
{
    bool bSucceeded = false;
    FString Error;
    TArray<FString> Warnings;

    /** The explicit scope: the loaded level plus the requested sublevels. */
    FString ScopeName;
    FString LevelPackage;
    TArray<FString> RequestedSublevels;
    TArray<FString> LoadedSublevels;
    TArray<FString> ExcludedSublevels;
    TArray<FString> UnloadedSublevels;
    /** False when a requested sublevel was not loaded, so the scope is partial. */
    bool bScopeComplete = true;

    /** The produced handoff the Maya side may import. */
    FString Directory;
    FString GeometryFile;
    FString ManifestFile;
    int64 GeometryBytes = 0;
    int64 ManifestBytes = 0;
    int32 ExportedObjectCount = 0;
    int32 SkippedActorCount = 0;
    int32 SuppressedComponentCount = 0;
    int32 TriangleCount = 0;
    int32 VertexCount = 0;

    /** The image-data check: a handoff carries no media, ever. */
    FMtoUTextureScan TextureScan;
    TArray<FString> ImageFiles;
    bool bImageDataDetected = false;

    /** Handoff identity Maya echoes back on `reference_result`. */
    FString TransferId;
    FString ManifestDigest;
};

/**
 * The editor side of the scene-reference handoff (protocol v10).
 *
 * The scope is explicit: the loaded persistent level plus the sublevels the user
 * asked for, never an automatic load and never an Actor-level selection. The
 * geometry comes from the engine's own FBX level export over exactly the static
 * mesh components of that scope; every other component of an exported actor is
 * suppressed for the duration of the export and restored afterwards. The run
 * fails closed: embedded media, an image file, a missing manifest or a runtime
 * refuse to hand off leaves no offer behind.
 *
 * Runs on the game thread.
 */
class FMtoUSceneRefExport
{
public:
    /**
     * Resolves the scope, exports, writes the manifest, checks it for image data
     * and offers the finished handoff to the linked session. Returns false with
     * Out.Error set and no offer made on any refusal.
     */
    static bool Run(
        UWorld& World,
        const TArray<FString>& RequestedSublevels,
        FMtoUSceneRefExportResult& Out);

    /** Scans an ASCII FBX for texture, video and embedded media records. */
    static void ScanFbxTextureReferences(const FString& FilePath, FMtoUTextureScan& Out);

    /** True when the scan or the produced files carry image data. */
    static bool HasImageData(const FMtoUSceneRefExportResult& Result);
};
