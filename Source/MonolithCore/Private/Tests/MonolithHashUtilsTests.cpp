#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithHashUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSha256FipsVectorsTest,
	"Monolith.Core.Hash.Sha256FipsVectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSha256FipsVectorsTest::RunTest(const FString& Parameters)
{
	FString Actual;
	TestTrue(TEXT("Empty input hashes successfully"),
		FMonolithHashUtils::TrySha256Bytes(TConstArrayView<uint8>(), Actual));
	TestEqual(TEXT("Empty input digest"), Actual,
		TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

	TestTrue(TEXT("UTF-8 text hashes successfully"), FMonolithHashUtils::TrySha256Text(TEXT("abc"), Actual));
	TestEqual(TEXT("abc digest"), Actual,
		TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

	struct FRepeatedByteVector
	{
		int32 Count;
		const TCHAR* Expected;
	};

	const FRepeatedByteVector BoundaryVectors[] = {
		{ 55, TEXT("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318") },
		{ 56, TEXT("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a") },
		{ 64, TEXT("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb") },
		{ 1000000, TEXT("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") }
	};

	for (const FRepeatedByteVector& Vector : BoundaryVectors)
	{
		TArray<uint8> Bytes;
		Bytes.Init(static_cast<uint8>('a'), Vector.Count);
		const FString Label = FString::Printf(TEXT("%d repeated bytes"), Vector.Count);
		TestTrue(Label + TEXT(" hashes successfully"),
			FMonolithHashUtils::TrySha256Bytes(MakeArrayView(Bytes), Actual));
		TestEqual(Label + TEXT(" digest"), Actual, FString(Vector.Expected));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
