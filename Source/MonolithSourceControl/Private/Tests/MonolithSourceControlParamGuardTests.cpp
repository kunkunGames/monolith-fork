#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "MonolithSourceControlActions.h"
#include "MonolithSourceControlP4Batch.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	class FScopedSourceControlTestNamespace
	{
	public:
		FScopedSourceControlTestNamespace()
		{
			FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("source_control"));
		}

		~FScopedSourceControlTestNamespace()
		{
			FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("source_control"));
			FMonolithSourceControlActions::RegisterActions();
		}
	};

	void AddValidPathArray(TSharedRef<FJsonObject> Params)
	{
		TArray<TSharedPtr<FJsonValue>> Paths;
		Paths.Add(MakeShared<FJsonValueString>(TEXT("Project.uproject")));
		Params->SetArrayField(TEXT("paths"), Paths);
	}

	void AddRepeatedPathArray(TSharedRef<FJsonObject> Params, int32 Count, const FString& Path)
	{
		TArray<TSharedPtr<FJsonValue>> Paths;
		Paths.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Paths.Add(MakeShared<FJsonValueString>(Path));
		}
		Params->SetArrayField(TEXT("paths"), Paths);
	}

	bool ExpectInvalidParam(
		FAutomationTestBase& Test,
		const TCHAR* Action,
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* ExpectedMessageFragment)
	{
		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("source_control"), Action, Params);
		const FString Prefix = FString::Printf(TEXT("source_control.%s invalid parameter"), Action);

		bool bOk = Test.TestFalse(*FString::Printf(TEXT("%s is rejected"), *Prefix), Result.bSuccess);
		if (!Result.bSuccess)
		{
			bOk &= Test.TestEqual(
				*FString::Printf(TEXT("%s uses JSON-RPC invalid-params code"), *Prefix),
				Result.ErrorCode,
				-32602);
			bOk &= Test.TestTrue(
				*FString::Printf(TEXT("%s explains the rejected field"), *Prefix),
				Result.ErrorMessage.Contains(ExpectedMessageFragment));
		}
		return bOk;
	}

	bool ExpectActionSuccess(
		FAutomationTestBase& Test,
		const TCHAR* Action,
		const TSharedPtr<FJsonObject>& Params)
	{
		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("source_control"), Action, Params);
		return Test.TestTrue(
			*FString::Printf(TEXT("source_control.%s accepts documented parameters"), Action),
			Result.bSuccess);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlTypedParamsTest,
	"Monolith.SourceControl.ParamValidation.TypedParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlTypedParamsTest::RunTest(const FString& Parameters)
{
	FScopedSourceControlTestNamespace ScopedNamespace;
	FMonolithSourceControlActions::RegisterActions();

	const TArray<FString> ExpectedActions = {
		TEXT("get_capabilities"),
		TEXT("get_status"),
		TEXT("checkout"),
		TEXT("add"),
		TEXT("checkout_or_add"),
		TEXT("delete"),
		TEXT("mark_for_delete"),
		TEXT("revert"),
		TEXT("revert_unchanged"),
		TEXT("list_opened"),
		TEXT("map_depot_paths")
	};

	bool bOk = TestEqual(
		TEXT("source_control registers exactly eleven actions"),
		FMonolithToolRegistry::Get().GetActions(TEXT("source_control")).Num(),
		ExpectedActions.Num());
	for (const FString& Action : ExpectedActions)
	{
		bOk &= TestTrue(
			*FString::Printf(TEXT("source_control.%s registers"), *Action),
			FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), Action));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("paths"), 1.0);
		bOk &= ExpectInvalidParam(*this, TEXT("get_status"), Params, TEXT("paths"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddValidPathArray(Params);
		Params->SetNumberField(TEXT("dry_run"), 1.0);
		bOk &= ExpectInvalidParam(*this, TEXT("add"), Params, TEXT("dry_run"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddValidPathArray(Params);
		Params->SetStringField(TEXT("dry_run"), TEXT("true"));
		bOk &= ExpectInvalidParam(*this, TEXT("checkout"), Params, TEXT("dry_run"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddValidPathArray(Params);
		Params->SetField(TEXT("dry_run"), MakeShared<FJsonValueNull>());
		bOk &= ExpectInvalidParam(*this, TEXT("checkout"), Params, TEXT("dry_run"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddValidPathArray(Params);
		Params->SetStringField(TEXT("dry_run"), TEXT("later"));
		bOk &= ExpectInvalidParam(*this, TEXT("checkout_or_add"), Params, TEXT("dry_run"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddValidPathArray(Params);
		Params->SetStringField(TEXT("confirm"), TEXT("sure"));
		bOk &= ExpectInvalidParam(*this, TEXT("delete"), Params, TEXT("confirm"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddValidPathArray(Params);
		Params->SetNumberField(TEXT("dry_run"), 1.0);
		bOk &= ExpectInvalidParam(*this, TEXT("mark_for_delete"), Params, TEXT("dry_run"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddValidPathArray(Params);
		Params->SetStringField(TEXT("confirm"), TEXT("sure"));
		bOk &= ExpectInvalidParam(*this, TEXT("revert"), Params, TEXT("confirm"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddValidPathArray(Params);
		Params->SetStringField(TEXT("dry_run"), TEXT("later"));
		bOk &= ExpectInvalidParam(*this, TEXT("revert_unchanged"), Params, TEXT("dry_run"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("resolve_packages"), TEXT("true"));
		bOk &= ExpectInvalidParam(*this, TEXT("list_opened"), Params, TEXT("resolve_packages"));
	}

	for (const double InvalidLimit : { 0.0, 5001.0, 1.5 })
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), InvalidLimit);
		bOk &= ExpectInvalidParam(*this, TEXT("list_opened"), Params, TEXT("limit"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("1"));
		bOk &= ExpectInvalidParam(*this, TEXT("list_opened"), Params, TEXT("limit"));
	}

	const TArray<FString> InvalidChangelists = { TEXT("1093-other"), TEXT("1093\nother") };
	for (const FString& InvalidChangelist : InvalidChangelists)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("changelist"), InvalidChangelist);
		bOk &= ExpectInvalidParam(*this, TEXT("list_opened"), Params, TEXT("changelist"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("changelist"), 1093.0);
		bOk &= ExpectInvalidParam(*this, TEXT("list_opened"), Params, TEXT("changelist"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("paths"), 1.0);
		bOk &= ExpectInvalidParam(*this, TEXT("map_depot_paths"), Params, TEXT("paths"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddRepeatedPathArray(
			Params,
			MonolithSourceControlP4::MaxInputPathCount + 1,
			TEXT("//project/Content/Test/duplicate.uasset"));
		bOk &= ExpectInvalidParam(*this, TEXT("map_depot_paths"), Params, TEXT("at most 5000"));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		AddRepeatedPathArray(Params, 1, TEXT("//project/Content/Test/control\tpath.uasset"));
		bOk &= ExpectInvalidParam(*this, TEXT("map_depot_paths"), Params, TEXT("control character"));
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlDocumentedInputTest,
	"Monolith.SourceControl.ParamValidation.DocumentedInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlDocumentedInputTest::RunTest(const FString& Parameters)
{
	FScopedSourceControlTestNamespace ScopedNamespace;
	FMonolithSourceControlActions::RegisterActions();

	bool bOk = true;

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("files"), FPaths::GetCleanFilename(FPaths::GetProjectFilePath()));
		bOk &= ExpectActionSuccess(*this, TEXT("get_status"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("files"), FPaths::GetCleanFilename(FPaths::GetProjectFilePath()));
		Params->SetBoolField(TEXT("dry_run"), true);
		bOk &= ExpectActionSuccess(*this, TEXT("checkout"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("paths"), FPaths::GetCleanFilename(FPaths::GetProjectFilePath()));
		Params->SetBoolField(TEXT("dry_run"), true);
		bOk &= ExpectActionSuccess(*this, TEXT("delete"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("paths"), FPaths::GetCleanFilename(FPaths::GetProjectFilePath()));
		Params->SetBoolField(TEXT("dry_run"), true);
		bOk &= ExpectActionSuccess(*this, TEXT("checkout_or_add"), Params);
	}

	{
		const FString ProjectFilename = FPaths::GetCleanFilename(FPaths::GetProjectFilePath());
		bOk &= TestFalse(TEXT("test host exposes a project filename"), ProjectFilename.IsEmpty());

		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Paths;
		Paths.Add(MakeShared<FJsonValueString>(ProjectFilename));
		Paths.Add(MakeShared<FJsonValueString>(TEXT("/Game/SourceControlTest/SC_TestAsset.SC_TestAsset")));
		Paths.Add(MakeShared<FJsonValueString>(TEXT("   ")));
		Params->SetArrayField(TEXT("files"), Paths);

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("source_control"), TEXT("map_depot_paths"), Params);
		bOk &= TestTrue(TEXT("source_control.map_depot_paths accepts relative and package paths"), Result.bSuccess);

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		const bool bHasExpectedRows =
			Result.Result.IsValid()
			&& Result.Result->TryGetArrayField(TEXT("paths"), Rows)
			&& Rows
			&& Rows->Num() == 3;
		bOk &= TestTrue(TEXT("source_control.map_depot_paths returns one row per input"), bHasExpectedRows);
		if (bHasExpectedRows)
		{
			const TSharedPtr<FJsonObject> RelativeRow = (*Rows)[0]->AsObject();
			const TSharedPtr<FJsonObject> PackageRow = (*Rows)[1]->AsObject();
			const TSharedPtr<FJsonObject> WhitespaceRow = (*Rows)[2]->AsObject();

			FString RelativeLocalPath;
			FString PackageLocalPath;
			RelativeRow->TryGetStringField(TEXT("local_path"), RelativeLocalPath);
			PackageRow->TryGetStringField(TEXT("local_path_no_extension"), PackageLocalPath);

			const FString AbsoluteProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FString ExpectedProjectPath = FPaths::Combine(AbsoluteProjectDir, ProjectFilename);
			FPaths::NormalizeFilename(ExpectedProjectPath);

			bOk &= TestEqual(TEXT("relative paths resolve from the project root"), RelativeLocalPath, ExpectedProjectPath);
			bOk &= TestTrue(TEXT("relative path resolution returns an absolute path"), !FPaths::IsRelative(RelativeLocalPath));
			bOk &= TestTrue(TEXT("package local paths are absolute"), !PackageLocalPath.IsEmpty() && !FPaths::IsRelative(PackageLocalPath));
			bOk &= TestTrue(
				TEXT("package local paths point into the project Content directory"),
				PackageLocalPath.Contains(TEXT("/Content/SourceControlTest/SC_TestAsset")));

			bool bWhitespaceValid = true;
			WhitespaceRow->TryGetBoolField(TEXT("valid"), bWhitespaceValid);
			bOk &= TestFalse(TEXT("whitespace-only path rows are invalid"), bWhitespaceValid);
		}
	}

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
