#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithSentinelFile.h"
#include "MonolithSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSettingsActivationPersistenceTest,
	"Monolith.Activation.PersistentState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSettingsActivationPersistenceTest::RunTest(const FString& Parameters)
{
	const FString TestDirectory = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("MonolithSettingsActivationTests"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString UserFile = FPaths::Combine(
		TestDirectory,
		TEXT("Saved"),
		TEXT("Config"),
		TEXT("WindowsEditor"),
		TEXT("Monolith.ini"));
	const FString LegacyFile = FPaths::Combine(
		TestDirectory,
		TEXT("Saved"),
		TEXT("Monolith"),
		TEXT("Activation.ini"));

	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestDirectory, false, true);
	};

	FMonolithActivation Activation =
		UMonolithSettings::LoadActivationForTests(
			UserFile,
			LegacyFile,
			false,
			true);
	TestFalse(TEXT("missing user override inherits the project server default"), Activation.bServerEnabled);
	TestTrue(TEXT("missing user override inherits the project indexing default"), Activation.bIndexingEnabled);
	TestFalse(TEXT("server is not marked user-overridden"), Activation.bServerUserSet);
	TestFalse(TEXT("indexing is not marked user-overridden"), Activation.bIndexingUserSet);

	const FMonolithActivation CachedInitial =
		UMonolithSettings::GetCachedActivationForTests(
			UserFile,
			LegacyFile,
			false,
			true,
			10.0);
	TestFalse(TEXT("cached server value starts on the project default"), CachedInitial.bServerEnabled);
	TestTrue(TEXT("cached indexing value starts on the project default"), CachedInitial.bIndexingEnabled);

	const FMonolithActivation CachedAfterProjectPolicyChange =
		UMonolithSettings::GetCachedActivationForTests(
			UserFile,
			LegacyFile,
			true,
			false,
			10.1);
	TestTrue(
		TEXT("project server default is part of the cache key"),
		CachedAfterProjectPolicyChange.bServerEnabled);
	TestFalse(
		TEXT("project indexing default is part of the cache key"),
		CachedAfterProjectPolicyChange.bIndexingEnabled);

	const FMonolithActivation CachedBeforeUserWrite =
		UMonolithSettings::GetCachedActivationForTests(
			UserFile,
			LegacyFile,
			false,
			true,
			10.2);
	TestFalse(TEXT("cache is reseeded before the user write"), CachedBeforeUserWrite.bServerEnabled);

	FString Error;
	TestTrue(
		TEXT("server activation writes a generated user config"),
		UMonolithSettings::SetServerActivatedForTests(UserFile, true, &Error));
	TestTrue(TEXT("server activation write error is empty"), Error.IsEmpty());

	const FMonolithActivation CachedAfterUserWrite =
		UMonolithSettings::GetCachedActivationForTests(
			UserFile,
			LegacyFile,
			false,
			true,
			10.3);
	TestTrue(TEXT("a successful user write invalidates the cache"), CachedAfterUserWrite.bServerEnabled);
	TestTrue(TEXT("the sibling remains on project policy after cache invalidation"), CachedAfterUserWrite.bIndexingEnabled);

	Activation = UMonolithSettings::LoadActivationForTests(
		UserFile,
		LegacyFile,
		false,
		true);
	TestTrue(TEXT("user server override wins over the project default"), Activation.bServerEnabled);
	TestTrue(TEXT("server is marked user-overridden"), Activation.bServerUserSet);
	TestTrue(TEXT("missing indexing override still inherits the project default"), Activation.bIndexingEnabled);
	TestFalse(TEXT("indexing remains project-defaulted"), Activation.bIndexingUserSet);

	TestTrue(
		TEXT("indexing deactivation updates the same generated config"),
		UMonolithSettings::SetIndexingActivatedForTests(UserFile, false, &Error));
	Activation = UMonolithSettings::LoadActivationForTests(
		UserFile,
		LegacyFile,
		false,
		true);
	TestTrue(TEXT("server override survives an indexing write"), Activation.bServerEnabled);
	TestFalse(TEXT("indexing override persists independently"), Activation.bIndexingEnabled);
	TestTrue(TEXT("indexing is marked user-overridden"), Activation.bIndexingUserSet);

	const FString InvalidContents =
		TEXT("[Monolith.UserActivation]\n")
		TEXT("ServerEnabled=not-a-bool\n")
		TEXT("IndexingEnabled=True\n");
	TestTrue(
		TEXT("invalid user config fixture writes"),
		FFileHelper::SaveStringToFile(InvalidContents, *UserFile));
	AddExpectedError(
		TEXT("Monolith activation config contains invalid ServerEnabled"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Activation = UMonolithSettings::LoadActivationForTests(
		UserFile,
		LegacyFile,
		true,
		false);
	TestFalse(TEXT("malformed user server value fails closed"), Activation.bServerEnabled);
	TestTrue(TEXT("valid user indexing value still loads"), Activation.bIndexingEnabled);

	IFileManager::Get().Delete(*UserFile);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LegacyFile), true);
	const FString LegacyContents =
		TEXT("[Monolith.Activation]\n")
		TEXT("ServerEnabled=False\n")
		TEXT("IndexingEnabled=True\n");
	TestTrue(
		TEXT("legacy activation fixture writes"),
		FFileHelper::SaveStringToFile(LegacyContents, *LegacyFile));

	Activation = UMonolithSettings::LoadActivationForTests(
		UserFile,
		LegacyFile,
		true,
		false);
	TestFalse(TEXT("legacy server choice is preserved during migration"), Activation.bServerEnabled);
	TestTrue(TEXT("legacy indexing choice is preserved during migration"), Activation.bIndexingEnabled);
	TestTrue(TEXT("migration creates the generated user config"), IFileManager::Get().FileExists(*UserFile));
	TestFalse(TEXT("migration retires the legacy activation file"), IFileManager::Get().FileExists(*LegacyFile));

	Activation = UMonolithSettings::LoadActivationForTests(
		UserFile,
		LegacyFile,
		true,
		false);
	TestFalse(TEXT("migrated server choice remains authoritative"), Activation.bServerEnabled);
	TestTrue(TEXT("migrated indexing choice remains authoritative"), Activation.bIndexingEnabled);

	const FString SentinelFile =
		FPaths::Combine(TestDirectory, TEXT("sentinel"));
	TestTrue(
		TEXT("sentinel ownership fixture writes"),
		FFileHelper::SaveStringToFile(TEXT("{}"), *SentinelFile));
	bool bOwnsSentinel = true;
	TestEqual(
		TEXT("a failed sentinel delete reports failure"),
		MonolithSentinelFile::CompleteRemovalAttempt(
			false,
			true,
			bOwnsSentinel),
		MonolithSentinelFile::ERemoveResult::Failed);
	TestTrue(
		TEXT("a failed sentinel delete retains ownership for retry"),
		bOwnsSentinel);
	TestEqual(
		TEXT("a later sentinel delete succeeds"),
		MonolithSentinelFile::RemoveOwned(
			SentinelFile,
			bOwnsSentinel),
		MonolithSentinelFile::ERemoveResult::Removed);
	TestFalse(
		TEXT("successful sentinel deletion releases ownership"),
		bOwnsSentinel);

	const FString ExternalUserFile = FPaths::Combine(
		TestDirectory,
		TEXT("External"),
		TEXT("Saved"),
		TEXT("Config"),
		TEXT("WindowsEditor"),
		TEXT("Monolith.ini"));
	const FString ExternalLegacyFile = FPaths::Combine(
		TestDirectory,
		TEXT("External"),
		TEXT("Saved"),
		TEXT("Monolith"),
		TEXT("Activation.ini"));
	const FMonolithActivation CachedBeforeExternalEdit =
		UMonolithSettings::GetCachedActivationForTests(
			ExternalUserFile,
			ExternalLegacyFile,
			false,
			true,
			20.0);
	TestFalse(TEXT("external-edit fixture starts on project server policy"), CachedBeforeExternalEdit.bServerEnabled);

	TestTrue(
		TEXT("external activation fixture directory exists"),
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExternalUserFile), true));
	const FString ExternalContents =
		TEXT("[Monolith.UserActivation]\n")
		TEXT("ServerEnabled=True\n");
	TestTrue(
		TEXT("external activation fixture writes"),
		FFileHelper::SaveStringToFile(ExternalContents, *ExternalUserFile));

	const FMonolithActivation CachedInsideExternalEditWindow =
		UMonolithSettings::GetCachedActivationForTests(
			ExternalUserFile,
			ExternalLegacyFile,
			false,
			true,
			20.5);
	TestFalse(
		TEXT("external edits stay bounded by the one-second revalidation interval"),
		CachedInsideExternalEditWindow.bServerEnabled);

	const FMonolithActivation CachedAfterExternalEditWindow =
		UMonolithSettings::GetCachedActivationForTests(
			ExternalUserFile,
			ExternalLegacyFile,
			false,
			true,
			21.1);
	TestTrue(
		TEXT("external edits are observed after the revalidation interval"),
		CachedAfterExternalEditWindow.bServerEnabled);

	return true;
}

#endif
