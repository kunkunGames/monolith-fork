#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Char.h"
#include "MonolithSourceResourceProvider.h"
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

	// A client-facing string must never expose a Windows or POSIX absolute path. The provider
	// routes reads through ResolveAndReadFile, which only ever emits ShortPath()-form output.
	bool LeaksAbsolutePath(const FString& Text)
	{
		// A Windows drive-letter absolute path ("D:\\..." or "C:/...") is a single drive letter at a
		// word boundary followed by ':' and a path separator. This deliberately does NOT match a URL
		// scheme separator like "monolith://" (there the char before ':' is itself a letter), so the
		// provider echoing the client's own monolith:// URI in a not-found error is not a leak.
		for (int32 i = 0; i + 2 < Text.Len(); ++i)
		{
			if (FChar::IsAlpha(Text[i]) && Text[i + 1] == TEXT(':')
				&& (Text[i + 2] == TEXT('\\') || Text[i + 2] == TEXT('/')))
			{
				if (i == 0 || !FChar::IsAlnum(Text[i - 1]))
				{
					return true;
				}
			}
		}
		// A leading POSIX root (e.g. "/home/..." or "/mnt/...") at the very start of the string.
		if (Text.StartsWith(TEXT("/")))
		{
			return true;
		}
		return false;
	}
}

// ListResources advertises exactly one template descriptor — no per-row file enumeration.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceResourceProviderTemplateTest,
	"Monolith.Source.Resources.ProviderTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceResourceProviderTemplateTest::RunTest(const FString& Parameters)
{
	FMonolithSourceResourceProvider Provider;

	TArray<FMonolithResourceDescriptor> Descriptors;
	Provider.ListResources(Descriptors);

	TestEqual(TEXT("Provider lists exactly one template descriptor"), Descriptors.Num(), 1);
	if (Descriptors.Num() == 1)
	{
		TestEqual(TEXT("Template URI matches the source file scheme"),
			Descriptors[0].Uri, FString(FMonolithSourceResourceProvider::TemplateUri()));
		TestFalse(TEXT("Template name is non-empty"), Descriptors[0].Name.IsEmpty());
		TestFalse(TEXT("Template description is non-empty"), Descriptors[0].Description.IsEmpty());
	}
	return true;
}

// URI ownership: a non-source URI is disowned (false); the bare scheme is owned-but-not-found.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceResourceProviderOwnershipTest,
	"Monolith.Source.Resources.ProviderOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceResourceProviderOwnershipTest::RunTest(const FString& Parameters)
{
	// Resolving an owned source URI attempts to open the engine source DB. In this headless context
	// that open may succeed, warn ("not found"), or error ("failed to open") depending on whether a
	// concurrent process holds the DB — all incidental to what this test verifies (URI ownership +
	// no absolute-path leak). Suppress this test instance's incidental log output so the result is
	// deterministic; the TestTrue/TestFalse assertions below remain the real gate.
	bSuppressLogs = true;

	FMonolithSourceResourceProvider Provider;

	// Foreign URI: provider must return false so the registry can try the next provider.
	{
		FMonolithResourceReadResult Out;
		const bool bOwned = Provider.ReadResource(TEXT("monolith://docs/specs/core"), Out);
		TestFalse(TEXT("Foreign URI is not owned by the source provider"), bOwned);
		TestFalse(TEXT("Foreign URI is not marked found"), Out.bFound);
	}

	// Bare scheme with no {path}: owned (true) but unresolved (bFound=false).
	{
		FMonolithResourceReadResult Out;
		const bool bOwned = Provider.ReadResource(FMonolithSourceResourceProvider::UriPrefix(), Out);
		TestTrue(TEXT("Bare source scheme is owned"), bOwned);
		TestFalse(TEXT("Bare source scheme resolves nothing"), Out.bFound);
		TestFalse(TEXT("Bare source scheme error does not leak an absolute path"), LeaksAbsolutePath(Out.Error));
	}

	// Owned URI for a path that cannot resolve (no DB in this headless context, or a missing
	// path). Either way the provider must own the URI and never leak an absolute path.
	{
		FMonolithResourceReadResult Out;
		const FString Uri = FString(FMonolithSourceResourceProvider::UriPrefix())
			+ TEXT("Definitely/Not/A/Real/Indexed/Path_Zzz.cpp");
		const bool bOwned = Provider.ReadResource(Uri, Out);
		TestTrue(TEXT("Owned source URI is claimed even when unresolved"), bOwned);
		TestFalse(TEXT("Unresolved owned read does not leak an absolute path (error)"), LeaksAbsolutePath(Out.Error));
		TestFalse(TEXT("Unresolved owned read does not leak an absolute path (text)"), LeaksAbsolutePath(Out.Text));
	}

	return true;
}

