#include "Misc/AutomationTest.h"
#include "MonolithGameFeatureActions.h"
#include "MonolithSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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
				TestEqual(TEXT("Only get_status registered by default"), Actions->Num(), 1);
			}
			if (AvailableWhenEnabled)
			{
				TestEqual(TEXT("Four gated inspection actions reported"), AvailableWhenEnabled->Num(), 4);
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
			TestTrue(TEXT("Read-only mode"), Result.Result->GetStringField(TEXT("mode")) == TEXT("read_only"));
			TestFalse(TEXT("No hard ToolsetRegistry dependency"), Result.Result->GetBoolField(TEXT("hard_toolsetregistry_dependency")));
			TestFalse(TEXT("Creation disabled by default"), Result.Result->GetBoolField(TEXT("creation_allowed")));
		}
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
		FMonolithActionResult Result = FMonolithGameFeatureActions::ValidatePlugin(MakeShared<FJsonObject>());
		TestFalse(TEXT("ValidatePlugin rejects missing plugin_name"), Result.bSuccess);
		TestEqual(TEXT("ValidatePlugin missing param code"), Result.ErrorCode, -32602);
	}

	Settings->bEnableGameFeatureActions = bOriginalEnabled;
	Settings->bAllowGameFeaturePluginCreation = bOriginalCreation;
	return true;
}
