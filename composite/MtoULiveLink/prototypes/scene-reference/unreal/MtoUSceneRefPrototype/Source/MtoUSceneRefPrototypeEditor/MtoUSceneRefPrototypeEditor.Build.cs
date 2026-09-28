// MtoU scene reference prototype (Issue 53 verification). Editor-only prototype code.

using UnrealBuildTool;

public class MtoUSceneRefPrototypeEditor : ModuleRules
{
	public MtoUSceneRefPrototypeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"EditorScriptingUtilities",
			"Json",
			"JsonUtilities",
			"Landscape",
			"LevelEditor",
			"MaterialEditor",
			"SubobjectDataInterface",
			"UnrealEd",
		});
	}
}
