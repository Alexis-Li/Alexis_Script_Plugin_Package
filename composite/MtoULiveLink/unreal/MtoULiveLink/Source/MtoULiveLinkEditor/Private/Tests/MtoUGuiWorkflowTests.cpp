#if WITH_DEV_AUTOMATION_TESTS
#include "MtoUGuiAcceptance.h"
#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"
#include "MtoULiveLinkPreview.h"

#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

/**
 * Opt-in regression of the workflows that existed before camera linkage,
 * driven through a real Maya GUI and the panel's own controls.
 *
 * The test exchanges the explicitly launched /Engine/Maps/Entry map for an
 * unsaved /Temp world, places a transient Binding Actor at the origin with a
 * transient duplicate of the fixture's Binding, and starts Maya through
 * `maya_gui_linkage_driver.py`. It then checks, against Maya's own world
 * pose and Morph weights: real-time preview; 缓存播放 capture, upload, UE
 * playback, seek and the return to real time; Model with Refresh and the
 * original/generated garment comparison; and Animation again after Model.
 * Each stage renders the Details at the fixture widths when the run can
 * render and saves the Maya panel. No project asset is edited or saved.
 */
namespace
{
using MtoUGuiAcceptance::ReadJsonFile;
using MtoUGuiAcceptance::WriteJsonFile;

FString CacheStateName(EMtoUCacheState State)
{
    switch (State)
    {
    case EMtoUCacheState::Idle: return TEXT("Idle");
    case EMtoUCacheState::Entered: return TEXT("Entered");
    case EMtoUCacheState::Receiving: return TEXT("Receiving");
    case EMtoUCacheState::Ready: return TEXT("Ready");
    case EMtoUCacheState::Playing: return TEXT("Playing");
    case EMtoUCacheState::Paused: return TEXT("Paused");
    case EMtoUCacheState::Completed: return TEXT("Completed");
    case EMtoUCacheState::Stopped: return TEXT("Stopped");
    case EMtoUCacheState::Failed: return TEXT("Failed");
    }
    return TEXT("Unknown");
}

class FGuiWorkflowCommand : public IAutomationLatentCommand
{
public:
    FGuiWorkflowCommand(FAutomationTestBase* InTest, FString InFixture)
        : Test(InTest), FixturePath(MoveTemp(InFixture)) {}

    ~FGuiWorkflowCommand() override
    {
        Driver.Terminate();
    }

