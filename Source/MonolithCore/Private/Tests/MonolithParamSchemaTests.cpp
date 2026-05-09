#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"

// FMonolithParamSchema alias rewriting test
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamSchemaAliasesTest,
	"Monolith.ParamSchema.ApplyAliases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSchemaAliasesTest::RunTest(const FString& Parameters)
{
	// Build a mock schema
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT(""), { TEXT("path") })
		.Optional(TEXT("count"), TEXT("number"), TEXT(""), TEXT("1"), { TEXT("limit") })
		.Build();

	// Test 1: canonical only
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));

		FString Collision;
		bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

		TestTrue(TEXT("ApplyAliases succeeds with canonical params"), bResult);
		TestEqual(TEXT("No collision string"), Collision, TEXT(""));
		TestEqual(TEXT("asset_path preserved"), Params->GetStringField(TEXT("asset_path")), TEXT("/Game/Map"));
		TestFalse(TEXT("path not created"), Params->HasField(TEXT("path")));
	}

	// Test 2: alias rewritten to canonical
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), TEXT("/Game/Map"));
		Params->SetNumberField(TEXT("limit"), 5);

		FString Collision;
		bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

		TestTrue(TEXT("ApplyAliases succeeds with alias params"), bResult);
		TestTrue(TEXT("asset_path created from alias"), Params->HasField(TEXT("asset_path")));
		TestEqual(TEXT("asset_path value matches alias"), Params->GetStringField(TEXT("asset_path")), TEXT("/Game/Map"));
		TestTrue(TEXT("count created from alias"), Params->HasField(TEXT("count")));
		TestEqual(TEXT("count value matches alias"), Params->GetNumberField(TEXT("count")), 5.0);
	}

	// Test 3: collision
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Other"));

		FString Collision;
		bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

		TestFalse(TEXT("ApplyAliases fails on collision"), bResult);
		TestTrue(TEXT("Collision string populated"), Collision.Contains(TEXT("Param collision: both canonical 'asset_path' and alias 'path' supplied")));
	}

	return true;
}

// FMonolithParamSchema unknown keys test
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamSchemaUnknownKeysTest,
	"Monolith.ParamSchema.FindUnknownKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSchemaUnknownKeysTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT(""), { TEXT("path") })
		.Build();

	// Test 1: valid params
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));

		TArray<FString> Unknown = FMonolithParamSchema::FindUnknownKeys(Schema, Params);
		TestEqual(TEXT("No unknown keys for canonical"), Unknown.Num(), 0);
	}

	// Test 2: valid alias
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), TEXT("/Game/Map"));

		TArray<FString> Unknown = FMonolithParamSchema::FindUnknownKeys(Schema, Params);
		TestEqual(TEXT("No unknown keys for valid alias"), Unknown.Num(), 0);
	}

	// Test 3: unknown key
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));
		Params->SetStringField(TEXT("wbp_path"), TEXT("/Game/WBP")); // Not in this schema
		Params->SetStringField(TEXT("typo_path"), TEXT("value"));

		TArray<FString> Unknown = FMonolithParamSchema::FindUnknownKeys(Schema, Params);
		TestEqual(TEXT("Found unknown keys"), Unknown.Num(), 2);
		TestTrue(TEXT("Contains wbp_path"), Unknown.Contains(TEXT("wbp_path")));
		TestTrue(TEXT("Contains typo_path"), Unknown.Contains(TEXT("typo_path")));
	}

	// Test 4: global allowlist (asset_path is globally allowed even if not in schema)
	{
		TSharedPtr<FJsonObject> EmptySchema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));

		TArray<FString> Unknown = FMonolithParamSchema::FindUnknownKeys(EmptySchema, Params);
		TestEqual(TEXT("asset_path is globally allowed"), Unknown.Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
