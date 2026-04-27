// Category D: Navigation, Focus, Rules — 5 actions
// 3.D.1 set_widget_navigation
// 3.D.2 set_initial_focus_target
// 3.D.3 force_focus [RUNTIME]
// 3.D.4 get_focus_path [RUNTIME]
// 3.D.5 request_refresh_focus [RUNTIME]
#include "MonolithCommonUIHelpers.h"

#if WITH_COMMONUI

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "CommonActivatableWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Types/NavigationMetaData.h"
#include "Blueprint/WidgetNavigation.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UObjectIterator.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Framework/Application/SlateApplication.h"
#include "Blueprint/UserWidget.h"

namespace MonolithCommonUINavigation
{
	static bool ParseDirection(const FString& S, EUINavigation& Out)
	{
		if (S.Equals(TEXT("Up"), ESearchCase::IgnoreCase))       { Out = EUINavigation::Up; return true; }
		if (S.Equals(TEXT("Down"), ESearchCase::IgnoreCase))     { Out = EUINavigation::Down; return true; }
		if (S.Equals(TEXT("Left"), ESearchCase::IgnoreCase))     { Out = EUINavigation::Left; return true; }
		if (S.Equals(TEXT("Right"), ESearchCase::IgnoreCase))    { Out = EUINavigation::Right; return true; }
		if (S.Equals(TEXT("Next"), ESearchCase::IgnoreCase))     { Out = EUINavigation::Next; return true; }
		if (S.Equals(TEXT("Previous"), ESearchCase::IgnoreCase)) { Out = EUINavigation::Previous; return true; }
		return false;
	}

