#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "MonolithSourceControlActions.h"
#include "MonolithSourceControlP4Batch.h"
#include "MonolithTestSupport.h"
#include "MonolithToolRegistry.h"
#include "SourceControlPreferences.h"

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

	bool ExpectActionSuccess(FAutomationTestBase& Test, const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("source_control"), Action, Params);
		return Test.TestTrue(
			*FString::Printf(TEXT("source_control.%s accepts tolerant params"), Action),
			Result.bSuccess);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceControlTypedParamsTest, "Monolith.ParamValidation.MonolithSourceControl.TypedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlTypedParamsTest::RunTest(const FString& Parameters)
{
	FScopedSourceControlTestNamespace ScopedNamespace;

	bool bOk = FMonolithTestSupport::RunRegistryContractCases(
		*this,
		TEXT("source_control"),
		[](FMonolithToolRegistry& /*Registry*/)
		{
			FMonolithSourceControlActions::RegisterActions();
		},
		{
			{ TEXT("get_capabilities"), true, TEXT("source_control.get_capabilities registers") },
			{ TEXT("get_status"), true, TEXT("source_control.get_status registers") },
			{ TEXT("checkout"), true, TEXT("source_control.checkout registers") },
			{ TEXT("add"), true, TEXT("source_control.add registers") },
			{ TEXT("checkout_or_add"), true, TEXT("source_control.checkout_or_add registers") },
			{ TEXT("delete"), true, TEXT("source_control.delete registers") },
			{ TEXT("mark_for_delete"), true, TEXT("source_control.mark_for_delete registers") },
			{ TEXT("revert"), true, TEXT("source_control.revert registers") },
			{ TEXT("revert_unchanged"), true, TEXT("source_control.revert_unchanged registers") },
			{ TEXT("list_opened"), true, TEXT("source_control.list_opened registers") },
			{ TEXT("map_depot_paths"), true, TEXT("source_control.map_depot_paths registers") }
		});

	const FMonolithActionExecutionPolicy ListOpenedPolicy =
		FMonolithToolRegistry::Get().GetActionExecutionPolicy(TEXT("source_control"), TEXT("list_opened"));
	const FMonolithActionExecutionPolicy MapDepotPathsPolicy =
		FMonolithToolRegistry::Get().GetActionExecutionPolicy(TEXT("source_control"), TEXT("map_depot_paths"));
	bOk &= TestEqual(TEXT("source_control.list_opened is explicitly read-only"), ListOpenedPolicy.PolicyId, TEXT("read_only"));
	bOk &= TestFalse(TEXT("source_control.list_opened does not rely on name inference"), ListOpenedPolicy.bDefaulted);
	bOk &= TestEqual(TEXT("source_control.map_depot_paths is explicitly read-only"), MapDepotPathsPolicy.PolicyId, TEXT("read_only"));
	bOk &= TestFalse(TEXT("source_control.map_depot_paths does not rely on name inference"), MapDepotPathsPolicy.bDefaulted);

	bool bHasDeleteNewFilesParam = false;
	bool bDeleteNewFilesDefaultsFalse = false;
	for (const FMonolithActionInfo& ActionInfo : FMonolithToolRegistry::Get().GetActions(TEXT("source_control")))
	{
		if (ActionInfo.Action != TEXT("revert") || !ActionInfo.ParamSchema.IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* DeleteNewFiles = nullptr;
		bHasDeleteNewFilesParam = ActionInfo.ParamSchema->TryGetObjectField(TEXT("delete_new_files"), DeleteNewFiles)
			&& DeleteNewFiles && DeleteNewFiles->IsValid();
		FString DefaultValue;
		bDeleteNewFilesDefaultsFalse = bHasDeleteNewFilesParam
			&& (*DeleteNewFiles)->TryGetStringField(TEXT("default"), DefaultValue)
			&& DefaultValue == TEXT("false");
		break;
	}
	bOk &= TestTrue(TEXT("source_control.revert exposes delete_new_files"), bHasDeleteNewFilesParam);
	bOk &= TestTrue(TEXT("source_control.revert preserves added files by default"), bDeleteNewFilesDefaultsFalse);

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("source_control"));

	bOk &= FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("source_control"),
		[](FMonolithToolRegistry& /*Registry*/)
		{
			FMonolithSourceControlActions::RegisterActions();
		},
		{
			{
				TEXT("get_status"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("paths"), 1.0);
				},
				TEXT("paths"),
				TEXT("source_control.get_status rejects non-string/non-array paths")
			},
			{
				TEXT("add"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetNumberField(TEXT("dry_run"), 1.0);
				},
				TEXT("dry_run"),
				TEXT("source_control.add rejects numeric dry_run")
			},
			{
				TEXT("checkout_or_add"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("dry_run"), TEXT("later"));
				},
				TEXT("dry_run"),
				TEXT("source_control.checkout_or_add rejects malformed dry_run string")
			},
			{
				TEXT("delete"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("confirm"), TEXT("sure"));
				},
				TEXT("confirm"),
				TEXT("source_control.delete rejects malformed confirm string")
			},
			{
				TEXT("mark_for_delete"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetNumberField(TEXT("dry_run"), 1.0);
				},
				TEXT("dry_run"),
				TEXT("source_control.mark_for_delete rejects numeric dry_run")
			},
			{
				TEXT("revert"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("confirm"), TEXT("sure"));
				},
				TEXT("confirm"),
				TEXT("source_control.revert rejects malformed confirm string")
			},
			{
				TEXT("revert"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("delete_new_files"), TEXT("later"));
				},
				TEXT("delete_new_files"),
				TEXT("source_control.revert rejects malformed delete_new_files string")
			},
			{
				TEXT("revert_unchanged"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("dry_run"), TEXT("later"));
				},
				TEXT("dry_run"),
				TEXT("source_control.revert_unchanged rejects malformed dry_run string")
			},
			{
				TEXT("list_opened"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("resolve_packages"), TEXT("later"));
				},
				TEXT("resolve_packages"),
				TEXT("source_control.list_opened rejects malformed resolve_packages string")
			},
			{
				TEXT("list_opened"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("limit"), 0.0);
				},
				TEXT("limit"),
				TEXT("source_control.list_opened rejects limit below 1")
			},
			{
				TEXT("list_opened"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("limit"), 5001.0);
				},
				TEXT("limit"),
				TEXT("source_control.list_opened rejects limit above 5000")
			},
			{
				TEXT("list_opened"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("limit"), 1.5);
				},
				TEXT("limit"),
				TEXT("source_control.list_opened rejects non-integral limit")
			},
			{
				TEXT("list_opened"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("limit"), TEXT("1"));
				},
				TEXT("limit"),
				TEXT("source_control.list_opened rejects string-coerced numeric limits")
			},
			{
				TEXT("list_opened"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("changelist"), TEXT("1093-other"));
				},
				TEXT("changelist"),
				TEXT("source_control.list_opened rejects non-decimal changelist values before p4")
			},
			{
				TEXT("list_opened"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("changelist"), 1093.0);
				},
				TEXT("changelist"),
				TEXT("source_control.list_opened rejects number-coerced changelists")
			},
			{
				TEXT("list_opened"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("changelist"), TEXT("1093\nother"));
				},
				TEXT("control character"),
				TEXT("source_control.list_opened rejects changelist control characters before p4")
			},
			{
				TEXT("map_depot_paths"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("paths"), 1.0);
				},
				TEXT("paths"),
				TEXT("source_control.map_depot_paths rejects non-string/non-array paths")
			},
			{
				TEXT("map_depot_paths"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddRepeatedPathArray(
						Params,
						MonolithSourceControlP4::MaxInputPathCount + 1,
						TEXT("//speed/duplicate.uasset"));
				},
				TEXT("at most 5000"),
				TEXT("source_control.map_depot_paths rejects 5001 raw duplicate paths before p4")
			},
			{
				TEXT("map_depot_paths"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddRepeatedPathArray(Params, 1, TEXT("//speed/control\tpath.uasset"));
				},
				TEXT("control character"),
				TEXT("source_control.map_depot_paths rejects path control characters before p4")
			}
		});

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceControlInputToleranceTest, "Monolith.ParamValidation.MonolithSourceControl.InputTolerance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlInputToleranceTest::RunTest(const FString& Parameters)
{
	FScopedSourceControlTestNamespace ScopedNamespace;
	FMonolithSourceControlActions::RegisterActions();

	bool bOk = true;

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("files"), TEXT("Project.uproject"));
		bOk &= ExpectActionSuccess(*this, TEXT("get_status"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("files"), TEXT("Project.uproject"));
		Params->SetStringField(TEXT("dry_run"), TEXT("true"));
		bOk &= ExpectActionSuccess(*this, TEXT("checkout"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("paths"), TEXT("Project.uproject"));
		Params->SetStringField(TEXT("dry_run"), TEXT("yes"));
		bOk &= ExpectActionSuccess(*this, TEXT("delete"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("paths"), TEXT("Project.uproject"));
		Params->SetStringField(TEXT("dry_run"), TEXT("1"));
		bOk &= ExpectActionSuccess(*this, TEXT("checkout_or_add"), Params);
	}

	{
		const bool bPreferenceBefore = USourceControlPreferences::ShouldDeleteNewFilesOnRevert();
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("paths"), TEXT("Speed.uproject"));
		Params->SetStringField(TEXT("dry_run"), TEXT("true"));
		Params->SetStringField(TEXT("delete_new_files"), TEXT("no"));
		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("source_control"), TEXT("revert"), Params);
		bOk &= TestTrue(TEXT("source_control.revert accepts tolerant delete_new_files=false"), Result.bSuccess);
		bool bDeleteNewFiles = true;
		bOk &= TestTrue(TEXT("source_control.revert reports delete_new_files"),
			Result.Result.IsValid() && Result.Result->TryGetBoolField(TEXT("delete_new_files"), bDeleteNewFiles));
		bOk &= TestFalse(TEXT("source_control.revert reports added-file deletion disabled"), bDeleteNewFiles);
		bOk &= TestEqual(
			TEXT("source_control.revert restores the editor-global delete-new-files preference"),
			USourceControlPreferences::ShouldDeleteNewFilesOnRevert(),
			bPreferenceBefore);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Paths;
		Paths.Add(MakeShared<FJsonValueString>(TEXT("Speed.uproject")));
		Paths.Add(MakeShared<FJsonValueString>(TEXT("/Game/Benchmarks/AI/BB_BenchAI.BB_BenchAI")));
		Paths.Add(MakeShared<FJsonValueString>(TEXT("   ")));
		Params->SetArrayField(TEXT("files"), Paths);
		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("source_control"), TEXT("map_depot_paths"), Params);
		bOk &= TestTrue(TEXT("source_control.map_depot_paths accepts relative and package paths"), Result.bSuccess);

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		bOk &= TestTrue(TEXT("source_control.map_depot_paths returns path rows"),
			Result.Result.IsValid() && Result.Result->TryGetArrayField(TEXT("paths"), Rows) && Rows && Rows->Num() == 3);
		if (Rows && Rows->Num() == 3)
		{
			const TSharedPtr<FJsonObject> RelativeRow = (*Rows)[0]->AsObject();
			const TSharedPtr<FJsonObject> PackageRow = (*Rows)[1]->AsObject();
			const TSharedPtr<FJsonObject> WhitespaceRow = (*Rows)[2]->AsObject();
			FString RelativeLocalPath;
			FString PackageLocalPath;
			RelativeRow->TryGetStringField(TEXT("local_path"), RelativeLocalPath);
			PackageRow->TryGetStringField(TEXT("local_path_no_extension"), PackageLocalPath);

			const FString AbsoluteProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FString ExpectedProjectPath = FPaths::Combine(AbsoluteProjectDir, TEXT("Speed.uproject"));
			FPaths::NormalizeFilename(ExpectedProjectPath);
			bOk &= TestEqual(TEXT("relative paths resolve from the project root"), RelativeLocalPath, ExpectedProjectPath);
			bOk &= TestTrue(TEXT("relative path resolution returns an absolute path"), !FPaths::IsRelative(RelativeLocalPath));
			bOk &= TestTrue(TEXT("package local paths are absolute"), !PackageLocalPath.IsEmpty() && !FPaths::IsRelative(PackageLocalPath));
			bOk &= TestTrue(TEXT("package local paths point into project Content"), PackageLocalPath.Contains(TEXT("/Content/Benchmarks/AI/BB_BenchAI")));
			bool bWhitespaceValid = true;
			WhitespaceRow->TryGetBoolField(TEXT("valid"), bWhitespaceValid);
			bOk &= TestFalse(TEXT("whitespace-only path rows are invalid"), bWhitespaceValid);
		}
	}

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
