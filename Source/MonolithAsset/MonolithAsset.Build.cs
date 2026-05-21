using UnrealBuildTool;

public class MonolithAsset : ModuleRules
{
	public MonolithAsset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MonolithCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			"AssetTools",
			"EditorScriptingUtilities",
			"ImageWrapper",
			"ImageCore",
			"RenderCore",
			"RHI",
			"SlateCore"
		});
	}
}
