using UnrealBuildTool;
using System.IO;

public class MonolithAI : ModuleRules
{
	public MonolithAI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Always-available engine AI modules
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine",
			"AIModule", "GameplayTasks", "GameplayTags", "NavigationSystem"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore", "MonolithBlueprint", "MonolithIndex",
			"UnrealEd", "BlueprintGraph", "AIGraph",
			"BehaviorTreeEditor", "EnvironmentQueryEditor",
			"Projects",  // IPluginManager (Phase D2)
			"Json", "JsonUtilities",
			"SQLiteCore"
		});

		// --- Conditional optional deps ---
		// MONOLITH_RELEASE_BUILD=1 forces all optional plugin deps OFF so binary
		// release zips never hard-link against plugins the end-user may not have
		// enabled. Mirrors the pattern in MonolithGAS.Build.cs / MonolithUI.Build.cs.
		// Origin: GitHub issue #30 (MonolithMesh.dll hard-linked GeometryScriptingCore).
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		// Hoist engine paths — shared across all conditional probes
		string EngineDir = "";
		string EnginePluginsDir = "";
		if (!bReleaseBuild)
		{
			EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
		}

		// --- Conditional: StateTree (engine plugin, EnabledByDefault=false) ---
		// StateTree itself contains StateTreeModule + StateTreeEditorModule (UncookedOnly).
		// GameplayStateTree is a SEPARATE engine plugin that depends on StateTree and
		// supplies StateTreeAIComponent / BT-to-StateTree task bridge.
		// PropertyBindingUtils is also a separate engine plugin required by
		// StateTree's binding/instance-data system. We gate the StateTree family
		// together under bHasStateTree because StateTree's own .uplugin lists
		// PropertyBindingUtils as a required dep and GameplayStateTree.uplugin
		// requires StateTree. Without StateTree the others are dead weight.
		// (Historical: the StructUtils plugin was previously listed here but is
		// deprecated since UE 5.5 — FInstancedStruct relocated into CoreUObject
		// and resolves transparently via the existing CoreUObject Public dep above.)
		bool bHasStateTree = false;
		if (!bReleaseBuild)
		{
			// 1. Engine Plugins/Runtime/StateTree (canonical UE 5.7 layout — confirmed)
			if (Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "StateTree")))
			{
				bHasStateTree = true;
			}
			// 2. Engine Plugins/AI/StateTree (alternate layout / future relocation)
			else if (Directory.Exists(Path.Combine(EnginePluginsDir, "AI", "StateTree")))
			{
				bHasStateTree = true;
			}
			// 3. Project Plugins/StateTree (manual install)
			else if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(
					Target.ProjectFile.Directory.FullName, "Plugins");
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasStateTree = Directory.Exists(
						Path.Combine(ProjectPluginsDir, "StateTree"));
				}
			}
		}

		if (bHasStateTree)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"StateTreeModule", "StateTreeEditorModule",
				"GameplayStateTreeModule",
				"PropertyBindingUtils"
			});
			PublicDefinitions.Add("WITH_STATETREE=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_STATETREE=0");
		}

		// --- Conditional: SmartObjects (engine plugin, EnabledByDefault=false) ---
		// SmartObjects plugin contains SmartObjectsModule + SmartObjectsEditorModule.
		// Both gated together — editor module is always co-installed with runtime.
		bool bHasSmartObjects = false;
		if (!bReleaseBuild)
		{
			// 1. Engine Plugins/Runtime/SmartObjects (canonical UE 5.7 layout — confirmed)
			if (Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "SmartObjects")))
			{
				bHasSmartObjects = true;
			}
			// 2. Engine Plugins/AI/SmartObjects (alternate layout / future relocation)
			else if (Directory.Exists(Path.Combine(EnginePluginsDir, "AI", "SmartObjects")))
			{
				bHasSmartObjects = true;
			}
			// 3. Project Plugins/SmartObjects (manual install)
			else if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(
					Target.ProjectFile.Directory.FullName, "Plugins");
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasSmartObjects = Directory.Exists(
						Path.Combine(ProjectPluginsDir, "SmartObjects"));
				}
			}
		}

		if (bHasSmartObjects)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"SmartObjectsModule",
				"SmartObjectsEditorModule"
			});
			PublicDefinitions.Add("WITH_SMARTOBJECTS=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_SMARTOBJECTS=0");
		}

		// --- Conditional: GameplayAbilities (Phase I2: BT-to-GAS task) ---
		// Engine plugin shipped with UE 5.7 by default. We probe the disk so a
		// project that disables the plugin entirely still compiles MonolithAI.
		// 3-location probe mirrors the canonical Build.cs pattern in
		// MonolithGAS.Build.cs.
		bool bHasGameplayAbilities = false;
		if (!bReleaseBuild)
		{
			// 1. Engine Plugins/Runtime/GameplayAbilities (canonical UE 5.7 layout)
			if (Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "GameplayAbilities")))
			{
				bHasGameplayAbilities = true;
			}
			// 2. Engine Plugins/GameplayAbilities (older layout / project install)
			else if (Directory.Exists(Path.Combine(EnginePluginsDir, "GameplayAbilities")))
			{
				bHasGameplayAbilities = true;
			}
			// 3. Project Plugins/ (manual install)
			else if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(
					Target.ProjectFile.Directory.FullName, "Plugins");
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasGameplayAbilities = Directory.Exists(
						Path.Combine(ProjectPluginsDir, "GameplayAbilities"));
				}
			}
		}

		if (bHasGameplayAbilities)
		{
			PublicDependencyModuleNames.Add("GameplayAbilities");
			PublicDefinitions.Add("WITH_GAMEPLAYABILITIES=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_GAMEPLAYABILITIES=0");
		}

		// --- Conditional: GameplayBehaviors (Experimental) ---
		bool bHasGameplayBehaviors = false;
		if (!bReleaseBuild)
		{
			if (Directory.Exists(Path.Combine(EnginePluginsDir, "GameplayBehaviors")))
			{
				bHasGameplayBehaviors = true;
			}
			else
			{
				string[] SearchDirs = new string[]
				{
					Path.Combine(EnginePluginsDir, "Runtime"),
					Path.Combine(EnginePluginsDir, "Experimental"),
					Path.Combine(EnginePluginsDir, "AI")
				};
				foreach (string Dir in SearchDirs)
				{
					if (Directory.Exists(Dir) &&
						Directory.GetDirectories(Dir, "GameplayBehaviors*", SearchOption.TopDirectoryOnly).Length > 0)
					{
						bHasGameplayBehaviors = true;
						break;
					}
				}
			}
		}

		if (bHasGameplayBehaviors)
		{
			PrivateDependencyModuleNames.Add("GameplayBehaviorsModule");
			PublicDefinitions.Add("WITH_GAMEPLAYBEHAVIORS=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_GAMEPLAYBEHAVIORS=0");
		}

		// --- Conditional: MassEntity (Experimental) ---
		bool bHasMassEntity = false;
		if (!bReleaseBuild)
		{
			string[] MassSearchDirs = new string[]
			{
				EnginePluginsDir,
				Path.Combine(EnginePluginsDir, "Runtime"),
				Path.Combine(EnginePluginsDir, "AI")
			};
			foreach (string Dir in MassSearchDirs)
			{
				if (Directory.Exists(Dir) &&
					Directory.GetDirectories(Dir, "MassEntity*", SearchOption.TopDirectoryOnly).Length > 0)
				{
					bHasMassEntity = true;
					break;
				}
			}
		}

		if (bHasMassEntity)
		{
			// MassGameplayEditor lives in Runtime/MassGameplay — assumed co-installed with MassEntity
			PrivateDependencyModuleNames.AddRange(new string[] { "MassEntity", "MassSpawner", "MassGameplayEditor" });
			PublicDefinitions.Add("WITH_MASSENTITY=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_MASSENTITY=0");
		}

		// --- Conditional: ZoneGraph (Experimental) ---
		bool bHasZoneGraph = false;
		if (!bReleaseBuild)
		{
			string[] ZoneSearchDirs = new string[]
			{
				EnginePluginsDir,
				Path.Combine(EnginePluginsDir, "Runtime"),
				Path.Combine(EnginePluginsDir, "Experimental")
			};
			foreach (string Dir in ZoneSearchDirs)
			{
				if (Directory.Exists(Dir) &&
					Directory.GetDirectories(Dir, "ZoneGraph*", SearchOption.TopDirectoryOnly).Length > 0)
				{
					bHasZoneGraph = true;
					break;
				}
			}
		}

		if (bHasZoneGraph)
		{
			PrivateDependencyModuleNames.Add("ZoneGraph");
			PublicDefinitions.Add("WITH_ZONEGRAPH=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_ZONEGRAPH=0");
		}
	}
}
