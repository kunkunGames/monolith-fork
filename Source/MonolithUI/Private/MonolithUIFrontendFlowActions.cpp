#include "MonolithUIActions.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "GameplayTagContainer.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "MonolithJsonUtils.h"
#include "MonolithUIInternal.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace
{
    struct FFrontendLayerSpec
    {
        FString LayerTag;
        FString WidgetName;
    };

    struct FFrontendScreenSpec
    {
        FString AssetPath;
        FString Role;
        FString ExpectedParentClass;
        FString DesiredFocusWidget;
        bool bRequireCommonActivatable = false;
        TArray<FString> RequiredWidgets;
        TArray<FString> ForbiddenWidgets;
        TMap<FString, FString> ExpectedVariables;
        TMap<FString, FString> ExpectedWidgetClasses;
        TArray<FString> RequiredGraphNeedles;
        TArray<FString> ForbiddenGraphNeedles;
    };

    static TSharedPtr<FJsonObject> MakeCheck(const TCHAR* Name, bool bOk, const FString& Detail)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetBoolField(TEXT("ok"), bOk);
        Obj->SetStringField(TEXT("status"), bOk ? TEXT("ok") : TEXT("failed"));
        Obj->SetStringField(TEXT("detail"), Detail);
        return Obj;
    }

    static void AddCheck(TArray<TSharedPtr<FJsonValue>>& Checks, const TCHAR* Name, bool bOk, const FString& Detail)
    {
        Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(Name, bOk, Detail)));
    }

    static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const TCHAR* Code, const FString& Message, const TCHAR* Severity = TEXT("error"))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("code"), Code);
        Obj->SetStringField(TEXT("message"), Message);
        Obj->SetStringField(TEXT("severity"), Severity);
        Issues.Add(MakeShared<FJsonValueObject>(Obj));
    }

    static TArray<TSharedPtr<FJsonValue>> StringArrayJson(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Arr.Add(MakeShared<FJsonValueString>(Value));
        }
        return Arr;
    }

    static bool ParseStringArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, TArray<FString>& OutValues, FString& OutError)
    {
        OutValues.Reset();
        if (!Obj.IsValid() || !Obj->HasField(FieldName))
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Obj->TryGetArrayField(FieldName, Values) || !Values)
        {
            OutError = FString::Printf(TEXT("%s must be an array of strings."), FieldName);
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Text;
            if (!Value.IsValid() || !Value->TryGetString(Text))
            {
                OutError = FString::Printf(TEXT("%s entries must be strings."), FieldName);
                return false;
            }
            Text.TrimStartAndEndInline();
            if (!Text.IsEmpty())
            {
                OutValues.Add(Text);
            }
        }
        return true;
    }

    static bool ParseStringMapField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, TMap<FString, FString>& OutMap, FString& OutError)
    {
        OutMap.Reset();
        if (!Obj.IsValid() || !Obj->HasField(FieldName))
        {
            return true;
        }

        const TSharedPtr<FJsonObject>* MapObj = nullptr;
        if (!Obj->TryGetObjectField(FieldName, MapObj) || !MapObj)
        {
            OutError = FString::Printf(TEXT("%s must be an object mapping strings to strings."), FieldName);
            return false;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FMonolithJsonUtils::GetFields(*MapObj))
        {
            FString Value;
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Value))
            {
                OutError = FString::Printf(TEXT("%s['%s'] must be a string."), FieldName, *Pair.Key);
                return false;
            }
            OutMap.Add(Pair.Key, Value);
        }
        return true;
    }

    static bool ParseLayerSpecs(const TSharedPtr<FJsonObject>& Params, TArray<FFrontendLayerSpec>& OutSpecs, FString& OutError)
    {
        OutSpecs.Reset();
        if (!Params.IsValid() || !Params->HasField(TEXT("required_layers")))
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params->TryGetArrayField(TEXT("required_layers"), Values) || !Values)
        {
            OutError = TEXT("required_layers must be an array of strings or {layer_tag, widget_name} objects.");
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FFrontendLayerSpec Spec;
            if (Value.IsValid() && Value->TryGetString(Spec.LayerTag))
            {
                Spec.LayerTag.TrimStartAndEndInline();
            }
            else
            {
                const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!Obj.IsValid())
                {
                    OutError = TEXT("required_layers entries must be strings or objects.");
                    return false;
                }
                if (!Obj->TryGetStringField(TEXT("layer_tag"), Spec.LayerTag) && !Obj->TryGetStringField(TEXT("tag"), Spec.LayerTag))
                {
                    OutError = TEXT("required_layers object entries require layer_tag.");
                    return false;
                }
                Obj->TryGetStringField(TEXT("widget_name"), Spec.WidgetName);
                Spec.LayerTag.TrimStartAndEndInline();
                Spec.WidgetName.TrimStartAndEndInline();
            }

            if (Spec.LayerTag.IsEmpty())
            {
                OutError = TEXT("required_layers cannot contain an empty layer tag.");
                return false;
            }
            OutSpecs.Add(MoveTemp(Spec));
        }
        return true;
    }

    static bool ParseScreenSpecs(const TSharedPtr<FJsonObject>& Params, TArray<FFrontendScreenSpec>& OutSpecs, FString& OutError)
    {
        OutSpecs.Reset();
        if (!Params.IsValid() || !Params->HasField(TEXT("screens")))
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params->TryGetArrayField(TEXT("screens"), Values) || !Values)
        {
            OutError = TEXT("screens must be an array of screen spec objects.");
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Obj.IsValid())
            {
                OutError = TEXT("screens entries must be objects.");
                return false;
            }

            FFrontendScreenSpec Spec;
            if (!Obj->TryGetStringField(TEXT("asset_path"), Spec.AssetPath))
            {
                OutError = TEXT("screens entries require asset_path.");
                return false;
            }
            Obj->TryGetStringField(TEXT("role"), Spec.Role);
            Obj->TryGetStringField(TEXT("expected_parent_class"), Spec.ExpectedParentClass);
            Obj->TryGetStringField(TEXT("desired_focus_widget"), Spec.DesiredFocusWidget);
            Obj->TryGetBoolField(TEXT("require_common_activatable"), Spec.bRequireCommonActivatable);

            Spec.AssetPath.TrimStartAndEndInline();
            Spec.Role.TrimStartAndEndInline();
            Spec.ExpectedParentClass.TrimStartAndEndInline();
            Spec.DesiredFocusWidget.TrimStartAndEndInline();

            if (Spec.AssetPath.IsEmpty())
            {
                OutError = TEXT("screens entries cannot contain an empty asset_path.");
                return false;
            }
            if (!ParseStringArrayField(Obj, TEXT("required_widgets"), Spec.RequiredWidgets, OutError) ||
                !ParseStringArrayField(Obj, TEXT("forbidden_widgets"), Spec.ForbiddenWidgets, OutError) ||
                !ParseStringArrayField(Obj, TEXT("required_graph_needles"), Spec.RequiredGraphNeedles, OutError) ||
                !ParseStringArrayField(Obj, TEXT("forbidden_graph_needles"), Spec.ForbiddenGraphNeedles, OutError) ||
                !ParseStringMapField(Obj, TEXT("expected_variables"), Spec.ExpectedVariables, OutError) ||
                !ParseStringMapField(Obj, TEXT("expected_widget_classes"), Spec.ExpectedWidgetClasses, OutError))
            {
                return false;
            }
            OutSpecs.Add(MoveTemp(Spec));
        }
        return true;
    }

    static UClass* ResolveClassSpecifier(FString Specifier)
    {
        Specifier.TrimStartAndEndInline();
        if (Specifier.IsEmpty())
        {
            return nullptr;
        }

        if (UClass* Loaded = StaticLoadClass(UObject::StaticClass(), nullptr, *Specifier))
        {
            return Loaded;
        }
        if (Specifier.StartsWith(TEXT("/Game/")) && !Specifier.EndsWith(TEXT("_C")))
        {
            const FString AssetName = FPackageName::GetLongPackageAssetName(Specifier);
            const FString GeneratedClassPath = Specifier + TEXT(".") + AssetName + TEXT("_C");
            if (UClass* Loaded = StaticLoadClass(UObject::StaticClass(), nullptr, *GeneratedClassPath))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindFirstObject<UClass>(*Specifier, EFindFirstObjectOptions::NativeFirst))
        {
            return Found;
        }
        if (!Specifier.StartsWith(TEXT("U")))
        {
            if (UClass* Found = FindFirstObject<UClass>(*(TEXT("U") + Specifier), EFindFirstObjectOptions::NativeFirst))
            {
                return Found;
            }
        }
        if (!Specifier.StartsWith(TEXT("A")))
        {
            if (UClass* Found = FindFirstObject<UClass>(*(TEXT("A") + Specifier), EFindFirstObjectOptions::NativeFirst))
            {
                return Found;
            }
        }
        return nullptr;
    }

    static TSharedPtr<FJsonObject> WidgetSummary(const UWidget* Widget)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Widget ? Widget->GetName() : FString());
        Obj->SetStringField(TEXT("class"), Widget && Widget->GetClass() ? Widget->GetClass()->GetName() : FString());
        Obj->SetStringField(TEXT("class_path"), Widget && Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString());
        Obj->SetStringField(TEXT("parent"), Widget && Widget->GetParent() ? Widget->GetParent()->GetName() : FString());
        bool bIsVariable = false;
        if (Widget)
        {
            if (const FBoolProperty* VariableProperty = FindFProperty<FBoolProperty>(Widget->GetClass(), TEXT("bIsVariable")))
            {
                bIsVariable = VariableProperty->GetPropertyValue_InContainer(Widget);
            }
        }
        Obj->SetBoolField(TEXT("is_variable"), bIsVariable);
        return Obj;
    }

    static bool NodeReferencesNeedle(const UEdGraphNode* Node, const FString& Needle)
    {
        if (!Node || Needle.IsEmpty())
        {
            return false;
        }

        if (Node->GetName().Contains(Needle) || Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(Needle))
        {
            return true;
        }

        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin)
            {
                continue;
            }
            if (Pin->PinName.ToString().Contains(Needle) ||
                Pin->PinFriendlyName.ToString().Contains(Needle) ||
                Pin->DefaultValue.Contains(Needle) ||
                Pin->AutogeneratedDefaultValue.Contains(Needle) ||
                Pin->DefaultTextValue.ToString().Contains(Needle))
            {
                return true;
            }
        }
        return false;
    }

    static bool BlueprintGraphsContainNeedle(const UBlueprint* Blueprint, const FString& Needle)
    {
        if (!Blueprint || Needle.IsEmpty())
        {
            return false;
        }

        TArray<UEdGraph*> Graphs;
        Graphs.Append(Blueprint->UbergraphPages);
        Graphs.Append(Blueprint->FunctionGraphs);
        Graphs.Append(Blueprint->MacroGraphs);
        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            if (Graph->GetName().Contains(Needle))
            {
                return true;
            }
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (NodeReferencesNeedle(Node, Needle))
                {
                    return true;
                }
            }
        }
        return false;
    }

    static TSharedPtr<FJsonObject> ValidateScreen(
        const FFrontendScreenSpec& Spec,
        bool bIncludeGraphScan,
        UClass* CommonActivatableClass,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        TSharedPtr<FJsonObject> Screen = MakeShared<FJsonObject>();
        Screen->SetStringField(TEXT("asset_path"), Spec.AssetPath);
        Screen->SetStringField(TEXT("role"), Spec.Role);
        Screen->SetBoolField(TEXT("require_common_activatable"), Spec.bRequireCommonActivatable);
        Screen->SetArrayField(TEXT("required_widgets"), StringArrayJson(Spec.RequiredWidgets));
        Screen->SetArrayField(TEXT("forbidden_widgets"), StringArrayJson(Spec.ForbiddenWidgets));

        TArray<TSharedPtr<FJsonValue>> ScreenChecks;
        TArray<TSharedPtr<FJsonValue>> RequiredWidgetRows;
        RequiredWidgetRows.Reserve(Spec.RequiredWidgets.Num());
        TArray<TSharedPtr<FJsonValue>> ForbiddenWidgetRows;
        ForbiddenWidgetRows.Reserve(Spec.ForbiddenWidgets.Num());
        TArray<TSharedPtr<FJsonValue>> VariableRows;
        VariableRows.Reserve(Spec.ExpectedVariables.Num());
        TArray<TSharedPtr<FJsonValue>> WidgetClassRows;
        WidgetClassRows.Reserve(Spec.ExpectedWidgetClasses.Num());
        TArray<TSharedPtr<FJsonValue>> GraphNeedleRows;
        GraphNeedleRows.Reserve(Spec.RequiredGraphNeedles.Num() + Spec.ForbiddenGraphNeedles.Num());

        FMonolithActionResult LoadError;
        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(Spec.AssetPath, LoadError);
        if (!WBP)
        {
            AddCheck(ScreenChecks, TEXT("asset_loadable"), false, LoadError.ErrorMessage);
            AddIssue(Issues, TEXT("screen_asset_not_loadable"), FString::Printf(TEXT("Screen '%s' could not be loaded: %s"), *Spec.AssetPath, *LoadError.ErrorMessage));
            Screen->SetBoolField(TEXT("ok"), false);
            Screen->SetArrayField(TEXT("checks"), ScreenChecks);
            return Screen;
        }

        AddCheck(ScreenChecks, TEXT("asset_loadable"), true, WBP->GetPathName());
        Screen->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetPathName() : FString());
        Screen->SetStringField(TEXT("generated_class"), WBP->GeneratedClass ? WBP->GeneratedClass->GetPathName() : FString());

        if (!Spec.ExpectedParentClass.IsEmpty())
        {
            UClass* ExpectedParentClass = ResolveClassSpecifier(Spec.ExpectedParentClass);
            const bool bParentMatches = ExpectedParentClass && WBP->ParentClass && WBP->ParentClass->IsChildOf(ExpectedParentClass);
            AddCheck(ScreenChecks, TEXT("expected_parent_class"), bParentMatches, Spec.ExpectedParentClass);
            if (!ExpectedParentClass)
            {
                AddIssue(Issues, TEXT("expected_parent_class_not_found"), FString::Printf(TEXT("Expected parent class '%s' for screen '%s' could not be resolved."), *Spec.ExpectedParentClass, *Spec.AssetPath));
            }
            else if (!bParentMatches)
            {
                AddIssue(Issues, TEXT("screen_parent_class_mismatch"), FString::Printf(TEXT("Screen '%s' parent '%s' is not a child of '%s'."), *Spec.AssetPath, WBP->ParentClass ? *WBP->ParentClass->GetPathName() : TEXT("<null>"), *ExpectedParentClass->GetPathName()));
            }
        }

        if (Spec.bRequireCommonActivatable)
        {
            const bool bIsActivatable = CommonActivatableClass && WBP->GeneratedClass && WBP->GeneratedClass->IsChildOf(CommonActivatableClass);
            AddCheck(ScreenChecks, TEXT("common_activatable_screen"), bIsActivatable, CommonActivatableClass ? CommonActivatableClass->GetPathName() : TEXT("CommonUI.CommonActivatableWidget unavailable"));
            if (!bIsActivatable)
            {
                AddIssue(Issues, TEXT("screen_not_common_activatable"), FString::Printf(TEXT("Screen '%s' is not a CommonActivatableWidget-generated Widget Blueprint."), *Spec.AssetPath));
            }
        }

        if (!WBP->WidgetTree)
        {
            AddIssue(Issues, TEXT("screen_widget_tree_missing"), FString::Printf(TEXT("Screen '%s' has no WidgetTree."), *Spec.AssetPath));
            Screen->SetBoolField(TEXT("ok"), false);
            Screen->SetArrayField(TEXT("checks"), ScreenChecks);
            return Screen;
        }

        for (const FString& WidgetName : Spec.RequiredWidgets)
        {
            UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
            TSharedPtr<FJsonObject> Row = WidgetSummary(Widget);
            Row->SetStringField(TEXT("expected_name"), WidgetName);
            Row->SetBoolField(TEXT("found"), Widget != nullptr);
            RequiredWidgetRows.Add(MakeShared<FJsonValueObject>(Row));
            if (!Widget)
            {
                AddIssue(Issues, TEXT("required_widget_missing"), FString::Printf(TEXT("Screen '%s' is missing required widget '%s'."), *Spec.AssetPath, *WidgetName));
            }
        }

        for (const FString& WidgetName : Spec.ForbiddenWidgets)
        {
            UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
            TSharedPtr<FJsonObject> Row = WidgetSummary(Widget);
            Row->SetStringField(TEXT("expected_absent_name"), WidgetName);
            Row->SetBoolField(TEXT("found"), Widget != nullptr);
            ForbiddenWidgetRows.Add(MakeShared<FJsonValueObject>(Row));
            if (Widget)
            {
                AddIssue(Issues, TEXT("forbidden_widget_present"), FString::Printf(TEXT("Screen '%s' still contains forbidden widget '%s'."), *Spec.AssetPath, *WidgetName));
            }
        }

        for (const TPair<FString, FString>& ExpectedClass : Spec.ExpectedWidgetClasses)
        {
            UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*ExpectedClass.Key));
            UClass* ExpectedClassPtr = ResolveClassSpecifier(ExpectedClass.Value);
            const bool bMatches = Widget && ExpectedClassPtr && Widget->IsA(ExpectedClassPtr);
            TSharedPtr<FJsonObject> Row = WidgetSummary(Widget);
            Row->SetStringField(TEXT("expected_widget"), ExpectedClass.Key);
            Row->SetStringField(TEXT("expected_class"), ExpectedClass.Value);
            Row->SetBoolField(TEXT("matches"), bMatches);
            WidgetClassRows.Add(MakeShared<FJsonValueObject>(Row));
            if (!Widget)
            {
                AddIssue(Issues, TEXT("widget_class_target_missing"), FString::Printf(TEXT("Screen '%s' is missing widget '%s' for expected_widget_classes."), *Spec.AssetPath, *ExpectedClass.Key));
            }
            else if (!ExpectedClassPtr)
            {
                AddIssue(Issues, TEXT("expected_widget_class_not_found"), FString::Printf(TEXT("Expected widget class '%s' for '%s' could not be resolved."), *ExpectedClass.Value, *ExpectedClass.Key));
            }
            else if (!bMatches)
            {
                AddIssue(Issues, TEXT("widget_class_mismatch"), FString::Printf(TEXT("Screen '%s' widget '%s' is '%s', not '%s'."), *Spec.AssetPath, *ExpectedClass.Key, Widget->GetClass() ? *Widget->GetClass()->GetPathName() : TEXT("<null>"), *ExpectedClassPtr->GetPathName()));
            }
        }

        for (const TPair<FString, FString>& ExpectedVariable : Spec.ExpectedVariables)
        {
            const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(WBP, FName(*ExpectedVariable.Key));
            const bool bFound = VariableIndex != INDEX_NONE;
            const FString ActualValue = bFound ? WBP->NewVariables[VariableIndex].DefaultValue : FString();
            const bool bMatches = bFound && ActualValue == ExpectedVariable.Value;
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), ExpectedVariable.Key);
            Row->SetBoolField(TEXT("found"), bFound);
            Row->SetStringField(TEXT("expected_default"), ExpectedVariable.Value);
            Row->SetStringField(TEXT("actual_default"), ActualValue);
            Row->SetBoolField(TEXT("matches"), bMatches);
            VariableRows.Add(MakeShared<FJsonValueObject>(Row));
            if (!bFound)
            {
                AddIssue(Issues, TEXT("expected_variable_missing"), FString::Printf(TEXT("Screen '%s' is missing expected variable '%s'."), *Spec.AssetPath, *ExpectedVariable.Key));
            }
            else if (!bMatches)
            {
                AddIssue(Issues, TEXT("expected_variable_default_mismatch"), FString::Printf(TEXT("Screen '%s' variable '%s' default is '%s', expected '%s'."), *Spec.AssetPath, *ExpectedVariable.Key, *ActualValue, *ExpectedVariable.Value));
            }
        }

        if (!Spec.DesiredFocusWidget.IsEmpty())
        {
            const UUserWidget* WidgetCDO = WBP->GeneratedClass ? WBP->GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
            const FName ActualFocus = WidgetCDO ? WidgetCDO->GetDesiredFocusWidgetName() : NAME_None;
            const bool bMatches = ActualFocus == FName(*Spec.DesiredFocusWidget);
            AddCheck(ScreenChecks, TEXT("desired_focus_widget"), bMatches, ActualFocus.ToString());
            if (!bMatches)
            {
                AddIssue(Issues, TEXT("desired_focus_widget_mismatch"), FString::Printf(TEXT("Screen '%s' desired focus is '%s', expected '%s'."), *Spec.AssetPath, *ActualFocus.ToString(), *Spec.DesiredFocusWidget));
            }
        }

        if (bIncludeGraphScan)
        {
            for (const FString& Needle : Spec.RequiredGraphNeedles)
            {
                const bool bFound = BlueprintGraphsContainNeedle(WBP, Needle);
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("needle"), Needle);
                Row->SetStringField(TEXT("expectation"), TEXT("required"));
                Row->SetBoolField(TEXT("found"), bFound);
                GraphNeedleRows.Add(MakeShared<FJsonValueObject>(Row));
                if (!bFound)
                {
                    AddIssue(Issues, TEXT("required_graph_needle_missing"), FString::Printf(TEXT("Screen '%s' graph scan did not find required text '%s'."), *Spec.AssetPath, *Needle));
                }
            }
            for (const FString& Needle : Spec.ForbiddenGraphNeedles)
            {
                const bool bFound = BlueprintGraphsContainNeedle(WBP, Needle);
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("needle"), Needle);
                Row->SetStringField(TEXT("expectation"), TEXT("forbidden"));
                Row->SetBoolField(TEXT("found"), bFound);
                GraphNeedleRows.Add(MakeShared<FJsonValueObject>(Row));
                if (bFound)
                {
                    AddIssue(Issues, TEXT("forbidden_graph_needle_present"), FString::Printf(TEXT("Screen '%s' graph scan still finds forbidden text '%s'."), *Spec.AssetPath, *Needle));
                }
            }
        }
        else if (Spec.RequiredGraphNeedles.Num() > 0 || Spec.ForbiddenGraphNeedles.Num() > 0)
        {
            AddIssue(Warnings, TEXT("graph_scan_disabled"), FString::Printf(TEXT("Screen '%s' supplied graph needles, but include_graph_scan=false."), *Spec.AssetPath), TEXT("warning"));
        }

        Screen->SetBoolField(TEXT("ok"), true);
        Screen->SetArrayField(TEXT("checks"), ScreenChecks);
        Screen->SetArrayField(TEXT("required_widget_results"), RequiredWidgetRows);
        Screen->SetArrayField(TEXT("forbidden_widget_results"), ForbiddenWidgetRows);
        Screen->SetArrayField(TEXT("expected_variable_results"), VariableRows);
        Screen->SetArrayField(TEXT("expected_widget_class_results"), WidgetClassRows);
        Screen->SetArrayField(TEXT("graph_needle_results"), GraphNeedleRows);
        return Screen;
    }

    static TSharedPtr<FJsonObject> ValidateLayout(
        const FString& LayoutAssetPath,
        const TArray<FFrontendLayerSpec>& RequiredLayers,
        UClass* PrimaryGameLayoutClass,
        UClass* ContainerBaseClass,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
        Layout->SetStringField(TEXT("asset_path"), LayoutAssetPath);
        Layout->SetBoolField(TEXT("provided"), !LayoutAssetPath.IsEmpty());

        TArray<TSharedPtr<FJsonValue>> LayerRows;
        LayerRows.Reserve(RequiredLayers.Num());
        if (LayoutAssetPath.IsEmpty())
        {
            if (RequiredLayers.Num() > 0)
            {
                AddIssue(Warnings, TEXT("layout_asset_not_supplied"), TEXT("required_layers were supplied but layout_asset_path is empty; layer widget checks were skipped."), TEXT("warning"));
            }
            Layout->SetStringField(TEXT("status"), TEXT("not_supplied"));
            Layout->SetArrayField(TEXT("required_layer_results"), LayerRows);
            return Layout;
        }

        FMonolithActionResult LoadError;
        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(LayoutAssetPath, LoadError);
        if (!WBP)
        {
            AddIssue(Issues, TEXT("layout_asset_not_loadable"), FString::Printf(TEXT("Layout '%s' could not be loaded: %s"), *LayoutAssetPath, *LoadError.ErrorMessage));
            Layout->SetStringField(TEXT("status"), TEXT("load_failed"));
            Layout->SetStringField(TEXT("error"), LoadError.ErrorMessage);
            return Layout;
        }

        Layout->SetStringField(TEXT("status"), TEXT("loaded"));
        Layout->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetPathName() : FString());
        const bool bIsPrimaryGameLayout = WBP->ParentClass && PrimaryGameLayoutClass && WBP->ParentClass->IsChildOf(PrimaryGameLayoutClass);
        Layout->SetBoolField(TEXT("is_primary_game_layout"), bIsPrimaryGameLayout);
        if (!bIsPrimaryGameLayout)
        {
            AddIssue(Issues, TEXT("layout_wrong_parent"), FString::Printf(TEXT("Layout '%s' parent '%s' is not a CommonGame.PrimaryGameLayout subclass."), *LayoutAssetPath, WBP->ParentClass ? *WBP->ParentClass->GetPathName() : TEXT("<null>")));
        }

        TArray<UWidget*> Widgets;
        if (WBP->WidgetTree)
        {
            WBP->WidgetTree->GetAllWidgets(Widgets);
        }
        Layout->SetNumberField(TEXT("widget_count"), Widgets.Num());

        for (const FFrontendLayerSpec& LayerSpec : RequiredLayers)
        {
            const FGameplayTag LayerTag = FGameplayTag::RequestGameplayTag(FName(*LayerSpec.LayerTag), /*ErrorIfNotFound=*/false);
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("layer_tag"), LayerSpec.LayerTag);
            Row->SetBoolField(TEXT("layer_tag_registered"), LayerTag.IsValid());
            Row->SetStringField(TEXT("widget_name"), LayerSpec.WidgetName);
            if (!LayerTag.IsValid())
            {
                AddIssue(Issues, TEXT("layer_tag_not_registered"), FString::Printf(TEXT("GameplayTag '%s' is not registered."), *LayerSpec.LayerTag));
            }

            bool bFoundCandidate = false;
            for (UWidget* Widget : Widgets)
            {
                if (!Widget || !ContainerBaseClass || !Widget->IsA(ContainerBaseClass))
                {
                    continue;
                }
                if (LayerSpec.WidgetName.IsEmpty() || Widget->GetName() == LayerSpec.WidgetName)
                {
                    bFoundCandidate = true;
                    Row->SetObjectField(TEXT("candidate"), WidgetSummary(Widget));
                    break;
                }
            }
            Row->SetBoolField(TEXT("container_candidate_found"), bFoundCandidate);
            Row->SetStringField(TEXT("register_layer_proof_status"), bFoundCandidate ? TEXT("container_candidate_found_graph_wiring_not_proven") : TEXT("missing_container_candidate"));
            if (!bFoundCandidate)
            {
                AddIssue(Issues, TEXT("required_layer_container_missing"), FString::Printf(TEXT("Layout '%s' has no CommonActivatable container candidate for layer '%s'%s."), *LayoutAssetPath, *LayerSpec.LayerTag, LayerSpec.WidgetName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" widget '%s'"), *LayerSpec.WidgetName)));
            }
            else
            {
                AddIssue(Warnings, TEXT("register_layer_graph_not_proven"), FString::Printf(TEXT("Layout '%s' has a layer container candidate for '%s', but static validation does not prove RegisterLayer graph wiring."), *LayoutAssetPath, *LayerSpec.LayerTag), TEXT("warning"));
            }
            LayerRows.Add(MakeShared<FJsonValueObject>(Row));
        }

        Layout->SetArrayField(TEXT("required_layer_results"), LayerRows);
        return Layout;
    }
}

