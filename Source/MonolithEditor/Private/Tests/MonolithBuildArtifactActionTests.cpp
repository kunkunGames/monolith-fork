#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MonolithBuildArtifactActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBuildArtifactRegistryAndGuardTest,
	"Monolith.Editor.BuildArtifact.RegistryAndGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	static TSharedPtr<FJsonObject> MakeParams()
	{
		return MakeShared<FJsonObject>();
	}

	static bool HasAction(const FString& Namespace, const FString& Action)
	{
		for (const FMonolithActionInfo& ActionInfo : FMonolithToolRegistry::Get().GetActions(Namespace))
		{
			if (ActionInfo.Action == Action)
			{
				return true;
			}
		}
		return false;
	}

	static FString FixtureRoot()
	{
		FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MonolithBuildArtifact"));
		FPaths::NormalizeFilename(Root);
		return Root;
	}
}

bool FMonolithBuildArtifactRegistryAndGuardTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("build.resolve_unreal_engine registered"), HasAction(TEXT("build"), TEXT("resolve_unreal_engine")));
	TestTrue(TEXT("build.run_buildcookrun registered"), HasAction(TEXT("build"), TEXT("run_buildcookrun")));
	TestTrue(TEXT("artifact.package_build_outputs registered"), HasAction(TEXT("artifact"), TEXT("package_build_outputs")));
	TestTrue(TEXT("artifact.mirror_screenshot_evidence registered"), HasAction(TEXT("artifact"), TEXT("mirror_screenshot_evidence")));
	TestTrue(TEXT("notify.discord_screenshot_evidence registered"), HasAction(TEXT("notify"), TEXT("discord_screenshot_evidence")));

	for (const TPair<FString, FString> NamespaceAction : {
		TPair<FString, FString>(TEXT("build"), TEXT("resolve_unreal_engine")),
		TPair<FString, FString>(TEXT("build"), TEXT("run_buildcookrun")),
		TPair<FString, FString>(TEXT("artifact"), TEXT("package_build_outputs")),
		TPair<FString, FString>(TEXT("artifact"), TEXT("mirror_screenshot_evidence")),
		TPair<FString, FString>(TEXT("notify"), TEXT("discord_screenshot_evidence"))
	})
	{
		TestEqual(
			FString::Printf(TEXT("%s.%s policy is read_only"), *NamespaceAction.Key, *NamespaceAction.Value),
			FMonolithToolRegistry::Get().GetActionExecutionPolicy(NamespaceAction.Key, NamespaceAction.Value).PolicyId,
			FString(TEXT("read_only")));
	}

	const FMonolithActionResult ResolveResult = FMonolithBuildArtifactActions::ResolveUnrealEngine(MakeParams());
	TestTrue(TEXT("resolve_unreal_engine succeeds"), ResolveResult.bSuccess);
	TestTrue(TEXT("resolve_unreal_engine result object is valid"), ResolveResult.Result.IsValid());
	if (ResolveResult.Result.IsValid())
	{
		bool bUatExists = false;
		bool bUbtExists = false;
		TestTrue(TEXT("uat_exists field exists"), ResolveResult.Result->TryGetBoolField(TEXT("uat_exists"), bUatExists));
		TestTrue(TEXT("ubt_exists field exists"), ResolveResult.Result->TryGetBoolField(TEXT("ubt_exists"), bUbtExists));
		TestTrue(TEXT("RunUAT exists for loaded engine"), bUatExists);
		TestTrue(TEXT("UBT exists for loaded engine"), bUbtExists);
	}

	TSharedPtr<FJsonObject> BuildDryRunParams = MakeParams();
	BuildDryRunParams->SetBoolField(TEXT("dry_run"), true);
	BuildDryRunParams->SetStringField(TEXT("custom_config"), TEXT("EOS"));
	const FMonolithActionResult BuildDryRun = FMonolithBuildArtifactActions::RunBuildCookRun(BuildDryRunParams);
	TestTrue(TEXT("run_buildcookrun dry-run succeeds"), BuildDryRun.bSuccess);
	if (BuildDryRun.Result.IsValid())
	{
		FString Status;
		FString CommandLine;
		TestTrue(TEXT("dry-run status exists"), BuildDryRun.Result->TryGetStringField(TEXT("status"), Status));
		TestEqual(TEXT("dry-run status"), Status, FString(TEXT("dry_run")));
		TestTrue(TEXT("command_line exists"), BuildDryRun.Result->TryGetStringField(TEXT("command_line"), CommandLine));
		TestTrue(TEXT("command_line contains BuildCookRun"), CommandLine.Contains(TEXT("BuildCookRun")));
		TestTrue(TEXT("command_line contains CustomConfig"), CommandLine.Contains(TEXT("-CustomConfig=EOS")));
	}

	TSharedPtr<FJsonObject> BuildNoConfirmParams = MakeParams();
	BuildNoConfirmParams->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult BuildNoConfirm = FMonolithBuildArtifactActions::RunBuildCookRun(BuildNoConfirmParams);
	TestFalse(TEXT("run_buildcookrun rejects dry_run=false without confirm"), BuildNoConfirm.bSuccess);
	TestTrue(TEXT("run_buildcookrun error mentions confirm"), BuildNoConfirm.ErrorMessage.Contains(TEXT("confirm=true")));

	TSharedPtr<FJsonObject> BuildUnsafePlatformParams = MakeParams();
	BuildUnsafePlatformParams->SetBoolField(TEXT("dry_run"), true);
	BuildUnsafePlatformParams->SetStringField(TEXT("platform"), TEXT("Win64&whoami"));
	const FMonolithActionResult BuildUnsafePlatform = FMonolithBuildArtifactActions::RunBuildCookRun(BuildUnsafePlatformParams);
	TestFalse(TEXT("run_buildcookrun rejects shell metacharacters in platform"), BuildUnsafePlatform.bSuccess);
	TestTrue(TEXT("run_buildcookrun unsafe platform error mentions guarded command line"), BuildUnsafePlatform.ErrorMessage.Contains(TEXT("guarded UAT command line")));

	TSharedPtr<FJsonObject> BuildUnsafeAdditionalArgsParams = MakeParams();
	BuildUnsafeAdditionalArgsParams->SetBoolField(TEXT("dry_run"), true);
	TArray<TSharedPtr<FJsonValue>> UnsafeAdditionalArgs;
	UnsafeAdditionalArgs.Add(MakeShared<FJsonValueString>(TEXT("-utf8output&whoami")));
	BuildUnsafeAdditionalArgsParams->SetArrayField(TEXT("additional_args"), UnsafeAdditionalArgs);
	const FMonolithActionResult BuildUnsafeAdditionalArgs = FMonolithBuildArtifactActions::RunBuildCookRun(BuildUnsafeAdditionalArgsParams);
	TestFalse(TEXT("run_buildcookrun rejects shell metacharacters in additional_args"), BuildUnsafeAdditionalArgs.bSuccess);

	const FString Root = FixtureRoot();
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString ArchiveDir = FPaths::Combine(Root, TEXT("Archive"));
	const FString MirrorDir = FPaths::Combine(Root, TEXT("Mirror"));
	IFileManager::Get().MakeDirectory(*ArchiveDir, true);
	const FString ArtifactFile = FPaths::Combine(ArchiveDir, TEXT("Build.txt"));
	TestTrue(TEXT("fixture artifact write"), FFileHelper::SaveStringToFile(TEXT("artifact"), *ArtifactFile));

	TSharedPtr<FJsonObject> PackageParams = MakeParams();
	PackageParams->SetStringField(TEXT("archive_dir"), ArchiveDir);
	PackageParams->SetBoolField(TEXT("dry_run"), true);
	PackageParams->SetBoolField(TEXT("write_manifest"), true);
	const FMonolithActionResult PackageDryRun = FMonolithBuildArtifactActions::PackageBuildOutputs(PackageParams);
	TestTrue(TEXT("package_build_outputs dry-run succeeds"), PackageDryRun.bSuccess);
	if (PackageDryRun.Result.IsValid())
	{
		bool bManifestWritten = true;
		TestTrue(TEXT("manifest_written field exists"), PackageDryRun.Result->TryGetBoolField(TEXT("manifest_written"), bManifestWritten));
		TestFalse(TEXT("dry-run does not write manifest"), bManifestWritten);
		const TSharedPtr<FJsonObject>* Manifest = nullptr;
		TestTrue(TEXT("manifest object exists"), PackageDryRun.Result->TryGetObjectField(TEXT("manifest"), Manifest));
		if (Manifest && Manifest->IsValid())
		{
			double FileCount = 0.0;
			TestTrue(TEXT("manifest file_count exists"), (*Manifest)->TryGetNumberField(TEXT("file_count"), FileCount));
			TestEqual(TEXT("manifest includes fixture file"), static_cast<int32>(FileCount), 1);
		}
	}

	TSharedPtr<FJsonObject> PackageNoConfirmParams = MakeParams();
	PackageNoConfirmParams->SetStringField(TEXT("archive_dir"), ArchiveDir);
	PackageNoConfirmParams->SetBoolField(TEXT("dry_run"), false);
	PackageNoConfirmParams->SetBoolField(TEXT("write_manifest"), true);
	const FMonolithActionResult PackageNoConfirm = FMonolithBuildArtifactActions::PackageBuildOutputs(PackageNoConfirmParams);
	TestFalse(TEXT("package_build_outputs write rejects no confirm"), PackageNoConfirm.bSuccess);

	const FString ScreenshotPath = FPaths::Combine(Root, TEXT("Evidence.png"));
	TestTrue(TEXT("fixture screenshot write"), FFileHelper::SaveStringToFile(TEXT("png placeholder"), *ScreenshotPath));
	TSharedPtr<FJsonObject> MirrorParams = MakeParams();
	TArray<TSharedPtr<FJsonValue>> Files;
	Files.Add(MakeShared<FJsonValueString>(ScreenshotPath));
	MirrorParams->SetArrayField(TEXT("files"), Files);
	MirrorParams->SetStringField(TEXT("dest_dir"), MirrorDir);
	MirrorParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult MirrorDryRun = FMonolithBuildArtifactActions::MirrorScreenshotEvidence(MirrorParams);
	TestTrue(TEXT("mirror_screenshot_evidence dry-run succeeds"), MirrorDryRun.bSuccess);
	if (MirrorDryRun.Result.IsValid())
	{
		FString Status;
		TestTrue(TEXT("mirror status exists"), MirrorDryRun.Result->TryGetStringField(TEXT("status"), Status));
		TestEqual(TEXT("mirror status planned"), Status, FString(TEXT("planned")));
	}

	TSharedPtr<FJsonObject> MirrorNoConfirmParams = MakeParams();
	MirrorNoConfirmParams->SetArrayField(TEXT("files"), Files);
	MirrorNoConfirmParams->SetStringField(TEXT("dest_dir"), MirrorDir);
	MirrorNoConfirmParams->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult MirrorNoConfirm = FMonolithBuildArtifactActions::MirrorScreenshotEvidence(MirrorNoConfirmParams);
	TestFalse(TEXT("mirror_screenshot_evidence rejects no confirm"), MirrorNoConfirm.bSuccess);

	TSharedPtr<FJsonObject> NotifyParams = MakeParams();
	NotifyParams->SetStringField(TEXT("test_name"), TEXT("Monolith build artifact action test"));
	NotifyParams->SetArrayField(TEXT("files"), Files);
	NotifyParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult NotifyDryRun = FMonolithBuildArtifactActions::DiscordScreenshotEvidence(NotifyParams);
	TestTrue(TEXT("discord_screenshot_evidence dry-run succeeds"), NotifyDryRun.bSuccess);
	if (NotifyDryRun.Result.IsValid())
	{
		bool bSent = true;
		TestTrue(TEXT("sent field exists"), NotifyDryRun.Result->TryGetBoolField(TEXT("sent"), bSent));
		TestFalse(TEXT("dry-run does not send"), bSent);
		TestTrue(TEXT("payload preview exists"), NotifyDryRun.Result->HasTypedField<EJson::Object>(TEXT("payload_preview")));
	}

	TSharedPtr<FJsonObject> NotifyNoConfirmParams = MakeParams();
	NotifyNoConfirmParams->SetStringField(TEXT("test_name"), TEXT("Monolith build artifact action test"));
	NotifyNoConfirmParams->SetBoolField(TEXT("send"), true);
	NotifyNoConfirmParams->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult NotifyNoConfirm = FMonolithBuildArtifactActions::DiscordScreenshotEvidence(NotifyNoConfirmParams);
	TestFalse(TEXT("discord_screenshot_evidence rejects send without confirm"), NotifyNoConfirm.bSuccess);

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
