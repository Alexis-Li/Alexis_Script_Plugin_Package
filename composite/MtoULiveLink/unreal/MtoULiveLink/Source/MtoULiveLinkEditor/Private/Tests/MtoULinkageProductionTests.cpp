#if WITH_DEV_AUTOMATION_TESTS
#include "MtoULinkage.h"
#include "MtoULiveLinkSource.h"
#include "MtoUSceneRefExport.h"
#include "MtoUGuiAcceptance.h"
#include "MtoULinkageEditor.h"
#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"
#include "MtoUCharacterComposition.h"
#include "MtoULiveLinkEditorTestFixtures.h"

#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "CineCameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicMesh/DynamicBoneAttribute.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "SequencerSettings.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "LiveLinkInstance.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UniversalObjectLocatorResolveParams.h"

/**
 * Opt-in production-sequence linkage acceptance.
 *
 * The supplied project owns every asset. The test loads its level without
 * saving it, linkage-follows /Temp copies of the root sequence and of the shot
 * that binds the character, rebinds the character's binding in that copy to a
 * transient Binding Actor placed at the fixture's anchor, and starts a real
 * Maya GUI through the tool's public entry. The editor ticks naturally: the
 * runtime source, the Live Link client, the Sequencer and the components are
 * never pumped by the test, and the takeover runs through the editor module's
 * production hooks. Each scenario enters linkage, checks the muted writers,
 * and leaves through one exit: the two panel exits, closing the sequence,
 * switching it, destroying the target and closing the world.
 *
 * With a `secondary` block the fixture names a second binding of the same
 * sequence (a possessable, or a spawnable the copy turns into one) and its
 * Skeletal Mesh. A second Binding Actor stands at the same anchor, Maya sets
 * its object through the pairing window's action, and every check covers both
 * objects: takeover per target, the world/screen/rotation/Morph oracle of each,
 * playback of the whole set, and the exits. The set scenarios refuse a moved
 * second target, refuse Maya's main root as the second object, refuse a group
 * without the second object, hold the whole set while one object's subject is
 * delayed, roll the whole set back when a second-object display fails after
 * the group was staged (both through the runtime's automation apply probe),
 * remove the second object from either host while the main character keeps
 * linkage, and destroy the second target.
 */
namespace
{
using MtoUGuiAcceptance::ReadJsonFile;
using MtoUGuiAcceptance::WriteJsonFile;

/** The second object's id in the v11 object set. */
const TCHAR* const SecondaryObjectId = TEXT("secondary");

TArray<TSharedPtr<FJsonValue>> NumberArray(std::initializer_list<double> Values)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (double Value : Values)
    {
        Out.Add(MakeShared<FJsonValueNumber>(Value));
    }
    return Out;
}

FVector ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    const TArray<TSharedPtr<FJsonValue>>& A = Object->GetArrayField(Field);
    return FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber());
}

/** The open Sequencer of one root sequence. */
TSharedPtr<ISequencer> OpenSequencerOf(ULevelSequence* Sequence)
{
    UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    for (IAssetEditorInstance* Instance : Editors->FindEditorsForAsset(Sequence))
    {
        if (Instance && Instance->GetEditorName() == FName(TEXT("LevelSequenceEditor")))
        {
            return static_cast<ILevelSequenceEditorToolkit*>(Instance)->GetSequencer();
        }
    }
    return nullptr;
}

/**
 * One Maya joint's world rotation in Unreal axes, placed by the anchor's
 * rotation. Maya (x, y, z) is Unreal (x, z, y), so Unreal's X, Y and Z axes are
 * Maya's X, Z and Y axes with their components swizzled the same way. A bone's
 * own local axes may differ from the joint's by a fixed rotation, so compare
 * rotation changes between frames rather than these values themselves.
 */
bool MayaWorldRotation(const TSharedPtr<FJsonObject>& Joint, const FQuat& Anchor, FQuat& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
    if (!Joint->TryGetArrayField(TEXT("axes"), A) || A->Num() != 9)
    {
        return false;
    }
    const auto Axis = [A](int32 Row)
    {
        return FVector((*A)[Row * 3]->AsNumber(), (*A)[Row * 3 + 2]->AsNumber(), (*A)[Row * 3 + 1]->AsNumber());
    };
    FMatrix Matrix = FMatrix::Identity;
    const FVector X = Axis(0), Y = Axis(2), Z = Axis(1);
    Matrix.SetAxes(&X, &Y, &Z);
    Out = Anchor * FQuat(Matrix);
    return true;
}

/**
 * The Binding a fixture object names: a transient copy of its `binding` asset,
 * or a transient Binding of its `skeletal_mesh` and optional `parts`.
 */
UMtoULiveLinkBinding* MakeFixtureBinding(const TSharedPtr<FJsonObject>& Spec, FString& OutError)
{
    FString Path;
    if (Spec->TryGetStringField(TEXT("binding"), Path))
    {
        UMtoULiveLinkBinding* Asset = LoadObject<UMtoULiveLinkBinding>(nullptr, *Path);
        if (!Asset)
        {
            OutError = TEXT("Binding did not load: ") + Path;
            return nullptr;
        }
        return DuplicateObject<UMtoULiveLinkBinding>(Asset, GetTransientPackage());
    }
    UMtoULiveLinkBinding* Binding = NewObject<UMtoULiveLinkBinding>(GetTransientPackage());
    Binding->SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *Spec->GetStringField(TEXT("skeletal_mesh")));
    if (!Binding->SkeletalMesh)
    {
        OutError = TEXT("Skeletal Mesh did not load: ") + Spec->GetStringField(TEXT("skeletal_mesh"));
        return nullptr;
    }
    const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
    if (Spec->TryGetArrayField(TEXT("parts"), Parts))
    {
        for (const TSharedPtr<FJsonValue>& Part : *Parts)
        {
            FMtoUCharacterPart& Entry = Binding->AdditionalParts.AddDefaulted_GetRef();
            Entry.SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *Part->AsString());
            Entry.PartName = FPaths::GetBaseFilename(Part->AsString());
            Entry.bEnabled = true;
            if (!Entry.SkeletalMesh)
            {
                OutError = TEXT("Part did not load: ") + Part->AsString();
                return nullptr;
            }
        }
    }
    Binding->EnsureCharacterPartIds();
    return Binding;
}

/**
 * The possessable of one named binding in a sequence copy. A spawnable of that
 * name becomes a possessable with the same tracks; its spawn track goes, as
 * the placed Binding Actor exists before the sequence plays.
 */
FGuid PossessableOfBinding(UMovieScene* MovieScene, const FString& Name, FString& OutLog)
{
    FGuid Guid;
    for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
    {
        if (MovieScene->GetPossessable(Index).GetName() == Name)
        {
            Guid = MovieScene->GetPossessable(Index).GetGuid();
        }
    }
    if (!Guid.IsValid())
    {
        for (int32 Index = 0; Index < MovieScene->GetSpawnableCount(); ++Index)
        {
            const FMovieSceneSpawnable& Spawnable = MovieScene->GetSpawnable(Index);
            if (Spawnable.GetName() == Name)
            {
                const FGuid Old = Spawnable.GetGuid();
                Guid = MovieScene->AddPossessable(Name, AMtoULiveLinkActor::StaticClass());
                MovieScene->MoveBindingContents(Old, Guid);
                MovieScene->RemoveSpawnable(Old);
                OutLog += FString::Printf(TEXT("spawnable '%s' became a possessable; "), *Name);
                break;
            }
        }
    }
    if (const FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid))
    {
        TArray<UMovieSceneTrack*> SpawnTracks;
        for (UMovieSceneTrack* Track : Binding->GetTracks())
        {
            if (Cast<UMovieSceneSpawnTrack>(Track))
            {
                SpawnTracks.Add(Track);
            }
        }
        for (UMovieSceneTrack* Track : SpawnTracks)
        {
            MovieScene->RemoveTrack(*Track);
            OutLog += TEXT("spawn track removed; ");
        }
    }
    if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
    {
        Possessable->SetPossessedObjectClass(AMtoULiveLinkActor::StaticClass());
    }
    return Guid;
}

/** A /Temp copy of a sequence asset; the original package is never touched. */
ULevelSequence* TempCopy(ULevelSequence* Original, const FString& Name)
{
    UPackage* Package = CreatePackage(*(TEXT("/Temp/MtoULinkageAcceptance/") + Name));
    ULevelSequence* Copy = DuplicateObject<ULevelSequence>(Original, Package, *Name);
    Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    return Copy;
}

