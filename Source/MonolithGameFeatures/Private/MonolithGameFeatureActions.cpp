#include "MonolithGameFeatureActions.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"
#include "PluginReferenceDescriptor.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UnrealType.h"

namespace MonolithGameFeatures
{
	static const FTopLevelAssetPath GameFeatureDataClassPath(TEXT("/Script/GameFeatures"), TEXT("GameFeatureData"));

	struct FGameFeaturePluginInfo
	{
		FString Name;
		FString FriendlyName;
		FString DescriptorPath;
		FString BaseDir;
		FString ContentDir;
		FString MountedAssetPath;
		bool bEnabled = false;
		bool bCanContainContent = false;
		bool bFromEngine = false;
		bool bLooksLikeGameFeature = false;
		bool bDeclaresGameFeaturesDependency = false;
		bool bDescriptorHasGameFeatureState = false;
		TSharedPtr<FJsonObject> DescriptorJson;
	};

	static int32 ClampLimit(double LimitValue)
	{
		return FMath::Clamp(static_cast<int32>(LimitValue), 1, 200);
	}

	static FString NormalizeFilename(FString Path)
	{
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	static FString RedactPath(const FString& Path)
	{
		FString Normalized = NormalizeFilename(Path);
		const FString ProjectDir = NormalizeFilename(FPaths::ProjectDir());
		const FString EngineDir = NormalizeFilename(FPaths::EngineDir());

		// FPaths::MakePathRelativeTo returns true even when the path is not a
		// descendant of the base (it emits leading "../" segments). Only treat
		// the result as project/engine-relative when it stays inside that root;
		// otherwise host filesystem segments (e.g. /Users/<name>/...) leak to
		// MCP clients. Anything outside both roots is replaced with a
		// non-revealing placeholder rather than the absolute path.
		FString Relative = Normalized;
		if (FPaths::MakePathRelativeTo(Relative, *ProjectDir) && !Relative.StartsWith(TEXT("..")))
		{
			return TEXT("<project>/") + Relative;
		}
		Relative = Normalized;
		if (FPaths::MakePathRelativeTo(Relative, *EngineDir) && !Relative.StartsWith(TEXT("..")))
		{
			return TEXT("<engine>/") + Relative;
		}
		return TEXT("<external>");
	}

	static bool JsonHasGameFeatureState(const TSharedPtr<FJsonObject>& DescriptorJson)
	{
		return DescriptorJson.IsValid()
			&& (DescriptorJson->HasField(TEXT("BuiltInInitialFeatureState"))
				|| DescriptorJson->HasField(TEXT("GameFeatureData"))
				|| DescriptorJson->HasField(TEXT("ExplicitlyLoaded")));
	}

	static bool DescriptorDeclaresGameFeaturesDependency(const TSharedRef<IPlugin>& Plugin)
	{
		for (const FPluginReferenceDescriptor& Dep : Plugin->GetDescriptor().Plugins)
		{
			if (Dep.Name == TEXT("GameFeatures") && Dep.bEnabled)
			{
				return true;
			}
		}
		return false;
	}

	static bool PathLooksLikeGameFeature(const FString& Path)
	{
		const FString Normalized = NormalizeFilename(Path).ToLower();
		return Normalized.Contains(TEXT("/plugins/gamefeatures/"));
	}

	static FGameFeaturePluginInfo MakePluginInfo(const TSharedRef<IPlugin>& Plugin)
	{
		FGameFeaturePluginInfo Info;
		Info.Name = Plugin->GetName();
		Info.FriendlyName = Plugin->GetFriendlyName();
		Info.DescriptorPath = Plugin->GetDescriptorFileName();
		Info.BaseDir = Plugin->GetBaseDir();
		Info.ContentDir = Plugin->GetContentDir();
		Info.MountedAssetPath = Plugin->GetMountedAssetPath();
		Info.bEnabled = Plugin->IsEnabled();
		Info.bCanContainContent = Plugin->CanContainContent();
		Info.bFromEngine = Plugin->GetLoadedFrom() == EPluginLoadedFrom::Engine;
#if WITH_EDITOR
		Info.DescriptorJson = Plugin->GetDescriptorJson();
#endif
		Info.bDeclaresGameFeaturesDependency = DescriptorDeclaresGameFeaturesDependency(Plugin);
		Info.bDescriptorHasGameFeatureState = JsonHasGameFeatureState(Info.DescriptorJson);
		Info.bLooksLikeGameFeature = Info.bDeclaresGameFeaturesDependency
			|| Info.bDescriptorHasGameFeatureState
			|| PathLooksLikeGameFeature(Info.DescriptorPath)
			|| PathLooksLikeGameFeature(Info.BaseDir);
		return Info;
	}

	static TArray<FGameFeaturePluginInfo> DiscoverPlugins(bool bIncludeEngine)
	{
		TArray<FGameFeaturePluginInfo> Plugins;
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
		{
			FGameFeaturePluginInfo Info = MakePluginInfo(Plugin);
			if (!Info.bLooksLikeGameFeature)
			{
				continue;
			}
			if (Info.bFromEngine && !bIncludeEngine)
			{
				continue;
			}
			Plugins.Add(MoveTemp(Info));
		}
		Plugins.Sort([](const FGameFeaturePluginInfo& A, const FGameFeaturePluginInfo& B)
		{
			return A.Name < B.Name;
		});
		return Plugins;
	}

	static TArray<FAssetData> GetGameFeatureDataAssets(const FString& PackagePath = FString())
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(GameFeatureDataClassPath);
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		if (!PackagePath.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PackagePath));
		}

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);

		// Filter.bRecursiveClasses already restricts results to GameFeatureData
		// and its subclasses. An exact-class post-filter would drop project
		// defined UGameFeatureData subclasses, making valid plugins look like
		// they ship no GameFeatureData asset, so no further filtering is done.

		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.PackageName.LexicalLess(B.PackageName);
		});
		return Assets;
	}

	static FString NormalizeAssetPath(FString AssetPath)
	{
		AssetPath.TrimStartAndEndInline();
		if (AssetPath.IsEmpty())
		{
			return FString();
		}
		if (AssetPath.Contains(TEXT(".")))
		{
			FString PackagePart;
			FString ObjectPart;
			AssetPath.Split(TEXT("."), &PackagePart, &ObjectPart, ESearchCase::CaseSensitive, ESearchDir::FromStart);
			return PackagePart;
		}
		return AssetPath;
	}

	static FString GetPackagePathForPlugin(const FGameFeaturePluginInfo& Plugin)
	{
		if (!Plugin.MountedAssetPath.IsEmpty())
		{
			FString Mounted = Plugin.MountedAssetPath;
			Mounted.RemoveFromEnd(TEXT("/"));
			return Mounted;
		}
		return FString::Printf(TEXT("/%s"), *Plugin.Name);
	}

	static TArray<FAssetData> FindGameFeatureDataForPlugin(const FGameFeaturePluginInfo& Plugin)
	{
		return GetGameFeatureDataAssets(GetPackagePathForPlugin(Plugin));
	}

	static TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& AssetData)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.GetAssetName().ToString());
		Row->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), AssetData.IsAssetLoaded());
		return Row;
	}

	static TSharedPtr<FJsonObject> PluginToJson(const FGameFeaturePluginInfo& Plugin, const TArray<FAssetData>* DataAssets = nullptr)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Plugin.Name);
		Row->SetStringField(TEXT("friendly_name"), Plugin.FriendlyName.IsEmpty() ? Plugin.Name : Plugin.FriendlyName);
		Row->SetStringField(TEXT("descriptor_path"), RedactPath(Plugin.DescriptorPath));
		Row->SetStringField(TEXT("base_dir"), RedactPath(Plugin.BaseDir));
		Row->SetStringField(TEXT("content_dir"), RedactPath(Plugin.ContentDir));
		Row->SetStringField(TEXT("content_root"), GetPackagePathForPlugin(Plugin));
		Row->SetBoolField(TEXT("enabled"), Plugin.bEnabled);
		Row->SetBoolField(TEXT("engine_plugin"), Plugin.bFromEngine);
		Row->SetBoolField(TEXT("can_contain_content"), Plugin.bCanContainContent);
		Row->SetBoolField(TEXT("declares_gamefeatures_dependency"), Plugin.bDeclaresGameFeaturesDependency);
		Row->SetBoolField(TEXT("descriptor_has_game_feature_state"), Plugin.bDescriptorHasGameFeatureState);

		if (DataAssets)
		{
			TArray<TSharedPtr<FJsonValue>> Assets;
			for (const FAssetData& AssetData : *DataAssets)
			{
				Assets.Add(MakeShared<FJsonValueObject>(AssetDataToJson(AssetData)));
			}
			Row->SetArrayField(TEXT("game_feature_data_assets"), Assets);
			Row->SetNumberField(TEXT("game_feature_data_count"), Assets.Num());
			if (DataAssets->Num() > 0)
			{
				Row->SetStringField(TEXT("game_feature_data"), (*DataAssets)[0].PackageName.ToString());
			}
		}
		return Row;
	}

	static bool TryGetPluginName(const TSharedPtr<FJsonObject>& Params, FString& OutPluginName, FMonolithActionResult& OutError)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("plugin_name"), OutPluginName) || OutPluginName.IsEmpty())
		{
			OutError = FMonolithActionResult::Error(TEXT("Missing required param 'plugin_name'"), -32602);
			return false;
		}
		return true;
	}

	static bool TryFindPluginByName(const FString& PluginName, FGameFeaturePluginInfo& OutPlugin)
	{
		for (const FGameFeaturePluginInfo& Plugin : DiscoverPlugins(true))
		{
			if (Plugin.Name.Equals(PluginName, ESearchCase::IgnoreCase))
			{
				OutPlugin = Plugin;
				return true;
			}
		}
		return false;
	}

	static bool TryResolveGameFeatureData(const TSharedPtr<FJsonObject>& Params, FAssetData& OutAsset, FString& OutPluginName, FString& OutError)
	{
		FString AssetPath;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("asset_path"), AssetPath);
			Params->TryGetStringField(TEXT("plugin_name"), OutPluginName);
		}

		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!NormalizedAssetPath.IsEmpty())
		{
			for (const FAssetData& Asset : GetGameFeatureDataAssets())
			{
				if (Asset.PackageName.ToString().Equals(NormalizedAssetPath, ESearchCase::IgnoreCase)
					|| Asset.GetObjectPathString().Equals(AssetPath, ESearchCase::IgnoreCase))
				{
					OutAsset = Asset;
					return true;
				}
			}
			OutError = FString::Printf(TEXT("No GameFeatureData asset found at '%s'"), *AssetPath);
			return false;
		}

		if (!OutPluginName.IsEmpty())
		{
			FGameFeaturePluginInfo Plugin;
			if (!TryFindPluginByName(OutPluginName, Plugin))
			{
				OutError = FString::Printf(TEXT("No GameFeature plugin named '%s' was found"), *OutPluginName);
				return false;
			}
			TArray<FAssetData> Assets = FindGameFeatureDataForPlugin(Plugin);
			if (Assets.Num() == 0)
			{
				OutError = FString::Printf(TEXT("Plugin '%s' has no indexed GameFeatureData asset under %s"), *OutPluginName, *GetPackagePathForPlugin(Plugin));
				return false;
			}
			// Prefer the descriptor-declared GameFeatureData asset before any
			// heuristic candidate; a multi-data plugin must not silently
			// resolve to an arbitrary Assets[0] that the .uplugin did not name.
			if (Plugin.DescriptorJson.IsValid())
			{
				FString DeclaredPath;
				if (Plugin.DescriptorJson->TryGetStringField(TEXT("GameFeatureData"), DeclaredPath)
					&& !DeclaredPath.IsEmpty())
				{
					const FString DeclaredPackage = NormalizeAssetPath(DeclaredPath);
					for (const FAssetData& Asset : Assets)
					{
						if (Asset.PackageName.ToString().Equals(DeclaredPackage, ESearchCase::IgnoreCase)
							|| Asset.GetObjectPathString().Equals(DeclaredPath, ESearchCase::IgnoreCase))
						{
							OutAsset = Asset;
							return true;
						}
					}
				}
			}
			OutAsset = Assets[0];
			return true;
		}

		OutError = TEXT("Provide either 'plugin_name' or 'asset_path'");
		return false;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static TSharedPtr<FJsonObject> MakeCheck(const FString& Name, bool bOk, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("name"), Name);
		Check->SetBoolField(TEXT("ok"), bOk);
		Check->SetStringField(TEXT("detail"), Detail);
		return Check;
	}

	static TSharedPtr<FJsonObject> MakeStatusJson()
	{
		const UMonolithSettings* Settings = UMonolithSettings::Get();
		const bool bEnabled = Settings && Settings->bEnableGameFeatureActions;
		const bool bCreationAllowed = bEnabled && Settings && Settings->bAllowGameFeaturePluginCreation;
		const TArray<FGameFeaturePluginInfo> Plugins = DiscoverPlugins(false);
		const TArray<FString> AlwaysActions = {
			TEXT("get_status")
		};
		const TArray<FString> InspectionActions = {
			TEXT("list_plugins"),
			TEXT("find_game_feature_data"),
			TEXT("describe_game_feature_data"),
			TEXT("validate_plugin")
		};
		TArray<FString> RegisteredActions = AlwaysActions;
		if (bEnabled)
		{
			RegisteredActions.Append(InspectionActions);
		}
		TArray<FString> ImplementedActions = AlwaysActions;
		ImplementedActions.Append(InspectionActions);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("gamefeatures"));
		Result->SetStringField(TEXT("mode"), TEXT("read_only"));
		Result->SetBoolField(TEXT("enabled"), bEnabled);
		Result->SetBoolField(TEXT("inspection_enabled"), bEnabled);
		Result->SetBoolField(TEXT("creation_allowed"), bCreationAllowed);
		Result->SetBoolField(TEXT("hard_toolsetregistry_dependency"), false);
		Result->SetBoolField(TEXT("gamefeatures_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("GameFeatures")));
		Result->SetBoolField(TEXT("gamefeatures_editor_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("GameFeaturesEditor")));
		Result->SetNumberField(TEXT("plugin_count"), Plugins.Num());
		Result->SetArrayField(TEXT("scan_roots"), StringArrayToJson({
			RedactPath(FPaths::ProjectPluginsDir() / TEXT("GameFeatures"))
		}));
		Result->SetArrayField(TEXT("actions"), StringArrayToJson(RegisteredActions));
		Result->SetArrayField(TEXT("registered_actions"), StringArrayToJson(RegisteredActions));
		Result->SetArrayField(TEXT("available_when_enabled"), StringArrayToJson(bEnabled ? TArray<FString>() : InspectionActions));
		Result->SetArrayField(TEXT("implemented_actions"), StringArrayToJson(ImplementedActions));
		Result->SetArrayField(TEXT("future_reserved_actions"), StringArrayToJson({
			TEXT("plan_plugin_creation"),
			TEXT("create_plugin")
		}));
		return Result;
	}
}

void FMonolithGameFeatureActions::Register(FMonolithToolRegistry& Registry, bool bEnableInspectionActions)
{
	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("get_status"),
		TEXT("Report read-only GameFeatures inspection availability, flags, module status, and discovered plugin count."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::GetStatus),
		EmptySchema());

	if (!bEnableInspectionActions)
	{
		return;
	}

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("list_plugins"),
		TEXT("List GameFeature-style plugins using plugin descriptors and AssetRegistry metadata. Read-only; no plugin activation or file writes."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::ListPlugins),
		ListPluginsSchema());

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("find_game_feature_data"),
		TEXT("Resolve a GameFeature plugin name or asset path to bounded GameFeatureData AssetRegistry metadata without loading arbitrary paths."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::FindGameFeatureData),
		FindGameFeatureDataSchema());

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("describe_game_feature_data"),
		TEXT("Load and summarize one GameFeatureData asset by plugin name or asset path, including bounded reflected action summaries."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::DescribeGameFeatureData),
		DescribeGameFeatureDataSchema());

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("validate_plugin"),
		TEXT("Validate a GameFeature plugin descriptor, content root, GameFeatureData asset, and creation gate state. Read-only."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::ValidatePlugin),
		ValidatePluginSchema());
}

