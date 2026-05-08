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
