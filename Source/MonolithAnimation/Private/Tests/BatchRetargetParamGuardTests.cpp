#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithActionRegistry.h"
#include "Serialization/JsonSerializer.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardBatchRetargetTest, "Monolith.ParamGuard.BatchRetarget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardBatchRetargetTest::RunTest(const FString& Parameters)
{
	FMonolithActionRegistry& Registry = FMonolithToolRegistry::Get().GetActionRegistry();
	const FMonolithActionHandler* Handler = Registry.GetActionHandler(TEXT("animation"), TEXT("batch_retarget_animations"));

	if (!Handler)
	{
		AddError(TEXT("Action animation/batch_retarget_animations not found."));
		return false;
	}

	auto Execute = [&](const FString& JsonStr) -> FMonolithActionResult
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		TSharedPtr<FJsonObject> JsonObj;
		FJsonSerializer::Deserialize(Reader, JsonObj);
		return Handler->Execute(JsonObj);
	};

	// 1. name_prefix bad type
	FMonolithActionResult PrefixRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"name_prefix\": 123 }"));
	TestFalse(TEXT("name_prefix number should fail"), PrefixRes.bSuccess);

	// 2. name_suffix bad type
	FMonolithActionResult SuffixRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"name_suffix\": 123 }"));
	TestFalse(TEXT("name_suffix number should fail"), SuffixRes.bSuccess);

	// 3. search bad type
	FMonolithActionResult SearchRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"search\": 123 }"));
	TestFalse(TEXT("search number should fail"), SearchRes.bSuccess);

	// 4. replace bad type
	FMonolithActionResult ReplaceRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"replace\": 123 }"));
	TestFalse(TEXT("replace number should fail"), ReplaceRes.bSuccess);

	// 5. include_referenced bad type
	FMonolithActionResult IncludeRefRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"include_referenced\": 123 }"));
	TestFalse(TEXT("include_referenced number should fail"), IncludeRefRes.bSuccess);

	// 6. overwrite bad type
	FMonolithActionResult OverwriteRes = Execute(TEXT("{ \"retargeter_path\": \"/Game/dummy\", \"source_anims\": [\"/Game/dummy\"], \"output_folder\": \"/Game/dummy\", \"overwrite\": 123 }"));
	TestFalse(TEXT("overwrite number should fail"), OverwriteRes.bSuccess);

	return true;
}
