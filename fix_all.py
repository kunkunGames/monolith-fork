import sys

def replace(file, search, rep):
    with open(file, "r") as f:
        content = f.read()
    content = content.replace(search, rep)
    with open(file, "w") as f:
        f.write(content)

replace("Source/MonolithBABridge/MonolithBABridge.Build.cs", "ProjectPluginsDir, \"BlueprintAssist*\",", "ProjectPluginsDir, \"BlueprintAssist_*\",")
replace("Source/MonolithComboGraph/MonolithComboGraph.Build.cs", "ProjectPluginsDir, \"ComboGraph*\",", "ProjectPluginsDir, \"ComboGraph_*\",")
replace("Source/MonolithGAS/MonolithGAS.Build.cs", "ProjectPluginsDir, \"BlueprintAttributes*\",", "ProjectPluginsDir, \"BlueprintAttributes_*\",")
replace("Source/MonolithLogicDriver/MonolithLogicDriver.Build.cs", "ProjectPluginsDir, \"LogicDriver*\",", "ProjectPluginsDir, \"LogicDriver_*\",")
replace("Source/MonolithMesh/MonolithMesh.Build.cs", "ProjectPluginsDir, \"GeometryScripting*\",", "ProjectPluginsDir, \"GeometryScripting_*\",")

# Now handle the MarketplaceDir ones which need the exact check first
def fix_marketplace(file, var_name, plugin_name):
    with open(file, "r") as f:
        content = f.read()

    # Example search:
    # 					bHasBlueprintAssist = Directory.GetDirectories(
    #						MarketplaceDir, "BlueprintAssist*",
    #						SearchOption.TopDirectoryOnly).Length > 0;

    search_str = f"""					{var_name} = Directory.GetDirectories(
						MarketplaceDir, "{plugin_name}*",
						SearchOption.TopDirectoryOnly).Length > 0;"""

    replace_str = f"""					{var_name} = Directory.Exists(
						Path.Combine(MarketplaceDir, "{plugin_name}"));

					if (!{var_name})
					{{
						{var_name} = Directory.GetDirectories(
							MarketplaceDir, "{plugin_name}_*",
							SearchOption.TopDirectoryOnly).Length > 0;
					}}"""

    content = content.replace(search_str, replace_str)
    with open(file, "w") as f:
        f.write(content)

fix_marketplace("Source/MonolithBABridge/MonolithBABridge.Build.cs", "bHasBlueprintAssist", "BlueprintAssist")
fix_marketplace("Source/MonolithComboGraph/MonolithComboGraph.Build.cs", "bHasComboGraph", "ComboGraph")
fix_marketplace("Source/MonolithGAS/MonolithGAS.Build.cs", "bHasGBA", "BlueprintAttributes")
fix_marketplace("Source/MonolithLogicDriver/MonolithLogicDriver.Build.cs", "bHasLogicDriver", "LogicDriver")
