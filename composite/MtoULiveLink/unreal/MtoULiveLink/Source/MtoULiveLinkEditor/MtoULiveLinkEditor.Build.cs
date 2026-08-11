using UnrealBuildTool;

public class MtoULiveLinkEditor : ModuleRules
{
    public MtoULiveLinkEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AssetRegistry", "Core", "CoreUObject", "Engine", "MtoULiveLink", "UnrealEd"
        });
    }
}
