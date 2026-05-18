#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithAnimationActions.h"
#include "MonolithToolRegistry.h"
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationSecurityPathTest, "Monolith.Security.Animation.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAnimationSecurityPathTest::RunTest(const FString& Parameters)
{
	// Setup payload with double slash to simulate malformed path
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), TEXT("//Game/MalformedPath/TestBlendSpace"));
	// skeleton_path is required by create_blend_space
	Payload->SetStringField(TEXT("skeleton_path"), TEXT("/Game/Anims/MySkeleton"));

	// Call the action
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("create_blend_space"), Payload);

	// Verify it failed gracefully and returned the validation error
	TestFalse(TEXT("Action should fail on malformed path"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about invalid package path"), Result.ErrorMessage.Contains(TEXT("Invalid package path")));

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

#endif // WITH_DEV_AUTOMATION_TESTS
