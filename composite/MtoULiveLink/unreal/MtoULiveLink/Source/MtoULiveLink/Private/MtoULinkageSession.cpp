#include "MtoULinkageSession.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkProtocol.h"
#include "MtoULiveLinkSource.h"
#include "MtoULinkage.h"

#include "Engine/World.h"

namespace
{
/** One placed Editor-world Binding actor is the only pairable target. */
bool IsEligiblePairingActor(const AMtoULiveLinkActor& Actor)
{
    if (Actor.HasAnyFlags(RF_ClassDefaultObject) || !IsValid(&Actor))
    {
        return false;
    }
    const UWorld* World = Actor.GetWorld();
    return World && World->WorldType == EWorldType::Editor;
}
}

// The one module-level hooks seam. A source that starts before the editor
// registered adopts them, so the registration order never loses a hook.
static FMtoULinkageHooks GMtoULinkageHooks;

// One listener serves the Editor session, so the pairing registry is
// process-wide: the user's pairing of a Binding actor with one Maya object id
// survives a source restart (module reload, automation host) instead of
// silently requiring a re-pair, and every source refuses an `init` for an
// object that no pairing names.
static TArray<FMtoULinkagePairing> GMtoULinkagePairings;
static int32 GMtoULinkagePairingGeneration = 0;

double FMtoULinkageSession::Now()
{
    return FPlatformTime::Seconds();
}

void FMtoULinkageSession::SetHooks(const FMtoULinkageHooks& InHooks)
{
    Hooks = InHooks;
    GMtoULinkageHooks = InHooks;
}

bool FMtoULinkageSession::PairTarget(
    const AMtoULiveLinkActor& Actor,
    const FString& ObjectId,
    FString& OutError,
    FString& OutErrorCode)
{
    OutError.Reset();
    OutErrorCode = TEXT("PAIRING_REQUIRED");
    if (ObjectId.IsEmpty())
    {
        OutError = TEXT("A linkage pairing requires a non-empty Maya object id.");
        OutErrorCode = TEXT("INVALID_MESSAGE");
        return false;
    }
    if (!IsEligiblePairingActor(Actor))
    {
        OutError = TEXT("Pair a placed Editor-world MtoU_LiveLink binding actor; PIE is not supported.");
        OutErrorCode = TEXT("TARGET_UNKNOWN");
        return false;
    }

    // Drop pairings whose actor no longer resolves, so a destroyed target can
    // never block a new pairing of the same object id.
    GMtoULinkagePairings.RemoveAll([](const FMtoULinkagePairing& Pairing)
    {
        return !Pairing.Actor.IsValid() || !IsEligiblePairingActor(*Pairing.Actor);
    });

    for (const FMtoULinkagePairing& Existing : GMtoULinkagePairings)
    {
        if (Existing.ObjectId == ObjectId && Existing.Actor.Get() != &Actor)
        {
            OutError = FString::Printf(
                TEXT("Maya object '%s' is already paired with %s; one target cannot serve two objects."),
                *ObjectId,
                *Existing.BindingPath);
            OutErrorCode = TEXT("TARGET_ALREADY_PAIRED");
            return false;
        }
    }

    ++GMtoULinkagePairingGeneration;
    FMtoULinkagePairing* Existing = nullptr;
    for (FMtoULinkagePairing& Pairing : GMtoULinkagePairings)
    {
        if (Pairing.Actor.Get() == &Actor)
        {
            Existing = &Pairing;
            break;
        }
    }
    if (!Existing)
    {
        Existing = &GMtoULinkagePairings.AddDefaulted_GetRef();
        Existing->Actor = const_cast<AMtoULiveLinkActor*>(&Actor);
    }
    Existing->ObjectId = ObjectId;
    Existing->BindingPath = Actor.GetPathName();
    Existing->Generation = FString::FromInt(GMtoULinkagePairingGeneration);
    OutErrorCode.Reset();
    Detail = FString::Printf(TEXT("Paired Maya object '%s' with %s."), *ObjectId, *Existing->BindingPath);
    PublishView();
    return true;
}

void FMtoULinkageSession::ClearTarget(const AMtoULiveLinkActor& Actor)
{
    // A running session keeps its negotiated set; only the pairing is gone.
    GMtoULinkagePairings.RemoveAll([&Actor](const FMtoULinkagePairing& Pairing)
    {
        return Pairing.Actor.Get() == &Actor;
    });
    PublishView();
}

const FMtoULinkagePairing* FMtoULinkageSession::FindPairing(const FString& ObjectId) const
{
    for (const FMtoULinkagePairing& Pairing : GMtoULinkagePairings)
    {
        if (Pairing.ObjectId == ObjectId && Pairing.Actor.IsValid())
        {
            return &Pairing;
        }
    }
    return nullptr;
}

bool FMtoULinkageSession::IsPaired(const FString& ObjectId) const
{
    return FindPairing(ObjectId) != nullptr;
}

