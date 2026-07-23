#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithMeshFurnishingActions.h"

#if WITH_DEV_AUTOMATION_TESTS
#if WITH_GEOMETRYSCRIPT

BEGIN_DEFINE_SPEC(FMonolithWorldGenFurnishRoomParamTest, "Monolith.Sentinel.WorldGen.FurnishRoomParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	TSharedPtr<FJsonObject> Params;
END_DEFINE_SPEC(FMonolithWorldGenFurnishRoomParamTest)

void FMonolithWorldGenFurnishRoomParamTest::Define()
{
	BeforeEach([this]()
	{
		Params = MakeShared<FJsonObject>();
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("worldgen"), TEXT("furnish_room")))
		{
			FMonolithMeshFurnishingActions::RegisterActions(Registry);
		}
	});

	Describe("furnish_room parameter validation", [this]()
	{
		It("should fail when room_type is missing", [this]()
		{
			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("furnish_room"), Params);
			TestFalse(TEXT("Missing room_type should fail"), Result.bSuccess);
		});

		It("should fail when world_bounds is missing", [this]()
		{
			Params->SetStringField(TEXT("room_type"), TEXT("office"));
			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("furnish_room"), Params);
			TestFalse(TEXT("Missing world_bounds should fail"), Result.bSuccess);
		});

		It("should fail when world_bounds is malformed", [this]()
		{
			Params->SetStringField(TEXT("room_type"), TEXT("office"));
			TSharedPtr<FJsonObject> BadBounds = MakeShared<FJsonObject>();
			BadBounds->SetStringField(TEXT("min"), TEXT("not_an_array"));
			Params->SetObjectField(TEXT("world_bounds"), BadBounds);

			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("furnish_room"), Params);
			TestFalse(TEXT("Malformed world_bounds should fail"), Result.bSuccess);
		});

		It("should fail when save_path_prefix is missing", [this]()
		{
			Params->SetStringField(TEXT("room_type"), TEXT("office"));

			TSharedPtr<FJsonObject> ValidBounds = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> MinArr;
			MinArr.Add(MakeShared<FJsonValueNumber>(0));
			MinArr.Add(MakeShared<FJsonValueNumber>(0));
			MinArr.Add(MakeShared<FJsonValueNumber>(0));
			ValidBounds->SetArrayField(TEXT("min"), MinArr);
			TArray<TSharedPtr<FJsonValue>> MaxArr;
			MaxArr.Add(MakeShared<FJsonValueNumber>(100));
			MaxArr.Add(MakeShared<FJsonValueNumber>(100));
			MaxArr.Add(MakeShared<FJsonValueNumber>(100));
			ValidBounds->SetArrayField(TEXT("max"), MaxArr);
			Params->SetObjectField(TEXT("world_bounds"), ValidBounds);

			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("furnish_room"), Params);
			TestFalse(TEXT("Missing save_path_prefix should fail"), Result.bSuccess);
		});

		It("should succeed when all required parameters are valid", [this]()
		{
			Params->SetStringField(TEXT("room_type"), TEXT("office"));

			TSharedPtr<FJsonObject> ValidBounds = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> MinArr;
			MinArr.Add(MakeShared<FJsonValueNumber>(0));
			MinArr.Add(MakeShared<FJsonValueNumber>(0));
			MinArr.Add(MakeShared<FJsonValueNumber>(0));
			ValidBounds->SetArrayField(TEXT("min"), MinArr);
			TArray<TSharedPtr<FJsonValue>> MaxArr;
			MaxArr.Add(MakeShared<FJsonValueNumber>(100));
			MaxArr.Add(MakeShared<FJsonValueNumber>(100));
			MaxArr.Add(MakeShared<FJsonValueNumber>(100));
			ValidBounds->SetArrayField(TEXT("max"), MaxArr);
			Params->SetObjectField(TEXT("world_bounds"), ValidBounds);

			Params->SetStringField(TEXT("save_path_prefix"), TEXT("/Game/Town/Furniture"));

			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("furnish_room"), Params);
			TestTrue(TEXT("Valid parameters should succeed"), Result.bSuccess);
		});
	});
}

#endif // WITH_GEOMETRYSCRIPT
#endif // WITH_DEV_AUTOMATION_TESTS