// Registry integration: a registered provider's template lists and reads fall through to it
// AFTER the static map misses, without shadowing static resources.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceResourceProviderRegistryTest,
	"Monolith.Source.Resources.ProviderRegistryFallthrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceResourceProviderRegistryTest::RunTest(const FString& Parameters)
{
	// The provider fallthrough resolves a source URI, which may emit an incidental DB-open warning or
	// error in this headless context (environment-dependent). Suppress this test instance's log
	// output so the result is deterministic; the assertions below remain the real gate.
	bSuppressLogs = true;

	FMonolithResourceRegistry& Registry = FMonolithResourceRegistry::Get();
	Registry.ResetForTests();

	TestEqual(TEXT("Registry starts with no providers"), Registry.GetProviderCount(), 0);

	TSharedRef<FMonolithSourceResourceProvider> Provider = MakeShared<FMonolithSourceResourceProvider>();
	Registry.RegisterProvider(Provider);
	TestEqual(TEXT("Provider count increments after register"), Registry.GetProviderCount(), 1);

	// Re-registering the same provider does not duplicate it.
	Registry.RegisterProvider(Provider);
	TestEqual(TEXT("Re-registering the same provider is idempotent"), Registry.GetProviderCount(), 1);

	// The provider template appears in the listing alongside any static resources.
	TSharedPtr<FJsonObject> List = Registry.ListResourcesJson(100, FString());
	TestTrue(TEXT("List result exists"), List.IsValid());
	if (List.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Resources = nullptr;
		TestTrue(TEXT("Resources array exists"), List->TryGetArrayField(TEXT("resources"), Resources));
		TestTrue(TEXT("Provider template URI is listed"),
			ResourceArrayContainsUri(Resources, FMonolithSourceResourceProvider::TemplateUri()));
	}

	// A read for an owned-but-unresolved source URI falls through the empty static map to the
	// provider, which reports not-found without leaking an absolute path.
	const FString Uri = FString(FMonolithSourceResourceProvider::UriPrefix())
		+ TEXT("Definitely/Not/A/Real/Indexed/Path_Zzz.cpp");
	FMonolithResourceReadResult Read = Registry.ReadResource(Uri);
	TestFalse(TEXT("Unresolved source URI is not found"), Read.bFound);
	TestFalse(TEXT("Registry read error does not leak an absolute path"), LeaksAbsolutePath(Read.Error));

	// A URI no provider owns still returns the registry's standard not-found error.
	FMonolithResourceReadResult MissingRead = Registry.ReadResource(TEXT("monolith://nope/unowned"));
	TestFalse(TEXT("Unowned URI is not found"), MissingRead.bFound);
	TestTrue(TEXT("Unowned URI has an error"), !MissingRead.Error.IsEmpty());

	// Unregister returns the registry to a clean provider state.
	Registry.UnregisterProvider(Provider);
	TestEqual(TEXT("Provider count returns to zero after unregister"), Registry.GetProviderCount(), 0);

	Registry.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
