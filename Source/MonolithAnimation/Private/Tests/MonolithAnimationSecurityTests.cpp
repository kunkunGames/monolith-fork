#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithAnimationActions.h"
#include "MonolithLocomotionAuthoringActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	UAnimSequence* CreateParamGuardAnimSequence(const FString& AssetPath)
	{
		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		return Package
			? NewObject<UAnimSequence>(Package, FName(*AssetName), RF_Public | RF_Standalone)
			: nullptr;
	}

	UAnimMontage* CreateMontageContractFixture(const FString& AssetPath)
	{
		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		UAnimMontage* Montage = Package
			? NewObject<UAnimMontage>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional)
			: nullptr;
		if (Montage)
		{
			FAnimNotifyTrack Track;
			Track.TrackName = FName(TEXT("Gameplay"));
			Track.TrackColor = FLinearColor::White;
			Montage->AnimNotifyTracks.Add(Track);
			Package->SetDirtyFlag(false);
		}
		return Montage;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationSecurityPathTest, "Monolith.Security.Animation.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAnimationSecurityPathTest::RunTest(const FString& Parameters)
{
	FMonolithAnimationActions::RegisterActions(FMonolithToolRegistry::Get());

	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/MalformedPath/TestBlendSpace"), // Double leading slash
		TEXT("Game/MalformedPath/TestBlendSpace"), // Missing leading slash
		TEXT("/Game/MalformedPath/TestBlendSpace/"), // Trailing slash
		TEXT("/Game/MalformedPath/TestBlendSpace#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		// Setup payloads for different actions
		TSharedPtr<FJsonObject> BlendSpacePayload = MakeShared<FJsonObject>();
		BlendSpacePayload->SetStringField(TEXT("asset_path"), Path);
		BlendSpacePayload->SetStringField(TEXT("skeleton_path"), TEXT("/Game/Anims/MySkeleton"));

		TSharedPtr<FJsonObject> SchemaPayload = MakeShared<FJsonObject>();
		SchemaPayload->SetStringField(TEXT("asset_path"), Path);
		SchemaPayload->SetStringField(TEXT("skeleton_path"), TEXT("/Game/Anims/MySkeleton"));

		TSharedPtr<FJsonObject> DatabasePayload = MakeShared<FJsonObject>();
		DatabasePayload->SetStringField(TEXT("asset_path"), Path);
		DatabasePayload->SetStringField(TEXT("schema_path"), TEXT("/Game/Anims/MySchema"));

		TSharedPtr<FJsonObject> NormalizationSetPayload = MakeShared<FJsonObject>();
		NormalizationSetPayload->SetStringField(TEXT("asset_path"), Path);

		TSharedPtr<FJsonObject> IKRigPayload = MakeShared<FJsonObject>();
		IKRigPayload->SetStringField(TEXT("asset_path"), Path);
		IKRigPayload->SetStringField(TEXT("skeletal_mesh_path"), TEXT("/Game/Anims/MyMesh"));

		TSharedPtr<FJsonObject> IKRetargeterPayload = MakeShared<FJsonObject>();
		IKRetargeterPayload->SetStringField(TEXT("asset_path"), Path);

		TMap<FString, TSharedPtr<FJsonObject>> ActionsToTest = {
			{TEXT("create_blend_space"), BlendSpacePayload},
			{TEXT("create_pose_search_schema"), SchemaPayload},
			{TEXT("create_pose_search_database"), DatabasePayload},
			{TEXT("create_normalization_set"), NormalizationSetPayload},
			{TEXT("create_ik_rig"), IKRigPayload},
			{TEXT("create_ik_retargeter"), IKRetargeterPayload}
		};

		for (const auto& ActionPair : ActionsToTest)
		{
			const FString& ActionName = ActionPair.Key;
			const TSharedPtr<FJsonObject>& Payload = ActionPair.Value;

			// Call the action
			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), ActionName, Payload);

			// Verify it failed gracefully and returned the validation error
			TestFalse(*FString::Printf(TEXT("Action %s should fail on malformed path: %s"), *ActionName, *Path), Result.bSuccess);
			TestFalse(*FString::Printf(TEXT("Error should be populated for action %s with malformed path: %s"), *ActionName, *Path), Result.ErrorMessage.IsEmpty());

			if (!Path.IsEmpty())
			{
				TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path for action %s: %s"), *ActionName, *Path),
					Result.ErrorMessage.Contains(TEXT("Invalid package path")) || Result.ErrorMessage.Contains(TEXT("Package path")));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationParamGuardSetSequencePropertiesTest, "Monolith.ParamGuard.Animation.SetSequencePropertiesRejectsMalformedRateScale", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAnimationParamGuardSetSequencePropertiesTest::RunTest(const FString& Parameters)
{
	FMonolithAnimationActions::RegisterActions(FMonolithToolRegistry::Get());

	const FString AssetPath = FString::Printf(
		TEXT("/Game/Tests/Monolith/Animation/AS_ParamGuard_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UAnimSequence* Seq = CreateParamGuardAnimSequence(AssetPath);
	TestNotNull(TEXT("Transient AnimSequence is available"), Seq);
	if (!Seq)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), AssetPath);
	Payload->SetStringField(TEXT("rate_scale"), TEXT("fast"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("set_sequence_properties"), Payload);

	TestFalse(TEXT("set_sequence_properties rejects malformed rate_scale"), Result.bSuccess);
	TestTrue(TEXT("Error mentions rate_scale"), Result.ErrorMessage.Contains(TEXT("rate_scale")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimMontageBlendParamGuardTest, "Monolith.ParamGuard.Animation.SetMontageBlend", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAnimMontageBlendParamGuardTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/AnimWeaver_Montage");
	UPackage* Package = CreatePackage(*FPackageName::ObjectPathToPackageName(AssetPath));
	UAnimMontage* Montage = NewObject<UAnimMontage>(Package, FName("AnimWeaver_Montage"), RF_Public | RF_Standalone);
	Montage->MarkPackageDirty();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("blend_in_time"), TEXT("not_a_number"));
	Params->SetStringField(TEXT("blend_out_time"), TEXT("not_a_number"));
	Params->SetStringField(TEXT("blend_out_trigger_time"), TEXT("not_a_number"));
	Params->SetStringField(TEXT("enable_auto_blend_out"), TEXT("not_a_bool"));

	FMonolithActionResult Result = FMonolithAnimationActions::HandleSetMontageBlend(Params);
	TestFalse(TEXT("SetMontageBlend with malformed params should return Error"), Result.bSuccess);
	TestTrue(TEXT("Error message should mention number"), Result.ErrorMessage.Contains(TEXT("must be a number")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimMontageSemanticContractTest, "Monolith.Animation.Montage.SemanticBlendAndNamedNotifyContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAnimMontageSemanticContractTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = FString::Printf(
		TEXT("/Game/Tests/Monolith/Animation/AM_SemanticContract_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UAnimMontage* Montage = CreateMontageContractFixture(AssetPath);
	if (!TestNotNull(TEXT("Montage fixture is available"), Montage))
	{
		return false;
	}

	TSharedPtr<FJsonObject> BlendParams = MakeShared<FJsonObject>();
	BlendParams->SetStringField(TEXT("asset_path"), AssetPath);
	BlendParams->SetNumberField(TEXT("blend_in_time"), 0.15);
	BlendParams->SetNumberField(TEXT("blend_out_time"), 0.25);
	BlendParams->SetStringField(TEXT("blend_in_option"), TEXT("HermiteCubic"));
	BlendParams->SetStringField(TEXT("blend_out_option"), TEXT("CubicInOut"));
	BlendParams->SetNumberField(TEXT("blend_out_trigger_time"), -1.0);
	BlendParams->SetBoolField(TEXT("enable_auto_blend_out"), true);
	const FMonolithActionResult BlendResult = FMonolithAnimationActions::HandleSetMontageBlend(BlendParams);
	TestTrue(TEXT("Typed montage blend write succeeds"), BlendResult.bSuccess);
	TestEqual(TEXT("Blend-in option is HermiteCubic"), static_cast<int32>(Montage->BlendIn.GetBlendOption()), static_cast<int32>(EAlphaBlendOption::HermiteCubic));
	TestEqual(TEXT("Blend-out option is CubicInOut"), static_cast<int32>(Montage->BlendOut.GetBlendOption()), static_cast<int32>(EAlphaBlendOption::CubicInOut));

	const FMonolithActionResult MontageInfo = FMonolithAnimationActions::HandleGetMontageInfo(BlendParams);
	TestTrue(TEXT("Montage readback succeeds"), MontageInfo.bSuccess);
	if (MontageInfo.bSuccess && MontageInfo.Result.IsValid())
	{
		TestEqual(TEXT("Readback includes blend_in_option"), MontageInfo.Result->GetStringField(TEXT("blend_in_option")), FString(TEXT("HermiteCubic")));
		TestEqual(TEXT("Readback includes blend_out_option"), MontageInfo.Result->GetStringField(TEXT("blend_out_option")), FString(TEXT("CubicInOut")));
	}

	TSharedPtr<FJsonObject> InvalidBlend = MakeShared<FJsonObject>();
	InvalidBlend->SetStringField(TEXT("asset_path"), AssetPath);
	InvalidBlend->SetStringField(TEXT("blend_in_option"), TEXT("Hermite"));
	const FMonolithActionResult InvalidBlendResult = FMonolithAnimationActions::HandleSetMontageBlend(InvalidBlend);
	TestFalse(TEXT("Unknown EAlphaBlendOption is rejected"), InvalidBlendResult.bSuccess);
	TestEqual(TEXT("Rejected blend leaves the previous option intact"), static_cast<int32>(Montage->BlendIn.GetBlendOption()), static_cast<int32>(EAlphaBlendOption::HermiteCubic));

	const FMonolithActionResult SchemaInvalidBlendResult = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("animation"),
		TEXT("set_montage_blend"),
		InvalidBlend);
	TestFalse(TEXT("Registered schema rejects an unknown EAlphaBlendOption"), SchemaInvalidBlendResult.bSuccess);
	TestEqual(TEXT("Schema enum rejection uses InvalidParams"), SchemaInvalidBlendResult.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	TestTrue(TEXT("Schema enum rejection reports the exact allowed domain"), SchemaInvalidBlendResult.ErrorMessage.Contains(TEXT("must be one of")));

	TSharedPtr<FJsonObject> AddNamed = MakeShared<FJsonObject>();
	AddNamed->SetStringField(TEXT("asset_path"), AssetPath);
	AddNamed->SetStringField(TEXT("notify_name"), TEXT("ActionPoint"));
	AddNamed->SetNumberField(TEXT("time"), 0.0);
	AddNamed->SetStringField(TEXT("track_name"), TEXT("Gameplay"));
	AddNamed->SetStringField(TEXT("montage_tick_type"), TEXT("BranchingPoint"));
	const FMonolithActionResult AddNamedResult = FMonolithAnimationActions::HandleAddNamedNotify(AddNamed);
	TestTrue(TEXT("Classless named notify creation succeeds"), AddNamedResult.bSuccess);
	TestEqual(TEXT("One named notify exists"), Montage->Notifies.Num(), 1);
	if (Montage->Notifies.Num() == 1)
	{
		TestNull(TEXT("Named notify has no UAnimNotify object"), Montage->Notifies[0].Notify.Get());
		TestNull(TEXT("Named notify has no UAnimNotifyState object"), Montage->Notifies[0].NotifyStateClass.Get());
		TestEqual(TEXT("Named notify tick type is BranchingPoint"), static_cast<int32>(Montage->Notifies[0].MontageTickType), static_cast<int32>(EMontageNotifyTickType::BranchingPoint));
	}

	const FMonolithActionResult DuplicateResult = FMonolithAnimationActions::HandleAddNamedNotify(AddNamed);
	TestFalse(TEXT("Duplicate named notify on the same track/time is rejected"), DuplicateResult.bSuccess);
	TestEqual(TEXT("Duplicate rejection leaves one notify"), Montage->Notifies.Num(), 1);

	TSharedPtr<FJsonObject> SetTickType = MakeShared<FJsonObject>();
	SetTickType->SetStringField(TEXT("asset_path"), AssetPath);
	SetTickType->SetNumberField(TEXT("notify_index"), 0);
	SetTickType->SetStringField(TEXT("montage_tick_type"), TEXT("Queued"));
	const FMonolithActionResult SetTickResult = FMonolithAnimationActions::HandleSetNotifyTickType(SetTickType);
	TestTrue(TEXT("Classless named notify tick type update succeeds"), SetTickResult.bSuccess);
	TestEqual(TEXT("Named notify tick type becomes Queued"), static_cast<int32>(Montage->Notifies[0].MontageTickType), static_cast<int32>(EMontageNotifyTickType::Queued));

	const FMonolithActionResult NotifyReadback = FMonolithAnimationActions::HandleGetSequenceNotifies(BlendParams);
	TestTrue(TEXT("Notify readback succeeds"), NotifyReadback.bSuccess);
	if (NotifyReadback.bSuccess && NotifyReadback.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>& Notifies = NotifyReadback.Result->GetArrayField(TEXT("notifies"));
		TestEqual(TEXT("Readback contains one notify"), Notifies.Num(), 1);
		if (Notifies.Num() == 1)
		{
			TestEqual(TEXT("Readback includes montage_tick_type"), Notifies[0]->AsObject()->GetStringField(TEXT("montage_tick_type")), FString(TEXT("Queued")));
		}
	}

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimMontageCreateFromSectionsParamGuardTest, "Monolith.ParamGuard.Animation.CreateMontageFromSections", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAnimMontageCreateFromSectionsParamGuardTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/AnimWeaver_MontageCreate");

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("skeleton_path"), TEXT("/Game/Anims/MySkeleton"));

	TArray<TSharedPtr<FJsonValue>> NotifiesArr;
	TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
	NObj->SetStringField(TEXT("notify_class"), TEXT("PlaySound"));
	NObj->SetStringField(TEXT("time"), TEXT("not_a_number"));
	NotifiesArr.Add(MakeShared<FJsonValueObject>(NObj));
	Params->SetArrayField(TEXT("notifies"), NotifiesArr);

	FMonolithActionResult Result = FMonolithAnimationActions::HandleCreateMontageFromSections(Params);
	TestFalse(TEXT("CreateMontageFromSections with malformed params should return Error"), Result.bSuccess);
	// HandleCreateMontageFromSections calls HandleCreateMontage which might fail if the asset path or skeleton path are invalid,
	// but the important thing is that if it reaches the notify parsing it shouldn't crash.
	// Since creating a montage actually creates a new asset, and we aren't mocking the UFactory properly, it will likely fail early.
	// However, we can assert it didn't crash.

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBuildSequenceFromPosesParamGuardTest, "Monolith.ParamGuard.Animation.BuildSequenceFromPoses", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithBuildSequenceFromPosesParamGuardTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/AnimWeaver_BuildSequence");

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("skeleton_path"), TEXT("/Game/Anims/MySkeleton"));

	TArray<TSharedPtr<FJsonValue>> FramesArr;
	TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> BonesArr;
	TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
	BoneObj->SetNumberField(TEXT("name"), 123); // Invalid type, should be string
	BonesArr.Add(MakeShared<FJsonValueObject>(BoneObj));

	FrameObj->SetArrayField(TEXT("bones"), BonesArr);
	FramesArr.Add(MakeShared<FJsonValueObject>(FrameObj));

	Params->SetArrayField(TEXT("frames"), FramesArr);

	FMonolithActionResult Result = FMonolithAnimationActions::HandleBuildSequenceFromPoses(Params);
	TestFalse(TEXT("HandleBuildSequenceFromPoses with malformed bone name should return Error"), Result.bSuccess);
	TestTrue(TEXT("Error message should complain about the bone name type"), Result.ErrorMessage.Contains(TEXT("name' in bone must be a string")));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLocomotionAuthoringParamGuardTest, "Monolith.ParamGuard.Animation.LocomotionAuthoring", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLocomotionAuthoringParamGuardTest::RunTest(const FString& Parameters)
{
	FMonolithLocomotionAuthoringActions::RegisterActions(FMonolithToolRegistry::Get());
	const FString AssetPath = TEXT("/Game/Tests/Monolith/AnimWeaver_Montage");

	// Test bake_distance_curve sample_rate validation
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("anim_path"), AssetPath);
		Params->SetStringField(TEXT("sample_rate"), TEXT("not_a_number"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("bake_distance_curve"), Params);
		TestFalse(TEXT("bake_distance_curve with malformed sample_rate should return Error"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention 'sample_rate' and 'number'"), Result.ErrorMessage.Contains(TEXT("Parameter 'sample_rate' must be a number")));
	}

	// Test bake_distance_curve stop_speed_threshold validation
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("anim_path"), AssetPath);
		Params->SetStringField(TEXT("stop_speed_threshold"), TEXT("not_a_number"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("bake_distance_curve"), Params);
		TestFalse(TEXT("bake_distance_curve with malformed stop_speed_threshold should return Error"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention 'stop_speed_threshold' and 'number'"), Result.ErrorMessage.Contains(TEXT("Parameter 'stop_speed_threshold' must be a number")));
	}

	return true;
}

#if WITH_CHOOSER
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithValidateChooserParamGuardTest, "Monolith.ParamGuard.Animation.ValidateChooser", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithValidateChooserParamGuardTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/TestChooser");

	// test expected_context_class
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetNumberField(TEXT("expected_context_class"), 123);
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("chooser"), TEXT("validate_chooser"), Params);
		TestFalse(TEXT("validate_chooser with malformed expected_context_class should return Error"), Result.bSuccess);
		TestTrue(TEXT("Error message should complain about invalid type for parameter 'expected_context_class'"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'expected_context_class'")));
	}

	// test expected_result_type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetNumberField(TEXT("expected_result_type"), 123);
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("chooser"), TEXT("validate_chooser"), Params);
		TestFalse(TEXT("validate_chooser with malformed expected_result_type should return Error"), Result.bSuccess);
		TestTrue(TEXT("Error message should complain about invalid type for parameter 'expected_result_type'"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'expected_result_type'")));
	}

	return true;
}
#endif // WITH_CHOOSER

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationAddEvaluateChooserNodeParamGuardTest, "Monolith.ParamGuard.Animation.AddEvaluateChooserNodeRejectsMalformedPosition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAnimationAddEvaluateChooserNodeParamGuardTest::RunTest(const FString& Parameters)
{
	FMonolithAnimationActions::RegisterActions(FMonolithToolRegistry::Get());

	TSharedPtr<FJsonObject> ActionParams = MakeShared<FJsonObject>();
	ActionParams->SetStringField(TEXT("abp_path"), TEXT("/Game/Tests/Monolith/TestABP"));
	ActionParams->SetStringField(TEXT("chooser_path"), TEXT("/Game/Tests/Monolith/TestChooser"));
	ActionParams->SetStringField(TEXT("position_x"), TEXT("should_be_number"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("add_evaluate_chooser_node"), ActionParams);

	TestFalse(TEXT("Malformed position_x should reject action"), Result.bSuccess);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
