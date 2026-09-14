#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"
#include "MtoULiveLinkPreview.h"
#include "MtoULiveLinkSource.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Editor.h"
#include "DrawDebugHelpers.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
TSharedPtr<FJsonObject> ReadCharacterJson(const FString& Path)
{
    FString Text;
    TSharedPtr<FJsonObject> Value;
    if (FFileHelper::LoadFileToString(Text, *Path))
    {
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Value);
    }
    return Value;
}
void WriteCharacterJson(const FString& Path, const TSharedRef<FJsonObject>& Value)
{
    FString Text;
    FJsonSerializer::Serialize(Value, TJsonWriterFactory<>::Create(&Text));
    FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
FTransform CharacterTransform(const TSharedPtr<FJsonValue>& Value)
{
    const auto& A = Value->AsArray();
    return FTransform(FQuat(A[3]->AsNumber(), A[4]->AsNumber(), A[5]->AsNumber(), A[6]->AsNumber()),
        FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber()),
        FVector(A[7]->AsNumber(), A[8]->AsNumber(), A[9]->AsNumber()));
}

// Explicit opt-in, asset-independent acceptance harness. The supplied project
// owns the assets; only a transient actor/binding and a disposable Maya process
// are changed. A latent command lets editor animation and rendering really tick.
class FCharacterAcceptanceCommand : public IAutomationLatentCommand
{
public:
    FCharacterAcceptanceCommand(FAutomationTestBase* InTest, FString InFixture,
        FString InMayapy, FString InPeer, FString InEvidence)
        : Test(InTest), FixturePath(MoveTemp(InFixture)), Mayapy(MoveTemp(InMayapy)),
          Peer(MoveTemp(InPeer)), Evidence(MoveTemp(InEvidence)) {}
    ~FCharacterAcceptanceCommand()
    {
        if (Process.IsValid())
        {
            if (FPlatformProcess::IsProcRunning(Process)) { FPlatformProcess::TerminateProc(Process, true); }
            FPlatformProcess::CloseProc(Process);
        }
        if (Source && Client) { Source->StopListener(); Client->RemoveSource(Source.ToSharedRef()); }
        if (Actor.IsValid()) { Actor->Destroy(); }
    }
    bool Update() override
    {
        if (Stage == -1) { return !Start(); }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(FString::Printf(TEXT("Character acceptance timed out at stage %d: %s"), Stage,
                Actor.IsValid() ? *Actor->GetConnectionStatus() : TEXT("no actor")));
            return true;
        }
        Source->Update(); Client->ForceTick();
        if (Actor.IsValid() && Fixture->HasField(TEXT("debug_bones")))
        {
            auto* Component = Actor->GetSkeletalMeshComponent();
            for (const auto& Name : Fixture->GetArrayField(TEXT("debug_bones")))
            {
                const FName Bone(*Name->AsString());
                const FVector Point = Component->GetSocketLocation(Bone);
                const FName Parent = Component->GetParentBone(Bone);
                DrawDebugSphere(Actor->GetWorld(), Point, 0.6, 8, FColor::Cyan, false, 0.0, 1);
                DrawDebugCoordinateSystem(Actor->GetWorld(), Point, Component->GetSocketRotation(Bone), 4.0, false, 0.0, 1, 1.5);
                if (!Parent.IsNone()) { DrawDebugLine(Actor->GetWorld(), Point,
                    Component->GetSocketLocation(Parent), FColor::Cyan, false, 0.0, 1, 1.5); }
            }
        }
        auto Result = ReadCharacterJson(ResultPath);
        if (!Result) { return false; }
        bool bOk = false;
        if (Result->TryGetBoolField(TEXT("ok"), bOk) && !bOk && Result->HasField(TEXT("error")))
        {
            Test->AddError(Result->GetStringField(TEXT("error"))); return true;
        }
        if (Stage == 0)
        {
            if (Result->GetStringField(TEXT("phase")) != TEXT("loaded")) { return false; }
            Send(TEXT("connect"), 0, TEXT("animation")); Stage = 1;
        }
        else if (Stage == 1 || Stage == 4)
        {
            if (!Acknowledged(Result)) { return false; }
            Test->TestEqual(TEXT("complete Maya skeleton"), Result->GetArrayField(TEXT("bones")).Num(),
                Binding->SkeletalMesh->GetRefSkeleton().GetNum());
            Test->AddInfo(TEXT("Negotiated remaps: ") + JsonText(Result->GetObjectField(TEXT("warning"))));
            if (Fixture->HasField(TEXT("expected_remaps")))
            {
                TArray<FString> Actual, Expected;
                for (const auto& V : Result->GetObjectField(TEXT("warning"))->GetArrayField(TEXT("bone_name_remaps"))) { Actual.Add(V->AsString()); }
                for (const auto& V : Fixture->GetArrayField(TEXT("expected_remaps"))) { Expected.Add(V->AsString()); }
                Actual.Sort(); Expected.Sort();
                Test->TestTrue(TEXT("actual handshake has exactly the expected remaps"), Actual == Expected);
            }
            Send(TEXT("pose"), 0); Pose = 0; Settled = 0; Stage = 2;
        }
        else if (Stage == 2)
        {
            if (!Acknowledged(Result)) { return false; }
            if (!Settled) { Settled = FPlatformTime::Seconds(); return false; }
            if (FPlatformTime::Seconds() - Settled < 2.0) { return false; }
            if (!CheckPose(Result)) { return true; }
            const FString Label = FString::Printf(TEXT("%s-pose-%d"), *Workflow, Pose);
            if (!FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
            {
                FScreenshotRequest::RequestScreenshot(FPaths::Combine(Evidence, Label + TEXT(".png")), false, false);
            }
            Test->AddInfo(TEXT("Accepted ") + Label);
            if (++Pose < Fixture->GetArrayField(TEXT("poses")).Num())
            {
                Send(TEXT("pose"), Pose); Settled = 0;
            }
            else
            {
                // Give the screenshot a frame before the synchronous Refresh.
                Stage = 3; Settled = FPlatformTime::Seconds();
            }
        }
        else if (Stage == 3)
        {
            if (FPlatformTime::Seconds() - Settled < 1.0) { return false; }
            const auto Before = Actor->GetPreviewReadiness();
            const auto Ready = FMtoUPreviewPreparation::RefreshActor(*Actor);
            Test->AddInfo(TEXT("Refresh: ") + Ready.Diagnostics);
            if (!Test->TestTrue(TEXT("actual Preview geometry passes Refresh"), Ready.IsUsable() && Ready.GeneratedPreview))
            {
                Test->AddError(Ready.Diagnostics); return true;
            }
            if (Before.GeneratedPreview)
            {
                Test->TestTrue(TEXT("Refresh replaces generated description"), Before.GeneratedPreview != Ready.GeneratedPreview);
            }
            Send(TEXT("disconnected")); Stage = 5; Settled = FPlatformTime::Seconds();
        }
        else if (Stage == 5)
        {
            if (!Acknowledged(Result) || FPlatformTime::Seconds() - Settled < 2.0) { return false; }
            FLiveLinkSubjectFrameData Old;
            Test->TestFalse(TEXT("refresh removes ended Live Link subject"),
                Client->EvaluateFrameFromSource_AnyThread(Key, ULiveLinkAnimationRole::StaticClass(), Old));
            Test->TestTrue(TEXT("refresh returns listener"), Source->GetSourceStatus().ToString().Contains(TEXT("Listening on")));
            if (Workflow == TEXT("animation"))
            {
                Workflow = TEXT("model"); Send(TEXT("connect"), 0, Workflow); Stage = 4;
            }
            else
            {
                // A third handshake after Model Refresh must restore a fresh
                // Animation description and pose, not the ended Model frame.
                Workflow = TEXT("reconnected-animation"); Send(TEXT("connect"), 0, TEXT("animation")); Stage = 6;
            }
        }
        else if (Stage == 6)
        {
            if (!Acknowledged(Result)) { return false; }
            Send(TEXT("pose"), 0); Pose = 0; Settled = 0; Stage = 7;
        }
        else if (Stage == 7)
        {
            if (!Acknowledged(Result)) { return false; }
            if (!Settled) { Settled = FPlatformTime::Seconds(); return false; }
            if (FPlatformTime::Seconds() - Settled < 2.0) { return false; }
            if (!CheckPose(Result)) { return true; }
            Send(TEXT("stop")); Stage = 8;
        }
        else if (Stage == 8 && Acknowledged(Result))
        {
            if (FPlatformProcess::IsProcRunning(Process)) { return false; }
            int32 ExitCode = -1;
            FPlatformProcess::GetProcReturnCode(Process, &ExitCode);
            Test->TestEqual(TEXT("Maya peer completes lifecycle cleanup"), ExitCode, 0);
            WriteCharacterJson(FPaths::Combine(Evidence, TEXT("final-peer.json")), Result.ToSharedRef());
            Test->AddInfo(TEXT("Character animation/model/refresh/reconnect acceptance complete."));
            return true;
        }
        return false;
    }
private:
    FString JsonText(TSharedPtr<FJsonObject> Object)
    {
        FString Text; FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Text)); return Text;
    }
    bool Start()
    {
        Fixture = ReadCharacterJson(FixturePath);
        if (!Fixture) { Test->AddError(TEXT("Invalid character fixture")); return false; }
        auto* Original = LoadObject<UMtoULiveLinkBinding>(nullptr, *Fixture->GetStringField(TEXT("binding")));
        if (!Original || !Original->SkeletalMesh || !Original->PreviewStaticMesh)
        { Test->AddError(TEXT("Fixture requires an existing complete Binding")); return false; }
        Binding.Reset(DuplicateObject<UMtoULiveLinkBinding>(Original, GetTransientPackage()));
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World->GetOutermost()->GetName().StartsWith(TEXT("/Engine/")))
        { Test->AddError(TEXT("Run character acceptance in a disposable editor launched on /Engine/Maps/Entry.")); return false; }
        // Never replace a user's map. The explicitly launched engine startup
        // map is exchanged for an unsaved /Temp world so geometry generation
        // does not attempt to write beneath a protected /Engine package.
        World = GEditor->NewMap();
        FActorSpawnParameters Params; Params.ObjectFlags |= RF_Transient;
        Actor = World->SpawnActor<AMtoULiveLinkActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
        Actor->SetActorLabel(TEXT("MtoU Character Acceptance (unsaved)"));
        Actor->SetBinding(Binding.Get());
        for (auto* View : GEditor->GetLevelViewportClients())
        {
            if (!View || !View->IsPerspective()) { continue; }
            const auto Bounds = Binding->SkeletalMesh->GetBounds();
            const FVector Center = Bounds.Origin;
            const FVector Eye = Center + FVector(0.8, 3.5, 0.4) * Bounds.SphereRadius;
            View->SetViewLocation(Eye); View->SetViewRotation((Center - Eye).Rotation());
            View->SetRealtime(true); View->SetViewMode(VMI_Unlit); View->FocusViewportOnBox(Bounds.GetBox().ExpandBy(Bounds.SphereRadius * 0.15), true); View->Invalidate();
        }
        auto* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        FSocket* Socket = Sockets->CreateSocket(NAME_Stream, TEXT("Character acceptance port"));
        auto Address = Sockets->CreateInternetAddr(); bool Valid = false;
        Address->SetIp(TEXT("127.0.0.1"), Valid); Address->SetPort(0);
        Socket->Bind(*Address); Socket->GetAddress(*Address); const int32 Port = Address->GetPort();
        Socket->Close(); Sockets->DestroySocket(Socket);
        Client = &IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
        Source = MakeShared<FMtoULiveLinkSource>(Port);
        Key = FLiveLinkSubjectKey(Client->AddSource(Source.ToSharedRef()), FName(TEXT("MtoU_Character")));
        IFileManager::Get().MakeDirectory(*Evidence, true);
        ResultPath = FPaths::Combine(Evidence, TEXT("character-peer.json"));
        CommandPath = FPaths::Combine(Evidence, TEXT("character-command.json"));
        Fixture->SetNumberField(TEXT("port"), Port); Fixture->SetStringField(TEXT("command"), CommandPath);
        const FString RuntimeFixture = FPaths::Combine(Evidence, TEXT("character-fixture.json"));
        WriteCharacterJson(RuntimeFixture, Fixture.ToSharedRef());
        FFileHelper::SaveStringToFile(TEXT("{}"), *CommandPath);
        FFileHelper::SaveStringToFile(TEXT("{\"phase\":\"starting\"}"), *ResultPath);
        const FString Args = FString::Printf(TEXT("\"%s\" --fixture \"%s\" --result \"%s\""), *Peer, *RuntimeFixture, *ResultPath);
        Process = FPlatformProcess::CreateProc(*Mayapy, *Args, false, true, true, nullptr, 0, nullptr, nullptr);
        if (!Process.IsValid()) { Test->AddError(TEXT("Could not start Maya")); return false; }
        Deadline = FPlatformTime::Seconds() + 900; Stage = 0; return true;
    }
    bool Acknowledged(TSharedPtr<FJsonObject> Result)
    { return Result->HasField(TEXT("id")) && Result->GetIntegerField(TEXT("id")) == CommandId; }
    void Send(const FString& Action, int32 InPose = 0, const FString& InWorkflow = FString())
    {
        auto Command = MakeShared<FJsonObject>(); Command->SetNumberField(TEXT("id"), ++CommandId);
        Command->SetStringField(TEXT("action"), Action); Command->SetNumberField(TEXT("pose"), InPose);
        Command->SetStringField(TEXT("workflow"), InWorkflow); WriteCharacterJson(CommandPath, Command);
    }
    bool CheckPose(TSharedPtr<FJsonObject> Result)
    {
        FLiveLinkSubjectFrameData Frame;
        if (!Test->TestTrue(TEXT("actual character frame is evaluated"),
            Client->EvaluateFrameFromSource_AnyThread(Key, ULiveLinkAnimationRole::StaticClass(), Frame))) { return false; }
        const auto* Static = Frame.StaticData.Cast<FLiveLinkSkeletonStaticData>();
        const auto* Animation = Frame.FrameData.Cast<FLiveLinkAnimationFrameData>();
        const auto& Ref = Binding->SkeletalMesh->GetRefSkeleton();
        if (!Static || !Animation || !Test->TestEqual(TEXT("full evaluated hierarchy"), Animation->Transforms.Num(), Ref.GetNum())) { return false; }
        TArray<FMatrix> BindWorld, CurrentWorld, RefWorld, PublishedWorld;
        for (int32 I = 0; I < Ref.GetNum(); ++I)
        {
            const int32 P = Ref.GetParentIndex(I);
            RefWorld.Add(Ref.GetRefBonePose()[I].ToMatrixWithScale() * (P < 0 ? FMatrix::Identity : RefWorld[P]));
        }
        double MaxPositionError = 0;
        double MaxDisplayedError = 0;
        double MaxAxisError = 0, MaxDisplayedAxisError = 0;
        for (int32 I = 0; I < Static->BoneNames.Num(); ++I)
        {
            const int32 P = Static->BoneParents[I];
            const int32 Target = Ref.FindBoneIndex(Static->BoneNames[I]);
            if (!Test->TestTrue(TEXT("published bone exists in actual Driver"), Target != INDEX_NONE)) { return false; }
            BindWorld.Add(CharacterTransform(Result->GetArrayField(TEXT("bind"))[I]).ToMatrixWithScale()
                * (P < 0 ? FMatrix::Identity : BindWorld[P]));
            CurrentWorld.Add(CharacterTransform(Result->GetArrayField(TEXT("transforms"))[I]).ToMatrixWithScale()
                * (P < 0 ? FMatrix::Identity : CurrentWorld[P]));
            PublishedWorld.Add(Animation->Transforms[I].ToMatrixWithScale()
                * (P < 0 ? FMatrix::Identity : PublishedWorld[P]));
            FMatrix Inverse;
            if (!VectorMatrixInverse(&Inverse, &BindWorld[I])) { Test->AddError(TEXT("Fixture bind inverse failed")); return false; }
            const FMatrix Expected = RefWorld[Target] * Inverse * CurrentWorld[I];
            MaxPositionError = FMath::Max(MaxPositionError, FVector::Distance(Expected.GetOrigin(), PublishedWorld[I].GetOrigin()));
            for (EAxis::Type Axis : {EAxis::X, EAxis::Y, EAxis::Z})
            {
                MaxAxisError = FMath::Max(MaxAxisError, FVector::Distance(
                    Expected.GetScaledAxis(Axis).GetSafeNormal(0.0),
                    PublishedWorld[I].GetScaledAxis(Axis).GetSafeNormal(0.0)));
            }
            const auto& Displayed = Actor->GetSkeletalMeshComponent()->GetComponentSpaceTransforms();
            if (Displayed.IsValidIndex(Target))
            {
                MaxDisplayedError = FMath::Max(MaxDisplayedError,
                    FVector::Distance(PublishedWorld[I].GetOrigin(), Displayed[Target].GetTranslation()));
                const FMatrix DisplayedMatrix = Displayed[Target].ToMatrixWithScale();
                for (EAxis::Type Axis : {EAxis::X, EAxis::Y, EAxis::Z})
                {
                    MaxDisplayedAxisError = FMath::Max(MaxDisplayedAxisError, FVector::Distance(
                        PublishedWorld[I].GetScaledAxis(Axis).GetSafeNormal(0.0),
                        DisplayedMatrix.GetScaledAxis(Axis).GetSafeNormal(0.0)));
                }
            }
            else { Test->AddError(TEXT("Displayed skeleton is incomplete")); return false; }
        }
        Test->AddInfo(FString::Printf(TEXT("%s pose %d: %d bones, maximum world position error %.9g cm"),
            *Workflow, Pose, Static->BoneNames.Num(), MaxPositionError));
        Test->TestTrue(TEXT("published world positions match component bind-space motion"), MaxPositionError < 0.1);
        Test->TestTrue(TEXT("published world axes match component bind-space motion"), MaxAxisError < 1.e-3);
        Test->AddInfo(FString::Printf(TEXT("Maximum normalized world axis error %.9g"), MaxAxisError));
        Test->AddInfo(FString::Printf(TEXT("Maximum displayed component position error %.9g cm"), MaxDisplayedError));
        Test->TestTrue(TEXT("rendered component follows published pose"), MaxDisplayedError < 0.1);
        Test->TestTrue(TEXT("rendered component axes follow published pose"), MaxDisplayedAxisError < 1.e-3);
        Test->AddInfo(FString::Printf(TEXT("Maximum displayed normalized axis error %.9g"), MaxDisplayedAxisError));
        Test->TestTrue(TEXT("workflow selects the intended display"), Actor->GetDisplayTarget() ==
            (Workflow == TEXT("model") ? EMtoUDisplayTarget::GeneratedPreview : EMtoUDisplayTarget::Driver));
        const FString Record = FPaths::Combine(Evidence, FString::Printf(TEXT("%s-pose-%d.json"), *Workflow, Pose));
        WriteCharacterJson(Record, Result.ToSharedRef());
        return MaxPositionError < 0.1 && MaxDisplayedError < 0.1 && MaxAxisError < 1.e-3 && MaxDisplayedAxisError < 1.e-3;
    }
    FAutomationTestBase* Test;
    FString FixturePath, Mayapy, Peer, Evidence, ResultPath, CommandPath, Workflow = TEXT("animation");
    TSharedPtr<FJsonObject> Fixture;
    TStrongObjectPtr<UMtoULiveLinkBinding> Binding;
    TWeakObjectPtr<AMtoULiveLinkActor> Actor;
    TSharedPtr<FMtoULiveLinkSource> Source;
    ILiveLinkClient* Client = nullptr;
    FLiveLinkSubjectKey Key;
    FProcHandle Process;
    int32 Stage = -1, CommandId = 0, Pose = 0;
    double Deadline = 0, Settled = 0;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUCharacterAcceptanceTest,
    "MtoULiveLink.Editor.Preview.CharacterAcceptance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMtoUCharacterAcceptanceTest::RunTest(const FString& Parameters)
{
    FString Fixture, Mayapy, Peer, Evidence;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUCharacterFixture="), Fixture))
    { AddInfo(TEXT("Character acceptance not requested; provide fixture, mayapy, peer and evidence paths.")); return true; }
    if (!FParse::Value(FCommandLine::Get(), TEXT("MtoUMayapy="), Mayapy)
        || !FParse::Value(FCommandLine::Get(), TEXT("MtoUCharacterPeer="), Peer)
        || !FParse::Value(FCommandLine::Get(), TEXT("MtoUEvidence="), Evidence))
    { AddError(TEXT("Missing character acceptance host arguments")); return false; }
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FCharacterAcceptanceCommand>(this, Fixture, Mayapy, Peer, Evidence));
    return true;
}
#endif
