#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithParamSchema.h"
#include "MonolithSourceActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

namespace
{
	TSharedPtr<FJsonObject> FindSourceActionSchema(const TCHAR* ActionName)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("source"), ActionName))
		{
			FMonolithSourceActions::RegisterAll();
		}

		for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("source")))
		{
			if (Info.Action == ActionName)
			{
				return Info.ParamSchema;
			}
		}
		return nullptr;
	}
}

// FMonolithParamSchema get_class_hierarchy alias test
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceRegistryClassHierarchyAliasTest,
	"Monolith.Registry.Source.ClassHierarchyAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceRegistryClassHierarchyAliasTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("symbol"), TEXT("string"), TEXT("Class name"), { TEXT("class_name") })
		.Optional(TEXT("direction"), TEXT("string"), TEXT("Direction: up (parents) or down (children)"), TEXT("both"))
		.Optional(TEXT("depth"), TEXT("integer"), TEXT("Max hierarchy depth"), TEXT("5"))
		.Build();

	// Test: alias rewritten to canonical
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("class_name"), TEXT("AActor"));

		FString Collision;
		bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

		TestTrue(TEXT("ApplyAliases succeeds with alias params"), bResult);
		TestTrue(TEXT("symbol created from alias"), Params->HasField(TEXT("symbol")));
		TestEqual(TEXT("symbol value matches alias"), Params->GetStringField(TEXT("symbol")), TEXT("AActor"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceRegistryCallGraphQueryAliasTest,
	"Monolith.Registry.Source.CallGraphQueryAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceRegistryCallGraphQueryAliasTest::RunTest(const FString& Parameters)
{
	auto TestAction = [this](const TCHAR* ActionName)
	{
		TSharedPtr<FJsonObject> Schema = FindSourceActionSchema(ActionName);
		TestTrue(FString::Printf(TEXT("%s schema exists"), ActionName), Schema.IsValid());
		if (!Schema.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("query"), TEXT("UObject::GetName"));

		FString Collision;
		const bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

		TestTrue(FString::Printf(TEXT("%s query alias applies"), ActionName), bResult);
		TestTrue(FString::Printf(TEXT("%s symbol created from query"), ActionName), Params->HasField(TEXT("symbol")));
		TestEqual(FString::Printf(TEXT("%s symbol value matches query"), ActionName),
			Params->GetStringField(TEXT("symbol")),
			FString(TEXT("UObject::GetName")));
	};

	TestAction(TEXT("find_callers"));
	TestAction(TEXT("find_callees"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceRegistrySearchSourceQueryAliasTest,
	"Monolith.Registry.Source.SearchSourceQueryAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceRegistrySearchSourceQueryAliasTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Schema = FindSourceActionSchema(TEXT("search_source"));
	TestTrue(TEXT("search_source schema exists"), Schema.IsValid());
	if (!Schema.IsValid())
	{
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("q"), TEXT("UObject"));

	FString Collision;
	const bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

	TestTrue(TEXT("search_source q alias applies"), bResult);
	TestTrue(TEXT("search_source query created from q"), Params->HasField(TEXT("query")));
	TestEqual(TEXT("search_source query value matches q"),
		Params->GetStringField(TEXT("query")),
		FString(TEXT("UObject")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
