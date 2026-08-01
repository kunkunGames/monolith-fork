// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithPinTypeGrammarTest.cpp
//
// Table test for the shared pin-type grammar (MonolithCore/Public/MonolithPinTypeGrammar.h).
//
// WHY IT LIVES IN MonolithBlueprint AND NOT MonolithCore:
//   MonolithCore.Build.cs lists no BlueprintGraph, and the UEdGraphSchema_K2::PC_*
//   constants are dllimport'd from that module — referencing them from a MonolithCore
//   translation unit is an LNK2019. The grammar header is header-only inline for
//   exactly that reason; its tests have to live in a module that links BlueprintGraph.
//
// WHAT IT LOCKS:
//   1. enum: tokens produce PC_Byte + the UEnum as PinSubCategoryObject — NEVER
//      PC_Enum. PC_Enum is a type-picker category with no branch in
//      FKismetCompilerUtilities::CreatePropertyOnScope, so it falls through to the
//      generic FIntProperty fallback and every Get/Set pin comes out an int
//      (issue #115).
//   2. Enum resolution covers a native short name, a full /Script/... object path
//      (which never worked before — FindFirstObject converts its whole argument into
//      a single FName) and a UUserDefinedEnum asset by short name and by path.
//   3. TryParsePinType fails BY TOKEN. This is the property that let the UI action's
//      hand-rolled "does this category want a sub-object?" guard be deleted: that
//      guard listed PC_Enum but not PC_Byte, so the enum fix alone would have made it
//      go dead and an unresolvable enum: would have silently produced a plain byte
//      variable. Failure keyed on the token cannot drift that way, and it reaches
//      into container value types the guard never inspected.
//
// The disposable UserDefinedEnum fixture is created in memory and trashed at the end.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithPinTypeGrammar.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/UserDefinedEnum.h"
#include "Kismet2/EnumEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

namespace MonolithPinTypeGrammarTest
{
	static const TCHAR* const FixtureEnumPackage = TEXT("/Game/Tests/Monolith/pintype/E_MonolithPinGrammarFixture");
	static const TCHAR* const FixtureEnumName    = TEXT("E_MonolithPinGrammarFixture");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPinTypeGrammarTokenTableTest,
	"Monolith.PinTypeGrammar.TokenTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPinTypeGrammarTokenTableTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPinTypeGrammarTest;

