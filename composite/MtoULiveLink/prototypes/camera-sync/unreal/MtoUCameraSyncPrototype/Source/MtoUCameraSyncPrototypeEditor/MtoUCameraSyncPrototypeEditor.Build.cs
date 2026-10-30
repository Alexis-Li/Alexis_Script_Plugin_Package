// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MtoUCameraSyncPrototypeEditor : ModuleRules
{
	public MtoUCameraSyncPrototypeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"CinematicCamera",
			"Json",
			"JsonUtilities",
			"LevelSequence",
			"LevelSequenceEditor",
			"MovieScene",
			"MovieSceneTracks",
			"Networking",
			"Sockets",
			"Sequencer",
			"UnrealEd",
		});
	}
}
