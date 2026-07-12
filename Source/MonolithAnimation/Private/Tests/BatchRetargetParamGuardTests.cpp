#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardBatchRetargetTest, "Monolith.ParamGuard.BatchRetarget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardBatchRetargetTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!TestTrue(
		TEXT("Action animation/batch_retarget_animations is registered"),
		Registry.HasAction(TEXT("animation"), TEXT("batch_retarget_animations"))))
	{
		return false;
	}

	auto Execute = [&](const FString& JsonStr) -> FMonolithActionResult
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		TSharedPtr<FJsonObject> JsonObj;
		FJsonSerializer::Deserialize(Reader, JsonObj);
		return Registry.ExecuteAction(TEXT("animation"), TEXT("batch_retarget_animations"), JsonObj);
	};
	auto TestInvalidParam = [this](const TCHAR* Label, const TCHAR* Field, const FMonolithActionResult& Result)
	{
		TestFalse(*FString::Printf(TEXT("%s should fail"), Label), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s should report invalid_params"), Label), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(*FString::Printf(TEXT("%s error should identify %s"), Label, Field), Result.ErrorMessage.Contains(Field));
	};

	// 1. name_prefix bad type
	FMonolithActionResult PrefixRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"name_prefix\": 123 }"));
	TestInvalidParam(TEXT("name_prefix number"), TEXT("name_prefix"), PrefixRes);

	// 2. name_suffix bad type
	FMonolithActionResult SuffixRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"name_suffix\": 123 }"));
	TestInvalidParam(TEXT("name_suffix number"), TEXT("name_suffix"), SuffixRes);

	// 3. search bad type
	FMonolithActionResult SearchRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"search\": 123 }"));
	TestInvalidParam(TEXT("search number"), TEXT("search"), SearchRes);

	// 4. replace bad type
	FMonolithActionResult ReplaceRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"replace\": 123 }"));
	TestInvalidParam(TEXT("replace number"), TEXT("replace"), ReplaceRes);

	// 5. include_referenced bad type
	FMonolithActionResult IncludeRefRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"include_referenced\": 123 }"));
	TestInvalidParam(TEXT("include_referenced number"), TEXT("include_referenced"), IncludeRefRes);

	// 6. overwrite bad type
	FMonolithActionResult OverwriteRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"overwrite\": 123 }"));
	TestInvalidParam(TEXT("overwrite number"), TEXT("overwrite"), OverwriteRes);

	return true;
}
