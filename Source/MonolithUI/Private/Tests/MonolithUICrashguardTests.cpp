#include "Misc/AutomationTest.h"
#include "MonolithUIInternal.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithCrashguardUICreatePackagePathTest, "Monolith.Crashguard.UI.CreatePackagePathValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithCrashguardUICreatePackagePathTest::RunTest(const FString& Parameters)
{
    FMonolithActionResult OutError;
    UWidgetBlueprint* WBP = MonolithUIInternal::CreateNewWidgetBlueprint(TEXT("//Game/Malformed"), OutError);
    TestNull(TEXT("CreateNewWidgetBlueprint should return null for malformed path"), WBP);
    TestFalse(TEXT("OutError should indicate failure"), OutError.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithCrashguardUITemplateCreatePackagePathTest, "Monolith.Crashguard.UI.TemplateCreatePackagePathValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithCrashguardUITemplateCreatePackagePathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("save_path"), TEXT("//Game/Malformed"));
    Params->SetStringField(TEXT("title_text"), TEXT("Test Title"));

    FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("scaffold_main_menu"), Params);

    TestFalse(TEXT("scaffold_main_menu should fail with malformed path"), Result.bSuccess);
    return true;
}
