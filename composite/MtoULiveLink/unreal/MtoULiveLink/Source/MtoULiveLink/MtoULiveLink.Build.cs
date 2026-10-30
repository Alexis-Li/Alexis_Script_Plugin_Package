using UnrealBuildTool;

public class MtoULiveLink : ModuleRules
{
    public MtoULiveLink(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Json" });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Json", "LiveLinkAnimationCore", "LiveLinkInterface", "Sockets"
        });
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
            // Automation constructs CPU skin-weight LOD fixtures.
            PrivateDependencyModuleNames.AddRange(new[] { "RenderCore", "RHI" });
        }
    }
}
