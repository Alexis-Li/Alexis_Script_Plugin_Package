#pragma once

#include "CoreMinimal.h"

#include "MtoULinkageTypes.h"

class AActor;
class ISequencer;
class ULevelSequence;
class ULevelSequencePlayer;
class UMovieSceneTrack;
class UObject;
class UWorld;

/**
 * Editor half of the protocol v10 preview linkage boundary.
 *
 * The runtime module owns the connection, the explicit pairing, the mode
 * transitions and the staged pose-group application, and calls this module
 * through `FMtoULinkageHooks`. This module owns everything the Level Sequence
 * side of that boundary needs: the open sequence's evaluated camera and time,
 * the writers that drive the paired target, and the editor-local mute/restore
 * of exactly those writers.
 *
 * Neither module includes the other's private headers: production code reaches
 * the runtime only through `MtoULinkage.h`, and the runtime reaches this module
 * only through the hook struct. Every function runs on the game thread.
 */
class FMtoULinkageEditor
{
public:
    /** Registers the editor-side hooks with the runtime; once per editor module startup. */
    static void InstallHooks();

    /**
     * Detaches the hooks. A takeover that is still active stays owned by this
     * module so a later `RestoreTarget` can still return the writers.
     */
    static void UninstallHooks();

    /** Where a camera/time evaluation reads its sequence state from. */
    struct FEvaluationSource
    {
        ULevelSequence* Sequence = nullptr;
        UWorld* World = nullptr;
        /** The open editor Sequencer, when the sequence is open in one. */
        TSharedPtr<ISequencer> Sequencer;
        /** A player the caller owns, used when no editor Sequencer is open. */
        ULevelSequencePlayer* Player = nullptr;
    };

    /**
     * Fills OutTarget from the open Level Sequence and its evaluated camera.
     * Returns false, with OutError naming what is missing, when there is no
     * open sequence, no evaluated camera or no camera content to send.
     *
     * The evaluation identity is `<sequence>@<tick>+<milli-tick>/<camera path>
     * #<camera content digest>`, so a moved playhead, a camera cut, a camera
     * change and an edit of the camera's own content each start a new target
     * generation while an unchanged heartbeated target keeps its serial.
     */
    static bool EvaluateCameraTime(
        const FEvaluationSource& Source, FMtoUCameraTimeTarget& OutTarget, FString& OutError);

    /** One writer that drives one of the paired target's objects. */
    struct FWriterRecord
    {
        TWeakObjectPtr<UMovieSceneTrack> Track;
        /** Engine branch this writer drives, for the panel and the tests. */
        FString Kind;
        /** Name of the binding the writer belongs to. */
        FString BindingName;
        /** Enabled rows this record owns; an empty list owns the whole track. */
        TArray<int32> Rows;
    };

    /**
     * Every writer that drives any of the target objects, in a stable order.
     * Writers that only drive unrelated objects are not listed; a writer that
     * drives the target *and* something else is refused, not listed.
     */
    static TArray<FWriterRecord> CollectTargetWriters(
        const FEvaluationSource& Source,
        const TArray<UObject*>& TargetObjects,
        const AActor* TargetAnchor,
        FString& OutError);

    /**
     * Mutes exactly the writers that drive the target objects, with the
     * editor-local mute flag that is never serialized with the asset, and
     * evaluates the sequence once so the released objects stop holding the
     * writers' last pose. The sequence's own playback is never paused.
     *
     * Refuses before writing, naming one of the v10 rejection codes:
     * `WRITER_NOT_ISOLATABLE` when a writer cannot be muted without touching
     * unrelated tracks, `SHARED_WRITER` when a writer drives another object
     * too, and `MOVING_SOCKET` when the target hangs from a moving socket.
     */
    static bool TakeOverTarget(
        const FEvaluationSource& Source,
        const TArray<UObject*>& TargetObjects,
        const AActor* TargetAnchor,
        FString& OutError,
        FString& OutErrorCode);

    /**
     * Restores every writer this module muted, per resource and idempotently,
     * and evaluates the sequence once so the writers' own poses return. False
     * names the resource that could not be restored instead of hiding it.
     */
    static bool RestoreTarget(FString& OutError, FString& OutErrorCode);

    /**
     * Remembered pairing the panel established. The hook prefers it and falls
     * back to resolving the runtime view's target path, so a pairing the
     * runtime still holds stays reachable after a panel reload.
     */
    static void SetObservedLinkageActor(TWeakObjectPtr<AActor> Actor);
    static TWeakObjectPtr<AActor> GetObservedLinkageActor();

    /**
     * A panel widget that is invalidated whenever the runtime publishes a new
     * linkage view, so the curated rows re-read `MtoUGetLinkageView()` instead
     * of waiting for the next selection change.
     */
    static void RegisterLinkagePanelWidget(TWeakPtr<class SWidget> Widget);

    /** Raised when the runtime published a new linkage view; the panel invalidates itself. */
    static FSimpleMulticastDelegate& OnLinkageViewChanged();
};
