#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Curves/CurveFloat.h"
#include "Misc/AutomationTest.h"
#include "MonolithLyraActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLyraRegistryContractTest,
	"Monolith.Lyra.RegistryContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLyraRegistryContractTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("lyra"), TEXT("get_status")))
	{
		FMonolithLyraActions::RegisterActions(Registry);
	}

	TestTrue(TEXT("lyra.get_status action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("get_status")));
	TestTrue(TEXT("lyra.describe_experience_graph action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("describe_experience_graph")));
	TestTrue(TEXT("lyra.validate_experience_bundle action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("validate_experience_bundle")));
	TestTrue(TEXT("lyra.describe_user_facing_experience action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("describe_user_facing_experience")));
	TestTrue(TEXT("lyra.validate_user_facing_experience action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("validate_user_facing_experience")));
	TestTrue(TEXT("lyra.validate_map_default_experience action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("validate_map_default_experience")));
	TestTrue(TEXT("lyra.validate_user_facing_map_reachability action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("validate_user_facing_map_reachability")));
	TestTrue(TEXT("lyra.describe_gameplay_tag_domain action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("describe_gameplay_tag_domain")));
	TestTrue(TEXT("lyra.validate_game_phase_flow action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("validate_game_phase_flow")));
	TestTrue(TEXT("lyra.describe_team_setup action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("describe_team_setup")));
	TestTrue(TEXT("lyra.describe_inventory_item action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("describe_inventory_item")));
	TestTrue(TEXT("lyra.describe_equipment_definition action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("describe_equipment_definition")));
	TestTrue(TEXT("lyra.describe_weapon_definition action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("describe_weapon_definition")));
	TestTrue(TEXT("lyra.describe_pawn_initialization_graph action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("describe_pawn_initialization_graph")));
	TestTrue(TEXT("lyra.validate_pawn_data_contract action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("validate_pawn_data_contract")));
	TestTrue(TEXT("lyra.describe_character_part_graph action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("describe_character_part_graph")));
	TestTrue(TEXT("lyra.validate_character_part_assets action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("validate_character_part_assets")));
	TestTrue(TEXT("lyra.set_experience_defaults action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("set_experience_defaults")));
	TestTrue(TEXT("lyra.add_experience_component_entry action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("add_experience_component_entry")));
	TestTrue(TEXT("lyra.remove_experience_component_entry action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("remove_experience_component_entry")));
	TestTrue(TEXT("lyra.set_user_facing_experience action is registered"), Registry.HasAction(TEXT("lyra"), TEXT("set_user_facing_experience")));

	FMonolithActionResult Status = FMonolithLyraActions::GetStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("get_status succeeds"), Status.bSuccess);
	TestTrue(TEXT("get_status returns json"), Status.Result.IsValid());
	if (Status.Result.IsValid())
	{
		TestEqual(TEXT("status namespace"), Status.Result->GetStringField(TEXT("namespace")), FString(TEXT("lyra")));
		const TArray<TSharedPtr<FJsonValue>>* CurrentActions = nullptr;
		TestTrue(TEXT("status has current_actions"), Status.Result->TryGetArrayField(TEXT("current_actions"), CurrentActions) && CurrentActions);
		bool bStatusHasAddComponentAction = false;
		if (CurrentActions)
		{
			for (const TSharedPtr<FJsonValue>& Value : *CurrentActions)
			{
				FString ActionId;
				if (Value.IsValid() && Value->TryGetString(ActionId) && ActionId == TEXT("lyra.add_experience_component_entry"))
				{
					bStatusHasAddComponentAction = true;
					break;
				}
			}
		}
		TestTrue(TEXT("status lists add_experience_component_entry"), bStatusHasAddComponentAction);
	}

	TSharedPtr<FJsonObject> AddComponentSchemaMissingActor = MakeShared<FJsonObject>();
	AddComponentSchemaMissingActor->SetStringField(TEXT("action_set_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingActionSet.MissingActionSet"));
	AddComponentSchemaMissingActor->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"));
	AddComponentSchemaMissingActor->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult AddComponentRequiredActor = Registry.ExecuteAction(
		TEXT("lyra"),
		TEXT("add_experience_component_entry"),
		AddComponentSchemaMissingActor);
	TestFalse(TEXT("add_experience_component_entry schema requires actor_class"), AddComponentRequiredActor.bSuccess);
	TestEqual(TEXT("add_experience_component_entry missing actor schema code"), AddComponentRequiredActor.ErrorCode, -32602);

	FMonolithActionResult TagDomain = FMonolithLyraActions::DescribeGameplayTagDomain(MakeShared<FJsonObject>());
	TestTrue(TEXT("describe_gameplay_tag_domain succeeds with defaults"), TagDomain.bSuccess);
	TestTrue(TEXT("describe_gameplay_tag_domain returns json"), TagDomain.Result.IsValid());
	if (TagDomain.Result.IsValid())
	{
		TestTrue(TEXT("describe_gameplay_tag_domain has tag_domain object"), TagDomain.Result->HasTypedField<EJson::Object>(TEXT("tag_domain")));
	}

	TSharedPtr<FJsonObject> BadTagDomainParams = MakeShared<FJsonObject>();
	BadTagDomainParams->SetNumberField(TEXT("max_tags"), 0);
	FMonolithActionResult BadTagDomain = FMonolithLyraActions::DescribeGameplayTagDomain(BadTagDomainParams);
	TestFalse(TEXT("describe_gameplay_tag_domain rejects non-positive max_tags"), BadTagDomain.bSuccess);
	TestEqual(TEXT("describe_gameplay_tag_domain invalid param code"), BadTagDomain.ErrorCode, -32602);

	TSharedPtr<FJsonObject> IncompatiblePhaseAbilityParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> IncompatiblePhaseAbilityPaths;
	IncompatiblePhaseAbilityPaths.Add(MakeShared<FJsonValueString>(TEXT("/Script/Engine.Actor")));
	IncompatiblePhaseAbilityParams->SetArrayField(TEXT("phase_ability_paths"), IncompatiblePhaseAbilityPaths);
	IncompatiblePhaseAbilityParams->SetNumberField(TEXT("max_assets"), 1);
	FMonolithActionResult IncompatiblePhaseAbility = FMonolithLyraActions::ValidateGamePhaseFlow(IncompatiblePhaseAbilityParams);
	TestTrue(TEXT("validate_game_phase_flow reports explicit incompatible phase ability as structured result"), IncompatiblePhaseAbility.bSuccess);
	TestTrue(TEXT("validate_game_phase_flow returns json"), IncompatiblePhaseAbility.Result.IsValid());
	if (IncompatiblePhaseAbility.Result.IsValid())
	{
		TestFalse(TEXT("validate_game_phase_flow ok=false for incompatible explicit phase ability"), IncompatiblePhaseAbility.Result->GetBoolField(TEXT("ok")));
		TestTrue(TEXT("validate_game_phase_flow includes explicit_path_errors"), IncompatiblePhaseAbility.Result->HasTypedField<EJson::Array>(TEXT("explicit_path_errors")));
	}

	TSharedPtr<FJsonObject> BadPhaseParams = MakeShared<FJsonObject>();
	BadPhaseParams->SetStringField(TEXT("phase_ability_paths"), TEXT("not-an-array"));
	FMonolithActionResult BadPhase = FMonolithLyraActions::ValidateGamePhaseFlow(BadPhaseParams);
	TestFalse(TEXT("validate_game_phase_flow rejects non-array phase_ability_paths"), BadPhase.bSuccess);
	TestEqual(TEXT("validate_game_phase_flow invalid param code"), BadPhase.ErrorCode, -32602);

	FMonolithActionResult MissingExperience = FMonolithLyraActions::DescribeExperienceGraph(MakeShared<FJsonObject>());
	TestFalse(TEXT("describe_experience_graph rejects missing experience_path"), MissingExperience.bSuccess);
	TestEqual(TEXT("describe_experience_graph invalid param code"), MissingExperience.ErrorCode, -32602);

	FMonolithActionResult MissingValidation = FMonolithLyraActions::ValidateExperienceBundle(MakeShared<FJsonObject>());
	TestFalse(TEXT("validate_experience_bundle rejects missing experience_path"), MissingValidation.bSuccess);
	TestEqual(TEXT("validate_experience_bundle invalid param code"), MissingValidation.ErrorCode, -32602);

	TSharedPtr<FJsonObject> DeepValidationParams = MakeShared<FJsonObject>();
	DeepValidationParams->SetStringField(TEXT("experience_path"), TEXT("/Script/Engine.Actor"));
	DeepValidationParams->SetBoolField(TEXT("validate_default_pawn_data"), true);
	DeepValidationParams->SetBoolField(TEXT("require_pawn_input_config"), true);
	DeepValidationParams->SetBoolField(TEXT("require_pawn_ability_sets"), true);
	DeepValidationParams->SetBoolField(TEXT("validate_action_sets"), true);
	DeepValidationParams->SetBoolField(TEXT("validate_game_feature_plugins"), true);
	FMonolithActionResult DeepValidation = FMonolithLyraActions::ValidateExperienceBundle(DeepValidationParams);
	TestTrue(TEXT("validate_experience_bundle deep mode returns structured result for bad path"), DeepValidation.bSuccess);
	TestTrue(TEXT("validate_experience_bundle deep mode returns json"), DeepValidation.Result.IsValid());
	if (DeepValidation.Result.IsValid())
	{
		TestFalse(TEXT("validate_experience_bundle deep mode ok=false for incompatible path"), DeepValidation.Result->GetBoolField(TEXT("ok")));
		TestTrue(TEXT("validate_experience_bundle deep mode has checks"), DeepValidation.Result->HasTypedField<EJson::Array>(TEXT("checks")));
		TestTrue(TEXT("validate_experience_bundle deep mode has game_feature_plugins"), DeepValidation.Result->HasTypedField<EJson::Array>(TEXT("game_feature_plugins")));
		TestTrue(TEXT("validate_experience_bundle deep mode flag echoed"), DeepValidation.Result->GetBoolField(TEXT("validate_default_pawn_data")));
	}

	FMonolithActionResult MissingUserFacing = FMonolithLyraActions::ValidateUserFacingExperience(MakeShared<FJsonObject>());
	TestFalse(TEXT("validate_user_facing_experience rejects missing user_facing_experience_path"), MissingUserFacing.bSuccess);
	TestEqual(TEXT("validate_user_facing_experience invalid param code"), MissingUserFacing.ErrorCode, -32602);

	FMonolithActionResult MissingMapDefault = FMonolithLyraActions::ValidateMapDefaultExperience(MakeShared<FJsonObject>());
	TestFalse(TEXT("validate_map_default_experience rejects missing map_path"), MissingMapDefault.bSuccess);
	TestEqual(TEXT("validate_map_default_experience invalid param code"), MissingMapDefault.ErrorCode, -32602);

	TSharedPtr<FJsonObject> BadExpectedExperienceParams = MakeShared<FJsonObject>();
	BadExpectedExperienceParams->SetStringField(TEXT("map_path"), TEXT("/Script/Engine.Actor"));
	BadExpectedExperienceParams->SetStringField(TEXT("expected_experience_id"), TEXT("Map:/Game/Maps/L_Test"));
	FMonolithActionResult BadExpectedExperience = FMonolithLyraActions::ValidateMapDefaultExperience(BadExpectedExperienceParams);
	TestFalse(TEXT("validate_map_default_experience rejects non-Lyra expected experience id"), BadExpectedExperience.bSuccess);
	TestEqual(TEXT("validate_map_default_experience expected id invalid param code"), BadExpectedExperience.ErrorCode, -32602);

	TSharedPtr<FJsonObject> BadMapParams = MakeShared<FJsonObject>();
	BadMapParams->SetStringField(TEXT("map_path"), TEXT("/Script/Engine.Actor"));
	FMonolithActionResult BadMap = FMonolithLyraActions::ValidateMapDefaultExperience(BadMapParams);
	TestTrue(TEXT("validate_map_default_experience returns structured result for bad map"), BadMap.bSuccess);
	TestTrue(TEXT("validate_map_default_experience returns json"), BadMap.Result.IsValid());
	if (BadMap.Result.IsValid())
	{
		TestFalse(TEXT("validate_map_default_experience ok=false for bad map"), BadMap.Result->GetBoolField(TEXT("ok")));
		TestTrue(TEXT("validate_map_default_experience has map object"), BadMap.Result->HasTypedField<EJson::Object>(TEXT("map")));
		TestTrue(TEXT("validate_map_default_experience has checks"), BadMap.Result->HasTypedField<EJson::Array>(TEXT("checks")));
	}

	FMonolithActionResult MissingReachability = FMonolithLyraActions::ValidateUserFacingMapReachability(MakeShared<FJsonObject>());
	TestFalse(TEXT("validate_user_facing_map_reachability rejects missing user_facing_experience_path"), MissingReachability.bSuccess);
	TestEqual(TEXT("validate_user_facing_map_reachability invalid param code"), MissingReachability.ErrorCode, -32602);

	TSharedPtr<FJsonObject> BadReachabilityParams = MakeShared<FJsonObject>();
	BadReachabilityParams->SetStringField(TEXT("user_facing_experience_path"), TEXT("/Script/Engine.Actor"));
	FMonolithActionResult BadReachability = FMonolithLyraActions::ValidateUserFacingMapReachability(BadReachabilityParams);
	TestTrue(TEXT("validate_user_facing_map_reachability returns structured result for bad asset"), BadReachability.bSuccess);
	TestTrue(TEXT("validate_user_facing_map_reachability returns json"), BadReachability.Result.IsValid());
	if (BadReachability.Result.IsValid())
	{
		TestFalse(TEXT("validate_user_facing_map_reachability ok=false for bad asset"), BadReachability.Result->GetBoolField(TEXT("ok")));
		TestTrue(TEXT("validate_user_facing_map_reachability has checks"), BadReachability.Result->HasTypedField<EJson::Array>(TEXT("checks")));
	}

	FMonolithActionResult TeamSetup = FMonolithLyraActions::DescribeTeamSetup(MakeShared<FJsonObject>());
	TestTrue(TEXT("describe_team_setup succeeds with defaults"), TeamSetup.bSuccess);
	TestTrue(TEXT("describe_team_setup returns json"), TeamSetup.Result.IsValid());
	if (TeamSetup.Result.IsValid())
	{
		TestTrue(TEXT("describe_team_setup has team_setup object"), TeamSetup.Result->HasTypedField<EJson::Object>(TEXT("team_setup")));
	}

	FMonolithActionResult MissingInventory = FMonolithLyraActions::DescribeInventoryItem(MakeShared<FJsonObject>());
	TestFalse(TEXT("describe_inventory_item rejects missing item_definition_path"), MissingInventory.bSuccess);
	TestEqual(TEXT("describe_inventory_item invalid param code"), MissingInventory.ErrorCode, -32602);

	FMonolithActionResult MissingEquipment = FMonolithLyraActions::DescribeEquipmentDefinition(MakeShared<FJsonObject>());
	TestFalse(TEXT("describe_equipment_definition rejects missing equipment_definition_path"), MissingEquipment.bSuccess);
	TestEqual(TEXT("describe_equipment_definition invalid param code"), MissingEquipment.ErrorCode, -32602);

	FMonolithActionResult MissingWeapon = FMonolithLyraActions::DescribeWeaponDefinition(MakeShared<FJsonObject>());
	TestFalse(TEXT("describe_weapon_definition rejects missing item_definition_path"), MissingWeapon.bSuccess);
	TestEqual(TEXT("describe_weapon_definition invalid param code"), MissingWeapon.ErrorCode, -32602);

	FMonolithActionResult MissingPawnGraph = FMonolithLyraActions::DescribePawnInitializationGraph(MakeShared<FJsonObject>());
	TestFalse(TEXT("describe_pawn_initialization_graph rejects missing pawn_data_path"), MissingPawnGraph.bSuccess);
	TestEqual(TEXT("describe_pawn_initialization_graph invalid param code"), MissingPawnGraph.ErrorCode, -32602);

	FMonolithActionResult MissingPawnValidation = FMonolithLyraActions::ValidatePawnDataContract(MakeShared<FJsonObject>());
	TestFalse(TEXT("validate_pawn_data_contract rejects missing pawn_data_path"), MissingPawnValidation.bSuccess);
	TestEqual(TEXT("validate_pawn_data_contract invalid param code"), MissingPawnValidation.ErrorCode, -32602);

	FMonolithActionResult CharacterPartGraph = FMonolithLyraActions::DescribeCharacterPartGraph(MakeShared<FJsonObject>());
	TestTrue(TEXT("describe_character_part_graph succeeds with defaults"), CharacterPartGraph.bSuccess);
	TestTrue(TEXT("describe_character_part_graph returns json"), CharacterPartGraph.Result.IsValid());
	if (CharacterPartGraph.Result.IsValid())
	{
		TestTrue(TEXT("describe_character_part_graph has reflected_types"), CharacterPartGraph.Result->HasTypedField<EJson::Array>(TEXT("reflected_types")));
	}

	FMonolithActionResult EmptyCharacterParts = FMonolithLyraActions::ValidateCharacterPartAssets(MakeShared<FJsonObject>());
	TestTrue(TEXT("validate_character_part_assets returns structured result for empty defaults"), EmptyCharacterParts.bSuccess);
	TestTrue(TEXT("validate_character_part_assets returns json"), EmptyCharacterParts.Result.IsValid());
	if (EmptyCharacterParts.Result.IsValid())
	{
		TestFalse(TEXT("validate_character_part_assets ok=false for empty required list"), EmptyCharacterParts.Result->GetBoolField(TEXT("ok")));
	}

	TSharedPtr<FJsonObject> BadCharacterPartParams = MakeShared<FJsonObject>();
	BadCharacterPartParams->SetStringField(TEXT("part_classes"), TEXT("not-an-array"));
	FMonolithActionResult BadCharacterParts = FMonolithLyraActions::ValidateCharacterPartAssets(BadCharacterPartParams);
	TestFalse(TEXT("validate_character_part_assets rejects non-array part_classes"), BadCharacterParts.bSuccess);
	TestEqual(TEXT("validate_character_part_assets invalid param code"), BadCharacterParts.ErrorCode, -32602);

	TSharedPtr<FJsonObject> SetExperienceWithoutGate = MakeShared<FJsonObject>();
	SetExperienceWithoutGate->SetStringField(TEXT("experience_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingExperience.MissingExperience"));
	SetExperienceWithoutGate->SetStringField(TEXT("default_pawn_data"), TEXT("/Game/Tests/Monolith/Lyra/MissingPawnData.MissingPawnData"));
	FMonolithActionResult SetExperienceGate = FMonolithLyraActions::SetExperienceDefaults(SetExperienceWithoutGate);
	TestFalse(TEXT("set_experience_defaults rejects mutation without dry_run or confirm"), SetExperienceGate.bSuccess);
	TestTrue(TEXT("set_experience_defaults gate mentions dry_run or confirm"), SetExperienceGate.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	TSharedPtr<FJsonObject> AddComponentWithoutGate = MakeShared<FJsonObject>();
	AddComponentWithoutGate->SetStringField(TEXT("action_set_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingActionSet.MissingActionSet"));
	AddComponentWithoutGate->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	AddComponentWithoutGate->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"));
	FMonolithActionResult AddComponentGate = FMonolithLyraActions::AddExperienceComponentEntry(AddComponentWithoutGate);
	TestFalse(TEXT("add_experience_component_entry rejects mutation without dry_run or confirm"), AddComponentGate.bSuccess);
	TestTrue(TEXT("add_experience_component_entry gate mentions dry_run or confirm"), AddComponentGate.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	TSharedPtr<FJsonObject> AddComponentMissingTarget = MakeShared<FJsonObject>();
	AddComponentMissingTarget->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	AddComponentMissingTarget->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"));
	AddComponentMissingTarget->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult MissingAddComponentTarget = FMonolithLyraActions::AddExperienceComponentEntry(AddComponentMissingTarget);
	TestFalse(TEXT("add_experience_component_entry rejects a missing owner target"), MissingAddComponentTarget.bSuccess);
	TestEqual(TEXT("add_experience_component_entry missing owner invalid param code"), MissingAddComponentTarget.ErrorCode, -32602);

	TSharedPtr<FJsonObject> AddComponentAmbiguousTarget = MakeShared<FJsonObject>();
	AddComponentAmbiguousTarget->SetStringField(TEXT("experience_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingExperience.MissingExperience"));
	AddComponentAmbiguousTarget->SetStringField(TEXT("action_set_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingActionSet.MissingActionSet"));
	AddComponentAmbiguousTarget->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	AddComponentAmbiguousTarget->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"));
	AddComponentAmbiguousTarget->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult AmbiguousAddComponentTarget = FMonolithLyraActions::AddExperienceComponentEntry(AddComponentAmbiguousTarget);
	TestFalse(TEXT("add_experience_component_entry rejects two owner targets"), AmbiguousAddComponentTarget.bSuccess);
	TestEqual(TEXT("add_experience_component_entry ambiguous owner invalid param code"), AmbiguousAddComponentTarget.ErrorCode, -32602);

	TSharedPtr<FJsonObject> AddComponentBadFlags = MakeShared<FJsonObject>();
	AddComponentBadFlags->SetStringField(TEXT("action_set_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingActionSet.MissingActionSet"));
	AddComponentBadFlags->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	AddComponentBadFlags->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"));
	AddComponentBadFlags->SetNumberField(TEXT("addition_flags"), 256);
	AddComponentBadFlags->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult BadAddComponentFlags = FMonolithLyraActions::AddExperienceComponentEntry(AddComponentBadFlags);
	TestFalse(TEXT("add_experience_component_entry rejects addition_flags outside uint8"), BadAddComponentFlags.bSuccess);
	TestEqual(TEXT("add_experience_component_entry flags invalid param code"), BadAddComponentFlags.ErrorCode, -32602);

	TSharedPtr<FJsonObject> AddComponentNoNetworkSide = MakeShared<FJsonObject>();
	AddComponentNoNetworkSide->SetStringField(TEXT("action_set_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingActionSet.MissingActionSet"));
	AddComponentNoNetworkSide->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	AddComponentNoNetworkSide->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"));
	AddComponentNoNetworkSide->SetBoolField(TEXT("client_component"), false);
	AddComponentNoNetworkSide->SetBoolField(TEXT("server_component"), false);
	AddComponentNoNetworkSide->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult NoNetworkSideAddComponent = FMonolithLyraActions::AddExperienceComponentEntry(AddComponentNoNetworkSide);
	TestFalse(TEXT("add_experience_component_entry requires a client or server side"), NoNetworkSideAddComponent.bSuccess);
	TestEqual(TEXT("add_experience_component_entry network side invalid param code"), NoNetworkSideAddComponent.ErrorCode, -32602);

	TSharedPtr<FJsonObject> AddComponentBadActorClass = MakeShared<FJsonObject>();
	AddComponentBadActorClass->SetStringField(TEXT("action_set_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingActionSet.MissingActionSet"));
	AddComponentBadActorClass->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.ActorComponent"));
	AddComponentBadActorClass->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"));
	AddComponentBadActorClass->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult BadAddComponentActorClass = FMonolithLyraActions::AddExperienceComponentEntry(AddComponentBadActorClass);
	TestFalse(TEXT("add_experience_component_entry validates actor_class derivation"), BadAddComponentActorClass.bSuccess);
	TestTrue(TEXT("add_experience_component_entry actor class error names Actor"), BadAddComponentActorClass.ErrorMessage.Contains(TEXT("Actor")));

	TSharedPtr<FJsonObject> AddComponentBadComponentClass = MakeShared<FJsonObject>();
	AddComponentBadComponentClass->SetStringField(TEXT("action_set_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingActionSet.MissingActionSet"));
	AddComponentBadComponentClass->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	AddComponentBadComponentClass->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.Actor"));
	AddComponentBadComponentClass->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult BadAddComponentComponentClass = FMonolithLyraActions::AddExperienceComponentEntry(AddComponentBadComponentClass);
	TestFalse(TEXT("add_experience_component_entry validates component_class derivation"), BadAddComponentComponentClass.bSuccess);
	TestTrue(TEXT("add_experience_component_entry component class error names ActorComponent"), BadAddComponentComponentClass.ErrorMessage.Contains(TEXT("ActorComponent")));

	TSharedPtr<FJsonObject> RemoveComponentWithoutGate = MakeShared<FJsonObject>();
	RemoveComponentWithoutGate->SetStringField(TEXT("experience_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingExperience.MissingExperience"));
	RemoveComponentWithoutGate->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"));
	FMonolithActionResult RemoveComponentGate = FMonolithLyraActions::RemoveExperienceComponentEntry(RemoveComponentWithoutGate);
	TestFalse(TEXT("remove_experience_component_entry rejects mutation without dry_run or confirm"), RemoveComponentGate.bSuccess);
	TestTrue(TEXT("remove_experience_component_entry gate mentions dry_run or confirm"), RemoveComponentGate.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	TSharedPtr<FJsonObject> SetUserFacingWithoutGate = MakeShared<FJsonObject>();
	SetUserFacingWithoutGate->SetStringField(TEXT("user_facing_experience_path"), TEXT("/Game/Tests/Monolith/Lyra/MissingPlaylist.MissingPlaylist"));
	SetUserFacingWithoutGate->SetStringField(TEXT("map_id"), TEXT("Map:/Game/Tests/Monolith/Lyra/L_Test"));
	FMonolithActionResult SetUserFacingGate = FMonolithLyraActions::SetUserFacingExperience(SetUserFacingWithoutGate);
	TestFalse(TEXT("set_user_facing_experience rejects mutation without dry_run or confirm"), SetUserFacingGate.bSuccess);
	TestTrue(TEXT("set_user_facing_experience gate mentions dry_run or confirm"), SetUserFacingGate.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLyraAddExperienceComponentEntryActionNameContractTest,
	"Monolith.Lyra.AddExperienceComponentEntry.ActionNameContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLyraAddExperienceComponentEntryActionNameContractTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("lyra"), TEXT("add_experience_component_entry")))
	{
		FMonolithLyraActions::RegisterActions(Registry);
	}

	UClass* ActionSetClass = StaticLoadClass(
		UObject::StaticClass(),
		nullptr,
		TEXT("/Script/LyraGame.LyraExperienceActionSet"));
	UClass* AddComponentsActionClass = StaticLoadClass(
		UObject::StaticClass(),
		nullptr,
		TEXT("/Script/GameFeatures.GameFeatureAction_AddComponents"));
	TestNotNull(TEXT("Lyra ExperienceActionSet class is available"), ActionSetClass);
	TestNotNull(TEXT("GameFeatureAction_AddComponents class is available"), AddComponentsActionClass);
	if (!ActionSetClass || !AddComponentsActionClass)
	{
		return false;
	}

	const FName OwnerName = MakeUniqueObjectName(
		GetTransientPackage(),
		ActionSetClass,
		TEXT("MonolithLyraActionNameContractOwner"));
	UObject* ActionSet = NewObject<UObject>(
		GetTransientPackage(),
		ActionSetClass,
		OwnerName,
		RF_Transient);
	TestNotNull(TEXT("Transient ExperienceActionSet owner was created"), ActionSet);
	if (!ActionSet)
	{
		return false;
	}

	const FString ExactActionName = TEXT("ExactNamedAddComponentsAction");
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("action_set_path"), ActionSet->GetPathName());
	CreateParams->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	CreateParams->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"));
	CreateParams->SetStringField(TEXT("action_name"), ExactActionName);
	CreateParams->SetBoolField(TEXT("confirm"), true);

	FMonolithActionResult CreateResult = Registry.ExecuteAction(
		TEXT("lyra"),
		TEXT("add_experience_component_entry"),
		CreateParams);
	TestTrue(TEXT("explicit action_name creation succeeds through registry action path"), CreateResult.bSuccess);
	TestTrue(TEXT("explicit action_name creation returns json"), CreateResult.Result.IsValid());
	if (CreateResult.Result.IsValid())
	{
		TestTrue(TEXT("explicit action_name creates an action"), CreateResult.Result->GetBoolField(TEXT("created_action")));
		TestTrue(TEXT("explicit action_name adds the component pair"), CreateResult.Result->GetBoolField(TEXT("added_component")));
		TestEqual(
			TEXT("reported action object name is the exact requested FName"),
			CreateResult.Result->GetStringField(TEXT("action_object_name")),
			ExactActionName);
	}

	UObject* ExactNamedAction = StaticFindObjectFast(
		UObject::StaticClass(),
		ActionSet,
		FName(*ExactActionName));
	TestNotNull(TEXT("exact requested action exists as an owner direct child"), ExactNamedAction);
	if (ExactNamedAction)
	{
		TestEqual(TEXT("created action did not gain a numeric suffix"), ExactNamedAction->GetName(), ExactActionName);
		TestTrue(TEXT("exact named child has AddComponents class"), ExactNamedAction->IsA(AddComponentsActionClass));
	}

	FMonolithActionResult RepeatResult = Registry.ExecuteAction(
		TEXT("lyra"),
		TEXT("add_experience_component_entry"),
		CreateParams);
	TestTrue(TEXT("same explicit request succeeds as an idempotent no-op"), RepeatResult.bSuccess);
	TestTrue(TEXT("same explicit request returns json"), RepeatResult.Result.IsValid());
	if (RepeatResult.Result.IsValid())
	{
		TestFalse(TEXT("same explicit request does not create another action"), RepeatResult.Result->GetBoolField(TEXT("would_create_action")));
		TestFalse(TEXT("same explicit request reports no planned change"), RepeatResult.Result->GetBoolField(TEXT("would_change")));
		TestFalse(TEXT("same explicit request performs no mutation"), RepeatResult.Result->GetBoolField(TEXT("changed")));
		TestTrue(TEXT("same explicit request reuses the exact action"), RepeatResult.Result->GetBoolField(TEXT("reused_action")));
		TestEqual(
			TEXT("idempotent response preserves exact action name"),
			RepeatResult.Result->GetStringField(TEXT("action_object_name")),
			ExactActionName);
	}

	const FString OrphanActionName = TEXT("OrphanActionNameCollision");
	UCurveFloat* IncompatibleOrphan = NewObject<UCurveFloat>(
		ActionSet,
		FName(*OrphanActionName),
		RF_Transient);
	TestNotNull(TEXT("incompatible direct-child collision fixture was created"), IncompatibleOrphan);

	TSharedPtr<FJsonObject> CollisionParams = MakeShared<FJsonObject>();
	CollisionParams->SetStringField(TEXT("action_set_path"), ActionSet->GetPathName());
	CollisionParams->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.GameStateBase"));
	CollisionParams->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.SceneComponent"));
	CollisionParams->SetStringField(TEXT("action_name"), OrphanActionName);
	CollisionParams->SetBoolField(TEXT("dry_run"), true);

	FMonolithActionResult CollisionResult = Registry.ExecuteAction(
		TEXT("lyra"),
		TEXT("add_experience_component_entry"),
		CollisionParams);
	TestFalse(TEXT("dry-run fails on an incompatible owner direct-child name collision"), CollisionResult.bSuccess);
	TestEqual(TEXT("direct-child collision is an invalid-param error"), CollisionResult.ErrorCode, -32602);
	TestTrue(TEXT("collision error identifies the owner direct child"), CollisionResult.ErrorMessage.Contains(TEXT("direct child")));
	TestTrue(TEXT("collision error identifies the incompatible class contract"), CollisionResult.ErrorMessage.Contains(TEXT("incompatible class")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