FMtoULinkageView FMtoULinkageSession::GetView() const
{
    FMtoULinkageView View;
    View.Mode = Mode;
    View.Control = Control;
    View.Identity.SessionId = SessionId;
    View.Identity.SetRevision = SetRevision;
    View.Identity.SpaceRevision = SpaceRevision;
    View.Identity.ModeRevision = ModeRevision;
    // Before a session negotiates, the panel still needs the pairing this
    // source would use, so an unnegotiated session falls back to the one
    // paired target exactly as the greeting answer does.
    const FMtoULinkagePairing* Pairing = SessionObjectId.IsEmpty()
        ? (GMtoULinkagePairings.Num() == 1 ? &GMtoULinkagePairings[0] : nullptr)
        : FindPairing(SessionObjectId);
    View.bPaired = Pairing != nullptr;
    if (Pairing)
    {
        View.PairedObjectId = Pairing->ObjectId;
        View.TargetPath = Pairing->BindingPath;
        View.TargetKind = TEXT("binding_actor");
    }
    if (Issued.IsSet())
    {
        View.TargetEvalId = Issued->EvalId;
        View.TargetDisplayFrame = Issued->DisplayFrame;
    }
    else if (Desired.IsSet())
    {
        View.TargetEvalId = Desired->EvalId;
        View.TargetDisplayFrame = Desired->DisplayFrame;
    }
    View.AppliedEvalId = LastAppliedEvalId;
    View.AppliedSampleSerial = LastAppliedSampleSerial;
    View.AppliedDisplayFrame = LastAppliedDisplayFrame;
    View.AppliedLagSeconds = LastAppliedLagSeconds;
    View.bConverged = bConverged;
    View.bTransitioning = bTransitioning;
    View.Detail = Detail;
    View.Error = Error;
    View.ErrorCode = ErrorCode;
    return View;
}

void FMtoULinkageSession::BeginSession(int32 InSessionId)
{
    SessionId = InSessionId;
    SetRevision = 0;
    SpaceRevision = 1;
    SessionObjectId.Reset();
    Mode = TEXT("realtime");
    Control = TEXT("maya");
    ModeRevision = 0;
    bTransitioning = false;
    bWritersBorrowed = false;
    bSessionEndRequested = false;
    Issued.Reset();
    Desired.Reset();
    TransportSerial = 0;
    FrameSerial = 0;
    LastPumpSeconds = 0.0;
    Staged.Reset();
    OfferedTransferIds.Reset();
    LastAppliedEvalId.Reset();
    LastAppliedSampleSerial = 0;
    bLastApplied = false;
    LastAppliedReason.Reset();
    LastAppliedDisplayFrame = 0.0;
    LastAppliedLagSeconds = 0.0;
    bConverged = false;
    Detail.Reset();
    Error.Reset();
    ErrorCode.Reset();
    PublishView();
}

void FMtoULinkageSession::EndSession()
{
    // The takeover is keyed on what is actually borrowed, not on the mode: a
    // disconnect while the writers are muted must hand them back even when the
    // session already returned to realtime.
    if (bWritersBorrowed)
    {
        FString Residual;
        RestoreBorrowedWriters(Residual);
        if (!Residual.IsEmpty())
        {
            Error = Residual;
            ErrorCode = TEXT("RECOVERY_REFUSED");
        }
    }
    Mode = TEXT("realtime");
    Control = TEXT("maya");
    ModeRevision = 0;
    bTransitioning = false;
    bWritersBorrowed = false;
    Issued.Reset();
    Desired.Reset();
    Staged.Reset();
    bConverged = false;
    Detail = TEXT("Disconnected; the linkage takeover was restored.");
    PublishView();
}

void FMtoULinkageSession::NoteHello(const FMtoUHelloMessage& Hello)
{
    (void)Hello;
    // The greeting is answered before any object is negotiated: the session
    // identity, the capabilities, the explicitly selected target (empty until
    // the user pairs) and the common space both sides must match.
    GMtoULinkagePairings.RemoveAll([](const FMtoULinkagePairing& Pairing)
    {
        return !Pairing.Actor.IsValid() || !IsEligiblePairingActor(*Pairing.Actor);
    });
    // Exactly one paired target can be named before the greeting answers; a
    // peer with several pairings hears that none was selected yet.
    const FMtoULinkagePairing* Pairing = GMtoULinkagePairings.Num() == 1
        ? &GMtoULinkagePairings[0]
        : nullptr;
    const TSharedRef<FJsonObject> Anchor = MakeShared<FJsonObject>();
    SendControl(FMtoUProtocol::EncodeHelloAck(
        SessionId,
        true,
        true,
        true,
        true,
        Pairing ? Pairing->ObjectId : FString(),
        Pairing ? TEXT("binding_actor") : FString(),
        Pairing ? Pairing->BindingPath : FString(),
        Pairing ? Pairing->Generation : FString(),
        SpaceRevision,
        Anchor));
    Detail = FString::Printf(TEXT("Greeted %s (Maya %s)."), *Hello.Host, *Hello.MayaVersion);
    PublishView();
}

void FMtoULinkageSession::NegotiateIdentity(
    const FString& ObjectId, int32 InSetRevision, int32 InSpaceRevision)
{
    SessionObjectId = ObjectId;
    SetRevision = InSetRevision;
    SpaceRevision = InSpaceRevision;
    PublishView();
}

