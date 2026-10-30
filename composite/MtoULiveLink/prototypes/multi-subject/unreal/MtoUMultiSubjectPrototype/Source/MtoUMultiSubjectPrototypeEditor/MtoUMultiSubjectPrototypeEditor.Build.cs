// MtoU multi-subject prototype (Issue 54 verification). Editor-only prototype code.

using UnrealBuildTool;

public class MtoUMultiSubjectPrototypeEditor : ModuleRules
{
	public MtoUMultiSubjectPrototypeEditor(ReadOnlyTargetRules Target) : base(Target)
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
			// The synthetic fixture writes Skeletal Meshes and Morph Targets from
			// generated geometry, the same way the product's Editor fixtures do.
			"AnimationCore",
			"DynamicMesh",
			"GeometryCore",
			"GeometryFramework",
			"GeometryScriptingCore",
			"MeshDescription",
			"SkeletalMeshDescription",
			// Wire contract and evidence files.
			"Json",
			"JsonUtilities",
			// Real TCP listener.
			"Networking",
			"Sockets",
			// Existing animation / Level Sequence writers the preview takes over from.
			"LevelSequence",
			"LevelSequenceEditor",
			"MovieScene",
			"MovieSceneTracks",
			"Sequencer",
			"UnrealEd",
		});
	}
}
