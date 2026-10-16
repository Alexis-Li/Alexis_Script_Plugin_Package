#pragma once

#include "CoreMinimal.h"
#include "MtoULinkageTypes.h"
#include "MtoULiveLinkProtocol.h"

class FMtoULiveLinkSource;
class AMtoULiveLinkActor;
struct FMtoUCameraTimeTarget;
struct FMtoUHelloMessage;
struct FMtoUModeMessage;
struct FMtoULeaveLinkageMessage;
struct FMtoUPoseGroupMessage;
struct FMtoUReferenceResultMessage;
struct FMtoUReferenceRemovedMessage;
struct FMtoUReferenceQueryMessage;
struct FMtoUAppliedComponent;

/**
 * The one Live Link source that owns the active linkage session. The cached
 * playback actions reach the source the same way, so the public linkage
 * functions of `MtoULinkage.h` never include a private source header.
 */
MTOULIVELINK_API TWeakPtr<FMtoULiveLinkSource> MtoUGetActiveLinkageSource();

/**
 * One explicit pairing of a Binding actor with one Maya object id. The user
 * performs it; the runtime refuses an `init` until it exists, because a
 * discovered actor is not consent to stream into it.
 */
struct FMtoULinkagePairing
{
    TWeakObjectPtr<AMtoULiveLinkActor> Actor;
    FString ObjectId;
    FString BindingPath;
    FString Generation;
};
/** One staged, immutable pose group and when it arrived. */
struct FMtoUStagedPoseGroup
{
    FMtoUPoseGroupMessage Message;
    double ReceivedSeconds = 0.0;
};

/**
 * Protocol v10 linkage state: explicit pairing, the exclusive time-control
 * mode, the desired/issued/applied target triple and the staged pose groups.
 *
 * Every member is touched on the Game Thread only. The network worker parses
 * the linkage messages and hands them to the owning source, which drains them
 * here; the editor module reaches this state only through the free functions
 * of `MtoULinkage.h`, which are implemented in MtoULinkageSession.cpp.
 */
class FMtoULinkageSession
{
public:
    /** Editor-side seam: the hooks the runtime asks the editor to run. */
    void SetHooks(const FMtoULinkageHooks& Hooks);
    const FMtoULinkageHooks& GetHooks() const { return Hooks; }

    /** Pair the actor's Binding with one Maya object id. */
    bool PairTarget(const AMtoULiveLinkActor& Actor, const FString& ObjectId,
        FString& OutError, FString& OutErrorCode);
    /** Drop the actor's pairing; a running session keeps its negotiated set. */
    void ClearTarget(const AMtoULiveLinkActor& Actor);
    /** The pairing that owns one object id, or nullptr. */
    const FMtoULinkagePairing* FindPairing(const FString& ObjectId) const;
    bool IsPaired(const FString& ObjectId) const;

    /** True while this eval is the issued target; a cancelled one is not. */
    bool IsIssued(const FString& EvalId) const
    {
        return Issued.IsSet() && Issued->EvalId == EvalId;
    }
    /**
     * True while this exact issue of the eval is in flight. A discrete event
     * that cancels a target and issues the same evaluation again starts a new
     * issue, so work published for the cancelled one is never confirmed.
     */
    bool IsIssued(const FString& EvalId, int32 InIssueSerial) const
    {
        return IsIssued(EvalId) && IssueSerial == InIssueSerial;
    }
    /** Identity of the latest issue; increases with every issued target. */
    int32 GetIssueSerial() const { return IssueSerial; }

    /** Current status snapshot for the panel and the editor pump. */
    FMtoULinkageView GetView() const;

    /** Identity of the streaming session this state belongs to. */
    int32 GetSessionId() const { return SessionId; }

    /**
     * One TCP connection became the current session. The greeting is answered
     * before any object is negotiated, so this only establishes the identity
     * the later messages must repeat.
     */
    void BeginSession(int32 SessionId);
    /** The connection ended; every borrowed resource is restored. */
    void EndSession();

    /** The greeting arrived; the answer names the current pairing and space. */
    void NoteHello(const FMtoUHelloMessage& Hello);
    /** Maya's authority statement after one transition. */
    void NoteMode(const FMtoUModeMessage& Message, double NowSeconds);
    /** Maya confirms it restored its own timeline, view and gate. */
    void NoteLeaveLinkage(const FMtoULeaveLinkageMessage& Message, double NowSeconds);
    /** One complete pose sample answering one target. */
    void StagePoseGroup(const FMtoUPoseGroupMessage& Message, double NowSeconds);
    /** The local handoff import, swap or removal finished. */
    void NoteReferenceResult(const FMtoUReferenceResultMessage& Message);
    /** The user removed the tool-owned reference container. */
    void NoteReferenceRemoved(const FMtoUReferenceRemovedMessage& Message);
    /** Maya asks for the handoff of a scope the user selected in Unreal. */
    void NoteReferenceQuery(const FMtoUReferenceQueryMessage& Message);

