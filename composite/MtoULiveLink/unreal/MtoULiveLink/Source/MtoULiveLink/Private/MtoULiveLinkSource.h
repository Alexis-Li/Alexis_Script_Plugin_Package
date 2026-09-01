#pragma once

#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "ILiveLinkSource.h"
#include "MtoUCacheSession.h"
#include "MtoULiveLinkProtocol.h"

class AMtoULiveLinkActor;
class FRunnableThread;
class ILiveLinkClient;
class FSocket;

/**
 * The one idempotent session-termination boundary shared by the Live Link
 * source and the Binding actor. Any active streaming session ends when its
 * Preview revision is invalidated or the Binding actor's lifetime ends, so a
 * new Preview revision can never receive frames negotiated for an older
 * Character snapshot. Requests made while no session is active are no-ops;
 * a new connection is always required to stream the newer revision.
 */
void MtoURequestStreamingSessionEnd();

struct FMtoUOutgoing
{
    uint64 SessionId = 0;
    TArray<uint8> Packet;
    int32 ExpectedBoneCount = 0;
    int32 ExpectedCurveCount = 0;
    bool bReady = false;
    bool bCloseAfter = false;
};

struct FMtoUPendingInit
{
    uint64 SessionId = 0;
    FMtoUInitMessage Message;
};

struct FMtoUPendingFrame
{
    uint64 SessionId = 0;
    FMtoUFrameMessage Message;
};

class FMtoULiveLinkSource final : public ILiveLinkSource,
                                  public FRunnable,
                                  public TSharedFromThis<FMtoULiveLinkSource>
{
public:
    explicit FMtoULiveLinkSource(uint16 InPort = 54321);
    virtual ~FMtoULiveLinkSource() override;

    virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;
    virtual void InitializeSettings(ULiveLinkSourceSettings* Settings) override;
    virtual void Update() override;
    virtual bool IsSourceStillValid() const override;
    virtual bool RequestSourceShutdown() override;
    virtual FText GetSourceType() const override;
    virtual FText GetSourceMachineName() const override;
    virtual FText GetSourceStatus() const override;

    virtual uint32 Run() override;
    virtual void Stop() override;

    bool StartListener();
    void StopListener();

private:
    void HandleInitOnGameThread(FMtoUInitMessage&& Message);
    void HandleCacheCommandsOnGameThread();
    bool DispatchCacheCommandOnGameThread(const FMtoUCacheCommand& Command);
    bool PublishFrameOnGameThread(const FMtoUFrameMessage& Frame);
    void PublishLatestFrameOnGameThread();
    void EnqueueReplyPacketOnGameThread(uint64 SessionId, TArray<uint8> Packet, bool bCloseAfter);
    void EnqueueErrorOnGameThread(
        const FString& Code,
        const FString& Message,
        const FString& Details = FString());
    bool IsCurrentSession(uint64 SessionId) const;
    void SetStatus(const FString& InStatus);

    const uint16 ConfiguredPort;
    TAtomic<bool> bStopRequested{false};
    TAtomic<bool> bSourceValid{true};
    TUniquePtr<FRunnableThread> Thread;

    mutable FCriticalSection StatusMutex;
    FString Status;

    mutable FCriticalSection PendingMutex;
    uint64 CurrentWorkerSession = 0;
    TOptional<FMtoUPendingInit> PendingInit;
    TOptional<FMtoUPendingFrame> PendingFrame;
    TQueue<uint64, EQueueMode::Spsc> DisconnectedSessions;
    TQueue<FMtoUOutgoing, EQueueMode::Spsc> OutgoingReplies;
    TQueue<FMtoUCacheCommand, EQueueMode::Spsc> PendingCacheCommands;

    ILiveLinkClient* Client = nullptr;
    FGuid SourceGuid;
    FLiveLinkSubjectKey SubjectKey;
    uint64 GameThreadSession = 0;
    int32 ExpectedBoneCount = 0;
    int32 ExpectedCurveCount = 0;
    // Authoritative character snapshot revision from the accepted init.
    int32 NegotiatedRevision = 0;
    TArray<FTransform> SourceBindLocalPose;
    TArray<FTransform> TargetRefLocalPose;
    TArray<int32> BoneParents;
    TArray<int32> AcceptedCurveIndices;
    TArray<FName> AcceptedCurveNames;
    TArray<TWeakObjectPtr<AMtoULiveLinkActor>> ParticipatingActors;

    // Game-thread-only transient cache owner scoped to GameThreadSession.
    FMtoUCacheSession CacheSession;
    // Set when playback starts; one completion or performance-failure reply
    // is reported exactly once per replay attempt.
    bool bPlaybackOutcomePending = false;
};
