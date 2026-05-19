#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