void FMtoULinkageSession::NoteMode(const FMtoUModeMessage& Message, double NowSeconds)
{
    if (Message.SessionId != SessionId)
    {
        SendError(
            TEXT("INVALID_MESSAGE"),
            TEXT("A mode message named another session."),
            FString::Printf(TEXT("message session %d, current session %d"), Message.SessionId, SessionId));
        return;
    }

    if (Message.Transition == TEXT("failed"))
    {
        // A transition that fails on either host keeps the previous mode and
        // restores what the transition borrowed, per resource and idempotent.
        // The rollback keys on the revision the message names rather than on
        // the transient flag: a Maya-initiated takeover deliberately clears
        // the flag while the session still holds linkage, and that is exactly
        // the case the previous code left half-entered.
        if (Message.ModeRevision == ModeRevision || bTransitioning)
        {
            FailTransition(TEXT("MODE_CONFLICT"), Message.Detail);
        }
        else
        {
            SendError(
                TEXT("MODE_CONFLICT"),
                TEXT("A failed transition named a mode revision this session never entered."),
                FString::Printf(TEXT("message mode revision %d, current %d"),
                    Message.ModeRevision, ModeRevision));
        }
        return;
    }

    if (Message.Mode == TEXT("linkage"))
    {
        if (Mode == TEXT("linkage") && ModeRevision == Message.ModeRevision && bTransitioning)
        {
            // Maya confirmed the transition Unreal started: the pose gate opens.
            bTransitioning = false;
            Detail = TEXT("Maya follows the Unreal sequence time.");
            PublishView();
            return;
        }
        if (Mode == TEXT("linkage"))
        {
            SendError(
                TEXT("MODE_CONFLICT"),
                TEXT("Linkage is already active."),
                FString::Printf(TEXT("current mode revision %d, message %d"), ModeRevision, Message.ModeRevision));
            return;
        }
        // Maya started the transition: Unreal performs its own half.
        FString TakeError;
        FString TakeCode;
        if (!BeginTakeover(Message.ModeRevision, TakeError, TakeCode, false))
        {
            SendControl(FMtoUProtocol::EncodeModeState(
                SessionId, ModeRevision, Mode, Control, TakeError));
            Error = TakeError;
            ErrorCode = TakeCode;
            PublishView();
            return;
        }
        return;
    }

    // realtime or cached: Maya returned to its own time authority.
    if (Message.ModeRevision <= ModeRevision)
    {
        SendError(
            TEXT("MODE_CONFLICT"),
            TEXT("A mode message named an older or repeated mode revision."),
            FString::Printf(TEXT("current mode revision %d, message %d"), ModeRevision, Message.ModeRevision));
        return;
    }
    ModeRevision = Message.ModeRevision;
    Mode = Message.Mode;
    Control = Message.Control.IsEmpty() ? TEXT("maya") : Message.Control;
    Issued.Reset();
    Desired.Reset();
    Staged.Reset();
    bTransitioning = false;
    bConverged = false;
    Detail = Message.Detail.IsEmpty()
        ? TEXT("Maya owns its own timeline again.")
        : Message.Detail;
    SendControl(FMtoUProtocol::EncodeModeState(
        SessionId, ModeRevision, Mode, Control, Detail));
    PublishView();
}

void FMtoULinkageSession::NoteLeaveLinkage(
    const FMtoULeaveLinkageMessage& Message, double NowSeconds)
{
    (void)NowSeconds;
    if (Message.SessionId != SessionId)
    {
        SendError(
            TEXT("INVALID_MESSAGE"),
            TEXT("A leave_linkage message named another session."),
            FString::Printf(TEXT("message session %d, current session %d"), Message.SessionId, SessionId));
        return;
    }
    if (Mode != TEXT("linkage"))
    {
        if (Message.Target == TEXT("end") && bWritersBorrowed)
        {
            FString Residual;
            RestoreBorrowedWriters(Residual);
            Error = Residual.IsEmpty() ? FString() : Residual;
            ErrorCode = Residual.IsEmpty() ? FString() : TEXT("RECOVERY_REFUSED");
            Detail = TEXT("The preview ended and the original writers were restored.");
            PublishView();
            return;
        }
        SendError(
            TEXT("LINKAGE_NOT_ACTIVE"),
            TEXT("Linkage is not active."),
            TEXT("Maya confirmed a leave while Unreal already holds its own time."));
        return;
    }
    if (Message.ModeRevision <= ModeRevision)
    {
        SendError(
            TEXT("MODE_CONFLICT"),
            TEXT("A leave_linkage message named an older or repeated mode revision."),
            FString::Printf(TEXT("current mode revision %d, message %d"), ModeRevision, Message.ModeRevision));
        return;
    }

    ModeRevision = Message.ModeRevision;
    Issued.Reset();
    Desired.Reset();
    Staged.Reset();
    bTransitioning = false;
    bConverged = false;

    FString Residual;
    if (Message.Target == TEXT("end"))
    {
        RestoreBorrowedWriters(Residual);
    }
    Mode = TEXT("realtime");
    Control = TEXT("maya");
    if (!Residual.IsEmpty())
    {
        // A failed restore ends the session and reports the residual instead
        // of hiding it behind a successful disconnect.
        Detail = Residual;
        Error = Residual;
        ErrorCode = TEXT("RECOVERY_REFUSED");
        bSessionEndRequested = true;
    }
    else
    {
        Detail = Message.Target == TEXT("end")
            ? TEXT("Preview ended; every borrowed writer was restored.")
            : TEXT("Maya restored its own state; Unreal keeps its takeover.");
    }
    SendControl(FMtoUProtocol::EncodeModeState(
        SessionId, ModeRevision, Mode, Control, Detail));
    PublishView();
}

