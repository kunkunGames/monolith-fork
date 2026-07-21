// Copyright tumourlove. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "MonolithToolRegistry.h"
#include "Spec/ResidentWidgetUISpecFingerprint.h"
#include "Spec/UISpecJsonSerializer.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace MonolithUI::ResidentUISpecFingerprintTests
{
    static constexpr const TCHAR* AlphaPackageName =
        TEXT("/Game/Tests/Monolith/UI/SpecFingerprint/WBP_Alpha");
    static constexpr const TCHAR* ZetaPackageName =
        TEXT("/Game/Tests/Monolith/UI/SpecFingerprint/WBP_Zeta");

    static UWidgetBlueprint* CreateResidentWidgetBlueprint(
        const FString& PackageName,
        const FString& LabelText,
        FString& OutError)
    {
        OutError.Reset();
        UPackage* const Package = CreatePackage(*PackageName);
        if (!Package)
        {
            OutError = FString::Printf(TEXT("CreatePackage failed for '%s'."), *PackageName);
            return nullptr;
        }
        Package->FullyLoad();

        FString AssetName;
        if (!PackageName.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
            || AssetName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Invalid test package name '%s'."), *PackageName);
            return nullptr;
        }

        UWidgetBlueprint* WidgetBlueprint = FindObject<UWidgetBlueprint>(Package, *AssetName);
        if (!WidgetBlueprint)
        {
            UWidgetBlueprintFactory* const Factory = NewObject<UWidgetBlueprintFactory>();
            Factory->BlueprintType = BPTYPE_Normal;
            Factory->ParentClass = UUserWidget::StaticClass();
            WidgetBlueprint = Cast<UWidgetBlueprint>(Factory->FactoryCreateNew(
                UWidgetBlueprint::StaticClass(),
                Package,
                FName(*AssetName),
                RF_Public | RF_Standalone,
                nullptr,
                GWarn));
        }
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
        {
            OutError = FString::Printf(TEXT("Failed to create resident Widget Blueprint '%s'."), *PackageName);
            return nullptr;
        }

        TestUtils::CleanupWidgetTree(WidgetBlueprint);
        UCanvasPanel* const Root = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(),
            TEXT("RootCanvas"));
        UTextBlock* const Label = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(
            UTextBlock::StaticClass(),
            TEXT("Label"));
        if (!Root || !Label)
        {
            OutError = FString::Printf(TEXT("Failed to construct WidgetTree for '%s'."), *PackageName);
            return nullptr;
        }
        Label->SetText(FText::FromString(LabelText));
        Root->AddChild(Label);
        WidgetBlueprint->WidgetTree->RootWidget = Root;

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
        FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
        if (!WidgetBlueprint->GeneratedClass)
        {
            OutError = FString::Printf(TEXT("Widget Blueprint '%s' did not compile a generated class."), *PackageName);
            return nullptr;
        }
        return WidgetBlueprint;
    }

    static bool IsLowercaseSha256(const FString& Value)
    {
        if (Value.Len() != 64)
        {
            return false;
        }
        for (const TCHAR Character : Value)
        {
            if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
            {
                return false;
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISpecCanonicalJsonOrderingTest,
    "MonolithUI.SpecFingerprint.CanonicalJsonOrdering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecCanonicalJsonOrderingTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> First = MakeShared<FJsonObject>();
    First->SetStringField(TEXT("zeta"), TEXT("last"));
    TSharedPtr<FJsonObject> FirstNested = MakeShared<FJsonObject>();
    FirstNested->SetStringField(TEXT("beta"), TEXT("b"));
    FirstNested->SetStringField(TEXT("alpha"), TEXT("a"));
    First->SetObjectField(TEXT("middle"), FirstNested);
    First->SetStringField(TEXT("alpha"), TEXT("first"));

    TSharedPtr<FJsonObject> Second = MakeShared<FJsonObject>();
    Second->SetStringField(TEXT("alpha"), TEXT("first"));
    TSharedPtr<FJsonObject> SecondNested = MakeShared<FJsonObject>();
    SecondNested->SetStringField(TEXT("alpha"), TEXT("a"));
    SecondNested->SetStringField(TEXT("beta"), TEXT("b"));
    Second->SetObjectField(TEXT("middle"), SecondNested);
    Second->SetStringField(TEXT("zeta"), TEXT("last"));

    FString FirstCanonical;
    FString SecondCanonical;
    FString Error;
    TestTrue(
        TEXT("First JSON canonicalizes"),
        FUISpecJsonSerializer::TryWriteCanonicalJson(First, FirstCanonical, Error));
    if (!Error.IsEmpty())
    {
        AddError(Error);
    }
    Error.Reset();
    TestTrue(
        TEXT("Second JSON canonicalizes"),
        FUISpecJsonSerializer::TryWriteCanonicalJson(Second, SecondCanonical, Error));
    if (!Error.IsEmpty())
    {
        AddError(Error);
    }
    TestEqual(TEXT("Object insertion order does not affect canonical JSON"), FirstCanonical, SecondCanonical);
    TestTrue(TEXT("Top-level keys are sorted"), FirstCanonical.StartsWith(TEXT("{\"alpha\":\"first\",\"middle\":{")));
    TestTrue(TEXT("Nested keys are sorted"), FirstCanonical.Contains(TEXT("{\"alpha\":\"a\",\"beta\":\"b\"}")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIResidentUISpecFingerprintTest,
    "MonolithUI.SpecFingerprint.ResidentGeneratedClassSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIResidentUISpecFingerprintTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::ResidentUISpecFingerprintTests;

    ON_SCOPE_EXIT
    {
        constexpr const TCHAR* PackageNames[] =
        {
            AlphaPackageName,
            ZetaPackageName,
        };

        TArray<UPackage*> PackagesToUnload;
        for (const TCHAR* PackageName : PackageNames)
        {
            if (UPackage* const Package = FindPackage(nullptr, PackageName))
            {
                Package->SetDirtyFlag(false);
                PackagesToUnload.Add(Package);
            }
        }

        if (!PackagesToUnload.IsEmpty())
        {
            FText UnloadError;
            if (!UPackageTools::UnloadPackages(PackagesToUnload, UnloadError))
            {
                AddError(FString::Printf(
                    TEXT("Failed to unload resident UISpec fingerprint fixtures: %s"),
                    *UnloadError.ToString()));
            }
        }

        for (const TCHAR* PackageName : PackageNames)
        {
            if (FindPackage(nullptr, PackageName))
            {
                AddError(FString::Printf(
                    TEXT("Resident UISpec fingerprint fixture remained loaded: '%s'."),
                    PackageName));
            }
        }
    };

    FString Error;
    UWidgetBlueprint* const Alpha = CreateResidentWidgetBlueprint(
        AlphaPackageName,
        TEXT("Alpha"),
        Error);
    if (!Alpha)
    {
        AddError(Error);
        return false;
    }
    UWidgetBlueprint* const Zeta = CreateResidentWidgetBlueprint(
        ZetaPackageName,
        TEXT("Zeta"),
        Error);
    if (!Zeta)
    {
        AddError(Error);
        return false;
    }

    TArray<UClass*> ReverseInput{Zeta->GeneratedClass, Alpha->GeneratedClass};
    const FMonolithUIResidentUISpecFingerprintResult Reverse =
        FMonolithUIResidentUISpecFingerprint::Compute(ReverseInput);
    TestTrue(TEXT("Resident generated-class set fingerprints"), Reverse.bSuccess);
    if (!Reverse.bSuccess)
    {
        AddError(FString::Printf(TEXT("%s: %s"), *Reverse.FailureCode, *Reverse.FailureReason));
        return false;
    }
    TestEqual(TEXT("Two package rows emitted"), Reverse.Widgets.Num(), 2);
    TestEqual(
        TEXT("Rows sort by exact package name"),
        Reverse.Widgets[0].PackageName,
        AlphaPackageName);
    TestTrue(TEXT("First per-WBP digest is lowercase SHA-256"), IsLowercaseSha256(Reverse.Widgets[0].UISpecSha256));
    TestTrue(TEXT("Second per-WBP digest is lowercase SHA-256"), IsLowercaseSha256(Reverse.Widgets[1].UISpecSha256));
    TestTrue(TEXT("Aggregate digest is lowercase SHA-256"), IsLowercaseSha256(Reverse.AggregateSha256));

    TArray<UClass*> ForwardInput{Alpha->GeneratedClass, Zeta->GeneratedClass};
    const FMonolithUIResidentUISpecFingerprintResult Forward =
        FMonolithUIResidentUISpecFingerprint::Compute(ForwardInput);
    TestTrue(TEXT("Forward generated-class set fingerprints"), Forward.bSuccess);
    TestEqual(TEXT("Input order does not affect aggregate identity"), Forward.AggregateSha256, Reverse.AggregateSha256);

    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    if (!TestTrue(
        TEXT("ui.compute_widget_uispec_fingerprint is registered"),
        Registry.HasAction(TEXT("ui"), TEXT("compute_widget_uispec_fingerprint"))))
    {
        return false;
    }
    const FMonolithActionExecutionPolicy ActionPolicy = Registry.GetActionExecutionPolicy(
        TEXT("ui"),
        TEXT("compute_widget_uispec_fingerprint"));
    TestEqual(TEXT("Fingerprint action policy is read_only"), ActionPolicy.PolicyId, TEXT("read_only"));
    TestFalse(TEXT("Fingerprint action read-only policy is explicit"), ActionPolicy.bDefaulted);
    TestFalse(TEXT("Fingerprint action never tracks dirty packages"), ActionPolicy.bDirtyPackageTracking);
    TestFalse(TEXT("Fingerprint action never wraps a transaction"), ActionPolicy.bTransactionWrapping);
    TestFalse(TEXT("Fingerprint action never runs post-edit validation"), ActionPolicy.bPostEditValidation);

    TSharedPtr<FJsonObject> ActionParams = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ActionPaths;
    ActionPaths.Add(MakeShared<FJsonValueString>(ZetaPackageName));
    ActionPaths.Add(MakeShared<FJsonValueString>(Alpha->GetPathName()));
    ActionParams->SetArrayField(TEXT("asset_paths"), ActionPaths);

    const FMonolithActionResult ActionResult = Registry.ExecuteAction(
        TEXT("ui"),
        TEXT("compute_widget_uispec_fingerprint"),
        ActionParams);
    if (!TestTrue(TEXT("MCP fingerprint action succeeds"), ActionResult.bSuccess && ActionResult.Result.IsValid()))
    {
        AddError(ActionResult.ErrorMessage);
        return false;
    }
    TestTrue(TEXT("Action payload reports success"), ActionResult.Result->GetBoolField(TEXT("bSuccess")));
    TestFalse(
        TEXT("Read-only action emits no source-control prepare payload"),
        ActionResult.Result->HasField(TEXT("source_control_prepare")));
    TestEqual(
        TEXT("Action reuses native aggregate identity"),
        ActionResult.Result->GetStringField(TEXT("aggregate_sha256")),
        Forward.AggregateSha256);
    TestEqual(
        TEXT("Action emits both fixture rows"),
        static_cast<int32>(ActionResult.Result->GetNumberField(TEXT("widget_count"))),
        2);
    const TArray<TSharedPtr<FJsonValue>>* ActionRows = nullptr;
    TestTrue(
        TEXT("Action returns package-sorted widget rows"),
        ActionResult.Result->TryGetArrayField(TEXT("widgets"), ActionRows)
            && ActionRows
            && ActionRows->Num() == 2
            && (*ActionRows)[0]->AsObject()->GetStringField(TEXT("package_name")) == AlphaPackageName);

    TSharedPtr<FJsonObject> TooManyParams = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> TooManyPaths;
    for (int32 Index = 0; Index < 129; ++Index)
    {
        TooManyPaths.Add(MakeShared<FJsonValueString>(AlphaPackageName));
    }
    TooManyParams->SetArrayField(TEXT("asset_paths"), TooManyPaths);
    const FMonolithActionResult TooManyResult = Registry.ExecuteAction(
        TEXT("ui"),
        TEXT("compute_widget_uispec_fingerprint"),
        TooManyParams);
    TestFalse(TEXT("Action rejects more than 128 inputs"), TooManyResult.bSuccess);
    TestTrue(
        TEXT("Count rejection is explicit and has no partial result"),
        !TooManyResult.Result.IsValid()
            && TooManyResult.ErrorData.IsValid()
            && TooManyResult.ErrorData->GetStringField(TEXT("failure_code")) == TEXT("InvalidAssetPathCount"));

    UTextBlock* const AlphaLabel = Cast<UTextBlock>(Alpha->WidgetTree->FindWidget(TEXT("Label")));
    if (!AlphaLabel)
    {
        AddError(TEXT("Alpha test WBP has no Label TextBlock."));
        return false;
    }
    AlphaLabel->SetText(FText::FromString(TEXT("Alpha changed")));
    const FMonolithUIResidentUISpecFingerprintResult Changed =
        FMonolithUIResidentUISpecFingerprint::Compute(ForwardInput);
    TestTrue(TEXT("Changed resident WBP fingerprints"), Changed.bSuccess);
    TestNotEqual(TEXT("UISpec data change changes aggregate identity"), Changed.AggregateSha256, Forward.AggregateSha256);

    TArray<UClass*> NativeClassInput{UUserWidget::StaticClass()};
    const FMonolithUIResidentUISpecFingerprintResult NativeClass =
        FMonolithUIResidentUISpecFingerprint::Compute(NativeClassInput);
    TestFalse(TEXT("Native UUserWidget class fails closed"), NativeClass.bSuccess);
    TestEqual(TEXT("Native class failure is explicit"), NativeClass.FailureCode, TEXT("NotWidgetBlueprintGeneratedClass"));
    TestTrue(TEXT("Failed result has no partial package rows"), NativeClass.Widgets.IsEmpty());
    TestTrue(TEXT("Failed result has no aggregate digest"), NativeClass.AggregateSha256.IsEmpty());

    TArray<UClass*> DuplicateInput{Alpha->GeneratedClass, Alpha->GeneratedClass};
    const FMonolithUIResidentUISpecFingerprintResult Duplicate =
        FMonolithUIResidentUISpecFingerprint::Compute(DuplicateInput);
    TestFalse(TEXT("Duplicate package fails closed"), Duplicate.bSuccess);
    TestEqual(TEXT("Duplicate failure is explicit"), Duplicate.FailureCode, TEXT("DuplicatePackage"));
    TestTrue(TEXT("Duplicate failure has no partial package rows"), Duplicate.Widgets.IsEmpty());

    UClass* const OriginalAlphaGeneratedClass = Alpha->GeneratedClass;
    Alpha->GeneratedClass = Zeta->GeneratedClass;
    TSharedPtr<FJsonObject> MismatchedParams = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> MismatchedPaths;
    MismatchedPaths.Add(MakeShared<FJsonValueString>(Alpha->GetPathName()));
    MismatchedParams->SetArrayField(TEXT("asset_paths"), MismatchedPaths);
    const FMonolithActionResult MismatchedResult = Registry.ExecuteAction(
        TEXT("ui"),
        TEXT("compute_widget_uispec_fingerprint"),
        MismatchedParams);
    Alpha->GeneratedClass = OriginalAlphaGeneratedClass;
    TestFalse(TEXT("Action rejects a mismatched generated-class back-reference"), MismatchedResult.bSuccess);
    TestTrue(
        TEXT("Back-reference rejection is fail-closed and explicit"),
        !MismatchedResult.Result.IsValid()
            && MismatchedResult.ErrorData.IsValid()
            && MismatchedResult.ErrorData->GetStringField(TEXT("failure_code"))
                == TEXT("GeneratedClassBackReferenceMismatch"));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
