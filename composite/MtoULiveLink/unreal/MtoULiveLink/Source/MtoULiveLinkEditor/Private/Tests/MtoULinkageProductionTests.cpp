#if WITH_DEV_AUTOMATION_TESTS
#include "MtoULinkage.h"
#include "MtoULinkageEditor.h"
#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"

#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "CineCameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "LiveLinkInstance.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
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
 */
namespace
{
TSharedPtr<FJsonObject> ReadJsonFile(const FString& Path)
{
    FString Text;
    TSharedPtr<FJsonObject> Value;
    if (FFileHelper::LoadFileToString(Text, *Path))
    {
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Value);
    }
    return Value;
}

void WriteJsonFile(const FString& Path, const TSharedRef<FJsonObject>& Value)
{
    FString Text;
    FJsonSerializer::Serialize(Value, TJsonWriterFactory<>::Create(&Text));
    FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

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
        if (Maya.IsValid())
        {
            if (FPlatformProcess::IsProcRunning(Maya))
            {
                FPlatformProcess::TerminateProc(Maya, true);
            }
            FPlatformProcess::CloseProc(Maya);
        }
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
        if (Step >= Steps.Num())
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
        if (const TSharedPtr<FJsonObject> Fresh = ReadJsonFile(MayaResultPath))
        {
            MayaResult = Fresh;
        }
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
        if (!MayaResult.IsValid() && Step >= 2)
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
        TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>();
        Command->SetNumberField(TEXT("id"), ++CommandId);
        Command->SetStringField(TEXT("action"), Action);
        Command->SetStringField(TEXT("target"), Target);
        Command->SetStringField(TEXT("name"), Name);
        WriteJsonFile(MayaCommandPath, Command);
        return CommandId;
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
        MayaCommandPath = FPaths::Combine(EvidenceDir, TEXT("maya-command.json"));
        MayaResultPath = FPaths::Combine(EvidenceDir, TEXT("maya-result.json"));
        FFileHelper::SaveStringToFile(TEXT("{}"), *MayaCommandPath);
        FFileHelper::SaveStringToFile(TEXT("{\"phase\":\"not-started\"}"), *MayaResultPath);
        Evidence = MakeShared<FJsonObject>();
        CleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda(
            [this](UWorld* World, bool, bool)
            {
                Log(FString::Printf(TEXT("world cleanup: %s (type %d)"),
                    World ? *World->GetPathName() : TEXT("none"), World ? static_cast<int32>(World->WorldType) : -1));
            });

        // ---- level, sequence copies and the rebound target ----------------
        Add(TEXT("load level and build the sequence copies"), 300.0, [this]() { return BuildScene(); });
        Add(TEXT("start Maya through the public entry"), 30.0, [this]() { return StartMaya(); });
        Add(TEXT("Maya connects and the character negotiates"), 600.0, [this]()
        {
            bool bConnected = false;
            return MayaResult.IsValid() && MayaResult->TryGetBoolField(TEXT("connected"), bConnected)
                && bConnected && Actor.IsValid()
                && Actor->GetConnectionStatus().StartsWith(TEXT("Connected"));
        });
        Add(TEXT("pair the target and open the sequence copy"), 60.0, [this]() { return PairAndOpen(); });
        Add(TEXT("record the sequence-driven pose before any takeover"), 60.0, [this]()
        {
            if (!SetTimeAndSettle(OracleFrames[0], 0.5) || !RecordWriterPose(BeforePose))
            {
                return false;
            }
            Log(TEXT("sequence-driven actor transform before takeover ")
                + Actor->GetActorTransform().ToHumanReadableString());
            return true;
        });

        const TArray<TSharedPtr<FJsonValue>>& Scenarios = Fixture->GetArrayField(TEXT("scenarios"));
        for (int32 Index = 0; Index < Scenarios.Num(); ++Index)
        {
            AddScenario(Scenarios[Index]->AsString(), Index);
        }
        Add(TEXT("quit Maya"), 120.0, [this]()
        {
            if (QuitId < 0)
            {
                QuitId = SendMaya(TEXT("quit"));
            }
            return !FPlatformProcess::IsProcRunning(Maya);
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
        UMtoULiveLinkBinding* BindingAsset = LoadObject<UMtoULiveLinkBinding>(nullptr, *Fixture->GetStringField(TEXT("binding")));
        if (!MainOriginal || !ShotOriginal || !SwitchSequence || !BindingAsset)
        {
            Test->AddError(TEXT("Fixture sequence or binding did not load"));
            return false;
        }
        Binding.Reset(DuplicateObject<UMtoULiveLinkBinding>(BindingAsset, GetTransientPackage()));
        MainCopy.Reset(TempCopy(MainOriginal, TEXT("Main")));
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
        return SpawnTarget();
    }

    bool SpawnTarget()
    {
        UWorld* World = WorldPtr.Get();
        const TSharedPtr<FJsonObject> Anchor = Fixture->GetObjectField(TEXT("anchor"));
        const FVector Location = ReadVector(Anchor, TEXT("location"));
        const FVector Euler = ReadVector(Anchor, TEXT("rotation"));
        AnchorTransform = FTransform(FRotator(Euler.Y, Euler.Z, Euler.X), Location);
        // Placed like an artist's actor; the level is never saved.
        FActorSpawnParameters Params;
        AMtoULiveLinkActor* Spawned = World->SpawnActor<AMtoULiveLinkActor>(
            Location, AnchorTransform.Rotator(), Params);
        Spawned->SetActorLabel(TEXT("MtoU Linkage Acceptance (unsaved)"));
        Spawned->SetBinding(Binding.Get());
        Actor = Spawned;

        // The character's binding in the shot copy now possesses the Binding
        // Actor: its existing animation and transform writers drive it.
        const FString TargetName = Fixture->GetStringField(TEXT("target_binding"));
        UMovieScene* MovieScene = ShotCopy->GetMovieScene();
        FGuid Guid;
        for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
        {
            FMovieScenePossessable& Possessable = MovieScene->GetPossessable(Index);
            if (Possessable.GetName() == TargetName)
            {
                Guid = Possessable.GetGuid();
                Possessable.SetPossessedObjectClass(AMtoULiveLinkActor::StaticClass());
            }
        }
        if (!Test->TestTrue(TEXT("the shot copy has the character binding"), Guid.IsValid()))
        {
            return false;
        }
        TargetGuid = Guid;
        if (!bAnchorKeyed)
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
        const TSharedPtr<FJsonObject> MayaFixture = Fixture->GetObjectField(TEXT("maya"));
        MayaFixture->SetStringField(TEXT("command"), MayaCommandPath);
        MayaFixture->SetStringField(TEXT("result"), MayaResultPath);
        MayaFixture->SetStringField(TEXT("evidence"), EvidenceDir);
        const FString DriverFixture = FPaths::Combine(EvidenceDir, TEXT("maya-driver-fixture.json"));
        WriteJsonFile(DriverFixture, MayaFixture.ToSharedRef());
        FPlatformMisc::SetEnvironmentVar(TEXT("MTOU_LINKAGE_DRIVER_FIXTURE"), *DriverFixture);
        FPlatformMisc::SetEnvironmentVar(TEXT("MAYA_APP_DIR"), *MayaFixture->GetStringField(TEXT("app_dir")));
        const FString Driver = MayaFixture->GetStringField(TEXT("driver"));
        const FString Args = FString::Printf(
            TEXT("-log \"%s\" -command \"python(\\\"import runpy; runpy.run_path(r'%s')\\\")\""),
            *FPaths::Combine(EvidenceDir, TEXT("maya.log")), *Driver);
        Maya = FPlatformProcess::CreateProc(*MayaFixture->GetStringField(TEXT("exe")), *Args,
            true, true, false, nullptr, 0, nullptr, nullptr);
        if (!Maya.IsValid())
        {
            Test->AddError(TEXT("Could not start Maya"));
            return false;
        }
        return true;
    }

    bool PairAndOpen()
    {
        FString Error;
        if (!Test->TestTrue(TEXT("the Binding Actor pairs with the Maya object"),
                MtoUPairLinkageTarget(*Actor, Fixture->GetStringField(TEXT("object_id")), Error)))
        {
            Test->AddError(Error);
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

    /** World positions of the oracle bones on the primary display. */
    bool RecordWriterPose(TMap<FName, FVector>& Out)
    {
        Out.Reset();
        USkeletalMeshComponent* Primary = Actor->GetSkeletalMeshComponent();
        for (const TSharedPtr<FJsonValue>& Name : Fixture->GetObjectField(TEXT("maya"))->GetArrayField(TEXT("oracle_joints")))
        {
            const FName Bone(*Name->AsString());
            if (Primary->GetBoneIndex(Bone) != INDEX_NONE)
            {
                Out.Add(Bone, Primary->GetSocketLocation(Bone));
            }
        }
        return Test->TestTrue(TEXT("the primary display owns the oracle bones"), Out.Num() >= 3);
    }

    bool WritersMuted(bool bExpected, const TCHAR* What)
    {
        int32 Live = 0;
        int32 Muted = 0;
        for (const TWeakObjectPtr<UMovieSceneTrack>& Weak : WriterTracks)
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
        return bExpected ? (Muted == WriterTracks.Num() && Muted > 0) : Muted == 0;
    }

    // -------------------------------------------------------------- scenarios

    void AddScenario(const FString& Scenario, int32 Index)
    {
        const FString Tag = FString::Printf(TEXT("[%s] "), *Scenario);
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
            return true;
        });
        if (Index == 0)
        {
            for (int32 Frame = 0; Frame < Fixture->GetArrayField(TEXT("oracle_frames")).Num(); ++Frame)
            {
                AddOracle(Frame);
            }
            AddPlayback();
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
        }
        AddExit(Scenario, Tag);
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
            SnapshotId = -1;
            SettledFrame = -1.0;
            CompareOracle(FrameIndex);
            return true;
        });
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

    void CompareOracle(int32 FrameIndex)
    {
        const TSharedPtr<FJsonObject> Snapshot = MayaResult->GetObjectField(TEXT("snapshot"));
        const TSharedPtr<FJsonObject> Joints = Snapshot->GetObjectField(TEXT("joints"));
        const TSharedPtr<ISequencer> Sequencer = SequencerWeak.Pin();
        UCameraComponent* Camera = Sequencer.IsValid() ? Sequencer->GetLastEvaluatedCameraCut().Get() : nullptr;
        FMinimalViewInfo View;
        if (Camera)
        {
            Camera->GetCameraView(0.0f, View);
        }
        USkeletalMeshComponent* Primary = Actor->GetSkeletalMeshComponent();
        double MaxWorld = 0.0;
        double MaxScreen = 0.0;
        int32 Compared = 0;
        TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Joint : Joints->Values)
        {
            const FName Bone(*Joint.Key);
            if (Primary->GetBoneIndex(Bone) == INDEX_NONE)
            {
                continue;
            }
            const TSharedPtr<FJsonObject> Entry = Joint.Value->AsObject();
            const FVector MayaWorld = ReadVector(Entry, TEXT("world"));
            // Common space: Maya (x, y, z) is Unreal (x, z, y), placed by the anchor.
            const FVector Expected = LinkageAnchor.TransformPosition(
                FVector(MayaWorld.X, MayaWorld.Z, MayaWorld.Y));
            const FVector Shown = Primary->GetSocketLocation(Bone);
            MaxWorld = FMath::Max(MaxWorld, FVector::Distance(Expected, Shown));
            FVector2D UnrealScreen;
            const TArray<TSharedPtr<FJsonValue>>* MayaScreen = nullptr;
            if (Camera && ProjectThroughCamera(View, Shown, UnrealScreen)
                && Entry->TryGetArrayField(TEXT("screen"), MayaScreen) && MayaScreen->Num() == 2)
            {
                const FVector2D MayaPoint((*MayaScreen)[0]->AsNumber(), (*MayaScreen)[1]->AsNumber());
                MaxScreen = FMath::Max(MaxScreen, FVector2D::Distance(UnrealScreen, MayaPoint));
                Record->SetArrayField(Joint.Key, NumberArray({Shown.X, Shown.Y, Shown.Z,
                    Expected.X, Expected.Y, Expected.Z, UnrealScreen.X, UnrealScreen.Y, MayaPoint.X, MayaPoint.Y}));
            }
            ++Compared;
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
        Record->SetStringField(TEXT("camera"), Camera ? Camera->GetOwner()->GetActorNameOrLabel() : TEXT("none"));
        Record->SetNumberField(TEXT("max_world_cm"), MaxWorld);
        Record->SetNumberField(TEXT("max_screen_ndc"), MaxScreen);
        Record->SetObjectField(TEXT("maya_snapshot"), Snapshot);
        WriteJsonFile(FPaths::Combine(EvidenceDir, FString::Printf(TEXT("oracle-%d.json"), FrameIndex)), Record);
        Log(FString::Printf(TEXT("oracle frame %.1f via %s: %d bones, max world %.4f cm, max screen %.5f (NDC)"),
            OracleFrames[FrameIndex], Camera ? *Camera->GetOwner()->GetActorNameOrLabel() : TEXT("no camera"),
            Compared, MaxWorld, MaxScreen));
        Test->TestTrue(TEXT("oracle compares the character's bones"), Compared >= 3);
        Test->TestTrue(TEXT("the displayed character matches Maya's world pose through the anchor"),
            MaxWorld < Fixture->GetNumberField(TEXT("world_tolerance_cm")));
        Test->TestTrue(TEXT("Unreal's rendered camera shows the character where Maya's linked camera does"),
            MaxScreen < Fixture->GetNumberField(TEXT("screen_tolerance_ndc")));
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
            return Now - PlayStarted > Fixture->GetNumberField(TEXT("play_seconds"));
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
        if (Scenario == TEXT("realtime") || Scenario == TEXT("end"))
        {
            Test->TestTrue(TEXT("the panel exit is accepted"),
                MtoURequestLinkageLeave(Scenario, View.Identity.ModeRevision + 1, Error));
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
        if (Scenario == TEXT("destroy_target"))
        {
            // The deleted target ended the streaming session too; the artist
            // places the actor again and reconnects from Maya.
            if (!Actor.IsValid())
            {
                if (!SpawnTarget())
                {
                    return true;
                }
                Reconnected = false;
                return false;
            }
            bool bConnected = false;
            MayaResult->TryGetBoolField(TEXT("connected"), bConnected);
            if (!Actor->GetConnectionStatus().StartsWith(TEXT("Connected")) || !bConnected)
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
                FString Error;
                Test->TestTrue(TEXT("the new Binding Actor pairs again"),
                    MtoUPairLinkageTarget(*Actor, Fixture->GetStringField(TEXT("object_id")), Error));
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
        TMap<FName, FVector> After;
        RecordWriterPose(After);
        double MaxDelta = 0.0;
        for (const TPair<FName, FVector>& Entry : BeforePose)
        {
            if (const FVector* Now = After.Find(Entry.Key))
            {
                MaxDelta = FMath::Max(MaxDelta, FVector::Distance(*Now, Entry.Value));
            }
        }
        Log(FString::Printf(TEXT("%s: the sequence animation drives the target again, max delta %.4f cm from before the takeover"),
            *Scenario, MaxDelta));
        Test->TestTrue(*(Tag + TEXT("the original animation writer shows its own pose again")), MaxDelta < 0.05);
        LeaveIssued.Reset();
        return true;
    }

    void Finish()
    {
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
        if (MayaResult.IsValid())
        {
            Evidence->SetObjectField(TEXT("maya_final"), MayaResult);
        }
        WriteJsonFile(FPaths::Combine(EvidenceDir, TEXT("ue-run.json")), Evidence.ToSharedRef());
    }

    FAutomationTestBase* Test;
    FString FixturePath, EvidenceDir, MayaCommandPath, MayaResultPath;
    TSharedPtr<FJsonObject> Fixture, MayaResult, Evidence;
    TArray<FStep> Steps;
    int32 Step = 0;
    double StepStarted = 0.0;
    int32 LogIndex = 0, CommandId = 0, ReportedMayaErrors = 0;
    FProcHandle Maya;
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
    TArray<TWeakObjectPtr<UMovieSceneTrack>> WriterTracks;
    TArray<FString> ProtectedPackages;
    TMap<FName, FVector> BeforePose;
    double SettleUntil = 0.0, SettledFrame = -1.0;
    int32 EnterRequested = -1, SnapshotId = -1, ShotId = -1, QuitId = -1, EndAfterRealtime = -1;
    double MayaTimeBefore = 0.0;
    FString MayaCameraBefore, LeaveIssued, LastApplied;
    double PlayStarted = 0.0, MaxLag = 0.0;
    int32 PlayTicks = 0;
    TArray<double> AppliedFrames;
    bool bStopped = false;
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
        AddInfo(TEXT("Production linkage not requested; pass -MtoULinkageProductionFixture=<fixture.json>."));
        return true;
    }
    FAutomationTestFramework::Get().EnqueueLatentCommand(
        MakeShared<FProductionLinkageCommand>(this, Fixture));
    return true;
}
#endif
