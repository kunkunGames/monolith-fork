// SPDX-License-Identifier: MIT
// FMonolithReflectionWalker automation tests.

#include "Misc/AutomationTest.h"

#include "MonolithBulkFillTypes.h"
#include "MonolithReflectionWalkerTestTypes.h"
#include "Reflection/MonolithDryRunGuard.h"
#include "Reflection/MonolithReflectionWalker.h"

#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FBulkFillSpec MakeSpec(bool bStrict = false)
	{
		FBulkFillSpec Spec;
		Spec.TargetNamespace = TEXT("reflection_test");
		Spec.TargetAsset = TEXT("/Temp/ReflectionWalkerTest");
		Spec.bStrict = bStrict;
		return Spec;
	}

	TSharedPtr<FJsonValue> JsonString(const FString& Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}

	TSharedPtr<FJsonValue> JsonNumber(double Value)
	{
		return MakeShared<FJsonValueNumber>(Value);
	}

	TSharedPtr<FJsonValue> JsonNull()
	{
		return MakeShared<FJsonValueNull>();
	}

	TSharedPtr<FJsonValue> JsonObjectValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedPtr<FJsonValue> JsonArrayValue(const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		return MakeShared<FJsonValueArray>(Values);
	}

	TSharedPtr<FJsonObject> MakeVectorObject(double X, double Y, double Z)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("X"), X);
		Object->SetNumberField(TEXT("Y"), Y);
		Object->SetNumberField(TEXT("Z"), Z);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeNestedObject(int32 Count, const FString& Label)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("NestedCount"), Count);
		Object->SetStringField(TEXT("NestedLabel"), Label);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeMapObject()
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("Alpha"), TEXT("One"));
		Object->SetStringField(TEXT("Beta"), TEXT("Two"));
		return Object;
	}

	TSharedPtr<FJsonObject> MakeFullWriteTree()
	{
		UEnum* Enum = StaticEnum<EMonolithReflectionWalkerTestEnum>();
		const FString HeavyToken = Enum
			? Enum->GetNameStringByValue(static_cast<int64>(EMonolithReflectionWalkerTestEnum::Heavy))
			: TEXT("Heavy");

		TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
		Tree->SetNumberField(TEXT("IntValue"), 42);
		Tree->SetNumberField(TEXT("FloatValue"), 3.5);
		Tree->SetStringField(TEXT("NameValue"), TEXT("Hero"));
		Tree->SetStringField(TEXT("StringValue"), TEXT("Ready"));
		Tree->SetObjectField(TEXT("VectorValue"), MakeVectorObject(1.0, 2.0, 3.0));
		Tree->SetArrayField(TEXT("IntArray"), { JsonNumber(1), JsonNumber(2), JsonNumber(3) });
		Tree->SetObjectField(TEXT("NameMap"), MakeMapObject());
		Tree->SetArrayField(TEXT("NameSet"), { JsonString(TEXT("One")), JsonString(TEXT("Two")) });
		Tree->SetStringField(TEXT("SoftTexture"), TEXT("Texture2D'/Engine/EngineResources/DefaultTexture.DefaultTexture'"));
		Tree->SetStringField(TEXT("EnumValue"), HeavyToken);
		Tree->SetObjectField(TEXT("Nested"), MakeNestedObject(7, TEXT("Inner")));
		return Tree;
	}

	TSharedPtr<FJsonObject> MakeClassReferenceTree(const FString& ClassPath)
	{
		TSharedPtr<FJsonObject> HardMap = MakeShared<FJsonObject>();
		HardMap->SetStringField(TEXT("Entry"), ClassPath);
		TSharedPtr<FJsonObject> SoftMap = MakeShared<FJsonObject>();
		SoftMap->SetStringField(TEXT("Entry"), ClassPath);

		TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
		Nested->SetStringField(TEXT("HardActorClass"), ClassPath);
		Nested->SetStringField(TEXT("NestedLabel"), TEXT("ChangedByRequest"));
		Nested->SetStringField(TEXT("SoftActorClass"), ClassPath);

		TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
		Tree->SetStringField(TEXT("HardActorClass"), ClassPath);
		Tree->SetStringField(TEXT("SoftActorClass"), ClassPath);
		Tree->SetArrayField(TEXT("HardActorClassArray"), {JsonString(ClassPath)});
		Tree->SetArrayField(TEXT("SoftActorClassArray"), {JsonString(ClassPath)});
		Tree->SetObjectField(TEXT("HardActorClassMap"), HardMap);
		Tree->SetObjectField(TEXT("SoftActorClassMap"), SoftMap);
		Tree->SetArrayField(TEXT("HardActorClassSet"), {JsonString(ClassPath)});
		Tree->SetArrayField(TEXT("SoftActorClassSet"), {JsonString(ClassPath)});
		Tree->SetObjectField(TEXT("Nested"), Nested);
		return Tree;
	}

	TSharedPtr<FJsonObject> MakeNullClassReferenceTree()
	{
		TSharedPtr<FJsonObject> HardMap = MakeShared<FJsonObject>();
		HardMap->SetField(TEXT("Entry"), JsonNull());
		TSharedPtr<FJsonObject> SoftMap = MakeShared<FJsonObject>();
		SoftMap->SetField(TEXT("Entry"), JsonNull());

		TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
		Nested->SetField(TEXT("HardActorClass"), JsonNull());
		Nested->SetField(TEXT("SoftActorClass"), JsonNull());

		TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
		Tree->SetField(TEXT("HardActorClass"), JsonNull());
		Tree->SetField(TEXT("SoftActorClass"), JsonNull());
		Tree->SetArrayField(TEXT("HardActorClassArray"), {JsonNull()});
		Tree->SetArrayField(TEXT("SoftActorClassArray"), {JsonNull()});
		Tree->SetObjectField(TEXT("HardActorClassMap"), HardMap);
		Tree->SetObjectField(TEXT("SoftActorClassMap"), SoftMap);
		Tree->SetArrayField(TEXT("HardActorClassSet"), {JsonNull()});
		Tree->SetArrayField(TEXT("SoftActorClassSet"), {JsonNull()});
		Tree->SetObjectField(TEXT("Nested"), Nested);
		return Tree;
	}

	const FBulkFillFieldWrite* FindWrite(const FDryRunReport& Report, const FString& Path)
	{
		return Report.FieldWrites.FindByPredicate(
			[&Path](const FBulkFillFieldWrite& Write)
			{
				return Write.Path == Path;
			});
	}

	bool AllWritesOk(const FDryRunReport& Report)
	{
		for (const FBulkFillFieldWrite& Write : Report.FieldWrites)
		{
			if (!Write.bOk)
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerScalarsAndContainersTest,
	"Leviathan.Monolith.Reflection.WalkerWritesAllScalarsAndContainers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerScalarsAndContainersTest::RunTest(const FString& /*Parameters*/)
{
	UMonolithReflectionWalkerTestObject* Object = NewObject<UMonolithReflectionWalkerTestObject>();
	TestNotNull(TEXT("fixture object"), Object);

	const FDryRunReport Report = FMonolithReflectionWalker::WriteTree(
		MakeFullWriteTree(),
		UMonolithReflectionWalkerTestObject::StaticClass(),
		Object,
		Object,
		MakeSpec(false));

	TestTrue(TEXT("committed write reports would_apply"), Report.bWouldApply);
	TestEqual(TEXT("no write errors"), Report.Errors, 0);
	TestTrue(TEXT("all field writes accepted"), AllWritesOk(Report));
	TestTrue(TEXT("has top-level writes"), Report.FieldWrites.Num() >= 11);

	TestEqual(TEXT("IntValue"), Object->IntValue, 42);
	TestEqual(TEXT("FloatValue"), Object->FloatValue, 3.5f);
	TestEqual(TEXT("NameValue"), Object->NameValue, FName(TEXT("Hero")));
	TestEqual(TEXT("StringValue"), Object->StringValue, FString(TEXT("Ready")));
	TestEqual(TEXT("VectorValue"), Object->VectorValue, FVector(1.0, 2.0, 3.0));
	TestEqual(TEXT("IntArray count"), Object->IntArray.Num(), 3);
	TestEqual(TEXT("IntArray[2]"), Object->IntArray.IsValidIndex(2) ? Object->IntArray[2] : INDEX_NONE, 3);
	TestEqual(TEXT("NameMap Alpha"), Object->NameMap.FindRef(FName(TEXT("Alpha"))), FString(TEXT("One")));
	TestTrue(TEXT("NameSet contains Two"), Object->NameSet.Contains(FName(TEXT("Two"))));
	TestTrue(TEXT("SoftTexture path accepted"), Object->SoftTexture.ToSoftObjectPath().ToString().Contains(TEXT("/Engine/EngineResources/DefaultTexture")));
	TestEqual(TEXT("EnumValue"), Object->EnumValue, EMonolithReflectionWalkerTestEnum::Heavy);
	TestEqual(TEXT("NestedCount"), Object->Nested.NestedCount, 7);
	TestEqual(TEXT("NestedLabel"), Object->Nested.NestedLabel, FString(TEXT("Inner")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerRejectsTypeCoerceTrapTest,
	"Leviathan.Monolith.Reflection.WalkerRejectsTypeCoerceTrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerRejectsTypeCoerceTrapTest::RunTest(const FString& /*Parameters*/)
{
	UMonolithReflectionWalkerTestObject* Object = NewObject<UMonolithReflectionWalkerTestObject>();
	Object->StringValue = TEXT("Original");

	TSharedPtr<FJsonObject> BadStringObject = MakeShared<FJsonObject>();
	BadStringObject->SetStringField(TEXT("unexpected"), TEXT("object"));

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetObjectField(TEXT("StringValue"), BadStringObject);

	const FDryRunReport Report = FMonolithReflectionWalker::WriteTree(
		Tree,
		UMonolithReflectionWalkerTestObject::StaticClass(),
		Object,
		Object,
		MakeSpec(false));

	const FBulkFillFieldWrite* Write = FindWrite(Report, TEXT("StringValue"));
	TestNotNull(TEXT("StringValue write exists"), Write);
	if (Write)
	{
		TestFalse(TEXT("object is rejected for scalar field"), Write->bOk);
		TestTrue(TEXT("reason explains scalar expectation"), Write->Reason.Contains(TEXT("expected scalar")));
	}
	TestEqual(TEXT("StringValue remains unchanged"), Object->StringValue, FString(TEXT("Original")));
	TestEqual(TEXT("one error"), Report.Errors, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerStrictModeBlocksUnknownKeyTest,
	"Leviathan.Monolith.Reflection.WalkerStrictModeBlocksUnknownKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerStrictModeBlocksUnknownKeyTest::RunTest(const FString& /*Parameters*/)
{
	UMonolithReflectionWalkerTestObject* Object = NewObject<UMonolithReflectionWalkerTestObject>();

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetNumberField(TEXT("NotAFieldOnThisStruct"), 42);

	const FDryRunReport PermissiveReport = FMonolithReflectionWalker::WriteTree(
		Tree,
		UMonolithReflectionWalkerTestObject::StaticClass(),
		Object,
		Object,
		MakeSpec(false));
	TestEqual(TEXT("permissive unknown key error count"), PermissiveReport.Errors, 1);
	TestTrue(TEXT("permissive mode still reports would_apply"), PermissiveReport.bWouldApply);

	const FDryRunReport StrictReport = FMonolithReflectionWalker::WriteTree(
		Tree,
		UMonolithReflectionWalkerTestObject::StaticClass(),
		Object,
		Object,
		MakeSpec(true));
	TestEqual(TEXT("strict unknown key error count"), StrictReport.Errors, 1);
	TestFalse(TEXT("strict mode blocks apply"), StrictReport.bWouldApply);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerEnumMissReportsTypoTest,
	"Leviathan.Monolith.Reflection.WalkerEnumMissReportsTypo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerEnumMissReportsTypoTest::RunTest(const FString& /*Parameters*/)
{
	UMonolithReflectionWalkerTestObject* Object = NewObject<UMonolithReflectionWalkerTestObject>();
	Object->EnumValue = EMonolithReflectionWalkerTestEnum::Light;

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetStringField(TEXT("EnumValue"), TEXT("Constnt"));

	const FDryRunReport Report = FMonolithReflectionWalker::WriteTree(
		Tree,
		UMonolithReflectionWalkerTestObject::StaticClass(),
		Object,
		Object,
		MakeSpec(false));

	const FBulkFillFieldWrite* Write = FindWrite(Report, TEXT("EnumValue"));
	TestNotNull(TEXT("EnumValue write exists"), Write);
	if (Write)
	{
		TestFalse(TEXT("typo enum token rejected"), Write->bOk);
		TestTrue(TEXT("reason reports enum miss"), Write->Reason.Contains(TEXT("not found")));
	}
	TestEqual(TEXT("EnumValue remains unchanged"), Object->EnumValue, EMonolithReflectionWalkerTestEnum::Light);
	TestEqual(TEXT("one enum error"), Report.Errors, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerDryRunNoSideEffectsTest,
	"Leviathan.Monolith.Reflection.DryRunNoSideEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerDryRunNoSideEffectsTest::RunTest(const FString& /*Parameters*/)
{
	UMonolithReflectionWalkerTestObject* Object = NewObject<UMonolithReflectionWalkerTestObject>();
	Object->IntValue = 5;
	Object->StringValue = TEXT("Before");
	Object->VectorValue = FVector(9.0, 8.0, 7.0);
	Object->IntArray = { 9, 8 };
	Object->NameMap.Add(FName(TEXT("Before")), TEXT("Value"));
	Object->NameSet.Add(FName(TEXT("Before")));
	Object->EnumValue = EMonolithReflectionWalkerTestEnum::Light;
	Object->Nested.NestedCount = 1;
	Object->Nested.NestedLabel = TEXT("BeforeNested");

	const FDryRunReport Report = FMonolithReflectionWalker::InspectTree(
		MakeFullWriteTree(),
		UMonolithReflectionWalkerTestObject::StaticClass(),
		Object,
		MakeSpec(false));

	TestFalse(TEXT("dry-run never applies"), Report.bWouldApply);
	TestEqual(TEXT("dry-run no write errors"), Report.Errors, 0);
	TestTrue(TEXT("dry-run reports accepted writes"), AllWritesOk(Report));

	TestEqual(TEXT("IntValue unchanged"), Object->IntValue, 5);
	TestEqual(TEXT("StringValue unchanged"), Object->StringValue, FString(TEXT("Before")));
	TestEqual(TEXT("VectorValue unchanged"), Object->VectorValue, FVector(9.0, 8.0, 7.0));
	TestEqual(TEXT("IntArray count unchanged"), Object->IntArray.Num(), 2);
	TestEqual(TEXT("IntArray[0] unchanged"), Object->IntArray.IsValidIndex(0) ? Object->IntArray[0] : INDEX_NONE, 9);
	TestEqual(TEXT("NameMap unchanged"), Object->NameMap.FindRef(FName(TEXT("Before"))), FString(TEXT("Value")));
	TestTrue(TEXT("NameSet unchanged"), Object->NameSet.Contains(FName(TEXT("Before"))));
	TestEqual(TEXT("EnumValue unchanged"), Object->EnumValue, EMonolithReflectionWalkerTestEnum::Light);
	TestEqual(TEXT("NestedCount unchanged"), Object->Nested.NestedCount, 1);
	TestEqual(TEXT("NestedLabel unchanged"), Object->Nested.NestedLabel, FString(TEXT("BeforeNested")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerClassMetaRejectsWithoutMutationTest,
	"Leviathan.Monolith.Reflection.ClassMetaConstraintsRejectWithoutMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerClassMetaRejectsWithoutMutationTest::RunTest(const FString& /*Parameters*/)
{
	UMonolithReflectionWalkerTestObject* Object = NewObject<UMonolithReflectionWalkerTestObject>();
	TestNotNull(TEXT("fixture object"), Object);
	if (!Object)
	{
		return false;
	}

	const TSoftClassPtr<AActor> OriginalSoftClass(AActor::StaticClass());
	Object->HardActorClass = AActor::StaticClass();
	Object->SoftActorClass = OriginalSoftClass;
	Object->HardActorClassArray = {AActor::StaticClass()};
	Object->SoftActorClassArray = {OriginalSoftClass};
	Object->HardActorClassMap.Add(FName(TEXT("Original")), AActor::StaticClass());
	Object->SoftActorClassMap.Add(FName(TEXT("Original")), OriginalSoftClass);
	Object->HardActorClassSet.Add(AActor::StaticClass());
	Object->SoftActorClassSet.Add(OriginalSoftClass);
	Object->Nested.HardActorClass = AActor::StaticClass();
	Object->Nested.SoftActorClass = OriginalSoftClass;
	Object->Nested.NestedLabel = TEXT("OriginalNested");

	const FDryRunReport Report = FMonolithReflectionWalker::WriteTree(
		MakeClassReferenceTree(UTexture2D::StaticClass()->GetPathName()),
		UMonolithReflectionWalkerTestObject::StaticClass(), Object, Object, MakeSpec(true));

	TestTrue(TEXT("incompatible hard/soft class paths report errors"), Report.Errors > 0);
	TestFalse(TEXT("strict class errors block apply"), Report.bWouldApply);
	TestFalse(TEXT("at least one class write is rejected"), AllWritesOk(Report));

	TestEqual(TEXT("direct hard class unchanged"), Object->HardActorClass.Get(), AActor::StaticClass());
	TestEqual(TEXT("direct soft class unchanged"), Object->SoftActorClass.Get(), AActor::StaticClass());
	TestEqual(TEXT("hard class array count unchanged"), Object->HardActorClassArray.Num(), 1);
	TestEqual(TEXT("hard class array value unchanged"), Object->HardActorClassArray[0].Get(), AActor::StaticClass());
	TestEqual(TEXT("soft class array count unchanged"), Object->SoftActorClassArray.Num(), 1);
	TestEqual(TEXT("soft class array value unchanged"), Object->SoftActorClassArray[0].Get(), AActor::StaticClass());

	TestEqual(TEXT("hard class map count unchanged"), Object->HardActorClassMap.Num(), 1);
	const TSubclassOf<AActor>* HardMapValue = Object->HardActorClassMap.Find(FName(TEXT("Original")));
	TestNotNull(TEXT("hard class map original entry remains"), HardMapValue);
	if (HardMapValue)
	{
		TestEqual(TEXT("hard class map value unchanged"), HardMapValue->Get(), AActor::StaticClass());
	}
	TestFalse(TEXT("hard class map rejected entry absent"), Object->HardActorClassMap.Contains(FName(TEXT("Entry"))));

	TestEqual(TEXT("soft class map count unchanged"), Object->SoftActorClassMap.Num(), 1);
	const TSoftClassPtr<AActor>* SoftMapValue = Object->SoftActorClassMap.Find(FName(TEXT("Original")));
	TestNotNull(TEXT("soft class map original entry remains"), SoftMapValue);
	if (SoftMapValue)
	{
		TestEqual(TEXT("soft class map value unchanged"), SoftMapValue->Get(), AActor::StaticClass());
	}
	TestFalse(TEXT("soft class map rejected entry absent"), Object->SoftActorClassMap.Contains(FName(TEXT("Entry"))));

	TestEqual(TEXT("hard class set count unchanged"), Object->HardActorClassSet.Num(), 1);
	for (const TSubclassOf<AActor>& ClassValue : Object->HardActorClassSet)
	{
		TestEqual(TEXT("hard class set value unchanged"), ClassValue.Get(), AActor::StaticClass());
	}
	TestEqual(TEXT("soft class set count unchanged"), Object->SoftActorClassSet.Num(), 1);
	for (const TSoftClassPtr<AActor>& ClassValue : Object->SoftActorClassSet)
	{
		TestEqual(TEXT("soft class set value unchanged"), ClassValue.Get(), AActor::StaticClass());
	}

	TestEqual(TEXT("nested hard class unchanged"), Object->Nested.HardActorClass.Get(), AActor::StaticClass());
	TestEqual(TEXT("nested soft class unchanged"), Object->Nested.SoftActorClass.Get(), AActor::StaticClass());
	TestEqual(TEXT("nested sibling write is rolled back"), Object->Nested.NestedLabel, FString(TEXT("OriginalNested")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerClassMetaAcceptsValidAndNullTest,
	"Leviathan.Monolith.Reflection.ClassMetaConstraintsAcceptValidAndNull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerClassMetaAcceptsValidAndNullTest::RunTest(const FString& /*Parameters*/)
{
	UMonolithReflectionWalkerTestObject* Object = NewObject<UMonolithReflectionWalkerTestObject>();
	TestNotNull(TEXT("fixture object"), Object);
	if (!Object)
	{
		return false;
	}

	const FDryRunReport ValidReport = FMonolithReflectionWalker::WriteTree(
		MakeClassReferenceTree(AActor::StaticClass()->GetPathName()),
		UMonolithReflectionWalkerTestObject::StaticClass(), Object, Object, MakeSpec(true));

	TestEqual(TEXT("valid class write errors"), ValidReport.Errors, 0);
	TestTrue(TEXT("valid class writes apply"), ValidReport.bWouldApply);
	TestTrue(TEXT("all valid class writes accepted"), AllWritesOk(ValidReport));
	TestEqual(TEXT("direct hard class set"), Object->HardActorClass.Get(), AActor::StaticClass());
	TestEqual(TEXT("direct soft class set"), Object->SoftActorClass.Get(), AActor::StaticClass());
	TestEqual(TEXT("hard class array set"), Object->HardActorClassArray[0].Get(), AActor::StaticClass());
	TestEqual(TEXT("soft class array set"), Object->SoftActorClassArray[0].Get(), AActor::StaticClass());
	const TSubclassOf<AActor>* HardMapValue = Object->HardActorClassMap.Find(FName(TEXT("Entry")));
	TestNotNull(TEXT("valid hard class map entry"), HardMapValue);
	if (HardMapValue)
	{
		TestEqual(TEXT("valid hard class map value"), HardMapValue->Get(), AActor::StaticClass());
	}
	const TSoftClassPtr<AActor>* SoftMapValue = Object->SoftActorClassMap.Find(FName(TEXT("Entry")));
	TestNotNull(TEXT("valid soft class map entry"), SoftMapValue);
	if (SoftMapValue)
	{
		TestEqual(TEXT("valid soft class map value"), SoftMapValue->Get(), AActor::StaticClass());
	}
	for (const TSubclassOf<AActor>& ClassValue : Object->HardActorClassSet)
	{
		TestEqual(TEXT("valid hard class set value"), ClassValue.Get(), AActor::StaticClass());
	}
	for (const TSoftClassPtr<AActor>& ClassValue : Object->SoftActorClassSet)
	{
		TestEqual(TEXT("valid soft class set value"), ClassValue.Get(), AActor::StaticClass());
	}
	TestEqual(TEXT("valid nested hard class"), Object->Nested.HardActorClass.Get(), AActor::StaticClass());
	TestEqual(TEXT("valid nested soft class"), Object->Nested.SoftActorClass.Get(), AActor::StaticClass());

	const FDryRunReport NullReport = FMonolithReflectionWalker::WriteTree(
		MakeNullClassReferenceTree(), UMonolithReflectionWalkerTestObject::StaticClass(), Object, Object,
		MakeSpec(true));

	TestEqual(TEXT("null class write errors"), NullReport.Errors, 0);
	TestTrue(TEXT("null class writes apply"), NullReport.bWouldApply);
	TestTrue(TEXT("all null class writes accepted"), AllWritesOk(NullReport));
	TestNull(TEXT("direct hard class cleared"), Object->HardActorClass.Get());
	TestTrue(TEXT("direct soft class cleared"), Object->SoftActorClass.IsNull());
	TestEqual(TEXT("null hard class array count"), Object->HardActorClassArray.Num(), 1);
	TestNull(TEXT("null hard class array element"), Object->HardActorClassArray[0].Get());
	TestEqual(TEXT("null soft class array count"), Object->SoftActorClassArray.Num(), 1);
	TestTrue(TEXT("null soft class array element"), Object->SoftActorClassArray[0].IsNull());
	HardMapValue = Object->HardActorClassMap.Find(FName(TEXT("Entry")));
	TestNotNull(TEXT("null hard class map entry exists"), HardMapValue);
	if (HardMapValue)
	{
		TestNull(TEXT("null hard class map value"), HardMapValue->Get());
	}
	SoftMapValue = Object->SoftActorClassMap.Find(FName(TEXT("Entry")));
	TestNotNull(TEXT("null soft class map entry exists"), SoftMapValue);
	if (SoftMapValue)
	{
		TestTrue(TEXT("null soft class map value"), SoftMapValue->IsNull());
	}
	TestEqual(TEXT("null hard class set count"), Object->HardActorClassSet.Num(), 1);
	for (const TSubclassOf<AActor>& ClassValue : Object->HardActorClassSet)
	{
		TestNull(TEXT("null hard class set value"), ClassValue.Get());
	}
	TestEqual(TEXT("null soft class set count"), Object->SoftActorClassSet.Num(), 1);
	for (const TSoftClassPtr<AActor>& ClassValue : Object->SoftActorClassSet)
	{
		TestTrue(TEXT("null soft class set value"), ClassValue.IsNull());
	}
	TestNull(TEXT("nested hard class cleared"), Object->Nested.HardActorClass.Get());
	TestTrue(TEXT("nested soft class cleared"), Object->Nested.SoftActorClass.IsNull());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