    bool Update() override
    {
        if (Steps.IsEmpty() && !Prepare())
        {
            return true;
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
        MayaResult = Driver.Poll();
        if (MayaResult.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
            if (MayaResult->TryGetArrayField(TEXT("errors"), Errors) && Errors->Num() > ReportedErrors)
            {
                for (int32 Index = ReportedErrors; Index < Errors->Num(); ++Index)
                {
                    Test->AddError(TEXT("Maya: ") + (*Errors)[Index]->AsString());
                }
                ReportedErrors = Errors->Num();
            }
        }
        if (Now - StepStarted > Steps[Step].Timeout)
        {
            const FMtoUCachePlaybackView Cache = Actor.IsValid() ? Actor->GetCachePlaybackView() : FMtoUCachePlaybackView();
            const FString Message = FString::Printf(TEXT("Step '%s' timed out; actor=%s cache=%s frames=%d current=%d; Maya: %s"),
                *Steps[Step].Name, Actor.IsValid() ? *Actor->GetConnectionStatus() : TEXT("none"),
                *CacheStateName(Cache.State), Cache.FrameCount, Cache.CurrentSourceFrame, *MayaPanel());
            Test->AddError(Message);
            Log(TEXT("TIMEOUT ") + Message);
            Finish();
            return true;
        }
        if (Steps[Step].Run())
        {
            if (bAborted)
            {
                Finish();
                return true;
            }
            Log(FString::Printf(TEXT("step %d '%s' done in %.2f s"), Step, *Steps[Step].Name, Now - StepStarted));
            ++Step;
            StepStarted = 0.0;
            Pending = -1;
            SettleUntil = 0.0;
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
        FFileHelper::SaveStringToFile(Line + LINE_TERMINATOR, *FPaths::Combine(EvidenceDir, TEXT("progress.log")),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
    }

    FString MayaPanel() const
    {
        const TSharedPtr<FJsonObject>* Panel = nullptr;
        if (!MayaResult.IsValid() || !MayaResult->TryGetObjectField(TEXT("panel"), Panel))
        {
            return TEXT("no panel state");
        }
        FString Text;
        FJsonSerializer::Serialize(Panel->ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
        return Text;
    }

    FString PanelField(const TCHAR* Field) const
    {
        const TSharedPtr<FJsonObject>* Panel = nullptr;
        FString Value;
        if (MayaResult.IsValid() && MayaResult->TryGetObjectField(TEXT("panel"), Panel))
        {
            (*Panel)->TryGetStringField(Field, Value);
        }
        return Value;
    }

    bool MayaConnected() const
    {
        bool bConnected = false;
        return MayaResult.IsValid() && MayaResult->TryGetBoolField(TEXT("connected"), bConnected) && bConnected;
    }

    bool Connected() const
    {
        return MayaConnected() && Actor.IsValid() && Actor->GetConnectionStatus().StartsWith(TEXT("Connected"));
    }

    /** Sends one command once per step and reports whether Maya finished it. */
    bool SendOnce(const FString& Action, const TSharedPtr<FJsonObject>& Fields = nullptr)
    {
        if (Pending < 0)
        {
            Pending = Driver.Send(Action, Fields);
        }
        return Driver.Acked(Pending);
    }

    TSharedPtr<FJsonObject> Field(const FString& Name, const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
        Fields->SetField(Name, Value);
        return Fields;
    }

    bool Settled(double Seconds)
    {
        if (SettleUntil == 0.0)
        {
            SettleUntil = FPlatformTime::Seconds() + Seconds;
        }
        return FPlatformTime::Seconds() >= SettleUntil;
    }

    // ----------------------------------------------------------------- steps

    bool Prepare()
    {
        Fixture = ReadJsonFile(FixturePath);
        if (!Fixture.IsValid())
        {
            Test->AddError(TEXT("Invalid GUI workflow fixture"));
            return false;
        }
        EvidenceDir = Fixture->GetStringField(TEXT("evidence"));
        IFileManager::Get().MakeDirectory(*EvidenceDir, true);
        Fixture->TryGetArrayField(TEXT("details_widths"), DetailsWidths);

        Add(TEXT("create the unsaved world and the Binding Actor"), 120.0, [this]() { return BuildScene(); });
        Add(TEXT("start Maya through the public entry"), 30.0, [this]()
        {
            return Driver.Start(*Test, Fixture->GetObjectField(TEXT("maya")), EvidenceDir)
                || Abort(TEXT("Maya did not start"));
        });
        Add(TEXT("Maya connects in Animation real time"), 600.0, [this]() { return Connected(); });

        // ---- real-time preview ------------------------------------------
        int32 Index = 0;
        for (const TSharedPtr<FJsonValue>& Frame : Fixture->GetArrayField(TEXT("realtime_frames")))
        {
            AddPoseCheck(FString::Printf(TEXT("real time %d"), Index++), Frame->AsNumber());
        }
        AddShots(TEXT("realtime"));

        // ---- cached playback ----------------------------------------------
        const TArray<TSharedPtr<FJsonValue>>& Range = Fixture->GetArrayField(TEXT("cache_range"));
        const int32 First = static_cast<int32>(Range[0]->AsNumber());
        const int32 Last = static_cast<int32>(Range[1]->AsNumber());
        const int32 Seek = static_cast<int32>(Fixture->GetNumberField(TEXT("cache_seek")));
        Add(TEXT("Maya selects 缓存播放"), 30.0, [this]()
        {
            return SendOnce(TEXT("mode"), Field(TEXT("target"), MakeShared<FJsonValueString>(TEXT("cached"))))
                && PanelField(TEXT("mode")) == TEXT("cached");
        });
        Add(TEXT("捕获并上传 a custom range"), 30.0, [this, First, Last]()
        {
            return SendOnce(TEXT("capture"), Field(TEXT("range"), MakeShared<FJsonValueArray>(
                TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(First), MakeShared<FJsonValueNumber>(Last)})));
        });
        Add(TEXT("Unreal holds the complete cache"), 300.0, [this, First, Last]()
        {
            const FMtoUCachePlaybackView Cache = Actor->GetCachePlaybackView();
            if (Cache.State != EMtoUCacheState::Ready)
            {
                return false;
            }
            Log(FString::Printf(TEXT("cache ready: source %d-%d, %d frames at %.2f fps; Maya: %s"),
                Cache.StartFrame, Cache.EndFrame, Cache.FrameCount, Cache.Fps, *MayaPanel()));
            Test->TestEqual(TEXT("the cache starts at the captured range"), Cache.StartFrame, First);
            Test->TestEqual(TEXT("the cache ends at the captured range"), Cache.EndFrame, Last);
            Test->TestEqual(TEXT("the cache holds every captured frame"), Cache.FrameCount, Last - First + 1);
            return true;
        });
        AddShots(TEXT("cache-ready"));
        Add(TEXT("UE 播放 completes the cache"), 120.0, [this, First, Last]()
        {
            if (!bStarted)
            {
                bStarted = Test->TestTrue(TEXT("播放 starts"), Actor->StartCachedPlayback());
                return !bStarted;
            }
            const FMtoUCachePlaybackView Cache = Actor->GetCachePlaybackView();
            if (Cache.State != EMtoUCacheState::Completed)
            {
                return false;
            }
            bStarted = false;
            Log(FString::Printf(TEXT("cache playback completed: %d frames applied, last source frame %d"),
                Cache.AppliedFrames, Cache.CurrentSourceFrame));
            Test->TestEqual(TEXT("playback applies every frame"), Cache.AppliedFrames, Last - First + 1);
            Test->TestEqual(TEXT("natural completion holds the final frame"), Cache.CurrentSourceFrame, Last);
            return true;
        });
        Add(TEXT("定位 holds the seek frame"), 30.0, [this, Seek]()
        {
            if (!bStarted)
            {
                bStarted = Test->TestTrue(TEXT("定位 is accepted"), Actor->SeekCachedPlayback(Seek));
                return !bStarted;
            }
            const FMtoUCachePlaybackView Cache = Actor->GetCachePlaybackView();
            if (Cache.State != EMtoUCacheState::Paused || Cache.CurrentSourceFrame != Seek || !Cache.bHasAppliedSourceFrame)
            {
                return false;
            }
            bStarted = false;
            return Settled(0.5);
        });
        AddPoseCheck(TEXT("cached seek"), Seek);
        AddShots(TEXT("cache-seek"));
        Add(TEXT("Maya returns to 实时预览 and Unreal clears the cache"), 60.0, [this]()
        {
            if (!SendOnce(TEXT("mode"), Field(TEXT("target"), MakeShared<FJsonValueString>(TEXT("realtime")))))
            {
                return false;
            }
            const FMtoUCachePlaybackView Cache = Actor->GetCachePlaybackView();
            return PanelField(TEXT("mode")) == TEXT("realtime") && Cache.FrameCount == 0 && Connected();
        });
        AddPoseCheck(TEXT("real time after the cache"), Fixture->GetNumberField(TEXT("model_frame")) + 30.0);

        // ---- Model, Refresh and garment comparison --------------------------
        AddStage(TEXT("refresh"));
        Add(TEXT("刷新预览 builds the Generated Preview"), 300.0, [this]()
        {
            const FMtoUPreviewReadiness Ready = FMtoUPreviewPreparation::RefreshActor(*Actor);
            Log(TEXT("Refresh: ") + Ready.Diagnostics.Left(400));
            return Test->TestTrue(TEXT("Refresh reaches a usable Generated Preview"), Ready.IsUsable() && Ready.GeneratedPreview);
        });
        Add(TEXT("Refresh ends the session in Maya"), 60.0, [this]() { return !MayaConnected(); });
        AddStage(TEXT("model"));
        Add(TEXT("Maya selects 模型"), 30.0, [this]()
        {
            return SendOnce(TEXT("workflow"), Field(TEXT("target"), MakeShared<FJsonValueString>(TEXT("model"))))
                && PanelField(TEXT("workflow")) == TEXT("model");
        });
        Add(TEXT("Maya connects in Model"), 300.0, [this]() { return SendOnce(TEXT("connect")) && Connected(); });
        AddPoseCheck(TEXT("model"), Fixture->GetNumberField(TEXT("model_frame")));
        Add(TEXT("Model shows the Generated Preview"), 30.0, [this]()
        {
            return Test->TestEqual(TEXT("Model displays the generated garment"),
                static_cast<int32>(Actor->GetDisplayTarget()), static_cast<int32>(EMtoUDisplayTarget::GeneratedPreview));
        });
        AddShots(TEXT("model-generated"));
        Add(TEXT("显示原始服装"), 30.0, [this]()
        {
            if (!bStarted)
            {
                bStarted = Test->TestTrue(TEXT("the original garment can be shown"), Actor->ShowOriginalGarment());
                return !bStarted;
            }
            bStarted = false;
            Test->TestEqual(TEXT("the original garment is displayed"),
                static_cast<int32>(Actor->GetDisplayTarget()), static_cast<int32>(EMtoUDisplayTarget::OriginalGarment));
            Test->TestTrue(TEXT("the comparison keeps the Model session"), Connected());
            return Settled(0.5);
        });
        AddShots(TEXT("model-original"));
        Add(TEXT("显示生成服装"), 30.0, [this]()
        {
            Test->TestTrue(TEXT("the generated garment can be restored"), Actor->RestoreGeneratedGarment());
            Test->TestEqual(TEXT("the generated garment is displayed again"),
                static_cast<int32>(Actor->GetDisplayTarget()), static_cast<int32>(EMtoUDisplayTarget::GeneratedPreview));
            return Test->TestTrue(TEXT("the comparison keeps the Model session"), Connected());
        });

        // ---- Animation again ------------------------------------------------
        AddStage(TEXT("animation-again"));
        Add(TEXT("Maya disconnects"), 60.0, [this]() { return SendOnce(TEXT("disconnect")) && !MayaConnected(); });
        Add(TEXT("Maya selects 动画"), 30.0, [this]()
        {
            return SendOnce(TEXT("workflow"), Field(TEXT("target"), MakeShared<FJsonValueString>(TEXT("animation"))))
                && PanelField(TEXT("workflow")) == TEXT("animation");
        });
        Add(TEXT("Maya connects in Animation again"), 300.0, [this]() { return SendOnce(TEXT("connect")) && Connected(); });
        AddPoseCheck(TEXT("animation after Model"), Fixture->GetArrayField(TEXT("realtime_frames"))[0]->AsNumber());
        AddShots(TEXT("animation-again"));
        Add(TEXT("quit Maya"), 120.0, [this]()
        {
            if (Pending < 0)
            {
                Pending = Driver.Send(TEXT("quit"));
            }
            return !Driver.IsRunning();
        });
        return true;
    }

    /** Names the stage Maya's dialogs are recorded under from here on. */
    void AddStage(const FString& Stage)
    {
        Add(TEXT("dialog stage ") + Stage, 30.0, [this, Stage]()
        {
            return SendOnce(TEXT("stage"), Field(TEXT("name"), MakeShared<FJsonValueString>(Stage)));
        });
    }

    /** Maya's dialogs each stage expects; any other dialog fails the test. */
    static TArray<MtoUGuiAcceptance::FExpectedDialog> ExpectedDialogs()
    {
        return {
            // The fixture's Morph names differ between Maya and Unreal.
            {TEXT("startup"), TEXT("BLENDSHAPE_MISMATCH")},
            {TEXT("model"), TEXT("BLENDSHAPE_MISMATCH")},
            {TEXT("animation-again"), TEXT("BLENDSHAPE_MISMATCH")},
            // Refresh Preview ends the session Maya was connected through.
            {TEXT("refresh"), TEXT("STREAM_INTERRUPTED")}};
    }

    /** Maya moves to one frame; Unreal shows Maya's world pose and Morph weights there. */
    void AddPoseCheck(const FString& Name, double Frame)
    {
        Add(Name + TEXT(": Maya moves to the frame"), 30.0, [this, Frame]()
        {
            return SendOnce(TEXT("time"), Field(TEXT("frame"), MakeShared<FJsonValueNumber>(Frame))) && Settled(1.0);
        });
        Add(Name + TEXT(": compare"), 30.0, [this, Name]()
        {
            if (!SendOnce(TEXT("snapshot")))
            {
                return false;
            }
            ComparePose(Name);
            return true;
        });
    }

    void ComparePose(const FString& Name)
    {
        const TSharedPtr<FJsonObject> Snapshot = MayaResult->GetObjectField(TEXT("snapshot"));
        USkeletalMeshComponent* Primary = Actor->GetSkeletalMeshComponent();
        const FTransform Placement = Actor->GetActorTransform();
        double MaxWorld = 0.0;
        int32 Bones = 0;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Joint : Snapshot->GetObjectField(TEXT("joints"))->Values)
        {
            const FName Bone(*Joint.Key);
            if (!Primary || Primary->GetBoneIndex(Bone) == INDEX_NONE)
            {
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>& World = Joint.Value->AsObject()->GetArrayField(TEXT("world"));
            // Common space: Maya (x, y, z) is Unreal (x, z, y), placed by the actor.
            const FVector Expected = Placement.TransformPosition(
                FVector(World[0]->AsNumber(), World[2]->AsNumber(), World[1]->AsNumber()));
            MaxWorld = FMath::Max(MaxWorld, FVector::Distance(Expected, Primary->GetSocketLocation(Bone)));
            ++Bones;
        }
        double MaxMorph = 0.0;
        double MaxExpected = 0.0;
        int32 Morphs = 0;
        const TSharedPtr<FJsonObject>* MorphValues = nullptr;
        if (Snapshot->TryGetObjectField(TEXT("morphs"), MorphValues))
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Morph : (*MorphValues)->Values)
            {
                const TArray<double> Shown = MtoUGuiAcceptance::DisplayedMorphWeights(*Actor, FName(*Morph.Key));
                for (const double Weight : Shown)
                {
                    MaxMorph = FMath::Max(MaxMorph, FMath::Abs(Weight - Morph.Value->AsNumber()));
                }
                if (Shown.Num() > 0)
                {
                    ++Morphs;
                    MaxExpected = FMath::Max(MaxExpected, FMath::Abs(Morph.Value->AsNumber()));
                }
            }
        }
        double MayaTime = 0.0;
        Snapshot->TryGetNumberField(TEXT("time"), MayaTime);
        Log(FString::Printf(TEXT("%s at Maya frame %.1f: %d bones, max world %.4f cm; %d morphs, max Maya weight %.3f, max delta %.5f"),
            *Name, MayaTime, Bones, MaxWorld, Morphs, MaxExpected, MaxMorph));
        TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
        Record->SetStringField(TEXT("stage"), Name);
        Record->SetNumberField(TEXT("max_world_cm"), MaxWorld);
        Record->SetNumberField(TEXT("bones"), Bones);
        Record->SetNumberField(TEXT("max_morph"), MaxMorph);
        Record->SetNumberField(TEXT("morphs"), Morphs);
        Record->SetObjectField(TEXT("maya_snapshot"), Snapshot);
        WriteJsonFile(FPaths::Combine(EvidenceDir, FString::Printf(TEXT("pose-%02d.json"), PoseIndex++)), Record);
        Test->TestTrue(*(Name + TEXT(": the primary display has the oracle bones")), Bones >= 3);
        Test->TestTrue(*(Name + TEXT(": Unreal shows Maya's world pose")),
            MaxWorld < Fixture->GetNumberField(TEXT("world_tolerance_cm")));
        const TArray<TSharedPtr<FJsonValue>>* Keyed = nullptr;
        if (Fixture->GetObjectField(TEXT("maya"))->TryGetArrayField(TEXT("keyed_morphs"), Keyed) && Keyed->Num() > 0)
        {
            Test->TestTrue(*(Name + TEXT(": a display shows the keyed Morphs")), Morphs > 0);
            Test->TestTrue(*(Name + TEXT(": the oracle Morph weights are non-zero")), MaxExpected > 0.05);
            Test->TestTrue(*(Name + TEXT(": the displays show Maya's Morph weights")),
                MaxMorph < Fixture->GetNumberField(TEXT("morph_tolerance")));
        }
    }

    /** The Details at the fixture widths and the Maya panel at one stage. */
    void AddShots(const FString& State)
    {
        Add(State + TEXT(": Details and Maya panel"), 60.0, [this, State]()
        {
            if (!SendOnce(TEXT("panel_shot"), Field(TEXT("name"), MakeShared<FJsonValueString>(State))))
            {
                return false;
            }
            if (DetailsWidths && DetailsWidths->Num() > 0)
            {
                MtoUGuiAcceptance::RenderDetails(*Test, *Actor, *DetailsWidths,
                    Fixture->HasField(TEXT("details_height")) ? Fixture->GetNumberField(TEXT("details_height")) : 1600.0,
                    EvidenceDir, State, [this](const FString& Line) { Log(Line); });
            }
            return true;
        });
    }

    bool BuildScene()
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        // Never replace a user's map: only the engine startup map or an
        // unsaved /Temp world is exchanged for the test's own world.
        const FString Package = World ? World->GetOutermost()->GetName() : FString();
        if (!Package.StartsWith(TEXT("/Engine/")) && !Package.StartsWith(TEXT("/Temp/")))
        {
            return Abort(TEXT("Run the GUI workflow test in an editor launched on /Engine/Maps/Entry, not on ") + Package);
        }
        UMtoULiveLinkBinding* Original = LoadObject<UMtoULiveLinkBinding>(nullptr, *Fixture->GetStringField(TEXT("binding")));
        if (!Original)
        {
            return Abort(TEXT("Fixture Binding did not load"));
        }
        // Every override stays on this transient duplicate.
        Binding.Reset(DuplicateObject<UMtoULiveLinkBinding>(Original, GetTransientPackage()));
        Binding->SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *Fixture->GetStringField(TEXT("primary_mesh")));
        Binding->PreviewStaticMesh = LoadObject<UStaticMesh>(nullptr, *Fixture->GetStringField(TEXT("preview_mesh")));
        Binding->DriverGarmentSlotOverride.Reset();
        for (const TSharedPtr<FJsonValue>& Slot : Fixture->GetArrayField(TEXT("driver_garment_slots")))
        {
            Binding->DriverGarmentSlotOverride.AddUnique(FName(*Slot->AsString()));
        }
        Binding->AdditionalParts.Reset();
        for (const TSharedPtr<FJsonValue>& Value : Fixture->GetArrayField(TEXT("parts")))
        {
            FMtoUCharacterPart& Part = Binding->AdditionalParts.AddDefaulted_GetRef();
            Part.PartName = Value->AsObject()->GetStringField(TEXT("name"));
            Part.SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *Value->AsObject()->GetStringField(TEXT("mesh")));
            Part.bEnabled = true;
            if (!Part.SkeletalMesh)
            {
                return Abort(TEXT("Fixture part did not load"));
            }
        }
        Binding->EnsureCharacterPartIds();
        if (!Binding->SkeletalMesh || !Binding->PreviewStaticMesh)
        {
            return Abort(TEXT("Fixture Primary Driver or Preview Static Mesh did not load"));
        }
        World = GEditor->NewMap();
        FActorSpawnParameters Params;
        Params.ObjectFlags |= RF_Transient;
        Actor = World->SpawnActor<AMtoULiveLinkActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
        Actor->SetActorLabel(TEXT("MtoU GUI Workflow (unsaved)"));
        Actor->SetBinding(Binding.Get());
        return true;
    }

    /** Ends the run after a setup failure instead of retrying the step. */
    bool Abort(const FString& Reason)
    {
        Test->AddError(Reason);
        bAborted = true;
        return true;
    }

    void Finish()
    {
        if (MayaResult.IsValid())
        {
            MtoUGuiAcceptance::CheckMayaDialogs(*Test, Driver.Poll(), ExpectedDialogs(),
                [this](const FString& Line) { Log(Line); });
            WriteJsonFile(FPaths::Combine(EvidenceDir, TEXT("maya-final.json")), MayaResult.ToSharedRef());
        }
        Driver.Terminate();
    }

    FAutomationTestBase* Test;
    FString FixturePath, EvidenceDir;
    TSharedPtr<FJsonObject> Fixture, MayaResult;
    const TArray<TSharedPtr<FJsonValue>>* DetailsWidths = nullptr;
    TArray<FStep> Steps;
    int32 Step = 0, Pending = -1, ReportedErrors = 0, PoseIndex = 0;
    double StepStarted = 0.0, SettleUntil = 0.0;
    bool bStarted = false;
    bool bAborted = false;
    MtoUGuiAcceptance::FMayaGuiDriver Driver;
    TStrongObjectPtr<UMtoULiveLinkBinding> Binding;
    TWeakObjectPtr<AMtoULiveLinkActor> Actor;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUGuiWorkflowTest,
    "MtoUOptIn.Editor.Workflow.GuiRegression",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUGuiWorkflowTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FString Fixture;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUGuiWorkflowFixture="), Fixture))
    {
        AddInfo(TEXT("GUI workflow regression not requested; run on /Engine/Maps/Entry with -MtoUGuiWorkflowFixture=<fixture.json>."));
        return true;
    }
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FGuiWorkflowCommand>(this, Fixture));
    return true;
}
#endif
