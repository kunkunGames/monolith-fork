#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithEditorSelectionActions.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorSelectionClassFilterTest,
	"Monolith.Editor.Selection.ClassFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSelectionClassFilterTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("empty filter matches actor class"),
		FMonolithEditorSelectionActions::MatchesClassFilter(AStaticMeshActor::StaticClass(), TEXT("")));
	TestTrue(TEXT("short class name matches actor class"),
		FMonolithEditorSelectionActions::MatchesClassFilter(AStaticMeshActor::StaticClass(), TEXT("StaticMeshActor")));
	TestTrue(TEXT("full class path matches actor class"),
		FMonolithEditorSelectionActions::MatchesClassFilter(AStaticMeshActor::StaticClass(), TEXT("/Script/Engine.StaticMeshActor")));
	TestTrue(TEXT("base class short name matches actor class"),
		FMonolithEditorSelectionActions::MatchesClassFilter(AStaticMeshActor::StaticClass(), TEXT("Actor")));
	TestTrue(TEXT("base class full path matches actor class"),
		FMonolithEditorSelectionActions::MatchesClassFilter(AStaticMeshActor::StaticClass(), TEXT("/Script/Engine.Actor")));
	TestFalse(TEXT("different short class does not match actor class"),
		FMonolithEditorSelectionActions::MatchesClassFilter(AStaticMeshActor::StaticClass(), TEXT("PointLight")));

	TestTrue(TEXT("short class name matches component class"),
		FMonolithEditorSelectionActions::MatchesClassFilter(UStaticMeshComponent::StaticClass(), TEXT("StaticMeshComponent")));
	TestTrue(TEXT("full class path matches component class"),
		FMonolithEditorSelectionActions::MatchesClassFilter(UStaticMeshComponent::StaticClass(), TEXT("/Script/Engine.StaticMeshComponent")));
	TestTrue(TEXT("base class short name matches component class"),
		FMonolithEditorSelectionActions::MatchesClassFilter(UStaticMeshComponent::StaticClass(), TEXT("ActorComponent")));
	TestFalse(TEXT("null class only matches empty filter"),
		FMonolithEditorSelectionActions::MatchesClassFilter(nullptr, TEXT("StaticMeshActor")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