void FMtoULinkageSession::StagePoseGroup(const FMtoUPoseGroupMessage& Message, double NowSeconds)
{
    if (Staged.Num() >= FMtoUProtocol::MaxPendingCameraTargets)
    {
        // The scheduler allows one in-flight answer and one waiting answer;
        // surplus work is refused instead of growing an unbounded backlog.
        SendError(
            TEXT("RESOURCE_LIMIT"),
            TEXT("Linkage groups arrive faster than the game thread applies them."),
            FString::Printf(TEXT("staged groups %d"), Staged.Num()));
        return;
    }
    FMtoUStagedPoseGroup& StagedGroup = Staged.AddDefaulted_GetRef();
    StagedGroup.Message = Message;
    StagedGroup.ReceivedSeconds = NowSeconds;
}

bool FMtoULinkageSession::TakeNextStagedGroup(FMtoUPoseGroupMessage& OutMessage)
{
    while (!Staged.IsEmpty())
    {
        FMtoUStagedPoseGroup Entry = MoveTemp(Staged[0]);
        Staged.RemoveAt(0);
        const FMtoUPoseGroupMessage& Group = Entry.Message;

        if (Group.SessionId != SessionId)
        {
            SendError(
                TEXT("INVALID_MESSAGE"),
                TEXT("A group named another session."),
                FString::Printf(TEXT("group session %d, current session %d"), Group.SessionId, SessionId));
            continue;
        }
        if (bTransitioning)
        {
            // The gate is closed during a transition: nothing is accepted for
            // the previous revision and the last complete display stays.
            SendError(
                TEXT("MODE_CONFLICT"),
                TEXT("A group arrived during a time-control transition."),
                FString::Printf(TEXT("target mode revision %d"), Group.ModeRevision));
            continue;
        }
        if (Mode != TEXT("linkage"))
        {
            SendError(
                TEXT("LINKAGE_NOT_ACTIVE"),
                TEXT("A group arrived while another host owns time."),
                FString::Printf(TEXT("current mode %s"), *Mode));
            continue;
        }
        if (Group.ModeRevision != ModeRevision)
        {
            SendError(
                TEXT("MODE_CONFLICT"),
                TEXT("A group named a different mode revision."),
                FString::Printf(TEXT("group mode revision %d, current %d"), Group.ModeRevision, ModeRevision));
            continue;
        }
        if (Group.SetRevision != SetRevision)
        {
            SendError(
                TEXT("SET_REVISION_STALE"),
                TEXT("A group named an older object set."),
                FString::Printf(TEXT("group set revision %d, current %d"), Group.SetRevision, SetRevision));
            continue;
        }
        if (Group.SpaceRevision != SpaceRevision)
        {
            SendError(
                TEXT("SPACE_REVISION_STALE"),
                TEXT("A group named an older common space."),
                FString::Printf(TEXT("group space revision %d, current %d"), Group.SpaceRevision, SpaceRevision));
            continue;
        }
        if (!Issued.IsSet() || Issued->EvalId != Group.EvalId)
        {
            // The target was cancelled or superseded; the last complete
            // display stays and nothing changes.
            SendError(
                TEXT("EVAL_STALE"),
                TEXT("A group answered a cancelled or superseded target."),
                FString::Printf(TEXT("group eval %s, current target %s"),
                    *Group.EvalId,
                    Issued.IsSet() ? *Issued->EvalId : TEXT("none")));
            continue;
        }
        if (Group.EvalId == LastAppliedEvalId && Group.SampleSerial <= LastAppliedSampleSerial)
        {
            // A repeated publication of an already applied sample is refused.
            SendError(
                TEXT("INVALID_MESSAGE"),
                TEXT("A group repeated a publication that was already applied."),
                FString::Printf(
                    TEXT("group sample_serial %d, applied sample_serial %d for eval %s"),
                    Group.SampleSerial,
                    LastAppliedSampleSerial,
                    *Group.EvalId));
            continue;
        }
        OutMessage = MoveTemp(Entry.Message);
        return true;
    }
    return false;
}

void FMtoULinkageSession::NoteGroupApplied(
    const FString& EvalId,
    int32 SampleSerial,
    bool bApplied,
    double AppliedDisplayFrame,
    const TArray<FMtoUAppliedComponent>& Components,
    const FString& Reason,
    double NowSeconds)
{
    const double LagSeconds = Issued.IsSet()
        ? FMath::Max(0.0, NowSeconds - IssuedSeconds)
        : 0.0;
    LastAppliedEvalId = EvalId;
    LastAppliedSampleSerial = SampleSerial;
    bLastApplied = bApplied;
    LastAppliedReason = Reason;
    LastAppliedDisplayFrame = AppliedDisplayFrame;
    LastAppliedLagSeconds = LagSeconds;

    if (bApplied)
    {
        Detail = FString::Printf(TEXT("Applied %s (sample %d)."), *EvalId, SampleSerial);
        if (Issued.IsSet() && Issued->EvalId == EvalId)
        {
            // The issued work is finished only once its components were read
            // back; a newer waiting target keeps its place. The cadence gate
            // is released so the next target is asked for at once instead of
            // waiting out the interval on a busy game thread.
            Issued.Reset();
            LastPumpSeconds = 0.0;
            if (Desired.IsSet())
            {
                FMtoUCameraTimeTarget Next = Desired.GetValue();
                Desired.Reset();
                IssueTarget(Next, NowSeconds);
            }
        }
    }
    else
    {
        Detail = FString::Printf(TEXT("Application failed for %s: %s"), *EvalId, *Reason);
        Error = Reason;
        ErrorCode = ErrorCode.IsEmpty() ? TEXT("LINKAGE_APPLY_FAILED") : ErrorCode;
    }

    SendControl(FMtoUProtocol::EncodeApplied(
        SessionId, ModeRevision, EvalId, SampleSerial, bApplied, AppliedDisplayFrame,
        LagSeconds, Components, Reason));
    PublishView();
}