	static bool ParseRule(const FString& S, EUINavigationRule& Out)
	{
		if (S.Equals(TEXT("Escape"), ESearchCase::IgnoreCase))   { Out = EUINavigationRule::Escape; return true; }
		if (S.Equals(TEXT("Stop"), ESearchCase::IgnoreCase))     { Out = EUINavigationRule::Stop; return true; }
		if (S.Equals(TEXT("Wrap"), ESearchCase::IgnoreCase))     { Out = EUINavigationRule::Wrap; return true; }
		if (S.Equals(TEXT("Explicit"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::Explicit; return true; }
		if (S.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))   { Out = EUINavigationRule::Custom; return true; }
		if (S.Equals(TEXT("CustomBoundary"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::CustomBoundary; return true; }
		return false;
	}

	// ----- 3.D.1 set_widget_navigation -----------------------------------------

	static FMonolithActionResult HandleSetWidgetNavigation(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName, DirStr, RuleStr;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("widget_name"), WidgetName) ||
			!Params->TryGetStringField(TEXT("direction"), DirStr) ||
			!Params->TryGetStringField(TEXT("rule"), RuleStr))
			return FMonolithActionResult::Error(TEXT("wbp_path, widget_name, direction, rule required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		EUINavigation Dir;
		if (!ParseDirection(DirStr, Dir))
			return FMonolithActionResult::Error(TEXT("direction must be Up/Down/Left/Right/Next/Previous"));
		EUINavigationRule Rule;
		if (!ParseRule(RuleStr, Rule))
			return FMonolithActionResult::Error(TEXT("rule must be Escape/Stop/Wrap/Explicit/Custom/CustomBoundary"));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		if (Rule == EUINavigationRule::Explicit)
		{
			FString ExplicitTargetName;
			if (!Params->TryGetStringField(TEXT("explicit_target"), ExplicitTargetName))
				return FMonolithActionResult::Error(TEXT("rule=Explicit requires explicit_target widget name"));

			UWidget* ExplicitTarget = nullptr;
			Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
			{
				if (!ExplicitTarget && W && W->GetFName() == FName(*ExplicitTargetName))
					ExplicitTarget = W;
			});
			if (!ExplicitTarget)
				return FMonolithActionResult::Error(FString::Printf(TEXT("explicit_target '%s' not found"), *ExplicitTargetName));

			Target->SetNavigationRuleExplicit(Dir, ExplicitTarget);
		}
		else
		{
			Target->SetNavigationRuleBase(Dir, Rule);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("direction"), DirStr);
		Result->SetStringField(TEXT("rule"), RuleStr);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.D.2 set_initial_focus_target --------------------------------------
	// UCommonActivatableWidget uses GetDesiredFocusTarget() which can be overridden via
	// NativeGetDesiredFocusTarget / BP_GetDesiredFocusTarget. In CDO terms there's no direct
	// property — the canonical pattern is to implement BP_GetDesiredFocusTarget in the WBP.
	// For a programmatic setter, we stamp a property we name "InitialFocusTarget" on the CDO
	// if the widget class exposes one; otherwise fall through to a metadata sentinel the
	// WBP author can query in their GetDesiredFocusTarget override. For most Leviathan use,
	// the simpler mechanism is: set a named UWidget* reference property. We probe for a
	// known-name convention (`DesiredFocusTarget` / `InitialFocusTarget`) on the WBP class.

	static FMonolithActionResult HandleSetInitialFocusTarget(const TSharedPtr<FJsonObject>& Params)
	{
		FString TargetWidgetName;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("target_widget"), TargetWidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and target_widget required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->GeneratedClass)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		if (!Wbp->GeneratedClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
			return FMonolithActionResult::Error(TEXT("WBP is not a UCommonActivatableWidget — focus target requires activatable parent"));

		// Ensure the target widget exists
		bool bTargetFound = false;
		if (Wbp->WidgetTree)
		{
			Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
			{
				if (!bTargetFound && W && W->GetFName() == FName(*TargetWidgetName))
					bTargetFound = true;
			});
		}
		if (!bTargetFound)
			return FMonolithActionResult::Error(FString::Printf(TEXT("target_widget '%s' not in WBP tree"), *TargetWidgetName));

		// Store target as FName via a dedicated UPROPERTY if present, else record as metadata.
		FName PropNames[] = { TEXT("DesiredFocusTargetName"), TEXT("InitialFocusTargetName") };
		UCommonActivatableWidget* CDO = Cast<UCommonActivatableWidget>(Wbp->GeneratedClass->GetDefaultObject());
		bool bStored = false;
		for (const FName& PN : PropNames)
		{
			if (FNameProperty* P = FindFProperty<FNameProperty>(Wbp->GeneratedClass, PN))
			{
				P->SetPropertyValue_InContainer(CDO, FName(*TargetWidgetName));
				bStored = true;
				break;
			}
		}

		if (!bStored)
		{
			return FMonolithActionResult::Error(
				TEXT("WBP has no DesiredFocusTargetName / InitialFocusTargetName FName UPROPERTY. Add one and override NativeGetDesiredFocusTarget to return WidgetTree->FindWidget(DesiredFocusTargetName)."));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("target_widget"), TargetWidgetName);
		Result->SetBoolField(TEXT("stored_to_cdo"), bStored);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Runtime widget lookup by name in PIE viewport widgets ----------------

	static UWidget* FindWidgetInPIE(const FName& WidgetName)
	{
		UWorld* PIE = MonolithCommonUI::GetPIEWorld();
		if (!PIE) return nullptr;

		for (TObjectIterator<UWidget> It; It; ++It)
		{
			UWidget* W = *It;
			if (!W || W->GetWorld() != PIE) continue;
			if (W->GetFName() == WidgetName) return W;
		}
		return nullptr;
	}

	// ----- 3.D.3 force_focus [RUNTIME] -----------------------------------------

	static FMonolithActionResult HandleForceFocus(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"));

		FString WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("widget_name required"));

		UWidget* Target = FindWidgetInPIE(FName(*WidgetName));
		if (!Target)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Widget '%s' not found in PIE"), *WidgetName));

		Target->SetFocus();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("widget_class"), Target->GetClass()->GetName());
		Result->SetBoolField(TEXT("has_user_focus"), Target->HasUserFocus(0));
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.D.4 get_focus_path [RUNTIME] --------------------------------------

	static FMonolithActionResult HandleGetFocusPath(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"));

		if (!FSlateApplication::IsInitialized())
			return FMonolithActionResult::Error(TEXT("Slate application not initialized"));

		FSlateApplication& App = FSlateApplication::Get();
		TSharedPtr<SWidget> FocusedSlate = App.GetUserFocusedWidget(0);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!FocusedSlate.IsValid())
		{
			Result->SetBoolField(TEXT("has_focus"), false);
			return FMonolithActionResult::Success(Result);
		}

		Result->SetBoolField(TEXT("has_focus"), true);
		Result->SetStringField(TEXT("focused_slate_type"), FocusedSlate->GetTypeAsString());

		// Walk up the Slate tree describing each widget
		TArray<TSharedPtr<FJsonValue>> Path;
		TSharedPtr<SWidget> Cur = FocusedSlate;
		while (Cur.IsValid())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("type"), Cur->GetTypeAsString());
			Entry->SetStringField(TEXT("tag"), Cur->GetTag().ToString());
			Path.Add(MakeShared<FJsonValueObject>(Entry));
			Cur = Cur->GetParentWidget();
		}
		Result->SetArrayField(TEXT("slate_path_leaf_to_root"), Path);

		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.D.5 request_refresh_focus [RUNTIME] -------------------------------

