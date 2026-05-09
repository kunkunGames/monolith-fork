#include "Misc/AutomationTest.h"
#include "MonolithMeshInspectionActions.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMeshSearchMeshesBySizeLimitTest, "Monolith.LimitGuard.MonolithMesh.SearchMeshesBySizeClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithMeshSearchMeshesBySizeLimitTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Negative search_meshes_by_size limit clamps to 1"), FMonolithMeshInspectionActions::ClampSearchMeshesBySizeLimit(-10), 1);
	TestEqual(TEXT("Zero search_meshes_by_size limit clamps to 1"), FMonolithMeshInspectionActions::ClampSearchMeshesBySizeLimit(0), 1);
	TestEqual(TEXT("In-range search_meshes_by_size limit is preserved"), FMonolithMeshInspectionActions::ClampSearchMeshesBySizeLimit(20), 20);
	TestEqual(TEXT("Oversized search_meshes_by_size limit clamps to 1000"), FMonolithMeshInspectionActions::ClampSearchMeshesBySizeLimit(50000), 1000);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
