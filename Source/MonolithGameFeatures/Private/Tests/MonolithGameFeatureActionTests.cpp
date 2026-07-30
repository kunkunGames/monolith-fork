#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "MonolithGameFeatureActions.h"
#include "MonolithGameFeatureActionTestHooks.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFeatureData.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#ifndef WITH_MONOLITH_GAMEFEATURES
#define WITH_MONOLITH_GAMEFEATURES 0
#endif

#if WITH_MONOLITH_GAMEFEATURES

namespace
{
	struct FGameFeatureTestAsset
	{
		UPackage* Package = nullptr;
		UGameFeatureData* Data = nullptr;

		void Release() const
		{
			if (Package)
			{
				Package->RemoveFromRoot();
			}
		}
	};

	static FGameFeatureTestAsset CreateGameFeatureTestAsset(const TCHAR* Prefix)
	{
		const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString AssetName = FString::Printf(TEXT("%s_%s"), Prefix, *UniqueSuffix);
		const FString PackageName = FString::Printf(TEXT("/Game/MonolithAutomation/%s"), *AssetName);

		FGameFeatureTestAsset Fixture;
		Fixture.Package = CreatePackage(*PackageName);
		if (Fixture.Package)
		{
			Fixture.Package->AddToRoot();
			Fixture.Data = NewObject<UGameFeatureData>(
				Fixture.Package,
				*AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			Fixture.Package->SetDirtyFlag(false);
		}
		return Fixture;
	}

	static bool TryGetActions(
		UGameFeatureData* Data,
		FArrayProperty*& OutArrayProperty,
		FObjectPropertyBase*& OutObjectProperty)
	{
		OutArrayProperty = Data ? FindFProperty<FArrayProperty>(Data->GetClass(), TEXT("Actions")) : nullptr;
		OutObjectProperty = OutArrayProperty ? CastField<FObjectPropertyBase>(OutArrayProperty->Inner) : nullptr;
		return OutArrayProperty && OutObjectProperty;
	}

	static int32 GetActionCount(UGameFeatureData* Data)
	{
		FArrayProperty* ArrayProperty = nullptr;
		FObjectPropertyBase* ObjectProperty = nullptr;
		if (!TryGetActions(Data, ArrayProperty, ObjectProperty))
		{
			return INDEX_NONE;
		}
		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(Data);
		return FScriptArrayHelper(ArrayProperty, ArrayPtr).Num();
	}

	static UObject* GetActionAt(UGameFeatureData* Data, int32 Index)
	{
		FArrayProperty* ArrayProperty = nullptr;
		FObjectPropertyBase* ObjectProperty = nullptr;
		if (!TryGetActions(Data, ArrayProperty, ObjectProperty))
		{
			return nullptr;
		}
		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(Data);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		return Helper.IsValidIndex(Index)
			? ObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index))
			: nullptr;
	}

	static UObject* AddNamedAction(UGameFeatureData* Data, UClass* ActionClass, const FName ActionName)
	{
		FArrayProperty* ArrayProperty = nullptr;
		FObjectPropertyBase* ObjectProperty = nullptr;
		if (!Data || !ActionClass || !TryGetActions(Data, ArrayProperty, ObjectProperty))
		{
			return nullptr;
		}

		UObject* ActionObject = NewObject<UObject>(Data, ActionClass, ActionName, RF_Transactional);
		if (!ActionObject)
		{
			return nullptr;
		}
		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(Data);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		const int32 NewIndex = Helper.AddValue();
		ObjectProperty->SetObjectPropertyValue(Helper.GetRawPtr(NewIndex), ActionObject);
		return ActionObject;
	}

	static TSharedPtr<FJsonObject> MakeAddComponentsParams(
		const UGameFeatureData* Data,
		const FString& ActionName,
		const FString& ComponentClass = TEXT("/Script/Engine.ActorComponent"))
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("game_feature_data_path"), Data ? Data->GetPathName() : FString());
		Params->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
		Params->SetStringField(TEXT("component_class"), ComponentClass);
		Params->SetStringField(TEXT("action_class_path"), TEXT("/Script/GameFeatures.GameFeatureAction_AddComponents"));
		Params->SetStringField(TEXT("action_name"), ActionName);
		Params->SetBoolField(TEXT("save"), false);
		Params->SetBoolField(TEXT("dry_run"), false);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGameFeaturesStatusTest,
	"Monolith.GameFeatures.StatusAndReadOnlyGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameFeaturesStatusTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	const bool bOriginalEnabled = Settings->bEnableGameFeatureActions;
	const bool bOriginalCreation = Settings->bAllowGameFeaturePluginCreation;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.UnregisterNamespace(TEXT("gamefeatures"));
	FMonolithGameFeatureActions::Register(Registry, false);
	TestEqual(TEXT("Disabled inspection registers status plus eight writers"),
		Registry.GetActions(TEXT("gamefeatures")).Num(), 9);

	Registry.UnregisterNamespace(TEXT("gamefeatures"));
	FMonolithGameFeatureActions::Register(Registry, true);
	TestEqual(TEXT("Enabled inspection registers the complete action surface"),
		Registry.GetActions(TEXT("gamefeatures")).Num(), 15);

	Settings->bEnableGameFeatureActions = false;
	Settings->bAllowGameFeaturePluginCreation = false;

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::GetStatus(MakeShared<FJsonObject>());
		TestTrue(TEXT("GetStatus succeeds while inspection disabled"), Result.bSuccess);
		TestTrue(TEXT("GetStatus returns json while inspection disabled"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* AvailableWhenEnabled = nullptr;
			TestFalse(TEXT("Inspection disabled"), Result.Result->GetBoolField(TEXT("inspection_enabled")));
			TestTrue(TEXT("Actions field exists"), Result.Result->TryGetArrayField(TEXT("actions"), Actions));
			TestTrue(TEXT("AvailableWhenEnabled field exists"), Result.Result->TryGetArrayField(TEXT("available_when_enabled"), AvailableWhenEnabled));
			if (Actions)
			{
				TestEqual(TEXT("Default actions include status and eight instanced-action writers"), Actions->Num(), 9);
			}
			if (AvailableWhenEnabled)
			{
				TestEqual(TEXT("Six gated inspection actions reported"), AvailableWhenEnabled->Num(), 6);
			}
		}
	}

	Settings->bEnableGameFeatureActions = true;
	Settings->bAllowGameFeaturePluginCreation = false;

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::GetStatus(MakeShared<FJsonObject>());
		TestTrue(TEXT("GetStatus succeeds"), Result.bSuccess);
		TestTrue(TEXT("GetStatus returns json"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			TestEqual(TEXT("Namespace"), Result.Result->GetStringField(TEXT("namespace")), FString(TEXT("gamefeatures")));
			TestTrue(TEXT("Inspection plus write mode"), Result.Result->GetStringField(TEXT("mode")) == TEXT("inspection_and_instanced_action_writes"));
			TestTrue(TEXT("Write action registered"), Result.Result->GetBoolField(TEXT("write_actions_registered")));
			TestFalse(TEXT("No hard ToolsetRegistry dependency"), Result.Result->GetBoolField(TEXT("hard_toolsetregistry_dependency")));
			TestFalse(TEXT("Creation disabled by default"), Result.Result->GetBoolField(TEXT("creation_allowed")));
		}
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddActionSetInputMapping(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddActionSetInputMapping rejects missing action_set_path"), Result.bSuccess);
		TestEqual(TEXT("AddActionSetInputMapping missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::SetPrimaryAssetScan(MakeShared<FJsonObject>());
		TestFalse(TEXT("SetPrimaryAssetScan rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("SetPrimaryAssetScan missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataInputMapping(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataInputMapping rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataInputMapping missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataWidgets(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataWidgets rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataWidgets missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataComponents(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataComponents rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataComponents missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataGameplayCuePaths(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataGameplayCuePaths rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataGameplayCuePaths missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataAbilities(MakeShared<FJsonObject>());
		TestFalse(TEXT("AddGameFeatureDataAbilities rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("AddGameFeatureDataAbilities missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::RemoveGameFeatureDataAction(MakeShared<FJsonObject>());
		TestFalse(TEXT("RemoveGameFeatureDataAction rejects missing game_feature_data_path"), Result.bSuccess);
		TestEqual(TEXT("RemoveGameFeatureDataAction missing param code"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1);
		Params->SetBoolField(TEXT("include_engine"), false);
		FMonolithActionResult Result = FMonolithGameFeatureActions::ListPlugins(Params);
		TestTrue(TEXT("ListPlugins handles empty projects"), Result.bSuccess);
		TestTrue(TEXT("ListPlugins returns json"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			TestTrue(TEXT("Count field exists"), Result.Result->HasField(TEXT("count")));
			TestTrue(TEXT("Plugins field exists"), Result.Result->HasField(TEXT("plugins")));
		}
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/DefinitelyNotAGameFeatureData"));
		FMonolithActionResult Result = FMonolithGameFeatureActions::FindGameFeatureData(Params);
		TestTrue(TEXT("FindGameFeatureData reports not found as data"), Result.bSuccess);
		TestTrue(TEXT("FindGameFeatureData returns json"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			TestFalse(TEXT("Missing asset not found"), Result.Result->GetBoolField(TEXT("found")));
		}
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 5);
		FMonolithActionResult Result = FMonolithGameFeatureActions::ListActionClasses(Params);
		TestTrue(TEXT("ListActionClasses succeeds"), Result.bSuccess);
		TestTrue(TEXT("ListActionClasses returns json"), Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			TestTrue(TEXT("Classes field exists"), Result.Result->HasField(TEXT("classes")));
		}
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::DescribeActionSet(MakeShared<FJsonObject>());
		TestFalse(TEXT("DescribeActionSet rejects missing action_set_path"), Result.bSuccess);
		TestEqual(TEXT("DescribeActionSet missing param code"), Result.ErrorCode, -32602);
	}

	{
		FMonolithActionResult Result = FMonolithGameFeatureActions::ValidatePlugin(MakeShared<FJsonObject>());
		TestFalse(TEXT("ValidatePlugin rejects missing plugin_name"), Result.bSuccess);
		TestEqual(TEXT("ValidatePlugin missing param code"), Result.ErrorCode, -32602);
	}

	Settings->bEnableGameFeatureActions = bOriginalEnabled;
	Settings->bAllowGameFeaturePluginCreation = bOriginalCreation;
	Registry.UnregisterNamespace(TEXT("gamefeatures"));
	FMonolithGameFeatureActions::Register(Registry, bOriginalEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGameFeaturesWriterSafetyTest,
	"Monolith.GameFeatures.WriterPreflightAndTypeSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameFeaturesWriterSafetyTest::RunTest(const FString& Parameters)
{
	{
		const FGameFeatureTestAsset Fixture = CreateGameFeatureTestAsset(TEXT("GF_InvalidPreflight"));
		TestNotNull(TEXT("Invalid-preflight GameFeatureData created"), Fixture.Data);
		if (Fixture.Data)
		{
			FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataComponents(
				MakeAddComponentsParams(Fixture.Data, TEXT("InvalidPreflight"), TEXT("/Script/Engine.Actor")));
			TestFalse(TEXT("Invalid component class is rejected"), Result.bSuccess);
			TestEqual(TEXT("Invalid component class is a parameter error"), Result.ErrorCode, -32602);
			TestEqual(TEXT("Rejected edit attaches no partial action"), GetActionCount(Fixture.Data), 0);
			TestFalse(TEXT("Rejected edit leaves package clean"), Fixture.Package->IsDirty());
		}
		Fixture.Release();
	}

	{
		const FGameFeatureTestAsset Fixture = CreateGameFeatureTestAsset(TEXT("GF_NoOp"));
		TestNotNull(TEXT("No-op GameFeatureData created"), Fixture.Data);
		if (Fixture.Data)
		{
			const TSharedPtr<FJsonObject> Params = MakeAddComponentsParams(Fixture.Data, TEXT("StableComponents"));
			FMonolithActionResult FirstResult = FMonolithGameFeatureActions::AddGameFeatureDataComponents(Params);
			TestTrue(TEXT("First component edit succeeds"), FirstResult.bSuccess);
			TestEqual(TEXT("First component edit creates one action"), GetActionCount(Fixture.Data), 1);
			if (UObject* CreatedAction = GetActionAt(Fixture.Data, 0))
			{
				TestEqual(TEXT("Created action preserves requested object name"), CreatedAction->GetName(), FString(TEXT("StableComponents")));
			}

			Fixture.Package->SetDirtyFlag(false);
			FMonolithActionResult SecondResult = FMonolithGameFeatureActions::AddGameFeatureDataComponents(Params);
			TestTrue(TEXT("Idempotent component edit succeeds"), SecondResult.bSuccess);
			if (SecondResult.Result.IsValid())
			{
				TestFalse(TEXT("Idempotent component edit reports unchanged"), SecondResult.Result->GetBoolField(TEXT("changed")));
				TestEqual(TEXT("Idempotent component edit removes no nulls"), SecondResult.Result->GetIntegerField(TEXT("removed_null_actions")), 0);
			}
			TestEqual(TEXT("Idempotent component edit preserves action count"), GetActionCount(Fixture.Data), 1);
			TestFalse(TEXT("Idempotent component edit does not dirty package"), Fixture.Package->IsDirty());

			Params->SetNumberField(TEXT("addition_flags"), 1);
			FMonolithActionResult UpdateResult = FMonolithGameFeatureActions::AddGameFeatureDataComponents(Params);
			TestTrue(TEXT("Existing component action update succeeds"), UpdateResult.bSuccess);
			if (UpdateResult.Result.IsValid())
			{
				TestTrue(TEXT("Existing component action reports updated entry"), UpdateResult.Result->GetBoolField(TEXT("updated_component")));
			}
			TestEqual(TEXT("Existing component action update preserves action count"), GetActionCount(Fixture.Data), 1);
			if (UObject* UpdatedAction = GetActionAt(Fixture.Data, 0))
			{
				TestEqual(TEXT("Existing component action update preserves object name"), UpdatedAction->GetName(), FString(TEXT("StableComponents")));
			}
			TestTrue(TEXT("Existing component action update dirties package"), Fixture.Package->IsDirty());

			Fixture.Package->SetDirtyFlag(false);
			FMonolithActionResult StableUpdateResult = FMonolithGameFeatureActions::AddGameFeatureDataComponents(Params);
			TestTrue(TEXT("Repeated updated component edit succeeds"), StableUpdateResult.bSuccess);
			if (StableUpdateResult.Result.IsValid())
			{
				TestFalse(TEXT("Repeated updated component edit reports unchanged"), StableUpdateResult.Result->GetBoolField(TEXT("changed")));
			}
			TestFalse(TEXT("Repeated updated component edit leaves package clean"), Fixture.Package->IsDirty());
		}
		Fixture.Release();
	}

	{
		const FGameFeatureTestAsset Fixture = CreateGameFeatureTestAsset(TEXT("GF_NameConflict"));
		TestNotNull(TEXT("Name-conflict GameFeatureData created"), Fixture.Data);
		if (Fixture.Data)
		{
			UClass* OtherActionClass = StaticLoadClass(
				UObject::StaticClass(),
				nullptr,
				TEXT("/Script/GameFeatures.GameFeatureAction_AddCheats"));
			UObject* ExistingAction = AddNamedAction(Fixture.Data, OtherActionClass, TEXT("SharedAction"));
			TestNotNull(TEXT("Conflicting named action created"), ExistingAction);
			Fixture.Package->SetDirtyFlag(false);

			FMonolithActionResult Result = FMonolithGameFeatureActions::AddGameFeatureDataComponents(
				MakeAddComponentsParams(Fixture.Data, TEXT("SharedAction")));
			TestFalse(TEXT("Named action with a different class is rejected"), Result.bSuccess);
			TestTrue(TEXT("Name conflict explains requested class mismatch"), Result.ErrorMessage.Contains(TEXT("not requested class")));
			TestEqual(TEXT("Name conflict preserves action count"), GetActionCount(Fixture.Data), 1);
			TestTrue(TEXT("Name conflict preserves selected object"), GetActionAt(Fixture.Data, 0) == ExistingAction);
			TestFalse(TEXT("Name conflict leaves package clean"), Fixture.Package->IsDirty());
		}
		Fixture.Release();
	}

	{
		UClass* AddComponentsClass = StaticLoadClass(
			UObject::StaticClass(),
			nullptr,
			TEXT("/Script/GameFeatures.GameFeatureAction_AddComponents"));
		UObject* TransientAction = AddComponentsClass
			? NewObject<UObject>(GetTransientPackage(), AddComponentsClass)
			: nullptr;
		TestNotNull(TEXT("Transient AddComponents action created"), TransientAction);
		if (TransientAction)
		{
			FString Error;
			TestFalse(
				TEXT("Soft-class property rejects a class outside MetaClass"),
				MonolithGameFeatures::TestHooks::TrySetSoftClassArrayEntry(
					TransientAction,
					TEXT("ComponentList"),
					TEXT("ActorClass"),
					TEXT("/Script/Engine.ActorComponent"),
					Error));
			TestTrue(TEXT("Soft-class MetaClass error is explicit"), Error.Contains(TEXT("not a child")));

			Error.Reset();
			TestTrue(
				TEXT("Soft-class property accepts a compatible class"),
				MonolithGameFeatures::TestHooks::TrySetSoftClassArrayEntry(
					TransientAction,
					TEXT("ComponentList"),
					TEXT("ActorClass"),
					TEXT("/Script/Engine.Actor"),
					Error));
		}
	}

	TestTrue(
		TEXT("A single GameFeatureData candidate resolves uniquely"),
		MonolithGameFeatures::TestHooks::HasUniqueGameFeatureDataCandidate(1));
	TestFalse(
		TEXT("Multiple GameFeatureData candidates require an explicit asset path"),
		MonolithGameFeatures::TestHooks::HasUniqueGameFeatureDataCandidate(2));

	return true;
}

#else

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGameFeaturesStatusTest,
	"Monolith.GameFeatures.StatusUnavailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameFeaturesStatusTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Unavailable dependency registers status only"),
		FMonolithToolRegistry::Get().GetActions(TEXT("gamefeatures")).Num(), 1);

	FMonolithActionResult Result = FMonolithGameFeatureActions::GetStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("GetStatus succeeds when optional dependency is not compiled"), Result.bSuccess);
	TestTrue(TEXT("GetStatus returns json"), Result.Result.IsValid());
	if (Result.Result.IsValid())
	{
		TestFalse(TEXT("GameFeatures not compiled"), Result.Result->GetBoolField(TEXT("with_gamefeatures")));
		TestEqual(TEXT("Dependency state"), Result.Result->GetStringField(TEXT("dependency_state")), FString(TEXT("unavailable")));
	}
	return true;
}

#endif