/** Normalized aperture coordinates of a world point in one camera's rendered view. */
bool ProjectThroughCamera(const FMinimalViewInfo& View, const FVector& World, FVector2D& Out)
{
    const FMatrix Rotation = FInverseRotationMatrix(View.Rotation) * FMatrix(
        FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
    const FMatrix ViewMatrix = FTranslationMatrix(-View.Location) * Rotation;
    const FMatrix Projection = View.CalculateProjectionMatrix();
    const FVector4 Clip = (ViewMatrix * Projection).TransformFVector4(FVector4(World, 1.0));
    if (Clip.W <= KINDA_SMALL_NUMBER)
    {
        return false;
    }
    Out = FVector2D(Clip.X / Clip.W, Clip.Y / Clip.W);
    return true;
}

class FProductionLinkageCommand : public IAutomationLatentCommand
{
public:
    FProductionLinkageCommand(FAutomationTestBase* InTest, FString InFixture)
        : Test(InTest), FixturePath(MoveTemp(InFixture)) {}

    ~FProductionLinkageCommand() override
    {
        FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
        Driver.Terminate();
    }

    bool Update() override
    {
        if (Steps.IsEmpty())
        {
            if (!Prepare())
            {
                return true;
            }
        }
        if (Step >= Steps.Num() || bAbort)
        {
            Finish();
            return true;
        }
        const double Now = FPlatformTime::Seconds();
        if (StepStarted == 0.0)
        {
            StepStarted = Now;
        }
        if (Now - StepStarted > Steps[Step].Timeout)
        {
            const FMtoULinkageView TimeoutView = MtoUGetLinkageView();
            const FString Message = FString::Printf(TEXT("Step '%s' timed out; UE view: mode=%s converged=%d target=%s@%.3f applied=%s@%.3f detail=%s error=%s; actor=%s; Maya: %s"),
                *Steps[Step].Name, *TimeoutView.Mode, TimeoutView.bConverged ? 1 : 0,
                *TimeoutView.TargetEvalId.Right(12), TimeoutView.TargetDisplayFrame,
                *TimeoutView.AppliedEvalId.Right(12), TimeoutView.AppliedDisplayFrame, *TimeoutView.Detail.Left(200),
                *MtoUGetLinkageView().Error, Actor.IsValid() ? *Actor->GetConnectionStatus() : TEXT("none"),
                *MayaSummary());
            Test->AddError(Message);
            Log(TEXT("TIMEOUT ") + Message);
            Finish();
            return true;
        }
        // The driver rewrites its result file continuously; a read that lands
        // mid-write keeps the last complete result instead of losing it.
        MayaResult = Driver.Poll();
        if (MayaResult.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* MayaErrors = nullptr;
            if (MayaResult->TryGetArrayField(TEXT("errors"), MayaErrors) && MayaErrors->Num() > ReportedMayaErrors)
            {
                for (int32 Index = ReportedMayaErrors; Index < MayaErrors->Num(); ++Index)
                {
                    Test->AddError(TEXT("Maya: ") + (*MayaErrors)[Index]->AsString());
                }
                ReportedMayaErrors = MayaErrors->Num();
            }
            bool bMayaOk = true;
            FString Phase;
            MayaResult->TryGetStringField(TEXT("phase"), Phase);
            if (MayaResult->TryGetBoolField(TEXT("ok"), bMayaOk) && !bMayaOk && Phase != TEXT("running"))
            {
                // The driver failed before it could follow Unreal.
                Test->AddError(TEXT("The Maya driver stopped in phase ") + Phase);
                Log(TEXT("ABORT the Maya driver stopped in phase ") + Phase);
                Finish();
                return true;
            }
        }
        const FString Status = Actor.IsValid() ? Actor->GetConnectionStatus() : FString(TEXT("no actor"));
        if (Status != LastStatus)
        {
            Log(FString::Printf(TEXT("actor status '%s' -> '%s' during step '%s'"),
                *LastStatus, *Status, *Steps[Step].Name));
            LastStatus = Status;
        }
        if (HasSecondary())
        {
            const FString SecondaryStatus = SecondaryActor.IsValid()
                ? SecondaryActor->GetConnectionStatus() : FString(TEXT("no actor"));
            if (SecondaryStatus != LastSecondaryStatus)
            {
                Log(FString::Printf(TEXT("second actor status '%s' -> '%s' during step '%s'"),
                    *LastSecondaryStatus, *SecondaryStatus, *Steps[Step].Name));
                LastSecondaryStatus = SecondaryStatus;
            }
            // The group Maya sent without the second object must never be shown.
            if (!DroppedEvalId.IsEmpty() && !bDroppedShown
                && MtoUGetLinkageView().AppliedEvalId == DroppedEvalId)
            {
                bDroppedShown = true;
                Test->AddError(TEXT("The group without the second object was reported applied: ") + DroppedEvalId);
            }
        }
        if (!MayaResult.IsValid() && bMayaStarted)
        {
            return false;
        }
        if (Steps[Step].Run())
        {
            Log(FString::Printf(TEXT("step %d '%s' done in %.2f s"), Step, *Steps[Step].Name, Now - StepStarted));
            ++Step;
            StepStarted = 0.0;
        }
        return false;
    }

private:
    struct FStep
    {
        FString Name;
        double Timeout = 60.0;
        TFunction<bool()> Run;
    };

    void Add(const FString& Name, double Timeout, TFunction<bool()> Run)
    {
        Steps.Add({Name, Timeout, MoveTemp(Run)});
    }

    void Log(const FString& Line)
    {
        Test->AddInfo(Line);
        // Progress is visible while the run is still going.
        FFileHelper::SaveStringToFile(Line + LINE_TERMINATOR, *FPaths::Combine(EvidenceDir, TEXT("progress.log")),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
        Evidence->SetStringField(FString::Printf(TEXT("log_%04d"), LogIndex++), Line);
    }

    FString MayaSummary() const
    {
        if (!MayaResult.IsValid())
        {
            return TEXT("no result yet");
        }
        FString Text;
        FJsonSerializer::Serialize(MayaResult.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
        return Text.Left(1500);
    }

    TSharedPtr<FJsonObject> MayaStatus() const
    {
        const TSharedPtr<FJsonObject>* Status = nullptr;
        return MayaResult.IsValid() && MayaResult->TryGetObjectField(TEXT("status"), Status)
            ? *Status : nullptr;
    }

    bool MayaAcked(int32 Id) const
    {
        int32 Ack = -1;
        return MayaResult.IsValid() && MayaResult->TryGetNumberField(TEXT("ack"), Ack) && Ack >= Id;
    }

    int32 SendMaya(const FString& Action, const FString& Target = FString(), const FString& Name = FString())
    {
        TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
        Fields->SetStringField(TEXT("target"), Target);
        Fields->SetStringField(TEXT("name"), Name);
        return Driver.Send(Action, Fields);
    }

    bool HasSecondary() const
    {
        return Fixture.IsValid() && Fixture->HasTypedField<EJson::Object>(TEXT("secondary"));
    }

    TSharedPtr<FJsonObject> SecondarySpec() const
    {
        return Fixture->GetObjectField(TEXT("secondary"));
    }

    /** The oracle joints of one object, as the Maya fixture lists them. */
    TArray<TSharedPtr<FJsonValue>> OracleJointsOf(bool bSecondary) const
    {
        const TSharedPtr<FJsonObject> Maya = Fixture->GetObjectField(TEXT("maya"));
        return bSecondary ? Maya->GetObjectField(TEXT("secondary"))->GetArrayField(TEXT("oracle_joints"))
                          : Maya->GetArrayField(TEXT("oracle_joints"));
    }

    /** Maya's object set says the second object is part of the connection. */
    bool MayaSetHasSecondary() const
    {
        const TSharedPtr<FJsonObject>* Objects = nullptr;
        FString Secondary;
        return MayaResult.IsValid() && MayaResult->TryGetObjectField(TEXT("objects"), Objects)
            && (*Objects)->TryGetStringField(TEXT("secondary"), Secondary) && !Secondary.IsEmpty();
    }

    int32 NegotiatedObjects() const
    {
        int32 Count = 0;
        for (const FMtoULinkageObjectView& Object : MtoUGetLinkageView().Objects)
        {
            Count += Object.bNegotiated ? 1 : 0;
        }
        return Count;
    }

    /** Both actors are connected and both objects are negotiated. */
    bool SetConnected() const
    {
        bool bConnected = false;
        if (!MayaResult.IsValid() || !MayaResult->TryGetBoolField(TEXT("connected"), bConnected) || !bConnected
            || !Actor.IsValid() || !Actor->GetConnectionStatus().StartsWith(TEXT("Connected")))
        {
            return false;
        }
        return !HasSecondary() || (SecondaryActor.IsValid()
            && SecondaryActor->GetConnectionStatus().StartsWith(TEXT("Connected"))
            && MayaSetHasSecondary());
    }

    /** Pairs every placed actor whose pairing a session change cleared. */
    void PairAgain()
    {
        FString Error;
        if (Actor.IsValid() && MtoUGetLinkagePairedObject(*Actor) != Fixture->GetStringField(TEXT("object_id")))
        {
            Test->TestTrue(TEXT("the main Binding Actor pairs again"),
                MtoUPairLinkageTarget(*Actor, Fixture->GetStringField(TEXT("object_id")), Error));
        }
        if (HasSecondary() && SecondaryActor.IsValid()
            && MtoUGetLinkagePairedObject(*SecondaryActor) != SecondaryObjectId)
        {
            Test->TestTrue(TEXT("the second Binding Actor pairs again"),
                MtoUPairLinkageTarget(*SecondaryActor, SecondaryObjectId, Error));
        }
    }

    bool Prepare()
    {
        Fixture = ReadJsonFile(FixturePath);
        if (!Fixture.IsValid())
        {
            Test->AddError(TEXT("Invalid production linkage fixture"));
            return false;
        }
        EvidenceDir = Fixture->GetStringField(TEXT("evidence"));
        IFileManager::Get().MakeDirectory(*EvidenceDir, true);
        Evidence = MakeShared<FJsonObject>();
        CleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda(
            [this](UWorld* World, bool, bool)
            {
                Log(FString::Printf(TEXT("world cleanup: %s (type %d)"),
                    World ? *World->GetPathName() : TEXT("none"), World ? static_cast<int32>(World->WorldType) : -1));
            });

        // ---- level, sequence copies and the rebound target ----------------
        Add(TEXT("load level and build the sequence copies"), 300.0, [this]() { return BuildScene(); });
        if (HasSecondary())
        {
            // A set connects only when every object has its target, so the
            // artist pairs both actors before connecting from Maya.
            Add(TEXT("pair both targets before Maya connects"), 30.0, [this]() { return PairTargets(); });
        }
        Add(TEXT("start Maya through the public entry"), 30.0, [this]() { return StartMaya(); });
        Add(TEXT("Maya connects and the character negotiates"), 600.0, [this]()
        {
            // A refused connection shows Maya's dialog; nothing later can run.
            const TArray<TSharedPtr<FJsonValue>>* Dialogs = nullptr;
            if (MayaResult.IsValid() && MayaResult->TryGetArrayField(TEXT("dialogs"), Dialogs))
            {
                for (const TSharedPtr<FJsonValue>& Dialog : *Dialogs)
                {
                    const TSharedPtr<FJsonObject>* Diagnostic = nullptr;
                    FString Code, Details;
                    if (Dialog->AsObject()->TryGetObjectField(TEXT("diagnostic"), Diagnostic))
                    {
                        (*Diagnostic)->TryGetStringField(TEXT("code"), Code);
                        (*Diagnostic)->TryGetStringField(TEXT("details"), Details);
                    }
                    if (!Code.IsEmpty() && Code != TEXT("BLENDSHAPE_MISMATCH"))
                    {
                        Test->AddError(FString::Printf(TEXT("Maya's connection was refused: %s %s"), *Code, *Details.Left(1500)));
                        bAbort = true;
                        return true;
                    }
                }
            }
            return SetConnected();
        });
        Add(TEXT("pair the target and open the sequence copy"), 60.0, [this]() { return PairAndOpen(); });
        Add(TEXT("Details renders while connected"), 60.0, [this]() { return RenderDetails(TEXT("connected")); });
        Add(TEXT("record the sequence-driven pose before any takeover"), 60.0, [this]()
        {
            if (!SetTimeAndSettle(OracleFrames[0], 0.5) || !RecordWriterPose(BeforePose)
                || (HasSecondary() && !RecordWriterPose(SecondaryBeforePose, true)))
            {
                return false;
            }
            Log(TEXT("sequence-driven actor transform before takeover ")
                + Actor->GetActorTransform().ToHumanReadableString());
            if (HasSecondary())
            {
                Log(TEXT("sequence-driven second actor transform before takeover ")
                    + SecondaryActor->GetActorTransform().ToHumanReadableString());
                Test->TestEqual(TEXT("the connection negotiated both objects"), NegotiatedObjects(), 2);
            }
            return true;
        });
        if (HasSecondary())
        {
            AddSetRefusals();
            AddCompositionSwaps();
        }

        const TArray<TSharedPtr<FJsonValue>>& Scenarios = Fixture->GetArrayField(TEXT("scenarios"));
        for (int32 Index = 0; Index < Scenarios.Num(); ++Index)
        {
            AddScenario(Scenarios[Index]->AsString(), Index);
        }
        if (HasSecondary() && Fixture->HasTypedField<EJson::Object>(TEXT("reference_probe")))
        {
            AddReferenceAlignment();
        }
        if (Fixture->HasTypedField<EJson::Object>(TEXT("lifecycle")))
        {
            Add(TEXT("[lifecycle] after the last exit Maya holds what a disconnected panel holds"), 120.0, [this]()
            {
                // Close the session the last scenario left open, then count.
                if (LifecyclePhase == 0)
                {
                    if (!MayaSessionOpen())
                    {
                        LifecyclePhase = 1;
                    }
                    else if (LifecycleId < 0)
                    {
                        LifecycleId = SendMaya(TEXT("disconnect"));
                    }
                    else if (MayaAcked(LifecycleId))
                    {
                        LifecycleId = -1;
                    }
                    return false;
                }
                if (LifecycleId < 0)
                {
                    LifecycleId = SendMaya(TEXT("resources"));
                    return false;
                }
                if (!MayaAcked(LifecycleId))
                {
                    return false;
                }
                LifecycleId = -1;
                LifecyclePhase = 0;
                LifecycleLastCensus = MayaResult->GetObjectField(TEXT("resources"));
                CompareCensus(TEXT("after the last exit"), LifecycleDisconnected);
                Evidence->SetArrayField(TEXT("lifecycle"), LifecycleSamples);
                return true;
            });
        }
        Add(TEXT("quit Maya"), 120.0, [this]()
        {
            if (QuitId < 0)
            {
                QuitId = SendMaya(TEXT("quit"));
            }
            return !Driver.IsRunning();
        });
        return true;
    }

    // ------------------------------------------------------------------ scene

    bool BuildScene()
    {
        const FString Level = Fixture->GetStringField(TEXT("level"));
        UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(Level);
        if (!World)
        {
            Test->AddError(TEXT("Could not load ") + Level);
            return false;
        }
        WorldPtr = World;
        MainOriginal = LoadObject<ULevelSequence>(nullptr, *Fixture->GetStringField(TEXT("main_sequence")));
        ShotOriginal = LoadObject<ULevelSequence>(nullptr, *Fixture->GetStringField(TEXT("shot_sequence")));
        SwitchSequence = LoadObject<ULevelSequence>(nullptr, *Fixture->GetStringField(TEXT("switch_sequence")));
        FString BindingError;
        Binding.Reset(MakeFixtureBinding(Fixture, BindingError));
        if (HasSecondary() && Binding.IsValid())
        {
            SecondaryBinding.Reset(MakeFixtureBinding(SecondarySpec(), BindingError));
        }
        if (!MainOriginal || !ShotOriginal || !SwitchSequence || !Binding.IsValid()
            || (HasSecondary() && !SecondaryBinding.IsValid()))
        {
            Test->AddError(TEXT("Fixture sequence or binding did not load ") + BindingError);
            return false;
        }
        MainCopy.Reset(TempCopy(MainOriginal, TEXT("Main")));
        if (ShotOriginal == MainOriginal)
        {
            // One sequence binds the targets itself; it has no shot to repoint.
            ShotCopy.Reset(MainCopy.Get());
            ProtectedPackages.Add(MainOriginal->GetPackage()->GetName());
            ProtectedPackages.Add(World->GetPackage()->GetName());
            return SpawnTarget() && (!HasSecondary() || SpawnTarget(true));
        }
        ShotCopy.Reset(TempCopy(ShotOriginal, TEXT("Shot")));
        int32 Repointed = 0;
        for (UMovieSceneTrack* Track : MainCopy->GetMovieScene()->GetTracks())
        {
            if (UMovieSceneSubTrack* SubTrack = Cast<UMovieSceneSubTrack>(Track))
            {
                for (UMovieSceneSection* Section : SubTrack->GetAllSections())
                {
                    UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(Section);
                    if (Sub && Sub->GetSequence() == ShotOriginal)
                    {
                        Sub->SetSequence(ShotCopy.Get());
                        ++Repointed;
                    }
                }
            }
        }
        if (!Test->TestEqual(TEXT("the copied root sequence plays the copied shot"), Repointed, 1))
        {
            return false;
        }
        for (const FString& Package : {MainOriginal->GetPackage()->GetName(), ShotOriginal->GetPackage()->GetName()})
        {
            ProtectedPackages.Add(Package);
        }
        ProtectedPackages.Add(World->GetPackage()->GetName());
        return SpawnTarget() && (!HasSecondary() || SpawnTarget(true));
    }

    bool SpawnTarget(bool bSecondary = false)
    {
        UWorld* World = WorldPtr.Get();
        const TSharedPtr<FJsonObject> Anchor = Fixture->GetObjectField(TEXT("anchor"));
        const FVector Location = ReadVector(Anchor, TEXT("location"));
        const FVector Euler = ReadVector(Anchor, TEXT("rotation"));
        AnchorTransform = FTransform(FRotator(Euler.Y, Euler.Z, Euler.X), Location);
        // Placed like an artist's actor; the level is never saved. Both
        // objects' actors stand at the same anchor; each object's own
        // placement comes from its root in Maya.
        FActorSpawnParameters Params;
        AMtoULiveLinkActor* Spawned = World->SpawnActor<AMtoULiveLinkActor>(
            Location, AnchorTransform.Rotator(), Params);
        Spawned->SetActorLabel(bSecondary ? TEXT("MtoU Second (unsaved)") : TEXT("MtoU Linkage Acceptance (unsaved)"));
        Spawned->SetBinding(bSecondary ? SecondaryBinding.Get() : Binding.Get());
        (bSecondary ? SecondaryActor : Actor) = Spawned;

        // The object's binding in the shot copy now possesses the Binding
        // Actor: its existing animation and transform writers drive it.
        const FString TargetName = (bSecondary ? SecondarySpec() : Fixture)->GetStringField(TEXT("target_binding"));
        UMovieScene* MovieScene = ShotCopy->GetMovieScene();
        FString ConversionLog;
        const FGuid Guid = PossessableOfBinding(MovieScene, TargetName, ConversionLog);
        if (!ConversionLog.IsEmpty())
        {
            Log(TEXT("binding '") + TargetName + TEXT("': ") + ConversionLog);
        }
        if (!Test->TestTrue(*FString::Printf(TEXT("the shot copy has the binding '%s'"), *TargetName), Guid.IsValid()))
        {
            return false;
        }
        (bSecondary ? SecondaryGuid : TargetGuid) = Guid;
        if (bSecondary && !bSecondaryAnchorPinned)
        {
            // The second object's transform writer places it where the
            // production scene stood it; in the copy it holds the shared
            // anchor instead, so taking it over leaves both targets at one
            // placement.
            bSecondaryAnchorPinned = true;
            const FVector AnchorEuler(AnchorTransform.Rotator().Roll, AnchorTransform.Rotator().Pitch,
                AnchorTransform.Rotator().Yaw);
            const double Values[6] = {Location.X, Location.Y, Location.Z,
                AnchorEuler.X, AnchorEuler.Y, AnchorEuler.Z};
            int32 Pinned = 0;
            if (const FMovieSceneBinding* TargetBinding = MovieScene->FindBinding(Guid))
            {
                for (UMovieSceneTrack* Track : TargetBinding->GetTracks())
                {
                    for (UMovieSceneSection* Section : Track->GetAllSections())
                    {
                        UMovieScene3DTransformSection* Transform = Cast<UMovieScene3DTransformSection>(Section);
                        if (!Transform)
                        {
                            continue;
                        }
                        TArrayView<FMovieSceneDoubleChannel*> Channels =
                            Transform->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
                        for (int32 Index = 0; Index < 6 && Index < Channels.Num(); ++Index)
                        {
                            Channels[Index]->Reset();
                            Channels[Index]->SetDefault(Values[Index]);
                            ++Pinned;
                        }
                    }
                }
            }
            Log(FString::Printf(TEXT("anchor pinned into the second object's transform writer: %d channels"), Pinned));
        }
        if (!bSecondary && !bAnchorKeyed)
        {
            // The shot's own transform writer places the character at the
            // fixture's anchor, in the copy only: once the takeover mutes that
            // writer, the actor holds this non-identity placement.
            bAnchorKeyed = true;
            const FVector AnchorEuler(AnchorTransform.Rotator().Roll, AnchorTransform.Rotator().Pitch,
                AnchorTransform.Rotator().Yaw);
            const double Offsets[6] = {Location.X, Location.Y, Location.Z,
                AnchorEuler.X, AnchorEuler.Y, AnchorEuler.Z};
            int32 Keyed = 0;
            if (const FMovieSceneBinding* TargetBinding = MovieScene->FindBinding(Guid))
            {
                for (UMovieSceneTrack* Track : TargetBinding->GetTracks())
                {
                    for (UMovieSceneSection* Section : Track->GetAllSections())
                    {
                        UMovieScene3DTransformSection* Transform = Cast<UMovieScene3DTransformSection>(Section);
                        if (!Transform)
                        {
                            continue;
                        }
                        TArrayView<FMovieSceneDoubleChannel*> Channels =
                            Transform->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
                        for (int32 Index = 0; Index < 6 && Index < Channels.Num(); ++Index)
                        {
                            for (FMovieSceneDoubleValue& Value : Channels[Index]->GetData().GetValues())
                            {
                                Value.Value += Offsets[Index];
                                ++Keyed;
                            }
                            const TOptional<double> Default = Channels[Index]->GetDefault();
                            Channels[Index]->SetDefault(Default.Get(0.0) + Offsets[Index]);
                        }
                    }
                }
            }
            Log(FString::Printf(TEXT("anchor keyed into the shot copy's transform writer: %d keys offset"), Keyed));
        }
        ShotCopy->UnbindPossessableObjects(Guid);
        ShotCopy->BindPossessableObject(Guid, *Spawned, World);
        TArray<UObject*, TInlineAllocator<1>> Resolved;
        ShotCopy->LocateBoundObjects(Guid, UE::UniversalObjectLocator::FResolveParams(World),
            TSharedPtr<const UE::MovieScene::FSharedPlaybackState>(), Resolved);
        Log(FString::Printf(TEXT("rebind: %d objects resolve, first %s"), Resolved.Num(),
            Resolved.Num() > 0 && Resolved[0] ? *Resolved[0]->GetPathName() : TEXT("none")));
        return Test->TestTrue(TEXT("the shot copy's character binding resolves to the Binding Actor"),
            Resolved.Num() == 1 && Resolved[0] == Spawned);
    }

    bool StartMaya()
    {
        bMayaStarted = Driver.Start(*Test, Fixture->GetObjectField(TEXT("maya")), EvidenceDir);
        return bMayaStarted;
    }

    /** Pairs each actor that is not already paired with its object. */
    bool PairTargets()
    {
        FString Error;
        if (MtoUGetLinkagePairedObject(*Actor) != Fixture->GetStringField(TEXT("object_id"))
            && !Test->TestTrue(TEXT("the Binding Actor pairs with the Maya object"),
                MtoUPairLinkageTarget(*Actor, Fixture->GetStringField(TEXT("object_id")), Error)))
        {
            Test->AddError(Error);
            return false;
        }
        if (HasSecondary() && MtoUGetLinkagePairedObject(*SecondaryActor) != SecondaryObjectId
            && !Test->TestTrue(TEXT("the second Binding Actor pairs with the second Maya object"),
                MtoUPairLinkageTarget(*SecondaryActor, SecondaryObjectId, Error)))
        {
            Test->AddError(Error);
            return false;
        }
        return true;
    }

    bool PairAndOpen()
    {
        if (!PairTargets())
        {
            return false;
        }
        FMtoULinkageEditor::SetObservedLinkageActor(Actor.Get());
        for (const TSharedPtr<FJsonValue>& Frame : Fixture->GetArrayField(TEXT("oracle_frames")))
        {
            OracleFrames.Add(Frame->AsNumber());
        }
        WriteInventory();
        return OpenMain();
    }

    bool OpenMain()
    {
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(MainCopy.Get());
        SequencerWeak = OpenSequencerOf(MainCopy.Get());
        return Test->TestTrue(TEXT("the sequence copy opens in Sequencer"), SequencerWeak.IsValid());
    }

    /** Bindings, dependencies, writers and components of the production target. */
    void WriteInventory()
    {
        TSharedRef<FJsonObject> Inventory = MakeShared<FJsonObject>();
        Inventory->SetStringField(TEXT("root_sequence"), MainOriginal->GetPathName());
        Inventory->SetStringField(TEXT("shot_sequence"), ShotOriginal->GetPathName());
        Inventory->SetStringField(TEXT("level"), WorldPtr->GetPackage()->GetName());
        TArray<TSharedPtr<FJsonValue>> Dependencies;
        TArray<TSharedPtr<FJsonValue>> Cameras;
        for (UMovieSceneTrack* Track : ShotCopy->GetMovieScene()->GetTracks())
        {
            if (const UMovieSceneSubTrack* Sub = Cast<UMovieSceneSubTrack>(Track))
            {
                for (UMovieSceneSection* Section : Sub->GetAllSections())
                {
                    if (const UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section))
                    {
                        Dependencies.Add(MakeShared<FJsonValueString>(TEXT("sub-sequence ")
                            + (SubSection->GetSequence() ? SubSection->GetSequence()->GetPathName() : TEXT("none"))));
                    }
                }
            }
        }
        if (UMovieSceneTrack* CutTrack = ShotCopy->GetMovieScene()->GetCameraCutTrack())
        {
            for (UMovieSceneSection* Section : CutTrack->GetAllSections())
            {
                const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
                if (!Cut)
                {
                    continue;
                }
                const FMovieScenePossessable* Camera =
                    ShotCopy->GetMovieScene()->FindPossessable(Cut->GetCameraBindingID().GetGuid());
                Cameras.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s [%d, %d)"),
                    Camera ? *Camera->GetName() : TEXT("spawnable or unknown"),
                    Cut->GetInclusiveStartFrame().Value, Cut->GetExclusiveEndFrame().Value)));
            }
        }
        if (const FMovieSceneBinding* TargetBinding = ShotCopy->GetMovieScene()->FindBinding(TargetGuid))
        {
            for (UMovieSceneTrack* Track : TargetBinding->GetTracks())
            {
                for (UMovieSceneSection* Section : Track->GetAllSections())
                {
                    if (const UMovieSceneSkeletalAnimationSection* Anim = Cast<UMovieSceneSkeletalAnimationSection>(Section))
                    {
                        Dependencies.Add(MakeShared<FJsonValueString>(TEXT("animation ")
                            + (Anim->Params.Animation ? Anim->Params.Animation->GetPathName() : TEXT("none"))));
                    }
                }
            }
        }
        Inventory->SetArrayField(TEXT("dependencies"), Dependencies);
        Inventory->SetArrayField(TEXT("camera_cuts"), Cameras);

        FMtoULinkageEditor::FEvaluationSource Source;
        Source.Sequence = MainCopy.Get();
        Source.World = WorldPtr.Get();
        const TArray<UObject*> Targets = FMtoULinkageEditor::ResolveTargetComponents(Actor.Get());
        TArray<TSharedPtr<FJsonValue>> Components;
        Components.Add(MakeShared<FJsonValueString>(TEXT("primary ")
            + Actor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()->GetPathName()));
        for (const TObjectPtr<UMtoUCharacterPartComponent>& Part : Actor->GetCharacterPartComponents())
        {
            if (Part)
            {
                Components.Add(MakeShared<FJsonValueString>(TEXT("part ")
                    + (Part->GetSkeletalMeshAsset() ? Part->GetSkeletalMeshAsset()->GetPathName() : TEXT("none"))));
            }
        }
        Inventory->SetArrayField(TEXT("components"), Components);
        FString Error;
        TArray<TSharedPtr<FJsonValue>> Writers;
        for (const FMtoULinkageEditor::FWriterRecord& Record :
            FMtoULinkageEditor::CollectTargetWriters(Source, Targets, Actor.Get(), Error))
        {
            Writers.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s %s rows=%d"),
                *Record.Kind, *Record.BindingName, Record.Rows.Num())));
            WriterTracks.Add(Record.Track);
        }
        Inventory->SetArrayField(TEXT("writers"), Writers);
        Inventory->SetStringField(TEXT("writer_inventory_error"), Error);
        if (HasSecondary() && SecondaryActor.IsValid())
        {
            // The second target's own writers, taken over and restored on their own.
            TArray<TSharedPtr<FJsonValue>> SecondaryComponents;
            SecondaryComponents.Add(MakeShared<FJsonValueString>(TEXT("primary ")
                + SecondaryActor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset()->GetPathName()));
            Inventory->SetArrayField(TEXT("secondary_components"), SecondaryComponents);
            FString SecondaryError;
            TArray<TSharedPtr<FJsonValue>> SecondaryWriters;
            SecondaryWriterTracks.Reset();
            for (const FMtoULinkageEditor::FWriterRecord& Record : FMtoULinkageEditor::CollectTargetWriters(
                     Source, FMtoULinkageEditor::ResolveTargetComponents(SecondaryActor.Get()),
                     SecondaryActor.Get(), SecondaryError))
            {
                SecondaryWriters.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s %s rows=%d"),
                    *Record.Kind, *Record.BindingName, Record.Rows.Num())));
                SecondaryWriterTracks.Add(Record.Track);
            }
            Inventory->SetArrayField(TEXT("secondary_writers"), SecondaryWriters);
            Inventory->SetStringField(TEXT("secondary_writer_inventory_error"), SecondaryError);
            Test->TestTrue(TEXT("the second target has writers of its own to take over"),
                !SecondaryWriters.IsEmpty() && SecondaryError.IsEmpty());
            for (const TWeakObjectPtr<UMovieSceneTrack>& Track : SecondaryWriterTracks)
            {
                Test->TestFalse(TEXT("no writer belongs to both targets"), WriterTracks.Contains(Track));
            }
            Log(FString::Printf(TEXT("inventory: the second target has %d writers%s"),
                SecondaryWriters.Num(), SecondaryError.IsEmpty() ? TEXT("") : *(TEXT(", error ") + SecondaryError)));
        }
        Inventory->SetArrayField(TEXT("anchor_location"), NumberArray({
            AnchorTransform.GetLocation().X, AnchorTransform.GetLocation().Y, AnchorTransform.GetLocation().Z}));
        Inventory->SetArrayField(TEXT("anchor_rotation_rpy"), NumberArray({
            AnchorTransform.Rotator().Roll, AnchorTransform.Rotator().Pitch, AnchorTransform.Rotator().Yaw}));
        WriteJsonFile(FPaths::Combine(EvidenceDir, TEXT("inventory.json")), Inventory);
        Test->TestTrue(TEXT("the production target has writers to take over"), !Writers.IsEmpty() && Error.IsEmpty());
        Log(FString::Printf(TEXT("inventory: %d writers, %d dependencies, %d camera cuts, %d components"),
            Writers.Num(), Dependencies.Num(), Cameras.Num(), Components.Num()));
    }

    // -------------------------------------------------------------- timeline

    bool SetTimeAndSettle(double DisplayFrame, double SettleSeconds)
    {
        const TSharedPtr<ISequencer> Sequencer = SequencerWeak.Pin();
        if (!Sequencer.IsValid())
        {
            return false;
        }
        if (SettleUntil == 0.0)
        {
            Sequencer->Pause();
            const FFrameTime Tick = FFrameRate::TransformTime(FFrameTime::FromDecimal(DisplayFrame),
                Sequencer->GetRootDisplayRate(), Sequencer->GetRootTickResolution());
            Sequencer->SetGlobalTime(Tick);
            SettleUntil = FPlatformTime::Seconds() + SettleSeconds;
            return false;
        }
        if (FPlatformTime::Seconds() < SettleUntil)
        {
            return false;
        }
        SettleUntil = 0.0;
        return true;
    }

    /**
     * The bones one object's target requires, as negotiation resolves them;
     * a bone of the display's skeleton outside them is never streamed.
     */
    const TSet<FName>& RequiredOf(bool bSecondary)
    {
        const UMtoULiveLinkBinding* Target = bSecondary ? SecondaryBinding.Get() : Binding.Get();
        FString Key = Target && Target->SkeletalMesh ? Target->SkeletalMesh->GetPathName() : FString();
        for (const FMtoUCharacterPart& Part : Target ? Target->AdditionalParts : TArray<FMtoUCharacterPart>())
        {
            Key += TEXT("|") + (Part.bEnabled && Part.SkeletalMesh ? Part.SkeletalMesh->GetPathName() : FString());
        }
        FRequiredBones& Cache = RequiredCache[bSecondary ? 1 : 0];
        if (Cache.Key != Key)
        {
            Cache.Key = Key;
            Cache.Bones = TSet<FName>(RequiredBones(Target));
        }
        return Cache.Bones;
    }

    /** World positions of the oracle bones on one object's primary display. */
    bool RecordWriterPose(TMap<FName, FVector>& Out, bool bSecondary = false)
    {
        Out.Reset();
        const TWeakObjectPtr<AMtoULiveLinkActor>& Owner = bSecondary ? SecondaryActor : Actor;
        if (!Owner.IsValid())
        {
            return false;
        }
        USkeletalMeshComponent* Primary = Owner->GetSkeletalMeshComponent();
        const TSet<FName>& Required = RequiredOf(bSecondary);
        for (const TSharedPtr<FJsonValue>& Name : OracleJointsOf(bSecondary))
        {
            const FName Bone(*Name->AsString());
            if (Primary->GetBoneIndex(Bone) != INDEX_NONE && Required.Contains(Bone))
            {
                Out.Add(Bone, Primary->GetSocketLocation(Bone));
            }
        }
        return Test->TestTrue(bSecondary ? TEXT("the second object's display owns its oracle bones")
                                         : TEXT("the primary display owns the oracle bones"), Out.Num() >= 3);
    }

    /** The largest move of any oracle bone from a recorded pose. */
    double PoseDelta(const TMap<FName, FVector>& Before, bool bSecondary)
    {
        TMap<FName, FVector> Now;
        RecordWriterPose(Now, bSecondary);
        double MaxDelta = 0.0;
        for (const TPair<FName, FVector>& Entry : Before)
        {
            if (const FVector* Current = Now.Find(Entry.Key))
            {
                MaxDelta = FMath::Max(MaxDelta, FVector::Distance(*Current, Entry.Value));
            }
        }
        return MaxDelta;
    }

    bool WritersMuted(bool bExpected, const TCHAR* What)
    {
        if (HasSecondary())
        {
            return WritersMutedIn(WriterTracks, bExpected, *(FString(What) + TEXT(" (main)")))
                & WritersMutedIn(SecondaryWriterTracks, bExpected, *(FString(What) + TEXT(" (second)")));
        }
        return WritersMutedIn(WriterTracks, bExpected, What);
    }

    bool WritersMutedIn(const TArray<TWeakObjectPtr<UMovieSceneTrack>>& Tracks, bool bExpected, const TCHAR* What)
    {
        int32 Live = 0;
        int32 Muted = 0;
        for (const TWeakObjectPtr<UMovieSceneTrack>& Weak : Tracks)
        {
            if (UMovieSceneTrack* Track = Weak.Get())
            {
                bool bTrackMuted = Track->IsLocalEvalDisabled();
                for (int32 Row = 0; Row <= Track->GetMaxRowIndex(); ++Row)
                {
                    bTrackMuted |= Track->IsLocalRowEvalDisabled(Row);
                }
                ++(bTrackMuted ? Muted : Live);
            }
        }
        Log(FString::Printf(TEXT("%s: %d writers muted, %d live"), What, Muted, Live));
        return bExpected ? (Muted == Tracks.Num() && Muted > 0) : Muted == 0;
    }

    // -------------------------------------------------------------- scenarios

    void AddScenario(const FString& Scenario, int32 Index)
    {
        const FString Tag = FString::Printf(TEXT("[%s] "), *Scenario);
        Add(Tag + TEXT("dialog stage"), 30.0, [this, Scenario]()
        {
            if (StageId < 0)
            {
                StageId = SendMaya(TEXT("stage"), FString(), Scenario);
            }
            if (!MayaAcked(StageId))
            {
                return false;
            }
            StageId = -1;
            return true;
        });
        if (Scenario == TEXT("lifecycle"))
        {
            AddLifecycle();
            return;
        }
        if (Scenario == TEXT("lifecycle_check"))
        {
            // After the Unreal exits before it: one more round, the same census.
            Add(TEXT("[lifecycle] after the Unreal exits"), 300.0, [this]()
            {
                return LifecycleCycle(TEXT("cycle")) && CompareCensus(TEXT("after the Unreal exits"));
            });
            return;
        }
        if (Scenario == TEXT("arms"))
        {
            AddArmsReplacement(true);
        }
        Add(Tag + TEXT("enter linkage"), 120.0, [this, Index]()
        {
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (EnterRequested != Index)
            {
                EnterRequested = Index;
                MayaTimeBefore = MayaResult->GetNumberField(TEXT("maya_time"));
                MayaCameraBefore = MayaResult->GetStringField(TEXT("panel_camera"));
                if (Index == 0)
                {
                    // The first entry starts from the Maya panel, the others
                    // from Unreal, so both directions are exercised.
                    SendMaya(TEXT("enter"));
                }
                else
                {
                    FString Error;
                    if (!MtoURequestLinkageEnter(View.Identity.ModeRevision + 1, Error))
                    {
                        Test->AddError(TEXT("Unreal entry refused: ") + Error);
                    }
                }
                return false;
            }
            const TSharedPtr<FJsonObject> Status = MayaStatus();
            bool bMayaActive = false;
            return View.Mode == TEXT("linkage") && !View.bTransitioning && View.bTakeoverHeld
                && Status.IsValid() && Status->TryGetBoolField(TEXT("active"), bMayaActive) && bMayaActive;
        });
        Add(Tag + TEXT("takeover mutes every writer of the target"), 30.0, [this, Tag]()
        {
            const bool bMuted = WritersMuted(true, *(Tag + TEXT("during linkage")));
            Test->TestTrue(*(Tag + TEXT("every target writer is muted")), bMuted);
            // The static anchor is where the paired actor stands while linkage
            // owns it: the spawn placement once its own writers are muted.
            LinkageAnchor = Actor->GetActorTransform();
            Log(FString::Printf(TEXT("%sanchor during linkage %s (placed %s)"), *Tag,
                *LinkageAnchor.ToHumanReadableString(), *AnchorTransform.ToHumanReadableString()));
            Test->TestTrue(*(Tag + TEXT("the muted writers leave the target at its placement")),
                LinkageAnchor.Equals(AnchorTransform, 0.01));
            Test->TestNotNull(*(Tag + TEXT("the primary display evaluates Live Link")),
                Cast<ULiveLinkInstance>(Actor->GetSkeletalMeshComponent()->GetAnimInstance()));
            if (HasSecondary())
            {
                Log(FString::Printf(TEXT("%ssecond target during linkage %s"), *Tag,
                    *SecondaryActor->GetActorTransform().ToHumanReadableString()));
                Test->TestTrue(*(Tag + TEXT("both targets share the anchor during linkage")),
                    SecondaryActor->GetActorTransform().Equals(AnchorTransform, 0.01));
                Test->TestNotNull(*(Tag + TEXT("the second object's display evaluates Live Link")),
                    Cast<ULiveLinkInstance>(SecondaryActor->GetSkeletalMeshComponent()->GetAnimInstance()));
                Test->TestEqual(*(Tag + TEXT("linkage holds both objects")), NegotiatedObjects(), 2);
            }
            return true;
        });
        if (Scenario == TEXT("remove_secondary") || Scenario == TEXT("ue_remove_secondary"))
        {
            AddRemoval(Scenario, Tag);
            return;
        }
        if (Scenario == TEXT("arms"))
        {
            AddArmsOracle();
            AddExit(Scenario, Tag);
            AddArmsReplacement(false);
            return;
        }
        if (Index == 0)
        {
            if (HasSecondary())
            {
                AddSetInjections();
            }
            for (int32 Frame = 0; Frame < Fixture->GetArrayField(TEXT("oracle_frames")).Num(); ++Frame)
            {
                AddOracle(Frame);
            }
            AddPlayback();
            if (Fixture->HasTypedField<EJson::Object>(TEXT("sustained")))
            {
                AddSustained();
            }
            if (HasSecondary())
            {
                Add(TEXT("the pairing window opens during linkage"), 30.0, [this]()
                {
                    return SendOnce(TEXT("objects_window"), TEXT("open"));
                });
            }
            Add(TEXT("panel screenshots during linkage"), 60.0, [this]()
            {
                if (ShotId < 0)
                {
                    ShotId = SendMaya(TEXT("panel_shot"), FString(), TEXT("linkage"));
                }
                if (!MayaAcked(ShotId))
                {
                    return false;
                }
                ShotId = -1;
                return true;
            });
            Add(TEXT("Details renders during linkage"), 60.0, [this]() { return RenderDetails(TEXT("linkage")); });
            if (HasSecondary())
            {
                // Closing and reopening the pairing window changes nothing of the set.
                Add(TEXT("the pairing window closes during linkage"), 30.0, [this]()
                {
                    return SendOnce(TEXT("objects_window"), TEXT("close"));
                });
                Add(TEXT("the pairing window reopens with the same set"), 30.0, [this]()
                {
                    if (!SendOnce(TEXT("objects_window"), TEXT("open")))
                    {
                        return false;
                    }
                    const TSharedPtr<FJsonObject> Status = MayaStatus();
                    bool bActive = false;
                    Test->TestTrue(TEXT("linkage continues while the pairing window closes and reopens"),
                        Status.IsValid() && Status->TryGetBoolField(TEXT("active"), bActive) && bActive
                        && MtoUGetLinkageView().Mode == TEXT("linkage"));
                    Test->TestTrue(TEXT("the reopened pairing window shows the same second object"), MayaSetHasSecondary());
                    return true;
                });
                Add(TEXT("panel screenshots after reopening"), 60.0, [this]()
                {
                    return SendOnce(TEXT("panel_shot"), FString(), TEXT("linkage-reopened"));
                });
            }
        }
        AddExit(Scenario, Tag);
    }

    /**
     * Renders the Binding Actor's Details at each fixture width into PNGs.
     * Only a run with a real RHI can render (-RenderOffscreen, not -NullRHI);
     * otherwise the step only logs that it was skipped. Every text that cannot
     * wrap and is painted narrower than it needs is logged as clipped.
     */
    bool RenderDetails(const FString& State)
    {
        const TArray<TSharedPtr<FJsonValue>>* Widths = nullptr;
        if (Fixture->TryGetArrayField(TEXT("details_widths"), Widths) && Widths->Num() > 0)
        {
            const double Height = Fixture->HasField(TEXT("details_height"))
                ? Fixture->GetNumberField(TEXT("details_height")) : 1600.0;
            MtoUGuiAcceptance::RenderDetails(*Test, *Actor, *Widths, Height,
                EvidenceDir, State, [this](const FString& Line) { Log(Line); });
            if (HasSecondary() && SecondaryActor.IsValid())
            {
                // The second actor's own rows: pairing, set state and 移除副对象.
                MtoUGuiAcceptance::RenderDetails(*Test, *SecondaryActor, *Widths, Height,
                    EvidenceDir, State + TEXT("-secondary"), [this](const FString& Line) { Log(Line); });
            }
        }
        return true;
    }

    void AddOracle(int32 FrameIndex)
    {
        const FString Name = FString::Printf(TEXT("oracle frame %d"), FrameIndex);
        Add(Name + TEXT(": converge"), 120.0, [this, FrameIndex]()
        {
            if (!SetTimeAndSettleOnce(OracleFrames[FrameIndex]))
            {
                return false;
            }
            const FMtoULinkageView View = MtoUGetLinkageView();
            return View.bConverged && !View.AppliedEvalId.IsEmpty()
                && FMath::IsNearlyEqual(View.AppliedDisplayFrame, OracleFrames[FrameIndex], 0.01);
        });
        Add(Name + TEXT(": compare world and screen"), 60.0, [this, FrameIndex]()
        {
            if (SnapshotId < 0)
            {
                SnapshotId = SendMaya(TEXT("snapshot"));
                return false;
            }
            if (!MayaAcked(SnapshotId))
            {
                return false;
            }
            // The bones follow the group at once; the displays refresh their
            // Morph weights with their next evaluation, which a busy editor
            // frame can delay. Wait for that, and fail if it never happens.
            const double MorphDelta = DisplayedMorphDelta();
            const double Now = FPlatformTime::Seconds();
            if (MorphDelta >= Fixture->GetNumberField(TEXT("morph_tolerance")))
            {
                if (MorphWaitStarted == 0.0)
                {
                    MorphWaitStarted = Now;
                }
                if (Now - MorphWaitStarted < 5.0)
                {
                    return false;
                }
            }
            if (MorphWaitStarted != 0.0)
            {
                Log(FString::Printf(TEXT("oracle frame %d: the displays needed %.2f s to show Maya's Morph weights (delta now %.5f)"),
                    FrameIndex, Now - MorphWaitStarted, MorphDelta));
                MorphWaitStarted = 0.0;
            }
            SnapshotId = -1;
            SettledFrame = -1.0;
            CompareOracle(FrameIndex);
            return true;
        });
    }

    /** The largest gap between Maya's snapshot Morph weights and what a display shows. */
    double DisplayedMorphDelta() const
    {
        double Delta = 0.0;
        if (!MayaResult.IsValid() || !MayaResult->HasTypedField<EJson::Object>(TEXT("snapshot")))
        {
            return Delta;
        }
        const TSharedPtr<FJsonObject> Snapshot = MayaResult->GetObjectField(TEXT("snapshot"));
        for (const bool bSecondary : {false, true})
        {
            const AMtoULiveLinkActor* Owner = bSecondary ? SecondaryActor.Get() : Actor.Get();
            const TSharedPtr<FJsonObject> Row = (bSecondary && !HasSecondary()) ? nullptr : SnapshotRow(Snapshot, bSecondary);
            const TSharedPtr<FJsonObject>* Morphs = nullptr;
            if (!Owner || !Row.IsValid() || !Row->TryGetObjectField(TEXT("morphs"), Morphs))
            {
                continue;
            }
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Morph : (*Morphs)->Values)
            {
                for (const double Weight : MtoUGuiAcceptance::DisplayedMorphWeights(*Owner, FName(*Morph.Key)))
                {
                    Delta = FMath::Max(Delta, FMath::Abs(Weight - Morph.Value->AsNumber()));
                }
            }
        }
        return Delta;
    }

    bool SetTimeAndSettleOnce(double Frame)
    {
        if (SettledFrame == Frame)
        {
            return true;
        }
        if (SetTimeAndSettle(Frame, 0.2))
        {
            SettledFrame = Frame;
            return true;
        }
        return false;
    }

    /** One object's oracle against its Maya snapshot row; the counts come back for the assertions. */
    struct FObjectOracle
    {
        double MaxWorld = 0.0, MaxScreen = 0.0, MaxMorph = 0.0, MaxExpectedMorph = 0.0;
        double MaxRotation = 0.0, MinMayaChange = 0.0;
        int32 Compared = 0, ComparedMorphs = 0, ComparedRotations = 0, MissingAxes = 0;
    };

    FObjectOracle CompareObjectOracle(int32 FrameIndex, bool bSecondary, const TSharedPtr<FJsonObject>& Row,
        const FMinimalViewInfo& View, bool bCamera, const TSharedRef<FJsonObject>& Record)
    {
        FObjectOracle Out;
        AMtoULiveLinkActor* Owner = bSecondary ? SecondaryActor.Get() : Actor.Get();
        USkeletalMeshComponent* Primary = Owner->GetSkeletalMeshComponent();
        const TSharedPtr<FJsonObject> Joints = Row->GetObjectField(TEXT("joints"));
        TMap<FName, FOracleRotation>& Rotations =
            (bSecondary ? SecondaryOracleRotations : OracleRotations).FindOrAdd(FrameIndex);
        const TSet<FName>& Required = RequiredOf(bSecondary);
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Joint : Joints->Values)
        {
            const FName Bone(*Joint.Key);
            if (Primary->GetBoneIndex(Bone) == INDEX_NONE || !Required.Contains(Bone))
            {
                continue;
            }
            const TSharedPtr<FJsonObject> Entry = Joint.Value->AsObject();
            const FVector MayaWorld = ReadVector(Entry, TEXT("world"));
            // Common space: Maya (x, y, z) is Unreal (x, z, y), placed by the anchor.
            const FVector Expected = LinkageAnchor.TransformPosition(
                FVector(MayaWorld.X, MayaWorld.Z, MayaWorld.Y));
            const FVector Shown = Primary->GetSocketLocation(Bone);
            Out.MaxWorld = FMath::Max(Out.MaxWorld, FVector::Distance(Expected, Shown));
            FQuat MayaRotation;
            if (MayaWorldRotation(Entry, LinkageAnchor.GetRotation(), MayaRotation))
            {
                Rotations.Add(Bone, {MayaRotation, Primary->GetSocketQuaternion(Bone)});
            }
            else
            {
                ++Out.MissingAxes;
            }
            FVector2D UnrealScreen;
            const TArray<TSharedPtr<FJsonValue>>* MayaScreen = nullptr;
            if (bCamera && ProjectThroughCamera(View, Shown, UnrealScreen)
                && Entry->TryGetArrayField(TEXT("screen"), MayaScreen) && MayaScreen->Num() == 2)
            {
                const FVector2D MayaPoint((*MayaScreen)[0]->AsNumber(), (*MayaScreen)[1]->AsNumber());
                Out.MaxScreen = FMath::Max(Out.MaxScreen, FVector2D::Distance(UnrealScreen, MayaPoint));
                Record->SetArrayField(Joint.Key, NumberArray({Shown.X, Shown.Y, Shown.Z,
                    Expected.X, Expected.Y, Expected.Z, UnrealScreen.X, UnrealScreen.Y, MayaPoint.X, MayaPoint.Y}));
            }
            ++Out.Compared;
        }
        // Morph weights: every reported Maya weight against each display of
        // this object that owns the target, read where the displays'
        // verification reads them.
        const TSharedPtr<FJsonObject>* Morphs = nullptr;
        TSharedRef<FJsonObject> MorphRecord = MakeShared<FJsonObject>();
        if (Row->TryGetObjectField(TEXT("morphs"), Morphs))
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Morph : (*Morphs)->Values)
            {
                const double Expected = Morph.Value->AsNumber();
                TArray<TSharedPtr<FJsonValue>> Shown;
                for (const double Weight : MtoUGuiAcceptance::DisplayedMorphWeights(*Owner, FName(*Morph.Key)))
                {
                    Out.MaxMorph = FMath::Max(Out.MaxMorph, FMath::Abs(Weight - Expected));
                    Shown.Add(MakeShared<FJsonValueNumber>(Weight));
                }
                if (Shown.Num() > 0)
                {
                    ++Out.ComparedMorphs;
                    Out.MaxExpectedMorph = FMath::Max(Out.MaxExpectedMorph, FMath::Abs(Expected));
                    TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetNumberField(TEXT("maya"), Expected);
                    Entry->SetArrayField(TEXT("displays"), Shown);
                    MorphRecord->SetObjectField(Morph.Key, Entry);
                }
            }
        }
        Record->SetObjectField(TEXT("morphs"), MorphRecord);
        Record->SetNumberField(TEXT("max_morph"), Out.MaxMorph);
        // Bone rotations: each display's rotation change since the first
        // oracle frame must be Maya's change, which a pose sampled at another
        // frame would miss. That holds only if Maya's pose differs from every
        // earlier oracle frame's.
        const TMap<FName, FOracleRotation>* First = (bSecondary ? SecondaryOracleRotations : OracleRotations).Find(0);
        if (FrameIndex > 0 && First)
        {
            TSharedRef<FJsonObject> RotationRecord = MakeShared<FJsonObject>();
            for (const TPair<FName, FOracleRotation>& Pair : Rotations)
            {
                if (const FOracleRotation* Base = First->Find(Pair.Key))
                {
                    const FQuat MayaChange = Pair.Value.Maya * Base->Maya.Inverse();
                    const FQuat ShownChange = Pair.Value.Shown * Base->Shown.Inverse();
                    const double Error = FMath::RadiansToDegrees(MayaChange.AngularDistance(ShownChange));
                    Out.MaxRotation = FMath::Max(Out.MaxRotation, Error);
                    ++Out.ComparedRotations;
                    RotationRecord->SetNumberField(Pair.Key.ToString(), Error);
                }
            }
            Record->SetObjectField(TEXT("rotation_error_deg"), RotationRecord);
            Out.MinMayaChange = TNumericLimits<double>::Max();
            for (int32 Earlier = 0; Earlier < FrameIndex; ++Earlier)
            {
                double Change = 0.0;
                if (const TMap<FName, FOracleRotation>* Before =
                        (bSecondary ? SecondaryOracleRotations : OracleRotations).Find(Earlier))
                {
                    for (const TPair<FName, FOracleRotation>& Pair : Rotations)
                    {
                        if (const FOracleRotation* Then = Before->Find(Pair.Key))
                        {
                            Change = FMath::Max(Change, FMath::RadiansToDegrees(Pair.Value.Maya.AngularDistance(Then->Maya)));
                        }
                    }
                }
                Out.MinMayaChange = FMath::Min(Out.MinMayaChange, Change);
            }
        }
        Record->SetNumberField(TEXT("max_rotation_error_deg"), Out.MaxRotation);
        Record->SetNumberField(TEXT("min_maya_rotation_change_deg"), Out.MinMayaChange);
        Record->SetNumberField(TEXT("max_world_cm"), Out.MaxWorld);
        Record->SetNumberField(TEXT("max_screen_ndc"), Out.MaxScreen);
        return Out;
    }

    /** The assertions of one object's oracle. */
    void CheckObjectOracle(int32 FrameIndex, bool bSecondary, const FObjectOracle& Result, const FString& CameraName)
    {
        const FString Who = bSecondary ? TEXT("second object") : TEXT("character");
        Log(FString::Printf(TEXT("oracle frame %.1f via %s, %s: %d bones, max world %.4f cm, max screen %.5f (NDC)"),
            OracleFrames[FrameIndex], *CameraName, *Who, Result.Compared, Result.MaxWorld, Result.MaxScreen));
        Test->TestTrue(*FString::Printf(TEXT("oracle compares the %s's bones"), *Who), Result.Compared >= 3);
        if (Result.MissingAxes > 0)
        {
            Test->AddError(FString::Printf(TEXT("oracle frame %.1f: the Maya driver's snapshot has no world axes for %d bones of the %s; the fixture's maya.driver predates the bone-rotation oracle, so bone rotations are not compared"),
                OracleFrames[FrameIndex], Result.MissingAxes, *Who));
        }
        else if (FrameIndex > 0)
        {
            const double Tolerance = Fixture->HasField(TEXT("rotation_tolerance_deg"))
                ? Fixture->GetNumberField(TEXT("rotation_tolerance_deg")) : 0.5;
            Log(FString::Printf(TEXT("oracle frame %.1f, %s: %d bone rotations since frame %.1f, max error %.4f deg; Maya's pose differs from every earlier oracle frame's by at least %.2f deg"),
                OracleFrames[FrameIndex], *Who, Result.ComparedRotations, OracleFrames[0], Result.MaxRotation,
                Result.MinMayaChange));
            Test->TestTrue(*FString::Printf(TEXT("oracle compares the %s's bone rotations"), *Who),
                Result.ComparedRotations >= 3);
            if (!Test->TestTrue(*FString::Printf(TEXT("the %s's oracle bone rotations differ from every earlier oracle frame's"), *Who),
                Result.MinMayaChange > 10.0 * Tolerance))
            {
                Log(FString::Printf(TEXT("oracle frame %.1f cannot be told from an earlier oracle frame for the %s: their Maya times lie a whole number of key periods (2 * maya.key_step) apart or symmetrically about a key, or the scene's own animation holds still there. Change maya.key_step or oracle_frames in the fixture."),
                    OracleFrames[FrameIndex], *Who));
            }
            Test->TestTrue(*FString::Printf(TEXT("the %s's displays show Maya's bone rotations of the oracle frame"), *Who),
                Result.MaxRotation < Tolerance);
        }
        const TSharedPtr<FJsonObject> Maya = Fixture->GetObjectField(TEXT("maya"));
        const TSharedPtr<FJsonObject> MayaObject = bSecondary ? Maya->GetObjectField(TEXT("secondary")) : Maya;
        const TArray<TSharedPtr<FJsonValue>>* KeyedMorphs = nullptr;
        const bool bKeyed = !bSecondary && MayaObject->TryGetArrayField(TEXT("keyed_morphs"), KeyedMorphs)
            && KeyedMorphs->Num() > 0;
        if (bKeyed || MayaObject->HasField(TEXT("morph_namespace")))
        {
            Log(FString::Printf(TEXT("oracle frame %.1f, %s: %d morphs, max Maya weight %.3f, max delta %.5f"),
                OracleFrames[FrameIndex], *Who, Result.ComparedMorphs, Result.MaxExpectedMorph, Result.MaxMorph));
            if (bKeyed)
            {
                Test->TestTrue(TEXT("every keyed Morph is shown by a display"), Result.ComparedMorphs >= KeyedMorphs->Num());
            }
            else
            {
                Test->TestTrue(*FString::Printf(TEXT("the %s's displays show some of Maya's animated Morphs"), *Who),
                    Result.ComparedMorphs >= 1);
            }
            Test->TestTrue(*FString::Printf(TEXT("the %s's oracle Morph weights are non-zero"), *Who),
                Result.MaxExpectedMorph > 0.05);
            Test->TestTrue(*FString::Printf(TEXT("the %s's displays show Maya's Morph weights"), *Who),
                Result.MaxMorph < Fixture->GetNumberField(TEXT("morph_tolerance")));
        }
        Test->TestTrue(*FString::Printf(TEXT("the displayed %s matches Maya's world pose through the anchor"), *Who),
            Result.MaxWorld < Fixture->GetNumberField(TEXT("world_tolerance_cm")));
        Test->TestTrue(*FString::Printf(TEXT("Unreal's rendered camera shows the %s where Maya's linked camera does"), *Who),
            Result.MaxScreen < Fixture->GetNumberField(TEXT("screen_tolerance_ndc")));
    }

    /** The snapshot row of one object; a driver without object rows reports the character only. */
    TSharedPtr<FJsonObject> SnapshotRow(const TSharedPtr<FJsonObject>& Snapshot, bool bSecondary) const
    {
        const TSharedPtr<FJsonObject>* Objects = nullptr;
        if (Snapshot->TryGetObjectField(TEXT("objects"), Objects))
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if ((*Objects)->TryGetObjectField(bSecondary ? SecondaryObjectId : TEXT("character"), Row))
            {
                return *Row;
            }
        }
        return bSecondary ? nullptr : Snapshot;
    }

    void CompareOracle(int32 FrameIndex, bool bWithSecondary = true)
    {
        const TSharedPtr<FJsonObject> Snapshot = MayaResult->GetObjectField(TEXT("snapshot"));
        const TSharedPtr<ISequencer> Sequencer = SequencerWeak.Pin();
        UCameraComponent* Camera = Sequencer.IsValid() ? Sequencer->GetLastEvaluatedCameraCut().Get() : nullptr;
        FMinimalViewInfo View;
        if (Camera)
        {
            Camera->GetCameraView(0.0f, View);
        }
        const FString CameraName = Camera ? Camera->GetOwner()->GetActorNameOrLabel() : TEXT("no camera");
        TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
        const FObjectOracle Main = CompareObjectOracle(FrameIndex, false, SnapshotRow(Snapshot, false), View,
            Camera != nullptr, Record);
        CheckObjectOracle(FrameIndex, false, Main, CameraName);
        if (HasSecondary() && bWithSecondary)
        {
            const TSharedPtr<FJsonObject> Row = SnapshotRow(Snapshot, true);
            if (Test->TestTrue(TEXT("the Maya snapshot has the second object's row"), Row.IsValid()))
            {
                TSharedRef<FJsonObject> SecondaryRecord = MakeShared<FJsonObject>();
                const FObjectOracle Second = CompareObjectOracle(FrameIndex, true, Row, View, Camera != nullptr,
                    SecondaryRecord);
                CheckObjectOracle(FrameIndex, true, Second, CameraName);
                Record->SetObjectField(TEXT("secondary"), SecondaryRecord);
                // Both characters have bones of the same names; each display
                // must show its own object's pose, never the other's.
                const TSharedPtr<FJsonObject> MainJoints = SnapshotRow(Snapshot, false)->GetObjectField(TEXT("joints"));
                const TSharedPtr<FJsonObject> SecondJoints = Row->GetObjectField(TEXT("joints"));
                int32 Shared = 0;
                double MinApart = TNumericLimits<double>::Max();
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Joint : SecondJoints->Values)
                {
                    if (MainJoints->HasField(Joint.Key))
                    {
                        ++Shared;
                        MinApart = FMath::Min(MinApart, FVector::Distance(
                            ReadVector(Joint.Value->AsObject(), TEXT("world")),
                            ReadVector(MainJoints->GetObjectField(Joint.Key), TEXT("world"))));
                    }
                }
                Log(FString::Printf(TEXT("oracle frame %.1f: %d oracle bones share a name across the two objects; in Maya they stand at least %.2f cm apart"),
                    OracleFrames[FrameIndex], Shared, Shared > 0 ? MinApart : 0.0));
                if (Shared > 0)
                {
                    Test->TestTrue(TEXT("same-name bones of the two objects stand apart, so a crossed stream would fail the oracle"),
                        MinApart > 10.0 * Fixture->GetNumberField(TEXT("world_tolerance_cm")));
                }
            }
        }
        Record->SetNumberField(TEXT("frame"), OracleFrames[FrameIndex]);
        Record->SetArrayField(TEXT("ue_view_location"), NumberArray({View.Location.X, View.Location.Y, View.Location.Z}));
        Record->SetArrayField(TEXT("ue_view_rotation_rpy"), NumberArray({View.Rotation.Roll, View.Rotation.Pitch, View.Rotation.Yaw}));
        Record->SetNumberField(TEXT("ue_view_fov"), View.FOV);
        Record->SetNumberField(TEXT("ue_view_aspect"), View.AspectRatio);
        Record->SetBoolField(TEXT("ue_view_constrain"), View.bConstrainAspectRatio != 0);
        Record->SetArrayField(TEXT("anchor_location"), NumberArray({LinkageAnchor.GetLocation().X,
            LinkageAnchor.GetLocation().Y, LinkageAnchor.GetLocation().Z}));
        Record->SetArrayField(TEXT("anchor_rotation_rpy"), NumberArray({LinkageAnchor.Rotator().Roll,
            LinkageAnchor.Rotator().Pitch, LinkageAnchor.Rotator().Yaw}));
        Record->SetStringField(TEXT("camera"), CameraName);
        Record->SetObjectField(TEXT("maya_snapshot"), Snapshot);
        WriteJsonFile(FPaths::Combine(EvidenceDir, FString::Printf(TEXT("oracle-%s%d.json"),
            bWithSecondary ? TEXT("") : TEXT("after-removal-"), FrameIndex)), Record);
    }

    void AddPlayback()
    {
        Add(TEXT("continuous playback keeps applying while Maya lags"), 120.0, [this]()
        {
            const double Now = FPlatformTime::Seconds();
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (PlayStarted == 0.0)
            {
                PlayStarted = Now;
                SequencerWeak.Pin()->OnPlay(false);
                return false;
            }
            if (View.AppliedEvalId != LastApplied && !View.AppliedEvalId.IsEmpty())
            {
                LastApplied = View.AppliedEvalId;
                AppliedFrames.Add(View.AppliedDisplayFrame);
                MaxLag = FMath::Max(MaxLag, View.AppliedLagSeconds);
            }
            ++PlayTicks;
            bAnchorMoved |= !Actor->GetActorTransform().Equals(LinkageAnchor, 0.01);
            bAnchorMoved |= HasSecondary() && !SecondaryActor->GetActorTransform().Equals(LinkageAnchor, 0.01);
            const double PlaySeconds = Fixture->GetNumberField(TEXT("play_seconds"));
            if (PluginSampleId < 0 && Now - PlayStarted >= 0.25 * PlaySeconds)
            {
                // Maya may still be loading plug-ins; record what is loaded while it plays.
                PluginSampleId = SendMaya(TEXT("sample_plugins"), FString(), TEXT("playback"));
            }
            return Now - PlayStarted > PlaySeconds && PluginSampleId >= 0 && MayaAcked(PluginSampleId);
        });
        Add(TEXT("stopping converges on the last target"), 60.0, [this]()
        {
            if (!bStopped)
            {
                SequencerWeak.Pin()->Pause();
                bStopped = true;
                const double Seconds = FPlatformTime::Seconds() - PlayStarted;
                bool bMonotonic = true;
                for (int32 Index = 1; Index < AppliedFrames.Num(); ++Index)
                {
                    bMonotonic &= AppliedFrames[Index] > AppliedFrames[Index - 1];
                }
                const double Span = AppliedFrames.Num() > 1 ? AppliedFrames.Last() - AppliedFrames[0] : 0.0;
                Log(FString::Printf(TEXT("playback: %.1f s, %d editor ticks, %d complete applications spanning %.1f frames, max lag %.3f s"),
                    Seconds, PlayTicks, AppliedFrames.Num(), Span, MaxLag));
                // Throughput depends on the plug-ins Maya loads; runs compare only with the same preferences.
                const TSharedRef<FJsonObject> Prefs = Driver.PreferencesRecord(TEXT("playback"));
                Log(MtoUGuiAcceptance::DescribeMayaPreferences(Prefs));
                Evidence->SetObjectField(TEXT("maya_prefs"), Prefs);
                Test->TestTrue(TEXT("Maya sampled its loaded plug-ins during playback"),
                    Prefs->GetNumberField(TEXT("loaded_plugins")) >= 0.0);
                Test->TestTrue(TEXT("several complete groups were applied while playing"), AppliedFrames.Num() >= 3);
                Test->TestTrue(TEXT("the applied time keeps advancing"), bMonotonic && Span > 0.0);
                Test->TestTrue(TEXT("Maya answers slower than the editor frame rate"),
                    PlayTicks > AppliedFrames.Num());
                Test->TestFalse(TEXT("the anchor stays static while the sequence plays"), bAnchorMoved);
                return false;
            }
            const FMtoULinkageView View = MtoUGetLinkageView();
            return View.bConverged && !View.AppliedEvalId.IsEmpty();
        });
    }

    // ------------------------------------------------------------ lifecycle

    /**
     * INT-09 in the interactive Maya: after one warm-up cycle the census of
     * what the tool holds (live OpenMaya callbacks counted at registration,
     * scriptJobs, Python threads, open sockets, MtoU nodes and windows) is the
     * baseline; `lifecycle.cycles` rounds of connect, linkage and 结束预览,
     * closing and reopening the panel, replacing the scene and a restore that
     * cannot delete the tool's camera must each return to it.
     */
    void AddLifecycle()
    {
        Add(TEXT("[lifecycle] warm-up cycle and baseline census"), 300.0, [this]()
        {
            if (!LifecycleCycle(TEXT("cycle")))
            {
                return false;
            }
            LifecycleBaseline = LifecycleLastCensus;
            Log(TEXT("[lifecycle] baseline ") + DescribeCensus(LifecycleBaseline));
            return true;
        });
        const int32 Cycles = static_cast<int32>(
            Fixture->GetObjectField(TEXT("lifecycle"))->GetNumberField(TEXT("cycles")));
        for (int32 Index = 1; Index <= Cycles; ++Index)
        {
            Add(FString::Printf(TEXT("[lifecycle] cycle %d"), Index), 300.0, [this, Index]()
            {
                return LifecycleCycle(TEXT("cycle")) && CompareCensus(FString::Printf(TEXT("cycle %d"), Index));
            });
        }
        for (int32 Index = 1; Index <= 3; ++Index)
        {
            Add(FString::Printf(TEXT("[lifecycle] panel closed and reopened %d"), Index), 300.0, [this, Index]()
            {
                return LifecycleCycle(TEXT("panel")) && CompareCensus(FString::Printf(TEXT("panel %d"), Index));
            });
        }
        for (int32 Index = 1; Index <= 2; ++Index)
        {
            Add(FString::Printf(TEXT("[lifecycle] scene replaced and reopened %d"), Index), 600.0, [this, Index]()
            {
                return LifecycleCycle(TEXT("scene")) && CompareCensus(FString::Printf(TEXT("scene %d"), Index));
            });
        }
        Add(TEXT("[lifecycle] a restore that cannot delete the tool camera reports it"), 300.0, [this]()
        {
            return LifecycleCycle(TEXT("recovery")) && CompareCensus(TEXT("after the reported residual was removed"));
        });
    }

    /** Sends one Maya command and waits for its acknowledgement and a condition. */
    bool LifecycleSend(const FString& Action, const FString& Target, TFunctionRef<bool()> Ready)
    {
        if (LifecycleId < 0)
        {
            LifecycleId = SendMaya(Action, Target);
            return false;
        }
        if (!MayaAcked(LifecycleId) || !Ready())
        {
            return false;
        }
        LifecycleId = -1;
        ++LifecyclePhase;
        return false;
    }

    bool MayaLinkageActive() const
    {
        const TSharedPtr<FJsonObject> Status = MayaStatus();
        bool bActive = false;
        return Status.IsValid() && Status->TryGetBoolField(TEXT("active"), bActive) && bActive;
    }

    bool LinkageEntered()
    {
        const FMtoULinkageView View = MtoUGetLinkageView();
        return View.Mode == TEXT("linkage") && !View.bTransitioning && View.bTakeoverHeld && MayaLinkageActive();
    }

    bool LinkageEnded()
    {
        const FMtoULinkageView View = MtoUGetLinkageView();
        return View.Mode == TEXT("realtime") && !View.bTransitioning && !View.bTakeoverHeld && !MayaLinkageActive();
    }

    /**
     * One lifecycle round, as a phase machine; true once its census arrived.
     * `cycle`: 镜头联动, 结束预览, 断开, 连接. `panel`: the same linkage, then the
     * panel closes (the session ends) and `run()` opens it again and connects.
     * `scene`: a new scene replaces the open one, then the fixture scene is
     * opened again and connected. `recovery`: the tool's camera is locked
     * before 结束预览, its residual must be reported, then the person unlocks
     * and deletes it and reconnects.
     */
    bool LifecycleCycle(const FString& Kind)
    {
        const bool bWithLinkage = Kind != TEXT("scene");
        switch (LifecyclePhase)
        {
        case 0:
            if (!bWithLinkage)
            {
                ++LifecyclePhase;
                return false;
            }
            return LifecycleSend(TEXT("enter"), FString(), [this]() { return LinkageEntered(); });
        case 1:
            if (Kind == TEXT("recovery"))
            {
                return LifecycleSend(TEXT("lock_link_camera"), FString(), []() { return true; });
            }
            ++LifecyclePhase;
            return false;
        case 2:
            if (!bWithLinkage)
            {
                ++LifecyclePhase;
                return false;
            }
            return LifecycleSend(TEXT("leave"), TEXT("end"), [this]() { return LinkageEnded(); });
        case 3:
        {
            if (Kind == TEXT("recovery"))
            {
                const TSharedPtr<FJsonObject> Status = MayaStatus();
                const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
                FString Residual;
                if (Status.IsValid() && Status->TryGetArrayField(TEXT("recovery_records"), Records))
                {
                    for (const TSharedPtr<FJsonValue>& Record : *Records)
                    {
                        Residual += Record->AsString() + TEXT("; ");
                    }
                }
                Log(TEXT("[lifecycle] restore residual: ") + Residual);
                Test->TestTrue(TEXT("[lifecycle] the camera the restore could not delete is reported by name"),
                    Residual.Contains(TEXT("MtoU_LinkCamera")));
                Test->TestTrue(TEXT("[lifecycle] Unreal still handed every writer back"),
                    WritersMuted(false, TEXT("[lifecycle] after the failed restore")));
            }
            const FString Action = Kind == TEXT("panel") ? TEXT("close_panel")
                : Kind == TEXT("scene") ? TEXT("new_scene") : TEXT("disconnect");
            return LifecycleSend(Action, FString(), [this]() { return !MayaSessionOpen(); });
        }
        case 4:
            if (Kind == TEXT("recovery"))
            {
                return LifecycleSend(TEXT("lock_link_camera"), TEXT("unlock"), []() { return true; });
            }
            if (Kind == TEXT("cycle"))
            {
                // The disconnected census: what the last exit of a run returns to.
                return LifecycleSend(TEXT("resources"), FString(), [this]()
                {
                    LifecycleDisconnected = MayaResult->GetObjectField(TEXT("resources"));
                    return true;
                });
            }
            ++LifecyclePhase;
            return false;
        case 5:
        {
            const FString Action = Kind == TEXT("panel") ? TEXT("open_panel")
                : Kind == TEXT("scene") ? TEXT("reopen_scene") : TEXT("connect");
            return LifecycleSend(Action, FString(), [this]() { return MayaConnected(); });
        }
        case 6:
            return LifecycleSend(TEXT("resources"), FString(), [this]()
            {
                LifecycleLastCensus = MayaResult->GetObjectField(TEXT("resources"));
                return true;
            });
        default:
            LifecyclePhase = 0;
            LifecycleSamples.Add(MakeShared<FJsonValueObject>(LifecycleLastCensus.ToSharedRef()));
            return true;
        }
    }

    static FString DescribeCensus(const TSharedPtr<FJsonObject>& Census)
    {
        FString Text;
        FJsonSerializer::Serialize(Census.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
        return Text;
    }

    /** The census must equal the baseline in every resource it counts. */
    bool CompareCensus(const FString& What, const TSharedPtr<FJsonObject>& Expected = nullptr)
    {
        const TSharedPtr<FJsonObject> Baseline = Expected.IsValid() ? Expected : LifecycleBaseline;
        const TSharedPtr<FJsonObject> Census = LifecycleLastCensus;
        const FString Tag = TEXT("[lifecycle] ") + What + TEXT(": ");
        for (const TCHAR* Field : {TEXT("callbacks"), TEXT("script_jobs"), TEXT("sockets")})
        {
            Test->TestEqual(*(Tag + Field + TEXT(" return to the baseline")),
                static_cast<int32>(Census->GetNumberField(Field)), static_cast<int32>(Baseline->GetNumberField(Field)));
        }
        for (const TCHAR* Field : {TEXT("threads"), TEXT("nodes"), TEXT("windows")})
        {
            FString Now, Before;
            for (const TSharedPtr<FJsonValue>& Value : Census->GetArrayField(Field))
            {
                Now += Value->AsString() + TEXT(",");
            }
            for (const TSharedPtr<FJsonValue>& Value : Baseline->GetArrayField(Field))
            {
                Before += Value->AsString() + TEXT(",");
            }
            Test->TestEqual(*(Tag + Field + TEXT(" return to the baseline")), Now, Before);
        }
        Log(Tag + DescribeCensus(Census));
        return true;
    }

    // ---------------------------------------------------- sustained playback

    /**
     * The frozen INT-10 measurement: for every delay tier the looping sequence
     * plays for `sustained.seconds` while each published group is held at the
     * set completion barrier for the tier's delay (0 measures the natural
     * delay), then a pause and a jump must each converge within the budget.
     * Every issued target's outcome comes from the runtime's own scheduling
     * record, on the game thread's monotonic clock.
     */
    void AddSustained()
    {
        const TSharedPtr<FJsonObject> Sustained = Fixture->GetObjectField(TEXT("sustained"));
        for (const TSharedPtr<FJsonValue>& Delay : Sustained->GetArrayField(TEXT("delays_ms")))
        {
            const double DelayMs = Delay->AsNumber();
            const double Seconds = Sustained->GetNumberField(TEXT("seconds"));
            Add(FString::Printf(TEXT("[sustained %.0f ms] continuous playback, pause and jump converge"), DelayMs),
                Seconds + 180.0, [this, DelayMs, Seconds]() { return SustainedStep(DelayMs, Seconds); });
        }
        Add(TEXT("[sustained] the loop mode is put back"), 10.0, [this]()
        {
            RestoreLoopMode();
            return true;
        });
    }

    void RestoreLoopMode()
    {
        const TSharedPtr<ISequencer> Sequencer = SequencerWeak.Pin();
        if (bLoopModeChanged && Sequencer.IsValid())
        {
            Sequencer->GetSequencerSettings()->SetLoopMode(LoopModeBefore);
        }
        bLoopModeChanged = false;
    }

    void SampleMemory()
    {
        SustainedUePeak = FMath::Max<uint64>(SustainedUePeak, FPlatformMemory::GetStats().UsedPhysical);
        SustainedMayaPeak = FMath::Max<uint64>(SustainedMayaPeak, Driver.MemoryBytes());
    }

    bool SustainedStep(double DelayMs, double Seconds)
    {
        const double Now = FPlatformTime::Seconds();
        const TSharedPtr<ISequencer> Sequencer = SequencerWeak.Pin();
        if (!Sequencer.IsValid())
        {
            Test->AddError(TEXT("[sustained] the sequence copy is no longer open"));
            return true;
        }
        const FMtoULinkageView View = MtoUGetLinkageView();
        if (SustainedPhase == 0)
        {
            const TSharedPtr<FJsonObject> Sustained = Fixture->GetObjectField(TEXT("sustained"));
            const double StartFrame = Sustained->HasField(TEXT("start_frame"))
                ? Sustained->GetNumberField(TEXT("start_frame")) : OracleFrames[0];
            if (!SetTimeAndSettle(StartFrame, 0.5) || !View.bConverged)
            {
                return false;
            }
            if (!bLoopModeChanged)
            {
                LoopModeBefore = Sequencer->GetSequencerSettings()->GetLoopMode();
                Sequencer->GetSequencerSettings()->SetLoopMode(ESequencerLoopMode::SLM_Loop);
                bLoopModeChanged = true;
            }
            HeldSince.Reset();
            if (DelayMs > 0.0)
            {
                // Injection point: Unreal's set completion barrier. Every
                // published group is held for the tier's delay after the
                // barrier first sees it, under natural ticking.
                MtoUSetLinkageApplyProbe([this, DelayMs](EMtoULinkageApplyStage Stage, const FString&, const FString& Generation)
                {
                    if (Stage != EMtoULinkageApplyStage::Published)
                    {
                        return true;
                    }
                    const double Seen = HeldSince.FindOrAdd(Generation, FPlatformTime::Seconds());
                    return FPlatformTime::Seconds() - Seen >= DelayMs / 1000.0;
                });
            }
            MtoUResetLinkageScheduleStats();
            SustainedUePeak = SustainedMayaPeak = 0;
            SampleMemory();
            SustainedUeStart = SustainedUePeak;
            SustainedMayaStart = SustainedMayaPeak;
            SustainedStart = Now;
            Sequencer->OnPlay(false);
            SustainedPhase = 1;
            return false;
        }
        if (SustainedPhase >= 1)
        {
            NoteSustainedFailure(View);
        }
        if (SustainedPhase == 1)
        {
            SampleMemory();
            if (HeldSince.Num() > 4096)
            {
                HeldSince.Reset();
            }
            if (Now - SustainedStart < Seconds)
            {
                return false;
            }
            Sequencer->Pause();
            SustainedPausedAt = Now;
            SustainedPhase = 2;
            return false;
        }
        if (SustainedPhase == 2)
        {
            if (!View.bConverged)
            {
                if (Now - SustainedPausedAt <= 30.0)
                {
                    return false;
                }
                SustainedPlayRecords = MtoUGetLinkageScheduleStats();
                return FinishSustained(DelayMs, Seconds, View);
            }
            SustainedPauseConvergence = Now - SustainedPausedAt;
            SustainedPlayRecords = MtoUGetLinkageScheduleStats();
            // A jump while paused: a discontinuity that must converge too.
            SustainedJumpFrame = FMath::IsNearlyEqual(View.AppliedDisplayFrame, OracleFrames[0], 0.5)
                ? OracleFrames[0] + 37.0 : OracleFrames[0];
            const FFrameTime Tick = FFrameRate::TransformTime(FFrameTime::FromDecimal(SustainedJumpFrame),
                Sequencer->GetRootDisplayRate(), Sequencer->GetRootTickResolution());
            Sequencer->SetGlobalTime(Tick);
            SustainedJumpedAt = Now;
            SustainedPhase = 3;
            return false;
        }
        if (!View.bConverged || !FMath::IsNearlyEqual(View.AppliedDisplayFrame, SustainedJumpFrame, 0.01))
        {
            return Now - SustainedJumpedAt > 30.0 ? FinishSustained(DelayMs, Seconds, View) : false;
        }
        SustainedJumpConvergence = Now - SustainedJumpedAt;
        return FinishSustained(DelayMs, Seconds, View);
    }

    /** Every refused application during the tier, with the first one's time and reason. */
    void NoteSustainedFailure(const FMtoULinkageView& View)
    {
        if (View.Error.IsEmpty() || View.Error == SustainedLastError)
        {
            return;
        }
        SustainedLastError = View.Error;
        ++SustainedFailures;
        if (SustainedFailures <= 5)
        {
            Log(FString::Printf(TEXT("[sustained] refusal %d at %.2f s: %s"), SustainedFailures,
                FPlatformTime::Seconds() - SustainedStart, *View.Error.Right(600)));
            // Which writer the display follows when the barrier refuses it.
            for (const bool bSecondary : {false, true})
            {
                const AMtoULiveLinkActor* Owner = bSecondary ? SecondaryActor.Get() : Actor.Get();
                const USkeletalMeshComponent* Display = Owner ? Owner->GetSkeletalMeshComponent() : nullptr;
                if (!Display)
                {
                    continue;
                }
                const UAnimInstance* Instance = Display->GetAnimInstance();
                Log(FString::Printf(TEXT("[sustained]   %s display: predicted LOD %d, forced LOD %d, recently rendered %d, pause anims %d, enable animation %d, update anim in editor %d"),
                    bSecondary ? TEXT("second") : TEXT("main"), Display->GetPredictedLODLevel(),
                    Display->GetForcedLOD(), Display->WasRecentlyRendered() ? 1 : 0,
                    Display->bPauseAnims ? 1 : 0, Display->bEnableAnimation ? 1 : 0,
                    Display->GetUpdateAnimationInEditor() ? 1 : 0));
                Log(FString::Printf(TEXT("[sustained]   %s display: anim instance %s, mode %d, mesh %s, tick %d, post-process off %d"),
                    bSecondary ? TEXT("second") : TEXT("main"),
                    Instance ? *Instance->GetClass()->GetName() : TEXT("none"),
                    static_cast<int32>(Display->GetAnimationMode()),
                    Display->GetSkeletalMeshAsset() ? *Display->GetSkeletalMeshAsset()->GetName() : TEXT("none"),
                    Display->IsComponentTickEnabled() ? 1 : 0, Display->GetDisablePostProcessBlueprint() ? 1 : 0));
            }
            WritersMuted(true, TEXT("[sustained] at the refusal"));
        }
    }

    static double Percentile(TArray<double> Values, double Fraction)
    {
        if (Values.IsEmpty())
        {
            return 0.0;
        }
        Values.Sort();
        const int32 Rank = FMath::Clamp(FMath::CeilToInt(Fraction * Values.Num()) - 1, 0, Values.Num() - 1);
        return Values[Rank];
    }

    bool FinishSustained(double DelayMs, double Seconds, const FMtoULinkageView& View)
    {
        MtoUSetLinkageApplyProbe(nullptr);
        const FString Tag = FString::Printf(TEXT("[sustained %.0f ms] "), DelayMs);
        const FMtoULinkageScheduleStats& Stats = SustainedPlayRecords;
        TArray<double> Latencies;
        TArray<double> AppliedAt;
        TArray<double> CancelledAt;
        int32 Open = 0, FailedTargets = 0;
        for (const FMtoULinkageIssueRecord& Record : Stats.Records)
        {
            if (Record.Outcome == TEXT("applied"))
            {
                Latencies.Add(Record.AppliedSeconds - Record.IssuedSeconds);
                AppliedAt.Add(Record.AppliedSeconds);
            }
            else if (Record.Outcome == TEXT("cancelled"))
            {
                CancelledAt.Add(Record.EndedSeconds);
            }
            else
            {
                ++Open;
            }
            FailedTargets += Record.FailedApplications > 0 ? 1 : 0;
        }
        AppliedAt.Sort();
        // A gap without a complete application counts against the budget
        // unless a cut, loop wrap or other cancellation happened inside it.
        double MaxGap = 0.0, MaxUnexcusedGap = 0.0;
        int32 ExcusedGaps = 0;
        double Previous = SustainedStart;
        for (int32 Index = 0; Index <= AppliedAt.Num(); ++Index)
        {
            const double Next = Index < AppliedAt.Num() ? AppliedAt[Index] : SustainedPausedAt;
            const double Gap = Next - Previous;
            bool bExcused = false;
            for (const double Cancelled : CancelledAt)
            {
                bExcused |= Cancelled > Previous && Cancelled <= Next;
            }
            MaxGap = FMath::Max(MaxGap, Gap);
            if (bExcused)
            {
                ExcusedGaps += Gap > 2.0 ? 1 : 0;
            }
            else
            {
                MaxUnexcusedGap = FMath::Max(MaxUnexcusedGap, Gap);
            }
            Previous = Next;
        }
        const double Played = SustainedPausedAt - SustainedStart;
        const double P50 = Percentile(Latencies, 0.50), P95 = Percentile(Latencies, 0.95),
            P99 = Percentile(Latencies, 0.99);
        const double MaxLatency = Latencies.IsEmpty() ? 0.0 : FMath::Max(Latencies);
        const double ApplyRate = Stats.Issued > 0 ? double(Stats.Applied) / double(Stats.Issued) : 0.0;
        const auto Mib = [](uint64 Bytes) { return double(Bytes) / (1024.0 * 1024.0); };
        Log(FString::Printf(TEXT("%splayed %.1f s: issued %d, applied %d (%.1f/s, rate %.3f), cancelled %d, open %d, "
                "overdue %d, coalesced %d, failed applications %d on %d targets; issued->applied p50 %.3f s, "
                "p95 %.3f s, p99 %.3f s, max %.3f s; longest gap %.3f s, longest gap without a cancellation "
                "%.3f s (%d longer gaps held a cut, wrap or cancellation); pause converged in %.3f s, jump in %.3f s; "
                "peaks in flight %d, waiting %d, staged %d, control out %d in %d; memory Unreal %.0f -> %.0f MiB, "
                "Maya %.0f -> %.0f MiB; dropped records %d"),
            *Tag, Played, Stats.Issued, Stats.Applied, Played > 0.0 ? Stats.Applied / Played : 0.0, ApplyRate,
            Stats.Cancelled, Open, Stats.Overdue, Stats.Coalesced, Stats.Failed, FailedTargets, P50, P95, P99,
            MaxLatency, MaxGap, MaxUnexcusedGap, ExcusedGaps, SustainedPauseConvergence, SustainedJumpConvergence,
            Stats.InFlightPeak, Stats.PendingPeak, Stats.StagedPeak, Stats.OutgoingControlPeak,
            Stats.IncomingControlPeak, Mib(SustainedUeStart), Mib(SustainedUePeak), Mib(SustainedMayaStart),
            Mib(SustainedMayaPeak), Stats.DroppedRecords));

        Log(FString::Printf(TEXT("%sdistinct refusals %d"), *Tag, SustainedFailures));
        const TSharedRef<FJsonObject> Tier = MakeShared<FJsonObject>();
        Tier->SetNumberField(TEXT("delay_ms"), DelayMs);
        Tier->SetStringField(TEXT("injection"), DelayMs > 0.0
            ? TEXT("each published group held at the Unreal set completion barrier for delay_ms")
            : TEXT("none (natural delay)"));
        Tier->SetNumberField(TEXT("frozen_seconds"), Seconds);
        Tier->SetNumberField(TEXT("played_seconds"), Played);
        Tier->SetNumberField(TEXT("issued"), Stats.Issued);
        Tier->SetNumberField(TEXT("applied"), Stats.Applied);
        Tier->SetNumberField(TEXT("applied_per_second"), Played > 0.0 ? Stats.Applied / Played : 0.0);
        Tier->SetNumberField(TEXT("apply_rate"), ApplyRate);
        Tier->SetNumberField(TEXT("cancelled"), Stats.Cancelled);
        Tier->SetNumberField(TEXT("open"), Open);
        Tier->SetNumberField(TEXT("overdue"), Stats.Overdue);
        Tier->SetNumberField(TEXT("coalesced"), Stats.Coalesced);
        Tier->SetNumberField(TEXT("failed_applications"), Stats.Failed);
        Tier->SetNumberField(TEXT("latency_p50_s"), P50);
        Tier->SetNumberField(TEXT("latency_p95_s"), P95);
        Tier->SetNumberField(TEXT("latency_p99_s"), P99);
        Tier->SetNumberField(TEXT("latency_max_s"), MaxLatency);
        Tier->SetNumberField(TEXT("max_gap_s"), MaxGap);
        Tier->SetNumberField(TEXT("max_gap_without_cancellation_s"), MaxUnexcusedGap);
        Tier->SetNumberField(TEXT("long_gaps_with_cancellation"), ExcusedGaps);
        Tier->SetNumberField(TEXT("pause_convergence_s"), SustainedPauseConvergence);
        Tier->SetNumberField(TEXT("jump_convergence_s"), SustainedJumpConvergence);
        Tier->SetNumberField(TEXT("in_flight_peak"), Stats.InFlightPeak);
        Tier->SetNumberField(TEXT("waiting_peak"), Stats.PendingPeak);
        Tier->SetNumberField(TEXT("staged_peak"), Stats.StagedPeak);
        Tier->SetNumberField(TEXT("control_out_peak"), Stats.OutgoingControlPeak);
        Tier->SetNumberField(TEXT("control_in_peak"), Stats.IncomingControlPeak);
        Tier->SetNumberField(TEXT("unreal_memory_start_mib"), Mib(SustainedUeStart));
        Tier->SetNumberField(TEXT("unreal_memory_peak_mib"), Mib(SustainedUePeak));
        Tier->SetNumberField(TEXT("maya_memory_start_mib"), Mib(SustainedMayaStart));
        Tier->SetNumberField(TEXT("maya_memory_peak_mib"), Mib(SustainedMayaPeak));
        Tier->SetNumberField(TEXT("distinct_refusals"), SustainedFailures);
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const FMtoULinkageIssueRecord& Record : Stats.Records)
        {
            const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("serial"), Record.IssueSerial);
            Row->SetNumberField(TEXT("frame"), Record.DisplayFrame);
            Row->SetNumberField(TEXT("issued_s"), Record.IssuedSeconds - SustainedStart);
            Row->SetNumberField(TEXT("ended_s"), Record.EndedSeconds > 0.0 ? Record.EndedSeconds - SustainedStart : -1.0);
            Row->SetStringField(TEXT("outcome"), Record.Outcome);
            Row->SetBoolField(TEXT("overdue"), Record.bOverdue);
            Row->SetNumberField(TEXT("failed_applications"), Record.FailedApplications);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        Tier->SetArrayField(TEXT("records"), Rows);
        WriteJsonFile(FPaths::Combine(EvidenceDir, FString::Printf(TEXT("sustained-%.0fms.json"), DelayMs)), Tier);
        SustainedTiers.Add(MakeShared<FJsonValueObject>(Tier));
        Evidence->SetArrayField(TEXT("sustained"), SustainedTiers);

        // The budgets frozen before the run (issue #59, design-01).
        Test->TestTrue(*(Tag + TEXT("the whole run fits the issue records")), Stats.DroppedRecords == 0);
        Test->TestTrue(*(Tag + TEXT("the sequence played for the frozen duration")), Played >= Seconds);
        Test->TestTrue(*(Tag + TEXT("complete groups were applied while playing")), Stats.Applied > 0);
        Test->TestTrue(*(Tag + TEXT("no continuous stretch goes longer than 2 s without a complete application")),
            MaxUnexcusedGap <= FMtoUProtocol::LinkageConvergenceSeconds);
        Test->TestTrue(*(Tag + TEXT("the pause converges within 2 s")),
            View.bConverged && SustainedPauseConvergence > 0.0
            && SustainedPauseConvergence <= FMtoUProtocol::LinkageConvergenceSeconds);
        Test->TestTrue(*(Tag + TEXT("the jump converges within 2 s")),
            SustainedJumpConvergence > 0.0 && SustainedJumpConvergence <= FMtoUProtocol::LinkageConvergenceSeconds);
        Test->TestTrue(*(Tag + TEXT("at most one target is in flight")), Stats.InFlightPeak <= 1);
        Test->TestTrue(*(Tag + TEXT("at most one target waits")), Stats.PendingPeak <= 1);
        Test->TestTrue(*(Tag + TEXT("staged groups stay within the frozen budget")),
            Stats.StagedPeak <= FMtoUProtocol::MaxPendingCameraTargets);
        Test->TestTrue(*(Tag + TEXT("both control queues stay within the frozen budget")),
            Stats.OutgoingControlPeak <= FMtoUProtocol::MaxControlQueue
            && Stats.IncomingControlPeak <= FMtoUProtocol::MaxControlQueue);
        SustainedPhase = 0;
        SustainedPauseConvergence = SustainedJumpConvergence = 0.0;
        SustainedFailures = 0;
        SustainedLastError.Reset();
        return true;
    }

    // ------------------------------------------------------------ object set

    int32 MayaDialogCount() const
    {
        const TArray<TSharedPtr<FJsonValue>>* Dialogs = nullptr;
        return MayaResult.IsValid() && MayaResult->TryGetArrayField(TEXT("dialogs"), Dialogs) ? Dialogs->Num() : 0;
    }

    /** Sends one Maya command once and finishes when Maya acknowledged it. */
    bool SendOnce(const FString& Action, const FString& Target = FString(), const FString& Name = FString())
    {
        if (OnceId < 0)
        {
            OnceId = SendMaya(Action, Target, Name);
            return false;
        }
        if (!MayaAcked(OnceId))
        {
            return false;
        }
        OnceId = -1;
        return true;
    }

    bool MayaConnected() const
    {
        bool bConnected = false;
        return MayaResult.IsValid() && MayaResult->TryGetBoolField(TEXT("connected"), bConnected) && bConnected;
    }

    /** Maya still holds a session object, connected or still being torn down. */
    bool MayaSessionOpen() const
    {
        bool bOpen = false;
        return MayaResult.IsValid() && MayaResult->TryGetBoolField(TEXT("session_open"), bOpen) && bOpen;
    }

    /** Refusals of an invalid set, each leaving the connected set as it was. */
    void AddSetRefusals()
    {
        Add(TEXT("[set] dialog stage"), 30.0, [this]() { return SendOnce(TEXT("stage"), FString(), TEXT("set_refusals")); });
        Add(TEXT("[set] one target cannot serve both objects"), 30.0, [this]()
        {
            FString Error;
            const bool bPaired = MtoUPairLinkageTarget(*Actor, SecondaryObjectId, Error);
            Log(TEXT("[set] pairing the main actor as the second object: ") + (bPaired ? FString(TEXT("accepted")) : Error));
            Test->TestFalse(TEXT("the main target cannot also be paired as the second object"), bPaired);
            Test->TestEqual(TEXT("the main target keeps its pairing"), MtoUGetLinkagePairedObject(*Actor),
                Fixture->GetStringField(TEXT("object_id")));
            Test->TestEqual(TEXT("the second target keeps its pairing"), MtoUGetLinkagePairedObject(*SecondaryActor),
                FString(SecondaryObjectId));
            return true;
        });
        Add(TEXT("[set] Maya refuses its main root as the second object"), 60.0, [this]()
        {
            if (RefusalPhase == 0)
            {
                DialogsBefore = MayaDialogCount();
                RefusalPhase = 1;
            }
            if (!SendOnce(TEXT("secondary_is_primary")))
            {
                return false;
            }
            RefusalPhase = 0;
            Test->TestTrue(TEXT("Maya explains why its main root cannot be the second object"),
                MayaDialogCount() > DialogsBefore);
            Test->TestTrue(TEXT("Maya keeps the second object it had"), MayaSetHasSecondary());
            Test->TestTrue(TEXT("the connection keeps both objects"), SetConnected() && NegotiatedObjects() == 2);
            return true;
        });
        Add(TEXT("[set] a second target at another placement is refused"), 60.0, [this]()
        {
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (RefusalPhase == 0)
            {
                RefusalPhase = 1;
                SecondaryActor->SetActorLocation(AnchorTransform.GetLocation() + FVector(15.0, 0.0, 0.0));
                FString Error;
                const bool bAccepted = MtoURequestLinkageEnter(View.Identity.ModeRevision + 1, Error);
                Log(TEXT("[set] entering with a moved second target: ")
                    + (bAccepted ? FString(TEXT("request sent")) : TEXT("refused at once: ") + Error));
                if (!bAccepted)
                {
                    RefusalText = Error;
                    RefusalPhase = 2;
                }
                RefusalStarted = FPlatformTime::Seconds();
                return false;
            }
            if (RefusalPhase == 1)
            {
                if (View.ErrorCode == TEXT("ANCHOR_MISMATCH") && View.Mode == TEXT("realtime") && !View.bTransitioning)
                {
                    RefusalText = View.ErrorCode + TEXT(" ") + View.Error + TEXT(" ") + View.Detail;
                    RefusalPhase = 2;
                }
                else if (View.Mode == TEXT("linkage") && !View.bTransitioning)
                {
                    Test->AddError(TEXT("linkage was entered with the second target at another placement"));
                    RefusalText = TEXT("entered");
                    RefusalPhase = 2;
                }
                return false;
            }
            SecondaryActor->SetActorTransform(AnchorTransform);
            if (View.Mode != TEXT("realtime") || View.bTransitioning || View.bTakeoverHeld)
            {
                if (View.Mode == TEXT("linkage") && !View.bTransitioning && LeaveAfterRefusal < 0)
                {
                    FString Error;
                    LeaveAfterRefusal = MtoURequestLinkageLeave(TEXT("end"), View.Identity.ModeRevision + 1, Error) ? 1 : 0;
                }
                return false;
            }
            Log(TEXT("[set] refusal: ") + RefusalText);
            Test->TestTrue(TEXT("the moved second target is refused for its placement"),
                RefusalText.Contains(TEXT("ANCHOR_MISMATCH")) || RefusalText.Contains(TEXT("placement")));
            Test->TestTrue(TEXT("the refused entry leaves every writer live"), WritersMuted(false, TEXT("[set] after the anchor refusal")));
            RefusalPhase = 0;
            LeaveAfterRefusal = -1;
            return true;
        });
        Add(TEXT("[set] wrong skeleton: dialog stage"), 30.0, [this]() { return SendOnce(TEXT("stage"), FString(), TEXT("wrong_skeleton")); });
        Add(TEXT("[set] wrong skeleton: disconnect"), 120.0, [this]()
        {
            if (RefusalPhase == 0)
            {
                if (SendOnce(TEXT("disconnect")))
                {
                    RefusalPhase = 1;
                }
                return false;
            }
            if (MayaConnected())
            {
                return false;
            }
            RefusalPhase = 0;
            return true;
        });
        Add(TEXT("[set] a second target of another skeleton is refused"), 600.0, [this]()
        {
            if (RefusalPhase == 0)
            {
                // The second actor now shows the main character's mesh, whose
                // skeleton the second Maya object does not have.
                WrongBinding.Reset(DuplicateObject<UMtoULiveLinkBinding>(Binding.Get(), GetTransientPackage()));
                SecondaryActor->SetBinding(WrongBinding.Get());
                DialogsBefore = MayaDialogCount();
                RefusalPhase = 1;
                return false;
            }
            if (RefusalPhase == 1)
            {
                if (SendOnce(TEXT("connect")))
                {
                    RefusalPhase = 2;
                    RefusalStarted = FPlatformTime::Seconds();
                }
                return false;
            }
            // Maya answers the refused object with a dialog; Unreal never
            // reports the second target connected.
            if (MayaDialogCount() <= DialogsBefore)
            {
                return false;
            }
            const TArray<TSharedPtr<FJsonValue>>& Dialogs = MayaResult->GetArrayField(TEXT("dialogs"));
            FString Text;
            FJsonSerializer::Serialize(Dialogs.Last()->AsObject().ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
            Log(TEXT("[set] wrong skeleton refusal dialog: ") + Text.Left(600));
            Test->TestTrue(TEXT("the wrong skeleton is refused by name"), Text.Contains(TEXT("SKELETON_MISMATCH")));
            Test->TestFalse(TEXT("the second target of the wrong skeleton never connects"),
                SecondaryActor->GetConnectionStatus().StartsWith(TEXT("Connected")) && NegotiatedObjects() == 2);
            RefusalPhase = 0;
            return true;
        });
        Add(TEXT("[set] the right skeleton connects again"), 600.0, [this]()
        {
            if (RefusalPhase == 0)
            {
                // A refused object may leave Maya connected with the main
                // character only; the right Binding needs a new connection.
                if (MayaConnected() || OnceId >= 0)
                {
                    if (OnceId >= 0 || !bDisconnectSent)
                    {
                        bDisconnectSent |= SendOnce(TEXT("disconnect"));
                    }
                    return false;
                }
                bDisconnectSent = false;
                SecondaryActor->SetBinding(SecondaryBinding.Get());
                RefusalPhase = 1;
                return false;
            }
            if (RefusalPhase == 1)
            {
                if (SendOnce(TEXT("connect")))
                {
                    RefusalPhase = 2;
                }
                return false;
            }
            if (!SetConnected())
            {
                return false;
            }
            PairAgain();
            RefusalPhase = 0;
            Test->TestEqual(TEXT("the reconnected set has both objects"), NegotiatedObjects(), 2);
            return true;
        });
    }

    /**
     * Scene reference in the common space of a two-object set. In the new map
     * the last exit left behind, both Binding Actors stand at the anchor next
     * to one probe cube; the reference is exported through the product path and
     * imported by Maya's own action, and the cube's world centre in Maya must be
     * the inverse anchor of its Unreal world position, the same space both
     * objects' poses arrive in. The fixture's `reference_probe` gives the cube's
     * `offset` and `rotation` relative to the anchor.
     */
    void AddReferenceAlignment()
    {
        Add(TEXT("[reference] dialog stage"), 30.0, [this]() { return SendOnce(TEXT("stage"), FString(), TEXT("reference")); });
        Add(TEXT("[reference] place the set and a probe in a new map"), 60.0, [this]()
        {
            UWorld* World = GEditor->GetEditorWorldContext().World();
            if (!World)
            {
                return false;
            }
            const TSharedPtr<FJsonObject> Probe = Fixture->GetObjectField(TEXT("reference_probe"));
            const FVector Euler = ReadVector(Probe, TEXT("rotation"));
            const FTransform Local(FRotator(Euler.Y, Euler.Z, Euler.X), ReadVector(Probe, TEXT("offset")));
            ProbeTransform = Local * AnchorTransform;
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transient;
            for (const bool bSecondary : {false, true})
            {
                AMtoULiveLinkActor* Spawned = World->SpawnActor<AMtoULiveLinkActor>(
                    AnchorTransform.GetLocation(), AnchorTransform.Rotator(), Params);
                Spawned->SetBinding(bSecondary ? SecondaryBinding.Get() : Binding.Get());
                (bSecondary ? SecondaryActor : Actor) = Spawned;
            }
            AStaticMeshActor* Cube = World->SpawnActor<AStaticMeshActor>(
                ProbeTransform.GetLocation(), ProbeTransform.Rotator(), Params);
            Cube->GetStaticMeshComponent()->SetStaticMesh(
                LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
            Cube->SetActorLabel(TEXT("MtoURefProbe"));
            WorldPtr = World;
            Log(TEXT("[reference] probe at ") + ProbeTransform.ToHumanReadableString());
            return PairTargets();
        });
        Add(TEXT("[reference] the set connects in the new map"), 600.0, [this]() { return ReconnectSet(TEXT("[reference]")); });
        Add(TEXT("[reference] export through the product path"), 300.0, [this]()
        {
            FMtoUSceneRefExportResult Result;
            const bool bExported = FMtoUSceneRefExport::Run(*WorldPtr, TArray<FString>(), Result);
            Log(FString::Printf(TEXT("[reference] export %s: %s"), bExported ? TEXT("offered") : TEXT("refused"),
                *Result.Error));
            Test->TestTrue(TEXT("the scene reference is exported and offered"), bExported);
            return true;
        });
        Add(TEXT("[reference] Maya imports the offered reference"), 300.0, [this]()
        {
            const TSharedPtr<FJsonObject>* Reference = nullptr;
            bool bOffered = false;
            if (!MayaResult->TryGetObjectField(TEXT("reference"), Reference)
                || !(*Reference)->TryGetBoolField(TEXT("offered"), bOffered) || !bOffered)
            {
                return false;
            }
            if (RefusalPhase == 0)
            {
                // One click of 导入/更新参考.
                if (SendOnce(TEXT("import_reference")))
                {
                    RefusalPhase = 1;
                }
                return false;
            }
            const TSharedPtr<FJsonObject> Report = (*Reference)->GetObjectField(TEXT("report"));
            FString Action;
            Report->TryGetStringField(TEXT("action"), Action);
            if (Action != TEXT("import"))
            {
                return false;
            }
            bool bAccepted = false;
            Report->GetObjectField(TEXT("report"))->TryGetBoolField(TEXT("accepted"), bAccepted);
            FString Text;
            FJsonSerializer::Serialize(Report.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
            Log(TEXT("[reference] import report: ") + Text.Left(600));
            Test->TestTrue(TEXT("Maya accepts and imports the reference"), bAccepted);
            RefusalPhase = 0;
            return true;
        });
        Add(TEXT("[reference] the probe lines up through the inverse anchor"), 60.0, [this]()
        {
            if (!SendOnce(TEXT("reference_snapshot"), FString(), TEXT("MtoURefProbe")))
            {
                return false;
            }
            const TSharedPtr<FJsonObject>* Nodes = nullptr;
            if (!MayaResult->TryGetObjectField(TEXT("reference_nodes"), Nodes) || (*Nodes)->Values.IsEmpty())
            {
                Test->AddError(TEXT("Maya's reference container has no probe node"));
                return true;
            }
            // Maya (x, y, z) is Unreal (x, z, y); the container stands at the inverse anchor.
            const FVector Local = AnchorTransform.InverseTransformPosition(ProbeTransform.GetLocation());
            const FVector Expected(Local.X, Local.Z, Local.Y);
            double MaxDelta = 0.0;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Node : (*Nodes)->Values)
            {
                const TArray<TSharedPtr<FJsonValue>>& C = Node.Value->AsArray();
                const FVector Shown(C[0]->AsNumber(), C[1]->AsNumber(), C[2]->AsNumber());
                MaxDelta = FMath::Max(MaxDelta, FVector::Distance(Shown, Expected));
                Log(FString::Printf(TEXT("[reference] %s: Maya centre %s, expected %s (%.4f cm)"), *Node.Key,
                    *Shown.ToString(), *Expected.ToString(), FVector::Distance(Shown, Expected)));
            }
            Test->TestTrue(TEXT("the reference probe stands where the set's common space puts it"),
                MaxDelta < Fixture->GetNumberField(TEXT("world_tolerance_cm")));
            return true;
        });
    }

    /**
     * Synthetic composition changes of a connected set: each replaces one
     * object's full body or parts, which must end the whole set; Maya then
     * connects again with the changed object, and the original composition
     * is restored and connected the same way. The fixture names the swaps in
     * `composition_swaps`: `{name, object: "character"|"secondary",
     * skeletal_mesh?, parts?}`.
     */
    void AddCompositionSwaps()
    {
        const TArray<TSharedPtr<FJsonValue>>* Swaps = nullptr;
        if (!Fixture->TryGetArrayField(TEXT("composition_swaps"), Swaps) || Swaps->IsEmpty())
        {
            return;
        }
        Add(TEXT("[swap] dialog stage"), 30.0, [this]() { return SendOnce(TEXT("stage"), FString(), TEXT("composition_swaps")); });
        for (const TSharedPtr<FJsonValue>& Value : *Swaps)
        {
            const TSharedPtr<FJsonObject> Swap = Value->AsObject();
            const FString Name = Swap->GetStringField(TEXT("name"));
            const bool bSecondary = Swap->GetStringField(TEXT("object")) == SecondaryObjectId;
            Add(FString::Printf(TEXT("[swap] %s ends the set"), *Name), 120.0, [this, Swap, Name, bSecondary]()
            {
                return ChangeComposition(Swap, bSecondary, Name, true);
            });
            Add(FString::Printf(TEXT("[swap] %s connects again"), *Name), 600.0, [this, Name]()
            {
                return ReconnectSet(TEXT("[swap] ") + Name + TEXT(" changed"));
            });
            Add(FString::Printf(TEXT("[swap] %s restored ends the set"), *Name), 120.0, [this, Swap, Name, bSecondary]()
            {
                return ChangeComposition(Swap, bSecondary, Name, false);
            });
            Add(FString::Printf(TEXT("[swap] %s restored connects again"), *Name), 600.0, [this, Name]()
            {
                return ReconnectSet(TEXT("[swap] ") + Name + TEXT(" restored"));
            });
        }
    }

    /** Applies one swap (or puts the original back) and waits for the set to end. */
    bool ChangeComposition(const TSharedPtr<FJsonObject>& Swap, bool bSecondary, const FString& Name, bool bApply)
    {
        AMtoULiveLinkActor* Owner = bSecondary ? SecondaryActor.Get() : Actor.Get();
        UMtoULiveLinkBinding* Target = bSecondary ? SecondaryBinding.Get() : Binding.Get();
        if (RefusalPhase == 0)
        {
            if (bApply)
            {
                SavedMesh = Target->SkeletalMesh;
                SavedParts = Target->AdditionalParts;
            }
            FString Path;
            const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
            if (Swap->TryGetStringField(TEXT("skeletal_mesh"), Path))
            {
                Target->SkeletalMesh = bApply ? LoadObject<USkeletalMesh>(nullptr, *Path) : SavedMesh.Get();
                Owner->NotifyBindingInputsChanged();
            }
            if (Swap->TryGetArrayField(TEXT("parts"), Parts))
            {
                if (bApply)
                {
                    Target->AdditionalParts.Reset();
                    for (const TSharedPtr<FJsonValue>& Part : *Parts)
                    {
                        FMtoUCharacterPart& Entry = Target->AdditionalParts.AddDefaulted_GetRef();
                        Entry.SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *Part->AsString());
                        Entry.PartName = FPaths::GetBaseFilename(Part->AsString());
                        Entry.bEnabled = true;
                    }
                    Target->EnsureCharacterPartIds();
                }
                else
                {
                    Target->AdditionalParts = SavedParts;
                }
                Owner->NotifyCharacterPartsChanged();
            }
            RefusalPhase = 1;
            return false;
        }
        return SetEnded(FString::Printf(TEXT("[swap] %s %s"), *Name, bApply ? TEXT("applied") : TEXT("restored")));
    }

    /** The changed object ends the whole set: neither actor stays connected, and Maya finished tearing its session down. */
    bool SetEnded(const FString& What)
    {
        if (MayaConnected() || MayaSessionOpen() || Actor->GetConnectionStatus().StartsWith(TEXT("Connected"))
            || SecondaryActor->GetConnectionStatus().StartsWith(TEXT("Connected")))
        {
            return false;
        }
        Log(FString::Printf(TEXT("%s: the set ended (UE '%s' / '%s')"), *What, *Actor->GetConnectionStatus(),
            *SecondaryActor->GetConnectionStatus()));
        Test->TestFalse(*(What + TEXT(" leaves no object streaming")),
            Actor->IsInLiveLinkSet() || SecondaryActor->IsInLiveLinkSet());
        RefusalPhase = 0;
        return true;
    }

    /**
     * Synthetic full-body/arms replacement of the second object. The arms
     * target is cut from the object's own full body: the triangles only the
     * fixture's `arms.chains` skin, on the same skeleton, with one Morph Target
     * for each Maya Morph of that object at the first oracle frame. Replacing
     * the full body with it (bArms) or putting the full body back ends the
     * whole set; Maya connects again and the set negotiates the new target's
     * required bones.
     */
    void AddArmsReplacement(bool bArms)
    {
        const FString What = bArms ? TEXT("[arms] full body replaced by arms") : TEXT("[arms] arms replaced by the full body");
        if (bArms)
        {
            Add(TEXT("[arms] build the synthetic arms target"), 300.0, [this]() { return BuildArmsTarget(); });
        }
        Add(What + TEXT(" ends the set"), 120.0, [this, bArms, What]()
        {
            if (RefusalPhase == 0)
            {
                if (!ArmsMesh.IsValid())
                {
                    Test->AddError(TEXT("[arms] no arms target was built"));
                    bAbort = true;
                    return true;
                }
                SecondaryBinding->SkeletalMesh = bArms ? ArmsMesh.Get() : FullBodyMesh.Get();
                SecondaryActor->NotifyBindingInputsChanged();
                RefusalPhase = 1;
                return false;
            }
            return SetEnded(What);
        });
        Add(What + TEXT(": connects again"), 600.0, [this, What]() { return ReconnectSet(What); });
        Add(What + TEXT(": required bones"), 30.0, [this, bArms, What]()
        {
            const TArray<FName> Required = RequiredBones(SecondaryBinding.Get());
            Log(FString::Printf(TEXT("%s: the second object now requires %d bones (full body %d, arms %d)"),
                *What, Required.Num(), FullRequired.Num(), ArmsRequired.Num()));
            Test->TestTrue(*(What + TEXT(": the set negotiates the new target's required bones")),
                Required == (bArms ? ArmsRequired : FullRequired));
            return true;
        });
    }

    static TArray<FName> RequiredBones(const UMtoULiveLinkBinding* Target)
    {
        const FMtoUCharacterComposition Composition = FMtoUCharacterComposition::Resolve(Target);
        TArray<FName> Out;
        if (Composition.IsUsable())
        {
            for (int32 Index = 0; Index < Composition.RequiredSkeleton.GetNum(); ++Index)
            {
                Out.Add(Composition.RequiredSkeleton.GetBoneName(Index));
            }
        }
        return Out;
    }

    bool BuildArmsTarget()
    {
        const TSharedPtr<FJsonObject> Arms = Fixture->GetObjectField(TEXT("arms"));
        USkeletalMesh* Body = SecondaryBinding->SkeletalMesh;
        FullBodyMesh.Reset(Body);
        TSet<FName> Chains;
        for (const TSharedPtr<FJsonValue>& Chain : Arms->GetArrayField(TEXT("chains")))
        {
            Chains.Add(FName(*Chain->AsString()));
        }
        // The Morphs Maya animates on this object at the first oracle frame.
        TArray<FName> MorphNames;
        if (const TSharedPtr<FJsonObject> Record = ReadJsonFile(FPaths::Combine(EvidenceDir, TEXT("oracle-0.json"))))
        {
            const TSharedPtr<FJsonObject> Row = SnapshotRow(Record->GetObjectField(TEXT("maya_snapshot")), true);
            const TSharedPtr<FJsonObject>* Morphs = nullptr;
            if (Row.IsValid() && Row->TryGetObjectField(TEXT("morphs"), Morphs))
            {
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Morph : (*Morphs)->Values)
                {
                    MorphNames.Add(FName(*Morph.Key));
                }
            }
        }
        if (!Test->TestTrue(TEXT("[arms] the first oracle frame names the second object's Maya Morphs"), MorphNames.Num() > 0))
        {
            bAbort = true;
            return true;
        }

        // Keep the triangles every corner of which only the arm chains skin.
        const FReferenceSkeleton& Skeleton = Body->GetRefSkeleton();
        TSet<FName> ArmBones;
        for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
        {
            for (int32 Bone = Index; Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone))
            {
                if (Chains.Contains(Skeleton.GetBoneName(Bone)))
                {
                    ArmBones.Add(Skeleton.GetBoneName(Index));
                    break;
                }
            }
        }
        UDynamicMesh* Source = NewObject<UDynamicMesh>(GetTransientPackage());
        FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
        ReadOptions.bApplyBuildSettings = false;
        ReadOptions.bRequestTangents = true;
        FGeometryScriptMeshReadLOD ReadLOD;
        ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(Body, Source, ReadOptions, ReadLOD, Outcome);
        if (!Test->TestTrue(TEXT("[arms] the full body's geometry is read"), Outcome == EGeometryScriptOutcomePins::Success))
        {
            bAbort = true;
            return true;
        }
        UE::Geometry::FDynamicMesh3 Mesh(Source->GetMeshRef());
        const UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* Weights = Mesh.HasAttributes()
            ? Mesh.Attributes()->GetSkinWeightsAttribute(FSkeletalMeshAttributes::DefaultSkinWeightProfileName) : nullptr;
        const UE::Geometry::FDynamicMeshBoneNameAttribute* Names = Mesh.HasAttributes() ? Mesh.Attributes()->GetBoneNames() : nullptr;
        if (!Test->TestTrue(TEXT("[arms] the full body's skin weights are read"), Weights && Names))
        {
            bAbort = true;
            return true;
        }
        TArray<int32> Others;
        for (const int32 Triangle : Mesh.TriangleIndicesItr())
        {
            const UE::Geometry::FIndex3i Corners = Mesh.GetTriangle(Triangle);
            bool bArms = true;
            for (int32 Corner = 0; Corner < 3 && bArms; ++Corner)
            {
                UE::AnimationCore::FBoneWeights Value;
                Weights->GetValue(Corners[Corner], Value);
                for (int32 Influence = 0; Influence < Value.Num() && bArms; ++Influence)
                {
                    bArms = ArmBones.Contains(Names->GetValue(Value[Influence].GetBoneIndex()));
                }
            }
            if (!bArms)
            {
                Others.Add(Triangle);
            }
        }
        const int32 BodyTriangles = Mesh.TriangleCount();
        for (const int32 Triangle : Others)
        {
            Mesh.RemoveTriangle(Triangle, true, false);
        }
        Mesh.CompactInPlace();
        if (Mesh.Attributes()->HasMaterialID())
        {
            for (const int32 Triangle : Mesh.TriangleIndicesItr())
            {
                Mesh.Attributes()->GetMaterialID()->SetValue(Triangle, 0);
            }
        }
        if (!Test->TestTrue(TEXT("[arms] the arm chains alone skin part of the full body"), Mesh.TriangleCount() > 0))
        {
            bAbort = true;
            return true;
        }
        // A /Temp package: the writer refuses the engine's transient package. Nothing saves it.
        UPackage* ArmsPackage = CreatePackage(TEXT("/Temp/MtoULinkageArmsTarget"));
        ArmsMesh.Reset(MtoUEditorTest::WriteTransientDriver(*ArmsPackage, *Body, Mesh, {FName(TEXT("Arms"))}));
        if (!Test->TestNotNull(TEXT("[arms] the arms target is written"), ArmsMesh.Get()))
        {
            bAbort = true;
            return true;
        }
        UE::Geometry::FAxisAlignedBox3d Bounds = Mesh.GetBounds();
        Bounds.Expand(1.0);
        for (const FName Name : MorphNames)
        {
            Test->TestTrue(*FString::Printf(TEXT("[arms] Morph Target '%s' is added"), *Name.ToString()),
                MtoUEditorTest::AddBoxMorph(*ArmsMesh, Name, Bounds, FVector3f(0.0f, 0.0f, 0.5f)));
        }

        // The required bones negotiation resolves for each target.
        FullRequired = RequiredBones(SecondaryBinding.Get());
        UMtoULiveLinkBinding* ArmsBinding = DuplicateObject<UMtoULiveLinkBinding>(SecondaryBinding.Get(), GetTransientPackage());
        ArmsBinding->SkeletalMesh = ArmsMesh.Get();
        ArmsRequired = RequiredBones(ArmsBinding);
        TSet<FString> Unstreamed;
        for (const TSharedPtr<FJsonValue>& Bone : Arms->GetArrayField(TEXT("unstreamed")))
        {
            Unstreamed.Add(Bone->AsString());
        }
        int32 Foreign = 0, Missing = 0, Kept = 0;
        for (const FName Bone : ArmsRequired)
        {
            Foreign += FullRequired.Contains(Bone) ? 0 : 1;
            Kept += Unstreamed.Contains(Bone.ToString()) ? 1 : 0;
        }
        for (const FName Chain : Chains)
        {
            Missing += ArmsRequired.Contains(Chain) ? 0 : 1;
        }
        Log(FString::Printf(TEXT("[arms] arms target: %d of %d triangles, %d Morph Targets (%s); required bones: arms %d, full body %d"),
            Mesh.TriangleCount(), BodyTriangles, MorphNames.Num(),
            *FString::JoinBy(MorphNames, TEXT(", "), [](FName Name) { return Name.ToString(); }),
            ArmsRequired.Num(), FullRequired.Num()));
        Log(TEXT("[arms] arms required bones: ") + FString::JoinBy(ArmsRequired, TEXT(" "), [](FName Name) { return Name.ToString(); }));
        Test->TestTrue(TEXT("[arms] the arms target requires fewer bones than the full body"),
            ArmsRequired.Num() > 0 && ArmsRequired.Num() < FullRequired.Num());
        Test->TestEqual(TEXT("[arms] every arms bone is a full-body bone"), Foreign, 0);
        Test->TestEqual(TEXT("[arms] the arms target requires its arm chains"), Missing, 0);
        Test->TestEqual(TEXT("[arms] the arms target requires none of the fixture's unstreamed bones"), Kept, 0);
        return true;
    }

    /** In linkage, the arms display shows Maya's arm bones and Morphs at the first oracle frame. */
    void AddArmsOracle()
    {
        Add(TEXT("[arms] converge on the first oracle frame"), 120.0, [this]()
        {
            if (!SetTimeAndSettleOnce(OracleFrames[0]))
            {
                return false;
            }
            const FMtoULinkageView View = MtoUGetLinkageView();
            return View.bConverged && !View.AppliedEvalId.IsEmpty()
                && FMath::IsNearlyEqual(View.AppliedDisplayFrame, OracleFrames[0], 0.01);
        });
        Add(TEXT("[arms] the arms display shows Maya's arms"), 60.0, [this]()
        {
            if (!SendOnce(TEXT("snapshot")))
            {
                return false;
            }
            const double Now = FPlatformTime::Seconds();
            if (DisplayedMorphDelta() >= Fixture->GetNumberField(TEXT("morph_tolerance")))
            {
                if (MorphWaitStarted == 0.0)
                {
                    MorphWaitStarted = Now;
                }
                if (Now - MorphWaitStarted < 5.0)
                {
                    return false;
                }
            }
            MorphWaitStarted = 0.0;
            SettledFrame = -1.0;
            const TSharedPtr<FJsonObject> Snapshot = MayaResult->GetObjectField(TEXT("snapshot"));
            FMinimalViewInfo View;
            TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
            TSharedRef<FJsonObject> ArmsRecord = MakeShared<FJsonObject>();
            const FObjectOracle Main = CompareObjectOracle(0, false, SnapshotRow(Snapshot, false), View, false, Record);
            CheckObjectOracle(0, false, Main, TEXT("no camera"));
            const FObjectOracle ArmsOracle = CompareObjectOracle(0, true, SnapshotRow(Snapshot, true), View, false, ArmsRecord);
            CheckObjectOracle(0, true, ArmsOracle, TEXT("no camera"));
            Record->SetObjectField(TEXT("secondary"), ArmsRecord);
            Record->SetObjectField(TEXT("maya_snapshot"), Snapshot);
            // Unreal's applied evidence for the arms object, as Maya received it.
            int32 ArmsBones = -1, ArmsMorphs = -1;
            const TSharedPtr<FJsonObject>* Applied = nullptr;
            if (MayaResult->TryGetObjectField(TEXT("applied"), Applied))
            {
                Record->SetObjectField(TEXT("applied"), *Applied);
                const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
                if ((*Applied)->TryGetArrayField(TEXT("components"), Components))
                {
                    for (const TSharedPtr<FJsonValue>& Component : *Components)
                    {
                        if (Component->AsObject()->GetStringField(TEXT("object_id")) == SecondaryObjectId)
                        {
                            ArmsBones = Component->AsObject()->GetIntegerField(TEXT("bones"));
                            ArmsMorphs = Component->AsObject()->GetIntegerField(TEXT("morphs"));
                        }
                    }
                }
            }
            WriteJsonFile(FPaths::Combine(EvidenceDir, TEXT("oracle-arms.json")), Record);
            Log(FString::Printf(TEXT("[arms] Unreal applied the arms object with %d bones and %d Morphs (required %d bones)"),
                ArmsBones, ArmsMorphs, ArmsRequired.Num()));
            Test->TestEqual(TEXT("[arms] the arms display holds exactly the arms' required bones"), ArmsBones, ArmsRequired.Num());
            Test->TestTrue(TEXT("[arms] the arms display holds Maya's Morphs"), ArmsMorphs >= 1);
            return true;
        });
    }

    /** Maya connects the set again; both objects negotiate. */
    bool ReconnectSet(const FString& What)
    {
        if (RefusalPhase == 0)
        {
            if (MayaSessionOpen())
            {
                return false;
            }
            if (SendOnce(TEXT("connect")))
            {
                RefusalPhase = 1;
                RefusalStarted = FPlatformTime::Seconds();
            }
            return false;
        }
        if (!SetConnected())
        {
            // An idle Maya after the 连接 click: the click landed while an
            // ending session was still torn down, so it is clicked again.
            if (!MayaSessionOpen() && FPlatformTime::Seconds() - RefusalStarted > 10.0)
            {
                Log(What + TEXT(": Maya stayed idle after 连接; connecting again"));
                RefusalPhase = 0;
            }
            return false;
        }
        PairAgain();
        Log(FString::Printf(TEXT("%s: the set connected again with %d objects (UE '%s' / '%s')"), *What,
            NegotiatedObjects(), *Actor->GetConnectionStatus(), *SecondaryActor->GetConnectionStatus()));
        Test->TestEqual(*(What + TEXT(": both objects negotiate")), NegotiatedObjects(), 2);
        RefusalPhase = 0;
        return true;
    }

    /**
     * Inside linkage: a group without the second object and a second-object
     * display that drops out during commit are never shown as a half set.
     */
    void AddSetInjections()
    {
        Add(TEXT("[inject] dialog stage"), 30.0, [this]() { return SendOnce(TEXT("stage"), FString(), TEXT("set_injections")); });
        Add(TEXT("[inject] converge before the injections"), 120.0, [this]()
        {
            if (!SetTimeAndSettleOnce(OracleFrames[0] + 9.0))
            {
                return false;
            }
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (!View.bConverged || !FMath::IsNearlyEqual(View.AppliedDisplayFrame, OracleFrames[0] + 9.0, 0.01))
            {
                return false;
            }
            SettledFrame = -1.0;
            InjectApplied = View.AppliedEvalId;
            RecordWriterPose(InjectMainPose);
            RecordWriterPose(InjectSecondPose, true);
            return true;
        });
        Add(TEXT("[inject] Maya sends one group without the second object"), 120.0, [this]()
        {
            if (RefusalPhase == 0)
            {
                if (SendOnce(TEXT("drop_secondary_once")))
                {
                    RefusalPhase = 1;
                }
                return false;
            }
            if (RefusalPhase == 1)
            {
                if (SetTimeAndSettle(OracleFrames[0] + 21.0, 0.0))
                {
                    RefusalPhase = 2;
                    RefusalStarted = FPlatformTime::Seconds();
                }
                return false;
            }
            const TSharedPtr<FJsonObject>* Dropped = nullptr;
            if (!MayaResult->TryGetObjectField(TEXT("dropped_group"), Dropped))
            {
                return false;
            }
            DroppedEvalId = (*Dropped)->GetStringField(TEXT("eval_id"));
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (View.ErrorCode != TEXT("INVALID_MESSAGE") && FPlatformTime::Seconds() - RefusalStarted < 15.0)
            {
                return false;
            }
            Log(FString::Printf(TEXT("[inject] incomplete group %s: UE error %s '%s', applied %s @ %.1f, mode %s"),
                *DroppedEvalId.Right(12), *View.ErrorCode, *View.Error.Left(160), *View.AppliedEvalId.Right(12),
                View.AppliedDisplayFrame, *View.Mode));
            Test->TestEqual(TEXT("the incomplete group is refused as an invalid message"), View.ErrorCode, FString(TEXT("INVALID_MESSAGE")));
            Test->TestNotEqual(TEXT("the incomplete group is never applied"), View.AppliedEvalId, DroppedEvalId);
            if (View.AppliedEvalId == InjectApplied)
            {
                // Nothing newer was applied, so both objects still show the last complete group.
                Test->TestTrue(TEXT("the main character keeps the last complete pose"), PoseDelta(InjectMainPose, false) < 0.05);
                Test->TestTrue(TEXT("the second object keeps the last complete pose"), PoseDelta(InjectSecondPose, true) < 0.05);
            }
            Test->TestEqual(TEXT("linkage continues after the refused group"), View.Mode, FString(TEXT("linkage")));
            RefusalPhase = 0;
            return true;
        });
        Add(TEXT("[inject] converge again after the incomplete group"), 120.0, [this]()
        {
            if (!SetTimeAndSettleOnce(OracleFrames[0] + 33.0))
            {
                return false;
            }
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (!View.bConverged || !FMath::IsNearlyEqual(View.AppliedDisplayFrame, OracleFrames[0] + 33.0, 0.01))
            {
                return false;
            }
            SettledFrame = -1.0;
            InjectApplied = View.AppliedEvalId;
            RecordWriterPose(InjectMainPose);
            RecordWriterPose(InjectSecondPose, true);
            return true;
        });
        Add(TEXT("[inject] one object's subject is delayed under natural ticking"), 120.0, [this]()
        {
            const double Now = FPlatformTime::Seconds();
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (RefusalPhase == 0)
            {
                InjectMorphs = DisplayedMorphs();
                StartFrameWatch(TEXT("delayed second object"));
                DelayUntil = 0.0;
                MtoUSetLinkageApplyProbe([this](EMtoULinkageApplyStage Stage, const FString& ObjectId, const FString&)
                {
                    if (Stage != EMtoULinkageApplyStage::Published || ObjectId != SecondaryObjectId)
                    {
                        return true;
                    }
                    if (DelayUntil == 0.0)
                    {
                        // The first group after the time change: hold the second
                        // object's subject for part of the convergence window.
                        DelayUntil = FPlatformTime::Seconds() + 1.2;
                    }
                    ++HeldChecks;
                    return FPlatformTime::Seconds() >= DelayUntil;
                });
                HeldChecks = 0;
                MaxHeldPoseDelta = 0.0;
                MaxHeldMorphDelta = 0.0;
                bAppliedDuringDropout = false;
                RefusalPhase = 1;
                return false;
            }
            if (RefusalPhase == 1)
            {
                if (SetTimeAndSettle(OracleFrames[0] + 45.0, 0.0))
                {
                    RefusalPhase = 2;
                }
                return false;
            }
            if (DelayUntil == 0.0 || Now < DelayUntil)
            {
                // While one object waits, no display of either object may show the new group.
                bAppliedDuringDropout |= View.AppliedEvalId != InjectApplied;
                MaxHeldPoseDelta = FMath::Max3(MaxHeldPoseDelta,
                    PoseDelta(InjectMainPose, false), PoseDelta(InjectSecondPose, true));
                MaxHeldMorphDelta = FMath::Max(MaxHeldMorphDelta, MorphDelta(InjectMorphs));
                return false;
            }
            if (!View.bConverged || !FMath::IsNearlyEqual(View.AppliedDisplayFrame, OracleFrames[0] + 45.0, 0.01))
            {
                return false;
            }
            MtoUSetLinkageApplyProbe(nullptr);
            CheckFrameWatch();
            Log(FString::Printf(TEXT("[inject] delayed second object: held %d subject checks over 1.2 s; during the hold the displays moved at most %.4f cm and %.5f Morph weight, applied changed %d; then the whole set converged at %.1f"),
                HeldChecks, MaxHeldPoseDelta, MaxHeldMorphDelta, bAppliedDuringDropout ? 1 : 0, View.AppliedDisplayFrame));
            Test->TestTrue(TEXT("the delayed object was really held"), HeldChecks > 1);
            Test->TestFalse(TEXT("no group is confirmed while one object is delayed"), bAppliedDuringDropout);
            Test->TestTrue(TEXT("both objects keep the last complete bones while one is delayed"), MaxHeldPoseDelta < 0.05);
            Test->TestTrue(TEXT("both objects keep the last complete Morphs while one is delayed"), MaxHeldMorphDelta < 0.001);
            Test->TestTrue(TEXT("both objects show the new group once the delay ends"),
                PoseDelta(InjectMainPose, false) > 0.05 || PoseDelta(InjectSecondPose, true) > 0.05);
            SettledFrame = -1.0;
            InjectApplied = View.AppliedEvalId;
            RecordWriterPose(InjectMainPose);
            RecordWriterPose(InjectSecondPose, true);
            RefusalPhase = 0;
            return true;
        });
        Add(TEXT("[inject] a second-object display fails after the group was staged"), 120.0, [this]()
        {
            const FMtoULinkageView View = MtoUGetLinkageView();
            USkeletalMeshComponent* Display = SecondaryActor->GetSkeletalMeshComponent();
            if (RefusalPhase == 0)
            {
                InjectMorphs = DisplayedMorphs();
                StartFrameWatch(TEXT("display failed after staging"));
                BrokenGeneration.Reset();
                RefusalStarted = 0.0;
                MtoUSetLinkageApplyProbe([this, Display](EMtoULinkageApplyStage Stage, const FString&, const FString& Generation)
                {
                    if (Stage == EMtoULinkageApplyStage::Staged && BrokenGeneration.IsEmpty())
                    {
                        // Every display staged the group; the second object's
                        // display leaves the world before the commit.
                        BrokenGeneration = Generation;
                        WatchBreakFrame = WatchedFrames.Num();
                        Display->UnregisterComponent();
                    }
                    return true;
                });
                bAppliedDuringDropout = false;
                RefusalPhase = 1;
                return false;
            }
            if (RefusalPhase == 1)
            {
                if (SetTimeAndSettle(OracleFrames[0] + 57.0, 0.0))
                {
                    RefusalPhase = 2;
                }
                return false;
            }
            if (BrokenGeneration.IsEmpty())
            {
                return false;
            }
            bAppliedDuringDropout |= View.AppliedEvalId != InjectApplied;
            if (RefusalStarted == 0.0)
            {
                RefusalStarted = FPlatformTime::Seconds();
            }
            if (FPlatformTime::Seconds() - RefusalStarted < 1.0)
            {
                return false;
            }
            // The rollback put every display back on the last complete group.
            const double MainDelta = PoseDelta(InjectMainPose, false);
            const double MorphChange = MorphDelta(InjectMorphs, false);
            Log(FString::Printf(TEXT("[inject] display failed after staging group %s: applied changed %d, main display %.4f cm and %.5f Morph weight from the last complete group, mode %s, detail '%s'"),
                *BrokenGeneration.Left(40), bAppliedDuringDropout ? 1 : 0, MainDelta, MorphChange, *View.Mode, *View.Detail.Left(200)));
            Test->TestFalse(TEXT("the group whose display failed after staging is never confirmed"), bAppliedDuringDropout);
            Test->TestTrue(TEXT("the main character is rolled back to the last complete bones"), MainDelta < 0.05);
            Test->TestTrue(TEXT("the main character is rolled back to the last complete Morphs"), MorphChange < 0.001);
            MtoUSetLinkageApplyProbe(nullptr);
            Display->RegisterComponent();
            RefusalStarted = 0.0;
            RefusalPhase = 0;
            return true;
        });
        Add(TEXT("[inject] the restored display shows the whole set again"), 120.0, [this]()
        {
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (View.Mode != TEXT("linkage"))
            {
                Test->AddError(TEXT("linkage ended after the display failure: ") + View.ErrorCode + TEXT(" ") + View.Error);
                return true;
            }
            if (!SetTimeAndSettleOnce(OracleFrames[0] + 69.0))
            {
                return false;
            }
            if (!View.bConverged || !FMath::IsNearlyEqual(View.AppliedDisplayFrame, OracleFrames[0] + 69.0, 0.01))
            {
                return false;
            }
            SettledFrame = -1.0;
            Test->TestNotNull(TEXT("the restored display evaluates Live Link again"),
                Cast<ULiveLinkInstance>(SecondaryActor->GetSkeletalMeshComponent()->GetAnimInstance()));
            Log(FString::Printf(TEXT("[inject] the whole set converged again at %.1f"), View.AppliedDisplayFrame));
            CheckFrameWatch();
            return true;
        });
    }

    struct FWatchedDisplay
    {
        TWeakObjectPtr<USkeletalMeshComponent> Display;
        FString Label;
        TArray<int32> Bones;
    };
    struct FWatchedState
    {
        bool bRegistered = false;
        TArray<FVector> Bones;
        TMap<FName, float> Morphs;
    };

    /**
     * Watches every display of both objects (primary displays and parts) at
     * every frame end, which is when a frame renders, from an injection until
     * the set has converged on the next group. Each frame must show, on all
     * registered displays together, either the last complete group the watch
     * started on or the group the set converged on at its end: never a mix,
     * and the new group only once Unreal confirmed it.
     */
    void StartFrameWatch(const FString& Name)
    {
        StopFrameWatch();
        FrameWatchName = Name;
        WatchedDisplays.Reset();
        WatchedFrames.Reset();
        WatchedApplied.Reset();
        WatchBreakFrame = INDEX_NONE;
        for (const bool bSecondary : {false, true})
        {
            AMtoULiveLinkActor* Owner = bSecondary ? SecondaryActor.Get() : Actor.Get();
            if (!Owner)
            {
                continue;
            }
            TArray<USkeletalMeshComponent*> Displays;
            Displays.Add(Owner->GetSkeletalMeshComponent());
            for (const TObjectPtr<UMtoUCharacterPartComponent>& Part : Owner->GetCharacterPartComponents())
            {
                Displays.Add(Part.Get());
            }
            for (USkeletalMeshComponent* Display : Displays)
            {
                if (!Display || !Display->GetSkeletalMeshAsset())
                {
                    continue;
                }
                FWatchedDisplay& Entry = WatchedDisplays.AddDefaulted_GetRef();
                Entry.Display = Display;
                Entry.Label = FString::Printf(TEXT("%s/%s"), bSecondary ? TEXT("secondary") : TEXT("character"),
                    *Display->GetSkeletalMeshAsset()->GetName());
                // Up to 64 bones spread over the skeleton, plus every oracle bone.
                const int32 Count = Display->GetSkeletalMeshAsset()->GetRefSkeleton().GetNum();
                const int32 Stride = FMath::Max(1, Count / 64);
                for (int32 Bone = 0; Bone < Count; Bone += Stride)
                {
                    Entry.Bones.AddUnique(Bone);
                }
                for (const TSharedPtr<FJsonValue>& Joint : OracleJointsOf(bSecondary))
                {
                    const int32 Bone = Display->GetBoneIndex(FName(*Joint->AsString()));
                    if (Bone != INDEX_NONE)
                    {
                        Entry.Bones.AddUnique(Bone);
                    }
                }
            }
        }
        FrameWatch = FCoreDelegates::OnEndFrame.AddLambda([this]()
        {
            TArray<FWatchedState>& Frame = WatchedFrames.AddDefaulted_GetRef();
            for (const FWatchedDisplay& Entry : WatchedDisplays)
            {
                FWatchedState& State = Frame.AddDefaulted_GetRef();
                USkeletalMeshComponent* Display = Entry.Display.Get();
                State.bRegistered = Display && Display->IsRegistered();
                if (!State.bRegistered)
                {
                    continue;
                }
                const TArray<FTransform>& Space = Display->GetComponentSpaceTransforms();
                for (const int32 Bone : Entry.Bones)
                {
                    State.Bones.Add(Space.IsValidIndex(Bone) ? Space[Bone].GetLocation() : FVector::ZeroVector);
                }
                for (const TPair<const UMorphTarget*, int32>& Active : Display->ActiveMorphTargets)
                {
                    if (Active.Key && Display->MorphTargetWeights.IsValidIndex(Active.Value))
                    {
                        State.Morphs.Add(Active.Key->GetFName(), Display->MorphTargetWeights[Active.Value]);
                    }
                }
            }
            const FMtoULinkageView View = MtoUGetLinkageView();
            WatchedApplied.Add(FString::Printf(TEXT("%s#%d"), *View.AppliedEvalId, View.AppliedSampleSerial));
        });
    }

    void StopFrameWatch()
    {
        if (FrameWatch.IsValid())
        {
            FCoreDelegates::OnEndFrame.Remove(FrameWatch);
            FrameWatch.Reset();
        }
    }

    /** The largest bone (cm) and Morph difference of one display between two frames. */
    static void WatchedDifference(const FWatchedState& A, const FWatchedState& B, double& OutBones, double& OutMorphs)
    {
        OutBones = 0.0;
        OutMorphs = 0.0;
        for (int32 Index = 0; Index < FMath::Min(A.Bones.Num(), B.Bones.Num()); ++Index)
        {
            OutBones = FMath::Max(OutBones, FVector::Distance(A.Bones[Index], B.Bones[Index]));
        }
        for (const TPair<FName, float>& Morph : A.Morphs)
        {
            OutMorphs = FMath::Max(OutMorphs, double(FMath::Abs(Morph.Value - B.Morphs.FindRef(Morph.Key))));
        }
        for (const TPair<FName, float>& Morph : B.Morphs)
        {
            OutMorphs = FMath::Max(OutMorphs, double(FMath::Abs(Morph.Value - A.Morphs.FindRef(Morph.Key))));
        }
    }

    /**
     * Checks that every watched frame shows, on all registered displays, exactly
     * the group Unreal last confirmed: the one the watch started on until a new
     * group is confirmed, then that group. A group's own reference is its last
     * watched frame, when the set had settled on it.
     */
    void CheckFrameWatch()
    {
        StopFrameWatch();
        const FString Tag = TEXT("[frames] ") + FrameWatchName;
        if (!Test->TestTrue(*(Tag + TEXT(": frames were watched")), WatchedFrames.Num() > 2))
        {
            return;
        }
        TMap<FString, int32> Reference;
        for (int32 FrameIndex = 0; FrameIndex < WatchedFrames.Num(); ++FrameIndex)
        {
            Reference.Add(WatchedApplied[FrameIndex], FrameIndex);
        }
        constexpr double BoneTolerance = 0.05;
        constexpr double MorphTolerance = 0.001;
        int32 Off = 0, Unregistered = 0, StartFrames = 0;
        double PeakStartBones = 0.0, PeakStartMorphs = 0.0, PeakBones = 0.0, PeakMorphs = 0.0;
        TArray<double> ObjectPeak = {0.0, 0.0};
        TArray<FString> Groups;
        for (int32 FrameIndex = 0; FrameIndex < WatchedFrames.Num(); ++FrameIndex)
        {
            const FString& Applied = WatchedApplied[FrameIndex];
            Groups.AddUnique(Applied);
            const TArray<FWatchedState>& Frame = WatchedFrames[FrameIndex];
            const TArray<FWatchedState>& Shown = WatchedFrames[Reference[Applied]];
            const bool bStartGroup = Applied == WatchedApplied[0];
            StartFrames += bStartGroup ? 1 : 0;
            FString FirstOff;
            for (int32 Display = 0; Display < Frame.Num(); ++Display)
            {
                if (!Frame[Display].bRegistered || !Shown[Display].bRegistered)
                {
                    Unregistered += Frame[Display].bRegistered ? 0 : 1;
                    continue;
                }
                double Bones = 0.0, Morphs = 0.0;
                WatchedDifference(Frame[Display], Shown[Display], Bones, Morphs);
                PeakBones = FMath::Max(PeakBones, Bones);
                PeakMorphs = FMath::Max(PeakMorphs, Morphs);
                if (bStartGroup)
                {
                    PeakStartBones = FMath::Max(PeakStartBones, Bones);
                    PeakStartMorphs = FMath::Max(PeakStartMorphs, Morphs);
                    const int32 Object = WatchedDisplays[Display].Label.StartsWith(TEXT("secondary")) ? 1 : 0;
                    ObjectPeak[Object] = FMath::Max(ObjectPeak[Object], Bones);
                }
                if ((Bones >= BoneTolerance || Morphs >= MorphTolerance) && FirstOff.IsEmpty())
                {
                    FirstOff = FString::Printf(TEXT("%s is %.4f cm / %.5f Morph from its confirmed group"),
                        *WatchedDisplays[Display].Label, Bones, Morphs);
                }
            }
            if (!FirstOff.IsEmpty() && ++Off <= 5)
            {
                Log(FString::Printf(TEXT("%s: frame %d%s, confirmed %s: %s"), *Tag, FrameIndex,
                    FrameIndex == WatchBreakFrame ? TEXT(" (display failed)") : TEXT(""),
                    *Applied.Left(60), *FirstOff));
            }
        }
        Log(FString::Printf(TEXT("%s: %d frame ends over %d displays, %d confirmed groups (display failed at frame %d, %d unregistered display samples); %d frames before the next confirmed group stayed within %.4f cm (character %.4f, second object %.4f) and %.5f Morph of the start; every frame within %.4f cm and %.5f Morph of its confirmed group; %d frames off"),
            *Tag, WatchedFrames.Num(), WatchedDisplays.Num(), Groups.Num(), WatchBreakFrame, Unregistered, StartFrames,
            PeakStartBones, ObjectPeak[0], ObjectPeak[1], PeakStartMorphs, PeakBones, PeakMorphs, Off));
        Test->TestTrue(*(Tag + TEXT(": a new group is confirmed while watched")), Groups.Num() > 1 && StartFrames > 1);
        Test->TestEqual(*(Tag + TEXT(": every frame shows exactly the last confirmed group on every display")), Off, 0);
    }

    /** Every Morph weight the latest Maya snapshot names, as each object's displays show it. */
    TMap<FString, double> DisplayedMorphs() const
    {
        TMap<FString, double> Out;
        if (!MayaResult.IsValid() || !MayaResult->HasTypedField<EJson::Object>(TEXT("snapshot")))
        {
            return Out;
        }
        const TSharedPtr<FJsonObject> Snapshot = MayaResult->GetObjectField(TEXT("snapshot"));
        for (const bool bSecondary : {false, true})
        {
            const AMtoULiveLinkActor* Owner = bSecondary ? SecondaryActor.Get() : Actor.Get();
            const TSharedPtr<FJsonObject> Row = SnapshotRow(Snapshot, bSecondary);
            const TSharedPtr<FJsonObject>* Morphs = nullptr;
            if (!Owner || !Row.IsValid() || !Row->TryGetObjectField(TEXT("morphs"), Morphs))
            {
                continue;
            }
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Morph : (*Morphs)->Values)
            {
                const TArray<double> Weights = MtoUGuiAcceptance::DisplayedMorphWeights(*Owner, FName(*Morph.Key));
                if (!Weights.IsEmpty())
                {
                    Out.Add((bSecondary ? TEXT("secondary/") : TEXT("character/")) + Morph.Key, Weights[0]);
                }
            }
        }
        return Out;
    }

    /** The largest change of the recorded Morph weights; the main character's only when asked. */
    double MorphDelta(const TMap<FString, double>& Before, bool bWithSecondary = true) const
    {
        double Delta = 0.0;
        const TMap<FString, double> Now = DisplayedMorphs();
        for (const TPair<FString, double>& Entry : Before)
        {
            if (!bWithSecondary && Entry.Key.StartsWith(TEXT("secondary/")))
            {
                continue;
            }
            if (const double* Current = Now.Find(Entry.Key))
            {
                Delta = FMath::Max(Delta, FMath::Abs(*Current - Entry.Value));
            }
        }
        return Delta;
    }

    /**
     * Removes the second object inside linkage from Maya or from Unreal; the
     * main character keeps linkage, then the object joins again by reconnecting.
     */
    void AddRemoval(const FString& Scenario, const FString& Tag)
    {
        const bool bFromUnreal = Scenario == TEXT("ue_remove_secondary");
        Add(Tag + TEXT("converge before the removal"), 120.0, [this]()
        {
            if (!SetTimeAndSettleOnce(OracleFrames[1]))
            {
                return false;
            }
            const FMtoULinkageView View = MtoUGetLinkageView();
            return View.bConverged && FMath::IsNearlyEqual(View.AppliedDisplayFrame, OracleFrames[1], 0.01);
        });
        Add(Tag + TEXT("remove the second object"), 120.0, [this, bFromUnreal, Tag]()
        {
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (RefusalPhase == 0)
            {
                SettledFrame = -1.0;
                RemovalStarted = FPlatformTime::Seconds();
                if (bFromUnreal)
                {
                    FString Error;
                    Test->TestTrue(*(Tag + TEXT("Unreal's 移除副对象 is accepted")), MtoURequestRemoveSecondaryObject(Error));
                    RefusalPhase = 2;
                }
                else
                {
                    RefusalPhase = 1;
                }
                return false;
            }
            if (RefusalPhase == 1)
            {
                if (SendOnce(TEXT("remove_secondary")))
                {
                    RefusalPhase = 2;
                }
                return false;
            }
            const TSharedPtr<FJsonObject> Status = MayaStatus();
            bool bMayaActive = false;
            if (NegotiatedObjects() != 1 || View.bRemovingObject || MayaSetHasSecondary()
                || !Status.IsValid() || !Status->TryGetBoolField(TEXT("active"), bMayaActive) || !bMayaActive)
            {
                return false;
            }
            Log(FString::Printf(TEXT("%sremoval confirmed in %.2f s: UE mode %s, set revision %d, detail '%s'"),
                *Tag, FPlatformTime::Seconds() - RemovalStarted, *View.Mode, View.Identity.SetRevision, *View.Detail));
            Test->TestEqual(*(Tag + TEXT("linkage continues after the removal")), View.Mode, FString(TEXT("linkage")));
            Test->TestTrue(*(Tag + TEXT("the main target stays taken over")),
                WritersMutedIn(WriterTracks, true, *(Tag + TEXT("main after the removal"))));
            Test->TestTrue(*(Tag + TEXT("only the second target's writers are handed back")),
                WritersMutedIn(SecondaryWriterTracks, false, *(Tag + TEXT("second after the removal"))));
            // The Binding Actor's display only ever shows Live Link; out of the
            // set it shows its Binding without a stream, like a disconnected actor.
            Test->TestEqual(*(Tag + TEXT("the second actor says it left the set")),
                SecondaryActor->GetConnectionStatus(), FString(TEXT("Removed from the set")));
            Test->TestTrue(*(Tag + TEXT("the restore left nothing behind")), View.RecoveryResidual.IsEmpty());
            RefusalPhase = 0;
            return true;
        });
        Add(Tag + TEXT("Details renders after the removal"), 60.0, [this, Scenario]()
        {
            return RenderDetails(Scenario + TEXT("-removed"));
        });
        Add(Tag + TEXT("the main character keeps following alone"), 120.0, [this, Tag]()
        {
            if (!SetTimeAndSettleOnce(OracleFrames[0]))
            {
                return false;
            }
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (!View.bConverged || !FMath::IsNearlyEqual(View.AppliedDisplayFrame, OracleFrames[0], 0.01))
            {
                return false;
            }
            if (SnapshotId < 0)
            {
                SnapshotId = SendMaya(TEXT("snapshot"));
                return false;
            }
            if (!MayaAcked(SnapshotId))
            {
                return false;
            }
            SnapshotId = -1;
            SettledFrame = -1.0;
            CompareOracle(0, false);
            return true;
        });
        Add(Tag + TEXT("end linkage with the main character only"), 120.0, [this, Tag]()
        {
            const FMtoULinkageView View = MtoUGetLinkageView();
            if (LeaveIssued != Tag)
            {
                LeaveIssued = Tag;
                FString Error;
                Test->TestTrue(*(Tag + TEXT("结束预览 is accepted")),
                    MtoURequestLinkageLeave(TEXT("end"), View.Identity.ModeRevision + 1, Error));
                return false;
            }
            if (View.Mode != TEXT("realtime") || View.bTransitioning || View.bTakeoverHeld)
            {
                return false;
            }
            LeaveIssued.Reset();
            return WritersMuted(false, *(Tag + TEXT("after 结束预览")));
        });
        Add(Tag + TEXT("the second object joins again"), 600.0, [this, Tag]()
        {
            if (RefusalPhase == 0)
            {
                // 设为副对象 changes the set, which ends the connection.
                if (SendOnce(TEXT("set_secondary")))
                {
                    RefusalPhase = 1;
                }
                return false;
            }
            if (RefusalPhase == 1)
            {
                if (MayaConnected() && !MayaSetHasSecondary())
                {
                    return false;
                }
                if (MayaConnected() && NegotiatedObjects() == 2)
                {
                    RefusalPhase = 3;
                    return false;
                }
                if (!MayaConnected())
                {
                    RefusalPhase = 2;
                }
                return false;
            }
            if (RefusalPhase == 2)
            {
                if (SendOnce(TEXT("connect")))
                {
                    RefusalPhase = 3;
                }
                return false;
            }
            if (!SetConnected())
            {
                return false;
            }
            PairAgain();
            RefusalPhase = 0;
            Log(Tag + TEXT("the second object joined again after reconnecting"));
            Test->TestEqual(*(Tag + TEXT("the set has both objects again")), NegotiatedObjects(), 2);
            return true;
        });
        Add(Tag + TEXT("the original writers drive both targets again"), 120.0, [this, Scenario, Tag]()
        {
            return RecoverAfter(Scenario, Tag);
        });
    }

    void AddExit(const FString& Scenario, const FString& Tag)
    {
        Add(Tag + TEXT("leave"), 120.0, [this, Scenario]() { return Leave(Scenario); });
        Add(Tag + TEXT("both hosts restored"), 120.0, [this, Scenario, Tag]()
        {
            const FMtoULinkageView View = MtoUGetLinkageView();
            const TSharedPtr<FJsonObject> Status = MayaStatus();
            bool bMayaActive = true;
            if (View.Mode != TEXT("realtime") || View.bTransitioning || !Status.IsValid()
                || !Status->TryGetBoolField(TEXT("active"), bMayaActive) || bMayaActive)
            {
                return false;
            }
            const bool bKeepsTakeover = Scenario == TEXT("realtime");
            if (View.bTakeoverHeld != bKeepsTakeover)
            {
                return false;
            }
            Test->TestTrue(*(Tag + TEXT("writers match the exit")),
                WritersMuted(bKeepsTakeover, *(Tag + TEXT("after the exit"))));
            const double MayaTime = MayaResult->GetNumberField(TEXT("maya_time"));
            Test->TestTrue(*(Tag + TEXT("Maya returns to the frame linkage started on")),
                FMath::IsNearlyEqual(MayaTime, MayaTimeBefore, 1e-4));
            Test->TestEqual(*(Tag + TEXT("Maya looks through its own camera again")),
                MayaResult->GetStringField(TEXT("panel_camera")), MayaCameraBefore);
            bool bCamera = true;
            MayaResult->TryGetBoolField(TEXT("link_camera_exists"), bCamera);
            Test->TestFalse(*(Tag + TEXT("Maya's link camera is gone")), bCamera);
            Log(FString::Printf(TEXT("%s exit: UE mode %s revision %d takeover %d, error '%s', detail '%s'"),
                *Scenario, *View.Mode, View.Identity.ModeRevision, View.bTakeoverHeld ? 1 : 0,
                *View.ErrorCode, *View.Detail));
            if (bKeepsTakeover)
            {
                // 返回实时 keeps Unreal's takeover; 结束预览 from Maya returns the
                // writers before the next scenario.
                EndAfterRealtime = SendMaya(TEXT("leave"), TEXT("end"));
            }
            return true;
        });
        if (Scenario == TEXT("realtime"))
        {
            Add(Tag + TEXT("结束预览 after 返回实时 returns the writers"), 60.0, [this, Tag]()
            {
                if (!MayaAcked(EndAfterRealtime) || MtoUGetLinkageView().bTakeoverHeld)
                {
                    return false;
                }
                Test->TestTrue(*(Tag + TEXT("writers are live again")), WritersMuted(false, *(Tag + TEXT("after 结束预览"))));
                return true;
            });
        }
        Add(Tag + TEXT("the original writers drive the target again"), 120.0, [this, Scenario, Tag]()
        {
            return RecoverAfter(Scenario, Tag);
        });
    }

    bool Leave(const FString& Scenario)
    {
        if (LeaveIssued == Scenario)
        {
            return true;
        }
        LeaveIssued = Scenario;
        const FMtoULinkageView View = MtoUGetLinkageView();
        FString Error;
        if (Scenario == TEXT("realtime") || Scenario == TEXT("end") || Scenario == TEXT("arms"))
        {
            // The arms scenario ends its preview the way 结束预览 does.
            Test->TestTrue(TEXT("the panel exit is accepted"),
                MtoURequestLinkageLeave(Scenario == TEXT("arms") ? TEXT("end") : *Scenario,
                    View.Identity.ModeRevision + 1, Error));
        }
        else if (Scenario == TEXT("maya_end"))
        {
            SendMaya(TEXT("leave"), TEXT("end"));
        }
        else if (Scenario == TEXT("close_sequencer"))
        {
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(MainCopy.Get());
            SequencerWeak.Reset();
        }
        else if (Scenario == TEXT("switch_sequence"))
        {
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(SwitchSequence);
        }
        else if (Scenario == TEXT("destroy_target"))
        {
            WorldPtr->DestroyActor(Actor.Get());
        }
        else if (Scenario == TEXT("destroy_secondary"))
        {
            WorldPtr->DestroyActor(SecondaryActor.Get());
        }
        else if (Scenario == TEXT("close_world"))
        {
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(MainCopy.Get());
            SequencerWeak.Reset();
            GEditor->NewMap();
        }
        else
        {
            Test->AddError(TEXT("Unknown scenario ") + Scenario);
        }
        return true;
    }

    /** After the exit, the scene is reopened as an artist would and checked. */
    bool RecoverAfter(const FString& Scenario, const FString& Tag)
    {
        if (Scenario == TEXT("close_world"))
        {
            return true;
        }
        if (Scenario == TEXT("destroy_target") || Scenario == TEXT("destroy_secondary"))
        {
            // The deleted target ended the streaming session too; the artist
            // places the actor again and reconnects from Maya.
            const bool bSecondary = Scenario == TEXT("destroy_secondary");
            if (!(bSecondary ? SecondaryActor : Actor).IsValid())
            {
                if (!SpawnTarget(bSecondary))
                {
                    return true;
                }
                if (HasSecondary())
                {
                    // A set reconnects only once every object has its target again.
                    PairTargets();
                }
                Reconnected = false;
                return false;
            }
            bool bConnected = false;
            MayaResult->TryGetBoolField(TEXT("connected"), bConnected);
            if (!SetConnected())
            {
                if (!bConnected && ReconnectId < 0)
                {
                    ReconnectId = SendMaya(TEXT("connect"));
                }
                return false;
            }
            if (!Reconnected)
            {
                Reconnected = true;
                ReconnectId = -1;
                PairAgain();
                FMtoULinkageEditor::SetObservedLinkageActor(Actor.Get());
                WriterTracks.Reset();
                WriteInventory();
            }
        }
        const TSharedPtr<ISequencer> Sequencer = SequencerWeak.Pin();
        if (!Sequencer.IsValid() || Sequencer->GetRootMovieSceneSequence() != MainCopy.Get())
        {
            OpenMain();
            return false;
        }
        if (!SetTimeAndSettle(OracleFrames[0], 0.5))
        {
            return false;
        }
        const double MaxDelta = PoseDelta(BeforePose, false);
        Log(FString::Printf(TEXT("%s: the sequence animation drives the target again, max delta %.4f cm from before the takeover"),
            *Scenario, MaxDelta));
        Test->TestTrue(*(Tag + TEXT("the original animation writer shows its own pose again")), MaxDelta < 0.05);
        if (HasSecondary())
        {
            const double SecondDelta = PoseDelta(SecondaryBeforePose, true);
            Log(FString::Printf(TEXT("%s: the sequence animation drives the second target again, max delta %.4f cm from before the takeover"),
                *Scenario, SecondDelta));
            Test->TestTrue(*(Tag + TEXT("the second target's own writers show their pose again")), SecondDelta < 0.05);
        }
        LeaveIssued.Reset();
        return true;
    }

    void Finish()
    {
        StopFrameWatch();
        MtoUSetLinkageApplyProbe(nullptr);
        RestoreLoopMode();
        for (const FString& Package : ProtectedPackages)
        {
            const UPackage* Loaded = FindPackage(nullptr, *Package);
            Test->TestFalse(*FString::Printf(TEXT("%s is never modified"), *Package),
                Loaded && Loaded->IsDirty() && Package != (WorldPtr.IsValid() ? WorldPtr->GetPackage()->GetName() : FString()));
        }
        for (const TWeakObjectPtr<UMovieSceneTrack>& Weak : WriterTracks)
        {
            if (const UMovieSceneTrack* Track = Weak.Get())
            {
                Test->TestFalse(TEXT("no writer stays muted"), Track->IsLocalEvalDisabled());
            }
        }
        for (const TWeakObjectPtr<UMovieSceneTrack>& Weak : SecondaryWriterTracks)
        {
            if (const UMovieSceneTrack* Track = Weak.Get())
            {
                Test->TestFalse(TEXT("no writer of the second target stays muted"), Track->IsLocalEvalDisabled());
            }
        }
        if (MayaResult.IsValid())
        {
            MayaResult = Driver.Poll();
            // Only the connect-time Morph mismatch of the fixture, the exits
            // that end the streaming session and the set refusals the test
            // provokes may show a dialog.
            MtoUGuiAcceptance::CheckMayaDialogs(*Test, MayaResult,
                {{TEXT("startup"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("destroy_target"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("destroy_target"), TEXT("STREAM_INTERRUPTED")},
                 {TEXT("close_world"), TEXT("STREAM_INTERRUPTED")},
                 {TEXT("set_refusals"), TEXT("ROLE_SETUP_FAILED")},
                 {TEXT("set_refusals"), TEXT("ANCHOR_MISMATCH")},
                 {TEXT("wrong_skeleton"), TEXT("SKELETON_MISMATCH")},
                 {TEXT("wrong_skeleton"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("composition_swaps"), TEXT("STREAM_INTERRUPTED")},
                 {TEXT("composition_swaps"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("arms"), TEXT("STREAM_INTERRUPTED")},
                 {TEXT("arms"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("reference"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("set_injections"), TEXT("INVALID_MESSAGE")},
                 {TEXT("remove_secondary"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("ue_remove_secondary"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("destroy_secondary"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("destroy_secondary"), TEXT("STREAM_INTERRUPTED")},
                 {TEXT("lifecycle"), TEXT("BLENDSHAPE_MISMATCH")},
                 {TEXT("lifecycle"), TEXT("STREAM_INTERRUPTED")},
                 {TEXT("lifecycle"), TEXT("RECOVERY_REFUSED")},
                 {TEXT("lifecycle_check"), TEXT("BLENDSHAPE_MISMATCH")}},
                [this](const FString& Line) { Log(Line); });
            Evidence->SetObjectField(TEXT("maya_final"), MayaResult);
        }
        WriteJsonFile(FPaths::Combine(EvidenceDir, TEXT("ue-run.json")), Evidence.ToSharedRef());
    }

    FAutomationTestBase* Test;
    FString FixturePath, EvidenceDir;
    TSharedPtr<FJsonObject> Fixture, MayaResult, Evidence;
    TArray<FStep> Steps;
    int32 Step = 0;
    double StepStarted = 0.0;
    int32 LogIndex = 0, ReportedMayaErrors = 0;
    MtoUGuiAcceptance::FMayaGuiDriver Driver;
    TWeakObjectPtr<UWorld> WorldPtr;
    ULevelSequence* MainOriginal = nullptr;
    ULevelSequence* ShotOriginal = nullptr;
    ULevelSequence* SwitchSequence = nullptr;
    TStrongObjectPtr<ULevelSequence> MainCopy, ShotCopy;
    TStrongObjectPtr<UMtoULiveLinkBinding> Binding;
    TWeakObjectPtr<AMtoULiveLinkActor> Actor;
    TWeakPtr<ISequencer> SequencerWeak;
    FGuid TargetGuid;
    FTransform AnchorTransform;
    TArray<double> OracleFrames;
    struct FOracleRotation
    {
        FQuat Maya;
        FQuat Shown;
    };
    TMap<int32, TMap<FName, FOracleRotation>> OracleRotations;
    TArray<TWeakObjectPtr<UMovieSceneTrack>> WriterTracks;
    TArray<FString> ProtectedPackages;
    TMap<FName, FVector> BeforePose;
    // The second object of the set, when the fixture names one.
    TStrongObjectPtr<UMtoULiveLinkBinding> SecondaryBinding, WrongBinding;
    TWeakObjectPtr<AMtoULiveLinkActor> SecondaryActor;
    FGuid SecondaryGuid;
    TArray<TWeakObjectPtr<UMovieSceneTrack>> SecondaryWriterTracks;
    TMap<FName, FVector> SecondaryBeforePose, InjectMainPose, InjectSecondPose;
    TMap<int32, TMap<FName, FOracleRotation>> SecondaryOracleRotations;
    bool bSecondaryAnchorPinned = false;
    bool bMayaStarted = false;
    FTransform ProbeTransform;
    // The composition a swap replaced, put back afterwards.
    TObjectPtr<USkeletalMesh> SavedMesh;
    TStrongObjectPtr<USkeletalMesh> ArmsMesh, FullBodyMesh;
    TArray<FName> ArmsRequired, FullRequired;
    struct FRequiredBones
    {
        FString Key;
        TSet<FName> Bones;
    };
    FRequiredBones RequiredCache[2];
    TArray<FMtoUCharacterPart> SavedParts;
    // The apply probe's injections inside linkage.
    double DelayUntil = 0.0, MaxHeldPoseDelta = 0.0, MaxHeldMorphDelta = 0.0;
    int32 HeldChecks = 0;
    FString BrokenGeneration;
    TMap<FString, double> InjectMorphs;
    FString FrameWatchName;
    FDelegateHandle FrameWatch;
    TArray<FWatchedDisplay> WatchedDisplays;
    TArray<TArray<FWatchedState>> WatchedFrames;
    TArray<FString> WatchedApplied;
    int32 WatchBreakFrame = INDEX_NONE;
    bool bAbort = false;
    bool bDroppedShown = false, bDisconnectSent = false, bAppliedDuringDropout = false;
    FString LastSecondaryStatus, DroppedEvalId, InjectApplied, RefusalText;
    int32 OnceId = -1, RefusalPhase = 0, DialogsBefore = 0, LeaveAfterRefusal = -1;
    double RefusalStarted = 0.0, RemovalStarted = 0.0;
    double SettleUntil = 0.0, SettledFrame = -1.0, MorphWaitStarted = 0.0;
    int32 EnterRequested = -1, StageId = -1, SnapshotId = -1, ShotId = -1, QuitId = -1, EndAfterRealtime = -1;
    double MayaTimeBefore = 0.0;
    FString MayaCameraBefore, LeaveIssued, LastApplied;
    double PlayStarted = 0.0, MaxLag = 0.0;
    int32 PlayTicks = 0;
    int32 PluginSampleId = -1;
    TArray<double> AppliedFrames;
    bool bStopped = false;
    // Lifecycle census (INT-09).
    int32 LifecyclePhase = 0, LifecycleId = -1;
    TSharedPtr<FJsonObject> LifecycleBaseline, LifecycleLastCensus, LifecycleDisconnected;
    TArray<TSharedPtr<FJsonValue>> LifecycleSamples;
    // Sustained playback (INT-10).
    int32 SustainedPhase = 0;
    double SustainedStart = 0.0, SustainedPausedAt = 0.0, SustainedJumpedAt = 0.0, SustainedJumpFrame = 0.0;
    double SustainedPauseConvergence = 0.0, SustainedJumpConvergence = 0.0;
    uint64 SustainedUePeak = 0, SustainedMayaPeak = 0, SustainedUeStart = 0, SustainedMayaStart = 0;
    TMap<FString, double> HeldSince;
    int32 SustainedFailures = 0;
    FString SustainedLastError;
    FMtoULinkageScheduleStats SustainedPlayRecords;
    TArray<TSharedPtr<FJsonValue>> SustainedTiers;
    ESequencerLoopMode LoopModeBefore = ESequencerLoopMode::SLM_NoLoop;
    bool bLoopModeChanged = false;
    bool Reconnected = true;
    FTransform LinkageAnchor;
    bool bAnchorKeyed = false;
    bool bAnchorMoved = false;
    FString LastStatus;
    FDelegateHandle CleanupHandle;
    int32 ReconnectId = -1;
};
}

/**
 * A production project's own content may log errors when it loads (for example
 * Blueprints that reference a game module the project does not build); only
 * this test's own assertions decide its result, and the log stays in the
 * report.
 */
class FMtoUProductionContentTestBase : public FAutomationTestBase
{
public:
    using FAutomationTestBase::FAutomationTestBase;
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }
};

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FMtoULinkageProductionSequenceTest,
    FMtoUProductionContentTestBase,
    "MtoUOptIn.Editor.Linkage.ProductionSequence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoULinkageProductionSequenceTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FString Fixture;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MtoULinkageProductionFixture="), Fixture))
    {
        AddError(TEXT("Production linkage requires -MtoULinkageProductionFixture=<fixture.json>."));
        return false;
    }
    FAutomationTestFramework::Get().EnqueueLatentCommand(
        MakeShared<FProductionLinkageCommand>(this, Fixture));
    return true;
}
#endif
