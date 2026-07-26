#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
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

	FMonolithServiceActivation Activation =
		UMonolithSettings::LoadServiceActivationFromFilesForTests(
			UserFile,
			LegacyFile,
			false,
			true);
	TestFalse(TEXT("missing user override inherits the project server default"), Activation.bServerEnabled);
	TestTrue(TEXT("missing user override inherits the project indexing default"), Activation.bIndexingEnabled);
	TestFalse(TEXT("server is not marked user-overridden"), Activation.bServerOverriddenByUser);
	TestFalse(TEXT("indexing is not marked user-overridden"), Activation.bIndexingOverriddenByUser);

	FString Error;
	TestTrue(
		TEXT("server activation writes a generated user config"),
		UMonolithSettings::SetServerActivationInFileForTests(UserFile, true, &Error));
	TestTrue(TEXT("server activation write error is empty"), Error.IsEmpty());

	Activation = UMonolithSettings::LoadServiceActivationFromFilesForTests(
		UserFile,
		LegacyFile,
		false,
		true);
	TestTrue(TEXT("user server override wins over the project default"), Activation.bServerEnabled);
	TestTrue(TEXT("server is marked user-overridden"), Activation.bServerOverriddenByUser);
	TestTrue(TEXT("missing indexing override still inherits the project default"), Activation.bIndexingEnabled);
	TestFalse(TEXT("indexing remains project-defaulted"), Activation.bIndexingOverriddenByUser);

	TestTrue(
		TEXT("indexing deactivation updates the same generated config"),
		UMonolithSettings::SetIndexingActivationInFileForTests(UserFile, false, &Error));
	Activation = UMonolithSettings::LoadServiceActivationFromFilesForTests(
		UserFile,
		LegacyFile,
		false,
		true);
	TestTrue(TEXT("server override survives an indexing write"), Activation.bServerEnabled);
	TestFalse(TEXT("indexing override persists independently"), Activation.bIndexingEnabled);
	TestTrue(TEXT("indexing is marked user-overridden"), Activation.bIndexingOverriddenByUser);

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
	Activation = UMonolithSettings::LoadServiceActivationFromFilesForTests(
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

	Activation = UMonolithSettings::LoadServiceActivationFromFilesForTests(
		UserFile,
		LegacyFile,
		true,
		false);
	TestFalse(TEXT("legacy server choice is preserved during migration"), Activation.bServerEnabled);
	TestTrue(TEXT("legacy indexing choice is preserved during migration"), Activation.bIndexingEnabled);
	TestTrue(TEXT("migration creates the generated user config"), IFileManager::Get().FileExists(*UserFile));
	TestFalse(TEXT("migration retires the legacy activation file"), IFileManager::Get().FileExists(*LegacyFile));

	Activation = UMonolithSettings::LoadServiceActivationFromFilesForTests(
		UserFile,
		LegacyFile,
		true,
		false);
	TestFalse(TEXT("migrated server choice remains authoritative"), Activation.bServerEnabled);
	TestTrue(TEXT("migrated indexing choice remains authoritative"), Activation.bIndexingEnabled);

	return true;
}

#endif
