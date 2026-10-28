#pragma once

#if WITH_DEV_AUTOMATION_TESTS
#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkActorDetails.h"

#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
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
    // Maya's driver reads the command file continuously; a write that meets
    // that read fails on Windows, and a lost command would stall the test.
    for (int32 Attempt = 0; Attempt < 50; ++Attempt)
    {
        if (FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            return;
        }
        FPlatformProcess::Sleep(0.02f);
    }
}

/**
 * Where Maya's preferences came from, what the `pluginPrefs.mel` Maya started with
 * loads automatically (the driver reads it before Maya can save it again) and the
 * plug-ins the driver's `sample_plugins` command found loaded under `SampleLabel`.
 * Without that sample `loaded_plugins` stays -1: the latest stage's count is never
 * reported in its place.
 */
inline TSharedRef<FJsonObject> MayaPreferencesRecord(const FString& Template, const FString& AppDir,
    const TSharedPtr<FJsonObject>& MayaResult, const FString& SampleLabel)
{
    TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
    Record->SetStringField(TEXT("source"), Template.IsEmpty() ? TEXT("app_dir") : TEXT("template"));
    Record->SetStringField(TEXT("template"), Template);
    Record->SetStringField(TEXT("app_dir"), AppDir);
    Record->SetStringField(TEXT("plugin_prefs"), FString());
    Record->SetStringField(TEXT("plugin_prefs_sha256"), FString());
    Record->SetNumberField(TEXT("autoload_plugins"), -1);
    Record->SetStringField(TEXT("sample"), SampleLabel);
    Record->SetNumberField(TEXT("loaded_plugins"), -1);
    const TSharedPtr<FJsonObject>* Prefs = nullptr;
    if (MayaResult.IsValid() && MayaResult->TryGetObjectField(TEXT("prefs"), Prefs))
    {
        FString Value;
        if ((*Prefs)->TryGetStringField(TEXT("plugin_prefs"), Value))
        {
            Record->SetStringField(TEXT("plugin_prefs"), Value);
        }
        if ((*Prefs)->TryGetStringField(TEXT("sha256"), Value))
        {
            Record->SetStringField(TEXT("plugin_prefs_sha256"), Value);
        }
        const TArray<TSharedPtr<FJsonValue>>* Autoload = nullptr;
        if ((*Prefs)->TryGetArrayField(TEXT("autoload"), Autoload))
        {
            Record->SetNumberField(TEXT("autoload_plugins"), Autoload->Num());
        }
    }
    const TSharedPtr<FJsonObject>* Samples = nullptr;
    const TSharedPtr<FJsonObject>* Sample = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Loaded = nullptr;
    if (MayaResult.IsValid() && MayaResult->TryGetObjectField(TEXT("plugin_samples"), Samples)
        && (*Samples)->TryGetObjectField(SampleLabel, Sample)
        && (*Sample)->TryGetArrayField(TEXT("loaded"), Loaded))
    {
        Record->SetNumberField(TEXT("loaded_plugins"), Loaded->Num());
        Record->SetStringField(TEXT("sampled_at_stage"), (*Sample)->GetStringField(TEXT("stage")));
        Record->SetNumberField(TEXT("sampled_at_seconds"), (*Sample)->GetNumberField(TEXT("elapsed_s")));
    }
    return Record;
}

/** One log line for a MayaPreferencesRecord(). */
inline FString DescribeMayaPreferences(const TSharedRef<FJsonObject>& Record)
{
    const FString Template = Record->GetStringField(TEXT("template"));
    const FString Source = Template.IsEmpty()
        ? FString(TEXT("app_dir used in place"))
        : FString::Printf(TEXT("template %s copied"), *Template);
    FString Sha = Record->GetStringField(TEXT("plugin_prefs_sha256"));
    if (Sha.IsEmpty())
    {
        Sha = TEXT("none (Maya's default set)");
    }
    const FString Label = Record->GetStringField(TEXT("sample"));
    FString Stage;
    const FString Loaded = Record->TryGetStringField(TEXT("sampled_at_stage"), Stage)
        ? FString::Printf(TEXT("%d plug-ins loaded at sample '%s' (stage '%s', %.1f s after the driver started)"),
            static_cast<int32>(Record->GetNumberField(TEXT("loaded_plugins"))), *Label, *Stage,
            Record->GetNumberField(TEXT("sampled_at_seconds")))
        : FString::Printf(TEXT("no '%s' plug-in sample"), *Label);
    return FString::Printf(TEXT("Maya preferences: %s, MAYA_APP_DIR %s; pluginPrefs.mel sha256 %s, %d autoload plug-ins; %s"),
        *Source, *Record->GetStringField(TEXT("app_dir")), *Sha,
        static_cast<int32>(Record->GetNumberField(TEXT("autoload_plugins"))), *Loaded);
}

