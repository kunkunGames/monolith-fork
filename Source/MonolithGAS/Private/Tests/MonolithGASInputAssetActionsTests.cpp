// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "Serialization/Archive.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithGASInputAssetActionsTestDetail
{
	static const TArray<FString> RequiredActions = {
		TEXT("list_input_actions"),
		TEXT("get_input_action"),
		TEXT("create_input_action"),
		TEXT("set_input_action_properties"),
		TEXT("list_input_mapping_contexts"),
		TEXT("get_input_mapping_context"),
		TEXT("create_input_mapping_context"),
		TEXT("add_input_mapping"),
		TEXT("remove_input_mapping"),
		TEXT("validate_input_mappings")
	};

	static const TArray<FString> MutatingActions = {
		TEXT("create_input_action"),
		TEXT("set_input_action_properties"),
		TEXT("create_input_mapping_context"),
		TEXT("add_input_mapping"),
		TEXT("remove_input_mapping")
	};

	static FString MakeObjectPath(const FString& PackagePath)
	{
		return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
	}

	static FMonolithActionResult Execute(
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("input"), Action, Params);
	}

	static TSharedPtr<FJsonObject> MakeCreateParams(
		const FString& AssetPath,
		bool bDryRun,
		bool bConfirm)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (bDryRun)
		{
			Params->SetBoolField(TEXT("dry_run"), true);
		}
		if (bConfirm)
		{
			Params->SetBoolField(TEXT("confirm"), true);
		}
		Params->SetBoolField(TEXT("save"), false);
		return Params;
	}

	static TSharedPtr<FJsonObject> MakeMappingParams(
		const FString& ContextPath,
		const FString& ActionPath,
		const FString& Key,
		bool bDryRun = false,
		bool bConfirm = true)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("context_path"), ContextPath);
		Params->SetStringField(TEXT("action_path"), ActionPath);
		Params->SetStringField(TEXT("key"), Key);
		if (bDryRun)
		{
			Params->SetBoolField(TEXT("dry_run"), true);
		}
		if (bConfirm)
		{
			Params->SetBoolField(TEXT("confirm"), true);
		}
		Params->SetBoolField(TEXT("save"), false);
		return Params;
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<FString> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(static_cast<int32>(Values.size()));
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static bool GetBool(const FMonolithActionResult& ActionResult, const TCHAR* Field, bool Default = false)
	{
		bool Value = Default;
		if (ActionResult.Result.IsValid())
		{
			ActionResult.Result->TryGetBoolField(Field, Value);
		}
		return Value;
	}

	static int32 GetInt(const FMonolithActionResult& ActionResult, const TCHAR* Field, int32 Default = 0)
	{
		double Value = static_cast<double>(Default);
		if (ActionResult.Result.IsValid())
		{
			ActionResult.Result->TryGetNumberField(Field, Value);
		}
		return static_cast<int32>(Value);
	}

	static FString GetString(
		const FMonolithActionResult& ActionResult,
		const TCHAR* Field,
		const FString& Default = FString())
	{
		FString Value = Default;
		if (ActionResult.Result.IsValid())
		{
			ActionResult.Result->TryGetStringField(Field, Value);
		}
		return Value;
	}

	static TSharedPtr<FJsonObject> GetErrorDataObject(
		const FMonolithActionResult& ActionResult)
	{
		if (!ActionResult.ErrorData.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (!ActionResult.ErrorData->TryGetObject(ErrorObject)
			|| !ErrorObject
			|| !ErrorObject->IsValid())
		{
			return nullptr;
		}
		return *ErrorObject;
	}

	static bool GetErrorBool(
		const FMonolithActionResult& ActionResult,
		const TCHAR* Field,
		bool Default = false)
	{
		bool Value = Default;
		if (const TSharedPtr<FJsonObject> ErrorObject = GetErrorDataObject(ActionResult))
		{
			ErrorObject->TryGetBoolField(Field, Value);
		}
		return Value;
	}

	static int32 GetErrorInt(
		const FMonolithActionResult& ActionResult,
		const TCHAR* Field,
		int32 Default = 0)
	{
		double Value = static_cast<double>(Default);
		if (const TSharedPtr<FJsonObject> ErrorObject = GetErrorDataObject(ActionResult))
		{
			ErrorObject->TryGetNumberField(Field, Value);
		}
		return static_cast<int32>(Value);
	}

	static FString GetErrorString(
		const FMonolithActionResult& ActionResult,
		const TCHAR* Field,
		const FString& Default = FString())
	{
		FString Value = Default;
		if (const TSharedPtr<FJsonObject> ErrorObject = GetErrorDataObject(ActionResult))
		{
			ErrorObject->TryGetStringField(Field, Value);
		}
		return Value;
	}

	static bool ResultRowsAreScopedToGame(
		const FMonolithActionResult& ActionResult,
		const TCHAR* ArrayField)
	{
		if (!ActionResult.bSuccess || !ActionResult.Result.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!ActionResult.Result->TryGetArrayField(ArrayField, Rows) || !Rows)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			FString PackagePath;
			if (!RowValue.IsValid()
				|| !RowValue->TryGetObject(Row)
				|| !Row
				|| !Row->IsValid()
				|| !(*Row)->TryGetStringField(TEXT("package_path"), PackagePath)
				|| (PackagePath != TEXT("/Game") && !PackagePath.StartsWith(TEXT("/Game/"))))
			{
				return false;
			}
		}
		return true;
	}

	static bool ResultRowsContainPackage(
		const FMonolithActionResult& ActionResult,
		const TCHAR* ArrayField,
		const FString& ExpectedPackagePath)
	{
		if (!ActionResult.bSuccess || !ActionResult.Result.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!ActionResult.Result->TryGetArrayField(ArrayField, Rows) || !Rows)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			FString PackagePath;
			if (RowValue.IsValid()
				&& RowValue->TryGetObject(Row)
				&& Row
				&& Row->IsValid()
				&& (*Row)->TryGetStringField(TEXT("package_path"), PackagePath)
				&& PackagePath == ExpectedPackagePath)
			{
				return true;
			}
		}
		return false;
	}

	static int32 CountLiveObjectsOfClass(const UClass* Class)
	{
		TArray<UObject*> Objects;
		GetObjectsOfClass(
			Class,
			Objects,
			true,
			RF_ClassDefaultObject | RF_ArchetypeObject,
			EInternalObjectFlags::Garbage);
		return Objects.Num();
	}

	static UObject* FindTestAsset(const FString& PackagePath)
	{
		return StaticFindObject(UObject::StaticClass(), nullptr, *MakeObjectPath(PackagePath));
	}

	static void CleanupTestAsset(const FString& PackagePath)
	{
		UObject* Asset = FindTestAsset(PackagePath);
		if (!Asset)
		{
			if (UPackage* Package = FindPackage(nullptr, *PackagePath))
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
			return;
		}

		if (UInputMappingContext* Context = Cast<UInputMappingContext>(Asset))
		{
			Context->UnmapAll();
		}

		UPackage* Package = Asset->GetOutermost();
		FAssetRegistryModule::AssetDeleted(Asset);
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}

	struct FScopedSaveBlocker
	{
		explicit FScopedSaveBlocker(const FString& PackagePath)
			: Filename(FPackageName::LongPackageNameToFilename(
				PackagePath,
				FPackageName::GetAssetPackageExtension()))
		{
			IFileManager& FileManager = IFileManager::Get();
			if (!FileManager.FileExists(*Filename)
				&& !FileManager.DirectoryExists(*Filename)
				&& FileManager.MakeDirectory(*FPaths::GetPath(Filename), true))
			{
				LockedFile.Reset(FileManager.CreateFileWriter(*Filename));
				if (LockedFile)
				{
					uint8 Marker = 0;
					LockedFile->Serialize(&Marker, sizeof(Marker));
					LockedFile->Flush();
					bReady = !LockedFile->IsError();
				}
			}
		}

		~FScopedSaveBlocker()
		{
			if (bReady)
			{
				LockedFile.Reset();
				IFileManager& FileManager = IFileManager::Get();
				if (FileManager.FileExists(*Filename))
				{
					FileManager.Delete(*Filename, false, true);
				}
			}
		}

		bool IsReady() const
		{
			return bReady && LockedFile.IsValid() && IFileManager::Get().FileExists(*Filename);
		}

		FString Filename;
		TUniquePtr<FArchive> LockedFile;
		bool bReady = false;
	};

	struct FScopedInputAssets
	{
		FString ActionA;
		FString ActionB;
		FString Context;

		FScopedInputAssets()
		{
			const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Root = FString::Printf(TEXT("/Game/Tests/Monolith/Input/%s"), *Suffix);
			ActionA = Root + TEXT("/IA_Primary");
			ActionB = Root + TEXT("/IA_Secondary");
			Context = Root + TEXT("/IMC_Test");
		}

		~FScopedInputAssets()
		{
			CleanupTestAsset(Context);
			CleanupTestAsset(ActionB);
			CleanupTestAsset(ActionA);
		}

		bool Create(bool bCreateSecondary = false) const
		{
			const FMonolithActionResult PrimaryResult =
				Execute(TEXT("create_input_action"), MakeCreateParams(ActionA, false, true));
			if (!PrimaryResult.bSuccess)
			{
				return false;
			}

			if (bCreateSecondary)
			{
				const FMonolithActionResult SecondaryResult =
					Execute(TEXT("create_input_action"), MakeCreateParams(ActionB, false, true));
				if (!SecondaryResult.bSuccess)
				{
					return false;
				}
			}

			const FMonolithActionResult ContextResult =
				Execute(TEXT("create_input_mapping_context"), MakeCreateParams(Context, false, true));
			return ContextResult.bSuccess;
		}

		UInputMappingContext* GetContext() const
		{
			return Cast<UInputMappingContext>(FindTestAsset(Context));
		}

		bool HasSavedFiles() const
		{
			for (const FString& PackagePath : { ActionA, ActionB, Context })
			{
				const FString Filename = FPackageName::LongPackageNameToFilename(
					PackagePath,
					FPackageName::GetAssetPackageExtension());
				if (IFileManager::Get().FileExists(*Filename))
				{
					return true;
				}
			}
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetRegistrationTest,
	"Monolith.ParamGuard.GAS.InputAssets.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bPassed = true;
	for (const FString& Action : RequiredActions)
	{
		bPassed &= TestTrue(
			*FString::Printf(TEXT("input.%s is registered by MonolithGAS startup"), *Action),
			Registry.HasAction(TEXT("input"), Action));
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetWriteGateTest,
	"Monolith.ParamGuard.GAS.InputAssets.WriteGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetWriteGateTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	bool bPassed = true;
	for (const FString& Action : MutatingActions)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (Action == TEXT("create_input_action")
			|| Action == TEXT("set_input_action_properties")
			|| Action == TEXT("create_input_mapping_context"))
		{
			Params->SetStringField(
				TEXT("asset_path"),
				TEXT("/Game/Tests/Monolith/Input/WriteGate/Asset"));
		}
		else
		{
			Params->SetStringField(
				TEXT("context_path"),
				TEXT("/Game/Tests/Monolith/Input/WriteGate/IMC_Missing"));
			Params->SetStringField(
				TEXT("action_path"),
				TEXT("/Game/Tests/Monolith/Input/WriteGate/IA_Missing"));
			Params->SetStringField(TEXT("key"), TEXT("SpaceBar"));
		}

		const FMonolithActionResult Result = Execute(Action, Params);
		bPassed &= TestFalse(
			*FString::Printf(TEXT("input.%s rejects an unconfirmed write"), *Action),
			Result.bSuccess);
		bPassed &= TestEqual(
			*FString::Printf(TEXT("input.%s reports invalid params"), *Action),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		bPassed &= TestTrue(
			*FString::Printf(TEXT("input.%s explains the mutation gate"), *Action),
			Result.ErrorMessage.Contains(TEXT("dry_run=true"))
				&& Result.ErrorMessage.Contains(TEXT("confirm=true")));
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetDryRunTest,
	"Monolith.ParamGuard.GAS.InputAssets.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetDryRunTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ActionPath =
		FString::Printf(TEXT("/Game/Tests/Monolith/Input/%s/IA_DryRun"), *Suffix);
	const FString ContextPath =
		FString::Printf(TEXT("/Game/Tests/Monolith/Input/%s/IMC_DryRun"), *Suffix);

	TSharedPtr<FJsonObject> ActionPreviewParams = MakeCreateParams(ActionPath, true, false);
	ActionPreviewParams->SetStringField(TEXT("value_type"), TEXT("Axis2D"));
	ActionPreviewParams->SetStringField(TEXT("description"), TEXT("Proposed action"));
	ActionPreviewParams->SetBoolField(TEXT("consume_input"), false);
	ActionPreviewParams->SetBoolField(TEXT("trigger_when_paused"), true);
	ActionPreviewParams->SetStringField(TEXT("accumulation"), TEXT("Cumulative"));
	const FMonolithActionResult ActionResult =
		Execute(TEXT("create_input_action"), ActionPreviewParams);

	TSharedPtr<FJsonObject> ContextPreviewParams = MakeCreateParams(ContextPath, true, false);
	ContextPreviewParams->SetStringField(TEXT("description"), TEXT("Proposed context"));
	const FMonolithActionResult ContextResult =
		Execute(TEXT("create_input_mapping_context"), ContextPreviewParams);

	bool bPassed = true;
	bPassed &= TestTrue(TEXT("InputAction dry-run succeeds"), ActionResult.bSuccess);
	bPassed &= TestTrue(TEXT("InputAction dry-run predicts creation"), GetBool(ActionResult, TEXT("would_create")));
	bPassed &= TestFalse(TEXT("InputAction dry-run does not report creation"), GetBool(ActionResult, TEXT("created")));
	bPassed &= TestTrue(TEXT("InputAction dry-run is marked dry_run"), GetBool(ActionResult, TEXT("dry_run")));
	bPassed &= TestEqual(TEXT("InputAction preview returns proposed value type"), GetString(ActionResult, TEXT("value_type")), TEXT("Axis2D"));
	bPassed &= TestEqual(TEXT("InputAction preview returns proposed description"), GetString(ActionResult, TEXT("description")), TEXT("Proposed action"));
	bPassed &= TestFalse(TEXT("InputAction preview returns proposed consume flag"), GetBool(ActionResult, TEXT("consume_input"), true));
	bPassed &= TestTrue(TEXT("InputAction preview returns proposed paused flag"), GetBool(ActionResult, TEXT("trigger_when_paused")));
	bPassed &= TestEqual(TEXT("InputAction preview returns proposed accumulation"), GetString(ActionResult, TEXT("accumulation")), TEXT("Cumulative"));
	bPassed &= TestEqual(TEXT("InputAction preview identifies proposed state"), GetString(ActionResult, TEXT("preview_state")), TEXT("proposed"));
	bPassed &= TestNull(TEXT("InputAction dry-run creates no object"), FindTestAsset(ActionPath));
	bPassed &= TestNull(TEXT("InputAction dry-run creates no package"), FindPackage(nullptr, *ActionPath));

	bPassed &= TestTrue(TEXT("InputMappingContext dry-run succeeds"), ContextResult.bSuccess);
	bPassed &= TestTrue(TEXT("InputMappingContext dry-run predicts creation"), GetBool(ContextResult, TEXT("would_create")));
	bPassed &= TestFalse(TEXT("InputMappingContext dry-run does not report creation"), GetBool(ContextResult, TEXT("created")));
	bPassed &= TestTrue(TEXT("InputMappingContext dry-run is marked dry_run"), GetBool(ContextResult, TEXT("dry_run")));
	bPassed &= TestEqual(TEXT("InputMappingContext preview returns proposed description"), GetString(ContextResult, TEXT("description")), TEXT("Proposed context"));
	bPassed &= TestEqual(TEXT("InputMappingContext preview identifies proposed state"), GetString(ContextResult, TEXT("preview_state")), TEXT("proposed"));
	bPassed &= TestNull(TEXT("InputMappingContext dry-run creates no object"), FindTestAsset(ContextPath));
	bPassed &= TestNull(TEXT("InputMappingContext dry-run creates no package"), FindPackage(nullptr, *ContextPath));
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetStrictParamsTest,
	"Monolith.ParamGuard.GAS.InputAssets.StrictParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetStrictParamsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	struct FInvalidCase
	{
		FString Label;
		FString Action;
		FString ExpectedField;
		TSharedPtr<FJsonObject> Params;
	};

	TArray<FInvalidCase> Cases;

	TSharedPtr<FJsonObject> StringSave = MakeCreateParams(
		TEXT("/Game/Tests/Monolith/Input/Strict/IA_StringSave"),
		true,
		false);
	StringSave->SetStringField(TEXT("save"), TEXT("false"));
	Cases.Add({ TEXT("string save"), TEXT("create_input_action"), TEXT("save"), StringSave });

	TSharedPtr<FJsonObject> StringConfirm = MakeShared<FJsonObject>();
	StringConfirm->SetStringField(
		TEXT("asset_path"),
		TEXT("/Game/Tests/Monolith/Input/Strict/IMC_StringConfirm"));
	StringConfirm->SetStringField(TEXT("confirm"), TEXT("true"));
	Cases.Add({
		TEXT("string confirm"),
		TEXT("create_input_mapping_context"),
		TEXT("confirm"),
		StringConfirm
	});

	TSharedPtr<FJsonObject> BadContextPaths = MakeShared<FJsonObject>();
	BadContextPaths->SetArrayField(
		TEXT("context_paths"),
		{ MakeShared<FJsonValueNumber>(42.0) });
	Cases.Add({
		TEXT("numeric context path item"),
		TEXT("validate_input_mappings"),
		TEXT("context_paths[0]"),
		BadContextPaths
	});

	TSharedPtr<FJsonObject> PartialSource = MakeMappingParams(
		TEXT("/Game/Tests/Monolith/Input/Strict/IMC_Missing"),
		TEXT("/Game/Tests/Monolith/Input/Strict/IA_Missing"),
		TEXT("SpaceBar"),
		true,
		false);
	PartialSource->SetStringField(
		TEXT("source_context_path"),
		TEXT("/Game/Tests/Monolith/Input/Strict/IMC_Source"));
	Cases.Add({
		TEXT("partial source mapping"),
		TEXT("add_input_mapping"),
		TEXT("source_action_path"),
		PartialSource
	});

	TSharedPtr<FJsonObject> NullModifierArray = MakeMappingParams(
		TEXT("/Game/Tests/Monolith/Input/Strict/IMC_Missing"),
		TEXT("/Game/Tests/Monolith/Input/Strict/IA_Missing"),
		TEXT("SpaceBar"),
		true,
		false);
	NullModifierArray->SetField(TEXT("modifier_classes"), MakeShared<FJsonValueNull>());
	Cases.Add({
		TEXT("null modifier array"),
		TEXT("add_input_mapping"),
		TEXT("modifier_classes"),
		NullModifierArray
	});

	TSharedPtr<FJsonObject> OutsideGame = MakeCreateParams(
		TEXT("/Engine/Tests/IA_OutsideGame"),
		true,
		false);
	Cases.Add({
		TEXT("outside Game path"),
		TEXT("create_input_action"),
		TEXT("/Game"),
		OutsideGame
	});

	TSharedPtr<FJsonObject> NumericAssetPath = MakeCreateParams(
		TEXT("/Game/Tests/Monolith/Input/Strict/IA_NumericPath"),
		true,
		false);
	NumericAssetPath->SetNumberField(TEXT("asset_path"), 42.0);
	Cases.Add({
		TEXT("numeric required asset path"),
		TEXT("create_input_action"),
		TEXT("asset_path"),
		NumericAssetPath
	});

	TSharedPtr<FJsonObject> NumericKey = MakeMappingParams(
		TEXT("/Game/Tests/Monolith/Input/Strict/IMC_Missing"),
		TEXT("/Game/Tests/Monolith/Input/Strict/IA_Missing"),
		TEXT("SpaceBar"),
		true,
		false);
	NumericKey->SetNumberField(TEXT("key"), 42.0);
	Cases.Add({
		TEXT("numeric required key"),
		TEXT("add_input_mapping"),
		TEXT("key"),
		NumericKey
	});

	TSharedPtr<FJsonObject> MismatchedObjectPath = MakeCreateParams(
		TEXT("/Game/Tests/Monolith/Input/Strict/IA_Mismatch.Other"),
		true,
		false);
	Cases.Add({
		TEXT("mismatched object path"),
		TEXT("create_input_action"),
		TEXT("object name"),
		MismatchedObjectPath
	});

	TSharedPtr<FJsonObject> DoubleSlashRoot = MakeShared<FJsonObject>();
	DoubleSlashRoot->SetStringField(TEXT("path"), TEXT("/Game//Input"));
	Cases.Add({
		TEXT("double-slash search root"),
		TEXT("list_input_actions"),
		TEXT("search path"),
		DoubleSlashRoot
	});

	TSharedPtr<FJsonObject> ObjectStyleRoot = MakeShared<FJsonObject>();
	ObjectStyleRoot->SetStringField(TEXT("path"), TEXT("/Game/Input.Bad"));
	Cases.Add({
		TEXT("object-style search root"),
		TEXT("list_input_mapping_contexts"),
		TEXT("search path"),
		ObjectStyleRoot
	});

	bool bPassed = true;
	for (const FInvalidCase& TestCase : Cases)
	{
		const FMonolithActionResult Result = Execute(TestCase.Action, TestCase.Params);
		bPassed &= TestFalse(*TestCase.Label, Result.bSuccess);
		bPassed &= TestEqual(
			*FString::Printf(TEXT("%s uses invalid-params"), *TestCase.Label),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		bPassed &= TestTrue(
			*FString::Printf(TEXT("%s identifies the bad contract"), *TestCase.Label),
			Result.ErrorMessage.Contains(TestCase.ExpectedField));
	}

	TSharedPtr<FJsonObject> EmptySelection = MakeShared<FJsonObject>();
	EmptySelection->SetArrayField(TEXT("context_paths"), TArray<TSharedPtr<FJsonValue>>());
	EmptySelection->SetStringField(TEXT("path"), TEXT("/Game//MustNotBeScanned"));
	const FMonolithActionResult EmptySelectionResult =
		Execute(TEXT("validate_input_mappings"), EmptySelection);
	bPassed &= TestTrue(
		TEXT("explicit empty context selection succeeds without global fallback"),
		EmptySelectionResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("explicit empty context selection checks zero contexts"),
		GetInt(EmptySelectionResult, TEXT("contexts_checked"), -1),
		0);
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetCreationUndoTest,
	"Monolith.ParamGuard.GAS.InputAssets.CreationUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetCreationUndoTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Root = FString::Printf(TEXT("/Game/Tests/Monolith/Input/%s"), *Suffix);
	const FString ActionPath = Root + TEXT("/IA_Undo");
	const FString ContextPath = Root + TEXT("/IMC_Undo");
	bool bPassed = TestNotNull(TEXT("editor transaction system is available"), GEditor);
	if (!GEditor)
	{
		return false;
	}

	TSharedPtr<FJsonObject> ActionParams = MakeCreateParams(ActionPath, false, true);
	ActionParams->SetStringField(TEXT("description"), TEXT("Action creation undo proof"));
	const FMonolithActionResult ActionCreate =
		Execute(TEXT("create_input_action"), ActionParams);
	bPassed &= TestTrue(TEXT("undo fixture InputAction is created"), ActionCreate.bSuccess);
	bPassed &= TestNotNull(TEXT("created InputAction is present before undo"), FindTestAsset(ActionPath));
	bPassed &= TestTrue(TEXT("InputAction creation transaction can be undone"), GEditor->UndoTransaction());
	bPassed &= TestNull(TEXT("undo removes InputAction from its asset path"), FindTestAsset(ActionPath));
	CollectGarbage(RF_NoFlags);
	bPassed &= TestTrue(TEXT("InputAction creation transaction can be redone"), GEditor->RedoTransaction());
	UInputAction* RedoneAction = Cast<UInputAction>(FindTestAsset(ActionPath));
	if (TestNotNull(TEXT("redo restores InputAction at its asset path"), RedoneAction))
	{
		bPassed &= TestEqual(
			TEXT("redo preserves InputAction creation state"),
			RedoneAction->ActionDescription.ToString(),
			TEXT("Action creation undo proof"));
	}

	TSharedPtr<FJsonObject> ContextParams = MakeCreateParams(ContextPath, false, true);
	ContextParams->SetStringField(TEXT("description"), TEXT("Context creation undo proof"));
	const FMonolithActionResult ContextCreate =
		Execute(TEXT("create_input_mapping_context"), ContextParams);
	bPassed &= TestTrue(TEXT("undo fixture InputMappingContext is created"), ContextCreate.bSuccess);
	bPassed &= TestNotNull(TEXT("created InputMappingContext is present before undo"), FindTestAsset(ContextPath));
	bPassed &= TestTrue(TEXT("InputMappingContext creation transaction can be undone"), GEditor->UndoTransaction());
	bPassed &= TestNull(TEXT("undo removes InputMappingContext from its asset path"), FindTestAsset(ContextPath));
	CollectGarbage(RF_NoFlags);
	bPassed &= TestTrue(TEXT("InputMappingContext creation transaction can be redone"), GEditor->RedoTransaction());
	UInputMappingContext* RedoneContext = Cast<UInputMappingContext>(FindTestAsset(ContextPath));
	if (TestNotNull(TEXT("redo restores InputMappingContext at its asset path"), RedoneContext))
	{
		bPassed &= TestEqual(
			TEXT("redo preserves InputMappingContext creation state"),
			RedoneContext->ContextDescription.ToString(),
			TEXT("Context creation undo proof"));
	}

	CleanupTestAsset(ContextPath);
	CleanupTestAsset(ActionPath);
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetLifecycleAndCloneTest,
	"Monolith.ParamGuard.GAS.InputAssets.LifecycleAndClone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetLifecycleAndCloneTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	FScopedInputAssets Assets;
	if (!TestTrue(TEXT("disposable input fixture is created"), Assets.Create(true)))
	{
		return false;
	}

	bool bPassed = true;
	UInputAction* PrimaryAction = Cast<UInputAction>(FindTestAsset(Assets.ActionA));
	UInputMappingContext* Context = Assets.GetContext();
	const FMonolithActionResult DefaultActionList =
		Execute(TEXT("list_input_actions"), MakeShared<FJsonObject>());
	const FMonolithActionResult DefaultContextList =
		Execute(TEXT("list_input_mapping_contexts"), MakeShared<FJsonObject>());
	bPassed &= TestTrue(
		TEXT("default InputAction listing is scoped to /Game"),
		ResultRowsAreScopedToGame(DefaultActionList, TEXT("actions")));
	bPassed &= TestTrue(
		TEXT("default InputAction listing includes the project fixture"),
		ResultRowsContainPackage(DefaultActionList, TEXT("actions"), Assets.ActionA));
	bPassed &= TestTrue(
		TEXT("default InputMappingContext listing is scoped to /Game"),
		ResultRowsAreScopedToGame(DefaultContextList, TEXT("contexts")));
	bPassed &= TestTrue(
		TEXT("default InputMappingContext listing includes the project fixture"),
		ResultRowsContainPackage(DefaultContextList, TEXT("contexts"), Assets.Context));
	if (TestNotNull(TEXT("created InputAction is loadable"), PrimaryAction))
	{
		bPassed &= TestTrue(
			TEXT("created InputAction participates in editor transactions"),
			PrimaryAction->HasAnyFlags(RF_Transactional));
	}
	if (TestNotNull(TEXT("created mapping context is loadable"), Context))
	{
		bPassed &= TestTrue(
			TEXT("created InputMappingContext participates in editor transactions"),
			Context->HasAnyFlags(RF_Transactional));
	}

	const EInputActionValueType OriginalValueType = PrimaryAction
		? PrimaryAction->ValueType
		: EInputActionValueType::Boolean;
	const FString OriginalActionDescription = PrimaryAction
		? PrimaryAction->ActionDescription.ToString()
		: FString();
	const FString OriginalContextDescription = Context
		? Context->ContextDescription.ToString()
		: FString();

	TSharedPtr<FJsonObject> OverwritePreview = MakeCreateParams(Assets.ActionA, true, false);
	OverwritePreview->SetBoolField(TEXT("overwrite"), true);
	OverwritePreview->SetStringField(TEXT("value_type"), TEXT("Axis2D"));
	OverwritePreview->SetStringField(TEXT("description"), TEXT("Overwrite proposal"));
	const FMonolithActionResult OverwritePreviewResult =
		Execute(TEXT("create_input_action"), OverwritePreview);
	bPassed &= TestTrue(TEXT("InputAction overwrite dry-run succeeds"), OverwritePreviewResult.bSuccess);
	bPassed &= TestEqual(TEXT("overwrite dry-run returns proposed value type"), GetString(OverwritePreviewResult, TEXT("value_type")), TEXT("Axis2D"));
	bPassed &= TestEqual(TEXT("overwrite dry-run returns proposed description"), GetString(OverwritePreviewResult, TEXT("description")), TEXT("Overwrite proposal"));

	TSharedPtr<FJsonObject> SetPreview = MakeCreateParams(Assets.ActionA, true, false);
	SetPreview->SetStringField(TEXT("value_type"), TEXT("Axis3D"));
	SetPreview->SetStringField(TEXT("description"), TEXT("Set proposal"));
	const FMonolithActionResult SetPreviewResult =
		Execute(TEXT("set_input_action_properties"), SetPreview);
	bPassed &= TestTrue(TEXT("InputAction property dry-run succeeds"), SetPreviewResult.bSuccess);
	bPassed &= TestEqual(TEXT("property dry-run returns proposed value type"), GetString(SetPreviewResult, TEXT("value_type")), TEXT("Axis3D"));
	bPassed &= TestEqual(TEXT("property dry-run returns proposed description"), GetString(SetPreviewResult, TEXT("description")), TEXT("Set proposal"));

	TSharedPtr<FJsonObject> ContextPreview = MakeCreateParams(Assets.Context, true, false);
	ContextPreview->SetBoolField(TEXT("overwrite"), true);
	ContextPreview->SetStringField(TEXT("description"), TEXT("Context proposal"));
	const FMonolithActionResult ContextPreviewResult =
		Execute(TEXT("create_input_mapping_context"), ContextPreview);
	bPassed &= TestTrue(TEXT("InputMappingContext overwrite dry-run succeeds"), ContextPreviewResult.bSuccess);
	bPassed &= TestEqual(TEXT("context dry-run returns proposed description"), GetString(ContextPreviewResult, TEXT("description")), TEXT("Context proposal"));

	if (PrimaryAction)
	{
		bPassed &= TestEqual(TEXT("InputAction dry-runs preserve actual value type"), PrimaryAction->ValueType, OriginalValueType);
		bPassed &= TestEqual(TEXT("InputAction dry-runs preserve actual description"), PrimaryAction->ActionDescription.ToString(), OriginalActionDescription);
	}
	if (Context)
	{
		bPassed &= TestEqual(TEXT("context dry-run preserves actual description"), Context->ContextDescription.ToString(), OriginalContextDescription);
	}

	TSharedPtr<FJsonObject> AddPrimary = MakeMappingParams(
		Assets.Context,
		Assets.ActionA,
		TEXT("SpaceBar"));
	AddPrimary->SetArrayField(
		TEXT("modifier_classes"),
		MakeStringArray({ UInputModifierNegate::StaticClass()->GetPathName() }));
	AddPrimary->SetArrayField(
		TEXT("trigger_classes"),
		MakeStringArray({ UInputTriggerHold::StaticClass()->GetPathName() }));

	const FMonolithActionResult PrimaryResult =
		Execute(TEXT("add_input_mapping"), AddPrimary);
	bPassed &= TestTrue(TEXT("explicit modifier/trigger mapping succeeds"), PrimaryResult.bSuccess);
	bPassed &= TestTrue(TEXT("primary mapping is created"), GetBool(PrimaryResult, TEXT("created")));
	bPassed &= TestEqual(TEXT("primary modifier count"), GetInt(PrimaryResult, TEXT("modifier_count")), 1);
	bPassed &= TestEqual(TEXT("primary trigger count"), GetInt(PrimaryResult, TEXT("trigger_count")), 1);
	bPassed &= TestFalse(TEXT("save=false does not write primary mapping"), GetBool(PrimaryResult, TEXT("saved")));

	TSharedPtr<FJsonObject> DryRunClone = MakeMappingParams(
		Assets.Context,
		Assets.ActionB,
		TEXT("Enter"),
		true,
		false);
	DryRunClone->SetStringField(TEXT("source_context_path"), Assets.Context);
	DryRunClone->SetStringField(TEXT("source_action_path"), Assets.ActionA);
	DryRunClone->SetStringField(TEXT("source_key"), TEXT("SpaceBar"));
	DryRunClone->SetArrayField(
		TEXT("modifier_classes"),
		MakeStringArray({ UInputModifierNegate::StaticClass()->GetPathName() }));
	DryRunClone->SetArrayField(
		TEXT("trigger_classes"),
		MakeStringArray({ UInputTriggerHold::StaticClass()->GetPathName() }));
	const int32 ModifierObjectsBeforeDryRun =
		CountLiveObjectsOfClass(UInputModifierNegate::StaticClass());
	const int32 TriggerObjectsBeforeDryRun =
		CountLiveObjectsOfClass(UInputTriggerHold::StaticClass());
	const int32 MappingsBeforeDryRun = Context ? Context->GetMappings().Num() : -1;
	const FMonolithActionResult DryRunCloneResult =
		Execute(TEXT("add_input_mapping"), DryRunClone);
	bPassed &= TestTrue(TEXT("mapping clone dry-run succeeds"), DryRunCloneResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("mapping clone dry-run identifies proposed state"),
		GetString(DryRunCloneResult, TEXT("preview_state")),
		TEXT("proposed"));
	bPassed &= TestEqual(
		TEXT("mapping class loading is deferred until confirmation"),
		GetString(DryRunCloneResult, TEXT("class_resolution")),
		TEXT("deferred_until_confirm"));
	bPassed &= TestTrue(TEXT("mapping clone dry-run predicts creation"), GetBool(DryRunCloneResult, TEXT("would_create")));
	bPassed &= TestEqual(TEXT("mapping clone dry-run reports modifier count"), GetInt(DryRunCloneResult, TEXT("modifier_count")), 1);
	bPassed &= TestEqual(TEXT("mapping clone dry-run reports trigger count"), GetInt(DryRunCloneResult, TEXT("trigger_count")), 1);
	bPassed &= TestEqual(
		TEXT("mapping clone dry-run constructs no modifier UObjects"),
		CountLiveObjectsOfClass(UInputModifierNegate::StaticClass()),
		ModifierObjectsBeforeDryRun);
	bPassed &= TestEqual(
		TEXT("mapping clone dry-run constructs no trigger UObjects"),
		CountLiveObjectsOfClass(UInputTriggerHold::StaticClass()),
		TriggerObjectsBeforeDryRun);
	bPassed &= TestEqual(
		TEXT("mapping clone dry-run preserves mapping count"),
		Context ? Context->GetMappings().Num() : -1,
		MappingsBeforeDryRun);

	const FString UnloadedModifierPackage =
		FPackageName::GetLongPackagePath(Assets.Context) + TEXT("/BP_UnloadedModifier");
	const FString UnloadedModifierClass =
		MakeObjectPath(UnloadedModifierPackage) + TEXT("_C");
	bPassed &= TestNull(
		TEXT("unloaded modifier fixture package starts absent"),
		FindPackage(nullptr, *UnloadedModifierPackage));
	TSharedPtr<FJsonObject> UnloadedClassPreview = MakeMappingParams(
		Assets.Context,
		Assets.ActionB,
		TEXT("Tab"),
		true,
		false);
	UnloadedClassPreview->SetArrayField(
		TEXT("modifier_classes"),
		MakeStringArray({ UnloadedModifierClass }));
	const FMonolithActionResult UnloadedClassPreviewResult =
		Execute(TEXT("add_input_mapping"), UnloadedClassPreview);
	bPassed &= TestTrue(
		TEXT("unloaded valid class path is accepted for dry-run"),
		UnloadedClassPreviewResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("unloaded class dry-run defers resolution"),
		GetString(UnloadedClassPreviewResult, TEXT("class_resolution")),
		TEXT("deferred_until_confirm"));
	bPassed &= TestNull(
		TEXT("unloaded class dry-run does not load its package"),
		FindPackage(nullptr, *UnloadedModifierPackage));
	bPassed &= TestEqual(
		TEXT("unloaded class dry-run preserves mapping count"),
		Context ? Context->GetMappings().Num() : -1,
		MappingsBeforeDryRun);

	TSharedPtr<FJsonObject> AddClone = MakeMappingParams(
		Assets.Context,
		Assets.ActionB,
		TEXT("Enter"));
	AddClone->SetStringField(TEXT("source_context_path"), Assets.Context);
	AddClone->SetStringField(TEXT("source_action_path"), Assets.ActionA);
	AddClone->SetStringField(TEXT("source_key"), TEXT("SpaceBar"));
	const FMonolithActionResult CloneResult =
		Execute(TEXT("add_input_mapping"), AddClone);
	bPassed &= TestTrue(TEXT("source mapping clone succeeds"), CloneResult.bSuccess);
	bPassed &= TestTrue(TEXT("clone reports source provenance"), GetBool(CloneResult, TEXT("cloned_from_source")));
	bPassed &= TestEqual(TEXT("cloned modifier count"), GetInt(CloneResult, TEXT("modifier_count")), 1);
	bPassed &= TestEqual(TEXT("cloned trigger count"), GetInt(CloneResult, TEXT("trigger_count")), 1);

	if (TestNotNull(TEXT("created mapping context is loadable"), Context))
	{
		bPassed &= TestEqual(TEXT("context contains two mappings"), Context->GetMappings().Num(), 2);
		if (Context->GetMappings().Num() == 2)
		{
			bPassed &= TestEqual(TEXT("source modifier is instantiated"), Context->GetMappings()[0].Modifiers.Num(), 1);
			bPassed &= TestEqual(TEXT("source trigger is instantiated"), Context->GetMappings()[0].Triggers.Num(), 1);
			bPassed &= TestEqual(TEXT("clone modifier is instantiated"), Context->GetMappings()[1].Modifiers.Num(), 1);
			bPassed &= TestEqual(TEXT("clone trigger is instantiated"), Context->GetMappings()[1].Triggers.Num(), 1);
			if (Context->GetMappings()[1].Modifiers.Num() == 1)
			{
				bPassed &= TestTrue(
					TEXT("clone preserves modifier class"),
					Context->GetMappings()[1].Modifiers[0]->IsA(UInputModifierNegate::StaticClass()));
			}
			if (Context->GetMappings()[1].Triggers.Num() == 1)
			{
				bPassed &= TestTrue(
					TEXT("clone preserves trigger class"),
					Context->GetMappings()[1].Triggers[0]->IsA(UInputTriggerHold::StaticClass()));
			}
		}
	}

	TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetArrayField(
		TEXT("context_paths"),
		MakeStringArray({ Assets.Context }));
	const FMonolithActionResult ValidateResult =
		Execute(TEXT("validate_input_mappings"), ValidateParams);
	bPassed &= TestTrue(TEXT("mapping validation action succeeds"), ValidateResult.bSuccess);
	bPassed &= TestTrue(TEXT("different keys validate cleanly"), GetBool(ValidateResult, TEXT("valid")));
	bPassed &= TestEqual(TEXT("one context is validated"), GetInt(ValidateResult, TEXT("contexts_checked")), 1);

	FString WindowsStyleContextPath = Assets.Context;
	WindowsStyleContextPath.ReplaceInline(TEXT("/"), TEXT("\\"));
	TSharedPtr<FJsonObject> WindowsStyleValidateParams = MakeShared<FJsonObject>();
	WindowsStyleValidateParams->SetArrayField(
		TEXT("context_paths"),
		MakeStringArray({ WindowsStyleContextPath }));
	const FMonolithActionResult WindowsStyleValidateResult =
		Execute(TEXT("validate_input_mappings"), WindowsStyleValidateParams);
	bPassed &= TestTrue(
		TEXT("context_paths normalizes Windows-style separators"),
		WindowsStyleValidateResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("Windows-style context path selects one context"),
		GetInt(WindowsStyleValidateResult, TEXT("contexts_checked")),
		1);

	TSharedPtr<FJsonObject> EncodedValidateParams = MakeShared<FJsonObject>();
	EncodedValidateParams->SetStringField(
		TEXT("context_paths"),
		FString::Printf(TEXT("[\"%s\"]"), *Assets.Context));
	const FMonolithActionResult EncodedValidateResult =
		Execute(TEXT("validate_input_mappings"), EncodedValidateParams);
	bPassed &= TestTrue(
		TEXT("registry recovers a JSON-encoded context_paths array"),
		EncodedValidateResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("recovered context_paths selects one context"),
		GetInt(EncodedValidateResult, TEXT("contexts_checked")),
		1);

	TSharedPtr<FJsonObject> EncodedClassArrayPreview = MakeMappingParams(
		Assets.Context,
		Assets.ActionB,
		TEXT("Tab"),
		true,
		false);
	EncodedClassArrayPreview->SetStringField(TEXT("modifier_classes"), TEXT("[]"));
	EncodedClassArrayPreview->SetStringField(TEXT("trigger_classes"), TEXT("[]"));
	const FMonolithActionResult EncodedClassArrayPreviewResult =
		Execute(TEXT("add_input_mapping"), EncodedClassArrayPreview);
	bPassed &= TestTrue(
		TEXT("registry recovers JSON-encoded modifier and trigger arrays"),
		EncodedClassArrayPreviewResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("recovered empty modifier array remains empty"),
		GetInt(EncodedClassArrayPreviewResult, TEXT("modifier_count"), -1),
		0);
	bPassed &= TestEqual(
		TEXT("recovered empty trigger array remains empty"),
		GetInt(EncodedClassArrayPreviewResult, TEXT("trigger_count"), -1),
		0);

	const FMonolithActionResult RemovePreviewResult = Execute(
		TEXT("remove_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionB, TEXT("Enter"), true, false));
	bPassed &= TestTrue(TEXT("mapping removal dry-run succeeds"), RemovePreviewResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("mapping removal dry-run identifies proposed state"),
		GetString(RemovePreviewResult, TEXT("preview_state")),
		TEXT("proposed"));
	bPassed &= TestEqual(
		TEXT("mapping removal dry-run predicts one removal"),
		GetInt(RemovePreviewResult, TEXT("would_remove_count")),
		1);
	bPassed &= TestEqual(
		TEXT("mapping removal dry-run reports no committed removals"),
		GetInt(RemovePreviewResult, TEXT("removed_count")),
		0);
	bPassed &= TestEqual(
		TEXT("mapping removal dry-run preserves both mappings"),
		Context ? Context->GetMappings().Num() : -1,
		2);

	const FMonolithActionResult RemoveResult = Execute(
		TEXT("remove_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionB, TEXT("Enter")));
	bPassed &= TestTrue(TEXT("mapping removal succeeds"), RemoveResult.bSuccess);
	bPassed &= TestEqual(TEXT("one mapping is removed"), GetInt(RemoveResult, TEXT("removed_count")), 1);
	bPassed &= TestTrue(TEXT("mapping removal reports change"), GetBool(RemoveResult, TEXT("changed")));
	bPassed &= TestEqual(
		TEXT("context retains the primary mapping"),
		Context ? Context->GetMappings().Num() : -1,
		1);

	TSharedPtr<FJsonObject> DuplicateSource = MakeMappingParams(
		Assets.Context,
		Assets.ActionA,
		TEXT("SpaceBar"));
	DuplicateSource->SetBoolField(TEXT("allow_duplicate"), true);
	DuplicateSource->SetArrayField(
		TEXT("modifier_classes"),
		TArray<TSharedPtr<FJsonValue>>());
	DuplicateSource->SetArrayField(
		TEXT("trigger_classes"),
		TArray<TSharedPtr<FJsonValue>>());
	const FMonolithActionResult DuplicateSourceResult =
		Execute(TEXT("add_input_mapping"), DuplicateSource);
	bPassed &= TestTrue(
		TEXT("duplicate source fixture is created explicitly"),
		DuplicateSourceResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("source context contains two action-key matches"),
		Context ? Context->GetMappings().Num() : -1,
		2);

	TSharedPtr<FJsonObject> AmbiguousClone = MakeMappingParams(
		Assets.Context,
		Assets.ActionB,
		TEXT("Enter"),
		true,
		false);
	AmbiguousClone->SetStringField(TEXT("source_context_path"), Assets.Context);
	AmbiguousClone->SetStringField(TEXT("source_action_path"), Assets.ActionA);
	AmbiguousClone->SetStringField(TEXT("source_key"), TEXT("SpaceBar"));
	const FMonolithActionResult AmbiguousCloneResult =
		Execute(TEXT("add_input_mapping"), AmbiguousClone);
	bPassed &= TestFalse(
		TEXT("ambiguous source selector is rejected"),
		AmbiguousCloneResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("ambiguous source selector uses invalid-params"),
		AmbiguousCloneResult.ErrorCode,
		FMonolithJsonUtils::ErrInvalidParams);
	bPassed &= TestTrue(
		TEXT("ambiguous source selector requests an exact row"),
		AmbiguousCloneResult.ErrorMessage.Contains(TEXT("source_mapping_index")));

	AmbiguousClone->SetNumberField(TEXT("source_mapping_index"), 1);
	const FMonolithActionResult IndexedCloneResult =
		Execute(TEXT("add_input_mapping"), AmbiguousClone);
	bPassed &= TestTrue(
		TEXT("exact source mapping index disambiguates duplicate rows"),
		IndexedCloneResult.bSuccess);
	bPassed &= TestEqual(
		TEXT("clone reports the selected source row"),
		GetInt(IndexedCloneResult, TEXT("source_mapping_index"), -1),
		1);
	bPassed &= TestEqual(
		TEXT("indexed clone uses the selected row's empty modifiers"),
		GetInt(IndexedCloneResult, TEXT("modifier_count"), -1),
		0);
	bPassed &= TestEqual(
		TEXT("indexed clone uses the selected row's empty triggers"),
		GetInt(IndexedCloneResult, TEXT("trigger_count"), -1),
		0);

	bPassed &= TestFalse(TEXT("save=false creates no uasset files"), Assets.HasSavedFiles());
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetSaveFailureReportingTest,
	"Monolith.ParamGuard.GAS.InputAssets.SaveFailureReporting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetSaveFailureReportingTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

#if !PLATFORM_WINDOWS
	// FScopedSaveBlocker forces the failure by holding a write handle open on the
	// destination .uasset. That only blocks SavePackage on platforms with
	// mandatory file locking: POSIX locking is advisory and does not prevent the
	// temp-file rename, so the save would succeed and none of the structured
	// failure assertions below could be evaluated. The expected log messages are
	// Windows-specific for the same reason. The save-failure contract itself is
	// platform independent; only this way of provoking it is not.
	AddInfo(TEXT("Skipped: provoking a package save failure requires Windows mandatory file locking."));
	return true;
#else
	FScopedInputAssets Assets;
	if (!TestTrue(TEXT("save-failure input fixture is created"), Assets.Create()))
	{
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Root = FString::Printf(TEXT("/Game/Tests/Monolith/Input/SaveFailure/%s"), *Suffix);
	const FString CreateActionPath = Root + TEXT("/IA_CreateFail");
	const FString CreateContextPath = Root + TEXT("/IMC_CreateFail");
	FScopedSaveBlocker CreateActionBlocker(CreateActionPath);
	FScopedSaveBlocker CreateContextBlocker(CreateContextPath);
	FScopedSaveBlocker SetActionBlocker(Assets.ActionA);
	FScopedSaveBlocker MappingContextBlocker(Assets.Context);
	bool bPassed = true;
	bPassed &= TestTrue(TEXT("create InputAction save target is blocked"), CreateActionBlocker.IsReady());
	bPassed &= TestTrue(TEXT("create InputMappingContext save target is blocked"), CreateContextBlocker.IsReady());
	bPassed &= TestTrue(TEXT("set InputAction save target is blocked"), SetActionBlocker.IsReady());
	bPassed &= TestTrue(TEXT("mapping context save target is blocked"), MappingContextBlocker.IsReady());
	if (!bPassed)
	{
		CleanupTestAsset(CreateContextPath);
		CleanupTestAsset(CreateActionPath);
		return false;
	}
	AddExpectedError(
		TEXT("Error moving file"),
		EAutomationExpectedErrorFlags::Contains,
		3);
	AddExpectedError(
		TEXT("MoveFile was unable to move"),
		EAutomationExpectedErrorFlags::Contains,
		30);
	AddExpectedError(
		TEXT("Error saving"),
		EAutomationExpectedErrorFlags::Contains,
		3);
	AddExpectedError(
		TEXT("Failed to move"),
		EAutomationExpectedErrorFlags::Contains,
		3);
	AddExpectedError(
		TEXT("Error opening file"),
		EAutomationExpectedErrorFlags::Contains,
		4);
	AddExpectedError(
		TEXT("package was marked as deleted in editor"),
		EAutomationExpectedErrorFlags::Contains,
		4);

	auto VerifyCommittedSaveFailure =
		[this, &bPassed](const TCHAR* Label, const FMonolithActionResult& Result)
		{
			bPassed &= TestFalse(
				*FString::Printf(TEXT("%s returns an error"), Label),
				Result.bSuccess);
			bPassed &= TestNotNull(
				*FString::Printf(TEXT("%s returns structured error.data"), Label),
				GetErrorDataObject(Result).Get());
			bPassed &= TestTrue(
				*FString::Printf(TEXT("%s reports the committed mutation"), Label),
				GetErrorBool(Result, TEXT("mutation_committed")));
			bPassed &= TestTrue(
				*FString::Printf(TEXT("%s reports partial mutation"), Label),
				GetErrorBool(Result, TEXT("partial_mutation")));
			bPassed &= TestTrue(
				*FString::Printf(TEXT("%s reports save failure"), Label),
				GetErrorBool(Result, TEXT("save_failed")));
			bPassed &= TestFalse(
				*FString::Printf(TEXT("%s is not retry-safe"), Label),
				GetErrorBool(Result, TEXT("retry_safe"), true));
			bPassed &= TestTrue(
				*FString::Printf(TEXT("%s preserves the save error"), Label),
				GetErrorString(Result, TEXT("save_error")).Contains(TEXT("SavePackage failed")));
		};

	TSharedPtr<FJsonObject> CreateActionParams =
		MakeCreateParams(CreateActionPath, false, true);
	CreateActionParams->SetStringField(TEXT("description"), TEXT("Created before save failure"));
	CreateActionParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult CreateActionResult =
		Execute(TEXT("create_input_action"), CreateActionParams);
	VerifyCommittedSaveFailure(TEXT("create_input_action"), CreateActionResult);
	bPassed &= TestTrue(
		TEXT("create_input_action error.data reports creation"),
		GetErrorBool(CreateActionResult, TEXT("created")));
	bPassed &= TestNotNull(
		TEXT("create_input_action mutation remains in memory"),
		FindTestAsset(CreateActionPath));

	TSharedPtr<FJsonObject> CreateContextParams =
		MakeCreateParams(CreateContextPath, false, true);
	CreateContextParams->SetStringField(TEXT("description"), TEXT("Created before save failure"));
	CreateContextParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult CreateContextResult =
		Execute(TEXT("create_input_mapping_context"), CreateContextParams);
	VerifyCommittedSaveFailure(TEXT("create_input_mapping_context"), CreateContextResult);
	bPassed &= TestTrue(
		TEXT("create_input_mapping_context error.data reports creation"),
		GetErrorBool(CreateContextResult, TEXT("created")));
	bPassed &= TestNotNull(
		TEXT("create_input_mapping_context mutation remains in memory"),
		FindTestAsset(CreateContextPath));

	TSharedPtr<FJsonObject> SetActionParams =
		MakeCreateParams(Assets.ActionA, false, true);
	SetActionParams->SetStringField(TEXT("description"), TEXT("Set before save failure"));
	SetActionParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult SetActionResult =
		Execute(TEXT("set_input_action_properties"), SetActionParams);
	VerifyCommittedSaveFailure(TEXT("set_input_action_properties"), SetActionResult);
	bPassed &= TestTrue(
		TEXT("set_input_action_properties error.data reports change"),
		GetErrorBool(SetActionResult, TEXT("changed")));

	TSharedPtr<FJsonObject> AddMappingParams =
		MakeMappingParams(Assets.Context, Assets.ActionA, TEXT("SpaceBar"));
	AddMappingParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult AddMappingResult =
		Execute(TEXT("add_input_mapping"), AddMappingParams);
	VerifyCommittedSaveFailure(TEXT("add_input_mapping"), AddMappingResult);
	bPassed &= TestTrue(
		TEXT("add_input_mapping error.data reports creation"),
		GetErrorBool(AddMappingResult, TEXT("created")));
	bPassed &= TestEqual(
		TEXT("add_input_mapping mutation remains in memory"),
		Assets.GetContext() ? Assets.GetContext()->GetMappings().Num() : -1,
		1);

	TSharedPtr<FJsonObject> RemoveMappingParams =
		MakeMappingParams(Assets.Context, Assets.ActionA, TEXT("SpaceBar"));
	RemoveMappingParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult RemoveMappingResult =
		Execute(TEXT("remove_input_mapping"), RemoveMappingParams);
	VerifyCommittedSaveFailure(TEXT("remove_input_mapping"), RemoveMappingResult);
	bPassed &= TestEqual(
		TEXT("remove_input_mapping error.data reports one removal"),
		GetErrorInt(RemoveMappingResult, TEXT("removed_count")),
		1);
	bPassed &= TestEqual(
		TEXT("remove_input_mapping mutation remains in memory"),
		Assets.GetContext() ? Assets.GetContext()->GetMappings().Num() : -1,
		0);

	CleanupTestAsset(CreateContextPath);
	CleanupTestAsset(CreateActionPath);
	return bPassed;
#endif // !PLATFORM_WINDOWS
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetIdempotencyConflictNoOpTest,
	"Monolith.ParamGuard.GAS.InputAssets.IdempotencyConflictNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetIdempotencyConflictNoOpTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	FScopedInputAssets Assets;
	if (!TestTrue(TEXT("disposable input fixture is created"), Assets.Create(true)))
	{
		return false;
	}

	UInputMappingContext* Context = Assets.GetContext();
	if (!TestNotNull(TEXT("created mapping context is loadable"), Context))
	{
		return false;
	}

	const FMonolithActionResult InitialAdd = Execute(
		TEXT("add_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionA, TEXT("SpaceBar")));
	bool bPassed = TestTrue(TEXT("initial mapping succeeds"), InitialAdd.bSuccess);
	bPassed &= TestEqual(TEXT("initial mapping count"), Context->GetMappings().Num(), 1);

	Context->GetOutermost()->SetDirtyFlag(false);
	const FMonolithActionResult RepeatedAdd = Execute(
		TEXT("add_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionA, TEXT("SpaceBar")));
	bPassed &= TestTrue(TEXT("repeated mapping call succeeds"), RepeatedAdd.bSuccess);
	bPassed &= TestFalse(TEXT("repeated mapping is a no-op"), GetBool(RepeatedAdd, TEXT("changed"), true));
	bPassed &= TestFalse(TEXT("repeated mapping predicts no change"), GetBool(RepeatedAdd, TEXT("would_change"), true));
	bPassed &= TestEqual(TEXT("repeated mapping does not duplicate"), Context->GetMappings().Num(), 1);
	bPassed &= TestFalse(
		TEXT("repeated mapping does not dirty the package"),
		Context->GetOutermost()->IsDirty());

	const FMonolithActionResult ConflictingAdd = Execute(
		TEXT("add_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionB, TEXT("SpaceBar")));
	bPassed &= TestTrue(TEXT("second action on the same key is authored"), ConflictingAdd.bSuccess);
	bPassed &= TestEqual(TEXT("conflicting mapping count"), Context->GetMappings().Num(), 2);

	TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetArrayField(
		TEXT("context_paths"),
		MakeStringArray({ Assets.Context }));
	const FMonolithActionResult ValidateResult =
		Execute(TEXT("validate_input_mappings"), ValidateParams);
	bPassed &= TestTrue(TEXT("conflict validation action succeeds"), ValidateResult.bSuccess);
	bPassed &= TestFalse(TEXT("same key on different actions is invalid"), GetBool(ValidateResult, TEXT("valid"), true));
	bPassed &= TestEqual(TEXT("one duplicate-key conflict is reported"), GetInt(ValidateResult, TEXT("conflicts")), 1);

	Context->GetOutermost()->SetDirtyFlag(false);
	const FMonolithActionResult MissingRemove = Execute(
		TEXT("remove_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionA, TEXT("Enter")));
	bPassed &= TestTrue(TEXT("removing an absent mapping succeeds as a no-op"), MissingRemove.bSuccess);
	bPassed &= TestFalse(TEXT("absent mapping removal reports no change"), GetBool(MissingRemove, TEXT("changed"), true));
	bPassed &= TestEqual(TEXT("absent mapping removal removes nothing"), GetInt(MissingRemove, TEXT("removed_count")), 0);
	bPassed &= TestEqual(TEXT("absent mapping removal predicts nothing"), GetInt(MissingRemove, TEXT("would_remove_count")), 0);
	bPassed &= TestEqual(TEXT("absent mapping removal preserves mappings"), Context->GetMappings().Num(), 2);
	bPassed &= TestFalse(
		TEXT("absent mapping removal does not dirty the package"),
		Context->GetOutermost()->IsDirty());
	bPassed &= TestFalse(TEXT("save=false creates no uasset files"), Assets.HasSavedFiles());
	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
