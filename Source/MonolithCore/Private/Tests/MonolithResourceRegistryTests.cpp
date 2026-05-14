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
	}

	FMonolithResourceReadResult Read = Registry.ReadResource(TEXT("monolith://test/resource"));
	TestTrue(TEXT("Read finds resource"), Read.bFound);
	TestEqual(TEXT("Read text"), Read.Text, TEXT("hello resource"));
	TestFalse(TEXT("Read is not truncated"), Read.bTruncated);

	FMonolithResourceReadResult Missing = Registry.ReadResource(TEXT("monolith://test/missing"));
	TestFalse(TEXT("Unknown resource is not found"), Missing.bFound);
	TestTrue(TEXT("Unknown resource has error"), !Missing.Error.IsEmpty());

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
		TestTrue(TEXT("Spec 00 URI is listed"), ResourceArrayContainsUri(Resources, TEXT("monolith://docs/specs/unrealmcp/00")));
	}

	FMonolithResourceReadResult Read = Registry.ReadResource(TEXT("monolith://docs/specs/unrealmcp/00"));
	TestTrue(TEXT("Spec 00 read succeeds"), Read.bFound);
	TestTrue(TEXT("Spec 00 content is markdown"), Read.MimeType == TEXT("text/markdown"));
	TestTrue(TEXT("Spec 00 content has expected heading"), Read.Text.Contains(TEXT("Implementation Order")));

	Registry.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
