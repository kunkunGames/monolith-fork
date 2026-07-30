#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMaterialMoveOperationsTests, "Monolith.ParamGuard.Material.MoveExpression", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMaterialMoveOperationsTests::RunTest(const FString& Parameters)
{
	// Test 1: Single move with wrong type for pos_x
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
		Payload->SetStringField(TEXT("expression_name"), TEXT("MyExpr"));
		Payload->SetBoolField(TEXT("pos_x"), true); // Should be number

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should fail on wrong type for pos_x"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention pos_x"), Result.ErrorMessage.Contains(TEXT("pos_x")));
	}

	// Test 2: Expressions array missing name
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));

		TArray<TSharedPtr<FJsonValue>> ExprArray;
		TSharedPtr<FJsonObject> ExprObj = MakeShared<FJsonObject>();
		ExprObj->SetNumberField(TEXT("x"), 100);
		ExprArray.Add(MakeShared<FJsonValueObject>(ExprObj));

		Payload->SetArrayField(TEXT("expressions"), ExprArray);

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should fail when expression misses name"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention name"), Result.ErrorMessage.Contains(TEXT("name")));
	}

	// Test 3: Expressions array with wrong type for x
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));

		TArray<TSharedPtr<FJsonValue>> ExprArray;
		TSharedPtr<FJsonObject> ExprObj = MakeShared<FJsonObject>();
		ExprObj->SetStringField(TEXT("name"), TEXT("MyExpr"));
		ExprObj->SetStringField(TEXT("x"), TEXT("100")); // Should be number
		ExprArray.Add(MakeShared<FJsonValueObject>(ExprObj));

		Payload->SetArrayField(TEXT("expressions"), ExprArray);

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should fail when expression x is wrong type"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention x"), Result.ErrorMessage.Contains(TEXT("x")));
	}

	// Test 4: Single move with wrong type for expression_name
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
		Payload->SetNumberField(TEXT("expression_name"), 123); // Should be string
		Payload->SetNumberField(TEXT("pos_x"), 100);

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should fail on wrong type for expression_name"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention expression_name"), Result.ErrorMessage.Contains(TEXT("expression_name")));
	}

	// Test 5: A string-serialized array is never accepted as an alternate wire format.
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
		Payload->SetStringField(TEXT("expressions"), TEXT("[{\"name\":\"MyExpr\",\"x\":100,\"y\":200}]"));

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should reject string-serialized expressions array"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention expressions array"), Result.ErrorMessage.Contains(TEXT("expressions")));
	}

	// Test 6: Every batch entry must be an object.
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
		Payload->SetArrayField(
			TEXT("expressions"),
			{MakeShared<FJsonValueString>(TEXT("MyExpr"))});

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should reject non-object batch entry"), Result.bSuccess);
		TestTrue(TEXT("Error message should identify expressions[0]"), Result.ErrorMessage.Contains(TEXT("expressions[0]")));
	}

	// Test 7: Fractional and out-of-range editor coordinates are rejected.
	{
		for (const double InvalidX : {100.5, static_cast<double>(TNumericLimits<int32>::Max()) + 1.0})
		{
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
			Payload->SetStringField(TEXT("expression_name"), TEXT("MyExpr"));
			Payload->SetNumberField(TEXT("pos_x"), InvalidX);

			const FMonolithActionResult Result =
				FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

			TestFalse(TEXT("Should reject non-int32 coordinate"), Result.bSuccess);
			TestTrue(TEXT("Coordinate error should mention pos_x"), Result.ErrorMessage.Contains(TEXT("pos_x")));
		}
	}

	// Test 8: Canonical and alias coordinates cannot both be present.
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
		TSharedPtr<FJsonObject> Expression = MakeShared<FJsonObject>();
		Expression->SetStringField(TEXT("name"), TEXT("MyExpr"));
		Expression->SetNumberField(TEXT("x"), 100);
		Expression->SetNumberField(TEXT("pos_x"), 200);
		Payload->SetArrayField(
			TEXT("expressions"),
			{MakeShared<FJsonValueObject>(Expression)});

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should reject ambiguous x aliases"), Result.bSuccess);
		TestTrue(TEXT("Alias error should identify both fields"), Result.ErrorMessage.Contains(TEXT("x")) && Result.ErrorMessage.Contains(TEXT("pos_x")));
	}

	// Test 9: Move mode and batch target names are unambiguous.
	{
		TSharedPtr<FJsonObject> BothModesPayload = MakeShared<FJsonObject>();
		BothModesPayload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
		BothModesPayload->SetStringField(TEXT("expression_name"), TEXT("MyExpr"));
		TSharedPtr<FJsonObject> Expression = MakeShared<FJsonObject>();
		Expression->SetStringField(TEXT("name"), TEXT("MyExpr"));
		BothModesPayload->SetArrayField(
			TEXT("expressions"),
			{MakeShared<FJsonValueObject>(Expression)});

		const FMonolithActionResult BothModesResult =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("material"),
				TEXT("move_expression"),
				BothModesPayload);
		TestFalse(TEXT("Should reject simultaneous single and batch modes"), BothModesResult.bSuccess);
		TestTrue(TEXT("Mode error should require exactly one selector"), BothModesResult.ErrorMessage.Contains(TEXT("exactly one")));

		TSharedPtr<FJsonObject> DuplicatePayload = MakeShared<FJsonObject>();
		DuplicatePayload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
		DuplicatePayload->SetArrayField(
			TEXT("expressions"),
			{
				MakeShared<FJsonValueObject>(Expression),
				MakeShared<FJsonValueObject>(Expression)
			});

		const FMonolithActionResult DuplicateResult =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("material"),
				TEXT("move_expression"),
				DuplicatePayload);
		TestFalse(TEXT("Should reject duplicate batch target names"), DuplicateResult.bSuccess);
		TestTrue(TEXT("Duplicate error should identify the duplicated name"), DuplicateResult.ErrorMessage.Contains(TEXT("duplicates expression")));
	}

	// Test 10: Batch target resolution is atomic; a missing target leaves every node unchanged.
	{
		const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = FString::Printf(
			TEXT("/Game/Tests/Monolith/Transient/M_MoveExpression_%s"),
			*UniqueSuffix);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		UPackage* Package = CreatePackage(*PackageName);
		UMaterial* Material = Package
			? NewObject<UMaterial>(
				Package,
				*AssetName,
				RF_Public | RF_Standalone | RF_Transactional)
			: nullptr;
		UMaterialExpressionConstant* ExistingExpression = Material
			? Cast<UMaterialExpressionConstant>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					Material,
					UMaterialExpressionConstant::StaticClass(),
					10,
					20))
			: nullptr;
		ON_SCOPE_EXIT
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			if (Material)
			{
				Material->ClearFlags(RF_Public | RF_Standalone);
				Material->MarkAsGarbage();
			}
			if (Package)
			{
				Package->MarkAsGarbage();
			}
		};

		TestNotNull(TEXT("Transient material should be created"), Material);
		TestNotNull(TEXT("Transient material expression should be created"), ExistingExpression);
		if (!Material || !ExistingExpression)
		{
			return false;
		}

		const int32 OriginalX = ExistingExpression->MaterialExpressionEditorX;
		const int32 OriginalY = ExistingExpression->MaterialExpressionEditorY;
		const auto MakeMove = [](const FString& Name, const int32 X, const int32 Y)
		{
			TSharedPtr<FJsonObject> Move = MakeShared<FJsonObject>();
			Move->SetStringField(TEXT("name"), Name);
			Move->SetNumberField(TEXT("x"), X);
			Move->SetNumberField(TEXT("y"), Y);
			return MakeShared<FJsonValueObject>(Move);
		};

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), Material->GetPathName());
		Payload->SetArrayField(
			TEXT("expressions"),
			{
				MakeMove(ExistingExpression->GetName(), 100, 200),
				MakeMove(TEXT("MissingExpression"), 300, 400)
			});

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Batch with missing target should fail"), Result.bSuccess);
		TestTrue(TEXT("Missing-target error should name the target"), Result.ErrorMessage.Contains(TEXT("MissingExpression")));
		TestEqual(TEXT("Existing expression X should remain unchanged"), ExistingExpression->MaterialExpressionEditorX, OriginalX);
		TestEqual(TEXT("Existing expression Y should remain unchanged"), ExistingExpression->MaterialExpressionEditorY, OriginalY);

		ExistingExpression->MaterialExpressionEditorX = TNumericLimits<int32>::Max();
		TSharedPtr<FJsonObject> OverflowPayload = MakeShared<FJsonObject>();
		OverflowPayload->SetStringField(TEXT("asset_path"), Material->GetPathName());
		OverflowPayload->SetStringField(TEXT("expression_name"), ExistingExpression->GetName());
		OverflowPayload->SetNumberField(TEXT("pos_x"), 1);
		OverflowPayload->SetBoolField(TEXT("relative"), true);

		const FMonolithActionResult OverflowResult =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("material"),
				TEXT("move_expression"),
				OverflowPayload);
		TestFalse(TEXT("Relative coordinate overflow should fail"), OverflowResult.bSuccess);
		TestTrue(TEXT("Overflow error should describe the coordinate range"), OverflowResult.ErrorMessage.Contains(TEXT("32-bit")));
		TestEqual(
			TEXT("Overflow failure should leave X unchanged"),
			ExistingExpression->MaterialExpressionEditorX,
			TNumericLimits<int32>::Max());
	}

	// Batch material actions accept one canonical wire format: a native JSON
	// string array. Reject string-encoded arrays and mixed element types before
	// any asset load or transaction begins.
	{
		const TArray<FString> BatchActions = {
			TEXT("batch_set_material_property"),
			TEXT("batch_recompile")
		};

		for (const FString& Action : BatchActions)
		{
			TSharedPtr<FJsonObject> StringEncodedPayload = MakeShared<FJsonObject>();
			StringEncodedPayload->SetStringField(
				TEXT("asset_paths"),
				TEXT("[\"/Game/DoesNotMatter\"]"));
			const FMonolithActionResult StringEncodedResult =
				FMonolithToolRegistry::Get().ExecuteAction(
					TEXT("material"),
					Action,
					StringEncodedPayload);
			TestFalse(
				*FString::Printf(TEXT("%s rejects string-encoded asset_paths"), *Action),
				StringEncodedResult.bSuccess);
			TestTrue(
				*FString::Printf(TEXT("%s string-encoded error names asset_paths"), *Action),
				StringEncodedResult.ErrorMessage.Contains(TEXT("asset_paths")));

			TSharedPtr<FJsonObject> MixedArrayPayload = MakeShared<FJsonObject>();
			MixedArrayPayload->SetArrayField(
				TEXT("asset_paths"),
				{
					MakeShared<FJsonValueString>(TEXT("/Game/DoesNotMatter")),
					MakeShared<FJsonValueNumber>(123.0)
				});
			const FMonolithActionResult MixedArrayResult =
				FMonolithToolRegistry::Get().ExecuteAction(
					TEXT("material"),
					Action,
					MixedArrayPayload);
			TestFalse(
				*FString::Printf(TEXT("%s rejects a mixed asset_paths array"), *Action),
				MixedArrayResult.bSuccess);
			TestTrue(
				*FString::Printf(TEXT("%s mixed-array error names asset_paths[1]"), *Action),
				MixedArrayResult.ErrorMessage.Contains(TEXT("asset_paths[1]")));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
