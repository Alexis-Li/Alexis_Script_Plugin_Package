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

bool IsSetObjectId(const FString& ObjectId)
{
    return ObjectId == FMtoUObjectIds::Character || ObjectId == FMtoUObjectIds::Secondary;
}
}

// The common anchor of a set is one placement: a translation within 0.01 cm
// and a rotation within 0.01 degrees of the primary target's.
constexpr double MtoUAnchorTranslationToleranceCm = 0.01;
constexpr double MtoUAnchorRotationToleranceDeg = 0.01;

bool MtoUSharesAnchor(const FTransform& Primary, const FTransform& Other, FString& OutDifference)
{
    const double Distance = FVector::Distance(Primary.GetLocation(), Other.GetLocation());
    const double Angle = FMath::RadiansToDegrees(
        Primary.GetRotation().AngularDistance(Other.GetRotation()));
    const bool bScale = Primary.GetScale3D().Equals(Other.GetScale3D(), 1.0e-4);
    if (Distance <= MtoUAnchorTranslationToleranceCm
        && Angle <= MtoUAnchorRotationToleranceDeg && bScale)
    {
        return true;
    }
    OutDifference = FString::Printf(
        TEXT("%.3f cm and %.3f degrees from the primary target%s"),
        Distance, Angle, bScale ? TEXT("") : TEXT(", at another scale"));
    return false;
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
    if (ObjectId.IsEmpty() || !IsSetObjectId(ObjectId))
    {
        OutError = FString::Printf(
            TEXT("A linkage pairing names the primary '%s' or the second object '%s'."),
            FMtoUObjectIds::Character, FMtoUObjectIds::Secondary);
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

FString FMtoULinkageSession::PairedObjectOf(const AMtoULiveLinkActor& Actor) const
{
    for (const FMtoULinkagePairing& Pairing : GMtoULinkagePairings)
    {
        if (Pairing.Actor.Get() == &Actor)
        {
            return Pairing.ObjectId;
        }
    }
    return FString();
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
    // The primary character's pairing is what the panel's linkage actions
    // need, before and after a session negotiates.
    const FMtoULinkagePairing* Pairing = FindPairing(
        SessionObjectId.IsEmpty() ? FString(FMtoUObjectIds::Character) : SessionObjectId);
    View.bPaired = Pairing != nullptr;
    if (Pairing)
    {
        View.PairedObjectId = Pairing->ObjectId;
        View.TargetPath = Pairing->BindingPath;
        View.TargetKind = TEXT("binding_actor");
    }
    // The objects the panel lists: the negotiated set while connected, plus
    // any paired object the set does not hold yet.
    for (const TCHAR* ObjectId : {FMtoUObjectIds::Character, FMtoUObjectIds::Secondary})
    {
        const FMtoULinkagePairing* ObjectPairing = FindPairing(ObjectId);
        const bool bNegotiated = SetObjectIds.Contains(ObjectId);
        if (!ObjectPairing && !bNegotiated)
        {
            continue;
        }
        FMtoULinkageObjectView& Object = View.Objects.AddDefaulted_GetRef();
        Object.ObjectId = ObjectId;
        Object.bPaired = ObjectPairing != nullptr;
        Object.TargetPath = ObjectPairing ? ObjectPairing->BindingPath : FString();
        Object.bNegotiated = bNegotiated;
    }
    View.bRemovingObject = !PendingRemovalObjectId.IsEmpty();
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
    View.TargetOverdueSeconds = Mode == TEXT("linkage") && Issued.IsSet() ? OverdueWaitSeconds : 0.0;
    View.bTransitioning = bTransitioning;
    View.bTakeoverHeld = bWritersBorrowed;
    View.Detail = Detail;
    View.bWritersRestored = bWritersRestored;
    View.RecoveryResidual = RecoveryResidual;
    View.RefusedGroupCount = RefusedGroupCount;
    View.RefusedCode = RefusedCode;
    View.RefusedMessage = RefusedMessage;
    View.Error = Error;
    View.ErrorCode = ErrorCode;
    return View;
}

void FMtoULinkageSession::BeginSession(int32 InSessionId)
{
    SessionId = InSessionId;
    ResetScheduleStats();
    bIssueOpen = false;
    IssuedRecord = INDEX_NONE;
    SetRevision = 0;
    SpaceRevision = 1;
    SessionObjectId.Reset();
    SetObjectIds.Reset();
    PendingRemovalObjectId.Reset();
    PendingRemovalSetRevision = 0;
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
    bWritersRestored = false;
    RecoveryResidual.Reset();
    RefusedGroupCount = 0;
    RefusedCode.Reset();
    RefusedMessage.Reset();
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
    SetObjectIds.Reset();
    PendingRemovalObjectId.Reset();
    PendingRemovalSetRevision = 0;
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
    // Every explicitly paired target, primary first; the list stays empty
    // until the user pairs one.
    TArray<FMtoUHelloTarget> Targets;
    for (const TCHAR* ObjectId : {FMtoUObjectIds::Character, FMtoUObjectIds::Secondary})
    {
        if (const FMtoULinkagePairing* Pairing = FindPairing(ObjectId))
        {
            FMtoUHelloTarget& Target = Targets.AddDefaulted_GetRef();
            Target.ObjectId = Pairing->ObjectId;
            Target.Kind = TEXT("binding_actor");
            Target.BindingPath = Pairing->BindingPath;
            Target.Generation = Pairing->Generation;
        }
    }
    const TSharedRef<FJsonObject> Anchor = MakeShared<FJsonObject>();
    SendControl(FMtoUProtocol::EncodeHelloAck(
        SessionId,
        true,
        true,
        true,
        true,
        Targets,
        SpaceRevision,
        Anchor));
    Detail = FString::Printf(TEXT("Greeted %s (Maya %s)."), *Hello.Host, *Hello.MayaVersion);
    PublishView();
}

void FMtoULinkageSession::NegotiateIdentity(
    const FString& ObjectId, int32 InSetRevision, int32 InSpaceRevision)
{
    SessionObjectId = ObjectId;
    SetObjectIds = {ObjectId};
    SetRevision = InSetRevision;
    SpaceRevision = InSpaceRevision;
    PublishView();
}

void FMtoULinkageSession::NegotiateObject(const FString& ObjectId)
{
    SetObjectIds.AddUnique(ObjectId);
    PublishView();
}

void FMtoULinkageSession::NoteRemoveObject(const FMtoURemoveObjectMessage& Message)
{
    if (Message.SessionId != SessionId)
    {
        SendError(
            TEXT("INVALID_MESSAGE"),
            TEXT("A remove_object message named another session."),
            FString::Printf(TEXT("message session %d, current session %d"),
                Message.SessionId, SessionId));
        return;
    }
    if (Message.ObjectId == FMtoUObjectIds::Character)
    {
        SendError(
            TEXT("INVALID_MESSAGE"),
            TEXT("The primary character cannot be removed inside a connection."),
            TEXT("Removing the 'character' object ends the connection instead."));
        return;
    }
    if (!SetObjectIds.Contains(Message.ObjectId))
    {
        SendError(
            TEXT("INVALID_MESSAGE"),
            TEXT("remove_object names an object outside the current set."),
            FString::Printf(TEXT("object '%s' is not part of the current set"), *Message.ObjectId));
        return;
    }
    if (Message.SetRevision != SetRevision + 1)
    {
        SendError(
            TEXT("SET_REVISION_STALE"),
            TEXT("remove_object must name the next set revision."),
            FString::Printf(TEXT("remove_object set revision %d, current %d"),
                Message.SetRevision, SetRevision));
        return;
    }
    PendingRemovalObjectId = Message.ObjectId;
    PendingRemovalSetRevision = Message.SetRevision;
    // The old set's work is closed at once: nothing in flight or waiting for
    // set N is published or confirmed any more.
    Issued.Reset();
    Desired.Reset();
    Staged.Reset();
    bConverged = false;
    Detail = FString::Printf(TEXT("Removing object '%s' from the set."), *Message.ObjectId);
    PublishView();
}

bool FMtoULinkageSession::RequestRemoveObject(FString& OutError)
{
    OutError.Reset();
    if (SessionId == 0 || !SetObjectIds.Contains(FMtoUObjectIds::Secondary))
    {
        OutError = TEXT("The running set has no second object to remove.");
        return false;
    }
    if (!PendingRemovalObjectId.IsEmpty() || bTransitioning)
    {
        OutError = TEXT("A transition of the set or the mode is still in progress.");
        return false;
    }
    FMtoURemoveObjectMessage Request;
    Request.SessionId = SessionId;
    Request.ObjectId = FMtoUObjectIds::Secondary;
    Request.SetRevision = SetRevision + 1;
    Request.Detail = TEXT("removed in the Unreal Details panel");
    NoteRemoveObject(Request);
    return PendingRemovalObjectId == FMtoUObjectIds::Secondary;
}

bool FMtoULinkageSession::ConsumeRemovalRequest(FString& OutObjectId, int32& OutSetRevision)
{
    if (PendingRemovalObjectId.IsEmpty())
    {
        return false;
    }
    OutObjectId = PendingRemovalObjectId;
    OutSetRevision = PendingRemovalSetRevision;
    return true;
}

void FMtoULinkageSession::CompleteObjectRemoval(
    const FString& ObjectId, int32 NewSetRevision, const FString& Residual)
{
    PendingRemovalObjectId.Reset();
    PendingRemovalSetRevision = 0;
    SetObjectIds.Remove(ObjectId);
    SetRevision = NewSetRevision;
    Issued.Reset();
    Desired.Reset();
    Staged.Reset();
    bConverged = false;
    // The display holds the old set's last group; the current target is
    // issued again so the next complete application belongs to the new set.
    LastAppliedEvalId.Reset();
    LastAppliedSampleSerial = 0;
    bLastApplied = false;
    LastPumpSeconds = 0.0;
    const bool bRestored = Residual.IsEmpty();
    Detail = bRestored
        ? FString::Printf(TEXT("Object '%s' was removed and restored; the set continues."), *ObjectId)
        : FString::Printf(TEXT("Object '%s' could not be restored: %s"), *ObjectId, *Residual);
    if (!bRestored)
    {
        Error = Detail;
        ErrorCode = TEXT("RECOVERY_REFUSED");
    }
    PublishView();
}

bool FMtoULinkageSession::ValidateCommonAnchor(FString& OutReason) const
{
    OutReason.Reset();
    const FMtoULinkagePairing* Primary = FindPairing(FMtoUObjectIds::Character);
    if (!Primary || !Primary->Actor.IsValid())
    {
        return true;
    }
    for (const FString& ObjectId : SetObjectIds)
    {
        if (ObjectId == FMtoUObjectIds::Character)
        {
            continue;
        }
        const FMtoULinkagePairing* Other = FindPairing(ObjectId);
        if (!Other || !Other->Actor.IsValid())
        {
            continue;
        }
        FString Difference;
        if (!MtoUSharesAnchor(Primary->Actor->GetActorTransform(),
                Other->Actor->GetActorTransform(), Difference))
        {
            OutReason = FString::Printf(
                TEXT("the target of object '%s' (%s) stands %s; the targets of one set share one placement"),
                *ObjectId, *Other->BindingPath, *Difference);
            return false;
        }
    }
    return true;
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
            // Maya confirmed the transition Unreal started: the pose gate opens,
            // and the echo tells Maya its answer arrived in time.
            bTransitioning = false;
            Detail = TEXT("Maya follows the Unreal sequence time.");
            SendControl(FMtoUProtocol::EncodeModeState(
                SessionId, ModeRevision, Mode, Control, Detail,
                FMtoUProtocol::MakeAnchorObject(TakeoverAnchor)));
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
        if (Message.ModeRevision <= ModeRevision)
        {
            // A Maya transition must be newer than every revision this session
            // used; an older one is a late confirmation of a transition that
            // already failed here, and taking over again would leave Unreal
            // driving a target Maya has stopped answering. The statement tells
            // Maya to roll back.
            SendControl(FMtoUProtocol::EncodeModeState(
                SessionId, ModeRevision, Mode, Control,
                FString::Printf(TEXT("Mode revision %d is not newer than the current %d."),
                    Message.ModeRevision, ModeRevision)));
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
        // Linkage already left (Unreal's own 返回实时 or 结束预览, or a refused
        // transition): the exit is idempotent. 结束预览 hands back any writer
        // still borrowed; both targets adopt Maya's newer revision and confirm
        // the current mode instead of failing an exit that already happened.
        FString Residual;
        if (Message.Target == TEXT("end"))
        {
            RestoreBorrowedWriters(Residual);
        }
        ModeRevision = FMath::Max(ModeRevision, Message.ModeRevision);
        Error = Residual.IsEmpty() ? FString() : Residual;
        ErrorCode = Residual.IsEmpty() ? FString() : TEXT("RECOVERY_REFUSED");
        Detail = !Residual.IsEmpty()
            ? Residual
            : Message.Target == TEXT("end")
                ? TEXT("The preview ended and the original writers were restored.")
                : TEXT("Maya restored its own state; Unreal already follows Maya's time.");
        SendControl(FMtoUProtocol::EncodeModeState(
            SessionId, ModeRevision, Mode, Control, Detail));
        PublishView();
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
    NoteQueuePeaks();
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
            // the previous revision and the last complete display stays. Maya
            // counts this details form, and the older-revision one below, as
            // scheduling instead of a conflict dialog (linkage.py).
            SendError(
                TEXT("MODE_CONFLICT"),
                TEXT("A group arrived during a time-control transition."),
                FString::Printf(TEXT("target mode revision %d"), Group.ModeRevision),
                ESendErrorKind::Scheduling);
            continue;
        }
        if (Mode != TEXT("linkage"))
        {
            SendError(
                TEXT("LINKAGE_NOT_ACTIVE"),
                TEXT("A group arrived while another host owns time."),
                FString::Printf(TEXT("current mode %s"), *Mode),
                ESendErrorKind::Scheduling);
            continue;
        }
        if (Group.ModeRevision != ModeRevision)
        {
            // A group of a mode Unreal already left was in flight when the
            // transition happened; only a newer revision is a real conflict.
            SendError(
                TEXT("MODE_CONFLICT"),
                TEXT("A group named a different mode revision."),
                FString::Printf(TEXT("group mode revision %d, current %d"), Group.ModeRevision, ModeRevision),
                Group.ModeRevision < ModeRevision ? ESendErrorKind::Scheduling : ESendErrorKind::Failure);
            continue;
        }
        if (Group.SetRevision != SetRevision)
        {
            // A group of the set a removal just closed was in flight; only a
            // newer set is a real conflict.
            SendError(
                TEXT("SET_REVISION_STALE"),
                TEXT("A group named an older object set."),
                FString::Printf(TEXT("group set revision %d, current %d"), Group.SetRevision, SetRevision),
                Group.SetRevision < SetRevision ? ESendErrorKind::Scheduling : ESendErrorKind::Failure);
            continue;
        }
        {
            // The group answers for exactly the negotiated set: every object
            // once, none outside it. A partial group is never applied.
            FString SetMismatch;
            TArray<FString> Named;
            for (const FMtoUPoseGroup& Row : Group.Objects)
            {
                if (Named.Contains(Row.ObjectId))
                {
                    SetMismatch = FString::Printf(
                        TEXT("the group names the duplicate object '%s'"), *Row.ObjectId);
                    break;
                }
                Named.Add(Row.ObjectId);
                if (!SetObjectIds.Contains(Row.ObjectId))
                {
                    SetMismatch = FString::Printf(
                        TEXT("the group names object '%s' outside the current set"), *Row.ObjectId);
                    break;
                }
            }
            for (const FString& ObjectId : SetObjectIds)
            {
                if (SetMismatch.IsEmpty() && !Named.Contains(ObjectId))
                {
                    SetMismatch = FString::Printf(
                        TEXT("the group lacks object '%s' of the current set"), *ObjectId);
                }
            }
            if (!SetMismatch.IsEmpty())
            {
                SendError(
                    TEXT("INVALID_MESSAGE"),
                    TEXT("A group does not carry exactly the negotiated object set."),
                    SetMismatch);
                continue;
            }
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
                    Issued.IsSet() ? *Issued->EvalId : TEXT("none")),
                ESendErrorKind::Scheduling);
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
    LastAppliedReason = Reason;
    if (bApplied)
    {
        // Only a group every display completed is the latest complete
        // application; a refused one leaves the previous display reported.
        LastAppliedEvalId = EvalId;
        LastAppliedSampleSerial = SampleSerial;
        bLastApplied = true;
        LastAppliedDisplayFrame = AppliedDisplayFrame;
        LastAppliedLagSeconds = LagSeconds;
        Detail = FString::Printf(TEXT("Applied %s (sample %d)."), *EvalId, SampleSerial);
        if (Issued.IsSet() && Issued->EvalId == EvalId)
        {
            // The issued work is finished only once its components were read
            // back; a newer waiting target keeps its place. The cadence gate
            // is released so the next target is asked for at once instead of
            // waiting out the interval on a busy game thread.
            EndIssueRecord(TEXT("applied"), NowSeconds);
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
        ++Stats.Failed;
        if (bIssueOpen && Stats.Records.IsValidIndex(IssuedRecord)
            && Issued.IsSet() && Issued->EvalId == EvalId)
        {
            ++Stats.Records[IssuedRecord].FailedApplications;
        }
        Detail = FString::Printf(TEXT("Application failed for %s: %s"), *EvalId, *Reason);
        Error = Reason;
        ErrorCode = ErrorCode.IsEmpty() ? TEXT("LINKAGE_APPLY_FAILED") : ErrorCode;
    }

    SendControl(FMtoUProtocol::EncodeApplied(
        SessionId, ModeRevision, EvalId, SampleSerial, bApplied, AppliedDisplayFrame,
        LagSeconds, Components, Reason));
    PublishView();
}

void FMtoULinkageSession::Tick(double NowSeconds, bool bAskEditorNow)
{
    if (bTransitioning
        && NowSeconds - TransitionStartedSeconds > FMtoUProtocol::LinkageConvergenceSeconds * 2.5)
    {
        // A transition Maya never confirms keeps the previous mode instead of
        // leaving the pose gate closed forever.
        FailTransition(TEXT("MODE_CONFLICT"), TEXT("The time-control transition was not confirmed."));
    }
    if (bWritersBorrowed && Hooks.ValidateTakeover)
    {
        FString LostReason;
        if (!Hooks.ValidateTakeover(LostReason))
        {
            EndForLostTakeover(LostReason);
        }
    }
    if (bWritersBorrowed)
    {
        // The anchor is static: a target moved apart from the others while
        // linkage holds them ends the linkage instead of showing two spaces.
        FString AnchorReason;
        if (!ValidateCommonAnchor(AnchorReason))
        {
            EndForLostTakeover(AnchorReason, TEXT("ANCHOR_MISMATCH"));
        }
    }
    if (Mode == TEXT("linkage") && !bTransitioning)
    {
        PumpCameraTargets(NowSeconds, bAskEditorNow);
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

void FMtoULinkageSession::PumpCameraTargets(double NowSeconds, bool bAskEditorNow)
{
    if (!Hooks.EvaluateCameraTime)
    {
        return;
    }
    // Bounded cadence: the editor is asked at most once per interval and never
    // more than one target is in flight. Published work waiting for its commit
    // asks at once, so a discrete event cancels it before it is confirmed.
    if (!bAskEditorNow && NowSeconds - LastPumpSeconds < CameraPumpIntervalSeconds)
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
        if (IsCutChange(Issued.GetValue(), Target) || Target.bDiscontinuous)
        {
            // A cut, pause, jump or camera edit cancels the target in flight and
            // any waiting one, even when the event names the evaluation already
            // in flight (a pause exactly on it); a late answer is EVAL_STALE and
            // changes nothing, and published work for it is never confirmed.
            Issued.Reset();
            Desired.Reset();
        }
        else if (Issued->EvalId == Target.EvalId)
        {
            // A heartbeat of the work already in flight is not a second one.
            return;
        }
    }
    if (Issued.IsSet())
    {
        if (Target.EvalSerial < Issued->EvalSerial)
        {
            return;
        }
        // Only the waiting target is replaceable; the issued one keeps running.
        if (Desired.IsSet() && Desired->EvalId != Target.EvalId)
        {
            ++Stats.Coalesced;
        }
        Desired = Target;
        NoteQueuePeaks();
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
    ++IssueSerial;
    // A target that was dropped without its group (a pause, cut, jump or
    // exit) ends here at the latest; the tick normally saw it first.
    EndIssueRecord(TEXT("cancelled"), NowSeconds);
    Issued = Target;
    IssuedSeconds = NowSeconds;
    bIssueOpen = true;
    ++Stats.Issued;
    IssuedRecord = INDEX_NONE;
    if (Stats.Records.Num() < FMtoULinkageScheduleStats::MaxRecords)
    {
        FMtoULinkageIssueRecord& Record = Stats.Records.AddDefaulted_GetRef();
        Record.IssueSerial = IssueSerial;
        Record.DisplayFrame = Target.DisplayFrame;
        Record.IssuedSeconds = NowSeconds;
        IssuedRecord = Stats.Records.Num() - 1;
    }
    else
    {
        ++Stats.DroppedRecords;
    }
    NoteQueuePeaks();
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
    const auto Refuse = [&](const TCHAR* Code, const FString& Message)
    {
        // The pairing is the consent to take the target over; its absence is
        // reported on the panel like any other refused transition.
        OutError = Message;
        OutErrorCode = Code;
        Error = OutError;
        ErrorCode = OutErrorCode;
        PublishView();
        return false;
    };
    if (!PendingRemovalObjectId.IsEmpty())
    {
        return Refuse(TEXT("MODE_CONFLICT"),
            TEXT("The second object is being removed; enter linkage once the set is confirmed."));
    }
    // Every object of the negotiated set (the primary alone before a session
    // negotiated) must be paired with a placed target: the pairing is the
    // consent to take each target over.
    TArray<FString> Objects = SetObjectIds;
    if (Objects.IsEmpty())
    {
        Objects.Add(SessionObjectId.IsEmpty() ? FString(FMtoUObjectIds::Character) : SessionObjectId);
    }
    for (const FString& ObjectId : Objects)
    {
        const FMtoULinkagePairing* Pairing = FindPairing(ObjectId);
        if (!Pairing)
        {
            return Refuse(TEXT("PAIRING_REQUIRED"), ObjectId == FMtoUObjectIds::Character
                ? FString(TEXT("Pair this actor's Binding with the Maya object before entering linkage."))
                : FString::Printf(
                    TEXT("Pair a second Binding actor with the Maya object '%s' before entering linkage."),
                    *ObjectId));
        }
        if (!Pairing->Actor.IsValid() || !IsEligiblePairingActor(*Pairing->Actor))
        {
            return Refuse(TEXT("TARGET_UNKNOWN"), FString::Printf(
                TEXT("The paired target of object '%s' no longer resolves in the Editor world."),
                *ObjectId));
        }
    }
    {
        FString AnchorReason;
        if (!ValidateCommonAnchor(AnchorReason))
        {
            return Refuse(TEXT("ANCHOR_MISMATCH"), FString::Printf(
                TEXT("The targets do not share one placement: %s."), *AnchorReason));
        }
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
    bWritersRestored = false;
    TakeoverAnchor = FTransform::Identity;
    if (Hooks.GetAnchor)
    {
        Hooks.GetAnchor(TakeoverAnchor);
    }
    RecoveryResidual.Reset();
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
    SendControl(FMtoUProtocol::EncodeModeState(SessionId, ModeRevision, Mode, Control, Detail,
        FMtoUProtocol::MakeAnchorObject(TakeoverAnchor)));
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

void FMtoULinkageSession::EndForLostTakeover(const FString& Reason, const TCHAR* Code)
{
    if (Mode == TEXT("linkage"))
    {
        // A new revision: Maya follows it as Unreal leaving linkage.
        ++ModeRevision;
    }
    Issued.Reset();
    Desired.Reset();
    Staged.Reset();
    bTransitioning = false;
    bConverged = false;
    // The last synchronisation belonged to a sequence that is gone; showing it
    // as current would claim a display nobody evaluates.
    LastAppliedEvalId.Reset();
    LastAppliedSampleSerial = 0;
    bLastApplied = false;
    LastAppliedReason.Reset();
    LastAppliedDisplayFrame = 0.0;
    LastAppliedLagSeconds = 0.0;

    FString Residual;
    RestoreBorrowedWriters(Residual);
    Mode = TEXT("realtime");
    Control = TEXT("maya");
    const FString Lost = Reason.IsEmpty()
        ? FString(TEXT("the takeover's sequence or target went away"))
        : Reason;
    Detail = FString::Printf(TEXT("Linkage ended: %s."), *Lost);
    Error = Residual.IsEmpty() ? Detail : FString::Printf(TEXT("%s %s"), *Detail, *Residual);
    ErrorCode = Residual.IsEmpty() ? Code : TEXT("RECOVERY_REFUSED");
    SendControl(FMtoUProtocol::EncodeModeState(
        SessionId, ModeRevision, Mode, Control, Error));
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
    bWritersRestored = OutResidual.IsEmpty();
    RecoveryResidual = OutResidual;
}

void FMtoULinkageSession::EndIssueRecord(const TCHAR* Outcome, double NowSeconds)
{
    if (!bIssueOpen)
    {
        return;
    }
    bIssueOpen = false;
    const FString Ended(Outcome);
    if (Ended == TEXT("applied"))
    {
        ++Stats.Applied;
    }
    else
    {
        ++Stats.Cancelled;
    }
    if (Stats.Records.IsValidIndex(IssuedRecord))
    {
        FMtoULinkageIssueRecord& Record = Stats.Records[IssuedRecord];
        Record.Outcome = Ended;
        Record.AppliedSeconds = Ended == TEXT("applied") ? NowSeconds : 0.0;
    }
    IssuedRecord = INDEX_NONE;
}

void FMtoULinkageSession::NoteQueuePeaks()
{
    Stats.InFlightPeak = FMath::Max(Stats.InFlightPeak, Issued.IsSet() ? 1 : 0);
    Stats.PendingPeak = FMath::Max(Stats.PendingPeak, Desired.IsSet() ? 1 : 0);
    Stats.StagedPeak = FMath::Max(Stats.StagedPeak, Staged.Num());
}

void FMtoULinkageSession::UpdateConvergence(double NowSeconds)
{
    if (bIssueOpen && !Issued.IsSet())
    {
        // The issued target was dropped without its group: a pause, cut,
        // jump or exit cancelled it.
        EndIssueRecord(TEXT("cancelled"), NowSeconds);
    }
    const bool bConvergedNow = Mode == TEXT("linkage")
        && !bTransitioning
        && !Issued.IsSet()
        && !Desired.IsSet()
        && bLastApplied;
    OverdueWaitSeconds = 0.0;
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
        OverdueWaitSeconds = NowSeconds - IssuedSeconds;
        if (Stats.Records.IsValidIndex(IssuedRecord) && !Stats.Records[IssuedRecord].bOverdue)
        {
            Stats.Records[IssuedRecord].bOverdue = true;
            ++Stats.Overdue;
        }
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

void FMtoULinkageSession::SendError(
    const FString& Code, const FString& Message, const FString& Details, ESendErrorKind Kind)
{
    SendControl(FMtoUProtocol::EncodeError(Code, Message, Details));
    if (Kind == ESendErrorKind::Scheduling)
    {
        // Normal while scrubbing or switching mode: the peer hears the refusal,
        // the panel only counts it.
        ++RefusedGroupCount;
        RefusedCode = Code;
        RefusedMessage = Message;
    }
    else
    {
        Error = Message;
        ErrorCode = Code;
    }
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
        if (!Source->GetLinkageSession().PairTarget(Actor, ObjectId, OutError, ErrorCode))
        {
            return false;
        }
        Source->NotePairingChangedOnGameThread();
        return true;
    }
    OutError = TEXT("The MtoU_LiveLink listener is unavailable.");
    return false;
}

void MtoUClearLinkageTarget(const AMtoULiveLinkActor& Actor)
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        Source->GetLinkageSession().ClearTarget(Actor);
        Source->NotePairingChangedOnGameThread();
    }
}

FString MtoUGetLinkagePairedObject(const AMtoULiveLinkActor& Actor)
{
    for (const FMtoULinkagePairing& Pairing : GMtoULinkagePairings)
    {
        if (Pairing.Actor.Get() == &Actor)
        {
            return Pairing.ObjectId;
        }
    }
    return FString();
}

AMtoULiveLinkActor* MtoUFindLinkagePairedActor(const FString& ObjectId)
{
    // While a session streams the object, its target is the actor it
    // negotiated, so a takeover never follows a pairing the stream ignores.
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        if (AMtoULiveLinkActor* Negotiated = Source->FindNegotiatedActorOnGameThread(ObjectId))
        {
            return IsEligiblePairingActor(*Negotiated) ? Negotiated : nullptr;
        }
    }
    for (const FMtoULinkagePairing& Pairing : GMtoULinkagePairings)
    {
        if (Pairing.ObjectId == ObjectId && Pairing.Actor.IsValid()
            && IsEligiblePairingActor(*Pairing.Actor))
        {
            return Pairing.Actor.Get();
        }
    }
    return nullptr;
}

bool MtoURequestRemoveSecondaryObject(FString& OutError)
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        return Source->GetLinkageSession().RequestRemoveObject(OutError);
    }
    OutError = TEXT("The MtoU_LiveLink listener is unavailable.");
    return false;
}

FMtoULinkageView MtoUGetLinkageView()
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        return Source->GetLinkageSession().GetView();
    }
    return FMtoULinkageView();
}

FMtoULinkageScheduleStats MtoUGetLinkageScheduleStats()
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        FMtoULinkageScheduleStats Stats = Source->GetLinkageSession().GetScheduleStats();
        Source->GetControlQueuePeaks(Stats.OutgoingControlPeak, Stats.IncomingControlPeak);
        return Stats;
    }
    return FMtoULinkageScheduleStats();
}

void MtoUResetLinkageScheduleStats()
{
    if (const TSharedPtr<FMtoULiveLinkSource> Source = MtoUGetActiveLinkageSource().Pin())
    {
        Source->GetLinkageSession().ResetScheduleStats();
        Source->ResetControlQueuePeaks();
    }
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
