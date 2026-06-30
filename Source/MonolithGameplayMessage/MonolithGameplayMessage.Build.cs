using UnrealBuildTool;

public class MonolithGameplayMessage : ModuleRules
{
	public MonolithGameplayMessage(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"GameplayTags",
				"Json",
				"JsonUtilities",
				"MonolithCore",
				"Projects"
			});
	}
}