void FMtoULinkageSession::Tick(double NowSeconds)
{
    if (bTransitioning
        && NowSeconds - TransitionStartedSeconds > FMtoUProtocol::LinkageConvergenceSeconds * 2.5)
    {
        // A transition Maya never confirms keeps the previous mode instead of
        // leaving the pose gate closed forever.
        FailTransition(TEXT("MODE_CONFLICT"), TEXT("The time-control transition was not confirmed."));
    }
    if (Mode == TEXT("linkage") && !bTransitioning)
    {
        PumpCameraTargets(NowSeconds);
    }
    UpdateConvergence(NowSeconds);
    PublishView();
}

bool FMtoULinkageSession::RequestEnter(int32 InModeRevision, FString& OutError, FString& OutErrorCode)
{
    OutError.Reset();
    OutErrorCode.Reset();
    const auto Refuse = [&](const FString& Code, const FString& Message)
    {
        // A refused transition reports its code instead of changing state.
        OutError = Message;
        OutErrorCode = Code;
        Error = Message;
        ErrorCode = Code;
        PublishView();
        return false;
    };
    if (SessionId == 0)
    {
        return Refuse(TEXT("LINKAGE_NOT_ACTIVE"),
            TEXT("Linkage requires an active streaming session."));
    }
    if (Mode == TEXT("linkage") || bTransitioning)
    {
        return Refuse(TEXT("MODE_CONFLICT"), TEXT("Linkage is already active."));
    }
    if (InModeRevision <= ModeRevision)
    {
        return Refuse(TEXT("MODE_CONFLICT"), FString::Printf(
            TEXT("Mode revision %d is not newer than the current %d."), InModeRevision, ModeRevision));
    }
    return BeginTakeover(InModeRevision, OutError, OutErrorCode, true);
}

bool FMtoULinkageSession::RequestLeave(
    const FString& Target, int32 InModeRevision, FString& OutError, FString& OutErrorCode)
{
    OutError.Reset();
    OutErrorCode.Reset();
    const auto Refuse = [&](const FString& Code, const FString& Message)
    {
        OutError = Message;
        OutErrorCode = Code;
        Error = Message;
        ErrorCode = Code;
        PublishView();
        return false;
    };
    if (Target != TEXT("realtime") && Target != TEXT("end"))
    {
        return Refuse(TEXT("INVALID_MESSAGE"),
            TEXT("A linkage exit target must be 'realtime' or 'end'."));
    }
    if (Mode != TEXT("linkage"))
    {
        // 结束预览 after 返回实时 still owns the original writers: the exit is
        // the statement that hands them back, so it stays available while the
        // takeover is held even though Unreal already follows Maya's time.
        if (Target != TEXT("end") || !bWritersBorrowed)
        {
            return Refuse(TEXT("LINKAGE_NOT_ACTIVE"), TEXT("Linkage is not active."));
        }
        FString Residual;
        RestoreBorrowedWriters(Residual);
        ModeRevision = InModeRevision;
        Mode = TEXT("realtime");
        Control = TEXT("maya");
        Error = Residual.IsEmpty() ? FString() : Residual;
        ErrorCode = Residual.IsEmpty() ? FString() : TEXT("RECOVERY_REFUSED");
        Detail = TEXT("The preview ended and the original writers were restored.");
        SendControl(FMtoUProtocol::EncodeModeState(
            SessionId, ModeRevision, Mode, Control, Detail));
        PublishView();
        return true;
    }
    if (bTransitioning)
    {
        return Refuse(TEXT("MODE_CONFLICT"),
            TEXT("A time-control transition is still in progress."));
    }
    if (InModeRevision <= ModeRevision)
    {
        return Refuse(TEXT("MODE_CONFLICT"), FString::Printf(
            TEXT("Mode revision %d is not newer than the current %d."), InModeRevision, ModeRevision));
    }

    ModeRevision = InModeRevision;
    Issued.Reset();
    Desired.Reset();
    Staged.Reset();
    bTransitioning = false;
    bConverged = false;

    FString Residual;
    if (Target == TEXT("end"))
    {
        RestoreBorrowedWriters(Residual);
    }
    Mode = TEXT("realtime");
    Control = TEXT("maya");
    if (!Residual.IsEmpty())
    {
        Detail = Residual;
        Error = Residual;
        ErrorCode = TEXT("RECOVERY_REFUSED");
        bSessionEndRequested = true;
    }
    else
    {
        Detail = Target == TEXT("end")
            ? TEXT("Preview ended; every borrowed writer was restored.")
            : TEXT("Returned to realtime; Unreal keeps its takeover.");
    }
    SendControl(FMtoUProtocol::EncodeModeState(SessionId, ModeRevision, Mode, Control, Detail));
    PublishView();
    return true;
}