	// --- Disposable UUserDefinedEnum fixture (ECppForm::Namespaced -> FByteProperty) --
	UPackage* FixturePkg = CreatePackage(FixtureEnumPackage);
	if (!TestNotNull(TEXT("Fixture package created"), FixturePkg))
	{
		return false;
	}
	UEnum* FixtureEnum = FEnumEditorUtils::CreateUserDefinedEnum(
		FixturePkg, FName(FixtureEnumName), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("Fixture UserDefinedEnum created"), FixtureEnum))
	{
		return false;
	}
	const FString FixtureShortName = FixtureEnum->GetName();
	const FString FixtureObjectPath = FixtureEnum->GetPathName();

	// --- Assertion helpers ----------------------------------------------------------
	auto ExpectOk = [this](const FString& TypeStr, const FName ExpectedCategory,
		const TCHAR* ExpectedSubObjectName, EPinContainerType ExpectedContainer)
	{
		FEdGraphPinType PinType;
		FString Error;
		const bool bParsed = MonolithPinTypeGrammar::TryParsePinType(TypeStr, PinType, Error);
		if (!TestTrue(FString::Printf(TEXT("'%s' parses (error: %s)"), *TypeStr, *Error), bParsed))
		{
			return;
		}
		TestEqual(*FString::Printf(TEXT("'%s' pin category"), *TypeStr),
			PinType.PinCategory, ExpectedCategory);
		TestEqual(*FString::Printf(TEXT("'%s' container type"), *TypeStr),
			static_cast<int32>(PinType.ContainerType), static_cast<int32>(ExpectedContainer));
		if (ExpectedSubObjectName)
		{
			if (TestTrue(FString::Printf(TEXT("'%s' has a sub-category object"), *TypeStr),
					PinType.PinSubCategoryObject.IsValid()))
			{
				TestEqual(*FString::Printf(TEXT("'%s' sub-category object name"), *TypeStr),
					PinType.PinSubCategoryObject->GetName(), FString(ExpectedSubObjectName));
			}
		}
	};

	auto ExpectFail = [this](const FString& TypeStr)
	{
		// Out must be left untouched on failure — seed it with a sentinel and check.
		const FName Sentinel(TEXT("MonolithUntouchedSentinel"));
		FEdGraphPinType PinType;
		PinType.PinCategory = Sentinel;
		FString Error;
		const bool bParsed = MonolithPinTypeGrammar::TryParsePinType(TypeStr, PinType, Error);
		TestFalse(FString::Printf(TEXT("'%s' is rejected"), *TypeStr), bParsed);
		TestFalse(FString::Printf(TEXT("'%s' rejection carries a reason"), *TypeStr), Error.IsEmpty());
		TestEqual(*FString::Printf(TEXT("'%s' leaves Out untouched"), *TypeStr),
			PinType.PinCategory, Sentinel);
	};

	// --- Success rows ---------------------------------------------------------------
	ExpectOk(TEXT("bool"), UEdGraphSchema_K2::PC_Boolean, nullptr, EPinContainerType::None);
	ExpectOk(TEXT("int"), UEdGraphSchema_K2::PC_Int, nullptr, EPinContainerType::None);
	ExpectOk(TEXT("byte"), UEdGraphSchema_K2::PC_Byte, nullptr, EPinContainerType::None);

	// The #115 rows. PC_Byte, never PC_Enum.
	ExpectOk(TEXT("enum:ESlateVisibility"), UEdGraphSchema_K2::PC_Byte,
		TEXT("ESlateVisibility"), EPinContainerType::None);
	ExpectOk(TEXT("enum:/Script/UMG.ESlateVisibility"), UEdGraphSchema_K2::PC_Byte,
		TEXT("ESlateVisibility"), EPinContainerType::None);
	// 'E'-prefix retry for callers that drop the conventional prefix.
	ExpectOk(TEXT("enum:SlateVisibility"), UEdGraphSchema_K2::PC_Byte,
		TEXT("ESlateVisibility"), EPinContainerType::None);
	ExpectOk(TEXT("enum:") + FixtureShortName, UEdGraphSchema_K2::PC_Byte,
		*FixtureShortName, EPinContainerType::None);
	ExpectOk(TEXT("enum:") + FixtureObjectPath, UEdGraphSchema_K2::PC_Byte,
		*FixtureShortName, EPinContainerType::None);
	ExpectOk(TEXT("array:enum:ESlateVisibility"), UEdGraphSchema_K2::PC_Byte,
		TEXT("ESlateVisibility"), EPinContainerType::Array);

	ExpectOk(TEXT("struct:Vector"), UEdGraphSchema_K2::PC_Struct,
		TEXT("Vector"), EPinContainerType::None);
	ExpectOk(TEXT("object:Actor"), UEdGraphSchema_K2::PC_Object,
		TEXT("Actor"), EPinContainerType::None);

	// --- Failure rows: unresolvable sub-objects fail BY TOKEN ------------------------
	ExpectFail(TEXT("enum:MonolithBogusEnumThatDoesNotExist"));
	ExpectFail(TEXT("struct:MonolithBogusStructThatDoesNotExist"));
	ExpectFail(TEXT("object:MonolithBogusClassThatDoesNotExist"));
	ExpectFail(TEXT("softobject:MonolithBogusClassThatDoesNotExist"));
	ExpectFail(TEXT("MonolithDefinitelyNotAPinType"));
	// Recursion into a container value type — the old category-shaped guards never
	// looked here, so this row is the one that proves the token contract is deeper.
	ExpectFail(TEXT("map:string:object:MonolithBogusClassThatDoesNotExist"));
	ExpectFail(TEXT("map:string"));

	// --- Legacy shape: still best-effort, but PC_Byte for enum: ----------------------
	// The remaining un-migrated call sites go through ParsePinTypeFromString. They must
	// not regress to PC_Enum, which is the whole of issue #115.
	{
		const FEdGraphPinType Legacy =
			MonolithPinTypeGrammar::ParsePinTypeFromString(TEXT("enum:ESlateVisibility"));
		TestEqual(TEXT("legacy parse of enum: is PC_Byte"),
			Legacy.PinCategory, FName(UEdGraphSchema_K2::PC_Byte));
		TestTrue(TEXT("legacy parse of enum: keeps the UEnum"),
			Cast<UEnum>(Legacy.PinSubCategoryObject.Get()) != nullptr);

		// Round-trip through the inverse.
		TestEqual(TEXT("enum: round-trips through PinTypeToString"),
			MonolithPinTypeGrammar::ContainerPrefix(Legacy) + MonolithPinTypeGrammar::PinTypeToString(Legacy),
			FString(TEXT("enum:ESlateVisibility")));
	}

	// --- Cleanup --------------------------------------------------------------------
	if (FixtureEnum)
	{
		FixtureEnum->ClearFlags(RF_Standalone | RF_Public);
		FixtureEnum->RemoveFromRoot();
		FixtureEnum->MarkAsGarbage();
	}
	if (FixturePkg)
	{
		FixturePkg->SetDirtyFlag(false);
		FixturePkg->ClearFlags(RF_Standalone);
		FixturePkg->RemoveFromRoot();
		FixturePkg->MarkAsGarbage();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
