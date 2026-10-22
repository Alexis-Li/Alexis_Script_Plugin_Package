using UnrealBuildTool;

public class MtoULiveLinkEditor : ModuleRules
{
    public MtoULiveLinkEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Json", "MtoULiveLink" });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ApplicationCore", "AssetRegistry", "CinematicCamera", "DynamicMesh", "EditorFramework", "Engine", "GeometryCore", "GeometryFramework",
            "GeometryScriptingCore", "ImageCore", "Json", "LevelSequence", "LevelSequenceEditor", "LiveLinkAnimationCore", "LiveLinkInterface", "MeshDescription",
            "MovieScene", "MovieSceneTracks", "PropertyEditor", "RenderCore", "Sequencer", "SkeletalMeshDescription", "Slate", "SlateCore", "Sockets",
            "StaticMeshDescription", "UMG", "UnrealEd"
        });
        // Editor-private bridge for the Refresh-during-session integration
        // test: the Runtime source lives in its Private dir, so only this
        // Editor module's tests may include it, never production headers.
        PrivateIncludePaths.Add(ModuleDirectory + "/../MtoULiveLink/Private");
    }
}
