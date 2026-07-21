#include "Misc/AutomationTest.h"
#include "MonolithMeshContextPropActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorldGenSetRoomDisturbanceParamTest, "Monolith.ParamGuard.WorldGen.SetRoomDisturbanceRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorldGenSetRoomDisturbanceParamTest::RunTest(const FString& Parameters)
{
	FMonolithMeshContextPropActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("set_room_disturbance action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("set_room_disturbance")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Test missing volume_name
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("set_room_disturbance"), Params);
	TestFalse(TEXT("set_room_disturbance rejects missing volume_name"), Result.bSuccess);
	TestTrue(TEXT("set_room_disturbance reports missing volume_name"), Result.ErrorMessage.Contains(TEXT("volume_name")));

	Params->SetStringField(TEXT("volume_name"), TEXT("TestVolume"));

	// Test missing disturbance
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("set_room_disturbance"), Params);
	TestFalse(TEXT("set_room_disturbance rejects missing disturbance"), Result.bSuccess);
	TestTrue(TEXT("set_room_disturbance reports missing disturbance"), Result.ErrorMessage.Contains(TEXT("disturbance")));

	// Test invalid disturbance level
	Params->SetStringField(TEXT("disturbance"), TEXT("invalid_level"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("set_room_disturbance"), Params);
	TestFalse(TEXT("set_room_disturbance rejects invalid disturbance level"), Result.bSuccess);
	TestTrue(TEXT("set_room_disturbance reports invalid disturbance level"), Result.ErrorMessage.Contains(TEXT("Invalid disturbance level")));

	// Test valid disturbance level
	Params->SetStringField(TEXT("disturbance"), TEXT("slightly_messy"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("set_room_disturbance"), Params);
	// We don't have a real mock volume, but it should pass the param guard check for disturbance level
	TestFalse(TEXT("set_room_disturbance valid params doesn't report param errors"), Result.ErrorMessage.Contains(TEXT("Invalid disturbance level")) || Result.ErrorMessage.Contains(TEXT("Missing required param")));

	return true;
}
