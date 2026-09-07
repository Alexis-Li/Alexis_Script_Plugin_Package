using UnrealBuildTool;

public class MtoULiveLinkEditor : ModuleRules
{
    public MtoULiveLinkEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "MtoULiveLink" });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AssetRegistry", "DynamicMesh", "EditorFramework", "Engine", "GeometryCore", "GeometryFramework",
            "GeometryScriptingCore", "Json", "LiveLinkAnimationCore", "LiveLinkInterface", "MeshDescription", "PropertyEditor",
            "SkeletalMeshDescription", "Slate", "SlateCore", "Sockets", "StaticMeshDescription", "UnrealEd"
        });
        // Editor-private bridge for the Refresh-during-session integration
        // test: the Runtime source lives in its Private dir, so only this
        // Editor module's tests may include it, never production headers.
        PrivateIncludePaths.Add(ModuleDirectory + "/../MtoULiveLink/Private");
    }
}
