#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIFontRepairRequiresRemapTest,
    "Monolith.ParamGuard.UI.CloneCompositeFontRequiresRemap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIFontRepairRequiresRemapTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("source_font_path"), TEXT("/Game/Tests/Monolith/UI/F_MissingSource"));
    Payload->SetStringField(TEXT("destination_font_path"), TEXT("/Game/Tests/Monolith/UI/F_MissingDest"));

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("clone_composite_font_with_remapped_faces"),
        Payload);

    TestFalse(TEXT("Composite font clone should reject missing remap configuration before loading assets"), Result.bSuccess);
    TestTrue(TEXT("Error should ask for root or face remap configuration"),
        Result.ErrorMessage.Contains(TEXT("source_root+dest_root")) ||
        Result.ErrorMessage.Contains(TEXT("font_face_remaps")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIFontRepairRequiresConfirmTest,
    "Monolith.ParamGuard.UI.CloneCompositeFontRequiresConfirm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIFontRepairRequiresConfirmTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("source_font_path"), TEXT("/Game/Tests/Monolith/UI/F_MissingSource"));
    Payload->SetStringField(TEXT("destination_font_path"), TEXT("/Game/Tests/Monolith/UI/F_MissingDest"));
    Payload->SetStringField(TEXT("source_root"), TEXT("/Game/Old"));
    Payload->SetStringField(TEXT("dest_root"), TEXT("/Game/New"));
    Payload->SetBoolField(TEXT("dry_run"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("clone_composite_font_with_remapped_faces"),
        Payload);

    TestFalse(TEXT("Mutating composite font clone should require confirm=true"), Result.bSuccess);
    TestTrue(TEXT("Error should mention confirm=true"), Result.ErrorMessage.Contains(TEXT("confirm=true")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISlateFontRepairRequiresRemapTest,
    "Monolith.ParamGuard.UI.RepairSlateFontReferencesRequiresRemap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISlateFontRepairRequiresRemapTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_MissingSlateFontRepairTarget"));

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("repair_slate_font_references"),
        Payload);

    TestFalse(TEXT("Slate font repair should reject missing remap configuration before loading assets"), Result.bSuccess);
    TestTrue(TEXT("Error should ask for root or font asset remap configuration"),
        Result.ErrorMessage.Contains(TEXT("source_root+dest_root")) ||
        Result.ErrorMessage.Contains(TEXT("font_asset_remaps")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISlateFontRepairRequiresConfirmTest,
    "Monolith.ParamGuard.UI.RepairSlateFontReferencesRequiresConfirm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISlateFontRepairRequiresConfirmTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_MissingSlateFontRepairTarget"));
    Payload->SetStringField(TEXT("source_root"), TEXT("/Game/Old"));
    Payload->SetStringField(TEXT("dest_root"), TEXT("/Game/New"));
    Payload->SetBoolField(TEXT("dry_run"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("repair_slate_font_references"),
        Payload);

    TestFalse(TEXT("Mutating Slate font repair should require confirm=true"), Result.bSuccess);
    TestTrue(TEXT("Error should mention confirm=true"), Result.ErrorMessage.Contains(TEXT("confirm=true")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
