#pragma once

#if WITH_DEV_AUTOMATION_TESTS
#include "MtoULiveLinkActor.h"

#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "IDetailsView.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Slate/WidgetRenderer.h"
#include "Styling/AppStyle.h"
#include "TextureResource.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

/**
 * Shared pieces of the opt-in acceptance tests that drive a real Maya GUI
 * through `maya_gui_linkage_driver.py` and render the Binding Actor's Details.
 */
namespace MtoUGuiAcceptance
{
inline TSharedPtr<FJsonObject> ReadJsonFile(const FString& Path)
{
    FString Text;
    TSharedPtr<FJsonObject> Value;
    if (FFileHelper::LoadFileToString(Text, *Path))
    {
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Value);
    }
    return Value;
}

inline void WriteJsonFile(const FString& Path, const TSharedRef<FJsonObject>& Value)
{
    FString Text;
    FJsonSerializer::Serialize(Value, TJsonWriterFactory<>::Create(&Text));
    FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

/**
 * One Maya GUI process running the driver. Commands go through a command
 * file the driver polls; the driver's result file carries its latest state
 * and the id of the last command it finished.
 */
class FMayaGuiDriver
{
public:
    /** Starts Maya with the fixture's `maya` object; paths go into the evidence directory. */
    bool Start(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& MayaFixture, const FString& EvidenceDir)
    {
        CommandPath = FPaths::Combine(EvidenceDir, TEXT("maya-command.json"));
        ResultPath = FPaths::Combine(EvidenceDir, TEXT("maya-result.json"));
        FFileHelper::SaveStringToFile(TEXT("{}"), *CommandPath);
        FFileHelper::SaveStringToFile(TEXT("{\"phase\":\"not-started\"}"), *ResultPath);
        MayaFixture->SetStringField(TEXT("command"), CommandPath);
        MayaFixture->SetStringField(TEXT("result"), ResultPath);
        MayaFixture->SetStringField(TEXT("evidence"), EvidenceDir);
        const FString DriverFixture = FPaths::Combine(EvidenceDir, TEXT("maya-driver-fixture.json"));
        WriteJsonFile(DriverFixture, MayaFixture.ToSharedRef());
        FPlatformMisc::SetEnvironmentVar(TEXT("MTOU_LINKAGE_DRIVER_FIXTURE"), *DriverFixture);
        FPlatformMisc::SetEnvironmentVar(TEXT("MAYA_APP_DIR"), *MayaFixture->GetStringField(TEXT("app_dir")));
        const FString Args = FString::Printf(
            TEXT("-log \"%s\" -command \"python(\\\"import runpy; runpy.run_path(r'%s')\\\")\""),
            *FPaths::Combine(EvidenceDir, TEXT("maya.log")), *MayaFixture->GetStringField(TEXT("driver")));
        Process = FPlatformProcess::CreateProc(*MayaFixture->GetStringField(TEXT("exe")), *Args,
            true, true, false, nullptr, 0, nullptr, nullptr);
        return Test.TestTrue(TEXT("Maya starts through the driver"), Process.IsValid());
    }

    /** Re-reads the driver's result; keeps the last complete one while it is being written. */
    const TSharedPtr<FJsonObject>& Poll()
    {
        if (TSharedPtr<FJsonObject> Latest = ReadJsonFile(ResultPath))
        {
            Result = Latest;
        }
        return Result;
    }

    const TSharedPtr<FJsonObject>& GetResult() const { return Result; }

    int32 Send(const FString& Action, const TSharedPtr<FJsonObject>& Fields = nullptr)
    {
        TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>();
        if (Fields.IsValid())
        {
            Command->Values = Fields->Values;
        }
        Command->SetNumberField(TEXT("id"), ++CommandId);
        Command->SetStringField(TEXT("action"), Action);
        WriteJsonFile(CommandPath, Command);
        return CommandId;
    }

    bool Acked(int32 Id) const
    {
        int32 Ack = -1;
        return Result.IsValid() && Result->TryGetNumberField(TEXT("ack"), Ack) && Ack >= Id;
    }

    bool IsRunning() { return Process.IsValid() && FPlatformProcess::IsProcRunning(Process); }

    /** Stops only the Maya process this test started. */
    void Terminate()
    {
        if (IsRunning())
        {
            FPlatformProcess::TerminateProc(Process, true);
        }
    }

private:
    FString CommandPath;
    FString ResultPath;
    FProcHandle Process;
    TSharedPtr<FJsonObject> Result;
    int32 CommandId = 0;
};

/**
 * The weight each display of the actor shows for one Morph target, read where
 * the displays' own verification reads it. Displays without the target are
 * skipped, so an empty result means no display owns it.
 */
inline TArray<double> DisplayedMorphWeights(const AMtoULiveLinkActor& Actor, const FName Name)
{
    TArray<double> Weights;
    TArray<USkeletalMeshComponent*> Displays;
    Actor.GetComponents(Displays);
    for (const USkeletalMeshComponent* Display : Displays)
    {
        const USkeletalMesh* Mesh = Display ? Display->GetSkeletalMeshAsset() : nullptr;
        const UMorphTarget* Target = Mesh ? Mesh->FindMorphTarget(Name) : nullptr;
        if (Target)
        {
            const int32* WeightIndex = Display->ActiveMorphTargets.Find(Target);
            Weights.Add(WeightIndex && Display->MorphTargetWeights.IsValidIndex(*WeightIndex)
                ? Display->MorphTargetWeights[*WeightIndex] : 0.0);
        }
    }
    return Weights;
}

/**
 * Renders the Binding Actor's Details at each width into
 * `details-<state>-<width>.png` and records every text that cannot wrap and
 * is painted narrower than it needs. Only a run with a real RHI can render
 * (-RenderOffscreen, not -NullRHI); otherwise this logs that it was skipped.
 * Returns the number of clipped texts.
 */
inline int32 RenderDetails(FAutomationTestBase& Test, AMtoULiveLinkActor& Actor,
    const TArray<TSharedPtr<FJsonValue>>& Widths, double Height, const FString& EvidenceDir,
    const FString& State, TFunctionRef<void(const FString&)> Log)
{
    if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
    {
        Log(TEXT("Details renders skipped: this run cannot render (NullRHI)"));
        return 0;
    }
    FPropertyEditorModule& PropertyEditor =
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
    TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
    int32 Clipped = 0;
    for (const TSharedPtr<FJsonValue>& WidthValue : Widths)
    {
        const int32 Width = static_cast<int32>(WidthValue->AsNumber());
        FDetailsViewArgs Args;
        Args.bHideSelectionTip = true;
        Args.bLockable = false;
        Args.bUpdatesFromSelection = false;
        TSharedRef<IDetailsView> View = PropertyEditor.CreateDetailView(Args);
        View->SetObject(&Actor);
        TSharedRef<SWidget> Panel = SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush(TEXT("Brushes.Panel")))
            .Padding(0.0f)
            [View];
        const FVector2D Size(Width, Height);
        FWidgetRenderer Renderer(true, false);
        UTextureRenderTarget2D* Target = FWidgetRenderer::CreateTargetFor(Size, TF_Bilinear, true);
        // The tree builds and lays out over several frames.
        for (int32 Pass = 0; Pass < 6; ++Pass)
        {
            Renderer.DrawWidget(Target, Panel, Size, 1.0f / 30.0f);
        }
        FlushRenderingCommands();
        TArray<FColor> Pixels;
        Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        const FString Path = FPaths::Combine(EvidenceDir, FString::Printf(TEXT("details-%s-%d.png"), *State, Width));
        const bool bSaved = Pixels.Num() == Width * static_cast<int32>(Height)
            && FImageUtils::SaveImageByExtension(*Path, FImageView(Pixels.GetData(), Width, static_cast<int32>(Height)));
        Test.TestTrue(*FString::Printf(TEXT("Details %s render at %d px is saved"), *State, Width), bSaved);

        // The engine's own asset pickers elide their combo text by design and
        // show the full path on hover; only the remaining texts must fit.
        TArray<TSharedPtr<FJsonValue>> ClippedTexts;
        TArray<TPair<TSharedRef<SWidget>, bool>> Pending{{Panel, false}};
        while (Pending.Num() > 0)
        {
            const TPair<TSharedRef<SWidget>, bool> Item = Pending.Pop();
            const TSharedRef<SWidget>& Widget = Item.Key;
            const bool bInCombo = Item.Value || Widget->GetType() == FName(TEXT("SComboButton"));
            if (!bInCombo && Widget->GetType() == FName(TEXT("STextBlock")) && Widget->GetVisibility().IsVisible())
            {
                TSharedRef<STextBlock> Text = StaticCastSharedRef<STextBlock>(Widget);
                const float Painted = Widget->GetPaintSpaceGeometry().GetLocalSize().X;
                const float Needed = Text->GetDesiredSize().X;
                if (Painted > 0.0f && Needed > Painted + 1.0f && !Text->GetText().IsEmpty())
                {
                    ClippedTexts.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s (%.0f of %.0f px)"),
                        *Text->GetText().ToString().Left(80), Painted, Needed)));
                }
            }
            FChildren* Children = Widget->GetChildren();
            for (int32 Child = 0; Children && Child < Children->Num(); ++Child)
            {
                Pending.Add({Children->GetChildAt(Child), bInCombo});
            }
        }
        Clipped += ClippedTexts.Num();
        Record->SetArrayField(FString::Printf(TEXT("%d"), Width), ClippedTexts);
        Log(FString::Printf(TEXT("Details %s at %d px: %s, %d clipped texts"),
            *State, Width, bSaved ? *Path : TEXT("not saved"), ClippedTexts.Num()));
    }
    WriteJsonFile(FPaths::Combine(EvidenceDir, FString::Printf(TEXT("details-%s.json"), *State)), Record);
    Test.TestEqual(*FString::Printf(TEXT("no Details text is clipped (%s)"), *State), Clipped, 0);
    return Clipped;
}
}
#endif
