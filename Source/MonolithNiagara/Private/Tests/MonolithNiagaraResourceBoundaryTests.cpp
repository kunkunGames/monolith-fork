#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraListSystemsLimitTest, "Monolith.LimitGuard.MonolithNiagara.ListSystemsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithNiagaraListSystemsLimitTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Negative list_systems limit clamps to 1"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(-10), 1);
	TestEqual(TEXT("Zero list_systems limit clamps to 1"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(0), 1);
	TestEqual(TEXT("In-range list_systems limit is preserved"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(200), 200);
	TestEqual(TEXT("Oversized list_systems limit clamps to 1000"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(50000), 1000);

	TestEqual(TEXT("Negative search_dynamic_inputs limit clamps to 1"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(-10), 1);
	TestEqual(TEXT("Zero search_dynamic_inputs limit clamps to 1"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(0), 1);
	TestEqual(TEXT("In-range search_dynamic_inputs limit is preserved"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(200), 200);
	TestEqual(TEXT("Oversized search_dynamic_inputs limit clamps to 1000"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(50000), 1000);

	TestEqual(TEXT("Negative list_module_scripts limit clamps to 1"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(-10), 1);
	TestEqual(TEXT("Zero list_module_scripts limit clamps to 1"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(0), 1);
	TestEqual(TEXT("In-range list_module_scripts limit is preserved"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(200), 200);
	TestEqual(TEXT("Oversized list_module_scripts limit clamps to 1000"), FMonolithNiagaraActions::ClampNiagaraQueryLimit(50000), 1000);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
