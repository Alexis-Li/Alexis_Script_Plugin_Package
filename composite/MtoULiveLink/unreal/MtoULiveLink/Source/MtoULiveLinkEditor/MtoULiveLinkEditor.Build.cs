using UnrealBuildTool;

public class MtoULiveLinkEditor : ModuleRules
{
    public MtoULiveLinkEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "MtoULiveLink" });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AssetRegistry", "DynamicMesh", "Engine", "GeometryCore", "GeometryFramework",
            "GeometryScriptingCore", "MeshDescription", "PropertyEditor",
            "SkeletalMeshDescription", "Slate", "SlateCore", "StaticMeshDescription", "UnrealEd"
        });
    }
}