bool FMtoULinkageSession::OfferReferenceHandoff(
    const FString& TransferId,
    const FString& Directory,
    const FString& ManifestDigest,
    const TSharedPtr<FJsonObject>& Scope,
    int64 ByteCount,
    int32 ObjectCount,
    int32 TriangleCount,
    FString& OutError)
{
    OutError.Reset();
    if (SessionId == 0)
    {
        OutError = TEXT("A reference handoff requires an active streaming session.");
        return false;
    }
    if (TransferId.IsEmpty() || Directory.IsEmpty() || !Scope.IsValid())
    {
        OutError = TEXT("A reference handoff requires a transfer id, a directory and a scope.");
        Error = OutError;
        ErrorCode = TEXT("INVALID_MESSAGE");
        PublishView();
        return false;
    }
    if (ManifestDigest.IsEmpty())
    {
        // The digest is what lets Maya verify the directory content, so an
        // empty one cannot describe a handoff.
        OutError = TEXT("A reference handoff requires a non-empty manifest_digest.");
        Error = OutError;
        ErrorCode = TEXT("INVALID_MESSAGE");
        PublishView();
        return false;
    }
    if (ByteCount < 1)
    {
        OutError = TEXT(
            "A reference handoff requires a positive byte count: the offer's "
            "'bytes' field must be a positive integer.");
        Error = OutError;
        ErrorCode = TEXT("INVALID_MESSAGE");
        PublishView();
        return false;
    }
    if (ObjectCount < 0 || TriangleCount < 0)
    {
        OutError = TEXT("A reference handoff cannot name a negative object or triangle count.");
        Error = OutError;
        ErrorCode = TEXT("INVALID_MESSAGE");
        PublishView();
        return false;
    }
    if (ByteCount > FMtoUProtocol::MaxReferenceBytes
        || ObjectCount > FMtoUProtocol::MaxReferenceObjects
        || TriangleCount > FMtoUProtocol::MaxReferenceTriangles)
    {
        OutError = FString::Printf(
            TEXT("The handoff exceeds a frozen reference budget (%lld bytes, %d objects, %d triangles)."),
            ByteCount,
            ObjectCount,
            TriangleCount);
        Error = OutError;
        ErrorCode = TEXT("REFERENCE_OVER_BUDGET");
        PublishView();
        return false;
    }

    SendControl(FMtoUProtocol::EncodeReferenceOffer(
        SessionId, TransferId, Directory, ManifestDigest, Scope, SpaceRevision,
        ByteCount, ObjectCount, TriangleCount));
    OfferedTransferIds.Add(TransferId);
    Detail = FString::Printf(TEXT("Offered reference handoff %s."), *TransferId);
    PublishView();
    return true;
}

void FMtoULinkageSession::NoteReferenceResult(const FMtoUReferenceResultMessage& Message)
{
    if (Message.SessionId != SessionId)
    {
        return;
    }
    if (Message.TransferId.IsEmpty())
    {
        Error = TEXT("A reference result must name its transfer_id.");
        ErrorCode = TEXT("INVALID_MESSAGE");
        PublishView();
        return;
    }
    if (!OfferedTransferIds.Contains(Message.TransferId))
    {
        // A result for a transfer this session never offered names work that
        // never started, so it cannot report an outcome.
        Detail = FString::Printf(
            TEXT("Reference result names transfer_id '%s', which this session "
                 "never offered."),
            *Message.TransferId);
        Error = Detail;
        ErrorCode = TEXT("INVALID_MESSAGE");
        PublishView();
        return;
    }
    Detail = FString::Printf(
        TEXT("Reference %s %s: %s (%lld objects, %lld triangles, %lld bytes)."),
        *Message.TransferId,
        Message.bAccepted ? TEXT("accepted") : TEXT("refused"),
        *Message.Detail,
        Message.ObjectCount,
        Message.TriangleCount,
        Message.ByteCount);
    if (!Message.bAccepted)
    {
        Error = Message.Detail;
        // The importer names why it refused the handoff; a fixed code would
        // report every refusal as a stale space revision.
        ErrorCode = Message.Code.IsEmpty() ? TEXT("REFERENCE_STALE") : Message.Code;
    }
    PublishView();
}

void FMtoULinkageSession::NoteReferenceRemoved(const FMtoUReferenceRemovedMessage& Message)
{
    if (Message.SessionId != SessionId)
    {
        return;
    }
    Detail = FString::Printf(TEXT("Reference container removed: %s"), *Message.Detail);
    PublishView();
}

void FMtoULinkageSession::NoteReferenceQuery(const FMtoUReferenceQueryMessage& Message)
{
    if (Message.SessionId != SessionId)
    {
        return;
    }
    // The Maya side owns the import; the editor answers the query by offering
    // a handoff of the scope the user selected in Unreal.
    Detail = FString::Printf(TEXT("Reference query %s for the selected scope."), *Message.TransferId);
    PublishView();
}

