using UnrealBuildTool;

public class MonolithSlate : ModuleRules
{
	public MonolithSlate(ReadOnlyTargetRules Target) : base(Target)
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
			"Slate",
			"SlateCore",
			"Json",
			"JsonUtilities",
			"ImageCore",
			"ImageWrapper"
		});
	}
}
