#if WITH_DEV_AUTOMATION_TESTS
#include "MtoUGuiAcceptance.h"

namespace
{
TSharedPtr<FJsonObject> ParseJson(const FString& Text)
{
    TSharedPtr<FJsonObject> Value;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Value);
    return Value;
}

FString PluginList(int32 Count)
{
    TArray<FString> Names;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        Names.Add(FString::Printf(TEXT("\"plugin%02d\""), Index));
    }
    return TEXT("[") + FString::Join(Names, TEXT(",")) + TEXT("]");
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUMayaPreferencesRecordTest,
    "MtoULiveLink.Editor.GuiAcceptance.MayaPreferencesRecord",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoUMayaPreferencesRecordTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // Maya kept autoloading after the oracle snapshots: 18 plug-ins before
    // playback, 25 while playing and 30 at a later stage.
    const FString Prefs = TEXT("\"prefs\":{\"plugin_prefs\":\"2024/zh_CN/prefs/pluginPrefs.mel\",")
        TEXT("\"sha256\":\"abc123\",\"autoload\":[\"modelingToolkit\",\"MASH\",\"xgenToolkit\"]}");
    const TSharedPtr<FJsonObject> Result = ParseJson(FString::Printf(
        TEXT("{%s,\"plugins\":{\"stage\":\"close_world\",\"loaded\":%s},")
        TEXT("\"plugin_samples\":{\"oracle\":{\"stage\":\"realtime\",\"loaded\":%s,\"elapsed_s\":30.0},")
        TEXT("\"playback\":{\"stage\":\"realtime\",\"loaded\":%s,\"elapsed_s\":41.5}}}"),
        *Prefs, *PluginList(30), *PluginList(18), *PluginList(25)));
    TestTrue(TEXT("the fixture result parses"), Result.IsValid());

    const TSharedRef<FJsonObject> Record = MtoUGuiAcceptance::MayaPreferencesRecord(
        TEXT("<scratch>/maya-prefs-minimal"), TEXT("<evidence>/mayaapp"), Result, TEXT("playback"));
    TestEqual(TEXT("the source is the template"), Record->GetStringField(TEXT("source")), FString(TEXT("template")));
    TestEqual(TEXT("the hash is the one Maya started with"),
        Record->GetStringField(TEXT("plugin_prefs_sha256")), FString(TEXT("abc123")));
    TestEqual(TEXT("the autoload count comes from pluginPrefs.mel"),
        Record->GetNumberField(TEXT("autoload_plugins")), 3.0);
    TestEqual(TEXT("the loaded count is the playback sample, not the oracle or the latest stage"),
        Record->GetNumberField(TEXT("loaded_plugins")), 25.0);
    TestEqual(TEXT("the sample keeps its stage"),
        Record->GetStringField(TEXT("sampled_at_stage")), FString(TEXT("realtime")));
    TestEqual(TEXT("the sample keeps its time"), Record->GetNumberField(TEXT("sampled_at_seconds")), 41.5);
    TestTrue(TEXT("the log line names the playback sample"),
        MtoUGuiAcceptance::DescribeMayaPreferences(Record).Contains(TEXT("25 plug-ins loaded at sample 'playback'")));

    const TSharedRef<FJsonObject> Missing = MtoUGuiAcceptance::MayaPreferencesRecord(
        FString(), TEXT("<maya-app-dir>"), Result, TEXT("absent"));
    TestEqual(TEXT("a missing sample is reported, not replaced by the latest stage"),
        Missing->GetNumberField(TEXT("loaded_plugins")), -1.0);
    TestEqual(TEXT("an app_dir used in place is the source"), Missing->GetStringField(TEXT("source")),
        FString(TEXT("app_dir")));
    TestTrue(TEXT("the log line says the sample is missing"),
        MtoUGuiAcceptance::DescribeMayaPreferences(Missing).Contains(TEXT("no 'absent' plug-in sample")));

    const TSharedRef<FJsonObject> NoList = MtoUGuiAcceptance::MayaPreferencesRecord(
        FString(), TEXT("<maya-app-dir>"), ParseJson(TEXT("{\"prefs\":{\"plugin_prefs\":null,\"sha256\":null,\"autoload\":null}}")),
        TEXT("playback"));
    TestEqual(TEXT("no saved list leaves the autoload count unknown"),
        NoList->GetNumberField(TEXT("autoload_plugins")), -1.0);
    TestTrue(TEXT("no saved list reads as Maya's default set"),
        MtoUGuiAcceptance::DescribeMayaPreferences(NoList).Contains(TEXT("none (Maya's default set)")));
    return true;
}
#endif