	static FMonolithActionResult HandleRequestRefreshFocus(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"));

		FString WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("widget_name required (activatable widget FName)"));

		UWorld* PIE = MonolithCommonUI::GetPIEWorld();
		UCommonActivatableWidget* Found = nullptr;
		for (TObjectIterator<UCommonActivatableWidget> It; It; ++It)
		{
			UCommonActivatableWidget* W = *It;
			if (!W || W->GetWorld() != PIE) continue;
			if (W->GetFName() == FName(*WidgetName)) { Found = W; break; }
		}
		if (!Found)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Activatable widget '%s' not found in PIE"), *WidgetName));

		Found->RequestRefreshFocus();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetBoolField(TEXT("refresh_requested"), true);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Registration --------------------------------------------------------

	void Register(FMonolithToolRegistry& Registry)
	{
		const FString Cat(TEXT("CommonUI"));

		Registry.RegisterAction(
			TEXT("ui"), TEXT("set_widget_navigation"),
			TEXT("Set a UWidget's navigation rule for a direction (Up/Down/Left/Right/Next/Previous)"),
			FMonolithActionHandler::CreateStatic(&HandleSetWidgetNavigation),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of widget whose nav to set"))
				.Required(TEXT("direction"), TEXT("string"), TEXT("Up|Down|Left|Right|Next|Previous"))
				.Required(TEXT("rule"), TEXT("string"), TEXT("Escape|Stop|Wrap|Explicit|Custom|CustomBoundary"))
				.Optional(TEXT("explicit_target"), TEXT("string"), TEXT("Required when rule=Explicit: target widget name"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("set_initial_focus_target"),
			TEXT("Store DesiredFocusTargetName FName UPROPERTY on a UCommonActivatableWidget CDO. WBP must expose this UPROPERTY and override NativeGetDesiredFocusTarget."),
			FMonolithActionHandler::CreateStatic(&HandleSetInitialFocusTarget),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("UCommonActivatableWidget blueprint path"))
				.Required(TEXT("target_widget"), TEXT("string"), TEXT("FName of widget to focus when screen activates"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("force_focus"),
			TEXT("[RUNTIME] Call SetFocus on a named widget in the live PIE viewport"),
			FMonolithActionHandler::CreateStatic(&HandleForceFocus),
			FParamSchemaBuilder()
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("FName of UWidget in active UMG tree"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("get_focus_path"),
			TEXT("[RUNTIME] Return the Slate focus chain leaf→root for diagnosing 'why is input eaten' bugs"),
			FMonolithActionHandler::CreateStatic(&HandleGetFocusPath),
			nullptr,
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("request_refresh_focus"),
			TEXT("[RUNTIME] Call RequestRefreshFocus on an active UCommonActivatableWidget (after dynamic content swap)"),
			FMonolithActionHandler::CreateStatic(&HandleRequestRefreshFocus),
			FParamSchemaBuilder()
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("FName of active UCommonActivatableWidget"))
				.Build(),
			Cat);
	}
}

#endif // WITH_COMMONUI
