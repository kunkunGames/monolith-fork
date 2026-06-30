using UnrealBuildTool;

public class MonolithLyra : ModuleRules
{
	public MonolithLyra(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",
			"UnrealEd",
			"AssetRegistry",
			"GameplayTags",
			"Json",
			"JsonUtilities",
			"Projects"
		});
	}
}
