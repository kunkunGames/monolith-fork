#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithResourceRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	bool ResourceArrayContainsUri(const TArray<TSharedPtr<FJsonValue>>* Resources, const FString& ExpectedUri)
	{
		if (!Resources)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Resources)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid())
			{
				FString Uri;
				if ((*Obj)->TryGetStringField(TEXT("uri"), Uri) && Uri == ExpectedUri)
				{
					return true;
				}
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithResourceRegistryBasicTest,
	"Monolith.Core.Resources.RegistryListRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResourceRegistryBasicTest::RunTest(const FString& Parameters)
{
	FMonolithResourceRegistry& Registry = FMonolithResourceRegistry::Get();
	Registry.ResetForTests();

	FMonolithResourceDescriptor Descriptor;
	Descriptor.Uri = TEXT("monolith://test/resource");
	Descriptor.Name = TEXT("Test resource");
	Descriptor.Description = TEXT("Resource registry test payload");
	Descriptor.MimeType = TEXT("text/plain");

	Registry.RegisterTextResource(
		Descriptor,
		FMonolithResourceRegistry::FTextResourceProvider::CreateLambda([]()
		{
			return FString(TEXT("hello resource"));
		}),
		64);

	TSharedPtr<FJsonObject> List = Registry.ListResourcesJson(10, FString());
	TestTrue(TEXT("List result exists"), List.IsValid());
	if (List.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Resources = nullptr;
		TestTrue(TEXT("Resources array exists"), List->TryGetArrayField(TEXT("resources"), Resources));
		TestTrue(TEXT("Registered URI is listed"), ResourceArrayContainsUri(Resources, TEXT("monolith://test/resource")));
		TestFalse(TEXT("Final resource page omits next cursor"), List->HasField(TEXT("nextCursor")));
	}

	FMonolithResourceReadResult Read = Registry.ReadResource(TEXT("monolith://test/resource"));
	TestTrue(TEXT("Read finds resource"), Read.bFound);
	TestEqual(TEXT("Read text"), Read.Text, TEXT("hello resource"));
	TestFalse(TEXT("Read is not truncated"), Read.bTruncated);

	FMonolithResourceReadResult Missing = Registry.ReadResource(TEXT("monolith://test/missing"));
	TestFalse(TEXT("Unknown resource is not found"), Missing.bFound);
	TestTrue(TEXT("Unknown resource has error"), !Missing.Error.IsEmpty());

	FMonolithResourceDescriptor LongDescriptor;
	LongDescriptor.Uri = TEXT("monolith://test/truncated");
	LongDescriptor.Name = TEXT("Truncated resource");
	LongDescriptor.Description = TEXT("Resource truncation test payload");
	LongDescriptor.MimeType = TEXT("text/plain");
	Registry.RegisterTextResource(
		LongDescriptor,
		FMonolithResourceRegistry::FTextResourceProvider::CreateLambda([]()
		{
			return FString(TEXT("abcdef"));
		}),
		3);

	FMonolithResourceReadResult Truncated = Registry.ReadResource(TEXT("monolith://test/truncated"));
	TestTrue(TEXT("Truncated resource is found"), Truncated.bFound);
	TestTrue(TEXT("Truncated resource marks truncation"), Truncated.bTruncated);
	TestEqual(TEXT("Truncated resource text is capped"), Truncated.Text, TEXT("abc"));

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithResourceRegistryPaginationTest,
	"Monolith.Core.Resources.RegistryPagination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResourceRegistryPaginationTest::RunTest(const FString& Parameters)
{
	FMonolithResourceRegistry& Registry = FMonolithResourceRegistry::Get();
	Registry.ResetForTests();

	for (int32 Index = 0; Index < 3; ++Index)
	{
		FMonolithResourceDescriptor Descriptor;
		Descriptor.Uri = FString::Printf(TEXT("monolith://test/page/%d"), Index);
		Descriptor.Name = FString::Printf(TEXT("Page %d"), Index);
		Descriptor.Description = TEXT("Resource pagination test payload");
		Descriptor.MimeType = TEXT("text/plain");

		Registry.RegisterTextResource(
			Descriptor,
			FMonolithResourceRegistry::FTextResourceProvider::CreateLambda([Index]()
			{
				return FString::Printf(TEXT("page %d"), Index);
			}),
			64);
	}

	TSharedPtr<FJsonObject> FirstPage = Registry.ListResourcesJson(2, FString());
	TestTrue(TEXT("First page exists"), FirstPage.IsValid());
	if (FirstPage.IsValid())
	{
		FString NextCursor;
		TestTrue(TEXT("First page has next cursor"), FirstPage->TryGetStringField(TEXT("nextCursor"), NextCursor));
		TestEqual(TEXT("First page cursor points to next offset"), NextCursor, TEXT("2"));
	}

	TSharedPtr<FJsonObject> FinalPage = Registry.ListResourcesJson(2, TEXT("2"));
	TestTrue(TEXT("Final page exists"), FinalPage.IsValid());
	if (FinalPage.IsValid())
	{
		TestFalse(TEXT("Final page omits next cursor"), FinalPage->HasField(TEXT("nextCursor")));
	}

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithDefaultSpecResourcesTest,
	"Monolith.Core.Resources.DefaultSpecProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDefaultSpecResourcesTest::RunTest(const FString& Parameters)
{
	FMonolithResourceRegistry& Registry = FMonolithResourceRegistry::Get();
	Registry.ResetForTests();
	Registry.RegisterDefaultResources();

	TSharedPtr<FJsonObject> List = Registry.ListResourcesJson(100, FString());
	TestTrue(TEXT("Default list exists"), List.IsValid());
	if (List.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Resources = nullptr;
		TestTrue(TEXT("Default resources array exists"), List->TryGetArrayField(TEXT("resources"), Resources));
		TestTrue(TEXT("Core spec URI is listed"), ResourceArrayContainsUri(Resources, TEXT("monolith://docs/specs/core")));
		TestTrue(TEXT("MCP resources spec URI is listed"), ResourceArrayContainsUri(Resources, TEXT("monolith://docs/specs/mcp-resources")));
	}

	FMonolithResourceReadResult Read = Registry.ReadResource(TEXT("monolith://docs/specs/mcp-resources"));
	TestTrue(TEXT("MCP resources spec read succeeds"), Read.bFound);
	TestTrue(TEXT("MCP resources spec content is markdown"), Read.MimeType == TEXT("text/markdown"));
	TestTrue(TEXT("MCP resources spec content has expected heading"), Read.Text.Contains(TEXT("Monolith MCP Resources First Slice")));

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLiveResourcesTest,
	"Monolith.Core.Resources.LiveProviders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLiveResourcesTest::RunTest(const FString& Parameters)
{
	FMonolithResourceRegistry& Registry = FMonolithResourceRegistry::Get();
	Registry.ResetForTests();
	Registry.RegisterDefaultResources();

	TSharedPtr<FJsonObject> List = Registry.ListResourcesJson(100, FString());
	TestTrue(TEXT("List exists"), List.IsValid());
	if (List.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Resources = nullptr;
		TestTrue(TEXT("resources array exists"), List->TryGetArrayField(TEXT("resources"), Resources));
		TestTrue(TEXT("tool-calls/recent URI is listed"), ResourceArrayContainsUri(Resources, TEXT("monolith://tool-calls/recent")));
		TestTrue(TEXT("audit/recent URI is listed"), ResourceArrayContainsUri(Resources, TEXT("monolith://audit/recent")));
		TestTrue(TEXT("progress/active URI is listed"), ResourceArrayContainsUri(Resources, TEXT("monolith://progress/active")));
	}

	// Live providers evaluate at read time and return bounded JSON objects even
	// when no records exist yet (no doc file dependency).
	FMonolithResourceReadResult ToolCalls = Registry.ReadResource(TEXT("monolith://tool-calls/recent"));
	TestTrue(TEXT("tool-calls read succeeds"), ToolCalls.bFound);
	TestEqual(TEXT("tool-calls mime is json"), ToolCalls.MimeType, FString(TEXT("application/json")));
	TestTrue(TEXT("tool-calls content is a JSON object"), ToolCalls.Text.StartsWith(TEXT("{")));

	FMonolithResourceReadResult Audit = Registry.ReadResource(TEXT("monolith://audit/recent"));
	TestTrue(TEXT("audit read succeeds"), Audit.bFound);
	TestEqual(TEXT("audit mime is json"), Audit.MimeType, FString(TEXT("application/json")));
	TestTrue(TEXT("audit content is a JSON object"), Audit.Text.StartsWith(TEXT("{")));

	FMonolithResourceReadResult Progress = Registry.ReadResource(TEXT("monolith://progress/active"));
	TestTrue(TEXT("progress read succeeds"), Progress.bFound);
	TestEqual(TEXT("progress mime is json"), Progress.MimeType, FString(TEXT("application/json")));
	TestTrue(TEXT("progress content is a JSON object"), Progress.Text.StartsWith(TEXT("{")));

	Registry.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