void FMtoULinkageSession::PumpCameraTargets(double NowSeconds)
{
    if (!Hooks.EvaluateCameraTime)
    {
        return;
    }
    // Bounded cadence: the editor is asked at most once per interval and never
    // more than one target is in flight.
    if (NowSeconds - LastPumpSeconds < CameraPumpIntervalSeconds)
    {
        return;
    }
    LastPumpSeconds = NowSeconds;

    FMtoUCameraTimeTarget Target;
    if (!Hooks.EvaluateCameraTime(Target))
    {
        return;
    }
    if (Target.EvalId.IsEmpty())
    {
        Detail = TEXT("The editor offered a target without an evaluation identity.");
        return;
    }
    if (Target.DisplayRateNumerator <= 0 || Target.DisplayRateDenominator <= 0
        || Target.TickNumerator <= 0 || Target.TickDenominator <= 0)
    {
        Detail = TEXT("The editor offered a target without a display rate or tick resolution.");
        return;
    }

    // The transport, frame and evaluation serials are Unreal's own counters;
    // the editor fills the identity the sequence produced.
    ++TransportSerial;
    ++FrameSerial;
    Target.TransportSerial = TransportSerial;
    Target.FrameSerial = FrameSerial;

    if (Issued.IsSet())
    {
        if (Issued->EvalId == Target.EvalId)
        {
            // A heartbeat of the work already in flight is not a second one.
            return;
        }
        if (IsCutChange(Issued.GetValue(), Target))
        {
            // A cut cancels the target in flight; a late answer is EVAL_STALE.
            Issued.Reset();
        }
    }
    if (Issued.IsSet())
    {
        if (Target.EvalSerial < Issued->EvalSerial)
        {
            return;
        }
        // Only the waiting target is replaceable; the issued one keeps running.
        Desired = Target;
        Detail = FString::Printf(TEXT("Waiting target %s."), *Target.EvalId);
        return;
    }
    if (Target.EvalId == LastAppliedEvalId)
    {
        // The display already holds this exact target; a paused edit produces
        // a new evaluation identity instead of a repeated publication.
        return;
    }
    IssueTarget(Target, NowSeconds);
}

void FMtoULinkageSession::IssueTarget(const FMtoUCameraTimeTarget& Target, double NowSeconds)
{
    Issued = Target;
    IssuedSeconds = NowSeconds;
    Detail = FString::Printf(TEXT("Issued target %s at frame %.3f."), *Target.EvalId, Target.DisplayFrame);
    SendControl(FMtoUProtocol::EncodeCameraTime(
        SessionId, SetRevision, ModeRevision, SpaceRevision, Target));
    PublishView();
}

bool FMtoULinkageSession::IsCutChange(
    const FMtoUCameraTimeTarget& Left, const FMtoUCameraTimeTarget& Right) const
{
    return Left.CutStage != Right.CutStage || Left.CutName != Right.CutName;
}

bool FMtoULinkageSession::BeginTakeover(
    int32 InModeRevision, FString& OutError, FString& OutErrorCode, bool bAwaitPeerConfirmation)
{
    const FMtoULinkagePairing* Pairing = FindPairing(SessionObjectId);
    if (!Pairing)
    {
        OutError = TEXT("Pair this actor's Binding with the Maya object before entering linkage.");
        OutErrorCode = TEXT("PAIRING_REQUIRED");
        return false;
    }
    if (!Pairing->Actor.IsValid() || !IsEligiblePairingActor(*Pairing->Actor))
    {
        OutError = TEXT("The paired target no longer resolves in the Editor world.");
        OutErrorCode = TEXT("TARGET_UNKNOWN");
        return false;
    }

    bTransitioning = true;
    TransitionStartedSeconds = Now();
    FString TakeError;
    FString TakeCode;
    if (Hooks.TakeOverTarget && !Hooks.TakeOverTarget(TakeError, TakeCode))
    {
        FailTransition(
            TakeCode.IsEmpty() ? TEXT("WRITER_NOT_ISOLATABLE") : TakeCode,
            TakeError.IsEmpty() ? TEXT("The target's writers cannot be isolated.") : TakeError);
        OutError = Error;
        OutErrorCode = ErrorCode;
        return false;
    }
    bWritersBorrowed = true;
    Mode = TEXT("linkage");
    Control = TEXT("unreal");
    ModeRevision = InModeRevision;
    Issued.Reset();
    Desired.Reset();
    bConverged = false;
    // An editor request waits for Maya to confirm before the pose gate opens;
    // a `mode` message Maya sent already carries that confirmation.
    bTransitioning = bAwaitPeerConfirmation;
    Detail = bAwaitPeerConfirmation
        ? TEXT("Unreal owns the sequence time; waiting for Maya to confirm.")
        : TEXT("Unreal owns the sequence time; Maya follows.");
    SendControl(FMtoUProtocol::EncodeModeState(SessionId, ModeRevision, Mode, Control, Detail));
    PublishView();
    return true;
}