/**
 * One Maya GUI process running the driver. Commands go through a command
 * file the driver polls; the driver's result file carries its latest state
 * and the id of the last command it finished.
 */
class FMayaGuiDriver
{
public:
    /**
     * Starts Maya with the fixture's `maya` object; paths go into the evidence directory.
     * With `app_dir_template` the template is copied to `<evidence>/mayaapp` and Maya
     * starts from that copy, so every run begins with the same preferences; otherwise
     * Maya uses `app_dir` in place and saves its preferences back into it.
     */
    bool Start(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& MayaFixture, const FString& EvidenceDir)
    {
        if (!PreparePreferences(Test, MayaFixture, EvidenceDir))
        {
            return false;
        }
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
        FPlatformMisc::SetEnvironmentVar(TEXT("MAYA_APP_DIR"), *AppDir);
        // A listener moved off the default port is where this Maya connects.
        FString Port;
        if (FParse::Value(FCommandLine::Get(), TEXT("MtoULiveLinkPort="), Port))
        {
            FPlatformMisc::SetEnvironmentVar(TEXT("MTOU_LIVELINK_PORT"), *Port);
        }
        const FString Args = FString::Printf(
            TEXT("-log \"%s\" -command \"python(\\\"import runpy; runpy.run_path(r'%s')\\\")\""),
            *FPaths::Combine(EvidenceDir, TEXT("maya.log")), *MayaFixture->GetStringField(TEXT("driver")));
        Process = FPlatformProcess::CreateProc(*MayaFixture->GetStringField(TEXT("exe")), *Args,
            true, true, false, &ProcessId, 0, nullptr, nullptr);
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

    /** Working set of the Maya process this test started, in bytes; 0 when unknown. */
    uint64 MemoryBytes() const
    {
        SIZE_T Bytes = 0;
        return ProcessId != 0 && FPlatformProcess::GetApplicationMemoryUsage(ProcessId, &Bytes) ? Bytes : 0;
    }

    /** Stops only the Maya process this test started. */
    void Terminate()
    {
        if (IsRunning())
        {
            FPlatformProcess::TerminateProc(Process, true);
        }
    }

    /** MayaPreferencesRecord() for this run, with the plug-ins sampled under `SampleLabel`. */
    TSharedRef<FJsonObject> PreferencesRecord(const FString& SampleLabel) const
    {
        return MayaPreferencesRecord(PrefsTemplate, AppDir, Result, SampleLabel);
    }

    FString DescribePreferences(const FString& SampleLabel) const
    {
        return DescribeMayaPreferences(PreferencesRecord(SampleLabel));
    }

private:
    bool PreparePreferences(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& MayaFixture,
        const FString& EvidenceDir)
    {
        PrefsTemplate.Reset();
        if (MayaFixture->TryGetStringField(TEXT("app_dir_template"), PrefsTemplate) && !PrefsTemplate.IsEmpty())
        {
            PrefsTemplate = FPaths::ConvertRelativePathToFull(PrefsTemplate);
            AppDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(EvidenceDir, TEXT("mayaapp")));
            if (!Test.TestTrue(TEXT("the Maya preference template exists"),
                    IFileManager::Get().DirectoryExists(*PrefsTemplate))
                || !Test.TestFalse(TEXT("the Maya preference template lies outside the evidence copy"),
                    FPaths::IsUnderDirectory(PrefsTemplate, AppDir) || FPaths::IsUnderDirectory(AppDir, PrefsTemplate)))
            {
                return false;
            }
            IFileManager::Get().DeleteDirectory(*AppDir, false, true);
            if (!Test.TestTrue(TEXT("the Maya preference template is copied for this run"),
                    FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*AppDir, *PrefsTemplate, true)))
            {
                return false;
            }
            MayaFixture->SetStringField(TEXT("app_dir"), AppDir);
            return true;
        }
        PrefsTemplate.Reset();
        AppDir.Reset();
        MayaFixture->TryGetStringField(TEXT("app_dir"), AppDir);
        return Test.TestFalse(TEXT("the fixture names app_dir or app_dir_template"), AppDir.IsEmpty());
    }

    FString PrefsTemplate;
    FString AppDir;
    FString CommandPath;
    FString ResultPath;
    FProcHandle Process;
    uint32 ProcessId = 0;
    TSharedPtr<FJsonObject> Result;
    int32 CommandId = 0;
};

