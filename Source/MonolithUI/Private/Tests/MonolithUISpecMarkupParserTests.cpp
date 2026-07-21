// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Actions/MonolithUIEffectActions.h"
#include "Actions/MonolithUISpecActions.h"
#include "MonolithUIActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithUICommon.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "MonolithUISlotActions.h"
#include "MonolithUIStylingActions.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/TextBlock.h"

namespace
{
	bool ContainsFindingCategory(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& ExpectedCategory)
	{
		if (!Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}

			FString Category;
			if (Obj->TryGetStringField(TEXT("category"), Category) && Category == ExpectedCategory)
			{
				return true;
			}
		}
		return false;
	}

	bool FindNodeById(const TSharedPtr<FJsonObject>& Node, const FString& ExpectedId, TSharedPtr<FJsonObject>& OutNode)
	{
		if (!Node.IsValid())
		{
			return false;
		}

		FString Id;
		if (Node->TryGetStringField(TEXT("id"), Id) && Id == ExpectedId)
		{
			OutNode = Node;
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (!Node->TryGetArrayField(TEXT("children"), Children) || !Children)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
		{
			if (FindNodeById(ChildValue.IsValid() ? ChildValue->AsObject() : nullptr, ExpectedId, OutNode))
			{
				return true;
			}
		}
		return false;
	}

	bool ContainsStepAction(const TArray<TSharedPtr<FJsonValue>>* Steps, const FString& ExpectedAction)
	{
		if (!Steps)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& StepValue : *Steps)
		{
			const TSharedPtr<FJsonObject> Step = StepValue.IsValid() ? StepValue->AsObject() : nullptr;
			if (!Step.IsValid())
			{
				continue;
			}

			FString Action;
			if (Step->TryGetStringField(TEXT("action"), Action) && Action == ExpectedAction)
			{
				return true;
			}
		}
		return false;
	}

	int32 CountStepAction(const TArray<TSharedPtr<FJsonValue>>* Steps, const FString& ExpectedAction)
	{
		if (!Steps)
		{
			return 0;
		}

		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& StepValue : *Steps)
		{
			const TSharedPtr<FJsonObject> Step = StepValue.IsValid() ? StepValue->AsObject() : nullptr;
			if (!Step.IsValid())
			{
				continue;
			}

			FString Action;
			if (Step->TryGetStringField(TEXT("action"), Action) && Action == ExpectedAction)
			{
				++Count;
			}
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecMarkupBasicTreeTest,
	"MonolithUI.SpecMarkup.BasicTreeConvertsToCanonicalSpec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecMarkupBasicTreeTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("markup"),
		TEXT("<VerticalBox Name=\"Root\"><TextBlock Name=\"Title\" Text=\"Hello\" FontSize=\"24\" slot.padding=\"4,8,4,8\" /></VerticalBox>"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), Params);
	if (!TestTrue(TEXT("convert_markup_to_ui_spec succeeds on wire"), Result.bSuccess && Result.Result.IsValid()))
	{
		return false;
	}

	TestTrue(TEXT("payload bSuccess"), Result.Result->GetBoolField(TEXT("bSuccess")));
	TestFalse(TEXT("does not create assets"), Result.Result->GetBoolField(TEXT("would_create_asset")));
	TestEqual(TEXT("node_count"), static_cast<int32>(Result.Result->GetNumberField(TEXT("node_count"))), 2);

	const TSharedPtr<FJsonObject> Spec = Result.Result->GetObjectField(TEXT("spec"));
	const TSharedPtr<FJsonObject> Root = Spec->GetObjectField(TEXT("rootWidget"));
	TestEqual(TEXT("root type"), Root->GetStringField(TEXT("type")), TEXT("VerticalBox"));
	TestEqual(TEXT("root id"), Root->GetStringField(TEXT("id")), TEXT("Root"));

	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	TestTrue(TEXT("children exist"), Root->TryGetArrayField(TEXT("children"), Children) && Children && Children->Num() == 1);
	if (Children && Children->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Child = (*Children)[0]->AsObject();
		TestEqual(TEXT("child type"), Child->GetStringField(TEXT("type")), TEXT("TextBlock"));
		TestEqual(TEXT("child id"), Child->GetStringField(TEXT("id")), TEXT("Title"));
		TestEqual(TEXT("child text"), Child->GetObjectField(TEXT("content"))->GetStringField(TEXT("text")), TEXT("Hello"));
		TestEqual(TEXT("slot padding left"), Child->GetObjectField(TEXT("slot"))->GetObjectField(TEXT("padding"))->GetNumberField(TEXT("left")), 4.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecMarkupUnknownTagStrictTest,
	"MonolithUI.SpecMarkup.UnknownTagStrictFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecMarkupUnknownTagStrictTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("markup"), TEXT("<NotAWidget Name=\"Root\" />"));
	Params->SetBoolField(TEXT("strict"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), Params);
	if (!TestTrue(TEXT("convert_markup_to_ui_spec returns structured failure"), Result.bSuccess && Result.Result.IsValid()))
	{
		return false;
	}

	TestFalse(TEXT("payload bSuccess"), Result.Result->GetBoolField(TEXT("bSuccess")));
	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	TestTrue(TEXT("errors contain MarkupType"),
		Result.Result->TryGetArrayField(TEXT("errors"), Errors) && ContainsFindingCategory(Errors, TEXT("MarkupType")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecMarkupInvalidSlotContextStrictTest,
	"MonolithUI.SpecMarkup.InvalidSlotContextStrictFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecMarkupInvalidSlotContextStrictTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("markup"),
		TEXT("<VerticalBox Name=\"Root\"><TextBlock Name=\"Title\" Text=\"Hello\" slot.position=\"16,24\" /></VerticalBox>"));
	Params->SetBoolField(TEXT("strict"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), Params);
	if (!TestTrue(TEXT("convert_markup_to_ui_spec returns structured failure"), Result.bSuccess && Result.Result.IsValid()))
	{
		return false;
	}

	TestFalse(TEXT("payload bSuccess"), Result.Result->GetBoolField(TEXT("bSuccess")));
	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	TestTrue(TEXT("errors contain MarkupSlotContext"),
		Result.Result->TryGetArrayField(TEXT("errors"), Errors) && ContainsFindingCategory(Errors, TEXT("MarkupSlotContext")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecMarkupRootSavePathReadOnlyTest,
	"MonolithUI.SpecMarkup.RootSavePathDoesNotCreateAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecMarkupRootSavePathReadOnlyTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("markup"), TEXT("<CanvasPanel Name=\"RootCanvas\" />"));
	Params->SetStringField(TEXT("root_save_path"), TEXT("/Game/Tests/UI/WBP_MarkupConverted"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), Params);
	if (!TestTrue(TEXT("convert_markup_to_ui_spec succeeds"), Result.bSuccess && Result.Result.IsValid()))
	{
		return false;
	}

	TestFalse(TEXT("read-only conversion"), Result.Result->GetBoolField(TEXT("would_create_asset")));
	TestEqual(TEXT("root_save_path echoed"), Result.Result->GetStringField(TEXT("root_save_path")), TEXT("/Game/Tests/UI/WBP_MarkupConverted"));
	TestEqual(TEXT("spec name derived from root_save_path"),
		Result.Result->GetObjectField(TEXT("spec"))->GetStringField(TEXT("name")),
		TEXT("WBP_MarkupConverted"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecMarkupBuildDumpRoundtripTest,
	"MonolithUI.SpecMarkup.BuildDumpRoundtrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecMarkupBuildDumpRoundtripTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MarkupRoundtrip");

	TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
	ConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<CanvasPanel Name=\"RootCanvas\">"
			 "<TextBlock Name=\"TitleText\" Text=\"HTML Design\" FontSize=\"28\" FontColor=\"#FFAA00\" slot.position=\"32,24\" slot.size=\"360,48\" />"
			 "<Button Name=\"StartButton\" slot.position=\"96,112\" slot.size=\"240,64\" />"
			 "</CanvasPanel>"));
	ConvertParams->SetStringField(TEXT("dialect"), TEXT("html"));
	ConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
	ConvertParams->SetStringField(TEXT("request_id"), TEXT("markup-roundtrip"));

	const FMonolithActionResult ConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), ConvertParams);
	if (!TestTrue(TEXT("convert action succeeds on wire"), ConvertResult.bSuccess && ConvertResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("convert payload bSuccess"), ConvertResult.Result->GetBoolField(TEXT("bSuccess")));
	TestEqual(TEXT("converted node_count"), static_cast<int32>(ConvertResult.Result->GetNumberField(TEXT("node_count"))), 3);
	TestFalse(TEXT("convert remains read-only"), ConvertResult.Result->GetBoolField(TEXT("would_create_asset")));

	const TSharedPtr<FJsonObject> Spec = ConvertResult.Result->GetObjectField(TEXT("spec"));
	if (!TestTrue(TEXT("converted spec exists"), Spec.IsValid()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
	BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
	BuildParams->SetObjectField(TEXT("spec"), Spec);
	BuildParams->SetBoolField(TEXT("overwrite"), true);
	BuildParams->SetBoolField(TEXT("dry_run"), false);
	BuildParams->SetStringField(TEXT("request_id"), TEXT("markup-roundtrip"));

	const FMonolithActionResult BuildResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("build_ui_from_spec"), BuildParams);
	if (!TestTrue(TEXT("build action succeeds on wire"), BuildResult.bSuccess && BuildResult.Result.IsValid()))
	{
		return false;
	}
	if (!BuildResult.Result->GetBoolField(TEXT("bSuccess")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		if (BuildResult.Result->TryGetArrayField(TEXT("errors"), Errors) && Errors)
		{
			for (const TSharedPtr<FJsonValue>& ErrorValue : *Errors)
			{
				const TSharedPtr<FJsonObject> ErrorObject = ErrorValue.IsValid() ? ErrorValue->AsObject() : nullptr;
				if (ErrorObject.IsValid())
				{
					AddError(FString::Printf(
						TEXT("build error [%s]: %s"),
						*ErrorObject->GetStringField(TEXT("category")),
						*ErrorObject->GetStringField(TEXT("message"))));
				}
			}
		}
		return false;
	}
	TestEqual(TEXT("build asset_path"), BuildResult.Result->GetStringField(TEXT("asset_path")), AssetPath);

	TSharedPtr<FJsonObject> DumpParams = MakeShared<FJsonObject>();
	DumpParams->SetStringField(TEXT("asset_path"), AssetPath);
	DumpParams->SetStringField(TEXT("request_id"), TEXT("markup-roundtrip"));

	const FMonolithActionResult DumpResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("dump_ui_spec"), DumpParams);
	if (!TestTrue(TEXT("dump action succeeds on wire"), DumpResult.bSuccess && DumpResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("dump payload bSuccess"), DumpResult.Result->GetBoolField(TEXT("bSuccess")));
	TestEqual(TEXT("dump request_id"), DumpResult.Result->GetStringField(TEXT("request_id")), TEXT("markup-roundtrip"));

	const TSharedPtr<FJsonObject> DumpedSpec = DumpResult.Result->GetObjectField(TEXT("spec"));
	const TSharedPtr<FJsonObject> DumpedRoot = DumpedSpec->GetObjectField(TEXT("rootWidget"));
	TestEqual(TEXT("dumped root type"), DumpedRoot->GetStringField(TEXT("type")), TEXT("CanvasPanel"));
	TestEqual(TEXT("dumped root id"), DumpedRoot->GetStringField(TEXT("id")), TEXT("RootCanvas"));

	TSharedPtr<FJsonObject> DumpedTitle;
	TestTrue(TEXT("dumped spec contains TitleText"), FindNodeById(DumpedRoot, TEXT("TitleText"), DumpedTitle));
	if (DumpedTitle.IsValid())
	{
		TestEqual(TEXT("dumped title type"), DumpedTitle->GetStringField(TEXT("type")), TEXT("TextBlock"));
		TestEqual(TEXT("dumped title text"), DumpedTitle->GetObjectField(TEXT("content"))->GetStringField(TEXT("text")), TEXT("HTML Design"));
		TestTrue(TEXT("dumped title font size"),
			FMath::IsNearlyEqual(DumpedTitle->GetObjectField(TEXT("content"))->GetNumberField(TEXT("fontSize")), 28.0, 1.0));
	}

	TSharedPtr<FJsonObject> DumpedButton;
	TestTrue(TEXT("dumped spec contains StartButton"), FindNodeById(DumpedRoot, TEXT("StartButton"), DumpedButton));
	if (DumpedButton.IsValid())
	{
		TestEqual(TEXT("dumped button type"), DumpedButton->GetStringField(TEXT("type")), TEXT("Button"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecRequiredTextStyleAndSiblingOrderDiffTest,
	"MonolithUI.SpecMarkup.DiffRequiredTextStyleAndSiblingOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecRequiredTextStyleAndSiblingOrderDiffTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);
	FMonolithUIStylingActions::RegisterActions(Registry);

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SpecRequiredTextStyleOrder");
	TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
	ConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<VerticalBox Name=\"RootBox\">"
			 "<TextBlock Name=\"FirstText\" Text=\"First\" FontSize=\"31\" />"
			 "<TextBlock Name=\"SecondText\" Text=\"Second\" FontSize=\"20\" />"
			 "</VerticalBox>"));
	ConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
	const FMonolithActionResult ConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), ConvertParams);
	if (!TestTrue(TEXT("convert action succeeds"), ConvertResult.bSuccess && ConvertResult.Result.IsValid()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> FixtureSpec = ConvertResult.Result->GetObjectField(TEXT("spec"));
	const TSharedPtr<FJsonObject> FixtureRoot = FixtureSpec->GetObjectField(TEXT("rootWidget"));
	TSharedPtr<FJsonObject> FixtureFirst;
	if (!TestTrue(TEXT("fixture contains FirstText"), FindNodeById(FixtureRoot, TEXT("FirstText"), FixtureFirst)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> FixtureContent = FixtureFirst->GetObjectField(TEXT("content"));
	FixtureContent->SetStringField(TEXT("fontFamily"), TEXT("/Engine/EngineFonts/RobotoDistanceField.RobotoDistanceField"));
	FixtureContent->SetStringField(TEXT("typeface"), TEXT("Italic"));
	FixtureContent->SetNumberField(TEXT("letterSpacing"), 14);
	FixtureContent->SetStringField(TEXT("justification"), TEXT("Right"));
	FixtureContent->SetNumberField(TEXT("outlineSize"), 2);
	FixtureContent->SetStringField(TEXT("outlineColor"), TEXT("#FF4000FF"));
	TSharedPtr<FJsonObject> FixtureShadowOffset = MakeShared<FJsonObject>();
	FixtureShadowOffset->SetNumberField(TEXT("x"), 3.0);
	FixtureShadowOffset->SetNumberField(TEXT("y"), 4.0);
	FixtureContent->SetObjectField(TEXT("shadowOffset"), FixtureShadowOffset);
	FixtureContent->SetStringField(TEXT("shadowColor"), TEXT("#00000080"));
	FixtureContent->SetNumberField(TEXT("lineHeightPercentage"), 1.25);
	TSharedPtr<FJsonObject> FixtureSecond;
	if (!TestTrue(TEXT("fixture contains SecondText"), FindNodeById(FixtureRoot, TEXT("SecondText"), FixtureSecond)))
	{
		return false;
	}
	const UTextBlock* DefaultTextBlock = GetDefault<UTextBlock>();
	if (!TestNotNull(TEXT("default TextBlock available"), DefaultTextBlock)
		|| !TestNotNull(TEXT("default TextBlock font object available"), DefaultTextBlock->GetFont().FontObject.Get()))
	{
		return false;
	}
	const FSlateFontInfo& DefaultFont = DefaultTextBlock->GetFont();
	const TSharedPtr<FJsonObject> FixtureSecondContent = FixtureSecond->GetObjectField(TEXT("content"));
	FixtureSecondContent->SetNumberField(TEXT("fontSize"), DefaultFont.Size);
	FixtureSecondContent->SetStringField(TEXT("fontFamily"), DefaultFont.FontObject->GetPathName());
	FixtureSecondContent->SetStringField(TEXT("typeface"), DefaultFont.TypefaceFontName.ToString());
	const TSharedPtr<FJsonObject> FixtureSecondSlot = FixtureSecond->GetObjectField(TEXT("slot"));
	FixtureSecondSlot->SetStringField(TEXT("hAlign"), TEXT("Fill"));
	FixtureSecondSlot->SetStringField(TEXT("vAlign"), TEXT("Fill"));
	FixtureSecondSlot->SetStringField(TEXT("sizeRule"), TEXT("Automatic"));
	FixtureSecondSlot->SetNumberField(TEXT("fillWeight"), 1.0);
	BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
	BuildParams->SetObjectField(TEXT("spec"), FixtureSpec);
	BuildParams->SetBoolField(TEXT("overwrite"), true);
	BuildParams->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult BuildResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("build_ui_from_spec"), BuildParams);
	if (!TestTrue(TEXT("build action succeeds"), BuildResult.bSuccess && BuildResult.Result.IsValid()))
	{
		return false;
	}

	FMonolithActionResult LoadError;
	UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(AssetPath, LoadError);
	if (!TestNotNull(TEXT("built WBP loaded"), WBP))
	{
		return false;
	}
	UTextBlock* FirstText = Cast<UTextBlock>(WBP->WidgetTree->FindWidget(TEXT("FirstText")));
	if (!TestNotNull(TEXT("FirstText loaded"), FirstText))
	{
		return false;
	}

	TestEqual(TEXT("builder applies font family"), FirstText->GetFont().FontObject->GetPathName(), TEXT("/Engine/EngineFonts/RobotoDistanceField.RobotoDistanceField"));
	TestEqual(TEXT("builder applies typeface"), FirstText->GetFont().TypefaceFontName, FName(TEXT("Italic")));
	TestEqual(TEXT("builder applies letter spacing"), FirstText->GetFont().LetterSpacing, 14);
	TestEqual(TEXT("builder applies outline size"), FirstText->GetFont().OutlineSettings.OutlineSize, 2);
	TestTrue(TEXT("builder applies shadow offset"), FirstText->GetShadowOffset().Equals(FVector2D(3.f, 4.f), 0.001f));
	TestTrue(TEXT("builder applies shadow color"), FirstText->GetShadowColorAndOpacity().Equals(FLinearColor(0.f, 0.f, 0.f, 128.f / 255.f), 0.01f));

	TSharedPtr<FJsonObject> IdenticalDiffParams = MakeShared<FJsonObject>();
	IdenticalDiffParams->SetStringField(TEXT("asset_path"), AssetPath);
	IdenticalDiffParams->SetObjectField(TEXT("desired_spec"), FixtureSpec);
	IdenticalDiffParams->SetStringField(TEXT("compare_mode"), TEXT("properties"));
	IdenticalDiffParams->SetBoolField(TEXT("required"), true);
	const FMonolithActionResult IdenticalDiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), IdenticalDiffParams);
	if (!TestTrue(TEXT("required full diff after build returns evidence"),
		IdenticalDiffResult.bSuccess && IdenticalDiffResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("omitted and explicit CDO defaults compare identical"), IdenticalDiffResult.Result->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("identical required diff bSuccess is true"), IdenticalDiffResult.Result->GetBoolField(TEXT("bSuccess")));
	TestFalse(TEXT("identical required diff is unchanged"), IdenticalDiffResult.Result->GetBoolField(TEXT("changed")));
	TestEqual(TEXT("identical required diff status"), IdenticalDiffResult.Result->GetStringField(TEXT("status")), TEXT("identical"));

	TSharedPtr<FJsonObject> DumpParams = MakeShared<FJsonObject>();
	DumpParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult DumpResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("dump_ui_spec"), DumpParams);
	if (!TestTrue(TEXT("dump action succeeds"), DumpResult.bSuccess && DumpResult.Result.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> DesiredSpec = DumpResult.Result->GetObjectField(TEXT("spec"));
	const TSharedPtr<FJsonObject> DesiredRoot = DesiredSpec->GetObjectField(TEXT("rootWidget"));
	TSharedPtr<FJsonObject> DesiredFirst;
	if (!TestTrue(TEXT("dump contains FirstText"), FindNodeById(DesiredRoot, TEXT("FirstText"), DesiredFirst)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> FirstContent = DesiredFirst->GetObjectField(TEXT("content"));
	TestEqual(
		TEXT("font family exported"),
		FirstContent->GetStringField(TEXT("fontFamily")),
		TEXT("/Engine/EngineFonts/RobotoDistanceField.RobotoDistanceField"));
	TestEqual(TEXT("typeface exported"), FirstContent->GetStringField(TEXT("typeface")), TEXT("Italic"));
	TestEqual(TEXT("letter spacing exported"), static_cast<int32>(FirstContent->GetNumberField(TEXT("letterSpacing"))), 14);
	TestEqual(TEXT("justification exported"), FirstContent->GetStringField(TEXT("justification")), TEXT("Right"));
	TestEqual(TEXT("outline size exported"), static_cast<int32>(FirstContent->GetNumberField(TEXT("outlineSize"))), 2);
	TestTrue(TEXT("outline color exported"), FirstContent->HasField(TEXT("outlineColor")));
	TestTrue(TEXT("shadow offset exported"), FirstContent->HasField(TEXT("shadowOffset")));
	TestTrue(TEXT("shadow color exported"), FirstContent->HasField(TEXT("shadowColor")));
	TestTrue(TEXT("line height exported"),
		FMath::IsNearlyEqual(FirstContent->GetNumberField(TEXT("lineHeightPercentage")), 1.25, 0.001));

	TArray<TSharedPtr<FJsonValue>> DesiredChildren = DesiredRoot->GetArrayField(TEXT("children"));
	if (!TestEqual(TEXT("fixture has two children"), DesiredChildren.Num(), 2))
	{
		return false;
	}
	Swap(DesiredChildren[0], DesiredChildren[1]);
	DesiredRoot->SetArrayField(TEXT("children"), DesiredChildren);
	FirstContent->SetStringField(TEXT("typeface"), TEXT("Bold"));
	FirstContent->SetStringField(TEXT("text"), TEXT("First invariant"));
	FirstContent->SetStringField(TEXT("justification"), TEXT("InvariantLeft"));

	TSharedPtr<FJsonObject> RequiredDiffParams = MakeShared<FJsonObject>();
	RequiredDiffParams->SetStringField(TEXT("asset_path"), AssetPath);
	RequiredDiffParams->SetObjectField(TEXT("desired_spec"), DesiredSpec);
	RequiredDiffParams->SetStringField(TEXT("compare_mode"), TEXT("properties"));
	RequiredDiffParams->SetBoolField(TEXT("required"), true);
	const FMonolithActionResult RequiredDiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), RequiredDiffParams);
	if (!TestTrue(TEXT("required diff preserves transport success"), RequiredDiffResult.bSuccess && RequiredDiffResult.Result.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("required mismatch payload is semantic failure"), RequiredDiffResult.Result->GetBoolField(TEXT("ok")));
	TestFalse(TEXT("required mismatch bSuccess is false"), RequiredDiffResult.Result->GetBoolField(TEXT("bSuccess")));
	TestEqual(TEXT("required mismatch status"), RequiredDiffResult.Result->GetStringField(TEXT("status")), TEXT("failed"));
	TestTrue(TEXT("required mismatch changed"), RequiredDiffResult.Result->GetBoolField(TEXT("changed")));

	bool bFoundChildOrder = false;
	bool bFoundContent = false;
	const TArray<TSharedPtr<FJsonValue>>& Changes = RequiredDiffResult.Result->GetArrayField(TEXT("changes"));
	for (const TSharedPtr<FJsonValue>& ChangeValue : Changes)
	{
		const TSharedPtr<FJsonObject> Change = ChangeValue.IsValid() ? ChangeValue->AsObject() : nullptr;
		if (!Change.IsValid())
		{
			continue;
		}
		const FString Kind = Change->GetStringField(TEXT("kind"));
		bFoundChildOrder |= Kind == TEXT("child_order");
		bFoundContent |= Kind == TEXT("content") && Change->GetStringField(TEXT("widget_id")) == TEXT("FirstText");
		TestTrue(TEXT("required diff row marked required"), Change->GetBoolField(TEXT("required")));
		TestEqual(TEXT("required diff row severity"), Change->GetStringField(TEXT("severity")), TEXT("high"));
	}
	TestTrue(TEXT("sibling child-order mismatch reported"), bFoundChildOrder);
	TestTrue(TEXT("TextBlock style mismatch reported as content"), bFoundContent);

	bool bFoundSetFontPatch = false;
	bool bFoundSafeSetTextPatch = false;
	const TArray<TSharedPtr<FJsonValue>>& PatchCandidates = RequiredDiffResult.Result->GetArrayField(TEXT("patch_candidates"));
	for (const TSharedPtr<FJsonValue>& PatchValue : PatchCandidates)
	{
		const TSharedPtr<FJsonObject> Patch = PatchValue.IsValid() ? PatchValue->AsObject() : nullptr;
		bFoundSetFontPatch |= Patch.IsValid() && Patch->GetStringField(TEXT("op")) == TEXT("set_font");
		if (Patch.IsValid() && Patch->GetStringField(TEXT("op")) == TEXT("set_text"))
		{
			bFoundSafeSetTextPatch = Patch->GetStringField(TEXT("text")) == TEXT("First invariant")
				&& !Patch->HasField(TEXT("justification"));
		}
	}
	TestTrue(TEXT("font mismatch routes to set_font owner action"), bFoundSetFontPatch);
	TestTrue(TEXT("set_text patch omits unsupported invariant justification"), bFoundSafeSetTextPatch);
	bool bFoundUnsupportedInvariantJustification = false;
	for (const TSharedPtr<FJsonValue>& UnsupportedValue : RequiredDiffResult.Result->GetArrayField(TEXT("unsupported_fields")))
	{
		const TSharedPtr<FJsonObject> UnsupportedField = UnsupportedValue.IsValid() ? UnsupportedValue->AsObject() : nullptr;
		bFoundUnsupportedInvariantJustification |= UnsupportedField.IsValid()
			&& UnsupportedField->GetStringField(TEXT("field")) == TEXT("content.textStyle");
	}
	TestTrue(TEXT("invariant justification remains explicit unsupported evidence"), bFoundUnsupportedInvariantJustification);

	TSharedPtr<FJsonObject> ApplyDryRunParams = MakeShared<FJsonObject>();
	ApplyDryRunParams->SetStringField(TEXT("asset_path"), AssetPath);
	ApplyDryRunParams->SetArrayField(TEXT("patch"), PatchCandidates);
	ApplyDryRunParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult ApplyDryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), ApplyDryRunParams);
	if (!TestTrue(TEXT("font patch dry-run succeeds"), ApplyDryRunResult.bSuccess && ApplyDryRunResult.Result.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* PlannedSteps = nullptr;
	TestTrue(TEXT("font patch dry-run exposes steps"),
		ApplyDryRunResult.Result->TryGetArrayField(TEXT("steps"), PlannedSteps) && PlannedSteps);
	TestTrue(TEXT("font patch dry-run composes ui.set_font owner"), ContainsStepAction(PlannedSteps, TEXT("set_font")));
	bool bTypefaceForwarded = false;
	if (PlannedSteps)
	{
		for (const TSharedPtr<FJsonValue>& StepValue : *PlannedSteps)
		{
			const TSharedPtr<FJsonObject> Step = StepValue.IsValid() ? StepValue->AsObject() : nullptr;
			if (!Step.IsValid() || Step->GetStringField(TEXT("action")) != TEXT("set_font"))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* StepParams = nullptr;
			bTypefaceForwarded = Step->TryGetObjectField(TEXT("params"), StepParams)
				&& StepParams
				&& StepParams->IsValid()
				&& (*StepParams)->GetStringField(TEXT("typeface")) == TEXT("Bold");
			break;
		}
	}
	TestTrue(TEXT("font patch forwards typeface to ui.set_font"), bTypefaceForwarded);

	TSharedPtr<FJsonObject> OptionalDiffParams = MakeShared<FJsonObject>();
	OptionalDiffParams->SetStringField(TEXT("asset_path"), AssetPath);
	OptionalDiffParams->SetObjectField(TEXT("desired_spec"), DesiredSpec);
	OptionalDiffParams->SetStringField(TEXT("compare_mode"), TEXT("properties"));
	const FMonolithActionResult OptionalDiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), OptionalDiffParams);
	if (!TestTrue(TEXT("optional diff succeeds"), OptionalDiffResult.bSuccess && OptionalDiffResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("optional mismatch remains advisory"), OptionalDiffResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("optional mismatch status"), OptionalDiffResult.Result->GetStringField(TEXT("status")), TEXT("different"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecZeroBorderPaddingConvergenceTest,
	"MonolithUI.SpecMarkup.ZeroBorderPaddingConverges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecZeroBorderPaddingConvergenceTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);
	FMonolithUISlotActions::RegisterActions(Registry);

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SpecZeroBorderPadding");
	TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
	ConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<Border Name=\"RootBorder\" style.padding=\"0\">"
			 "<TextBlock Name=\"Label\" Text=\"Zero padding\" slot.padding=\"0\" />"
			 "</Border>"));
	ConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
	const FMonolithActionResult ConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), ConvertParams);
	if (!TestTrue(TEXT("zero-padding markup converts"), ConvertResult.bSuccess && ConvertResult.Result.IsValid()
		&& ConvertResult.Result->GetBoolField(TEXT("bSuccess"))))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> DesiredSpec = ConvertResult.Result->GetObjectField(TEXT("spec"));
	const TSharedPtr<FJsonObject> DesiredRoot = DesiredSpec->GetObjectField(TEXT("rootWidget"));
	TestTrue(TEXT("converter retains explicit zero Border style padding"),
		DesiredRoot->GetObjectField(TEXT("style"))->HasTypedField<EJson::Object>(TEXT("padding")));
	TSharedPtr<FJsonObject> DesiredLabel;
	if (!TestTrue(TEXT("converted spec contains Label"),
		FindNodeById(DesiredRoot, TEXT("Label"), DesiredLabel)))
	{
		return false;
	}
	TestTrue(TEXT("converter retains explicit zero BorderSlot padding"),
		DesiredLabel->GetObjectField(TEXT("slot"))->HasTypedField<EJson::Object>(TEXT("padding")));

	TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
	BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
	BuildParams->SetObjectField(TEXT("spec"), DesiredSpec);
	BuildParams->SetBoolField(TEXT("overwrite"), true);
	BuildParams->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult BuildResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("build_ui_from_spec"), BuildParams);
	if (!TestTrue(TEXT("zero-padding WBP builds"), BuildResult.bSuccess && BuildResult.Result.IsValid()
		&& BuildResult.Result->GetBoolField(TEXT("bSuccess"))))
	{
		return false;
	}

	FMonolithActionResult LoadError;
	UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(AssetPath, LoadError);
	UBorder* RootBorder = WBP && WBP->WidgetTree ? Cast<UBorder>(WBP->WidgetTree->RootWidget) : nullptr;
	if (!TestNotNull(TEXT("built root Border loads"), RootBorder))
	{
		return false;
	}
	TestTrue(TEXT("builder preserves explicit zero Border padding"), RootBorder->GetPadding() == FMargin(0.f));

	TSharedPtr<FJsonObject> NonZeroPadding = MakeShared<FJsonObject>();
	NonZeroPadding->SetNumberField(TEXT("left"), 4.0);
	NonZeroPadding->SetNumberField(TEXT("top"), 2.0);
	NonZeroPadding->SetNumberField(TEXT("right"), 4.0);
	NonZeroPadding->SetNumberField(TEXT("bottom"), 2.0);
	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("asset_path"), AssetPath);
	SetParams->SetStringField(TEXT("widget_name"), TEXT("RootBorder"));
	SetParams->SetStringField(TEXT("property_name"), TEXT("Padding"));
	SetParams->SetObjectField(TEXT("value"), NonZeroPadding);
	const FMonolithActionResult SetResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("set_widget_property"), SetParams);
	if (!TestTrue(TEXT("set_widget_property accepts Margin object through public schema"),
		SetResult.bSuccess && SetResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("typed Margin echo remains an object"),
		SetResult.Result->TryGetField(TEXT("value_json")).IsValid()
		&& SetResult.Result->TryGetField(TEXT("value_json"))->Type == EJson::Object);
	TestTrue(TEXT("object Margin write reaches Border"), RootBorder->GetPadding() == FMargin(4.f, 2.f, 4.f, 2.f));

	TSharedPtr<FJsonObject> DiffParams = MakeShared<FJsonObject>();
	DiffParams->SetStringField(TEXT("asset_path"), AssetPath);
	DiffParams->SetObjectField(TEXT("desired_spec"), DesiredSpec);
	DiffParams->SetStringField(TEXT("compare_mode"), TEXT("properties"));
	DiffParams->SetBoolField(TEXT("required"), true);
	const FMonolithActionResult DiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), DiffParams);
	if (!TestTrue(TEXT("required padding diff returns evidence"), DiffResult.bSuccess && DiffResult.Result.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("non-zero padding is detected"), DiffResult.Result->GetBoolField(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>> PatchCandidates = DiffResult.Result->GetArrayField(TEXT("patch_candidates"));
	if (!TestEqual(TEXT("padding diff emits only the owner style patch"), PatchCandidates.Num(), 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> PaddingPatch = PatchCandidates[0]->AsObject();
	TestEqual(TEXT("padding diff routes through set_style"), PaddingPatch->GetStringField(TEXT("op")), TEXT("set_style"));
	const TSharedPtr<FJsonObject> PatchPadding = PaddingPatch->GetObjectField(TEXT("padding"));
	TestTrue(TEXT("padding patch retains explicit all-zero Margin"),
		FMath::IsNearlyZero(PatchPadding->GetNumberField(TEXT("left"))) &&
		FMath::IsNearlyZero(PatchPadding->GetNumberField(TEXT("top"))) &&
		FMath::IsNearlyZero(PatchPadding->GetNumberField(TEXT("right"))) &&
		FMath::IsNearlyZero(PatchPadding->GetNumberField(TEXT("bottom"))));

	TSharedPtr<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("asset_path"), AssetPath);
	ApplyParams->SetArrayField(TEXT("patch"), PatchCandidates);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("confirm"), true);
	ApplyParams->SetBoolField(TEXT("compile"), true);
	ApplyParams->SetBoolField(TEXT("save"), false);
	ApplyParams->SetBoolField(TEXT("read_back"), true);
	const FMonolithActionResult ApplyResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), ApplyParams);
	if (!TestTrue(TEXT("zero-padding patch applies"), ApplyResult.bSuccess && ApplyResult.Result.IsValid()
		&& ApplyResult.Result->GetBoolField(TEXT("ok"))))
	{
		return false;
	}
	TestTrue(TEXT("patch restores exact zero Border padding"), RootBorder->GetPadding() == FMargin(0.f));

	const FMonolithActionResult FinalDiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), DiffParams);
	if (!TestTrue(TEXT("final required padding diff returns evidence"),
		FinalDiffResult.bSuccess && FinalDiffResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("final required padding diff converges"), FinalDiffResult.Result->GetBoolField(TEXT("ok")));
	TestFalse(TEXT("final required padding diff is unchanged"), FinalDiffResult.Result->GetBoolField(TEXT("changed")));

	UTextBlock* Label = WBP && WBP->WidgetTree
		? Cast<UTextBlock>(WBP->WidgetTree->FindWidget(TEXT("Label")))
		: nullptr;
	UBorderSlot* LabelSlot = Label ? Cast<UBorderSlot>(Label->Slot) : nullptr;
	if (!TestNotNull(TEXT("Label uses a Border slot"), LabelSlot))
	{
		return false;
	}
	LabelSlot->SetPadding(FMargin(8.f, 6.f, 4.f, 2.f));

	const FMonolithActionResult SlotDiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), DiffParams);
	if (!TestTrue(TEXT("explicit zero slot padding diff returns evidence"),
		SlotDiffResult.bSuccess && SlotDiffResult.Result.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("non-zero slot padding is detected"), SlotDiffResult.Result->GetBoolField(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>> SlotPatchCandidates =
		SlotDiffResult.Result->GetArrayField(TEXT("patch_candidates"));
	if (!TestEqual(TEXT("slot padding diff emits one owner patch"), SlotPatchCandidates.Num(), 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> SlotPatch = SlotPatchCandidates[0]->AsObject();
	TestEqual(TEXT("slot padding diff routes through set_slot_property"),
		SlotPatch->GetStringField(TEXT("op")), TEXT("set_slot_property"));
	const TSharedPtr<FJsonObject> SlotPatchPadding =
		SlotPatch->GetObjectField(TEXT("slot"))->GetObjectField(TEXT("padding"));
	TestTrue(TEXT("slot patch retains explicit all-zero Margin"),
		FMath::IsNearlyZero(SlotPatchPadding->GetNumberField(TEXT("left"))) &&
		FMath::IsNearlyZero(SlotPatchPadding->GetNumberField(TEXT("top"))) &&
		FMath::IsNearlyZero(SlotPatchPadding->GetNumberField(TEXT("right"))) &&
		FMath::IsNearlyZero(SlotPatchPadding->GetNumberField(TEXT("bottom"))));

	ApplyParams->SetArrayField(TEXT("patch"), SlotPatchCandidates);
	const FMonolithActionResult ApplySlotResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), ApplyParams);
	if (!TestTrue(TEXT("zero slot-padding patch applies"),
		ApplySlotResult.bSuccess && ApplySlotResult.Result.IsValid()
		&& ApplySlotResult.Result->GetBoolField(TEXT("ok"))))
	{
		return false;
	}
	TestTrue(TEXT("patch restores exact zero BorderSlot padding"), LabelSlot->GetPadding() == FMargin(0.f));

	const FMonolithActionResult FinalSlotDiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), DiffParams);
	if (!TestTrue(TEXT("final required slot padding diff returns evidence"),
		FinalSlotDiffResult.bSuccess && FinalSlotDiffResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("final required slot padding diff converges"),
		FinalSlotDiffResult.Result->GetBoolField(TEXT("ok")));
	TestFalse(TEXT("final required slot padding diff is unchanged"),
		FinalSlotDiffResult.Result->GetBoolField(TEXT("changed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecChildOrderPatchConvergenceTest,
	"MonolithUI.SpecMarkup.ChildOrderPatchConvergesPanels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecChildOrderPatchConvergenceTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);
	FMonolithUISlotActions::RegisterActions(Registry);

	struct FPanelCase
	{
		FString Label;
		FString AssetSuffix;
		FString CurrentMarkup;
		FString DesiredMarkup;
	};

	const TArray<FPanelCase> Cases = {
		{
			TEXT("VerticalBox"),
			TEXT("VerticalBox"),
			TEXT("<VerticalBox Name=\"RootPanel\">"
				 "<TextBlock Name=\"First\" Text=\"First\" slot.padding=\"1,2,3,4\" />"
				 "<TextBlock Name=\"Second\" Text=\"Second\" slot.padding=\"5,6,7,8\" />"
				 "<TextBlock Name=\"Third\" Text=\"Third\" slot.padding=\"9,10,11,12\" />"
				 "</VerticalBox>"),
			TEXT("<VerticalBox Name=\"RootPanel\">"
				 "<TextBlock Name=\"Third\" Text=\"Third\" slot.padding=\"9,10,11,12\" />"
				 "<TextBlock Name=\"First\" Text=\"First\" slot.padding=\"1,2,3,4\" />"
				 "<TextBlock Name=\"Second\" Text=\"Second\" slot.padding=\"5,6,7,8\" />"
				 "</VerticalBox>")
		},
		{
			TEXT("Overlay"),
			TEXT("Overlay"),
			TEXT("<Overlay Name=\"RootPanel\">"
				 "<TextBlock Name=\"First\" Text=\"First\" slot.padding=\"1,2,3,4\" />"
				 "<TextBlock Name=\"Second\" Text=\"Second\" slot.padding=\"5,6,7,8\" />"
				 "<TextBlock Name=\"Third\" Text=\"Third\" slot.padding=\"9,10,11,12\" />"
				 "</Overlay>"),
			TEXT("<Overlay Name=\"RootPanel\">"
				 "<TextBlock Name=\"Third\" Text=\"Third\" slot.padding=\"9,10,11,12\" />"
				 "<TextBlock Name=\"First\" Text=\"First\" slot.padding=\"1,2,3,4\" />"
				 "<TextBlock Name=\"Second\" Text=\"Second\" slot.padding=\"5,6,7,8\" />"
				 "</Overlay>")
		},
		{
			TEXT("CanvasPanel"),
			TEXT("Canvas"),
			TEXT("<CanvasPanel Name=\"RootPanel\">"
				 "<TextBlock Name=\"First\" Text=\"First\" slot.position=\"10,20\" slot.size=\"100,30\" />"
				 "<TextBlock Name=\"Second\" Text=\"Second\" slot.position=\"30,40\" slot.size=\"110,35\" />"
				 "<TextBlock Name=\"Third\" Text=\"Third\" slot.position=\"50,60\" slot.size=\"120,40\" />"
				 "</CanvasPanel>"),
			TEXT("<CanvasPanel Name=\"RootPanel\">"
				 "<TextBlock Name=\"Third\" Text=\"Third\" slot.position=\"50,60\" slot.size=\"120,40\" />"
				 "<TextBlock Name=\"First\" Text=\"First\" slot.position=\"10,20\" slot.size=\"100,30\" />"
				 "<TextBlock Name=\"Second\" Text=\"Second\" slot.position=\"30,40\" slot.size=\"110,35\" />"
				 "</CanvasPanel>")
		}
	};

	const TArray<FString> DesiredOrder = { TEXT("Third"), TEXT("First"), TEXT("Second") };
	for (const FPanelCase& PanelCase : Cases)
	{
		const FString AssetPath = FString::Printf(
			TEXT("/Game/Tests/Monolith/UI/WBP_SpecChildOrder_%s"),
			*PanelCase.AssetSuffix);

		TSharedPtr<FJsonObject> CurrentConvertParams = MakeShared<FJsonObject>();
		CurrentConvertParams->SetStringField(TEXT("markup"), PanelCase.CurrentMarkup);
		CurrentConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
		const FMonolithActionResult CurrentConvertResult =
			Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), CurrentConvertParams);
		if (!TestTrue(*FString::Printf(TEXT("%s current markup converts"), *PanelCase.Label),
			CurrentConvertResult.bSuccess && CurrentConvertResult.Result.IsValid()))
		{
			return false;
		}

		TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
		BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
		BuildParams->SetObjectField(TEXT("spec"), CurrentConvertResult.Result->GetObjectField(TEXT("spec")));
		BuildParams->SetBoolField(TEXT("overwrite"), true);
		BuildParams->SetBoolField(TEXT("dry_run"), false);
		const FMonolithActionResult BuildResult =
			Registry.ExecuteAction(TEXT("ui"), TEXT("build_ui_from_spec"), BuildParams);
		if (!TestTrue(*FString::Printf(TEXT("%s current WBP builds"), *PanelCase.Label),
			BuildResult.bSuccess && BuildResult.Result.IsValid() && BuildResult.Result->GetBoolField(TEXT("bSuccess"))))
		{
			return false;
		}

		FMonolithActionResult LoadError;
		UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(AssetPath, LoadError);
		if (!TestNotNull(*FString::Printf(TEXT("%s built WBP loads"), *PanelCase.Label), WBP))
		{
			return false;
		}
		UPanelWidget* RootPanel = WBP && WBP->WidgetTree ? Cast<UPanelWidget>(WBP->WidgetTree->RootWidget) : nullptr;
		if (!TestNotNull(*FString::Printf(TEXT("%s root is a panel"), *PanelCase.Label), RootPanel))
		{
			return false;
		}

		TMap<FName, UPanelSlot*> OriginalSlots;
		for (const FName ChildName : { FName(TEXT("First")), FName(TEXT("Second")), FName(TEXT("Third")) })
		{
			UWidget* Child = WBP->WidgetTree->FindWidget(ChildName);
			if (!TestNotNull(*FString::Printf(TEXT("%s child %s exists"), *PanelCase.Label, *ChildName.ToString()), Child))
			{
				return false;
			}
			OriginalSlots.Add(ChildName, Child->Slot);
		}

		TSharedPtr<FJsonObject> DesiredConvertParams = MakeShared<FJsonObject>();
		DesiredConvertParams->SetStringField(TEXT("markup"), PanelCase.DesiredMarkup);
		DesiredConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
		const FMonolithActionResult DesiredConvertResult =
			Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), DesiredConvertParams);
		if (!TestTrue(*FString::Printf(TEXT("%s desired markup converts"), *PanelCase.Label),
			DesiredConvertResult.bSuccess && DesiredConvertResult.Result.IsValid()))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> DesiredSpec = DesiredConvertResult.Result->GetObjectField(TEXT("spec"));

		TSharedPtr<FJsonObject> DiffParams = MakeShared<FJsonObject>();
		DiffParams->SetStringField(TEXT("asset_path"), AssetPath);
		DiffParams->SetObjectField(TEXT("desired_spec"), DesiredSpec);
		DiffParams->SetStringField(TEXT("compare_mode"), TEXT("structural"));
		DiffParams->SetBoolField(TEXT("required"), true);
		const FMonolithActionResult DiffResult =
			Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), DiffParams);
		if (!TestTrue(*FString::Printf(TEXT("%s required diff returns evidence"), *PanelCase.Label),
			DiffResult.bSuccess && DiffResult.Result.IsValid()))
		{
			return false;
		}
		TestFalse(*FString::Printf(TEXT("%s mismatch fails required semantic gate"), *PanelCase.Label),
			DiffResult.Result->GetBoolField(TEXT("ok")));
		TestEqual(*FString::Printf(TEXT("%s reports one child_order change"), *PanelCase.Label),
			static_cast<int32>(DiffResult.Result->GetNumberField(TEXT("change_count"))), 1);

		const TArray<TSharedPtr<FJsonValue>>& Unsupported = DiffResult.Result->GetArrayField(TEXT("unsupported_fields"));
		TestEqual(*FString::Printf(TEXT("%s child order has no unsupported fields"), *PanelCase.Label), Unsupported.Num(), 0);
		const TArray<TSharedPtr<FJsonValue>>& PatchCandidates = DiffResult.Result->GetArrayField(TEXT("patch_candidates"));
		if (!TestEqual(*FString::Printf(TEXT("%s emits deterministic reorder patch count"), *PanelCase.Label),
			PatchCandidates.Num(), DesiredOrder.Num()))
		{
			return false;
		}
		for (int32 Index = 0; Index < PatchCandidates.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Patch = PatchCandidates[Index]->AsObject();
			TestEqual(*FString::Printf(TEXT("%s patch %d owner op"), *PanelCase.Label, Index),
				Patch->GetStringField(TEXT("op")), TEXT("move_widget"));
			TestEqual(*FString::Printf(TEXT("%s patch %d child"), *PanelCase.Label, Index),
				Patch->GetStringField(TEXT("widget_name")), DesiredOrder[Index]);
			TestEqual(*FString::Printf(TEXT("%s patch %d parent"), *PanelCase.Label, Index),
				Patch->GetStringField(TEXT("new_parent_name")), TEXT("RootPanel"));
			TestEqual(*FString::Printf(TEXT("%s patch %d sibling index"), *PanelCase.Label, Index),
				static_cast<int32>(Patch->GetNumberField(TEXT("sibling_index"))), Index);
		}

		TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
		DryRunParams->SetStringField(TEXT("asset_path"), AssetPath);
		DryRunParams->SetArrayField(TEXT("patch"), PatchCandidates);
		DryRunParams->SetBoolField(TEXT("dry_run"), true);
		const FMonolithActionResult DryRunResult =
			Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
		if (!TestTrue(*FString::Printf(TEXT("%s reorder dry-run succeeds"), *PanelCase.Label),
			DryRunResult.bSuccess && DryRunResult.Result.IsValid() && DryRunResult.Result->GetBoolField(TEXT("ok"))))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* DryRunSteps = nullptr;
		TestTrue(*FString::Printf(TEXT("%s dry-run exposes steps"), *PanelCase.Label),
			DryRunResult.Result->TryGetArrayField(TEXT("steps"), DryRunSteps) && DryRunSteps);
		TestEqual(*FString::Printf(TEXT("%s dry-run uses only move owner steps"), *PanelCase.Label),
			CountStepAction(DryRunSteps, TEXT("move_widget")), DesiredOrder.Num());

		TSharedPtr<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
		ApplyParams->SetStringField(TEXT("asset_path"), AssetPath);
		ApplyParams->SetArrayField(TEXT("patch"), PatchCandidates);
		ApplyParams->SetBoolField(TEXT("dry_run"), false);
		ApplyParams->SetBoolField(TEXT("confirm"), true);
		ApplyParams->SetBoolField(TEXT("compile"), true);
		ApplyParams->SetBoolField(TEXT("save"), false);
		ApplyParams->SetBoolField(TEXT("read_back"), true);
		const FMonolithActionResult ApplyResult =
			Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), ApplyParams);
		if (!TestTrue(*FString::Printf(TEXT("%s reorder apply succeeds"), *PanelCase.Label),
			ApplyResult.bSuccess && ApplyResult.Result.IsValid() && ApplyResult.Result->GetBoolField(TEXT("ok"))))
		{
			return false;
		}

		for (int32 Index = 0; Index < DesiredOrder.Num(); ++Index)
		{
			UWidget* Child = WBP->WidgetTree->FindWidget(FName(*DesiredOrder[Index]));
			TestEqual(*FString::Printf(TEXT("%s final child %d"), *PanelCase.Label, Index),
				RootPanel->GetChildAt(Index), Child);
			TestEqual(*FString::Printf(TEXT("%s %s preserves exact slot instance"), *PanelCase.Label, *DesiredOrder[Index]),
				Child ? Child->Slot.Get() : nullptr, OriginalSlots.FindRef(FName(*DesiredOrder[Index])));
		}

		const FMonolithActionResult FinalDiffResult =
			Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), DiffParams);
		if (!TestTrue(*FString::Printf(TEXT("%s final required diff succeeds"), *PanelCase.Label),
			FinalDiffResult.bSuccess && FinalDiffResult.Result.IsValid()))
		{
			return false;
		}
		TestTrue(*FString::Printf(TEXT("%s final required diff is identical"), *PanelCase.Label),
			FinalDiffResult.Result->GetBoolField(TEXT("ok")));
		TestEqual(*FString::Printf(TEXT("%s final required status"), *PanelCase.Label),
			FinalDiffResult.Result->GetStringField(TEXT("status")), TEXT("identical"));
		TestEqual(*FString::Printf(TEXT("%s final structural diff count"), *PanelCase.Label),
			static_cast<int32>(FinalDiffResult.Result->GetNumberField(TEXT("change_count"))), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchLifecycleTest,
	"MonolithUI.SpecMarkup.PatchLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchLifecycleTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);
	FMonolithUISlotActions::RegisterActions(Registry);
	FMonolithUIStylingActions::RegisterActions(Registry);

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SpecPatchLifecycle");

	TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
	ConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<CanvasPanel Name=\"RootCanvas\">"
			 "<TextBlock Name=\"TitleText\" Text=\"Before\" FontSize=\"24\" slot.position=\"24,24\" slot.size=\"320,48\" />"
			 "<TextBlock Name=\"RemoveMe\" Text=\"Temporary\" slot.position=\"24,96\" slot.size=\"240,40\" />"
			 "</CanvasPanel>"));
	ConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
	const FMonolithActionResult ConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), ConvertParams);
	if (!TestTrue(TEXT("convert action succeeds"), ConvertResult.bSuccess && ConvertResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("convert payload bSuccess"), ConvertResult.Result->GetBoolField(TEXT("bSuccess")));

	TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
	BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
	BuildParams->SetObjectField(TEXT("spec"), ConvertResult.Result->GetObjectField(TEXT("spec")));
	BuildParams->SetBoolField(TEXT("overwrite"), true);
	BuildParams->SetBoolField(TEXT("dry_run"), false);

	const FMonolithActionResult BuildResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("build_ui_from_spec"), BuildParams);
	if (!TestTrue(TEXT("build action succeeds"), BuildResult.bSuccess && BuildResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("build payload bSuccess"), BuildResult.Result->GetBoolField(TEXT("bSuccess")));

	TSharedPtr<FJsonObject> DesiredConvertParams = MakeShared<FJsonObject>();
	DesiredConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<CanvasPanel Name=\"RootCanvas\">"
			 "<TextBlock Name=\"TitleText\" Text=\"After Patch\" FontSize=\"26\" FontColor=\"#000000\" slot.position=\"24,24\" slot.size=\"320,48\" />"
			 "<TextBlock Name=\"AddedLabel\" Text=\"Added From Patch\" FontSize=\"18\" slot.position=\"48,152\" slot.size=\"300,44\" />"
			 "</CanvasPanel>"));
	DesiredConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
	const FMonolithActionResult DesiredConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), DesiredConvertParams);
	if (!TestTrue(TEXT("desired convert action succeeds"), DesiredConvertResult.bSuccess && DesiredConvertResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("desired convert payload bSuccess"), DesiredConvertResult.Result->GetBoolField(TEXT("bSuccess")));

	TSharedPtr<FJsonObject> DiffParams = MakeShared<FJsonObject>();
	DiffParams->SetStringField(TEXT("asset_path"), AssetPath);
	DiffParams->SetObjectField(TEXT("desired_spec"), DesiredConvertResult.Result->GetObjectField(TEXT("spec")));
	DiffParams->SetStringField(TEXT("compare_mode"), TEXT("properties"));

	const FMonolithActionResult DiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), DiffParams);
	if (!TestTrue(TEXT("diff action succeeds"), DiffResult.bSuccess && DiffResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("diff payload ok"), DiffResult.Result->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("diff detects changes"), DiffResult.Result->GetBoolField(TEXT("changed")));
	TestTrue(TEXT("diff emits patch candidates"),
		static_cast<int32>(DiffResult.Result->GetNumberField(TEXT("patch_candidate_count"))) >= 3);

	TArray<TSharedPtr<FJsonValue>> PatchOps;

	TSharedPtr<FJsonObject> AddOp = MakeShared<FJsonObject>();
	AddOp->SetStringField(TEXT("op"), TEXT("add_widget"));
	AddOp->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
	AddOp->SetStringField(TEXT("widget_name"), TEXT("AddedLabel"));
	AddOp->SetStringField(TEXT("parent_name"), TEXT("RootCanvas"));
	TSharedPtr<FJsonObject> AddSlot = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AddPos = MakeShared<FJsonObject>();
	AddPos->SetNumberField(TEXT("x"), 48.0);
	AddPos->SetNumberField(TEXT("y"), 152.0);
	TSharedPtr<FJsonObject> AddSize = MakeShared<FJsonObject>();
	AddSize->SetNumberField(TEXT("x"), 300.0);
	AddSize->SetNumberField(TEXT("y"), 44.0);
	AddSlot->SetObjectField(TEXT("position"), AddPos);
	AddSlot->SetObjectField(TEXT("size"), AddSize);
	AddOp->SetObjectField(TEXT("slot"), AddSlot);
	TSharedPtr<FJsonObject> AddContent = MakeShared<FJsonObject>();
	AddContent->SetStringField(TEXT("text"), TEXT("Added From Patch"));
	AddContent->SetNumberField(TEXT("fontSize"), 18.0);
	AddOp->SetObjectField(TEXT("content"), AddContent);
	PatchOps.Add(MakeShared<FJsonValueObject>(AddOp));

	TSharedPtr<FJsonObject> TextOp = MakeShared<FJsonObject>();
	TextOp->SetStringField(TEXT("op"), TEXT("set_text"));
	TextOp->SetStringField(TEXT("widget_name"), TEXT("TitleText"));
	TextOp->SetStringField(TEXT("text"), TEXT("After Patch"));
	TextOp->SetNumberField(TEXT("font_size"), 26.0);
	TextOp->SetStringField(TEXT("text_color"), TEXT("#000000"));
	PatchOps.Add(MakeShared<FJsonValueObject>(TextOp));

	TSharedPtr<FJsonObject> RemoveOp = MakeShared<FJsonObject>();
	RemoveOp->SetStringField(TEXT("op"), TEXT("remove_widget"));
	RemoveOp->SetStringField(TEXT("widget_name"), TEXT("RemoveMe"));
	PatchOps.Add(MakeShared<FJsonValueObject>(RemoveOp));

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), AssetPath);
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("dry-run patch action succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("dry-run payload ok"), DryRunResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("dry-run step_count"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("step_count"))), 4);
	TestEqual(TEXT("dry-run executed_step_count"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("executed_step_count"))), 0);

	TSharedPtr<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("asset_path"), AssetPath);
	ApplyParams->SetArrayField(TEXT("patch"), PatchOps);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("confirm"), true);
	ApplyParams->SetBoolField(TEXT("compile"), true);
	ApplyParams->SetBoolField(TEXT("save"), false);
	ApplyParams->SetBoolField(TEXT("read_back"), true);

	const FMonolithActionResult ApplyResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), ApplyParams);
	if (!TestTrue(TEXT("apply patch action succeeds"), ApplyResult.bSuccess && ApplyResult.Result.IsValid()))
	{
		return false;
	}
	if (!ApplyResult.Result->GetBoolField(TEXT("ok")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		if (ApplyResult.Result->TryGetArrayField(TEXT("steps"), Steps) && Steps)
		{
			for (const TSharedPtr<FJsonValue>& StepValue : *Steps)
			{
				const TSharedPtr<FJsonObject> Step = StepValue.IsValid() ? StepValue->AsObject() : nullptr;
				if (Step.IsValid() && !Step->GetBoolField(TEXT("ok")))
				{
					AddError(FString::Printf(
						TEXT("patch step failed: %s.%s status=%s"),
						*Step->GetStringField(TEXT("namespace")),
						*Step->GetStringField(TEXT("action")),
						*Step->GetStringField(TEXT("status"))));
				}
			}
		}
		return false;
	}
	TestEqual(TEXT("apply status"), ApplyResult.Result->GetStringField(TEXT("status")), TEXT("applied"));
	TestTrue(TEXT("apply compiled"), ApplyResult.Result->GetBoolField(TEXT("compiled")));
	TestTrue(TEXT("roundtrip proof exists"), ApplyResult.Result->HasField(TEXT("roundtrip_proof")));

	TSharedPtr<FJsonObject> DumpParams = MakeShared<FJsonObject>();
	DumpParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult DumpResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("dump_ui_spec"), DumpParams);
	if (!TestTrue(TEXT("dump action succeeds"), DumpResult.bSuccess && DumpResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("dump payload bSuccess"), DumpResult.Result->GetBoolField(TEXT("bSuccess")));

	const TSharedPtr<FJsonObject> DumpedSpec = DumpResult.Result->GetObjectField(TEXT("spec"));
	const TSharedPtr<FJsonObject> DumpedRoot = DumpedSpec->GetObjectField(TEXT("rootWidget"));

	TSharedPtr<FJsonObject> Added;
	TestTrue(TEXT("AddedLabel exists after patch"), FindNodeById(DumpedRoot, TEXT("AddedLabel"), Added));
	if (Added.IsValid())
	{
		TestEqual(TEXT("AddedLabel text"), Added->GetObjectField(TEXT("content"))->GetStringField(TEXT("text")), TEXT("Added From Patch"));
	}

	TSharedPtr<FJsonObject> Title;
	TestTrue(TEXT("TitleText exists after patch"), FindNodeById(DumpedRoot, TEXT("TitleText"), Title));
	if (Title.IsValid())
	{
		TestEqual(TEXT("TitleText patched text"), Title->GetObjectField(TEXT("content"))->GetStringField(TEXT("text")), TEXT("After Patch"));
		TestEqual(TEXT("TitleText patched fontColor"), Title->GetObjectField(TEXT("content"))->GetStringField(TEXT("fontColor")), TEXT("#000000FF"));
	}

	TSharedPtr<FJsonObject> Removed;
	TestFalse(TEXT("RemoveMe removed after patch"), FindNodeById(DumpedRoot, TEXT("RemoveMe"), Removed));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecDiffGraphBindingPreservationTest,
	"MonolithUI.SpecMarkup.DiffGraphBindingPreservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecDiffGraphBindingPreservationTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SpecDiffGraphBinding");

	TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
	ConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<CanvasPanel Name=\"RootCanvas\">"
			 "<TextBlock Name=\"TitleText\" Text=\"Bound\" slot.position=\"24,24\" slot.size=\"320,48\" />"
			 "</CanvasPanel>"));
	ConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);

	const FMonolithActionResult ConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), ConvertParams);
	if (!TestTrue(TEXT("convert action succeeds"), ConvertResult.bSuccess && ConvertResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("convert payload bSuccess"), ConvertResult.Result->GetBoolField(TEXT("bSuccess")));

	TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
	BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
	BuildParams->SetObjectField(TEXT("spec"), ConvertResult.Result->GetObjectField(TEXT("spec")));
	BuildParams->SetBoolField(TEXT("overwrite"), true);
	BuildParams->SetBoolField(TEXT("dry_run"), false);

	const FMonolithActionResult BuildResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("build_ui_from_spec"), BuildParams);
	if (!TestTrue(TEXT("build action succeeds"), BuildResult.bSuccess && BuildResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("build payload bSuccess"), BuildResult.Result->GetBoolField(TEXT("bSuccess")));

	FMonolithActionResult LoadError;
	UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(AssetPath, LoadError);
	if (!TestNotNull(TEXT("built WBP loaded for binding fixture"), WBP))
	{
		return false;
	}

	WBP->Modify();
	WBP->Bindings.Reset();
	FDelegateEditorBinding Binding;
	Binding.ObjectName = TEXT("TitleText");
	Binding.PropertyName = FName(TEXT("Text"));
	Binding.FunctionName = FName(TEXT("GetTitleText"));
	Binding.SourceProperty = FName(TEXT("TitleTextSource"));
	Binding.Kind = EBindingKind::Property;
	WBP->Bindings.Add(Binding);

	TSharedPtr<FJsonObject> DiffParams = MakeShared<FJsonObject>();
	DiffParams->SetStringField(TEXT("asset_path"), AssetPath);
	DiffParams->SetObjectField(TEXT("desired_spec"), ConvertResult.Result->GetObjectField(TEXT("spec")));
	DiffParams->SetStringField(TEXT("compare_mode"), TEXT("structural"));

	const FMonolithActionResult DiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), DiffParams);
	if (!TestTrue(TEXT("diff action succeeds"), DiffResult.bSuccess && DiffResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("diff payload ok"), DiffResult.Result->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* BindingReportPtr = nullptr;
	if (!TestTrue(TEXT("diff has graph binding preservation report"),
		DiffResult.Result->TryGetObjectField(TEXT("graph_binding_preservation"), BindingReportPtr) && BindingReportPtr && BindingReportPtr->IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>& BindingReport = *BindingReportPtr;
	TestEqual(TEXT("binding report schema"), BindingReport->GetStringField(TEXT("schema_version")), TEXT("ui_graph_binding_preservation.v1"));
	TestEqual(TEXT("binding count"), static_cast<int32>(BindingReport->GetNumberField(TEXT("binding_count"))), 1);
	TestEqual(TEXT("preserved binding count"), static_cast<int32>(BindingReport->GetNumberField(TEXT("preserved_by_default_count"))), 1);
	TestEqual(TEXT("at-risk binding count"), static_cast<int32>(BindingReport->GetNumberField(TEXT("at_risk_binding_count"))), 0);
	TestFalse(TEXT("bindings are not represented in UISpec"), BindingReport->GetBoolField(TEXT("represented_in_ui_spec")));

	const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
	if (!TestTrue(TEXT("binding rows exist"), BindingReport->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings && Bindings->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> BindingRow = (*Bindings)[0]->AsObject();
	if (TestTrue(TEXT("binding row object"), BindingRow.IsValid()))
	{
		TestEqual(TEXT("binding row widget"), BindingRow->GetStringField(TEXT("widget_name")), TEXT("TitleText"));
		TestEqual(TEXT("binding row status"), BindingRow->GetStringField(TEXT("status")), TEXT("preserved_by_default"));
		TestEqual(TEXT("binding row patch behavior"), BindingRow->GetStringField(TEXT("patch_behavior")), TEXT("not_modified_by_ui_spec_patch"));
		TestFalse(TEXT("binding row represented in UISpec"), BindingRow->GetBoolField(TEXT("represented_in_ui_spec")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchEffectRoutingDryRunTest,
	"MonolithUI.SpecMarkup.PatchEffectRoutingDryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchEffectRoutingDryRunTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	MonolithUI::FEffectSurfaceActions::Register(Registry);

	TestTrue(TEXT("EffectSurface corner owner action registered"),
		Registry.HasAction(TEXT("ui"), TEXT("set_effect_surface_corners")));
	TestTrue(TEXT("EffectSurface fill owner action registered"),
		Registry.HasAction(TEXT("ui"), TEXT("set_effect_surface_fill")));

	TSharedPtr<FJsonObject> AddOp = MakeShared<FJsonObject>();
	AddOp->SetStringField(TEXT("op"), TEXT("add_widget"));
	AddOp->SetStringField(TEXT("widget_class"), TEXT("EffectSurface"));
	AddOp->SetStringField(TEXT("widget_name"), TEXT("EffectCard"));
	AddOp->SetStringField(TEXT("parent_name"), TEXT("RootCanvas"));

	TSharedPtr<FJsonObject> Effect = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CornerRadii;
	CornerRadii.Add(MakeShared<FJsonValueNumber>(12.0));
	CornerRadii.Add(MakeShared<FJsonValueNumber>(12.0));
	CornerRadii.Add(MakeShared<FJsonValueNumber>(8.0));
	CornerRadii.Add(MakeShared<FJsonValueNumber>(8.0));
	Effect->SetArrayField(TEXT("cornerRadii"), CornerRadii);
	Effect->SetNumberField(TEXT("smoothness"), 1.25);
	Effect->SetStringField(TEXT("solidColor"), TEXT("#20242CFF"));
	Effect->SetNumberField(TEXT("backdropBlurStrength"), 18.0);

	auto MakeShadowLayer = [](double X, double Y, double Blur, double Spread, const FString& Color)
	{
		TSharedPtr<FJsonObject> Layer = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Offset = MakeShared<FJsonObject>();
		Offset->SetNumberField(TEXT("x"), X);
		Offset->SetNumberField(TEXT("y"), Y);
		Layer->SetObjectField(TEXT("offset"), Offset);
		Layer->SetNumberField(TEXT("blur"), Blur);
		Layer->SetNumberField(TEXT("spread"), Spread);
		Layer->SetStringField(TEXT("color"), Color);
		return MakeShared<FJsonValueObject>(Layer);
	};

	TArray<TSharedPtr<FJsonValue>> DropShadows;
	DropShadows.Add(MakeShadowLayer(0.0, 6.0, 16.0, 0.0, TEXT("#00000066")));
	Effect->SetArrayField(TEXT("dropShadows"), DropShadows);

	TArray<TSharedPtr<FJsonValue>> InnerShadows;
	InnerShadows.Add(MakeShadowLayer(0.0, 1.0, 6.0, 0.0, TEXT("#FFFFFFFF")));
	Effect->SetArrayField(TEXT("innerShadows"), InnerShadows);
	AddOp->SetObjectField(TEXT("effect"), Effect);

	TArray<TSharedPtr<FJsonValue>> PatchOps;
	PatchOps.Add(MakeShared<FJsonValueObject>(AddOp));

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_EffectPatchRouting"));
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("effect dry-run patch action succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("effect dry-run payload ok"), DryRunResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("effect dry-run step_count"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("step_count"))), 6);

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!TestTrue(TEXT("effect dry-run returns steps"), DryRunResult.Result->TryGetArrayField(TEXT("steps"), Steps) && Steps))
	{
		return false;
	}

	TestTrue(TEXT("plans add_widget"), ContainsStepAction(Steps, TEXT("add_widget")));
	TestTrue(TEXT("plans set_effect_surface_corners"), ContainsStepAction(Steps, TEXT("set_effect_surface_corners")));
	TestTrue(TEXT("plans set_effect_surface_fill"), ContainsStepAction(Steps, TEXT("set_effect_surface_fill")));
	TestTrue(TEXT("plans set_effect_surface_backdropBlur"), ContainsStepAction(Steps, TEXT("set_effect_surface_backdropBlur")));
	TestTrue(TEXT("plans set_effect_surface_dropShadow"), ContainsStepAction(Steps, TEXT("set_effect_surface_dropShadow")));
	TestTrue(TEXT("plans set_effect_surface_innerShadow"), ContainsStepAction(Steps, TEXT("set_effect_surface_innerShadow")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchStyleRoutingDryRunTest,
	"MonolithUI.SpecMarkup.PatchStyleRoutingDryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchStyleRoutingDryRunTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);

	TSharedPtr<FJsonObject> AddOp = MakeShared<FJsonObject>();
	AddOp->SetStringField(TEXT("op"), TEXT("add_widget"));
	AddOp->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
	AddOp->SetStringField(TEXT("widget_name"), TEXT("FadedLabel"));
	AddOp->SetStringField(TEXT("parent_name"), TEXT("RootCanvas"));

	TSharedPtr<FJsonObject> Style = MakeShared<FJsonObject>();
	Style->SetNumberField(TEXT("opacity"), 0.5);
	Style->SetStringField(TEXT("visibility"), TEXT("Hidden"));
	AddOp->SetObjectField(TEXT("style"), Style);

	TArray<TSharedPtr<FJsonValue>> PatchOps;
	PatchOps.Add(MakeShared<FJsonValueObject>(AddOp));

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_StylePatchRouting"));
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("style dry-run patch action succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("style dry-run payload ok"), DryRunResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("style dry-run step_count"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("step_count"))), 3);

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!TestTrue(TEXT("style dry-run returns steps"), DryRunResult.Result->TryGetArrayField(TEXT("steps"), Steps) && Steps))
	{
		return false;
	}

	TestTrue(TEXT("plans add_widget"), ContainsStepAction(Steps, TEXT("add_widget")));
	TestEqual(TEXT("plans two set_widget_property style steps"), CountStepAction(Steps, TEXT("set_widget_property")), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchTypedStyleRoutingDryRunTest,
	"MonolithUI.SpecMarkup.PatchTypedStyleRoutingDryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchTypedStyleRoutingDryRunTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);

	TArray<TSharedPtr<FJsonValue>> PatchOps;

	TSharedPtr<FJsonObject> SizeBoxOp = MakeShared<FJsonObject>();
	SizeBoxOp->SetStringField(TEXT("op"), TEXT("set_style"));
	SizeBoxOp->SetStringField(TEXT("widget_name"), TEXT("CardSize"));
	SizeBoxOp->SetStringField(TEXT("widget_class"), TEXT("SizeBox"));
	SizeBoxOp->SetNumberField(TEXT("width"), 320.0);
	SizeBoxOp->SetNumberField(TEXT("height"), 180.0);
	SizeBoxOp->SetNumberField(TEXT("minDesiredWidth"), 240.0);
	PatchOps.Add(MakeShared<FJsonValueObject>(SizeBoxOp));

	TSharedPtr<FJsonObject> BorderOp = MakeShared<FJsonObject>();
	BorderOp->SetStringField(TEXT("op"), TEXT("set_style"));
	BorderOp->SetStringField(TEXT("widget_name"), TEXT("CardBorder"));
	BorderOp->SetStringField(TEXT("widget_class"), TEXT("Border"));
	BorderOp->SetStringField(TEXT("background"), TEXT("#20242CFF"));
	TSharedPtr<FJsonObject> Padding = MakeShared<FJsonObject>();
	Padding->SetNumberField(TEXT("left"), 12.0);
	Padding->SetNumberField(TEXT("top"), 8.0);
	Padding->SetNumberField(TEXT("right"), 12.0);
	Padding->SetNumberField(TEXT("bottom"), 8.0);
	BorderOp->SetObjectField(TEXT("padding"), Padding);
	PatchOps.Add(MakeShared<FJsonValueObject>(BorderOp));

	TSharedPtr<FJsonObject> ProgressOp = MakeShared<FJsonObject>();
	ProgressOp->SetStringField(TEXT("op"), TEXT("set_style"));
	ProgressOp->SetStringField(TEXT("widget_name"), TEXT("HealthFill"));
	ProgressOp->SetStringField(TEXT("widget_class"), TEXT("ProgressBar"));
	ProgressOp->SetStringField(TEXT("background"), TEXT("#4CC978FF"));
	PatchOps.Add(MakeShared<FJsonValueObject>(ProgressOp));

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_TypedStylePatchRouting"));
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("typed-style dry-run patch action succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("typed-style dry-run payload ok"), DryRunResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("typed-style dry-run step_count"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("step_count"))), 6);

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!TestTrue(TEXT("typed-style dry-run returns steps"), DryRunResult.Result->TryGetArrayField(TEXT("steps"), Steps) && Steps))
	{
		return false;
	}

	TestEqual(TEXT("plans six set_widget_property typed-style steps"), CountStepAction(Steps, TEXT("set_widget_property")), 6);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchCommonUIStyleRoutingDryRunTest,
	"MonolithUI.SpecMarkup.PatchCommonUIStyleRoutingDryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchCommonUIStyleRoutingDryRunTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);

	TArray<TSharedPtr<FJsonValue>> PatchOps;

	TSharedPtr<FJsonObject> StyleOp = MakeShared<FJsonObject>();
	StyleOp->SetStringField(TEXT("op"), TEXT("apply_style_to_widget"));
	StyleOp->SetStringField(TEXT("widget_name"), TEXT("PrimaryButton"));
	StyleOp->SetStringField(TEXT("style_asset"), TEXT("/Game/UI/Styles/WBP_PrimaryButtonStyle.WBP_PrimaryButtonStyle_C"));
	PatchOps.Add(MakeShared<FJsonValueObject>(StyleOp));

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_CommonUIStylePatchRouting"));
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("common style dry-run action succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("common style dry-run payload ok"), DryRunResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("common style dry-run step_count"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("step_count"))), 1);

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!TestTrue(TEXT("common style dry-run steps array"), DryRunResult.Result->TryGetArrayField(TEXT("steps"), Steps) && Steps))
	{
		return false;
	}
	TestTrue(TEXT("common style dry-run uses apply_style_to_widget"), ContainsStepAction(Steps, TEXT("apply_style_to_widget")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchReplaceRoutingDryRunTest,
	"MonolithUI.SpecMarkup.PatchReplaceRoutingDryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchReplaceRoutingDryRunTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);

	TSharedPtr<FJsonObject> ReplaceOp = MakeShared<FJsonObject>();
	ReplaceOp->SetStringField(TEXT("op"), TEXT("replace_widget"));
	ReplaceOp->SetStringField(TEXT("widget_name"), TEXT("OldPanel"));
	ReplaceOp->SetStringField(TEXT("parent_name"), TEXT("RootCanvas"));
	ReplaceOp->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));

	TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), 24.0);
	Position->SetNumberField(TEXT("y"), 32.0);
	Slot->SetObjectField(TEXT("position"), Position);
	TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
	Size->SetNumberField(TEXT("x"), 240.0);
	Size->SetNumberField(TEXT("y"), 48.0);
	Slot->SetObjectField(TEXT("size"), Size);
	ReplaceOp->SetObjectField(TEXT("slot"), Slot);

	TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
	Content->SetStringField(TEXT("text"), TEXT("Replaced"));
	Content->SetNumberField(TEXT("fontSize"), 22.0);
	Content->SetStringField(TEXT("fontColor"), TEXT("#FFFFFFFF"));
	ReplaceOp->SetObjectField(TEXT("content"), Content);

	TSharedPtr<FJsonObject> Style = MakeShared<FJsonObject>();
	Style->SetNumberField(TEXT("opacity"), 0.75);
	ReplaceOp->SetObjectField(TEXT("style"), Style);

	TArray<TSharedPtr<FJsonValue>> PatchOps;
	PatchOps.Add(MakeShared<FJsonValueObject>(ReplaceOp));

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_ReplacePatchRouting"));
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("replace dry-run action succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("replace dry-run payload ok"), DryRunResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("replace dry-run step_count"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("step_count"))), 4);

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!TestTrue(TEXT("replace dry-run steps array"), DryRunResult.Result->TryGetArrayField(TEXT("steps"), Steps) && Steps))
	{
		return false;
	}

	TestEqual(TEXT("plans one remove_widget"), CountStepAction(Steps, TEXT("remove_widget")), 1);
	TestEqual(TEXT("plans one add_widget"), CountStepAction(Steps, TEXT("add_widget")), 1);
	TestEqual(TEXT("plans one set_text"), CountStepAction(Steps, TEXT("set_text")), 1);
	TestEqual(TEXT("plans one set_widget_property"), CountStepAction(Steps, TEXT("set_widget_property")), 1);
	TestFalse(TEXT("does not register or call duplicate replace action"), ContainsStepAction(Steps, TEXT("replace_widget")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchReplacePreserveChildrenTest,
	"MonolithUI.SpecMarkup.PatchReplacePreserveChildren",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchReplacePreserveChildrenTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);
	FMonolithUISlotActions::RegisterActions(Registry);
	FMonolithUIStylingActions::RegisterActions(Registry);

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_ReplacePreserveChildrenPatch");

	TSharedPtr<FJsonObject> CurrentConvertParams = MakeShared<FJsonObject>();
	CurrentConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<CanvasPanel Name=\"RootCanvas\">"
			 "<CanvasPanel Name=\"OldPanel\" slot.position=\"24,32\" slot.size=\"300,120\">"
			 "<TextBlock Name=\"ChildText\" Text=\"Keep\" slot.position=\"4,4\" slot.size=\"120,32\" />"
			 "</CanvasPanel>"
			 "</CanvasPanel>"));
	CurrentConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
	const FMonolithActionResult CurrentConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), CurrentConvertParams);
	if (!TestTrue(TEXT("current convert action succeeds"), CurrentConvertResult.bSuccess && CurrentConvertResult.Result.IsValid()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
	BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
	BuildParams->SetObjectField(TEXT("spec"), CurrentConvertResult.Result->GetObjectField(TEXT("spec")));
	BuildParams->SetBoolField(TEXT("overwrite"), true);
	BuildParams->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult BuildResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("build_ui_from_spec"), BuildParams);
	if (!TestTrue(TEXT("build action succeeds"), BuildResult.bSuccess && BuildResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("build payload bSuccess"), BuildResult.Result->GetBoolField(TEXT("bSuccess")));

	TSharedPtr<FJsonObject> DesiredConvertParams = MakeShared<FJsonObject>();
	DesiredConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<CanvasPanel Name=\"RootCanvas\">"
			 "<VerticalBox Name=\"OldPanel\" slot.position=\"24,32\" slot.size=\"300,120\">"
			 "<TextBlock Name=\"ChildText\" Text=\"Keep\" />"
			 "</VerticalBox>"
			 "</CanvasPanel>"));
	DesiredConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
	const FMonolithActionResult DesiredConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), DesiredConvertParams);
	if (!TestTrue(TEXT("desired convert action succeeds"), DesiredConvertResult.bSuccess && DesiredConvertResult.Result.IsValid()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> DiffParams = MakeShared<FJsonObject>();
	DiffParams->SetStringField(TEXT("asset_path"), AssetPath);
	DiffParams->SetObjectField(TEXT("desired_spec"), DesiredConvertResult.Result->GetObjectField(TEXT("spec")));
	DiffParams->SetStringField(TEXT("compare_mode"), TEXT("structural"));
	const FMonolithActionResult DiffResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("diff_ui_spec"), DiffParams);
	if (!TestTrue(TEXT("diff action succeeds"), DiffResult.bSuccess && DiffResult.Result.IsValid()))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PatchCandidates = nullptr;
	if (!TestTrue(TEXT("diff patch candidates array"), DiffResult.Result->TryGetArrayField(TEXT("patch_candidates"), PatchCandidates) && PatchCandidates))
	{
		return false;
	}

	bool bFoundPreserveCandidate = false;
	for (const TSharedPtr<FJsonValue>& CandidateValue : *PatchCandidates)
	{
		const TSharedPtr<FJsonObject> Candidate = CandidateValue.IsValid() ? CandidateValue->AsObject() : nullptr;
		if (!Candidate.IsValid())
		{
			continue;
		}

		FString Op;
		FString WidgetName;
		if (Candidate->TryGetStringField(TEXT("op"), Op)
			&& Candidate->TryGetStringField(TEXT("widget_name"), WidgetName)
			&& Op == TEXT("replace_widget")
			&& WidgetName == TEXT("OldPanel"))
		{
			bFoundPreserveCandidate = Candidate->GetBoolField(TEXT("preserve_children"));
			const TArray<TSharedPtr<FJsonValue>>* CandidateChildNames = nullptr;
			bFoundPreserveCandidate &= Candidate->TryGetArrayField(TEXT("child_widget_names"), CandidateChildNames)
				&& CandidateChildNames
				&& CandidateChildNames->Num() == 1
				&& (*CandidateChildNames)[0]->AsString() == TEXT("ChildText");
			break;
		}
	}
	TestTrue(TEXT("diff replace candidate offers preserve_children for same direct child ids"), bFoundPreserveCandidate);

	TSharedPtr<FJsonObject> ReplaceOp = MakeShared<FJsonObject>();
	ReplaceOp->SetStringField(TEXT("op"), TEXT("replace_widget"));
	ReplaceOp->SetStringField(TEXT("widget_name"), TEXT("OldPanel"));
	ReplaceOp->SetStringField(TEXT("parent_name"), TEXT("RootCanvas"));
	ReplaceOp->SetStringField(TEXT("widget_class"), TEXT("VerticalBox"));
	ReplaceOp->SetBoolField(TEXT("preserve_children"), true);
	ReplaceOp->SetBoolField(TEXT("confirm_replace"), true);
	TArray<TSharedPtr<FJsonValue>> ChildNames;
	ChildNames.Add(MakeShared<FJsonValueString>(TEXT("ChildText")));
	ReplaceOp->SetArrayField(TEXT("child_widget_names"), ChildNames);

	TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), 24.0);
	Position->SetNumberField(TEXT("y"), 32.0);
	Slot->SetObjectField(TEXT("position"), Position);
	TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
	Size->SetNumberField(TEXT("x"), 300.0);
	Size->SetNumberField(TEXT("y"), 120.0);
	Slot->SetObjectField(TEXT("size"), Size);
	ReplaceOp->SetObjectField(TEXT("slot"), Slot);

	TArray<TSharedPtr<FJsonValue>> PatchOps;
	PatchOps.Add(MakeShared<FJsonValueObject>(ReplaceOp));

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), AssetPath);
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("preserve replace dry-run succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("preserve replace dry-run ok"), DryRunResult.Result->GetBoolField(TEXT("ok")));

	const TArray<TSharedPtr<FJsonValue>>* DryRunSteps = nullptr;
	if (!TestTrue(TEXT("preserve dry-run steps array"), DryRunResult.Result->TryGetArrayField(TEXT("steps"), DryRunSteps) && DryRunSteps))
	{
		return false;
	}
	TestEqual(TEXT("plans one add_widget"), CountStepAction(DryRunSteps, TEXT("add_widget")), 1);
	TestEqual(TEXT("plans one move_widget"), CountStepAction(DryRunSteps, TEXT("move_widget")), 1);
	TestEqual(TEXT("plans one remove_widget"), CountStepAction(DryRunSteps, TEXT("remove_widget")), 1);
	TestEqual(TEXT("plans one rename_widget"), CountStepAction(DryRunSteps, TEXT("rename_widget")), 1);
	TestFalse(TEXT("does not call duplicate replace action"), ContainsStepAction(DryRunSteps, TEXT("replace_widget")));

	TSharedPtr<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("asset_path"), AssetPath);
	ApplyParams->SetArrayField(TEXT("patch"), PatchOps);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("confirm"), true);
	ApplyParams->SetBoolField(TEXT("compile"), true);
	ApplyParams->SetBoolField(TEXT("save"), false);
	ApplyParams->SetBoolField(TEXT("read_back"), true);
	const FMonolithActionResult ApplyResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), ApplyParams);
	if (!TestTrue(TEXT("preserve replace apply succeeds"), ApplyResult.bSuccess && ApplyResult.Result.IsValid()))
	{
		return false;
	}
	if (!ApplyResult.Result->GetBoolField(TEXT("ok")))
	{
		AddError(TEXT("preserve replace apply returned ok=false"));
		return false;
	}

	TSharedPtr<FJsonObject> DumpParams = MakeShared<FJsonObject>();
	DumpParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult DumpResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("dump_ui_spec"), DumpParams);
	if (!TestTrue(TEXT("dump action succeeds after preserve replace"), DumpResult.bSuccess && DumpResult.Result.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> DumpedSpec = DumpResult.Result->GetObjectField(TEXT("spec"));
	const TSharedPtr<FJsonObject> DumpedRoot = DumpedSpec->GetObjectField(TEXT("rootWidget"));
	TSharedPtr<FJsonObject> ReplacedPanel;
	TestTrue(TEXT("OldPanel stable name remains after replacement"), FindNodeById(DumpedRoot, TEXT("OldPanel"), ReplacedPanel));
	if (ReplacedPanel.IsValid())
	{
		TestEqual(TEXT("OldPanel replacement type"), ReplacedPanel->GetStringField(TEXT("type")), TEXT("VerticalBox"));
		const TArray<TSharedPtr<FJsonValue>>* ReplacedChildren = nullptr;
		TestTrue(TEXT("OldPanel still has children"), ReplacedPanel->TryGetArrayField(TEXT("children"), ReplacedChildren) && ReplacedChildren && ReplacedChildren->Num() == 1);
		TSharedPtr<FJsonObject> ChildText;
		TestTrue(TEXT("ChildText remains under replaced OldPanel"), FindNodeById(ReplacedPanel, TEXT("ChildText"), ChildText));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchBorderBrushRoutingDryRunTest,
	"MonolithUI.SpecMarkup.PatchBorderBrushRoutingDryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchBorderBrushRoutingDryRunTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);

	TSharedPtr<FJsonObject> AddOp = MakeShared<FJsonObject>();
	AddOp->SetStringField(TEXT("op"), TEXT("add_widget"));
	AddOp->SetStringField(TEXT("widget_class"), TEXT("Border"));
	AddOp->SetStringField(TEXT("widget_name"), TEXT("CardBorder"));
	AddOp->SetStringField(TEXT("parent_name"), TEXT("RootCanvas"));

	TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
	Content->SetStringField(TEXT("brushPath"), TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	AddOp->SetObjectField(TEXT("content"), Content);

	TArray<TSharedPtr<FJsonValue>> PatchOps;
	PatchOps.Add(MakeShared<FJsonValueObject>(AddOp));

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_BorderBrushPatchRouting"));
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("border brush dry-run action succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("border brush dry-run payload ok"), DryRunResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("border brush dry-run step_count"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("step_count"))), 2);

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!TestTrue(TEXT("border brush dry-run steps array"), DryRunResult.Result->TryGetArrayField(TEXT("steps"), Steps) && Steps))
	{
		return false;
	}

	TestEqual(TEXT("plans one add_widget"), CountStepAction(Steps, TEXT("add_widget")), 1);
	TestEqual(TEXT("plans one set_brush"), CountStepAction(Steps, TEXT("set_brush")), 1);
	TestFalse(TEXT("does not route Border brush through set_image"), ContainsStepAction(Steps, TEXT("set_image")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchSlotPreflightTest,
	"MonolithUI.SpecMarkup.PatchSlotPreflight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchSlotPreflightTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);
	FMonolithUISlotActions::RegisterActions(Registry);

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SpecPatchSlotPreflight");

	TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
	ConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<VerticalBox Name=\"RootBox\">"
			 "<TextBlock Name=\"RowLabel\" Text=\"Row\" slot.hAlign=\"Fill\" />"
			 "</VerticalBox>"));
	ConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
	const FMonolithActionResult ConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), ConvertParams);
	if (!TestTrue(TEXT("convert action succeeds"), ConvertResult.bSuccess && ConvertResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("convert payload bSuccess"), ConvertResult.Result->GetBoolField(TEXT("bSuccess")));

	TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
	BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
	BuildParams->SetObjectField(TEXT("spec"), ConvertResult.Result->GetObjectField(TEXT("spec")));
	BuildParams->SetBoolField(TEXT("overwrite"), true);
	BuildParams->SetBoolField(TEXT("dry_run"), false);

	const FMonolithActionResult BuildResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("build_ui_from_spec"), BuildParams);
	if (!TestTrue(TEXT("build action succeeds"), BuildResult.bSuccess && BuildResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("build payload bSuccess"), BuildResult.Result->GetBoolField(TEXT("bSuccess")));

	TSharedPtr<FJsonObject> SlotOp = MakeShared<FJsonObject>();
	SlotOp->SetStringField(TEXT("op"), TEXT("set_slot_property"));
	SlotOp->SetStringField(TEXT("widget_name"), TEXT("RowLabel"));
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), 16.0);
	Position->SetNumberField(TEXT("y"), 24.0);
	TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
	Size->SetNumberField(TEXT("x"), 220.0);
	Size->SetNumberField(TEXT("y"), 44.0);
	SlotOp->SetObjectField(TEXT("position"), Position);
	SlotOp->SetObjectField(TEXT("size"), Size);

	TArray<TSharedPtr<FJsonValue>> PatchOps;
	PatchOps.Add(MakeShared<FJsonValueObject>(SlotOp));

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), AssetPath);
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("dry-run preflight action succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("dry-run preflight marks payload not ok"), DryRunResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("dry-run preflight status"), DryRunResult.Result->GetStringField(TEXT("status")), TEXT("planned_with_unsupported"));
	const TArray<TSharedPtr<FJsonValue>>* Unsupported = nullptr;
	if (!TestTrue(TEXT("dry-run reports unsupported slot fields"),
		DryRunResult.Result->TryGetArrayField(TEXT("unsupported_fields"), Unsupported) && Unsupported && Unsupported->Num() >= 2))
	{
		return false;
	}
	TestEqual(TEXT("dry-run executes no child steps"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("executed_step_count"))), 0);

	TSharedPtr<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("asset_path"), AssetPath);
	ApplyParams->SetArrayField(TEXT("patch"), PatchOps);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("confirm"), true);

	const FMonolithActionResult ApplyResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), ApplyParams);
	if (!TestTrue(TEXT("apply preflight action succeeds"), ApplyResult.bSuccess && ApplyResult.Result.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("apply preflight marks payload not ok"), ApplyResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("apply preflight status"), ApplyResult.Result->GetStringField(TEXT("status")), TEXT("unsupported_patch_ops"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISpecPatchBoxSizeRuleRoutingTest,
	"MonolithUI.SpecMarkup.PatchBoxSizeRuleRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISpecPatchBoxSizeRuleRoutingTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithUI::FSpecActions::Register(Registry);
	FMonolithUIActions::RegisterActions(Registry);
	FMonolithUISlotActions::RegisterActions(Registry);

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SpecPatchBoxSizeRuleRouting");
	TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
	ConvertParams->SetStringField(
		TEXT("markup"),
		TEXT("<VerticalBox Name=\"RootBox\"><TextBlock Name=\"ActionTile\" Text=\"Action\" /></VerticalBox>"));
	ConvertParams->SetStringField(TEXT("root_save_path"), AssetPath);
	const FMonolithActionResult ConvertResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("convert_markup_to_ui_spec"), ConvertParams);
	if (!TestTrue(TEXT("convert action succeeds"), ConvertResult.bSuccess && ConvertResult.Result.IsValid()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
	BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
	BuildParams->SetObjectField(TEXT("spec"), ConvertResult.Result->GetObjectField(TEXT("spec")));
	BuildParams->SetBoolField(TEXT("overwrite"), true);
	BuildParams->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult BuildResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("build_ui_from_spec"), BuildParams);
	if (!TestTrue(TEXT("build action succeeds"), BuildResult.bSuccess && BuildResult.Result.IsValid()))
	{
		return false;
	}
	if (!TestTrue(TEXT("build payload bSuccess"), BuildResult.Result->GetBoolField(TEXT("bSuccess"))))
	{
		return false;
	}

	TSharedPtr<FJsonObject> SlotObject = MakeShared<FJsonObject>();
	SlotObject->SetStringField(TEXT("sizeRule"), TEXT("Fill"));
	SlotObject->SetNumberField(TEXT("fillWeight"), 2.0);
	TSharedPtr<FJsonObject> SlotOp = MakeShared<FJsonObject>();
	SlotOp->SetStringField(TEXT("op"), TEXT("set_slot_property"));
	SlotOp->SetStringField(TEXT("widget_name"), TEXT("ActionTile"));
	SlotOp->SetObjectField(TEXT("slot"), SlotObject);

	TArray<TSharedPtr<FJsonValue>> PatchOps;
	PatchOps.Add(MakeShared<FJsonValueObject>(SlotOp));
	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), AssetPath);
	DryRunParams->SetArrayField(TEXT("patch"), PatchOps);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult DryRunResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("apply_ui_spec_patch"), DryRunParams);
	if (!TestTrue(TEXT("box-size dry-run succeeds"), DryRunResult.bSuccess && DryRunResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("box-size dry-run payload is supported"), DryRunResult.Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("box-size dry-run status"), DryRunResult.Result->GetStringField(TEXT("status")), TEXT("planned"));
	TestEqual(TEXT("box-size dry-run plans one owner action"), static_cast<int32>(DryRunResult.Result->GetNumberField(TEXT("step_count"))), 1);

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!TestTrue(TEXT("box-size dry-run steps array"), DryRunResult.Result->TryGetArrayField(TEXT("steps"), Steps) && Steps && Steps->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Step = (*Steps)[0]->AsObject();
	if (!TestTrue(TEXT("box-size step is set_slot_property"), Step.IsValid() && Step->GetStringField(TEXT("action")) == TEXT("set_slot_property")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* RoutedParams = nullptr;
	if (!TestTrue(TEXT("box-size step params exist"), Step->TryGetObjectField(TEXT("params"), RoutedParams) && RoutedParams && RoutedParams->IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("camel sizeRule canonicalized"), (*RoutedParams)->GetStringField(TEXT("size_rule")), TEXT("Fill"));
	TestTrue(TEXT("camel fillWeight canonicalized"), FMath::IsNearlyEqual((*RoutedParams)->GetNumberField(TEXT("fill_weight")), 2.0));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
