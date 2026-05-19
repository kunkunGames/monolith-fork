#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMeshSecurityPathTest, "Monolith.Security.Mesh.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshSecurityPathTest::RunTest(const FString& Parameters)
{
	// Setup payload with double slash to simulate malformed path
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("save_path"), TEXT("//Game/MalformedPath/TestProxyMesh"));

	// Required fields for generate_proxy_mesh
	TArray<TSharedPtr<FJsonValue>> ActorNames;
	ActorNames.Add(MakeShared<FJsonValueString>(TEXT("TestActor1")));
	ActorNames.Add(MakeShared<FJsonValueString>(TEXT("TestActor2")));
	Payload->SetArrayField(TEXT("actor_names"), ActorNames);

	// Test generate_proxy_mesh
	FMonolithActionResult ProxyResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("generate_proxy_mesh"), Payload);

	// Verify it failed gracefully and returned the validation error
	TestFalse(TEXT("generate_proxy_mesh should fail on malformed path"), ProxyResult.bSuccess);
	TestTrue(TEXT("generate_proxy_mesh error should complain about invalid package path"), ProxyResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	// Setup payload for setup_hlod
	TSharedPtr<FJsonObject> HlodPayload = MakeShared<FJsonObject>();
	HlodPayload->SetStringField(TEXT("save_path"), TEXT("//Game/MalformedPath/TestHLOD"));
	HlodPayload->SetStringField(TEXT("layer_type"), TEXT("MeshMerge"));

	// Test setup_hlod
	FMonolithActionResult HlodResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("setup_hlod"), HlodPayload);

	// Verify it failed gracefully and returned the validation error
	TestFalse(TEXT("setup_hlod should fail on malformed path"), HlodResult.bSuccess);
	TestTrue(TEXT("setup_hlod error should complain about invalid package path"), HlodResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
