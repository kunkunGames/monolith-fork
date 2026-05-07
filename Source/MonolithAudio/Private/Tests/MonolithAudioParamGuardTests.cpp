#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithAudioAssetActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteAudioAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("audio"), Action))
		{
			FMonolithAudioAssetActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("audio"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.ModifySoundSubmixRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SM_TestSubmix"));

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("output_volume_db"), TEXT("malformed")); // Should be number
	Params->SetObjectField(TEXT("properties"), Props);

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("modify_sound_submix"), Params);

	TestTrue(TEXT("ModifySoundSubmix with malformed property should return Error"), !Result.bSuccess);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