FMonolithActionResult FMonolithUIActions::HandleValidateFrontendMenuFlow(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    TArray<FFrontendLayerSpec> RequiredLayers;
    TArray<FFrontendScreenSpec> Screens;
    if (!ParseLayerSpecs(Params, RequiredLayers, Error) || !ParseScreenSpecs(Params, Screens, Error))
    {
        return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
    }

    const FString LayoutAssetPath = MonolithUIInternal::GetOptionalString(Params, TEXT("layout_asset_path"));
    const FString DialogClassPath = MonolithUIInternal::GetOptionalString(Params, TEXT("dialog_class"));
    const FString ModalLayerTagName = MonolithUIInternal::GetOptionalString(Params, TEXT("modal_layer_tag"), TEXT("UI.Layer.Modal"));
    const bool bRequireLayoutAsset = MonolithUIInternal::GetOptionalBool(Params, TEXT("require_layout_asset"), false);
    const bool bRequireDialog = MonolithUIInternal::GetOptionalBool(Params, TEXT("require_dialog"), false);
    const bool bIncludeGraphScan = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_graph_scan"), true);

    if (bRequireLayoutAsset && LayoutAssetPath.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("validate_frontend_menu_flow: require_layout_asset=true requires layout_asset_path."), FMonolithJsonUtils::ErrInvalidParams);
    }
    if (LayoutAssetPath.IsEmpty() && RequiredLayers.IsEmpty() && Screens.IsEmpty() && DialogClassPath.IsEmpty() && !bRequireDialog)
    {
        return FMonolithActionResult::Error(TEXT("validate_frontend_menu_flow requires at least one of layout_asset_path, required_layers, screens, dialog_class, or require_dialog=true."), FMonolithJsonUtils::ErrInvalidParams);
    }

    UClass* PrimaryGameLayoutClass = ResolveClassSpecifier(TEXT("/Script/CommonGame.PrimaryGameLayout"));
    UClass* ContainerBaseClass = ResolveClassSpecifier(TEXT("/Script/CommonUI.CommonActivatableWidgetContainerBase"));
    UClass* CommonActivatableClass = ResolveClassSpecifier(TEXT("/Script/CommonUI.CommonActivatableWidget"));
    UClass* DialogBaseClass = ResolveClassSpecifier(TEXT("/Script/CommonGame.CommonGameDialog"));

    const FGameplayTag ModalLayerTag = FGameplayTag::RequestGameplayTag(FName(*ModalLayerTagName), /*ErrorIfNotFound=*/false);

    TArray<TSharedPtr<FJsonValue>> Checks;
    Checks.Reserve(5);
    TArray<TSharedPtr<FJsonValue>> Issues;
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TArray<TSharedPtr<FJsonValue>> ScreenRows;
    ScreenRows.Reserve(Screens.Num());

    AddCheck(Checks, TEXT("primary_game_layout_available"), PrimaryGameLayoutClass != nullptr, PrimaryGameLayoutClass ? PrimaryGameLayoutClass->GetPathName() : FString());
    AddCheck(Checks, TEXT("common_activatable_container_available"), ContainerBaseClass != nullptr, ContainerBaseClass ? ContainerBaseClass->GetPathName() : FString());
    AddCheck(Checks, TEXT("common_activatable_widget_available"), CommonActivatableClass != nullptr, CommonActivatableClass ? CommonActivatableClass->GetPathName() : FString());
    AddCheck(Checks, TEXT("common_game_dialog_available"), DialogBaseClass != nullptr, DialogBaseClass ? DialogBaseClass->GetPathName() : FString());
    AddCheck(Checks, TEXT("modal_layer_tag_registered"), ModalLayerTag.IsValid(), ModalLayerTagName);

    if (!ModalLayerTag.IsValid())
    {
        AddIssue(Issues, TEXT("modal_layer_tag_not_registered"), FString::Printf(TEXT("GameplayTag '%s' is not registered."), *ModalLayerTagName));
    }
    if (!PrimaryGameLayoutClass && (!LayoutAssetPath.IsEmpty() || RequiredLayers.Num() > 0))
    {
        AddIssue(Issues, TEXT("primary_game_layout_unavailable"), TEXT("CommonGame.PrimaryGameLayout is unavailable."));
    }
    if (!ContainerBaseClass && (!LayoutAssetPath.IsEmpty() || RequiredLayers.Num() > 0))
    {
        AddIssue(Issues, TEXT("common_activatable_container_unavailable"), TEXT("CommonUI.CommonActivatableWidgetContainerBase is unavailable."));
    }
    if (!CommonActivatableClass)
    {
        for (const FFrontendScreenSpec& Screen : Screens)
        {
            if (Screen.bRequireCommonActivatable)
            {
                AddIssue(Issues, TEXT("common_activatable_widget_unavailable"), TEXT("CommonUI.CommonActivatableWidget is unavailable."));
                break;
            }
        }
    }

    TSharedPtr<FJsonObject> Dialog = MakeShared<FJsonObject>();
    Dialog->SetStringField(TEXT("class_path"), DialogClassPath);
    Dialog->SetBoolField(TEXT("required"), bRequireDialog);
    if (DialogClassPath.IsEmpty())
    {
        Dialog->SetStringField(TEXT("status"), TEXT("not_supplied"));
        if (bRequireDialog)
        {
            AddIssue(Issues, TEXT("dialog_class_missing"), TEXT("require_dialog=true but dialog_class is empty."));
        }
    }
    else
    {
        UClass* DialogClass = ResolveClassSpecifier(DialogClassPath);
        const bool bIsDialog = DialogClass && DialogBaseClass && DialogClass->IsChildOf(DialogBaseClass);
        Dialog->SetStringField(TEXT("resolved_class"), DialogClass ? DialogClass->GetPathName() : FString());
        Dialog->SetBoolField(TEXT("found"), DialogClass != nullptr);
        Dialog->SetBoolField(TEXT("is_common_game_dialog"), bIsDialog);
        Dialog->SetBoolField(TEXT("abstract"), DialogClass ? DialogClass->HasAnyClassFlags(CLASS_Abstract) : false);
        Dialog->SetBoolField(TEXT("deprecated"), DialogClass ? DialogClass->HasAnyClassFlags(CLASS_Deprecated) : false);
        Dialog->SetStringField(TEXT("status"), bIsDialog ? TEXT("ok") : TEXT("invalid"));
        if (!DialogClass)
        {
            AddIssue(Issues, TEXT("dialog_class_not_found"), FString::Printf(TEXT("Dialog class '%s' could not be resolved."), *DialogClassPath));
        }
        else if (!bIsDialog)
        {
            AddIssue(Issues, TEXT("dialog_class_wrong_parent"), FString::Printf(TEXT("Dialog class '%s' is not a CommonGame.CommonGameDialog subclass."), *DialogClass->GetPathName()));
        }
        if (DialogClass && DialogClass->HasAnyClassFlags(CLASS_Abstract))
        {
            AddIssue(Issues, TEXT("dialog_class_abstract"), FString::Printf(TEXT("Dialog class '%s' is abstract."), *DialogClass->GetPathName()));
        }
        if (DialogClass && DialogClass->HasAnyClassFlags(CLASS_Deprecated))
        {
            AddIssue(Issues, TEXT("dialog_class_deprecated"), FString::Printf(TEXT("Dialog class '%s' is deprecated."), *DialogClass->GetPathName()));
        }
    }

    TSharedPtr<FJsonObject> Layout = ValidateLayout(LayoutAssetPath, RequiredLayers, PrimaryGameLayoutClass, ContainerBaseClass, Issues, Warnings);
    for (const FFrontendScreenSpec& ScreenSpec : Screens)
    {
        ScreenRows.Add(MakeShared<FJsonValueObject>(ValidateScreen(ScreenSpec, bIncludeGraphScan, CommonActivatableClass, Issues, Warnings)));
    }

    TArray<TSharedPtr<FJsonValue>> Limitations;
    Limitations.Add(MakeShared<FJsonValueString>(TEXT("Static validation does not prove runtime screen navigation, button click reachability, CommonUI input routing, or RegisterLayer graph wiring.")));
    Limitations.Add(MakeShared<FJsonValueString>(TEXT("Graph needle checks are lexical over loaded Blueprint graph node titles, names, and pin defaults; they are not semantic execution-flow proof.")));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("action"), TEXT("validate_frontend_menu_flow"));
    Result->SetBoolField(TEXT("ok"), Issues.Num() == 0);
    Result->SetStringField(TEXT("modal_layer_tag"), ModalLayerTagName);
    Result->SetBoolField(TEXT("modal_layer_tag_registered"), ModalLayerTag.IsValid());
    Result->SetBoolField(TEXT("include_graph_scan"), bIncludeGraphScan);
    Result->SetObjectField(TEXT("layout"), Layout);
    Result->SetObjectField(TEXT("dialog"), Dialog);
    Result->SetArrayField(TEXT("screens"), ScreenRows);
    Result->SetArrayField(TEXT("checks"), Checks);
    Result->SetArrayField(TEXT("issues"), Issues);
    Result->SetArrayField(TEXT("warnings"), Warnings);
    Result->SetArrayField(TEXT("limitations"), Limitations);
    Result->SetStringField(TEXT("overall_status"), Issues.Num() == 0 ? (Warnings.Num() == 0 ? TEXT("ok") : TEXT("warnings")) : TEXT("issues"));
    return FMonolithActionResult::Success(Result);
}