FMonolithActionResult FMonolithGameFeatureActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithActionResult::Success(MonolithGameFeatures::MakeStatusJson());
}

FMonolithActionResult FMonolithGameFeatureActions::ListPlugins(const TSharedPtr<FJsonObject>& Params)
{
	double LimitValue = 50.0;
	bool bIncludeEngine = false;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("limit"), LimitValue);
		Params->TryGetBoolField(TEXT("include_engine"), bIncludeEngine);
	}
	const int32 Limit = MonolithGameFeatures::ClampLimit(LimitValue);
	const TArray<MonolithGameFeatures::FGameFeaturePluginInfo> Plugins = MonolithGameFeatures::DiscoverPlugins(bIncludeEngine);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	for (const MonolithGameFeatures::FGameFeaturePluginInfo& Plugin : Plugins)
	{
		++MatchedCount;
		if (Rows.Num() >= Limit)
		{
			continue;
		}
		const TArray<FAssetData> DataAssets = MonolithGameFeatures::FindGameFeatureDataForPlugin(Plugin);
		Rows.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::PluginToJson(Plugin, &DataAssets)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("include_engine"), bIncludeEngine);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("plugins"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::FindGameFeatureData(const TSharedPtr<FJsonObject>& Params)
{
	FAssetData ResolvedAsset;
	FString PluginName;
	FString Error;
	const bool bFound = MonolithGameFeatures::TryResolveGameFeatureData(Params, ResolvedAsset, PluginName, Error);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("found"), bFound);
	if (!PluginName.IsEmpty())
	{
		Result->SetStringField(TEXT("plugin_name"), PluginName);
	}
	if (bFound)
	{
		Result->SetObjectField(TEXT("game_feature_data"), MonolithGameFeatures::AssetDataToJson(ResolvedAsset));
	}
	else
	{
		Result->SetStringField(TEXT("reason"), Error);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::DescribeGameFeatureData(const TSharedPtr<FJsonObject>& Params)
{
	FAssetData ResolvedAsset;
	FString PluginName;
	FString Error;
	if (!MonolithGameFeatures::TryResolveGameFeatureData(Params, ResolvedAsset, PluginName, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UObject* Asset = ResolvedAsset.GetAsset();
	if (!Asset)
	{
		return FMonolithActionResult::Error(TEXT("Failed to load GameFeatureData asset"), -32603);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!PluginName.IsEmpty())
	{
		Result->SetStringField(TEXT("plugin_name"), PluginName);
	}
	Result->SetObjectField(TEXT("asset"), MonolithGameFeatures::AssetDataToJson(ResolvedAsset));
	Result->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	Result->SetStringField(TEXT("class_path"), Asset->GetClass()->GetClassPathName().ToString());

	TArray<TSharedPtr<FJsonValue>> Actions;
	int32 TotalActionCount = 0;
	if (const FArrayProperty* ActionsProperty = FindFProperty<FArrayProperty>(Asset->GetClass(), TEXT("Actions")))
	{
		const void* ArrayPtr = ActionsProperty->ContainerPtrToValuePtr<void>(Asset);
		FScriptArrayHelper Helper(ActionsProperty, ArrayPtr);
		TotalActionCount = Helper.Num();
		const int32 Limit = FMath::Min(TotalActionCount, 50);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			TSharedPtr<FJsonObject> ActionJson = MakeShared<FJsonObject>();
			ActionJson->SetNumberField(TEXT("index"), Index);

			UObject* ActionObject = nullptr;
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(ActionsProperty->Inner))
			{
				ActionObject = ObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index));
			}

			if (ActionObject)
			{
				ActionJson->SetStringField(TEXT("class"), ActionObject->GetClass()->GetName());
				ActionJson->SetStringField(TEXT("class_path"), ActionObject->GetClass()->GetClassPathName().ToString());
				int32 PropertyCount = 0;
				for (TFieldIterator<FProperty> It(ActionObject->GetClass()); It; ++It)
				{
					++PropertyCount;
				}
				ActionJson->SetNumberField(TEXT("property_count"), PropertyCount);
			}
			else
			{
				ActionJson->SetStringField(TEXT("class"), TEXT("null"));
			}
			Actions.Add(MakeShared<FJsonValueObject>(ActionJson));
		}
	}

	Result->SetNumberField(TEXT("action_count"), TotalActionCount);
	Result->SetBoolField(TEXT("actions_truncated"), TotalActionCount > Actions.Num());
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetBoolField(TEXT("raw_object_graph_dumped"), false);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::ValidatePlugin(const TSharedPtr<FJsonObject>& Params)
{
	FString PluginName;
	FMonolithActionResult Error = FMonolithActionResult::Error(TEXT("Missing required param 'plugin_name'"), -32602);
	if (!MonolithGameFeatures::TryGetPluginName(Params, PluginName, Error))
	{
		return Error;
	}

	MonolithGameFeatures::FGameFeaturePluginInfo Plugin;
	if (!MonolithGameFeatures::TryFindPluginByName(PluginName, Plugin))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No GameFeature plugin named '%s' was found"), *PluginName), -32602);
	}

	const TArray<FAssetData> DataAssets = MonolithGameFeatures::FindGameFeatureDataForPlugin(Plugin);
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const bool bCreationAllowed = Settings && Settings->bEnableGameFeatureActions && Settings->bAllowGameFeaturePluginCreation;

	TArray<TSharedPtr<FJsonValue>> Checks;
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("descriptor"),
		FPaths::FileExists(Plugin.DescriptorPath),
		MonolithGameFeatures::RedactPath(Plugin.DescriptorPath))));
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("gamefeatures_dependency"),
		Plugin.bDeclaresGameFeaturesDependency,
		Plugin.bDeclaresGameFeaturesDependency ? TEXT("Descriptor declares enabled GameFeatures plugin dependency") : TEXT("Descriptor does not declare enabled GameFeatures plugin dependency"))));
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("content_root"),
		Plugin.bCanContainContent && !Plugin.MountedAssetPath.IsEmpty(),
		Plugin.MountedAssetPath.IsEmpty() ? TEXT("Plugin has no mounted asset path") : Plugin.MountedAssetPath)));
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("game_feature_data"),
		DataAssets.Num() > 0,
		DataAssets.Num() > 0 ? DataAssets[0].PackageName.ToString() : TEXT("No GameFeatureData asset found under plugin content root"))));
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("creation_gate"),
		!bCreationAllowed,
		bCreationAllowed ? TEXT("Creation flags are enabled, but no creation action is registered in this first slice") : TEXT("Creation disabled in this read-only slice"))));

	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (!Plugin.bDeclaresGameFeaturesDependency)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Descriptor was identified by path or metadata hint; it does not declare an enabled GameFeatures plugin dependency.")));
	}
	if (!Plugin.bDescriptorHasGameFeatureState)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Descriptor does not expose BuiltInInitialFeatureState/GameFeatureData metadata.")));
	}
	if (DataAssets.Num() > 1)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Multiple GameFeatureData assets found; callers should pass asset_path to inspect a specific asset.")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("plugin_name"), Plugin.Name);
	Result->SetBoolField(TEXT("ok"), FPaths::FileExists(Plugin.DescriptorPath) && Plugin.bDeclaresGameFeaturesDependency && Plugin.bCanContainContent && DataAssets.Num() > 0);
	Result->SetObjectField(TEXT("plugin"), MonolithGameFeatures::PluginToJson(Plugin, &DataAssets));
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetArrayField(TEXT("next_actions"), MonolithGameFeatures::StringArrayToJson({
		TEXT("gamefeatures.describe_game_feature_data"),
		TEXT("gamefeatures.list_plugins")
	}));
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::EmptySchema()
{
	return FParamSchemaBuilder().Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::ListPluginsSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum plugins to return, clamped to 1..200"), TEXT("50"))
		.Optional(TEXT("include_engine"), TEXT("bool"), TEXT("Include engine GameFeature plugins; default only reports project/external plugins"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::FindGameFeatureDataSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("plugin_name"), TEXT("string"), TEXT("GameFeature plugin name to inspect"))
		.Optional(TEXT("asset_path"), TEXT("string"), TEXT("GameFeatureData package or object path"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::DescribeGameFeatureDataSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("plugin_name"), TEXT("string"), TEXT("GameFeature plugin name to inspect"))
		.Optional(TEXT("asset_path"), TEXT("string"), TEXT("GameFeatureData package or object path"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::ValidatePluginSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("plugin_name"), TEXT("string"), TEXT("GameFeature plugin name to validate"))
		.Build();
}
