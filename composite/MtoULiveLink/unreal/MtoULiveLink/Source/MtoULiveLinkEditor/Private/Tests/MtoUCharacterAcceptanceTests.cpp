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
#include "MtoUCharacterComposition.h"
#include "MtoULiveLinkPreview.h"
#include "MtoULiveLinkSource.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
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
            Test->TestTrue(TEXT("Maya publishes a nonempty scene snapshot"),
                Result->GetArrayField(TEXT("bones")).Num() > 0);
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
                // The viewport captures on a later tick. Keep this pose until
                // the image has been written instead of switching Maya to the
                // next pose while the screenshot is still pending.
                Stage = 10;
                Settled = FPlatformTime::Seconds();
            }
            Test->AddInfo(TEXT("Accepted ") + Label);
            if (Stage != 10 && ++Pose < Fixture->GetArrayField(TEXT("poses")).Num())
            {
                Send(TEXT("pose"), Pose); Settled = 0;
            }
            else if (Stage != 10)
            {
                Stage = bPreviewStages ? 3 : 9; Settled = FPlatformTime::Seconds();
            }
        }
        else if (Stage == 10)
        {
            if (FScreenshotRequest::IsScreenshotRequested() || FPlatformTime::Seconds() - Settled < 1.0)
            { return false; }
            if (++Pose < Fixture->GetArrayField(TEXT("poses")).Num())
            {
                Send(TEXT("pose"), Pose); Settled = 0; Stage = 2;
            }
            else
            {
                Stage = bPreviewStages ? 3 : 9; Settled = FPlatformTime::Seconds();
            }
        }
        else if (Stage == 9)
        {
            // A Binding without a Preview Static Mesh cannot build a Generated
            // Preview, so this character verifies the Animation session across
            // an explicit Maya disconnect and reconnect instead of a Model run.
            if (FPlatformTime::Seconds() - Settled < 1.0) { return false; }
            Test->AddInfo(TEXT("Binding has no Preview Static Mesh: verifying the Animation reconnect instead of the Model workflow."));
            Workflow = TEXT("reconnected-animation");
            Send(TEXT("reconnect"), 0, TEXT("animation")); Stage = 6;
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
        if (!Original || !Original->SkeletalMesh)
        { Test->AddError(TEXT("Fixture requires an existing Binding with a Primary Driver")); return false; }
        // The Model phases need a Preview Static Mesh on the Binding; without
        // one the run stays on the Animation phases and reconnects instead.
        bPreviewStages = Original->PreviewStaticMesh != nullptr;
        Binding.Reset(DuplicateObject<UMtoULiveLinkBinding>(Original, GetTransientPackage()));
        // Optional fixture overrides stay on this transient duplicate. They let
        // a production project's existing meshes be accepted in a new outfit
        // combination without editing or saving the owner's Binding asset.
        if (Fixture->HasField(TEXT("primary_mesh")))
        {
            const FString Path = Fixture->GetStringField(TEXT("primary_mesh"));
            Binding->SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *Path);
            if (!Binding->SkeletalMesh)
            { Test->AddError(TEXT("Could not load fixture Primary Driver: ") + Path); return false; }
            Binding->PreviewStaticMesh = nullptr;
            Binding->DriverGarmentSlotOverride.Reset();
        }
        if (Fixture->HasField(TEXT("preview_mesh")))
        {
            const FString Path = Fixture->GetStringField(TEXT("preview_mesh"));
            Binding->PreviewStaticMesh = Path.IsEmpty() ? nullptr : LoadObject<UStaticMesh>(nullptr, *Path);
            if (!Path.IsEmpty() && !Binding->PreviewStaticMesh)
            { Test->AddError(TEXT("Could not load fixture Preview Static Mesh: ") + Path); return false; }
        }
        if (Fixture->HasField(TEXT("parts")))
        {
            Binding->AdditionalParts.Reset();
            for (const TSharedPtr<FJsonValue>& Value : Fixture->GetArrayField(TEXT("parts")))
            {
                const TSharedPtr<FJsonObject> PartValue = Value->AsObject();
                if (!PartValue) { Test->AddError(TEXT("Fixture part must be an object")); return false; }
                FMtoUCharacterPart& Part = Binding->AdditionalParts.AddDefaulted_GetRef();
                Part.PartName = PartValue->GetStringField(TEXT("name"));
                const FString Path = PartValue->GetStringField(TEXT("mesh"));
                Part.SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *Path);
                Part.bEnabled = !PartValue->HasField(TEXT("enabled")) || PartValue->GetBoolField(TEXT("enabled"));
                if (!Part.SkeletalMesh)
                { Test->AddError(TEXT("Could not load fixture Additional Part: ") + Path); return false; }
            }
            Binding->EnsureCharacterPartIds();
        }
        bPreviewStages = Binding->PreviewStaticMesh != nullptr;
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
            View->SetRealtime(true); View->ChangeBufferVisualizationMode(FName(TEXT("BaseColor")));
            View->FocusViewportOnBox(Bounds.GetBox().ExpandBy(Bounds.SphereRadius * 0.15), true); View->Invalidate();
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
        if (!Static || !Animation) { Test->AddError(TEXT("Streamed frame is incomplete")); return false; }
        // The subject publishes the character's required bones: the positive
        // skin influences of every enabled mesh plus their ancestors, not one
        // mesh's complete exported hierarchy.
        const FMtoUCharacterComposition Composition =
            FMtoUCharacterComposition::Resolve(Binding.Get());
        if (!Test->TestTrue(TEXT("character composition resolves for pose comparison"), Composition.IsUsable()))
        {
            Test->AddError(Composition.Diagnostics);
            return false;
        }
        const FReferenceSkeleton& Required = Composition.RequiredSkeleton;
        if (!Test->TestEqual(TEXT("published required hierarchy"), Animation->Transforms.Num(), Required.GetNum())
            || !Test->TestEqual(TEXT("published required names"), Static->BoneNames.Num(), Required.GetNum())
            || !Test->TestEqual(TEXT("published required parents"), Static->BoneParents.Num(), Required.GetNum())) { return false; }
        // The Maya peer reports its complete snapshot in Maya order, so each
        // published bone is resolved to its Maya source by parent-scoped short
        // name, exactly like the negotiation; the ready reply's remap list
        // bridges an Unreal import rename.
        const TArray<TSharedPtr<FJsonValue>>& MayaBones = Result->GetArrayField(TEXT("bones"));
        const TArray<TSharedPtr<FJsonValue>>& MayaBind = Result->GetArrayField(TEXT("bind"));
        const TArray<TSharedPtr<FJsonValue>>& MayaCurrent = Result->GetArrayField(TEXT("transforms"));
        TArray<FString> MayaNames;
        TArray<int32> MayaParents;
        for (const TSharedPtr<FJsonValue>& Value : MayaBones)
        {
            const TArray<TSharedPtr<FJsonValue>>& Entry = Value->AsArray();
            MayaNames.Add(Entry.Num() > 0 ? Entry[0]->AsString() : FString());
            MayaParents.Add(Entry.Num() > 1 ? static_cast<int32>(Entry[1]->AsNumber()) : INDEX_NONE);
        }
        if (!Test->TestEqual(TEXT("Maya snapshot carries its hierarchy"), MayaNames.Num(), MayaParents.Num())
            || !Test->TestEqual(TEXT("Maya snapshot carries every bind transform"), MayaBind.Num(), MayaNames.Num())
            || !Test->TestEqual(TEXT("Maya snapshot carries every current transform"), MayaCurrent.Num(), MayaNames.Num()))
        { return false; }
        TMap<FString, TSet<FString>> RenamedFrom;
        if (Result->HasField(TEXT("warning")))
        {
            for (const TSharedPtr<FJsonValue>& Value
                : Result->GetObjectField(TEXT("warning"))->GetArrayField(TEXT("bone_name_remaps")))
            {
                FString MayaPath, UnrealName;
                if (Value->AsString().Split(TEXT(" -> "), &MayaPath, &UnrealName))
                {
                    FString MayaParent, MayaName;
                    if (MayaPath.Split(TEXT("/"), &MayaParent, &MayaName))
                    {
                        RenamedFrom.FindOrAdd(UnrealName).Add(MayaName);
                    }
                }
            }
        }
        TArray<int32> PublishedToMaya;
        PublishedToMaya.Init(INDEX_NONE, Static->BoneNames.Num());
        TSet<int32> ClaimedMaya;
        bool bResolved = true;
        for (int32 I = 0; I < Static->BoneNames.Num() && bResolved; ++I)
        {
            const int32 PublishedParent = Static->BoneParents[I];
            const int32 MayaParent = PublishedParent == INDEX_NONE ? INDEX_NONE : PublishedToMaya[PublishedParent];
            const FString PublishedName = Static->BoneNames[I].ToString();
            TArray<int32> Candidates;
            for (int32 Maya = 0; Maya < MayaNames.Num(); ++Maya)
            {
                if (MayaParents[Maya] != MayaParent || ClaimedMaya.Contains(Maya)) { continue; }
                const TSet<FString>* Sources = RenamedFrom.Find(PublishedName);
                if (MayaNames[Maya] == PublishedName || (Sources && Sources->Contains(MayaNames[Maya])))
                {
                    Candidates.Add(Maya);
                }
            }
            bResolved = Test->TestTrue(FString::Printf(TEXT("published bone %s resolves to one Maya source"),
                *PublishedName), Candidates.Num() == 1)
                && Test->TestTrue(TEXT("published hierarchy keeps the required order and parents"),
                    Static->BoneNames[I] == Required.GetBoneName(I)
                    && PublishedParent == Required.GetParentIndex(I));
            if (bResolved)
            {
                PublishedToMaya[I] = Candidates[0];
                ClaimedMaya.Add(Candidates[0]);
            }
        }
        if (!bResolved) { return false; }
        TArray<FMatrix> BindWorld, CurrentWorld, RequiredWorld, PublishedWorld;
        BindWorld.SetNum(MayaNames.Num());
        CurrentWorld.SetNum(MayaNames.Num());
        for (int32 Maya = 0; Maya < MayaNames.Num(); ++Maya)
        {
            const int32 P = MayaParents[Maya];
            if (P >= Maya || (P >= 0 && !BindWorld.IsValidIndex(P))) { Test->AddError(TEXT("Maya snapshot hierarchy is not parent-first")); return false; }
            BindWorld[Maya] = CharacterTransform(MayaBind[Maya]).ToMatrixWithScale()
                * (P < 0 ? FMatrix::Identity : BindWorld[P]);
            CurrentWorld[Maya] = CharacterTransform(MayaCurrent[Maya]).ToMatrixWithScale()
                * (P < 0 ? FMatrix::Identity : CurrentWorld[P]);
        }
        for (int32 I = 0; I < Required.GetNum(); ++I)
        {
            const int32 P = Required.GetParentIndex(I);
            RequiredWorld.Add(Required.GetRefBonePose()[I].ToMatrixWithScale() * (P < 0 ? FMatrix::Identity : RequiredWorld[P]));
        }
        double MaxPositionError = 0;
        double MaxDisplayedError = 0;
        double MaxAxisError = 0, MaxDisplayedAxisError = 0;
        for (int32 I = 0; I < Static->BoneNames.Num(); ++I)
        {
            const int32 P = Static->BoneParents[I];
            const FName BoneName = Static->BoneNames[I];
            const int32 Maya = PublishedToMaya[I];
            PublishedWorld.Add(Animation->Transforms[I].ToMatrixWithScale()
                * (P < 0 ? FMatrix::Identity : PublishedWorld[P]));
            FMatrix Inverse;
            if (!VectorMatrixInverse(&Inverse, &BindWorld[Maya])) { Test->AddError(TEXT("Fixture bind inverse failed")); return false; }
            const FMatrix Expected = RequiredWorld[I] * Inverse * CurrentWorld[Maya];
            MaxPositionError = FMath::Max(MaxPositionError, FVector::Distance(Expected.GetOrigin(), PublishedWorld[I].GetOrigin()));
            for (EAxis::Type Axis : {EAxis::X, EAxis::Y, EAxis::Z})
            {
                MaxAxisError = FMath::Max(MaxAxisError, FVector::Distance(
                    Expected.GetScaledAxis(Axis).GetSafeNormal(0.0),
                    PublishedWorld[I].GetScaledAxis(Axis).GetSafeNormal(0.0)));
            }
            // Verify every mesh that owns a shared bone, and the owning part
            // for part-only bones. Checking only the Primary would miss a
            // detached Head or Hair component in the real character.
            TArray<const USkeletalMeshComponent*> Displays;
            Displays.Add(Actor->GetSkeletalMeshComponent());
            for (const TObjectPtr<UMtoUCharacterPartComponent>& Part : Actor->GetCharacterPartComponents())
            {
                if (Part) { Displays.Add(Part); }
            }
            bool bDisplayed = false;
            for (const USkeletalMeshComponent* DisplayComponent : Displays)
            {
                if (!DisplayComponent) { continue; }
                const int32 DisplayIndex = DisplayComponent->GetBoneIndex(BoneName);
                if (DisplayIndex == INDEX_NONE) { continue; }
                const TArray<FTransform>& Displayed = DisplayComponent->GetComponentSpaceTransforms();
                if (!Displayed.IsValidIndex(DisplayIndex))
                { Test->AddError(TEXT("Displayed skeleton is incomplete")); return false; }
                bDisplayed = true;
                MaxDisplayedError = FMath::Max(MaxDisplayedError,
                    FVector::Distance(PublishedWorld[I].GetOrigin(), Displayed[DisplayIndex].GetTranslation()));
                const FMatrix DisplayedMatrix = Displayed[DisplayIndex].ToMatrixWithScale();
                for (EAxis::Type Axis : {EAxis::X, EAxis::Y, EAxis::Z})
                {
                    MaxDisplayedAxisError = FMath::Max(MaxDisplayedAxisError, FVector::Distance(
                        PublishedWorld[I].GetScaledAxis(Axis).GetSafeNormal(0.0),
                        DisplayedMatrix.GetScaledAxis(Axis).GetSafeNormal(0.0)));
                }
            }
            if (!bDisplayed) { Test->AddError(TEXT("No displayed component owns a required bone")); return false; }
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
    bool bPreviewStages = true;
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
