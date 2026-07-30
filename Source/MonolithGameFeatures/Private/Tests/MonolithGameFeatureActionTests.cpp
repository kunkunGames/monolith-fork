#include "Misc/AutomationTest.h"
#include "MonolithGameFeatureActions.h"
#include "MonolithSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#ifndef WITH_MONOLITH_GAMEFEATURES
#define WITH_MONOLITH_GAMEFEATURES 0
#endif

#if WITH_MONOLITH_GAMEFEATURES

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGameFeaturesStatusTest,
	"Monolith.GameFeatures.StatusAndReadOnlyGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameFeaturesStatusTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	const bool bOriginalEnabled = Settings->bEnableGameFeatureActions;
	const bool bOriginalCreation = Settings->bAllowGameFeaturePluginCreation;

	Settings->bEnableGameFeatureActions = false;
	Settings->bAllowGameFeaturePluginCreation = false;

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::GetStatus(MakeShared<FJsonObject>());
		TestTrue(TEXT("GetStatus succeeds while inspection disabled"), Result.bSuccess);
		TestTrue(TEXT("GetStatus returns json while inspection disabled"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* AvailableWhenEnabled = nullptr;
			TestFalse(TEXT("Inspection disabled"), Result.Result->GetBoolField(TEXT("inspection_enabled")));
			TestTrue(TEXT("Actions field exists"), Result.Result->TryGetArrayField(TEXT("actions"), Actions));
			TestTrue(TEXT("AvailableWhenEnabled field exists"), Result.Result->TryGetArrayField(TEXT("available_when_enabled"), AvailableWhenEnabled));
			if (Actions)
			{
				TestEqual(TEXT("Default actions include status and nine instanced-action writers"), Actions->Num(), 10);
			}
			if (AvailableWhenEnabled)
			{
				TestEqual(TEXT("Six gated inspection actions reported"), AvailableWhenEnabled->Num(), 6);
			}
		}
	}

	Settings->bEnableGameFeatureActions = true;
	Settings->bAllowGameFeaturePluginCreation = false;

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::GetStatus(MakeShared<FJsonObject>());
		TestTrue(TEXT("GetStatus succeeds"), Result.bSuccess);
		TestTrue(TEXT("GetStatus returns json"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			TestEqual(TEXT("Namespace"), Result.Result->GetStringField(TEXT("namespace")), FString(TEXT("gamefeatures")));
			TestTrue(TEXT("Inspection plus write mode"), Result.Result->GetStringField(TEXT("mode")) == TEXT("inspection_and_instanced_action_writes"));
			TestTrue(TEXT("Write action registered"), Result.Result->GetBoolField(TEXT("write_actions_registered")));
			TestFalse(TEXT("No hard ToolsetRegistry dependency"), Result.Result->GetBoolField(TEXT("hard_toolsetregistry_dependency")));
			TestFalse(TEXT("Creation disabled by default"), Result.Result->GetBoolField(TEXT("creation_allowed")));
		}
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddActionSetInputMapping(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddActionSetInputMapping rejects missing action_set_path"), Result.bSuccess);
		TestEqual(TEXT("AddActionSetInputMapping missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddActionSetComponents(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddActionSetComponents rejects missing action_set_path"), Result.bSuccess);
		TestEqual(TEXT("AddActionSetComponents missing param code"), Result.ErrorCode, -32602);

		const TSharedPtr<FJsonObject> Schema = FMonolithGameFeatureActions::AddActionSetComponentsSchema();
		const TSharedPtr<FJsonObject>* ActionSetPathParam = nullptr;
		const TSharedPtr<FJsonObject>* ActorClassParam = nullptr;
		const TSharedPtr<FJsonObject>* ComponentClassParam = nullptr;
		TestTrue(TEXT("AddActionSetComponents schema requires action_set_path"),
			Schema.IsValid()
				&& Schema->TryGetObjectField(TEXT("action_set_path"), ActionSetPathParam)
				&& ActionSetPathParam
				&& (*ActionSetPathParam)->GetBoolField(TEXT("required")));
		TestTrue(TEXT("AddActionSetComponents schema requires actor_class"),
			Schema.IsValid()
				&& Schema->TryGetObjectField(TEXT("actor_class"), ActorClassParam)
				&& ActorClassParam
				&& (*ActorClassParam)->GetBoolField(TEXT("required")));
		TestTrue(TEXT("AddActionSetComponents schema requires component_class"),
			Schema.IsValid()
				&& Schema->TryGetObjectField(TEXT("component_class"), ComponentClassParam)
				&& ComponentClassParam
				&& (*ComponentClassParam)->GetBoolField(TEXT("required")));
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::SetPrimaryAssetScan(MakeShared<FJsonObject>());
		TestFalse(TEXT("SetPrimaryAssetScan rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("SetPrimaryAssetScan missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataInputMapping(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataInputMapping rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataInputMapping missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataWidgets(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataWidgets rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataWidgets missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataComponents(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataComponents rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataComponents missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataGameplayCuePaths(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataGameplayCuePaths rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataGameplayCuePaths missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataAbilities(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataAbilities rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataAbilities missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::RemoveGameFeatureDataAction(MakeShared<FJsonObject>());
		TestFalse(TEXT("RemoveGameFeatureDataAction rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("RemoveGameFeatureDataAction missing param code"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1);
		Params->SetBoolField(TEXT("include_engine"), false);
		FMonolithActionResult Result = FMonolithGameFeatureActions::ListPlugins(Params);
		TestTrue(TEXT("ListPlugins handles empty projects"), Result.bSuccess);
		TestTrue(TEXT("ListPlugins returns json"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			TestTrue(TEXT("Count field exists"), Result.Result->HasField(TEXT("count")));
			TestTrue(TEXT("Plugins field exists"), Result.Result->HasField(TEXT("plugins")));
		}
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/DefinitelyNotAGameFeatureData"));
		FMonolithActionResult Result = FMonolithGameFeatureActions::FindGameFeatureData(Params);
		TestTrue(TEXT("FindGameFeatureData reports not found as data"), Result.bSuccess);
		TestTrue(TEXT("FindGameFeatureData returns json"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			TestFalse(TEXT("Missing asset not found"), Result.Result->GetBoolField(TEXT("found")));
		}
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 5);
		FMonolithActionResult Result = FMonolithGameFeatureActions::ListActionClasses(Params);
		TestTrue(TEXT("ListActionClasses succeeds"), Result.bSuccess);
		TestTrue(TEXT("ListActionClasses returns json"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			TestTrue(TEXT("Classes field exists"), Result.Result->HasField(TEXT("classes")));
		}
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::DescribeActionSet(MakeShared<FJsonObject>());
		TestFalse(TEXT("DescribeActionSet rejects missing action_set_path"), Result.bSuccess);
		TestEqual(TEXT("DescribeActionSet missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::ValidatePlugin(MakeShared<FJsonObject>());
		TestFalse(TEXT("ValidatePlugin rejects missing plugin_name"), Result.bSuccess);
		TestEqual(TEXT("ValidatePlugin missing param code"), Result.ErrorCode, -32602);
	}

	Settings->bEnableGameFeatureActions = bOriginalEnabled;
	Settings->bAllowGameFeaturePluginCreation = bOriginalCreation;
	return true;
}

#else

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGameFeaturesStatusTest,
	"Monolith.GameFeatures.StatusUnavailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameFeaturesStatusTest::RunTest(const FString& Parameters)
{
	FMonolithActionResult Result = FMonolithGameFeatureActions::GetStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("GetStatus succeeds when optional dependency is not compiled"), Result.bSuccess);
	TestTrue(TEXT("GetStatus returns json"), Result.Result.IsValid());
	if (Result.Result.IsValid())
	{
		TestFalse(TEXT("GameFeatures not compiled"), Result.Result->GetBoolField(TEXT("with_gamefeatures")));
		TestEqual(TEXT("Dependency state"), Result.Result->GetStringField(TEXT("dependency_state")), FString(TEXT("unavailable")));
	}
	return true;
}

#endif
