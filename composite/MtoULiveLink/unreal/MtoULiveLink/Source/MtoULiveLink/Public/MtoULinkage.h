#pragma once

#include "CoreMinimal.h"

#include "MtoULinkageTypes.h"

class AMtoULiveLinkActor;

/**
 * Preview linkage boundary (protocol v10).
 *
 * The runtime module owns the connection, the explicit pairing, the mode
 * transitions and the staged pose-group application. The editor module owns
 * the Level Sequence writers, the evaluated camera/time and the scene-reference
 * handoff, and reaches the runtime only through these functions; neither module
 * includes the other's private headers.
 *
 * Every function runs on the game thread.
 */

/** Register the editor-side hooks; an empty struct detaches them. */
MTOULIVELINK_API void MtoUSetLinkageHooks(const FMtoULinkageHooks& Hooks);

/** Pair the actor's Binding with one Maya object id. */
MTOULIVELINK_API bool MtoUPairLinkageTarget(
    const AMtoULiveLinkActor& Actor, const FString& ObjectId, FString& OutError);

/** Drop the actor's pairing; a running session keeps its negotiated set. */
MTOULIVELINK_API void MtoUClearLinkageTarget(const AMtoULiveLinkActor& Actor);

/** The object id the actor is paired as (`character` or `secondary`), or empty. */
MTOULIVELINK_API FString MtoUGetLinkagePairedObject(const AMtoULiveLinkActor& Actor);

/** The placed actor paired with one object id, or nullptr. */
MTOULIVELINK_API AMtoULiveLinkActor* MtoUFindLinkagePairedActor(const FString& ObjectId);

/**
 * Remove the second object from the running set: its writers and displays
 * are restored and Maya continues with the primary character only.
 */
MTOULIVELINK_API bool MtoURequestRemoveSecondaryObject(FString& OutError);

/** Current status snapshot for the panel and the editor pump. */
MTOULIVELINK_API FMtoULinkageView MtoUGetLinkageView();

/** Scheduling measurement of the active session (see FMtoULinkageScheduleStats). */
MTOULIVELINK_API FMtoULinkageScheduleStats MtoUGetLinkageScheduleStats();

/** Start a new measurement window in the active session. */
MTOULIVELINK_API void MtoUResetLinkageScheduleStats();

/** Enter the linkage mode; the editor must have taken over the target first. */
MTOULIVELINK_API bool MtoURequestLinkageEnter(int32 ModeRevision, FString& OutError);

/** Leave the linkage mode for `realtime` or `end`. */
MTOULIVELINK_API bool MtoURequestLinkageLeave(
    const FString& Target, int32 ModeRevision, FString& OutError);

/** Tell Maya that a finished reference handoff is available to import. */
MTOULIVELINK_API bool MtoUOfferReferenceHandoff(
    const FString& TransferId,
    const FString& Directory,
    const FString& ManifestDigest,
    const TSharedPtr<class FJsonObject>& Scope,
    int64 ByteCount,
    int32 ObjectCount,
    int32 TriangleCount,
    FString& OutError);