    /** The negotiated identity of the streaming session. */
    void NegotiateIdentity(const FString& ObjectId, int32 SetRevision, int32 SpaceRevision);

    /**
     * Take the next staged group that passed the identity checks; a refused
     * group is consumed, reported and never applied, so the last complete
     * display stays.
     */
    bool TakeNextStagedGroup(FMtoUPoseGroupMessage& OutMessage);

    /** Record the actual component evidence and answer with `applied`. */
    void NoteGroupApplied(const FString& EvalId, int32 SampleSerial, bool bApplied,
        double AppliedDisplayFrame, const TArray<FMtoUAppliedComponent>& Components,
        const FString& Reason, double NowSeconds);

    /**
     * Game-thread pump: current targets, staged groups and the view. With
     * bAskEditorNow the editor is asked for the current target regardless of
     * the pump cadence, so a discrete event is seen before published work is
     * resolved.
     */
    void Tick(double NowSeconds, bool bAskEditorNow = false);

    /** Enter the linkage mode; the editor must have taken over first. */
    bool RequestEnter(int32 ModeRevision, FString& OutError, FString& OutErrorCode);
    /** Leave the linkage mode for `realtime` or `end`. */
    bool RequestLeave(const FString& Target, int32 ModeRevision,
        FString& OutError, FString& OutErrorCode);

    /** Tell Maya that a finished reference handoff is available to import. */
    bool OfferReferenceHandoff(const FString& TransferId, const FString& Directory,
        const FString& ManifestDigest, const TSharedPtr<FJsonObject>& Scope,
        int64 ByteCount, int32 ObjectCount, int32 TriangleCount, FString& OutError);

    /** Reply sink installed by the owning source; runs on the Game Thread. */
    TFunction<void(TArray<uint8>&&, bool bControl)> SendReply;

    /**
     * True once a failed restore ended the session; the source consumes the
     * request and closes the connection through the shared termination
     * boundary. Consuming never loses a second request.
     */
    bool ConsumeSessionEndRequest();

private:
    static double Now();
    // Bounded cadence of the camera/time pump: the editor is asked at most
    // once per interval and never more than one target is in flight.
    static constexpr double CameraPumpIntervalSeconds = 0.01;

    void SendControl(TArray<uint8>&& Packet);
    void SendError(const FString& Code, const FString& Message, const FString& Details);
    void PublishView();
    void UpdateConvergence(double NowSeconds);
    void PumpCameraTargets(double NowSeconds, bool bAskEditorNow);
    void IssueTarget(const FMtoUCameraTimeTarget& Target, double NowSeconds);
    bool IsCutChange(const FMtoUCameraTimeTarget& Left, const FMtoUCameraTimeTarget& Right) const;
    // One shared half of an entering transition: the pairing must still
    // resolve, the editor must isolate the writers, and only then does the
    // mode change. Both an editor request and a Maya-initiated `mode` run it.
    bool BeginTakeover(int32 ModeRevision, FString& OutError, FString& OutErrorCode,
        bool bAwaitPeerConfirmation);
    void FailTransition(const FString& Code, const FString& Detail);
    // The takeover's sequence, world or target went away: every borrowed
    // writer is restored, the stale synchronisation is dropped and Maya hears
    // that linkage ended so it restores its own state too.
    void EndForLostTakeover(const FString& Reason);
    void RestoreBorrowedWriters(FString& OutResidual);

    FMtoULinkageHooks Hooks;

    int32 SessionId = 0;
    int32 SetRevision = 0;
    int32 SpaceRevision = 1;
    FString SessionObjectId;

    FString Mode = TEXT("realtime");
    FString Control = TEXT("maya");
    int32 ModeRevision = 0;
    bool bTransitioning = false;
    double TransitionStartedSeconds = 0.0;
    bool bWritersBorrowed = false;

    TOptional<FMtoUCameraTimeTarget> Issued;
    TOptional<FMtoUCameraTimeTarget> Desired;
    double IssuedSeconds = 0.0;
    int32 TransportSerial = 0;
    int32 FrameSerial = 0;
    int32 IssueSerial = 0;
    double LastPumpSeconds = 0.0;
    bool bSessionEndRequested = false;

    TArray<FMtoUStagedPoseGroup> Staged;
    FString LastAppliedEvalId;
    int32 LastAppliedSampleSerial = 0;
    bool bLastApplied = false;
    FString LastAppliedReason;
    double LastAppliedDisplayFrame = 0.0;
    double LastAppliedLagSeconds = 0.0;
    bool bConverged = false;

    FString Detail;
    FString Error;
    FString ErrorCode;
    // Transfers this session offered, so a reference result for a handoff that
    // was never offered cannot report an outcome.
    TSet<FString> OfferedTransferIds;
};
