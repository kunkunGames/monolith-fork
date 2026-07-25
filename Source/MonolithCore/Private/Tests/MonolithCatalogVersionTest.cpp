// SPDX-License-Identifier: MIT
// Automation coverage for the conditional discovery catalog protocol.

#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithCatalogVersionTestDetail
{
	static const FString TestNamespace = TEXT("__catalog_version_test");

	struct FScopedTestNamespace
	{
		FScopedTestNamespace()
		{
			FMonolithToolRegistry::Get().UnregisterNamespace(TestNamespace);
		}

		~FScopedTestNamespace()
		{
			FMonolithToolRegistry::Get().UnregisterNamespace(TestNamespace);
		}
	};

	static FMonolithActionHandler MakeHandler()
	{
		return FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("ok"), true);
			return FMonolithActionResult::Success(Result);
		});
	}

	static TSharedPtr<FJsonObject> MakeEquivalentSchema(bool bReverseInsertionOrder)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> Alpha = MakeShared<FJsonObject>();
		Alpha->SetStringField(TEXT("description"), TEXT("Alpha parameter"));
		Alpha->SetStringField(TEXT("type"), TEXT("string"));

		TSharedPtr<FJsonObject> Beta = MakeShared<FJsonObject>();
		Beta->SetBoolField(TEXT("required"), false);
		Beta->SetStringField(TEXT("type"), TEXT("integer"));

		if (bReverseInsertionOrder)
		{
			Schema->SetObjectField(TEXT("beta"), Beta);
			Schema->SetObjectField(TEXT("alpha"), Alpha);
		}
		else
		{
			Schema->SetObjectField(TEXT("alpha"), Alpha);
			Schema->SetObjectField(TEXT("beta"), Beta);
		}
		return Schema;
	}

	static void RegisterPair(bool bReverseActionOrder, bool bReverseSchemaOrder)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		auto RegisterAlpha = [&]()
		{
			Registry.RegisterAction(
				TestNamespace,
				TEXT("alpha"),
				TEXT("Alpha test action"),
				MakeHandler(),
				MakeEquivalentSchema(bReverseSchemaOrder),
				TEXT("Tests"));
		};
		auto RegisterBeta = [&]()
		{
			Registry.RegisterAction(
				TestNamespace,
				TEXT("beta"),
				TEXT("Beta test action"),
				MakeHandler(),
				MakeEquivalentSchema(!bReverseSchemaOrder),
				TEXT("Tests"));
		};

		if (bReverseActionOrder)
		{
			RegisterBeta();
			RegisterAlpha();
		}
		else
		{
			RegisterAlpha();
			RegisterBeta();
		}
	}

	static FString ReadVersion(const FMonolithActionResult& Result)
	{
		FString Version;
		if (Result.bSuccess && Result.Result.IsValid())
		{
			Result.Result->TryGetStringField(TEXT("catalog_version"), Version);
		}
		return Version;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithCatalogFingerprintStabilityTest,
	"Monolith.Core.CatalogVersion.FingerprintStability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCatalogFingerprintStabilityTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCatalogVersionTestDetail;

	FScopedTestNamespace Cleanup;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FString Baseline = Registry.GetCatalogFingerprint();

	TestTrue(TEXT("fingerprint uses the bounded sha256 format"),
		Baseline.StartsWith(TEXT("sha256:")) && Baseline.Len() == 23);

	RegisterPair(/*bReverseActionOrder=*/true, /*bReverseSchemaOrder=*/false);
	const FString FirstOrder = Registry.GetCatalogFingerprint();
	TestNotEqual(TEXT("registering actions changes the fingerprint"), FirstOrder, Baseline);

	Registry.UnregisterNamespace(TestNamespace);
	RegisterPair(/*bReverseActionOrder=*/false, /*bReverseSchemaOrder=*/true);
	const FString SecondOrder = Registry.GetCatalogFingerprint();
	TestEqual(TEXT("registration and JSON key order do not affect the fingerprint"), SecondOrder, FirstOrder);

	Registry.SetActionAnnotations(
		TestNamespace,
		TEXT("alpha"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Annotated alpha"));
	const FString ActionAnnotationVersion = Registry.GetCatalogFingerprint();
	TestNotEqual(TEXT("action annotation changes affect the fingerprint"), ActionAnnotationVersion, SecondOrder);

	FMonolithDispatcherAnnotations DispatcherAnnotations;
	DispatcherAnnotations.bReadOnlyHint = true;
	DispatcherAnnotations.bIdempotentHint = true;
	DispatcherAnnotations.Title = TEXT("Catalog test dispatcher");
	Registry.SetDispatcherAnnotations(TestNamespace, DispatcherAnnotations);
	const FString DispatcherAnnotationVersion = Registry.GetCatalogFingerprint();
	TestNotEqual(TEXT("dispatcher annotation changes affect the fingerprint"), DispatcherAnnotationVersion, ActionAnnotationVersion);

	Registry.UnregisterNamespace(TestNamespace);
	TestEqual(TEXT("unregistering the namespace restores the fingerprint"), Registry.GetCatalogFingerprint(), Baseline);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConditionalDiscoverProtocolTest,
	"Monolith.Core.CatalogVersion.ConditionalDiscover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConditionalDiscoverProtocolTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCatalogVersionTestDetail;

	FScopedTestNamespace Cleanup;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	const FMonolithActionResult Status = Registry.ExecuteAction(
		TEXT("monolith"), TEXT("status"), MakeShared<FJsonObject>());
	TestTrue(TEXT("status succeeds"), Status.bSuccess);
	const FString StatusVersion = ReadVersion(Status);
	TestFalse(TEXT("status exposes catalog_version"), StatusVersion.IsEmpty());

	const FMonolithActionResult FullDiscover = Registry.ExecuteAction(
		TEXT("monolith"), TEXT("discover"), MakeShared<FJsonObject>());
	TestTrue(TEXT("legacy full discover succeeds"), FullDiscover.bSuccess);
	TestEqual(TEXT("status and full discover expose the same version"), ReadVersion(FullDiscover), StatusVersion);
	if (FullDiscover.Result.IsValid())
	{
		TestTrue(TEXT("legacy full discover still contains namespace details"),
			FullDiscover.Result->HasField(TEXT("namespaces")));
		TestTrue(TEXT("legacy full discover still contains total_actions"),
			FullDiscover.Result->HasField(TEXT("total_actions")));
	}

	TSharedPtr<FJsonObject> MatchingParams = MakeShared<FJsonObject>();
	MatchingParams->SetStringField(TEXT("if_version"), StatusVersion);
	const FMonolithActionResult Unchanged = Registry.ExecuteAction(
		TEXT("monolith"), TEXT("discover"), MatchingParams);
	TestTrue(TEXT("matching conditional discover succeeds"), Unchanged.bSuccess);
	if (Unchanged.Result.IsValid())
	{
		FString StatusValue;
		Unchanged.Result->TryGetStringField(TEXT("status"), StatusValue);
		TestEqual(TEXT("matching version returns unchanged"), StatusValue, FString(TEXT("unchanged")));
		TestEqual(TEXT("unchanged response preserves version"), ReadVersion(Unchanged), StatusVersion);
		TestTrue(TEXT("unchanged response carries action count"), Unchanged.Result->HasField(TEXT("total_actions")));
		TestTrue(TEXT("unchanged response carries namespace names"), Unchanged.Result->HasField(TEXT("namespaces")));
		TestFalse(TEXT("unchanged response omits optional module details"), Unchanged.Result->HasField(TEXT("optional_modules")));
		TestFalse(TEXT("unchanged response omits the guide hint"), Unchanged.Result->HasField(TEXT("guide_hint")));

		const FString Serialized = FMonolithJsonUtils::Serialize(Unchanged.Result);
		FTCHARToUTF8 SerializedUtf8(*Serialized);
		TestTrue(TEXT("unchanged response remains under 1 KiB"), SerializedUtf8.Length() < 1024);
	}

	Registry.RegisterAction(
		TestNamespace,
		TEXT("new_action"),
		TEXT("Catalog mutation"),
		MakeHandler(),
		MakeEquivalentSchema(false));

	const FMonolithActionResult Changed = Registry.ExecuteAction(
		TEXT("monolith"), TEXT("discover"), MatchingParams);
	TestTrue(TEXT("stale-version discover succeeds"), Changed.bSuccess);
	const FString ChangedVersion = ReadVersion(Changed);
	TestNotEqual(TEXT("catalog mutation changes discover version"), ChangedVersion, StatusVersion);
	if (Changed.Result.IsValid())
	{
		TestFalse(TEXT("stale version returns full output, not unchanged status"),
			Changed.Result->HasField(TEXT("status")));
		TestTrue(TEXT("stale version full output includes namespace details"),
			Changed.Result->HasField(TEXT("namespaces")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConditionalDiscoverTypeValidationTest,
	"Monolith.Core.CatalogVersion.IfVersionTypeValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConditionalDiscoverTypeValidationTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("if_version"), 123.0);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("monolith"), TEXT("discover"), Params);
	TestFalse(TEXT("non-string if_version is rejected"), Result.bSuccess);
	TestEqual(TEXT("type error uses invalid-params code"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	TestTrue(TEXT("type error names if_version"), Result.ErrorMessage.Contains(TEXT("if_version")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
