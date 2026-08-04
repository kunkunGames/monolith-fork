// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithGASInputAssetActionsTests
{
	const TArray<FString> RequiredActions = {
		TEXT("list_input_actions"),
		TEXT("get_input_action"),
		TEXT("list_input_mapping_contexts"),
		TEXT("get_input_mapping_context"),
		TEXT("validate_input_mappings")
	};

	FString MakeObjectPath(const FString& PackagePath)
	{
		return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
	}

	FMonolithActionResult Execute(
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("input"), Action, Params);
	}

	int32 GetInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 Default = 0)
	{
		double Value = static_cast<double>(Default);
		if (Object.IsValid())
		{
			Object->TryGetNumberField(Field, Value);
		}
		return static_cast<int32>(Value);
	}

	bool GetBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool Default = false)
	{
		bool Value = Default;
		if (Object.IsValid())
		{
			Object->TryGetBoolField(Field, Value);
		}
		return Value;
	}

	FString GetString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Default = FString())
	{
		FString Value = Default;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	void CleanupAsset(const FString& PackagePath)
	{
		UObject* Asset = StaticFindObject(
			UObject::StaticClass(),
			nullptr,
			*MakeObjectPath(PackagePath));
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		if (Asset)
		{
			if (UInputMappingContext* Context = Cast<UInputMappingContext>(Asset))
			{
				Context->UnmapAll();
			}
			FAssetRegistryModule::AssetDeleted(Asset);
			const FName TransientName = MakeUniqueObjectName(
				GetTransientPackage(),
				Asset->GetClass(),
				*FString::Printf(TEXT("MONOLITH_INPUT_TEST_%s"), *Asset->GetName()));
			Asset->Rename(
				*TransientName.ToString(),
				GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}

	template <typename TAsset>
	TAsset* CreateAsset(const FString& PackagePath)
	{
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			return nullptr;
		}
		TAsset* Asset = NewObject<TAsset>(
			Package,
			*FPackageName::GetLongPackageAssetName(PackagePath),
			RF_Public | RF_Standalone | RF_Transactional);
		if (Asset)
		{
			FAssetRegistryModule::AssetCreated(Asset);
		}
		return Asset;
	}

	struct FScopedInputFixture
	{
		FString Root;
		FString ActionAPath;
		FString ActionBPath;
		FString ContextPath;
		UInputAction* ActionA = nullptr;
		UInputAction* ActionB = nullptr;
		UInputMappingContext* Context = nullptr;

		FScopedInputFixture()
		{
			Root = FString::Printf(
				TEXT("/Game/Tests/Monolith/InputInspection/%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
			ActionAPath = Root + TEXT("/IA_Primary");
			ActionBPath = Root + TEXT("/IA_Secondary");
			ContextPath = Root + TEXT("/IMC_Test");
		}

		~FScopedInputFixture()
		{
			CleanupAsset(ContextPath);
			CleanupAsset(ActionBPath);
			CleanupAsset(ActionAPath);
		}

		bool Create()
		{
			ActionA = CreateAsset<UInputAction>(ActionAPath);
			ActionB = CreateAsset<UInputAction>(ActionBPath);
			Context = CreateAsset<UInputMappingContext>(ContextPath);
			if (!ActionA || !ActionB || !Context)
			{
				return false;
			}

			ActionA->ValueType = EInputActionValueType::Axis2D;
			ActionA->ActionDescription = FText::FromString(TEXT("Primary test action"));
			ActionA->Triggers.Add(NewObject<UInputTriggerPressed>(ActionA));
			ActionA->Modifiers.Add(NewObject<UInputModifierNegate>(ActionA));
			Context->ContextDescription = FText::FromString(TEXT("Read-only inspection fixture"));
			Context->MapKey(ActionA, EKeys::SpaceBar);
			Context->MapKey(ActionB, EKeys::SpaceBar);

			ActionA->GetOutermost()->SetDirtyFlag(false);
			ActionB->GetOutermost()->SetDirtyFlag(false);
			Context->GetOutermost()->SetDirtyFlag(false);
			return true;
		}

		bool PackagesAreClean() const
		{
			return ActionA
				&& ActionB
				&& Context
				&& !ActionA->GetOutermost()->IsDirty()
				&& !ActionB->GetOutermost()->IsDirty()
				&& !Context->GetOutermost()->IsDirty();
		}
	};

	TSharedPtr<FJsonObject> MakeAssetParams(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Params;
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<FString> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetRegistrationTest,
	"Monolith.Input.Assets.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bPassed = true;
	for (const FString& Action : MonolithGASInputAssetActionsTests::RequiredActions)
	{
		bPassed &= TestTrue(
			*FString::Printf(TEXT("input.%s is registered"), *Action),
			Registry.HasAction(TEXT("input"), Action));
	}
	bPassed &= TestEqual(
		TEXT("The read-only input namespace owns exactly five actions"),
		Registry.GetActions(TEXT("input")).Num(),
		5);
	bPassed &= TestFalse(
		TEXT("Mutation actions are not exposed by this surface"),
		Registry.HasAction(TEXT("input"), TEXT("create_input_action")));

	const FMonolithDispatcherAnnotations Annotations =
		Registry.GetDispatcherAnnotations(TEXT("input"));
	bPassed &= TestTrue(TEXT("input dispatcher is read-only"), Annotations.bReadOnlyHint);
	bPassed &= TestTrue(TEXT("input dispatcher is idempotent"), Annotations.bIdempotentHint);

	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("input")))
	{
		if (Info.Action == TEXT("get_input_mapping_context"))
		{
			bPassed &= TestTrue(
				TEXT("Mapping read schema publishes pagination"),
				Info.ParamSchema.IsValid()
					&& Info.ParamSchema->HasField(TEXT("mapping_offset"))
					&& Info.ParamSchema->HasField(TEXT("mapping_limit")));
		}
		if (Info.Action == TEXT("validate_input_mappings"))
		{
			bPassed &= TestTrue(
				TEXT("Validation schema publishes scan bounds"),
				Info.ParamSchema.IsValid()
					&& Info.ParamSchema->HasField(TEXT("mapping_scan_limit"))
					&& Info.ParamSchema->HasField(TEXT("limit")));
		}
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetParamGuardTest,
	"Monolith.Input.Assets.ParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetParamGuardTest::RunTest(const FString& /*Parameters*/)
{
	bool bPassed = true;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 0);
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("list_input_actions"), Params);
		bPassed &= TestFalse(TEXT("Zero list limit is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Zero list limit is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_path"), 17);
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("get_input_action"), Params);
		bPassed &= TestFalse(TEXT("Non-string asset path is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Non-string asset path is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Input/IMC_Test.IMC_Other"));
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("get_input_mapping_context"), Params);
		bPassed &= TestFalse(TEXT("Mismatched object leaf is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Mismatched object leaf is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Input/IMC_Test"));
		Params->SetNumberField(TEXT("mapping_limit"), 501);
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("get_input_mapping_context"), Params);
		bPassed &= TestFalse(TEXT("Mapping page over hard cap is rejected"), Result.bSuccess);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(
			TEXT("context_paths"),
			MonolithGASInputAssetActionsTests::MakeStringArray({ TEXT("/Game/Input/IMC_Test") }));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Input"));
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestFalse(TEXT("Mutually exclusive validation selectors are rejected"), Result.bSuccess);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("context_paths"), {});
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestFalse(TEXT("Explicit empty context list is rejected"), Result.bSuccess);
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetReadbackTest,
	"Monolith.Input.Assets.ReadbackAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetReadbackTest::RunTest(const FString& /*Parameters*/)
{
	MonolithGASInputAssetActionsTests::FScopedInputFixture Fixture;
	if (!TestTrue(TEXT("Fixture assets were created"), Fixture.Create()))
	{
		return false;
	}

	bool bPassed = true;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), Fixture.Root);
		Params->SetNumberField(TEXT("limit"), 1);
		Params->SetBoolField(TEXT("include_details"), true);
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("list_input_actions"), Params);
		bPassed &= TestTrue(TEXT("Input action list succeeds"), Result.bSuccess);
		bPassed &= TestEqual(
			TEXT("Input action list total"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("total")),
			2);
		bPassed &= TestEqual(
			TEXT("Input action page count"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("count")),
			1);
		bPassed &= TestTrue(
			TEXT("Input action page reports more"),
			MonolithGASInputAssetActionsTests::GetBool(Result.Result, TEXT("has_more")));
	}
	{
		const FMonolithActionResult Result = MonolithGASInputAssetActionsTests::Execute(
			TEXT("get_input_action"),
			MonolithGASInputAssetActionsTests::MakeAssetParams(Fixture.ActionAPath));
		bPassed &= TestTrue(TEXT("Input action read succeeds"), Result.bSuccess);
		bPassed &= TestEqual(
			TEXT("Input action value type is preserved"),
			MonolithGASInputAssetActionsTests::GetString(Result.Result, TEXT("value_type")),
			FString(TEXT("Axis2D")));
		bPassed &= TestEqual(
			TEXT("Input action trigger count"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("trigger_count")),
			1);
		bPassed &= TestEqual(
			TEXT("Input action modifier count"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("modifier_count")),
			1);
	}
	{
		TSharedPtr<FJsonObject> Params =
			MonolithGASInputAssetActionsTests::MakeAssetParams(Fixture.ContextPath);
		Params->SetNumberField(TEXT("mapping_limit"), 1);
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("get_input_mapping_context"), Params);
		bPassed &= TestTrue(TEXT("Mapping context read succeeds"), Result.bSuccess);
		bPassed &= TestEqual(
			TEXT("Mapping context total mappings"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("mapping_count")),
			2);
		bPassed &= TestEqual(
			TEXT("Mapping context returned mappings"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("mappings_returned")),
			1);
		bPassed &= TestTrue(
			TEXT("Mapping context reports truncation"),
			MonolithGASInputAssetActionsTests::GetBool(
				Result.Result,
				TEXT("mappings_truncated")));
		bPassed &= TestTrue(
			TEXT("Mapping context reports more"),
			MonolithGASInputAssetActionsTests::GetBool(
				Result.Result,
				TEXT("has_more_mappings")));
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(
			TEXT("context_paths"),
			MonolithGASInputAssetActionsTests::MakeStringArray({ Fixture.ContextPath }));
		Params->SetNumberField(TEXT("mapping_scan_limit"), 100);
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestTrue(TEXT("Mapping validation succeeds"), Result.bSuccess);
		bPassed &= TestTrue(
			TEXT("Mapping validation is complete"),
			MonolithGASInputAssetActionsTests::GetBool(Result.Result, TEXT("complete")));
		bPassed &= TestTrue(
			TEXT("Duplicate-key warning does not invalidate"),
			MonolithGASInputAssetActionsTests::GetBool(Result.Result, TEXT("valid")));
		bPassed &= TestEqual(
			TEXT("Mapping validation error count"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("errors")),
			0);
		bPassed &= TestEqual(
			TEXT("Mapping validation warning count"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("warnings")),
			1);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(
			TEXT("context_paths"),
			MonolithGASInputAssetActionsTests::MakeStringArray({
				Fixture.ContextPath,
				Fixture.Root + TEXT("/ZZ_IMC_Missing")
			}));
		Params->SetNumberField(TEXT("limit"), 1);
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestTrue(TEXT("Paginated validation returns structured output"), Result.bSuccess);
		bPassed &= TestTrue(
			TEXT("Returned validation page is complete"),
			MonolithGASInputAssetActionsTests::GetBool(Result.Result, TEXT("page_complete")));
		bPassed &= TestFalse(
			TEXT("One page does not cover all contexts"),
			MonolithGASInputAssetActionsTests::GetBool(
				Result.Result,
				TEXT("all_contexts_covered"),
				true));
		bPassed &= TestFalse(
			TEXT("Partial context coverage cannot claim complete"),
			MonolithGASInputAssetActionsTests::GetBool(
				Result.Result,
				TEXT("complete"),
				true));
		bPassed &= TestFalse(
			TEXT("Partial context coverage cannot claim valid"),
			MonolithGASInputAssetActionsTests::GetBool(
				Result.Result,
				TEXT("valid"),
				true));
		bPassed &= TestTrue(
			TEXT("Paginated validation reports another page"),
			MonolithGASInputAssetActionsTests::GetBool(Result.Result, TEXT("has_more")));
		bPassed &= TestEqual(
			TEXT("A complete clean page adds no errors"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("errors")),
			0);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(
			TEXT("context_paths"),
			MonolithGASInputAssetActionsTests::MakeStringArray({ Fixture.ContextPath }));
		Params->SetNumberField(TEXT("mapping_scan_limit"), 1);
		const FMonolithActionResult Result =
			MonolithGASInputAssetActionsTests::Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestTrue(TEXT("Bounded mapping validation returns structured output"), Result.bSuccess);
		bPassed &= TestFalse(
			TEXT("Bounded mapping validation reports incomplete"),
			MonolithGASInputAssetActionsTests::GetBool(
				Result.Result,
				TEXT("complete"),
				true));
		bPassed &= TestFalse(
			TEXT("Incomplete mapping validation cannot claim valid"),
			MonolithGASInputAssetActionsTests::GetBool(
				Result.Result,
				TEXT("valid"),
				true));
		bPassed &= TestEqual(
			TEXT("Scan cutoff is an explicit error"),
			MonolithGASInputAssetActionsTests::GetInt(Result.Result, TEXT("errors")),
			1);
	}

	bPassed &= TestTrue(TEXT("Read-only calls leave fixture packages clean"), Fixture.PackagesAreClean());
	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
