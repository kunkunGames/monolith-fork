#include "Misc/AutomationTest.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithNiagaraActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraCrashguardPathTest, "Monolith.Crashguard.Niagara.ValidatePackagePath", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraCrashguardPathTest::RunTest(const FString& Parameters)
{
    // Test comprehensively evaluating multiple edge cases:
    // empty path, double-slash path, missing /Game/ root, trailing slash, illegal characters

    TestFalse(TEXT("Empty path should be caught"), MonolithCore::ValidatePackagePath(TEXT("")).IsEmpty());
    TestFalse(TEXT("Double-slash path should be caught"), MonolithCore::ValidatePackagePath(TEXT("//Game/Niagara/MySystem")).IsEmpty());
    TestFalse(TEXT("Missing /Game/ root should be caught"), MonolithCore::ValidatePackagePath(TEXT("Engine/Niagara/MySystem")).IsEmpty());
    TestFalse(TEXT("Trailing slash should be caught"), MonolithCore::ValidatePackagePath(TEXT("/Game/Niagara/MySystem/")).IsEmpty());
    TestFalse(TEXT("Illegal characters should be caught"), MonolithCore::ValidatePackagePath(TEXT("/Game/Niagara/MySystem#1")).IsEmpty());

    TestTrue(TEXT("Valid path should pass"), MonolithCore::ValidatePackagePath(TEXT("/Game/Niagara/MySystem")).IsEmpty());

    return true;
}
