using UnrealBuildTool;

public class MonolithLoading : ModuleRules
{
	public MonolithLoading(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
				"MonolithCore",
				"Projects"
			});
	}
}