/** One dialog a driver stage may legitimately show, by diagnostic code. */
struct FExpectedDialog
{
    FString Stage;
    FString Code;
};

/**
 * Fails the test for every Maya dialog the scenario did not expect. The driver
 * records each dialog with the stage that was current when it appeared; a
 * dialog is expected only when `Allowed` lists its stage and diagnostic code.
 * Returns the number of unexpected dialogs.
 */
inline int32 CheckMayaDialogs(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& MayaResult,
    const TArray<FExpectedDialog>& Allowed, TFunctionRef<void(const FString&)> Log)
{
    const TArray<TSharedPtr<FJsonValue>>* Dialogs = nullptr;
    if (!MayaResult.IsValid() || !MayaResult->TryGetArrayField(TEXT("dialogs"), Dialogs))
    {
        return 0;
    }
    int32 Unexpected = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Dialogs)
    {
        const TSharedPtr<FJsonObject> Dialog = Value->AsObject();
        FString Stage, Code, Summary;
        Dialog->TryGetStringField(TEXT("stage"), Stage);
        const TSharedPtr<FJsonObject>* Diagnostic = nullptr;
        if (Dialog->TryGetObjectField(TEXT("diagnostic"), Diagnostic) && Diagnostic->IsValid())
        {
            (*Diagnostic)->TryGetStringField(TEXT("code"), Code);
            (*Diagnostic)->TryGetStringField(TEXT("details"), Summary);
        }
        const bool bExpected = Allowed.ContainsByPredicate([&Stage, &Code](const FExpectedDialog& Entry)
        {
            return Entry.Stage == Stage && Entry.Code == Code;
        });
        Log(FString::Printf(TEXT("Maya dialog at stage '%s': %s (%s)"), *Stage, *Code,
            bExpected ? TEXT("expected") : TEXT("UNEXPECTED")));
        if (!bExpected)
        {
            ++Unexpected;
            Test.AddError(FString::Printf(TEXT("Unexpected Maya dialog at stage '%s': %s: %s"),
                *Stage, *Code, *Summary.Left(300)));
        }
    }
    return Unexpected;
}

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
 * `details-<state>-<width>.png` with every MtoU group expanded, tall enough to
 * hold all rows (`Height` is the starting height; the render grows until the
 * last painted text fits), and records every text that cannot wrap and is
 * painted narrower than it needs. Only a run with a real RHI can render
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
    constexpr int32 MaxHeight = 8192;
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
        // The groups start expanded only for the panel built here.
        FMtoULiveLinkActorDetails::bExpandAllGroups = true;
        View->SetObject(&Actor);
        TSharedRef<SWidget> Panel = SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush(TEXT("Brushes.Panel")))
            .Padding(0.0f)
            [View];
        FWidgetRenderer Renderer(true, false);
        UTextureRenderTarget2D* Target = nullptr;
        int32 PaintedTexts = 0;
        int32 UsedHeight = FMath::Clamp(static_cast<int32>(Height), 200, MaxHeight);
        TArray<TSharedPtr<FJsonValue>> ClippedTexts;
        for (;;)
        {
            const FVector2D Size(Width, UsedHeight);
            Target = FWidgetRenderer::CreateTargetFor(Size, TF_Bilinear, true);
            // The tree builds and lays out over several frames.
            for (int32 Pass = 0; Pass < 8; ++Pass)
            {
                Renderer.DrawWidget(Target, Panel, Size, 1.0f / 30.0f);
            }

            // The engine's own asset pickers elide their combo text by design and
            // show the full path on hover; only the remaining texts must fit.
            ClippedTexts.Reset();
            PaintedTexts = 0;
            float Bottom = 0.0f;
            const float Top = Panel->GetPaintSpaceGeometry().GetAbsolutePosition().Y;
            TArray<TPair<TSharedRef<SWidget>, bool>> Pending{{Panel, false}};
            while (Pending.Num() > 0)
            {
                const TPair<TSharedRef<SWidget>, bool> Item = Pending.Pop();
                const TSharedRef<SWidget>& Widget = Item.Key;
                const bool bInCombo = Item.Value || Widget->GetType() == FName(TEXT("SComboButton"));
                if (Widget->GetType() == FName(TEXT("STextBlock")) && Widget->GetVisibility().IsVisible())
                {
                    TSharedRef<STextBlock> Text = StaticCastSharedRef<STextBlock>(Widget);
                    const FGeometry& Geometry = Widget->GetPaintSpaceGeometry();
                    const float Painted = Geometry.GetLocalSize().X;
                    if (Painted > 0.0f && !Text->GetText().IsEmpty())
                    {
                        ++PaintedTexts;
                        Bottom = FMath::Max(Bottom, Geometry.GetAbsolutePosition().Y - Top + Geometry.GetLocalSize().Y);
                        const float Needed = Text->GetDesiredSize().X;
                        if (!bInCombo && Needed > Painted + 1.0f)
                        {
                            ClippedTexts.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s (%.0f of %.0f px)"),
                                *Text->GetText().ToString().Left(80), Painted, Needed)));
                        }
                    }
                }
                FChildren* Children = Widget->GetChildren();
                for (int32 Child = 0; Children && Child < Children->Num(); ++Child)
                {
                    Pending.Add({Children->GetChildAt(Child), bInCombo});
                }
            }
            // Rows are generated only inside the viewport, so a last text
            // close to the bottom edge means more rows may follow.
            if (Bottom < UsedHeight - 80 || UsedHeight >= MaxHeight)
            {
                if (Bottom >= UsedHeight - 80)
                {
                    Test.AddError(FString::Printf(
                        TEXT("Details %s at %d px needs more than %d px; rows below it were not checked"),
                        *State, Width, MaxHeight));
                }
                break;
            }
            UsedHeight = FMath::Min(UsedHeight * 2, MaxHeight);
        }
        FMtoULiveLinkActorDetails::bExpandAllGroups = false;
        FlushRenderingCommands();
        TArray<FColor> Pixels;
        Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        const FString Path = FPaths::Combine(EvidenceDir, FString::Printf(TEXT("details-%s-%d.png"), *State, Width));
        const bool bSaved = Pixels.Num() == Width * UsedHeight
            && FImageUtils::SaveImageByExtension(*Path, FImageView(Pixels.GetData(), Width, UsedHeight));
        Test.TestTrue(*FString::Printf(TEXT("Details %s render at %d px is saved"), *State, Width), bSaved);

        Clipped += ClippedTexts.Num();
        Record->SetArrayField(FString::Printf(TEXT("%d"), Width), ClippedTexts);
        Log(FString::Printf(TEXT("Details %s at %d px: %s, %d px tall, %d texts checked, %d clipped texts"),
            *State, Width, bSaved ? *Path : TEXT("not saved"), UsedHeight, PaintedTexts, ClippedTexts.Num()));
    }
    WriteJsonFile(FPaths::Combine(EvidenceDir, FString::Printf(TEXT("details-%s.json"), *State)), Record);
    Test.TestEqual(*FString::Printf(TEXT("no Details text is clipped (%s)"), *State), Clipped, 0);
    return Clipped;
}
}
#endif
