using UnrealBuildTool;

public class {{PLUGIN_NAME}}Editor : ModuleRules
{
    public {{PLUGIN_NAME}}Editor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
    }
}