void FMtoULinkageSession::FailTransition(const FString& Code, const FString& InDetail)
{
    // A failed transition keeps the previous mode and reports the code.
    bTransitioning = false;
    Mode = TEXT("realtime");
    Control = TEXT("maya");
    Issued.Reset();
    Desired.Reset();
    bConverged = false;

    FString Residual;
    if (bWritersBorrowed)
    {
        RestoreBorrowedWriters(Residual);
    }
    // A restore failure is the residual the operator must see; the transition
    // code still names why the mode never changed.
    Error = Residual.IsEmpty()
        ? (InDetail.IsEmpty() ? Code : InDetail)
        : FString::Printf(TEXT("%s; %s"), *InDetail, *Residual);
    ErrorCode = Residual.IsEmpty() ? Code : TEXT("RECOVERY_REFUSED");
    SendControl(FMtoUProtocol::EncodeModeState(SessionId, ModeRevision, Mode, Control, Error));
    PublishView();
}

void FMtoULinkageSession::RestoreBorrowedWriters(FString& OutResidual)
{
    OutResidual.Reset();
    if (!bWritersBorrowed)
    {
        return;
    }
    bWritersBorrowed = false;
    if (Hooks.RestoreTarget)
    {
        FString RestoreError;
        FString RestoreCode;
        if (!Hooks.RestoreTarget(RestoreError, RestoreCode))
        {
            OutResidual = RestoreError.IsEmpty()
                ? (RestoreCode.IsEmpty() ? TEXT("A borrowed writer could not be restored.") : RestoreCode)
                : RestoreError;
        }
    }
}

void FMtoULinkageSession::UpdateConvergence(double NowSeconds)
{
    const bool bConvergedNow = Mode == TEXT("linkage")
        && !bTransitioning
        && !Issued.IsSet()
        && !Desired.IsSet()
        && bLastApplied;
    if (bConvergedNow)
    {
        LastAppliedLagSeconds = 0.0;
        bConverged = true;
        return;
    }
    bConverged = false;
    if (Mode == TEXT("linkage") && Issued.IsSet()
        && NowSeconds - IssuedSeconds > FMtoUProtocol::LinkageConvergenceSeconds)
    {
        // Not converged: the last target and the elapsed time are reported
        // instead of claiming a synchronisation that did not happen.
        Detail = FString::Printf(
            TEXT("Not converged: target %s waiting %.2f s."),
            *Issued->EvalId,
            NowSeconds - IssuedSeconds);
    }
}

void FMtoULinkageSession::SendControl(TArray<uint8>&& Packet)
{
    if (SendReply)
    {
        SendReply(MoveTemp(Packet), true);
    }
}

void FMtoULinkageSession::SendError(const FString& Code, const FString& Message, const FString& Details)
{
    SendControl(FMtoUProtocol::EncodeError(Code, Message, Details));
    Error = Message;
    ErrorCode = Code;
    PublishView();
}

void FMtoULinkageSession::PublishView()
{
    if (Hooks.OnViewChanged)
    {
        Hooks.OnViewChanged(GetView());
    }
}

bool FMtoULinkageSession::ConsumeSessionEndRequest()
{
    const bool bRequested = bSessionEndRequested;
    bSessionEndRequested = false;
    return bRequested;
}

// ---------------------------------------------------------------------------
// Public boundary (MtoULinkage.h). Every function runs on the Game Thread and
// reaches the one active source's session the same way the cached-playback
// actions do, so the editor module never includes runtime privates.
// ---------------------------------------------------------------------------

void MtoUSetLinkageHooks(const FMtoULinkageHooks& Hooks)
{
    GMtoULinkageHooks = Hooks;
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        Source->GetLinkageSession().SetHooks(Hooks);
    }
}

bool MtoUPairLinkageTarget(
    const AMtoULiveLinkActor& Actor, const FString& ObjectId, FString& OutError)
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        FString ErrorCode;
        return Source->GetLinkageSession().PairTarget(Actor, ObjectId, OutError, ErrorCode);
    }
    OutError = TEXT("The MtoU_LiveLink listener is unavailable.");
    return false;
}

void MtoUClearLinkageTarget(const AMtoULiveLinkActor& Actor)
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        Source->GetLinkageSession().ClearTarget(Actor);
    }
}

FMtoULinkageView MtoUGetLinkageView()
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        return Source->GetLinkageSession().GetView();
    }
    return FMtoULinkageView();
}

bool MtoURequestLinkageEnter(int32 ModeRevision, FString& OutError)
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        FString ErrorCode;
        return Source->GetLinkageSession().RequestEnter(ModeRevision, OutError, ErrorCode);
    }
    OutError = TEXT("The MtoU_LiveLink listener is unavailable.");
    return false;
}

bool MtoURequestLinkageLeave(
    const FString& Target, int32 ModeRevision, FString& OutError)
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        FString ErrorCode;
        return Source->GetLinkageSession().RequestLeave(Target, ModeRevision, OutError, ErrorCode);
    }
    OutError = TEXT("The MtoU_LiveLink listener is unavailable.");
    return false;
}

bool MtoUOfferReferenceHandoff(
    const FString& TransferId,
    const FString& Directory,
    const FString& ManifestDigest,
    const TSharedPtr<FJsonObject>& Scope,
    int64 ByteCount,
    int32 ObjectCount,
    int32 TriangleCount,
    FString& OutError)
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        return Source->GetLinkageSession().OfferReferenceHandoff(
            TransferId, Directory, ManifestDigest, Scope, ByteCount, ObjectCount,
            TriangleCount, OutError);
    }
    OutError = TEXT("The MtoU_LiveLink listener is unavailable.");
    return false;
}
