// Copyright tumourlove. All Rights Reserved.
// MonolithUISpecActions.cpp
//
// Phase H — registers the two LLM-facing entry points:
//
//   * `ui::build_ui_from_spec`   — the centerpiece transactional builder.
//   * `ui::dump_ui_spec_schema`  — JSON-Schema-style description of
//                                  FUISpecDocument + the live allowlist
//                                  projection.
//
// The MCP handler is intentionally thin — it's a parse + dispatch shim that
// hands off to FUISpecBuilder. All the policy decisions (atomicity, dry-run,
// strict mode) live in the builder; the action handler exists only to map
// the JSON wire shape onto FUISpecBuilderInputs and back to a JSON response.

#include "Actions/MonolithUISpecActions.h"

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"

#include "Spec/UISpec.h"
#include "Spec/UISpecValidator.h"
#include "Spec/UISpecBuilder.h"
// Phase J: dump_ui_spec serializer.
#include "Spec/UISpecSerializer.h"

#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UITypeRegistry.h"
#include "Registry/UIPropertyAllowlist.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Containers/Map.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Texture2D.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Modules/ModuleManager.h"
#include "WidgetBlueprint.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "XmlFile.h"

#include "MonolithUICommon.h"

namespace MonolithUI::SpecActionsInternal
{
    // ------------------------------------------------------------------
    // JSON -> FUISpecDocument parser. Manual walk because FUISpecNode
    // recurses through TSharedPtr (not a UPROPERTY) — FJsonObjectConverter
    // can't traverse it.

    static void ParseSlot(const TSharedPtr<FJsonObject>& Obj, FUISpecSlot& OutSlot);
    static void ParseStyle(const TSharedPtr<FJsonObject>& Obj, FUISpecStyle& OutStyle);
    static void ParseContent(const TSharedPtr<FJsonObject>& Obj, FUISpecContent& OutContent);
    static void ParseEffect(const TSharedPtr<FJsonObject>& Obj, FUISpecEffect& OutEffect);
    static void ParseCommonUI(const TSharedPtr<FJsonObject>& Obj, FUISpecCommonUI& OutCUI);
    static TSharedPtr<FUISpecNode> ParseNode(const TSharedPtr<FJsonObject>& Obj);

    static FName GetFNameField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        FString S;
        if (Obj.IsValid() && Obj->TryGetStringField(Field, S) && !S.IsEmpty())
        {
            return FName(*S);
        }
        return NAME_None;
    }

    static double GetNumberField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Def = 0.0)
    {
        double V = Def;
        if (Obj.IsValid())
        {
            Obj->TryGetNumberField(Field, V);
        }
        return V;
    }

    static bool GetBoolField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, bool Def = false)
    {
        bool V = Def;
        if (Obj.IsValid())
        {
            Obj->TryGetBoolField(Field, V);
        }
        return V;
    }

    static FString GetStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        FString S;
        if (Obj.IsValid())
        {
            Obj->TryGetStringField(Field, S);
        }
        return S;
    }

    static FVector2D ParseVec2(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid()) return FVector2D::ZeroVector;
        return FVector2D(GetNumberField(Obj, TEXT("x")), GetNumberField(Obj, TEXT("y")));
    }

    static FLinearColor ParseColor(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        FString S;
        if (Obj.IsValid() && Obj->TryGetStringField(Field, S) && !S.IsEmpty())
        {
            FLinearColor C;
            if (MonolithUI::TryParseColor(S, C))
            {
                return C;
            }
        }
        // Default to white (matches the FUISpecStyle default).
        return FLinearColor::White;
    }

    static FMargin ParseMargin(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetObjectField(Field, Sub) || !Sub) return FMargin();
        return FMargin(
            (float)GetNumberField(*Sub, TEXT("left")),
            (float)GetNumberField(*Sub, TEXT("top")),
            (float)GetNumberField(*Sub, TEXT("right")),
            (float)GetNumberField(*Sub, TEXT("bottom")));
    }

    static void ParseSlot(const TSharedPtr<FJsonObject>& Obj, FUISpecSlot& OutSlot)
    {
        if (!Obj.IsValid()) return;
        OutSlot.AnchorPreset = GetFNameField(Obj, TEXT("anchorPreset"));
        OutSlot.bAutoSize    = GetBoolField(Obj, TEXT("autoSize"), false);
        OutSlot.ZOrder       = (int32)GetNumberField(Obj, TEXT("zOrder"));
        OutSlot.HAlign       = GetFNameField(Obj, TEXT("hAlign"));
        OutSlot.VAlign       = GetFNameField(Obj, TEXT("vAlign"));
        OutSlot.SizeRule     = GetFNameField(Obj, TEXT("sizeRule"));
        OutSlot.FillWeight   = (float)GetNumberField(Obj, TEXT("fillWeight"), 1.0);

        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if (Obj->TryGetObjectField(TEXT("position"), Sub) && Sub) OutSlot.Position = ParseVec2(*Sub);
        if (Obj->TryGetObjectField(TEXT("size"), Sub)     && Sub) OutSlot.Size     = ParseVec2(*Sub);
        if (Obj->TryGetObjectField(TEXT("alignment"), Sub) && Sub) OutSlot.Alignment = ParseVec2(*Sub);
        OutSlot.Padding = ParseMargin(Obj, TEXT("padding"));
    }

    static void ParseStyle(const TSharedPtr<FJsonObject>& Obj, FUISpecStyle& OutStyle)
    {
        if (!Obj.IsValid()) return;
        OutStyle.Width        = (float)GetNumberField(Obj, TEXT("width"));
        OutStyle.Height       = (float)GetNumberField(Obj, TEXT("height"));
        OutStyle.Opacity      = (float)GetNumberField(Obj, TEXT("opacity"), 1.0);
        OutStyle.BorderWidth  = (float)GetNumberField(Obj, TEXT("borderWidth"));
        OutStyle.bUseCustomSize = GetBoolField(Obj, TEXT("useCustomSize"));
        double DesiredValue = 0.0;
        OutStyle.bOverrideMinDesiredWidth = GetBoolField(Obj, TEXT("overrideMinDesiredWidth"));
        if (Obj->TryGetNumberField(TEXT("minDesiredWidth"), DesiredValue))
        {
            OutStyle.bOverrideMinDesiredWidth = true;
            OutStyle.MinDesiredWidth = (float)DesiredValue;
        }
        OutStyle.bOverrideMinDesiredHeight = GetBoolField(Obj, TEXT("overrideMinDesiredHeight"));
        if (Obj->TryGetNumberField(TEXT("minDesiredHeight"), DesiredValue))
        {
            OutStyle.bOverrideMinDesiredHeight = true;
            OutStyle.MinDesiredHeight = (float)DesiredValue;
        }
        OutStyle.bOverrideMaxDesiredWidth = GetBoolField(Obj, TEXT("overrideMaxDesiredWidth"));
        if (Obj->TryGetNumberField(TEXT("maxDesiredWidth"), DesiredValue))
        {
            OutStyle.bOverrideMaxDesiredWidth = true;
            OutStyle.MaxDesiredWidth = (float)DesiredValue;
        }
        OutStyle.bOverrideMaxDesiredHeight = GetBoolField(Obj, TEXT("overrideMaxDesiredHeight"));
        if (Obj->TryGetNumberField(TEXT("maxDesiredHeight"), DesiredValue))
        {
            OutStyle.bOverrideMaxDesiredHeight = true;
            OutStyle.MaxDesiredHeight = (float)DesiredValue;
        }
        OutStyle.Visibility   = GetFNameField(Obj, TEXT("visibility"));
        OutStyle.Background   = ParseColor(Obj, TEXT("background"));
        OutStyle.BorderColor  = ParseColor(Obj, TEXT("borderColor"));
        OutStyle.Padding      = ParseMargin(Obj, TEXT("padding"));
    }

    static void ParseContent(const TSharedPtr<FJsonObject>& Obj, FUISpecContent& OutContent)
    {
        if (!Obj.IsValid()) return;
        OutContent.Text        = GetStringField(Obj, TEXT("text"));
        OutContent.FontSize    = (float)GetNumberField(Obj, TEXT("fontSize"));
        OutContent.WrapMode    = GetFNameField(Obj, TEXT("wrapMode"));
        OutContent.BrushPath   = GetStringField(Obj, TEXT("brushPath"));
        OutContent.Placeholder = GetStringField(Obj, TEXT("placeholder"));
        OutContent.FontColor   = ParseColor(Obj, TEXT("fontColor"));
    }

    static void ParseEffect(const TSharedPtr<FJsonObject>& Obj, FUISpecEffect& OutEffect)
    {
        if (!Obj.IsValid()) return;
        OutEffect.Smoothness = (float)GetNumberField(Obj, TEXT("smoothness"), 1.0);
        OutEffect.SolidColor = ParseColor(Obj, TEXT("solidColor"));
        OutEffect.BackdropBlurStrength = (float)GetNumberField(Obj, TEXT("backdropBlurStrength"));

        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Obj->TryGetArrayField(TEXT("cornerRadii"), Arr) && Arr && Arr->Num() >= 4)
        {
            OutEffect.CornerRadii = FVector4(
                (*Arr)[0]->AsNumber(),
                (*Arr)[1]->AsNumber(),
                (*Arr)[2]->AsNumber(),
                (*Arr)[3]->AsNumber());
        }

        // Phase J: parse drop / inner shadow arrays so the roundtrip is
        // symmetric with FUISpecSerializer's array-path read. The Phase H
        // deferral is closed here -- the arrays land in Out*Shadows and the
        // EffectSurfaceBuilder reads them via the typed FEffectShadow path.
        auto ParseShadowArray = [&Obj](const TCHAR* Field, TArray<FUISpecEffectShadow>& Out, bool bDefaultInset)
        {
            const TArray<TSharedPtr<FJsonValue>>* ShArr = nullptr;
            if (!Obj->TryGetArrayField(Field, ShArr) || !ShArr) return;
            for (const TSharedPtr<FJsonValue>& V : *ShArr)
            {
                const TSharedPtr<FJsonObject>* SObj = nullptr;
                if (!V.IsValid() || !V->TryGetObject(SObj) || !SObj) continue;
                FUISpecEffectShadow S;
                const TSharedPtr<FJsonObject>* OffObj = nullptr;
                if ((*SObj)->TryGetObjectField(TEXT("offset"), OffObj) && OffObj)
                {
                    S.Offset = FVector2D(
                        GetNumberField(*OffObj, TEXT("x")),
                        GetNumberField(*OffObj, TEXT("y")));
                }
                S.Blur   = (float)GetNumberField(*SObj, TEXT("blur"));
                S.Spread = (float)GetNumberField(*SObj, TEXT("spread"));
                S.bInset = bDefaultInset;
                (*SObj)->TryGetBoolField(TEXT("inset"), S.bInset);
                FString HexColor;
                if ((*SObj)->TryGetStringField(TEXT("color"), HexColor) && !HexColor.IsEmpty())
                {
                    FLinearColor C;
                    if (MonolithUI::TryParseColor(HexColor, C))
                    {
                        S.Color = C;
                    }
                }
                Out.Add(S);
            }
        };

        ParseShadowArray(TEXT("dropShadows"),  OutEffect.DropShadows,  /*bDefaultInset=*/false);
        ParseShadowArray(TEXT("innerShadows"), OutEffect.InnerShadows, /*bDefaultInset=*/true);
    }

    static void ParseCommonUI(const TSharedPtr<FJsonObject>& Obj, FUISpecCommonUI& OutCUI)
    {
        if (!Obj.IsValid()) return;
        OutCUI.InputLayer = GetFNameField(Obj, TEXT("inputLayer"));
        OutCUI.InputMode  = GetFNameField(Obj, TEXT("inputMode"));

        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Obj->TryGetArrayField(TEXT("styleRefs"), Arr) && Arr)
        {
            OutCUI.StyleRefs.Reserve(Arr->Num());
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    OutCUI.StyleRefs.Add(FName(*V->AsString()));
                }
            }
        }
    }

    static TSharedPtr<FUISpecNode> ParseNode(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid()) return nullptr;
        TSharedPtr<FUISpecNode> Node = MakeShared<FUISpecNode>();

        Node->Type            = GetFNameField(Obj, TEXT("type"));
        Node->Id              = GetFNameField(Obj, TEXT("id"));
        Node->StyleRef        = GetFNameField(Obj, TEXT("styleRef"));
        Node->CustomClassPath = GetStringField(Obj, TEXT("customClassPath"));

        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if (Obj->TryGetObjectField(TEXT("slot"),    Sub) && Sub) ParseSlot(*Sub, Node->Slot);
        if (Obj->TryGetObjectField(TEXT("style"),   Sub) && Sub) ParseStyle(*Sub, Node->Style);
        if (Obj->TryGetObjectField(TEXT("content"), Sub) && Sub) ParseContent(*Sub, Node->Content);

        if (Obj->TryGetObjectField(TEXT("effect"), Sub) && Sub)
        {
            ParseEffect(*Sub, Node->Effect);
            Node->bHasEffect = true;
        }

        if (Obj->TryGetObjectField(TEXT("commonUI"), Sub) && Sub)
        {
            ParseCommonUI(*Sub, Node->CommonUI);
            Node->bHasCommonUI = true;
        }

        const TArray<TSharedPtr<FJsonValue>>* AnimRefArr = nullptr;
        if (Obj->TryGetArrayField(TEXT("animationRefs"), AnimRefArr) && AnimRefArr)
        {
            Node->AnimationRefs.Reserve(AnimRefArr->Num());
            for (const TSharedPtr<FJsonValue>& V : *AnimRefArr)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    Node->AnimationRefs.Add(FName(*V->AsString()));
                }
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* ChildArr = nullptr;
        if (Obj->TryGetArrayField(TEXT("children"), ChildArr) && ChildArr)
        {
            Node->Children.Reserve(ChildArr->Num());
            for (const TSharedPtr<FJsonValue>& V : *ChildArr)
            {
                const TSharedPtr<FJsonObject>* ChildObj = nullptr;
                if (V.IsValid() && V->TryGetObject(ChildObj) && ChildObj)
                {
                    if (TSharedPtr<FUISpecNode> Child = ParseNode(*ChildObj))
                    {
                        Node->Children.Add(Child);
                    }
                }
            }
        }
        return Node;
    }

    /**
     * Entry point: parse a `spec` JSON object into a populated FUISpecDocument.
     * Returns false on syntactic problems with `spec` (we bubble up a single
     * error finding into OutValidation so the caller can short-circuit
     * uniformly with the validator-fail path).
     */
    static bool ParseDocument(
        const TSharedPtr<FJsonObject>& SpecObj,
        FUISpecDocument& OutDoc,
        FUISpecValidationResult& OutValidation)
    {
        if (!SpecObj.IsValid())
        {
            FUISpecError E;
            E.Severity = EUISpecErrorSeverity::Error;
            E.Category = TEXT("Parse");
            E.Message  = TEXT("`spec` is missing or not a JSON object.");
            OutValidation.Errors.Add(MoveTemp(E));
            OutValidation.bIsValid = false;
            return false;
        }

        OutDoc.Version     = (int32)GetNumberField(SpecObj, TEXT("version"), 1);
        OutDoc.Name        = GetStringField(SpecObj, TEXT("name"));
        OutDoc.ParentClass = GetStringField(SpecObj, TEXT("parentClass"));
        OutDoc.bTreatWarningsAsErrors =
            GetBoolField(SpecObj, TEXT("treatWarningsAsErrors"), false);

        // Metadata.
        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if (SpecObj->TryGetObjectField(TEXT("metadata"), Sub) && Sub)
        {
            OutDoc.Metadata.AuthoringTool = GetStringField(*Sub, TEXT("authoringTool"));
            OutDoc.Metadata.SourceFile    = GetStringField(*Sub, TEXT("sourceFile"));
            OutDoc.Metadata.Author        = GetStringField(*Sub, TEXT("author"));
            OutDoc.Metadata.Description   = GetStringField(*Sub, TEXT("description"));
        }

        // Styles map.
        if (SpecObj->TryGetObjectField(TEXT("styles"), Sub) && Sub)
        {
            for (const auto& Pair : (*Sub)->Values)
            {
                const TSharedPtr<FJsonObject>* StyleObj = nullptr;
                if (Pair.Value.IsValid() && Pair.Value->TryGetObject(StyleObj) && StyleObj)
                {
                    FUISpecStyle Style;
                    ParseStyle(*StyleObj, Style);
                    const FString StyleName(Pair.Key.Len(), *Pair.Key);
                    OutDoc.Styles.Add(FName(*StyleName), Style);
                }
            }
        }

        // Animations.
        const TArray<TSharedPtr<FJsonValue>>* AnimArr = nullptr;
        if (SpecObj->TryGetArrayField(TEXT("animations"), AnimArr) && AnimArr)
        {
            OutDoc.Animations.Reserve(AnimArr->Num());
            for (const TSharedPtr<FJsonValue>& V : *AnimArr)
            {
                const TSharedPtr<FJsonObject>* AnimObj = nullptr;
                if (V.IsValid() && V->TryGetObject(AnimObj) && AnimObj)
                {
                    FUISpecAnimation A;
                    A.Name           = GetFNameField(*AnimObj, TEXT("name"));
                    A.TargetWidgetId = GetFNameField(*AnimObj, TEXT("targetWidgetId"));
                    A.TargetProperty = GetFNameField(*AnimObj, TEXT("targetProperty"));
                    A.Duration       = (float)GetNumberField(*AnimObj, TEXT("duration"));
                    A.Delay          = (float)GetNumberField(*AnimObj, TEXT("delay"));
                    A.Easing         = GetFNameField(*AnimObj, TEXT("easing"));
                    A.LoopMode       = GetFNameField(*AnimObj, TEXT("loopMode"));
                    A.bAutoPlay      = GetBoolField(*AnimObj, TEXT("autoPlay"), false);
                    // Keyframes: iterate JSON array; rich tangent fields supported.
                    const TArray<TSharedPtr<FJsonValue>>* KFArr = nullptr;
                    if ((*AnimObj)->TryGetArrayField(TEXT("keyframes"), KFArr) && KFArr)
                    {
                        A.Keyframes.Reserve(KFArr->Num());
                        for (const TSharedPtr<FJsonValue>& KV : *KFArr)
                        {
                            const TSharedPtr<FJsonObject>* KObj = nullptr;
                            if (KV.IsValid() && KV->TryGetObject(KObj) && KObj)
                            {
                                FUISpecKeyframe K;
                                K.Time         = (float)GetNumberField(*KObj, TEXT("time"));
                                K.ScalarValue  = (float)GetNumberField(*KObj, TEXT("scalarValue"));
                                K.Easing       = GetFNameField(*KObj, TEXT("easing"));
                                K.bUseCustomTangents = GetBoolField(*KObj, TEXT("useCustomTangents"), false);
                                K.ArriveTangent = (float)GetNumberField(*KObj, TEXT("arriveTangent"));
                                K.LeaveTangent  = (float)GetNumberField(*KObj, TEXT("leaveTangent"));
                                K.ArriveWeight  = (float)GetNumberField(*KObj, TEXT("arriveWeight"));
                                K.LeaveWeight   = (float)GetNumberField(*KObj, TEXT("leaveWeight"));
                                A.Keyframes.Add(K);
                            }
                        }
                    }
                    OutDoc.Animations.Add(A);
                }
            }
        }

        // Root.
        if (SpecObj->TryGetObjectField(TEXT("rootWidget"), Sub) && Sub)
        {
            OutDoc.Root = ParseNode(*Sub);
        }

        return true;
    }

    // ------------------------------------------------------------------
    // Markup -> FUISpecDocument parser.
    //
    // This intentionally does not mutate Widget Blueprints. It adapts the
    // useful part of external HTML/XML layout import into Monolith's canonical
    // FUISpecDocument path so build/diff/proof workflows remain owner-action
    // based instead of cloning apply_layout-style direct writes.

    struct FMarkupParseContext
    {
        const FUITypeRegistry* TypeRegistry = nullptr;
        FUISpecValidationResult Validation;
        bool bStrict = true;
        int32 AutoId = 0;
    };

    static bool AttrEquals(const FString& Actual, const TCHAR* Expected)
    {
        return Actual.Equals(Expected, ESearchCase::IgnoreCase);
    }

    static bool TryGetXmlAttribute(const FXmlNode& Node, const TCHAR* FieldName, FString& OutValue)
    {
        for (const FXmlAttribute& Attr : Node.GetAttributes())
        {
            if (AttrEquals(Attr.GetTag(), FieldName))
            {
                OutValue = Attr.GetValue();
                return true;
            }
        }
        return false;
    }

    static FString GetXmlAttribute(const FXmlNode& Node, const TCHAR* FieldName)
    {
        FString Value;
        TryGetXmlAttribute(Node, FieldName, Value);
        return Value;
    }

    static void AddMarkupFinding(
        FMarkupParseContext& Context,
        EUISpecErrorSeverity Severity,
        const TCHAR* Category,
        const FString& JsonPath,
        const FName& WidgetId,
        const FString& Message,
        const FString& SuggestedFix = FString(),
        const TArray<FString>& ValidOptions = TArray<FString>())
    {
        FUISpecError Finding;
        Finding.Severity = Severity;
        Finding.Category = Category;
        Finding.JsonPath = JsonPath;
        Finding.WidgetId = WidgetId;
        Finding.Message = Message;
        Finding.SuggestedFix = SuggestedFix;
        Finding.ValidOptions = ValidOptions;

        if (Severity == EUISpecErrorSeverity::Warning)
        {
            Context.Validation.Warnings.Add(MoveTemp(Finding));
        }
        else
        {
            Context.Validation.Errors.Add(MoveTemp(Finding));
        }
    }

    static void AddMarkupAttributeFinding(
        FMarkupParseContext& Context,
        const FName& WidgetId,
        const FString& JsonPath,
        const FString& AttributeName,
        const FString& Message,
        const FString& SuggestedFix)
    {
        AddMarkupFinding(
            Context,
            Context.bStrict ? EUISpecErrorSeverity::Error : EUISpecErrorSeverity::Warning,
            TEXT("MarkupAttribute"),
            JsonPath,
            WidgetId,
            FString::Printf(TEXT("%s Attribute: %s"), *Message, *AttributeName),
            SuggestedFix);
    }

    static FString MakeMarkupJsonPath(const FString& ParentPath, const FString& NodeId)
    {
        if (ParentPath.IsEmpty())
        {
            return FString::Printf(TEXT("/rootWidget[%s]"), *NodeId);
        }
        return FString::Printf(TEXT("%s/children[%s]"), *ParentPath, *NodeId);
    }

    static bool TryParseMarkupFloat(const FString& Text, float& OutValue)
    {
        return FDefaultValueHelper::ParseFloat(Text.TrimStartAndEnd(), OutValue);
    }

    static bool TryParseMarkupInt(const FString& Text, int32& OutValue)
    {
        return FDefaultValueHelper::ParseInt(Text.TrimStartAndEnd(), OutValue);
    }

    static bool TryParseMarkupBool(const FString& Text, bool& OutValue)
    {
        const FString Normalized = Text.TrimStartAndEnd().ToLower();
        if (Normalized == TEXT("true") || Normalized == TEXT("1") || Normalized == TEXT("yes"))
        {
            OutValue = true;
            return true;
        }
        if (Normalized == TEXT("false") || Normalized == TEXT("0") || Normalized == TEXT("no"))
        {
            OutValue = false;
            return true;
        }
        return false;
    }

    static void SplitMarkupNumbers(const FString& Text, TArray<FString>& OutParts)
    {
        FString Normalized = Text;
        Normalized.ReplaceInline(TEXT(","), TEXT(" "));
        Normalized.ParseIntoArrayWS(OutParts);
    }

    static bool TryParseMarkupVec2(const FString& Text, FVector2D& OutValue)
    {
        TArray<FString> Parts;
        SplitMarkupNumbers(Text, Parts);
        if (Parts.Num() != 2)
        {
            return false;
        }

        float X = 0.f;
        float Y = 0.f;
        if (!TryParseMarkupFloat(Parts[0], X) || !TryParseMarkupFloat(Parts[1], Y))
        {
            return false;
        }
        OutValue = FVector2D(X, Y);
        return true;
    }

    static bool TryParseMarkupMargin(const FString& Text, FMargin& OutValue)
    {
        TArray<FString> Parts;
        SplitMarkupNumbers(Text, Parts);
        if (Parts.Num() != 4)
        {
            return false;
        }

        float Left = 0.f;
        float Top = 0.f;
        float Right = 0.f;
        float Bottom = 0.f;
        if (!TryParseMarkupFloat(Parts[0], Left)
            || !TryParseMarkupFloat(Parts[1], Top)
            || !TryParseMarkupFloat(Parts[2], Right)
            || !TryParseMarkupFloat(Parts[3], Bottom))
        {
            return false;
        }
        OutValue = FMargin(Left, Top, Right, Bottom);
        return true;
    }

    static bool TryParseMarkupColor(const FString& Text, FLinearColor& OutValue)
    {
        return MonolithUI::TryParseColor(Text.TrimStartAndEnd(), OutValue);
    }

    static void AddBadValueFinding(
        FMarkupParseContext& Context,
        const FName& WidgetId,
        const FString& JsonPath,
        const FString& AttributeName,
        const FString& ExpectedShape)
    {
        AddMarkupFinding(
            Context,
            EUISpecErrorSeverity::Error,
            TEXT("MarkupValue"),
            JsonPath,
            WidgetId,
            FString::Printf(TEXT("Attribute '%s' has an invalid value for %s."), *AttributeName, *ExpectedShape),
            FString::Printf(TEXT("Use %s."), *ExpectedShape));
    }

    static bool TryGetKnownSlotAttributeName(const FString& AttributeName, FString& OutCanonical)
    {
        static const TCHAR* KnownSlotAttributes[] = {
            TEXT("slot.anchorPreset"),
            TEXT("slot.position"),
            TEXT("slot.size"),
            TEXT("slot.alignment"),
            TEXT("slot.padding"),
            TEXT("slot.autoSize"),
            TEXT("slot.zOrder"),
            TEXT("slot.hAlign"),
            TEXT("slot.vAlign"),
            TEXT("slot.sizeRule"),
            TEXT("slot.fillWeight")
        };

        for (const TCHAR* KnownAttribute : KnownSlotAttributes)
        {
            if (AttrEquals(AttributeName, KnownAttribute))
            {
                OutCanonical = KnownAttribute;
                return true;
            }
        }
        return false;
    }

    static bool TagEquals(const FString& Actual, const TCHAR* Expected)
    {
        return Actual.Equals(Expected, ESearchCase::IgnoreCase);
    }

    static bool IsAnyTag(const FString& Actual, std::initializer_list<const TCHAR*> ExpectedTags)
    {
        for (const TCHAR* Expected : ExpectedTags)
        {
            if (TagEquals(Actual, Expected))
            {
                return true;
            }
        }
        return false;
    }

    static bool TryGetAllowedSlotAttributesForParent(const FString& ParentTag, TArray<FString>& OutAllowed)
    {
        OutAllowed.Reset();
        if (ParentTag.IsEmpty())
        {
            return true;
        }

        if (TagEquals(ParentTag, TEXT("CanvasPanel")))
        {
            OutAllowed = {
                TEXT("slot.anchorPreset"),
                TEXT("slot.position"),
                TEXT("slot.size"),
                TEXT("slot.alignment"),
                TEXT("slot.autoSize"),
                TEXT("slot.zOrder")
            };
            return true;
        }

        if (IsAnyTag(ParentTag, { TEXT("VerticalBox"), TEXT("HorizontalBox") }))
        {
            OutAllowed = {
                TEXT("slot.padding"),
                TEXT("slot.hAlign"),
                TEXT("slot.vAlign"),
                TEXT("slot.sizeRule"),
                TEXT("slot.fillWeight")
            };
            return true;
        }

        if (IsAnyTag(ParentTag, {
            TEXT("Overlay"),
            TEXT("ScrollBox"),
            TEXT("SizeBox"),
            TEXT("WrapBox"),
            TEXT("WidgetSwitcher"),
            TEXT("Border")
        }))
        {
            OutAllowed = {
                TEXT("slot.padding"),
                TEXT("slot.hAlign"),
                TEXT("slot.vAlign")
            };
            return true;
        }

        if (TagEquals(ParentTag, TEXT("ScaleBox")))
        {
            OutAllowed = {
                TEXT("slot.hAlign"),
                TEXT("slot.vAlign")
            };
            return true;
        }

        if (TagEquals(ParentTag, TEXT("GridPanel")))
        {
            OutAllowed = {
                TEXT("slot.padding"),
                TEXT("slot.hAlign"),
                TEXT("slot.vAlign"),
                TEXT("slot.position"),
                TEXT("slot.size"),
                TEXT("slot.zOrder")
            };
            return true;
        }

        if (TagEquals(ParentTag, TEXT("UniformGridPanel")))
        {
            OutAllowed = {
                TEXT("slot.hAlign"),
                TEXT("slot.vAlign"),
                TEXT("slot.position")
            };
            return true;
        }

        return false;
    }

    static bool ValidateMarkupSlotAttributeForParent(
        FMarkupParseContext& Context,
        const FName& WidgetId,
        const FString& JsonPath,
        const FString& ParentTag,
        const FString& AttributeName)
    {
        FString CanonicalAttribute;
        if (!TryGetKnownSlotAttributeName(AttributeName, CanonicalAttribute))
        {
            return true;
        }

        TArray<FString> AllowedAttributes;
        if (!TryGetAllowedSlotAttributesForParent(ParentTag, AllowedAttributes))
        {
            return true;
        }

        if (AllowedAttributes.Contains(CanonicalAttribute))
        {
            return true;
        }

        const FString ParentLabel = ParentTag.IsEmpty()
            ? FString(TEXT("the root widget"))
            : FString::Printf(TEXT("parent widget type '%s'"), *ParentTag);
        AddMarkupFinding(
            Context,
            Context.bStrict ? EUISpecErrorSeverity::Error : EUISpecErrorSeverity::Warning,
            TEXT("MarkupSlotContext"),
            JsonPath,
            WidgetId,
            FString::Printf(
                TEXT("Attribute '%s' is not valid for %s because that parent does not create a compatible slot class."),
                *AttributeName,
                *ParentLabel),
            TEXT("Move the widget under a compatible parent panel, or replace the slot.* attribute with one of the listed parent-slot attributes."),
            AllowedAttributes);
        return false;
    }

    static bool TryApplyMarkupAttribute(
        FMarkupParseContext& Context,
        FUISpecNode& Node,
        const FString& JsonPath,
        const FString& AttributeName,
        const FString& AttributeValue)
    {
        if (AttrEquals(AttributeName, TEXT("Name")) || AttrEquals(AttributeName, TEXT("id")))
        {
            if (!AttributeValue.IsEmpty())
            {
                Node.Id = FName(*AttributeValue);
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("styleRef")))
        {
            Node.StyleRef = FName(*AttributeValue);
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("customClassPath")))
        {
            Node.CustomClassPath = AttributeValue;
            return true;
        }

        if (AttrEquals(AttributeName, TEXT("Text")) || AttrEquals(AttributeName, TEXT("text")))
        {
            Node.Content.Text = AttributeValue;
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("FontSize")) || AttrEquals(AttributeName, TEXT("fontSize")))
        {
            if (!TryParseMarkupFloat(AttributeValue, Node.Content.FontSize))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a numeric font size"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("FontColor")) || AttrEquals(AttributeName, TEXT("fontColor")))
        {
            if (!TryParseMarkupColor(AttributeValue, Node.Content.FontColor))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a Monolith color string such as #FFFFFFFF"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("WrapMode")) || AttrEquals(AttributeName, TEXT("wrapMode")))
        {
            Node.Content.WrapMode = FName(*AttributeValue);
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("BrushPath")) || AttrEquals(AttributeName, TEXT("brushPath")))
        {
            Node.Content.BrushPath = AttributeValue;
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("Placeholder")) || AttrEquals(AttributeName, TEXT("placeholder")))
        {
            Node.Content.Placeholder = AttributeValue;
            return true;
        }

        if (AttrEquals(AttributeName, TEXT("style.width")) || AttrEquals(AttributeName, TEXT("Width")))
        {
            if (!TryParseMarkupFloat(AttributeValue, Node.Style.Width))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a numeric width"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("style.height")) || AttrEquals(AttributeName, TEXT("Height")))
        {
            if (!TryParseMarkupFloat(AttributeValue, Node.Style.Height))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a numeric height"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("style.opacity")) || AttrEquals(AttributeName, TEXT("Opacity")))
        {
            if (!TryParseMarkupFloat(AttributeValue, Node.Style.Opacity))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a numeric opacity"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("style.visibility")) || AttrEquals(AttributeName, TEXT("Visibility")))
        {
            Node.Style.Visibility = FName(*AttributeValue);
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("style.padding")))
        {
            if (!TryParseMarkupMargin(AttributeValue, Node.Style.Padding))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("four margin numbers: left,top,right,bottom"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("style.background")) || AttrEquals(AttributeName, TEXT("Background")))
        {
            if (!TryParseMarkupColor(AttributeValue, Node.Style.Background))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a Monolith color string such as #FFFFFFFF"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("style.borderColor")))
        {
            if (!TryParseMarkupColor(AttributeValue, Node.Style.BorderColor))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a Monolith color string such as #FFFFFFFF"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("style.borderWidth")))
        {
            if (!TryParseMarkupFloat(AttributeValue, Node.Style.BorderWidth))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a numeric border width"));
            }
            return true;
        }

        if (AttrEquals(AttributeName, TEXT("slot.anchorPreset")))
        {
            Node.Slot.AnchorPreset = FName(*AttributeValue);
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.position")))
        {
            if (!TryParseMarkupVec2(AttributeValue, Node.Slot.Position))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("two numbers: x,y"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.size")))
        {
            if (!TryParseMarkupVec2(AttributeValue, Node.Slot.Size))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("two numbers: x,y"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.alignment")))
        {
            if (!TryParseMarkupVec2(AttributeValue, Node.Slot.Alignment))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("two numbers: x,y"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.padding")))
        {
            if (!TryParseMarkupMargin(AttributeValue, Node.Slot.Padding))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("four margin numbers: left,top,right,bottom"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.autoSize")))
        {
            if (!TryParseMarkupBool(AttributeValue, Node.Slot.bAutoSize))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("true or false"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.zOrder")))
        {
            if (!TryParseMarkupInt(AttributeValue, Node.Slot.ZOrder))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("an integer z-order"));
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.hAlign")))
        {
            Node.Slot.HAlign = FName(*AttributeValue);
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.vAlign")))
        {
            Node.Slot.VAlign = FName(*AttributeValue);
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.sizeRule")))
        {
            Node.Slot.SizeRule = FName(*AttributeValue);
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("slot.fillWeight")))
        {
            if (!TryParseMarkupFloat(AttributeValue, Node.Slot.FillWeight))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a numeric fill weight"));
            }
            return true;
        }

        if (AttrEquals(AttributeName, TEXT("common.inputLayer")))
        {
            Node.CommonUI.InputLayer = FName(*AttributeValue);
            Node.bHasCommonUI = true;
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("common.inputMode")))
        {
            Node.CommonUI.InputMode = FName(*AttributeValue);
            Node.bHasCommonUI = true;
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("common.styleRefs")))
        {
            TArray<FString> StyleRefs;
            AttributeValue.ParseIntoArray(StyleRefs, TEXT(","), true);
            for (FString& StyleRef : StyleRefs)
            {
                StyleRef.TrimStartAndEndInline();
                if (!StyleRef.IsEmpty())
                {
                    Node.CommonUI.StyleRefs.Add(FName(*StyleRef));
                }
            }
            Node.bHasCommonUI = true;
            return true;
        }

        if (AttrEquals(AttributeName, TEXT("effect.cornerRadii")))
        {
            TArray<FString> Parts;
            SplitMarkupNumbers(AttributeValue, Parts);
            float TL = 0.f;
            float TR = 0.f;
            float BR = 0.f;
            float BL = 0.f;
            if (Parts.Num() != 4
                || !TryParseMarkupFloat(Parts[0], TL)
                || !TryParseMarkupFloat(Parts[1], TR)
                || !TryParseMarkupFloat(Parts[2], BR)
                || !TryParseMarkupFloat(Parts[3], BL))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("four corner radius numbers: topLeft,topRight,bottomRight,bottomLeft"));
            }
            else
            {
                Node.Effect.CornerRadii = FVector4(TL, TR, BR, BL);
                Node.bHasEffect = true;
            }
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("effect.solidColor")))
        {
            if (!TryParseMarkupColor(AttributeValue, Node.Effect.SolidColor))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a Monolith color string such as #FFFFFFFF"));
            }
            Node.bHasEffect = true;
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("effect.smoothness")))
        {
            if (!TryParseMarkupFloat(AttributeValue, Node.Effect.Smoothness))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a numeric smoothness value"));
            }
            Node.bHasEffect = true;
            return true;
        }
        if (AttrEquals(AttributeName, TEXT("effect.backdropBlurStrength")))
        {
            if (!TryParseMarkupFloat(AttributeValue, Node.Effect.BackdropBlurStrength))
            {
                AddBadValueFinding(Context, Node.Id, JsonPath, AttributeName, TEXT("a numeric blur strength"));
            }
            Node.bHasEffect = true;
            return true;
        }

        return false;
    }

    static bool IsMarkupDocumentWrapperTag(const FString& Tag)
    {
        return Tag.Equals(TEXT("UISpec"), ESearchCase::IgnoreCase)
            || Tag.Equals(TEXT("UMG"), ESearchCase::IgnoreCase)
            || Tag.Equals(TEXT("WidgetBlueprint"), ESearchCase::IgnoreCase);
    }

    static bool IsKnownWidgetType(const FMarkupParseContext& Context, const FString& Tag)
    {
        return !Context.TypeRegistry || Context.TypeRegistry->FindByToken(FName(*Tag)) != nullptr;
    }

    static TSharedPtr<FUISpecNode> ParseMarkupNode(
        const FXmlNode& XmlNode,
        FMarkupParseContext& Context,
        const FString& ParentPath,
        const FString& ParentTag)
    {
        const FString Tag = XmlNode.GetTag().TrimStartAndEnd();
        TSharedPtr<FUISpecNode> Node = MakeShared<FUISpecNode>();
        Node->Type = FName(*Tag);

        FString IdString = GetXmlAttribute(XmlNode, TEXT("Name"));
        if (IdString.IsEmpty())
        {
            IdString = GetXmlAttribute(XmlNode, TEXT("id"));
        }
        if (IdString.IsEmpty())
        {
            IdString = FString::Printf(TEXT("%s_%d"), *Tag, ++Context.AutoId);
        }
        Node->Id = FName(*IdString);

        const FString JsonPath = MakeMarkupJsonPath(ParentPath, IdString);

        if (!IsKnownWidgetType(Context, Tag))
        {
            AddMarkupFinding(
                Context,
                Context.bStrict ? EUISpecErrorSeverity::Error : EUISpecErrorSeverity::Warning,
                TEXT("MarkupType"),
                JsonPath,
                Node->Id,
                FString::Printf(TEXT("Unknown UMG widget type token '%s'."), *Tag),
                TEXT("Use ui.describe_widget_type_schema or ui.list_widget_types, then replace the tag with a registered Monolith UI type token."));
        }

        for (const FXmlAttribute& Attr : XmlNode.GetAttributes())
        {
            const FString AttributeName = Attr.GetTag();
            if (!ValidateMarkupSlotAttributeForParent(Context, Node->Id, JsonPath, ParentTag, AttributeName))
            {
                continue;
            }
            if (!TryApplyMarkupAttribute(Context, *Node, JsonPath, AttributeName, Attr.GetValue()))
            {
                AddMarkupAttributeFinding(
                    Context,
                    Node->Id,
                    JsonPath,
                    AttributeName,
                    TEXT("Unsupported markup attribute."),
                    TEXT("Use Name/id, content fields, style.* fields, slot.* fields, common.* fields, or effect.* fields. Convert CSS and event handlers before calling this action."));
            }
        }

        const FString TextContent = XmlNode.GetContent().TrimStartAndEnd();
        if (!TextContent.IsEmpty() && Node->Content.Text.IsEmpty())
        {
            Node->Content.Text = TextContent;
        }

        const TArray<FXmlNode*>& Children = XmlNode.GetChildrenNodes();
        Node->Children.Reserve(Children.Num());
        for (const FXmlNode* Child : Children)
        {
            if (Child)
            {
                if (TSharedPtr<FUISpecNode> ChildNode = ParseMarkupNode(*Child, Context, JsonPath, Tag))
                {
                    Node->Children.Add(ChildNode);
                }
            }
        }

        return Node;
    }

    static void AppendValidationResult(FUISpecValidationResult& InOut, const FUISpecValidationResult& Other)
    {
        InOut.Errors.Append(Other.Errors);
        InOut.Warnings.Append(Other.Warnings);
        InOut.bIsValid = (InOut.Errors.Num() == 0);
    }

    static FString GetAssetNameFromPath(const FString& AssetPath)
    {
        if (AssetPath.IsEmpty())
        {
            return FString();
        }
        if (FPackageName::IsValidLongPackageName(AssetPath))
        {
            return FPackageName::GetLongPackageAssetName(AssetPath);
        }
        return FPaths::GetBaseFilename(AssetPath);
    }

    static bool ParseMarkupDocument(
        const FString& Markup,
        const FString& Dialect,
        bool bStrict,
        const FString& RootSavePath,
        const FString& ExplicitName,
        const FString& ExplicitParentClass,
        const FString& SourceName,
        bool bTreatWarningsAsErrors,
        FUISpecDocument& OutDocument,
        FUISpecValidationResult& OutValidation)
    {
        FMarkupParseContext Context;
        Context.bStrict = bStrict;
        if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
        {
            Context.TypeRegistry = &Sub->GetTypeRegistry();
        }
        else
        {
            AddMarkupFinding(
                Context,
                EUISpecErrorSeverity::Warning,
                TEXT("Registry"),
                TEXT("/"),
                NAME_None,
                TEXT("UMonolithUIRegistrySubsystem is unavailable; widget tag validation was skipped."),
                TEXT("Run in editor context when schema validation should prove registered UMG tokens."));
        }

        FXmlFile XmlFile(Markup, EConstructMethod::ConstructFromBuffer);
        if (!XmlFile.IsValid() || !XmlFile.GetRootNode())
        {
            AddMarkupFinding(
                Context,
                EUISpecErrorSeverity::Error,
                TEXT("Parse"),
                TEXT("/"),
                NAME_None,
                FString::Printf(TEXT("Markup XML parse failure: %s"), *XmlFile.GetLastError()),
                TEXT("Use strict XML-like markup: one root element, quoted attributes, closed tags, and escaped special characters."));
            OutValidation = MoveTemp(Context.Validation);
            OutValidation.bIsValid = false;
            return false;
        }

        const FXmlNode* RootXml = XmlFile.GetRootNode();
        const bool bDocumentWrapper = IsMarkupDocumentWrapperTag(RootXml->GetTag());
        if (bDocumentWrapper)
        {
            FString WrapperName = GetXmlAttribute(*RootXml, TEXT("name"));
            if (WrapperName.IsEmpty())
            {
                WrapperName = GetXmlAttribute(*RootXml, TEXT("Name"));
            }
            OutDocument.Name = ExplicitName.IsEmpty() ? WrapperName : ExplicitName;
            OutDocument.ParentClass = ExplicitParentClass.IsEmpty()
                ? GetXmlAttribute(*RootXml, TEXT("parentClass"))
                : ExplicitParentClass;
            const TArray<FXmlNode*>& Children = RootXml->GetChildrenNodes();
            RootXml = Children.Num() > 0 ? Children[0] : nullptr;
        }
        else
        {
            OutDocument.Name = ExplicitName;
            OutDocument.ParentClass = ExplicitParentClass;
        }

        if (OutDocument.Name.IsEmpty())
        {
            OutDocument.Name = GetAssetNameFromPath(RootSavePath);
        }
        if (OutDocument.Name.IsEmpty())
        {
            OutDocument.Name = TEXT("ConvertedWidget");
        }
        if (OutDocument.ParentClass.IsEmpty())
        {
            OutDocument.ParentClass = TEXT("UserWidget");
        }

        OutDocument.Version = 1;
        OutDocument.Metadata.AuthoringTool = TEXT("MonolithUI.convert_markup_to_ui_spec");
        OutDocument.Metadata.SourceFile = SourceName;
        OutDocument.Metadata.Description = FString::Printf(TEXT("Converted from %s markup."), *Dialect);
        OutDocument.bTreatWarningsAsErrors = bTreatWarningsAsErrors;

        if (!RootXml)
        {
            AddMarkupFinding(
                Context,
                EUISpecErrorSeverity::Error,
                TEXT("Structure"),
                TEXT("/rootWidget"),
                NAME_None,
                TEXT("Markup document wrapper has no child widget root."),
                TEXT("Add exactly one UMG widget element inside the UISpec/UMG/WidgetBlueprint wrapper."));
        }
        else
        {
            OutDocument.Root = ParseMarkupNode(*RootXml, Context, FString(), FString());
        }

        FUISpecValidationResult Combined = MoveTemp(Context.Validation);
        if (OutDocument.Root.IsValid())
        {
            AppendValidationResult(Combined, FUISpecValidator::Validate(OutDocument));
        }
        Combined.bIsValid = Combined.Errors.Num() == 0
            && (!bTreatWarningsAsErrors || Combined.Warnings.Num() == 0);
        OutValidation = MoveTemp(Combined);
        return OutValidation.bIsValid;
    }

    /**
     * Pack a populated FUISpecBuilderResult into the JSON response shape.
     * Mirror of the documented action wire shape:
     *   { bSuccess, asset_path, request_id|null, validation, node_counts,
     *     diff?, errors?, warnings? }
     */
    static TSharedPtr<FJsonObject> PackResponse(const FUISpecBuilderResult& R, bool bDryRun)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField  (TEXT("bSuccess"),    R.bSuccess);
        Out->SetStringField(TEXT("asset_path"),  R.AssetPath);
        if (!R.RequestId.IsEmpty())
        {
            Out->SetStringField(TEXT("request_id"), R.RequestId);
        }

        // Validation block — flat shape so the LLM can grep its way through.
        TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
        Validation->SetBoolField(TEXT("is_valid"), R.Validation.bIsValid);
        Validation->SetStringField(TEXT("llm_report"), R.Validation.ToLLMReport());
        Out->SetObjectField(TEXT("validation"), Validation);

        TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
        Counts->SetNumberField(TEXT("created"),  R.NodesCreated);
        Counts->SetNumberField(TEXT("modified"), R.NodesModified);
        Counts->SetNumberField(TEXT("removed"),  R.NodesRemoved);
        Out->SetObjectField(TEXT("node_counts"), Counts);

        if (R.Errors.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Errs;
            Errs.Reserve(R.Errors.Num());
            for (const FUISpecError& E : R.Errors)
            {
                TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
                EObj->SetStringField(TEXT("category"), E.Category.ToString());
                EObj->SetStringField(TEXT("widget_id"), E.WidgetId.ToString());
                EObj->SetStringField(TEXT("message"),  E.Message);
                EObj->SetStringField(TEXT("json_path"), E.JsonPath);
                EObj->SetStringField(TEXT("suggested_fix"), E.SuggestedFix);
                Errs.Add(MakeShared<FJsonValueObject>(EObj));
            }
            Out->SetArrayField(TEXT("errors"), Errs);
        }
        if (R.Warnings.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Warns;
            Warns.Reserve(R.Warnings.Num());
            for (const FUISpecError& W : R.Warnings)
            {
                TSharedPtr<FJsonObject> WObj = MakeShared<FJsonObject>();
                WObj->SetStringField(TEXT("category"), W.Category.ToString());
                WObj->SetStringField(TEXT("widget_id"), W.WidgetId.ToString());
                WObj->SetStringField(TEXT("message"),  W.Message);
                WObj->SetStringField(TEXT("suggested_fix"), W.SuggestedFix);
                Warns.Add(MakeShared<FJsonValueObject>(WObj));
            }
            Out->SetArrayField(TEXT("warnings"), Warns);
        }
        if (bDryRun || R.DiffLines.Num() > 0)
        {
            TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> Lines;
            Lines.Reserve(R.DiffLines.Num());
            for (const FString& L : R.DiffLines)
            {
                Lines.Add(MakeShared<FJsonValueString>(L));
            }
            Diff->SetArrayField(TEXT("lines"), Lines);
            Diff->SetBoolField(TEXT("dry_run"), bDryRun);
            Out->SetObjectField(TEXT("diff"), Diff);
        }
        return Out;
    }

    // ------------------------------------------------------------------
    // ui::build_ui_from_spec handler

    static FMonolithActionResult HandleBuildUIFromSpec(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString AssetPath;
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
        }

        const TSharedPtr<FJsonObject>* SpecObjPtr = nullptr;
        if (!Params->TryGetObjectField(TEXT("spec"), SpecObjPtr) || !SpecObjPtr || !(*SpecObjPtr).IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing or invalid required param: spec (must be a JSON object)"), -32602);
        }

        // Phase K: pull request_id BEFORE the parse step so the parse-fail
        // response can echo it back. The parse-fail path used to drop request_id
        // on the floor — caller correlation broke when the JSON had a syntax
        // error. Now the echo is uniform across parse-fail / validate-fail /
        // builder-fail / success.
        FString RequestId;
        Params->TryGetStringField(TEXT("request_id"), RequestId);

        FUISpecDocument Document;
        FUISpecValidationResult ParseValidation;
        if (!ParseDocument(*SpecObjPtr, Document, ParseValidation))
        {
            FUISpecBuilderResult R;
            R.AssetPath = AssetPath;
            R.RequestId = RequestId;  // Phase K — echo even on parse failure.
            R.Validation = ParseValidation;
            // Return as success-on-the-wire so the LLM gets the full shape;
            // bSuccess=false in the payload signals semantic failure.
            return FMonolithActionResult::Success(PackResponse(R, /*bDryRun=*/false));
        }

        FUISpecBuilderInputs Inputs;
        Inputs.Document  = &Document;
        Inputs.AssetPath = AssetPath;
        Inputs.bOverwrite             = true;  // default per spec
        Inputs.bDryRun                = false;
        Inputs.bTreatWarningsAsErrors = false;
        Inputs.bRawMode               = false;
        Params->TryGetBoolField(TEXT("overwrite"), Inputs.bOverwrite);
        Params->TryGetBoolField(TEXT("dry_run"), Inputs.bDryRun);
        Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), Inputs.bTreatWarningsAsErrors);
        Params->TryGetBoolField(TEXT("raw_mode"), Inputs.bRawMode);
        // Phase K — request_id was already pulled above. Reuse the same value
        // so parse-fail / validate-fail / success all echo the identical token.
        Inputs.RequestId = RequestId;

        // Per-document override — the spec itself may set the strict flag.
        if (Document.bTreatWarningsAsErrors)
        {
            Inputs.bTreatWarningsAsErrors = true;
        }

        const FUISpecBuilderResult R = FUISpecBuilder::Build(Inputs);
        return FMonolithActionResult::Success(PackResponse(R, Inputs.bDryRun));
    }

    // ------------------------------------------------------------------
    // ui::dump_ui_spec_schema handler

    /**
     * Build a JSON-Schema-style projection of `FUISpecDocument` plus the
     * live allowlist projection. Intentionally informal — the LLM reads it
     * as a contract surface, not a strict validator.
     */
    static FMonolithActionResult HandleDumpUISpecSchema(const TSharedPtr<FJsonObject>& /*Params*/)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("schema_version"), TEXT("1"));
        Out->SetStringField(TEXT("document_type"), TEXT("FUISpecDocument"));

        // Document-level fields (top-level keys recognised by the parser).
        {
            TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
            auto AddField = [&Fields](const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
            {
                TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
                F->SetStringField(TEXT("type"), Type);
                F->SetStringField(TEXT("description"), Desc);
                Fields->SetObjectField(Name, F);
            };
            AddField(TEXT("version"),     TEXT("integer"), TEXT("Schema version. Bumped on incompatible parser changes."));
            AddField(TEXT("name"),        TEXT("string"),  TEXT("Widget Blueprint name (becomes the asset filename)."));
            AddField(TEXT("parentClass"), TEXT("string"),  TEXT("Parent class token (UserWidget / CommonActivatableWidget / CommonUserWidget) or full /Script path."));
            AddField(TEXT("metadata"),    TEXT("object"),  TEXT("FUISpecMetadata bag (authoringTool / sourceFile / author / description / tags)."));
            AddField(TEXT("styles"),      TEXT("object"),  TEXT("Map<name, FUISpecStyle> — named styles referenced by node.styleRef."));
            AddField(TEXT("animations"),  TEXT("array"),   TEXT("FUISpecAnimation[] — named widget animations."));
            AddField(TEXT("treatWarningsAsErrors"), TEXT("boolean"), TEXT("When true, validator warnings escalate to errors."));
            AddField(TEXT("rootWidget"),  TEXT("object"),  TEXT("Required. FUISpecNode root of the widget tree."));
            Out->SetObjectField(TEXT("document_fields"), Fields);
        }

        // Node-level fields.
        {
            TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
            auto AddField = [&Fields](const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
            {
                TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
                F->SetStringField(TEXT("type"), Type);
                F->SetStringField(TEXT("description"), Desc);
                Fields->SetObjectField(Name, F);
            };
            AddField(TEXT("type"),            TEXT("string"),  TEXT("Widget type token resolved through FUITypeRegistry (e.g. 'VerticalBox', 'TextBlock', 'EffectSurface')."));
            AddField(TEXT("id"),              TEXT("string"),  TEXT("Variable name of the widget on the WBP. Must be unique within the spec."));
            AddField(TEXT("slot"),            TEXT("object"),  TEXT("FUISpecSlot (anchorPreset / position / size / alignment / padding / autoSize / hAlign / vAlign / zOrder / sizeRule / fillWeight)."));
            AddField(TEXT("style"),           TEXT("object"),  TEXT("FUISpecStyle (width / height / minDesiredWidth / minDesiredHeight / maxDesiredWidth / maxDesiredHeight / override flags / padding / background / borderColor / borderWidth / opacity / visibility)."));
            AddField(TEXT("content"),         TEXT("object"),  TEXT("FUISpecContent (text / fontSize / fontColor / wrapMode / brushPath / placeholder)."));
            AddField(TEXT("effect"),          TEXT("object"),  TEXT("FUISpecEffect — UEffectSurface only. Sub-bag triggers bHasEffect."));
            AddField(TEXT("commonUI"),        TEXT("object"),  TEXT("FUISpecCommonUI (inputLayer / inputMode / styleRefs[]). Sub-bag triggers bHasCommonUI."));
            AddField(TEXT("styleRef"),        TEXT("string"),  TEXT("Named entry in document.styles."));
            AddField(TEXT("animationRefs"),   TEXT("array"),   TEXT("Names of entries in document.animations to bind to this widget."));
            AddField(TEXT("customClassPath"), TEXT("string"),  TEXT("Fallback when 'type' is not in the registry — full Blueprint class path."));
            AddField(TEXT("children"),        TEXT("array"),   TEXT("FUISpecNode[] — nested widgets."));
            Out->SetObjectField(TEXT("node_fields"), Fields);
        }

        // Live allowlist projection per type — same shape ui::dump_property_allowlist
        // gives us, embedded here so a single call returns the full contract.
        {
            TSharedPtr<FJsonObject> ByType = MakeShared<FJsonObject>();
            if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
            {
                const FUITypeRegistry& Reg = Sub->GetTypeRegistry();
                const FUIPropertyAllowlist& Allow = Sub->GetAllowlist();
                for (const FUITypeRegistryEntry& Entry : Reg.GetAll())
                {
                    const TArray<FString>& Paths = Allow.GetAllowedPaths(Entry.Token);
                    if (Paths.Num() == 0) continue;

                    TArray<TSharedPtr<FJsonValue>> Arr;
                    Arr.Reserve(Paths.Num());
                    for (const FString& P : Paths)
                    {
                        Arr.Add(MakeShared<FJsonValueString>(P));
                    }
                    ByType->SetArrayField(Entry.Token.ToString(), Arr);
                }
            }
            Out->SetObjectField(TEXT("allowlist_by_type"), ByType);
        }

        return FMonolithActionResult::Success(Out);
    }

    // ------------------------------------------------------------------
    // Phase J: ui::dump_ui_spec handler
    //
    // Reads an existing UWidgetBlueprint and emits a FUISpecDocument JSON
    // suitable for round-tripping through ui::build_ui_from_spec. Pure read;
    // no asset mutation. Mirrors the build response shape so action surfaces
    // can compose the two passes uniformly.
    // ------------------------------------------------------------------

    /** Emit FUISpecSlot as JSON. Inverse of ParseSlot above. */
    static TSharedPtr<FJsonObject> SlotToJson(const FUISpecSlot& S)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!S.AnchorPreset.IsNone()) Out->SetStringField(TEXT("anchorPreset"), S.AnchorPreset.ToString());
        if (!S.HAlign.IsNone())       Out->SetStringField(TEXT("hAlign"), S.HAlign.ToString());
        if (!S.VAlign.IsNone())       Out->SetStringField(TEXT("vAlign"), S.VAlign.ToString());
        if (!S.SizeRule.IsNone())
        {
            Out->SetStringField(TEXT("sizeRule"), S.SizeRule.ToString());
            Out->SetNumberField(TEXT("fillWeight"), S.FillWeight);
        }
        if (S.bAutoSize)              Out->SetBoolField(TEXT("autoSize"), true);
        if (S.ZOrder != 0)            Out->SetNumberField(TEXT("zOrder"), S.ZOrder);

        // Position, size, alignment as object{x,y}.
        TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
        Pos->SetNumberField(TEXT("x"), S.Position.X);
        Pos->SetNumberField(TEXT("y"), S.Position.Y);
        Out->SetObjectField(TEXT("position"), Pos);

        TSharedPtr<FJsonObject> Sz = MakeShared<FJsonObject>();
        Sz->SetNumberField(TEXT("x"), S.Size.X);
        Sz->SetNumberField(TEXT("y"), S.Size.Y);
        Out->SetObjectField(TEXT("size"), Sz);

        TSharedPtr<FJsonObject> Al = MakeShared<FJsonObject>();
        Al->SetNumberField(TEXT("x"), S.Alignment.X);
        Al->SetNumberField(TEXT("y"), S.Alignment.Y);
        Out->SetObjectField(TEXT("alignment"), Al);

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetNumberField(TEXT("left"),   S.Padding.Left);
        P->SetNumberField(TEXT("top"),    S.Padding.Top);
        P->SetNumberField(TEXT("right"),  S.Padding.Right);
        P->SetNumberField(TEXT("bottom"), S.Padding.Bottom);
        Out->SetObjectField(TEXT("padding"), P);
        return Out;
    }

    static FString ColorToHexString(const FLinearColor& C)
    {
        const FColor B = C.ToFColor(/*bSRGB=*/false);
        return FString::Printf(TEXT("#%02X%02X%02X%02X"), B.R, B.G, B.B, B.A);
    }

    /** Emit FUISpecStyle as JSON. Inverse of ParseStyle. */
    static TSharedPtr<FJsonObject> StyleToJson(const FUISpecStyle& S)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (S.Width  != 0.f) Out->SetNumberField(TEXT("width"),  S.Width);
        if (S.Height != 0.f) Out->SetNumberField(TEXT("height"), S.Height);
        if (S.bOverrideMinDesiredWidth)
        {
            Out->SetBoolField(TEXT("overrideMinDesiredWidth"), true);
            Out->SetNumberField(TEXT("minDesiredWidth"), S.MinDesiredWidth);
        }
        if (S.bOverrideMinDesiredHeight)
        {
            Out->SetBoolField(TEXT("overrideMinDesiredHeight"), true);
            Out->SetNumberField(TEXT("minDesiredHeight"), S.MinDesiredHeight);
        }
        if (S.bOverrideMaxDesiredWidth)
        {
            Out->SetBoolField(TEXT("overrideMaxDesiredWidth"), true);
            Out->SetNumberField(TEXT("maxDesiredWidth"), S.MaxDesiredWidth);
        }
        if (S.bOverrideMaxDesiredHeight)
        {
            Out->SetBoolField(TEXT("overrideMaxDesiredHeight"), true);
            Out->SetNumberField(TEXT("maxDesiredHeight"), S.MaxDesiredHeight);
        }
        if (S.BorderWidth != 0.f) Out->SetNumberField(TEXT("borderWidth"), S.BorderWidth);
        Out->SetNumberField(TEXT("opacity"), S.Opacity);
        if (S.bUseCustomSize) Out->SetBoolField(TEXT("useCustomSize"), true);
        if (!S.Visibility.IsNone()) Out->SetStringField(TEXT("visibility"), S.Visibility.ToString());
        Out->SetStringField(TEXT("background"),  ColorToHexString(S.Background));
        Out->SetStringField(TEXT("borderColor"), ColorToHexString(S.BorderColor));

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetNumberField(TEXT("left"),   S.Padding.Left);
        P->SetNumberField(TEXT("top"),    S.Padding.Top);
        P->SetNumberField(TEXT("right"),  S.Padding.Right);
        P->SetNumberField(TEXT("bottom"), S.Padding.Bottom);
        Out->SetObjectField(TEXT("padding"), P);
        return Out;
    }

    /** Emit FUISpecContent as JSON. Inverse of ParseContent. */
    static TSharedPtr<FJsonObject> ContentToJson(const FUISpecContent& C)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!C.Text.IsEmpty())        Out->SetStringField(TEXT("text"), C.Text);
        if (C.FontSize != 0.f)        Out->SetNumberField(TEXT("fontSize"), C.FontSize);
        if (!C.WrapMode.IsNone())     Out->SetStringField(TEXT("wrapMode"), C.WrapMode.ToString());
        if (!C.BrushPath.IsEmpty())   Out->SetStringField(TEXT("brushPath"), C.BrushPath);
        if (!C.Placeholder.IsEmpty()) Out->SetStringField(TEXT("placeholder"), C.Placeholder);
        Out->SetStringField(TEXT("fontColor"), ColorToHexString(C.FontColor));
        return Out;
    }

    /** Emit a single FUISpecEffectShadow entry. */
    static TSharedPtr<FJsonObject> ShadowToJson(const FUISpecEffectShadow& S)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> Off = MakeShared<FJsonObject>();
        Off->SetNumberField(TEXT("x"), S.Offset.X);
        Off->SetNumberField(TEXT("y"), S.Offset.Y);
        Out->SetObjectField(TEXT("offset"), Off);
        Out->SetNumberField(TEXT("blur"),   S.Blur);
        Out->SetNumberField(TEXT("spread"), S.Spread);
        Out->SetStringField(TEXT("color"),  ColorToHexString(S.Color));
        Out->SetBoolField  (TEXT("inset"),  S.bInset);
        return Out;
    }

    /** Emit FUISpecEffect as JSON. Inverse of ParseEffect; closes the Phase H deferral. */
    static TSharedPtr<FJsonObject> EffectToJson(const FUISpecEffect& E)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Radii;
        Radii.Add(MakeShared<FJsonValueNumber>(E.CornerRadii.X));
        Radii.Add(MakeShared<FJsonValueNumber>(E.CornerRadii.Y));
        Radii.Add(MakeShared<FJsonValueNumber>(E.CornerRadii.Z));
        Radii.Add(MakeShared<FJsonValueNumber>(E.CornerRadii.W));
        Out->SetArrayField(TEXT("cornerRadii"), Radii);
        Out->SetNumberField(TEXT("smoothness"), E.Smoothness);
        Out->SetStringField(TEXT("solidColor"), ColorToHexString(E.SolidColor));
        Out->SetNumberField(TEXT("backdropBlurStrength"), E.BackdropBlurStrength);

        TArray<TSharedPtr<FJsonValue>> Drops;
        Drops.Reserve(E.DropShadows.Num());
        for (const FUISpecEffectShadow& S : E.DropShadows)
        {
            Drops.Add(MakeShared<FJsonValueObject>(ShadowToJson(S)));
        }
        if (Drops.Num() > 0) Out->SetArrayField(TEXT("dropShadows"), Drops);

        TArray<TSharedPtr<FJsonValue>> Inners;
        Inners.Reserve(E.InnerShadows.Num());
        for (const FUISpecEffectShadow& S : E.InnerShadows)
        {
            Inners.Add(MakeShared<FJsonValueObject>(ShadowToJson(S)));
        }
        if (Inners.Num() > 0) Out->SetArrayField(TEXT("innerShadows"), Inners);
        return Out;
    }

    /** Emit FUISpecCommonUI as JSON. Inverse of ParseCommonUI. */
    static TSharedPtr<FJsonObject> CommonUIToJson(const FUISpecCommonUI& C)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!C.InputLayer.IsNone()) Out->SetStringField(TEXT("inputLayer"), C.InputLayer.ToString());
        if (!C.InputMode.IsNone())  Out->SetStringField(TEXT("inputMode"),  C.InputMode.ToString());
        if (C.StyleRefs.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            Arr.Reserve(C.StyleRefs.Num());
            for (const FName& N : C.StyleRefs)
            {
                Arr.Add(MakeShared<FJsonValueString>(N.ToString()));
            }
            Out->SetArrayField(TEXT("styleRefs"), Arr);
        }
        return Out;
    }

    /** Recursive node serialiser. Inverse of ParseNode. */
    static TSharedPtr<FJsonObject> NodeToJson(const FUISpecNode& N)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!N.Type.IsNone()) Out->SetStringField(TEXT("type"), N.Type.ToString());
        if (!N.Id.IsNone())   Out->SetStringField(TEXT("id"),   N.Id.ToString());
        if (!N.StyleRef.IsNone()) Out->SetStringField(TEXT("styleRef"), N.StyleRef.ToString());
        if (!N.CustomClassPath.IsEmpty()) Out->SetStringField(TEXT("customClassPath"), N.CustomClassPath);

        Out->SetObjectField(TEXT("slot"),    SlotToJson(N.Slot));
        Out->SetObjectField(TEXT("style"),   StyleToJson(N.Style));
        Out->SetObjectField(TEXT("content"), ContentToJson(N.Content));
        if (N.bHasEffect)   Out->SetObjectField(TEXT("effect"),   EffectToJson(N.Effect));
        if (N.bHasCommonUI) Out->SetObjectField(TEXT("commonUI"), CommonUIToJson(N.CommonUI));

        if (N.AnimationRefs.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            Arr.Reserve(N.AnimationRefs.Num());
            for (const FName& A : N.AnimationRefs)
            {
                Arr.Add(MakeShared<FJsonValueString>(A.ToString()));
            }
            Out->SetArrayField(TEXT("animationRefs"), Arr);
        }

        if (N.Children.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            Arr.Reserve(N.Children.Num());
            for (const TSharedPtr<FUISpecNode>& C : N.Children)
            {
                if (C.IsValid())
                {
                    Arr.Add(MakeShared<FJsonValueObject>(NodeToJson(*C)));
                }
            }
            Out->SetArrayField(TEXT("children"), Arr);
        }
        return Out;
    }

    static TSharedPtr<FJsonObject> AnimationToJson(const FUISpecAnimation& A)
    {
        TSharedPtr<FJsonObject> AObj = MakeShared<FJsonObject>();
        AObj->SetStringField(TEXT("name"), A.Name.ToString());
        AObj->SetStringField(TEXT("targetWidgetId"), A.TargetWidgetId.ToString());
        AObj->SetStringField(TEXT("targetProperty"), A.TargetProperty.ToString());
        AObj->SetNumberField(TEXT("duration"), A.Duration);
        AObj->SetNumberField(TEXT("delay"), A.Delay);
        if (!A.Easing.IsNone()) AObj->SetStringField(TEXT("easing"), A.Easing.ToString());
        if (!A.LoopMode.IsNone()) AObj->SetStringField(TEXT("loopMode"), A.LoopMode.ToString());
        AObj->SetBoolField(TEXT("autoPlay"), A.bAutoPlay);
        if (A.Keyframes.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Kfs;
            Kfs.Reserve(A.Keyframes.Num());
            for (const FUISpecKeyframe& K : A.Keyframes)
            {
                TSharedPtr<FJsonObject> KO = MakeShared<FJsonObject>();
                KO->SetNumberField(TEXT("time"), K.Time);
                KO->SetNumberField(TEXT("scalarValue"), K.ScalarValue);
                if (!K.Easing.IsNone()) KO->SetStringField(TEXT("easing"), K.Easing.ToString());
                Kfs.Add(MakeShared<FJsonValueObject>(KO));
            }
            AObj->SetArrayField(TEXT("keyframes"), Kfs);
        }
        return AObj;
    }

    static TSharedPtr<FJsonObject> DocumentToJson(const FUISpecDocument& Document)
    {
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetNumberField(TEXT("version"), Document.Version);
        Spec->SetStringField(TEXT("name"), Document.Name);
        Spec->SetStringField(TEXT("parentClass"), Document.ParentClass);
        Spec->SetBoolField(TEXT("treatWarningsAsErrors"), Document.bTreatWarningsAsErrors);

        TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
        Meta->SetStringField(TEXT("authoringTool"), Document.Metadata.AuthoringTool);
        Meta->SetStringField(TEXT("sourceFile"), Document.Metadata.SourceFile);
        Meta->SetStringField(TEXT("author"), Document.Metadata.Author);
        Meta->SetStringField(TEXT("description"), Document.Metadata.Description);
        Spec->SetObjectField(TEXT("metadata"), Meta);

        if (Document.Styles.Num() > 0)
        {
            TSharedPtr<FJsonObject> Styles = MakeShared<FJsonObject>();
            for (const TPair<FName, FUISpecStyle>& Pair : Document.Styles)
            {
                Styles->SetObjectField(Pair.Key.ToString(), StyleToJson(Pair.Value));
            }
            Spec->SetObjectField(TEXT("styles"), Styles);
        }

        if (Document.Animations.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            Arr.Reserve(Document.Animations.Num());
            for (const FUISpecAnimation& A : Document.Animations)
            {
                Arr.Add(MakeShared<FJsonValueObject>(AnimationToJson(A)));
            }
            Spec->SetArrayField(TEXT("animations"), Arr);
        }

        if (Document.Root.IsValid())
        {
            Spec->SetObjectField(TEXT("rootWidget"), NodeToJson(*Document.Root));
        }
        return Spec;
    }

    static TSharedPtr<FJsonObject> ValidationFindingToJson(const FUISpecError& Finding)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("severity"),
            Finding.Severity == EUISpecErrorSeverity::Error
                ? TEXT("error")
                : (Finding.Severity == EUISpecErrorSeverity::Warning ? TEXT("warning") : TEXT("info")));
        Obj->SetStringField(TEXT("category"), Finding.Category.ToString());
        Obj->SetStringField(TEXT("widget_id"), Finding.WidgetId.ToString());
        Obj->SetStringField(TEXT("json_path"), Finding.JsonPath);
        Obj->SetStringField(TEXT("message"), Finding.Message);
        Obj->SetStringField(TEXT("suggested_fix"), Finding.SuggestedFix);
        if (Finding.ValidOptions.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Options;
            Options.Reserve(Finding.ValidOptions.Num());
            for (const FString& Option : Finding.ValidOptions)
            {
                Options.Add(MakeShared<FJsonValueString>(Option));
            }
            Obj->SetArrayField(TEXT("valid_options"), Options);
        }
        return Obj;
    }

    static void SetValidationFindingArray(
        TSharedPtr<FJsonObject> Obj,
        const TCHAR* FieldName,
        const TArray<FUISpecError>& Findings)
    {
        if (Findings.Num() == 0)
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Rows;
        Rows.Reserve(Findings.Num());
        for (const FUISpecError& Finding : Findings)
        {
            Rows.Add(MakeShared<FJsonValueObject>(ValidationFindingToJson(Finding)));
        }
        Obj->SetArrayField(FieldName, Rows);
    }

    static int32 CountNodes(const TSharedPtr<FUISpecNode>& Node)
    {
        if (!Node.IsValid())
        {
            return 0;
        }

        int32 Count = 1;
        for (const TSharedPtr<FUISpecNode>& Child : Node->Children)
        {
            Count += CountNodes(Child);
        }
        return Count;
    }

    static TSharedPtr<FJsonObject> PackMarkupConversionResponse(
        const FUISpecDocument& Document,
        const FUISpecValidationResult& Validation,
        const FString& Dialect,
        bool bStrict,
        const FString& RootSavePath,
        const FString& RequestId)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("bSuccess"), Validation.bIsValid);
        Out->SetBoolField(TEXT("ok"), Validation.bIsValid);
        Out->SetStringField(TEXT("schema_version"), TEXT("ui_markup_to_spec.v1"));
        Out->SetStringField(TEXT("dialect"), Dialect);
        Out->SetBoolField(TEXT("strict"), bStrict);
        Out->SetBoolField(TEXT("would_create_asset"), false);
        if (!RootSavePath.IsEmpty())
        {
            Out->SetStringField(TEXT("root_save_path"), RootSavePath);
        }
        if (!RequestId.IsEmpty())
        {
            Out->SetStringField(TEXT("request_id"), RequestId);
        }
        Out->SetNumberField(TEXT("node_count"), CountNodes(Document.Root));

        TSharedPtr<FJsonObject> ValidationObj = MakeShared<FJsonObject>();
        ValidationObj->SetBoolField(TEXT("is_valid"), Validation.bIsValid);
        ValidationObj->SetStringField(TEXT("llm_report"), Validation.ToLLMReport());
        ValidationObj->SetNumberField(TEXT("error_count"), Validation.Errors.Num());
        ValidationObj->SetNumberField(TEXT("warning_count"), Validation.Warnings.Num());
        Out->SetObjectField(TEXT("validation"), ValidationObj);

        Out->SetObjectField(TEXT("spec"), DocumentToJson(Document));
        SetValidationFindingArray(Out, TEXT("errors"), Validation.Errors);
        SetValidationFindingArray(Out, TEXT("warnings"), Validation.Warnings);

        TArray<TSharedPtr<FJsonValue>> NextActions;
        auto AddNextAction = [&NextActions](const FString& ToolName, bool bAvailable)
        {
            TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
            Action->SetStringField(TEXT("tool"), ToolName);
            Action->SetBoolField(TEXT("available"), bAvailable);
            NextActions.Add(MakeShared<FJsonValueObject>(Action));
        };
        AddNextAction(TEXT("ui.build_ui_from_spec"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("build_ui_from_spec")));
        AddNextAction(TEXT("ui.dump_ui_spec_schema"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("dump_ui_spec_schema")));
        AddNextAction(TEXT("ui.describe_widget_type_schema"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("describe_widget_type_schema")));
        Out->SetArrayField(TEXT("next_actions"), NextActions);
        return Out;
    }

    static FMonolithActionResult HandleConvertMarkupToUISpec(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString Markup;
        if (!Params->TryGetStringField(TEXT("markup"), Markup) || Markup.TrimStartAndEnd().IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("Missing or empty required param: markup"), -32602);
        }

        FString Dialect = TEXT("umg_xml_v1");
        Params->TryGetStringField(TEXT("dialect"), Dialect);
        if (!Dialect.Equals(TEXT("umg_xml_v1"), ESearchCase::IgnoreCase)
            && !Dialect.Equals(TEXT("umg_html_v1"), ESearchCase::IgnoreCase)
            && !Dialect.Equals(TEXT("html"), ESearchCase::IgnoreCase))
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Unsupported dialect '%s'. Supported: umg_xml_v1, umg_html_v1, html."), *Dialect),
                -32602);
        }

        bool bStrict = true;
        bool bTreatWarningsAsErrors = false;
        Params->TryGetBoolField(TEXT("strict"), bStrict);
        Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);

        FString RootSavePath;
        FString SpecName;
        FString ParentClass;
        FString SourceName;
        FString RequestId;
        Params->TryGetStringField(TEXT("root_save_path"), RootSavePath);
        Params->TryGetStringField(TEXT("spec_name"), SpecName);
        Params->TryGetStringField(TEXT("parent_class"), ParentClass);
        Params->TryGetStringField(TEXT("source_name"), SourceName);
        Params->TryGetStringField(TEXT("request_id"), RequestId);

        FUISpecDocument Document;
        FUISpecValidationResult Validation;
        ParseMarkupDocument(
            Markup,
            Dialect,
            bStrict,
            RootSavePath,
            SpecName,
            ParentClass,
            SourceName,
            bTreatWarningsAsErrors,
            Document,
            Validation);

        return FMonolithActionResult::Success(
            PackMarkupConversionResponse(Document, Validation, Dialect, bStrict, RootSavePath, RequestId));
    }

    /** Pack a populated FUISpecSerializerResult into the JSON response shape. */
    static TSharedPtr<FJsonObject> PackDumpResponse(const FUISpecSerializerResult& R)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField  (TEXT("bSuccess"),    R.bSuccess);
        Out->SetStringField(TEXT("asset_path"),  R.AssetPath);
        if (!R.RequestId.IsEmpty())
        {
            Out->SetStringField(TEXT("request_id"), R.RequestId);
        }
        Out->SetNumberField(TEXT("nodes_visited"),       R.NodesVisited);
        Out->SetNumberField(TEXT("animations_captured"), R.AnimationsCaptured);

        // Document body -- mirrors the parser's input shape.
        Out->SetObjectField(TEXT("spec"), DocumentToJson(R.Document));

        if (R.Errors.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Errs;
            Errs.Reserve(R.Errors.Num());
            for (const FUISpecError& E : R.Errors)
            {
                TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
                EObj->SetStringField(TEXT("category"), E.Category.ToString());
                EObj->SetStringField(TEXT("message"),  E.Message);
                Errs.Add(MakeShared<FJsonValueObject>(EObj));
            }
            Out->SetArrayField(TEXT("errors"), Errs);
        }
        if (R.Warnings.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Warns;
            Warns.Reserve(R.Warnings.Num());
            for (const FUISpecError& W : R.Warnings)
            {
                TSharedPtr<FJsonObject> WObj = MakeShared<FJsonObject>();
                WObj->SetStringField(TEXT("category"), W.Category.ToString());
                WObj->SetStringField(TEXT("message"),  W.Message);
                Warns.Add(MakeShared<FJsonValueObject>(WObj));
            }
            Out->SetArrayField(TEXT("warnings"), Warns);
        }
        return Out;
    }

    static FMonolithActionResult HandleDumpUISpec(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString AssetPath;
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        {
            return FMonolithActionResult::Error(
                TEXT("Missing or empty required param: asset_path"), -32602);
        }

        FUISpecSerializerInputs In;
        In.AssetPath = AssetPath;
        Params->TryGetBoolField(TEXT("emit_defaults"), In.bEmitDefaults);
        Params->TryGetStringField(TEXT("request_id"), In.RequestId);

        const FUISpecSerializerResult R = FUISpecSerializer::Dump(In);
        return FMonolithActionResult::Success(PackDumpResponse(R));
    }

    // ------------------------------------------------------------------
    // ui::audit_widget_layout handler
    //
    // Batch structural validation layered on top of FUISpecSerializer. This
    // stays read-only: it serializes WBP trees and inspects the resulting spec
    // for risky Canvas-slot and text-containment patterns.
    // ------------------------------------------------------------------

    struct FWidgetLayoutAuditAccumulator
    {
        FString AssetPath;
        TSet<FString> AllowedCanvasSlots;
        TSet<FString> SuppressedRuleIds;
        FString RuleProfile = TEXT("shipping");
        TArray<TSharedPtr<FJsonValue>>& Findings;
        TMap<FString, int32> SlotCounts;
        int32 NodeCount = 0;
        int32 CanvasSlotCount = 0;
        int32 ErrorCount = 0;
        int32 WarningCount = 0;

        FWidgetLayoutAuditAccumulator(
            const FString& InAssetPath,
            const TSet<FString>& InAllowedCanvasSlots,
            const TSet<FString>& InSuppressedRuleIds,
            const FString& InRuleProfile,
            TArray<TSharedPtr<FJsonValue>>& InFindings)
            : AssetPath(InAssetPath)
            , AllowedCanvasSlots(InAllowedCanvasSlots)
            , SuppressedRuleIds(InSuppressedRuleIds)
            , RuleProfile(InRuleProfile)
            , Findings(InFindings)
        {
        }
    };

    static bool TryReadStringArrayParam(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* FieldName,
        TArray<FString>& OutValues,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values)
        {
            return true;
        }

        for (int32 Index = 0; Index < Values->Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                OutError = FString::Printf(TEXT("`%s[%d]` must be a string."), FieldName, Index);
                return false;
            }

            FString StringValue;
            if (!Value->TryGetString(StringValue) || StringValue.IsEmpty())
            {
                OutError = FString::Printf(TEXT("`%s[%d]` must be a non-empty string."), FieldName, Index);
                return false;
            }
            OutValues.Add(MoveTemp(StringValue));
        }
        return true;
    }

    static TSharedPtr<FJsonObject> Vec2ToJson(const FVector2D& Value)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("x"), Value.X);
        Out->SetNumberField(TEXT("y"), Value.Y);
        return Out;
    }

    static bool IdentifierMatches(
        const TSet<FString>& Identifiers,
        const FString& AssetPath,
        const FString& WidgetPath,
        const FString& WidgetId)
    {
        return Identifiers.Contains(TEXT("*"))
            || Identifiers.Contains(AssetPath)
            || Identifiers.Contains(WidgetId)
            || Identifiers.Contains(FString::Printf(TEXT("%s::%s"), *AssetPath, *WidgetId))
            || Identifiers.Contains(FString::Printf(TEXT("%s::%s"), *AssetPath, *WidgetPath));
    }

    static FString NormalizeLayoutAuditRuleProfile(const FString& InProfile)
    {
        FString Profile = InProfile;
        Profile.TrimStartAndEndInline();
        Profile.ToLowerInline();
        if (Profile == TEXT("strict") || Profile == TEXT("shipping") || Profile == TEXT("advisory"))
        {
            return Profile;
        }
        return TEXT("shipping");
    }

    static const TCHAR* ResolveLayoutAuditSeverity(
        const FString& RuleProfile,
        const FString& RuleId,
        const TCHAR* DefaultSeverity)
    {
        if (RuleId.Equals(TEXT("EdgeUiMissingSafeZone"), ESearchCase::IgnoreCase)
            && RuleProfile.Equals(TEXT("strict"), ESearchCase::IgnoreCase))
        {
            return TEXT("error");
        }
        return DefaultSeverity;
    }

    static void AddLayoutAuditFinding(
        FWidgetLayoutAuditAccumulator& Acc,
        const TCHAR* Severity,
        const TCHAR* Category,
        const FString& WidgetPath,
        const FName& WidgetId,
        const FString& Message,
        const FString& SuggestedFix,
        const TSharedPtr<FJsonObject>& Details = nullptr)
    {
        const FString RuleId(Category);
        if (Acc.SuppressedRuleIds.Contains(RuleId))
        {
            return;
        }

        const TCHAR* EffectiveSeverity = ResolveLayoutAuditSeverity(Acc.RuleProfile, RuleId, Severity);

        TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
        Finding->SetStringField(TEXT("severity"), EffectiveSeverity);
        Finding->SetStringField(TEXT("category"), RuleId);
        Finding->SetStringField(TEXT("rule_id"), RuleId);
        Finding->SetStringField(TEXT("rule_profile"), Acc.RuleProfile);
        Finding->SetStringField(TEXT("asset_path"), Acc.AssetPath);
        Finding->SetStringField(TEXT("widget_id"), WidgetId.ToString());
        Finding->SetStringField(TEXT("widget_path"), WidgetPath);
        Finding->SetStringField(TEXT("message"), Message);
        Finding->SetStringField(TEXT("suggested_fix"), SuggestedFix);
        if (Details.IsValid())
        {
            Finding->SetObjectField(TEXT("details"), Details);
        }

        if (FCString::Stricmp(EffectiveSeverity, TEXT("error")) == 0)
        {
            ++Acc.ErrorCount;
        }
        else if (FCString::Stricmp(EffectiveSeverity, TEXT("warning")) == 0)
        {
            ++Acc.WarningCount;
        }

        Acc.Findings.Add(MakeShared<FJsonValueObject>(Finding));
    }

    static bool AnchorNameContains(const FName& AnchorPreset, const TCHAR* Token)
    {
        return AnchorPreset.ToString().Contains(Token, ESearchCase::IgnoreCase);
    }

    static bool IsTextLikeNode(const FUISpecNode& Node)
    {
        const FString Type = Node.Type.ToString();
        return Type.Equals(TEXT("TextBlock"), ESearchCase::IgnoreCase)
            || Type.Equals(TEXT("RichTextBlock"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("Text"), ESearchCase::IgnoreCase);
    }

    static bool IsDynamicTextLikeId(const FName& WidgetId)
    {
        const FString Id = WidgetId.ToString();
        static const TCHAR* DynamicTokens[] =
        {
            TEXT("Stage"),
            TEXT("Wave"),
            TEXT("Prompt"),
            TEXT("Info"),
            TEXT("Description"),
            TEXT("Name"),
            TEXT("Title"),
            TEXT("Label"),
        };

        for (const TCHAR* Token : DynamicTokens)
        {
            if (Id.Contains(Token, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    static FString MakeLayoutWidgetPath(const FString& ParentPath, const FName& WidgetId)
    {
        return ParentPath.IsEmpty()
            ? WidgetId.ToString()
            : FString::Printf(TEXT("%s/%s"), *ParentPath, *WidgetId.ToString());
    }

    static bool NodeTypeEquals(const FUISpecNode& Node, const TCHAR* TypeName)
    {
        return Node.Type.ToString().Equals(TypeName, ESearchCase::IgnoreCase);
    }

    static bool IsCanvasPanelNode(const FUISpecNode& Node)
    {
        return NodeTypeEquals(Node, TEXT("CanvasPanel"));
    }

    static bool IsSafeZoneNode(const FUISpecNode& Node)
    {
        return NodeTypeEquals(Node, TEXT("SafeZone"));
    }

    static bool IsLayeredPanelType(const FName& Type)
    {
        return Type == FName(TEXT("CanvasPanel")) || Type == FName(TEXT("Overlay"));
    }

    static bool IsListLikePanelNode(const FUISpecNode& Node)
    {
        const FString Type = Node.Type.ToString();
        return Type.Equals(TEXT("ScrollBox"), ESearchCase::IgnoreCase)
            || Type.Equals(TEXT("VerticalBox"), ESearchCase::IgnoreCase)
            || Type.Equals(TEXT("HorizontalBox"), ESearchCase::IgnoreCase)
            || Type.Equals(TEXT("WrapBox"), ESearchCase::IgnoreCase)
            || Type.Equals(TEXT("GridPanel"), ESearchCase::IgnoreCase)
            || Type.Equals(TEXT("UniformGridPanel"), ESearchCase::IgnoreCase);
    }

    static bool IsInteractiveNodeType(const FUISpecNode& Node)
    {
        const FString Type = Node.Type.ToString();
        return Type.Contains(TEXT("Button"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("CheckBox"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("ComboBox"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("EditableText"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("InputKeySelector"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("Slider"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("SpinBox"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("ListView"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("TileView"), ESearchCase::IgnoreCase);
    }

    static bool IsButtonLikeInteractiveNodeType(const FUISpecNode& Node)
    {
        const FString Type = Node.Type.ToString();
        return Type.Contains(TEXT("Button"), ESearchCase::IgnoreCase)
            || Type.Contains(TEXT("CheckBox"), ESearchCase::IgnoreCase);
    }

    static bool HasInteractiveStateStyleEvidence(const FUISpecNode& Node)
    {
        return !Node.StyleRef.IsNone()
            || (Node.bHasCommonUI && Node.CommonUI.StyleRefs.Num() > 0);
    }

    static TArray<TSharedPtr<FJsonValue>> MakeInteractiveStateNamesJson()
    {
        TArray<TSharedPtr<FJsonValue>> States;
        States.Add(MakeShared<FJsonValueString>(TEXT("normal")));
        States.Add(MakeShared<FJsonValueString>(TEXT("hovered")));
        States.Add(MakeShared<FJsonValueString>(TEXT("pressed")));
        States.Add(MakeShared<FJsonValueString>(TEXT("disabled")));
        States.Add(MakeShared<FJsonValueString>(TEXT("focused")));
        return States;
    }

    static FString GetUiAuditMaterialDomainString(UMaterialInterface* Material)
    {
        const UMaterial* BaseMaterial = Material ? Material->GetMaterial() : nullptr;
        if (!BaseMaterial)
        {
            return TEXT("unknown");
        }

        const UEnum* DomainEnum = StaticEnum<EMaterialDomain>();
        return DomainEnum
            ? DomainEnum->GetNameStringByValue(static_cast<int64>(BaseMaterial->MaterialDomain))
            : TEXT("unknown");
    }

    static bool IsUiDomainMaterial(UMaterialInterface* Material)
    {
        const UMaterial* BaseMaterial = Material ? Material->GetMaterial() : nullptr;
        return BaseMaterial && BaseMaterial->MaterialDomain == MD_UI;
    }

    static bool IsHitTestVisible(const FUISpecNode& Node)
    {
        const FString Visibility = Node.Style.Visibility.ToString();
        return Visibility.IsEmpty()
            || Visibility.Equals(TEXT("Visible"), ESearchCase::IgnoreCase);
    }

    static bool IsHiddenVisibility(const FUISpecNode& Node)
    {
        return Node.Style.Visibility.ToString().Equals(TEXT("Hidden"), ESearchCase::IgnoreCase);
    }

    static bool HasInteractiveDescendant(const FUISpecNode& Node)
    {
        if (IsInteractiveNodeType(Node))
        {
            return true;
        }

        for (const TSharedPtr<FUISpecNode>& Child : Node.Children)
        {
            if (Child.IsValid() && HasInteractiveDescendant(*Child))
            {
                return true;
            }
        }
        return false;
    }

    static bool IsDecorativeLayerCandidate(const FUISpecNode& Node)
    {
        if (!IsHitTestVisible(Node))
        {
            return false;
        }

        const FString Type = Node.Type.ToString();
        const bool bDecorativeType =
            Type.Equals(TEXT("Image"), ESearchCase::IgnoreCase)
            || Type.Equals(TEXT("Border"), ESearchCase::IgnoreCase);
        return bDecorativeType && !HasInteractiveDescendant(Node);
    }

    static bool IsLikelyCoveringLayer(const FUISpecNode& Node, const FName& ParentType)
    {
        if (ParentType == FName(TEXT("Overlay")))
        {
            return true;
        }

        return AnchorNameContains(Node.Slot.AnchorPreset, TEXT("stretch"))
            || (Node.Slot.Size.X >= 64.f && Node.Slot.Size.Y >= 32.f)
            || (Node.Style.Width >= 64.f && Node.Style.Height >= 32.f);
    }

    static bool IsLayerAbove(
        const FUISpecNode& Node,
        int32 NodeIndex,
        const FUISpecNode& Other,
        int32 OtherIndex,
        const FName& ParentType)
    {
        if (ParentType == FName(TEXT("CanvasPanel")))
        {
            return Node.Slot.ZOrder > Other.Slot.ZOrder
                || (Node.Slot.ZOrder == Other.Slot.ZOrder && NodeIndex > OtherIndex);
        }
        return NodeIndex > OtherIndex;
    }

    static bool IsSingleChildCanvasWrapper(const FUISpecNode& Node)
    {
        if (!IsCanvasPanelNode(Node) || Node.Children.Num() != 1 || !Node.Children[0].IsValid())
        {
            return false;
        }

        const FUISpecNode& Child = *Node.Children[0];
        return Child.Slot.ZOrder == 0
            && !AnchorNameContains(Child.Slot.AnchorPreset, TEXT("right"))
            && !AnchorNameContains(Child.Slot.AnchorPreset, TEXT("bottom"))
            && !AnchorNameContains(Child.Slot.AnchorPreset, TEXT("stretch"));
    }

    static bool IsEdgeUiCandidate(const FUISpecNode& Node, bool bCanvasSlot)
    {
        if (!bCanvasSlot)
        {
            return false;
        }

        const bool bEdgeAnchor =
            AnchorNameContains(Node.Slot.AnchorPreset, TEXT("top"))
            || AnchorNameContains(Node.Slot.AnchorPreset, TEXT("bottom"))
            || AnchorNameContains(Node.Slot.AnchorPreset, TEXT("left"))
            || AnchorNameContains(Node.Slot.AnchorPreset, TEXT("right"));
        const bool bNearEdge =
            Node.Slot.Position.X <= 32.f
            || Node.Slot.Position.Y <= 32.f
            || Node.Slot.Position.X < -KINDA_SMALL_NUMBER
            || Node.Slot.Position.Y < -KINDA_SMALL_NUMBER;
        const bool bHasAuthoredFootprint =
            (Node.Slot.Size.X > 0.f && Node.Slot.Size.Y > 0.f)
            || (Node.Style.Width > 0.f && Node.Style.Height > 0.f);

        return bEdgeAnchor && bNearEdge && bHasAuthoredFootprint;
    }

    static bool NodeAddsWidthBound(const FUISpecNode& Node, bool bCanvasSlot)
    {
        return Node.Style.Width > 0.f
            || (Node.Style.bOverrideMaxDesiredWidth && Node.Style.MaxDesiredWidth > 0.f)
            || (bCanvasSlot && Node.Slot.Size.X > 0.f && !Node.Slot.bAutoSize);
    }

    static void AuditParentLevelLayoutRules(
        const FUISpecNode& Node,
        const FName& ParentType,
        const FString& WidgetPath,
        int32 CanvasDepth,
        FWidgetLayoutAuditAccumulator& Acc)
    {
        if (!ParentType.IsNone() && IsSingleChildCanvasWrapper(Node))
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetNumberField(TEXT("child_count"), Node.Children.Num());
            Details->SetStringField(TEXT("child_id"), Node.Children[0]->Id.ToString());
            AddLayoutAuditFinding(
                Acc,
                TEXT("warning"),
                TEXT("OneChildCanvasWrapper"),
                WidgetPath,
                Node.Id,
                TEXT("Nested CanvasPanel has a single child and no serialized anchor/Z-order need."),
                TEXT("Replace the wrapper with the child or an automatic layout owner such as Overlay, SizeBox, Border, VerticalBox, or HorizontalBox."),
                Details);
        }

        if (IsCanvasPanelNode(Node) && (CanvasDepth >= 2 || ParentType == FName(TEXT("CanvasPanel"))))
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetNumberField(TEXT("canvas_depth"), CanvasDepth);
            Details->SetNumberField(TEXT("child_count"), Node.Children.Num());
            AddLayoutAuditFinding(
                Acc,
                TEXT("warning"),
                TEXT("CanvasOveruse"),
                WidgetPath,
                Node.Id,
                TEXT("Nested CanvasPanel usage increases absolute-placement drift and makes responsive UMG harder to prove."),
                TEXT("Keep CanvasPanel as the intentional top-level absolute-placement owner, then use automatic layout containers inside it unless the slot is explicitly allowlisted."),
                Details);
        }

        if (IsListLikePanelNode(Node) && Node.Children.Num() >= 12)
        {
            TMap<FString, int32> TypeCounts;
            FString MostCommonType;
            int32 MostCommonCount = 0;
            for (const TSharedPtr<FUISpecNode>& Child : Node.Children)
            {
                if (!Child.IsValid())
                {
                    continue;
                }
                const FString ChildType = Child->Type.ToString();
                const int32 Count = ++TypeCounts.FindOrAdd(ChildType);
                if (Count > MostCommonCount)
                {
                    MostCommonCount = Count;
                    MostCommonType = ChildType;
                }
            }

            if (MostCommonCount >= 8)
            {
                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetNumberField(TEXT("child_count"), Node.Children.Num());
                Details->SetStringField(TEXT("repeated_child_type"), MostCommonType);
                Details->SetNumberField(TEXT("repeated_child_count"), MostCommonCount);
                AddLayoutAuditFinding(
                    Acc,
                    TEXT("warning"),
                    TEXT("LargeStaticListWithoutListView"),
                    WidgetPath,
                    Node.Id,
                    TEXT("Large repeated static child set found under a panel that does not virtualize entries."),
                    TEXT("Use ui.setup_list_view or a TileView/ListView-backed entry widget for large dynamic collections, or suppress this rule when the list is intentionally small and static."),
                    Details);
            }
        }

        if (!IsLayeredPanelType(Node.Type))
        {
            return;
        }

        for (int32 ChildIndex = 0; ChildIndex < Node.Children.Num(); ++ChildIndex)
        {
            const TSharedPtr<FUISpecNode>& Child = Node.Children[ChildIndex];
            if (!Child.IsValid()
                || !IsDecorativeLayerCandidate(*Child)
                || !IsLikelyCoveringLayer(*Child, Node.Type))
            {
                continue;
            }

            FString BlockedSibling;
            for (int32 OtherIndex = 0; OtherIndex < Node.Children.Num(); ++OtherIndex)
            {
                if (OtherIndex == ChildIndex)
                {
                    continue;
                }

                const TSharedPtr<FUISpecNode>& Other = Node.Children[OtherIndex];
                if (Other.IsValid()
                    && IsLayerAbove(*Child, ChildIndex, *Other, OtherIndex, Node.Type)
                    && HasInteractiveDescendant(*Other))
                {
                    BlockedSibling = Other->Id.ToString();
                    break;
                }
            }

            if (!BlockedSibling.IsEmpty())
            {
                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetStringField(TEXT("parent_type"), Node.Type.ToString());
                Details->SetStringField(TEXT("blocked_interactive_sibling"), BlockedSibling);
                Details->SetNumberField(TEXT("child_index"), ChildIndex);
                Details->SetNumberField(TEXT("z_order"), Child->Slot.ZOrder);
                AddLayoutAuditFinding(
                    Acc,
                    TEXT("error"),
                    TEXT("DecorativeHitTestBlocker"),
                    MakeLayoutWidgetPath(WidgetPath, Child->Id),
                    Child->Id,
                    TEXT("Decorative Image/Border layer is hit-test visible above an interactive widget."),
                    TEXT("Route the layer through ui.set_widget_property Visibility=SelfHitTestInvisible/HitTestInvisible, lower its Z-order, or provide runtime interaction proof if it is intentionally interactive."),
                    Details);
            }
        }
    }

    static void AuditWidgetNodeRecursive(
        const FUISpecNode& Node,
        const FName& ParentType,
        const FString& WidgetPath,
        bool bInheritedWidthBound,
        int32 CanvasDepth,
        bool bHasSafeZoneAncestor,
        FWidgetLayoutAuditAccumulator& Acc)
    {
        ++Acc.NodeCount;

        const bool bCanvasSlot = ParentType == FName(TEXT("CanvasPanel"));
        const int32 CurrentCanvasDepth = CanvasDepth + (IsCanvasPanelNode(Node) ? 1 : 0);
        const bool bCurrentSafeZoneAncestor = bHasSafeZoneAncestor || IsSafeZoneNode(Node);
        AuditParentLevelLayoutRules(Node, ParentType, WidgetPath, CurrentCanvasDepth, Acc);

        if (!ParentType.IsNone())
        {
            const FString SlotType = FString::Printf(TEXT("%sSlot"), *ParentType.ToString());
            Acc.SlotCounts.FindOrAdd(SlotType)++;
        }

        if (bCanvasSlot)
        {
            ++Acc.CanvasSlotCount;

            const FString WidgetId = Node.Id.ToString();
            if (!IdentifierMatches(Acc.AllowedCanvasSlots, Acc.AssetPath, WidgetPath, WidgetId))
            {
                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetStringField(TEXT("anchor_preset"), Node.Slot.AnchorPreset.ToString());
                Details->SetObjectField(TEXT("position"), Vec2ToJson(Node.Slot.Position));
                Details->SetObjectField(TEXT("size"), Vec2ToJson(Node.Slot.Size));
                Details->SetObjectField(TEXT("alignment"), Vec2ToJson(Node.Slot.Alignment));
                Details->SetBoolField(TEXT("auto_size"), Node.Slot.bAutoSize);
                AddLayoutAuditFinding(
                    Acc,
                    TEXT("warning"),
                    TEXT("UnwhitelistedCanvasSlot"),
                    WidgetPath,
                    Node.Id,
                    TEXT("CanvasPanelSlot found without an explicit audit allowlist entry."),
                    TEXT("Move this child to an automatic layout container, or add an explicit allowlist identifier if the Canvas placement is intentional."),
                    Details);
            }

            const bool bRightAnchored = AnchorNameContains(Node.Slot.AnchorPreset, TEXT("right"));
            const bool bBottomAnchored = AnchorNameContains(Node.Slot.AnchorPreset, TEXT("bottom"));
            const bool bNegativeRightOffset = Node.Slot.Position.X < -KINDA_SMALL_NUMBER;
            const bool bNegativeBottomOffset = Node.Slot.Position.Y < -KINDA_SMALL_NUMBER;
            const bool bRightAlignmentMismatch = bRightAnchored && Node.Slot.Alignment.X < 0.5f && bNegativeRightOffset;
            const bool bBottomAlignmentMismatch = bBottomAnchored && Node.Slot.Alignment.Y < 0.5f && bNegativeBottomOffset;
            if (bRightAlignmentMismatch || bBottomAlignmentMismatch)
            {
                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetStringField(TEXT("anchor_preset"), Node.Slot.AnchorPreset.ToString());
                Details->SetObjectField(TEXT("position"), Vec2ToJson(Node.Slot.Position));
                Details->SetObjectField(TEXT("alignment"), Vec2ToJson(Node.Slot.Alignment));
                Details->SetBoolField(TEXT("right_alignment_mismatch"), bRightAlignmentMismatch);
                Details->SetBoolField(TEXT("bottom_alignment_mismatch"), bBottomAlignmentMismatch);
                AddLayoutAuditFinding(
                    Acc,
                    TEXT("error"),
                    TEXT("CanvasAnchorMismatch"),
                    WidgetPath,
                    Node.Id,
                    TEXT("Edge-anchored Canvas slot uses negative offsets with an opposing zero-style alignment."),
                    TEXT("Match right/bottom anchors with right/bottom alignment, or move this placement into a single responsive layout owner."),
                    Details);
            }

            if (!bCurrentSafeZoneAncestor && IsEdgeUiCandidate(Node, bCanvasSlot))
            {
                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetStringField(TEXT("anchor_preset"), Node.Slot.AnchorPreset.ToString());
                Details->SetObjectField(TEXT("position"), Vec2ToJson(Node.Slot.Position));
                Details->SetObjectField(TEXT("size"), Vec2ToJson(Node.Slot.Size));
                Details->SetBoolField(TEXT("has_safe_zone_ancestor"), bCurrentSafeZoneAncestor);
                AddLayoutAuditFinding(
                    Acc,
                    TEXT("warning"),
                    TEXT("EdgeUiMissingSafeZone"),
                    WidgetPath,
                    Node.Id,
                    TEXT("Edge-anchored Canvas child has no SafeZone ancestor in the serialized widget tree."),
                    TEXT("Wrap HUD edge groups in SafeZone, or compose ui.measure_widget_layout with explicit safe_zone profiles and suppress this rule only with matching visual/runtime proof."),
                    Details);
            }
        }

        const bool bHasWidthBound = bInheritedWidthBound || NodeAddsWidthBound(Node, bCanvasSlot);
        const bool bHasWrap = Node.Content.WrapMode == FName(TEXT("Wrap"));
        if (IsTextLikeNode(Node) && IsDynamicTextLikeId(Node.Id) && !bHasWrap && !bHasWidthBound)
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetStringField(TEXT("wrap_mode"), Node.Content.WrapMode.ToString());
            Details->SetNumberField(TEXT("font_size"), Node.Content.FontSize);
            Details->SetNumberField(TEXT("text_length"), Node.Content.Text.Len());
            AddLayoutAuditFinding(
                Acc,
                TEXT("warning"),
                TEXT("UnboundedDynamicText"),
                WidgetPath,
                Node.Id,
                TEXT("Dynamic text-like widget has no serialized wrap mode and no inherited width bound."),
                    TEXT("Add a SizeBox max width, explicit wrapping/truncation policy, or a bounded parent slot so long localized text cannot escape its background."),
                Details);
        }

        if (IsInteractiveNodeType(Node) && IsHiddenVisibility(Node))
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetStringField(TEXT("visibility"), Node.Style.Visibility.ToString());
            Details->SetStringField(TEXT("widget_type"), Node.Type.ToString());
            AddLayoutAuditFinding(
                Acc,
                TEXT("warning"),
                TEXT("HiddenInteractiveSpace"),
                WidgetPath,
                Node.Id,
                TEXT("Interactive widget uses Hidden visibility, which still occupies layout space."),
                TEXT("Use Collapsed when removing an interactive control from responsive layout, or keep Hidden only when reserved space is intentional and documented."),
                Details);
        }

        if (!Node.Content.BrushPath.IsEmpty())
        {
            if (UMaterialInterface* BoundMaterial = LoadObject<UMaterialInterface>(nullptr, *Node.Content.BrushPath))
            {
                if (!IsUiDomainMaterial(BoundMaterial))
                {
                    TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                    Details->SetStringField(TEXT("widget_type"), Node.Type.ToString());
                    Details->SetStringField(TEXT("material_path"), Node.Content.BrushPath);
                    Details->SetStringField(TEXT("material_domain"), GetUiAuditMaterialDomainString(BoundMaterial));
                    Details->SetStringField(TEXT("expected_domain"), TEXT("MD_UI"));
                    Details->SetStringField(TEXT("binding_field"), TEXT("content.brushPath"));
                    AddLayoutAuditFinding(
                        Acc,
                        TEXT("error"),
                        TEXT("MaterialDomainMismatch"),
                        WidgetPath,
                        Node.Id,
                        TEXT("UMG-bound brush material is not UI-domain."),
                        TEXT("Use a material whose Material Domain is UI, or route material creation through workflow.ui_material_hlsl_effect / material owner actions and rerun widget proof before shipping."),
                        Details);
                }
            }
        }

        if (IsButtonLikeInteractiveNodeType(Node) && !HasInteractiveStateStyleEvidence(Node))
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetStringField(TEXT("widget_type"), Node.Type.ToString());
            Details->SetBoolField(TEXT("has_style_ref"), !Node.StyleRef.IsNone());
            Details->SetBoolField(TEXT("has_commonui_style_ref"), Node.bHasCommonUI && Node.CommonUI.StyleRefs.Num() > 0);
            Details->SetBoolField(TEXT("has_commonui_bag"), Node.bHasCommonUI);
            Details->SetStringField(TEXT("native_state_style_inspection"), TEXT("not_serialized"));
            Details->SetArrayField(TEXT("expected_interactive_states"), MakeInteractiveStateNamesJson());
            AddLayoutAuditFinding(
                Acc,
                TEXT("warning"),
                TEXT("UnstyledInteractiveState"),
                WidgetPath,
                Node.Id,
                TEXT("Button-like widget has no serialized style reference or CommonUI style evidence for interactive states."),
                TEXT("Attach a CommonUI styleRef or named UI-spec style evidence for normal/hovered/pressed/disabled/focused states, then rerun workflow.ui_shipping_widget_blueprint for visual/runtime proof; suppress only when engine-default styling is intentional."),
                Details);
        }

        for (const TSharedPtr<FUISpecNode>& Child : Node.Children)
        {
            if (!Child.IsValid())
            {
                continue;
            }

            const FString ChildPath = MakeLayoutWidgetPath(WidgetPath, Child->Id);
            AuditWidgetNodeRecursive(
                *Child,
                Node.Type,
                ChildPath,
                bHasWidthBound,
                CurrentCanvasDepth,
                bCurrentSafeZoneAncestor,
                Acc);
        }
    }

    static void CollectWidgetBlueprintAssetPaths(const FString& PathPrefix, bool bIncludeTests, TArray<FString>& OutAssetPaths)
    {
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        FARFilter Filter;
        Filter.bRecursivePaths = true;
        Filter.PackagePaths.Add(FName(*PathPrefix));
        Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());

        TArray<FAssetData> Assets;
        AssetRegistry.GetAssets(Filter, Assets);
        OutAssetPaths.Reserve(OutAssetPaths.Num() + Assets.Num());
        for (const FAssetData& Asset : Assets)
        {
            const FString PackageName = Asset.PackageName.ToString();
            if (!bIncludeTests && PackageName.StartsWith(TEXT("/Game/Tests/")))
            {
                continue;
            }
            OutAssetPaths.Add(PackageName);
        }
        OutAssetPaths.Sort();
    }

    static FMonolithActionResult HandleAuditWidgetLayout(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        TArray<FString> AssetPaths;
        TArray<FString> AllowedCanvasSlots;
        TArray<FString> SuppressedRuleIds;
        FString ParamError;
        if (!TryReadStringArrayParam(Params, TEXT("asset_paths"), AssetPaths, ParamError)
            || !TryReadStringArrayParam(Params, TEXT("allowed_canvas_slots"), AllowedCanvasSlots, ParamError)
            || !TryReadStringArrayParam(Params, TEXT("suppress_rule_ids"), SuppressedRuleIds, ParamError))
        {
            return FMonolithActionResult::Error(ParamError, -32602);
        }

        bool bIncludeTests = false;
        bool bTreatWarningsAsErrors = false;
        Params->TryGetBoolField(TEXT("include_tests"), bIncludeTests);
        Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);

        FString PathPrefix = TEXT("/Game/UI");
        Params->TryGetStringField(TEXT("path_prefix"), PathPrefix);
        FString RuleProfile = TEXT("shipping");
        Params->TryGetStringField(TEXT("rule_profile"), RuleProfile);
        RuleProfile = NormalizeLayoutAuditRuleProfile(RuleProfile);
        if (AssetPaths.Num() == 0)
        {
            CollectWidgetBlueprintAssetPaths(PathPrefix, bIncludeTests, AssetPaths);
        }

        TSet<FString> AllowedSet;
        for (const FString& Identifier : AllowedCanvasSlots)
        {
            AllowedSet.Add(Identifier);
        }

        TSet<FString> SuppressedRuleSet;
        for (const FString& RuleId : SuppressedRuleIds)
        {
            SuppressedRuleSet.Add(RuleId);
        }

        TArray<TSharedPtr<FJsonValue>> AssetReports;
        TArray<TSharedPtr<FJsonValue>> Findings;
        int32 TotalNodes = 0;
        int32 TotalCanvasSlots = 0;
        int32 TotalErrors = 0;
        int32 TotalWarnings = 0;
        int32 DumpErrors = 0;

        for (const FString& AssetPath : AssetPaths)
        {
            FUISpecSerializerInputs DumpInputs;
            DumpInputs.AssetPath = AssetPath;
            const FUISpecSerializerResult DumpResult = FUISpecSerializer::Dump(DumpInputs);

            TSharedPtr<FJsonObject> AssetOut = MakeShared<FJsonObject>();
            AssetOut->SetStringField(TEXT("asset_path"), AssetPath);
            AssetOut->SetBoolField(TEXT("dump_success"), DumpResult.bSuccess);
            AssetOut->SetNumberField(TEXT("nodes_visited"), DumpResult.NodesVisited);

            if (!DumpResult.bSuccess || !DumpResult.Document.Root.IsValid())
            {
                ++DumpErrors;
                TArray<TSharedPtr<FJsonValue>> Errors;
                for (const FUISpecError& Error : DumpResult.Errors)
                {
                    TSharedPtr<FJsonObject> ErrorOut = MakeShared<FJsonObject>();
                    ErrorOut->SetStringField(TEXT("category"), Error.Category.ToString());
                    ErrorOut->SetStringField(TEXT("message"), Error.Message);
                    Errors.Add(MakeShared<FJsonValueObject>(ErrorOut));
                }
                AssetOut->SetArrayField(TEXT("errors"), Errors);
                AssetReports.Add(MakeShared<FJsonValueObject>(AssetOut));
                continue;
            }

            AssetOut->SetStringField(TEXT("parent_class"), DumpResult.Document.ParentClass);
            AssetOut->SetStringField(TEXT("root_type"), DumpResult.Document.Root->Type.ToString());
            AssetOut->SetStringField(TEXT("root_widget"), DumpResult.Document.Root->Id.ToString());

            FWidgetLayoutAuditAccumulator Acc(AssetPath, AllowedSet, SuppressedRuleSet, RuleProfile, Findings);
            AuditWidgetNodeRecursive(
                *DumpResult.Document.Root,
                NAME_None,
                DumpResult.Document.Root->Id.ToString(),
                /*bInheritedWidthBound=*/false,
                /*CanvasDepth=*/0,
                /*bHasSafeZoneAncestor=*/false,
                Acc);

            TSharedPtr<FJsonObject> SlotCounts = MakeShared<FJsonObject>();
            for (const TPair<FString, int32>& Pair : Acc.SlotCounts)
            {
                SlotCounts->SetNumberField(Pair.Key, Pair.Value);
            }

            AssetOut->SetNumberField(TEXT("node_count"), Acc.NodeCount);
            AssetOut->SetNumberField(TEXT("canvas_slot_count"), Acc.CanvasSlotCount);
            AssetOut->SetNumberField(TEXT("error_count"), Acc.ErrorCount);
            AssetOut->SetNumberField(TEXT("warning_count"), Acc.WarningCount);
            AssetOut->SetObjectField(TEXT("slot_counts"), SlotCounts);
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetOut));

            TotalNodes += Acc.NodeCount;
            TotalCanvasSlots += Acc.CanvasSlotCount;
            TotalErrors += Acc.ErrorCount;
            TotalWarnings += Acc.WarningCount;
        }

        if (AssetPaths.Num() == 0)
        {
            ++TotalErrors;
            TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
            Finding->SetStringField(TEXT("severity"), TEXT("error"));
            Finding->SetStringField(TEXT("category"), TEXT("AssetInventory"));
            Finding->SetStringField(TEXT("asset_path"), PathPrefix);
            Finding->SetStringField(TEXT("message"), TEXT("No WidgetBlueprint assets were found for the requested path_prefix."));
            Finding->SetStringField(TEXT("suggested_fix"), TEXT("Pass explicit asset_paths or verify the path_prefix points at a content folder containing WidgetBlueprint assets."));
            Findings.Add(MakeShared<FJsonValueObject>(Finding));
        }

        const bool bPassed = DumpErrors == 0
            && TotalErrors == 0
            && (!bTreatWarningsAsErrors || TotalWarnings == 0);

        TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("assets_scanned"), AssetPaths.Num());
        Summary->SetNumberField(TEXT("nodes_scanned"), TotalNodes);
        Summary->SetNumberField(TEXT("canvas_slots"), TotalCanvasSlots);
        Summary->SetNumberField(TEXT("error_count"), TotalErrors + DumpErrors);
        Summary->SetNumberField(TEXT("warning_count"), TotalWarnings);
        Summary->SetNumberField(TEXT("dump_error_count"), DumpErrors);
        Summary->SetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);
        Summary->SetStringField(TEXT("rule_profile"), RuleProfile);

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("bSuccess"), bPassed);
        Out->SetStringField(TEXT("status"), bPassed ? TEXT("ok") : TEXT("findings_failed"));
        Out->SetStringField(TEXT("path_prefix"), PathPrefix);
        Out->SetObjectField(TEXT("summary"), Summary);
        Out->SetArrayField(TEXT("assets"), AssetReports);
        Out->SetArrayField(TEXT("findings"), Findings);
        return FMonolithActionResult::Success(Out);
    }

    // ------------------------------------------------------------------
    // ui::audit_widget_material_lifecycle handler
    //
    // Read-only Widget Blueprint graph audit for dynamic material instance
    // creation sites. This is intentionally separate from layout lint: layout
    // reads the authored widget tree, while this action reads K2 graph evidence
    // and flags MID creation in repeated lifecycle execution paths.
    // ------------------------------------------------------------------

    struct FMaterialLifecycleGraphContext
    {
        FString GraphType = TEXT("unknown");
        FString GraphLifecycle = TEXT("unknown");
        bool bGraphNameIsRepeatedLifecycle = false;
        bool bGraphContainsRepeatedLifecycleEvent = false;
        TMap<const UEdGraphNode*, FString> RepeatedReachability;
    };

    static bool TextMatchesAnyToken(const FString& InText, const TArray<FString>& Tokens)
    {
        const FString Lower = InText.ToLower();
        for (const FString& Token : Tokens)
        {
            if (Lower.Contains(Token))
            {
                return true;
            }
        }
        return false;
    }

    static FString DetectRepeatedLifecycleToken(const FString& InText)
    {
        const FString Lower = InText.ToLower();
        if (Lower.Contains(TEXT("receivetick")) || Lower.Contains(TEXT("event tick")) || Lower.Contains(TEXT(" tick")) || Lower == TEXT("tick"))
        {
            return TEXT("tick");
        }
        if (Lower.Contains(TEXT("synchronizeproperties")) || Lower.Contains(TEXT("synchronize properties")))
        {
            return TEXT("synchronize_properties");
        }
        if (Lower.Contains(TEXT("onpaint")) || Lower.Contains(TEXT("nativepaint")) || Lower.Contains(TEXT(" paint")))
        {
            return TEXT("paint");
        }
        if (Lower.Contains(TEXT("prepass")))
        {
            return TEXT("prepass");
        }
        return FString();
    }

    static FString DetectStableLifecycleToken(const FString& InText)
    {
        const FString Lower = InText.ToLower();
        if (Lower.Contains(TEXT("oninitialized")) || Lower.Contains(TEXT("initialized")))
        {
            return TEXT("initialized");
        }
        if (Lower.Contains(TEXT("preconstruct")) || Lower.Contains(TEXT("pre construct")))
        {
            return TEXT("pre_construct");
        }
        if (Lower.Contains(TEXT("construct")))
        {
            return TEXT("construct");
        }
        return FString();
    }

    static FString GetEventLifecycleToken(const UK2Node_Event* EventNode)
    {
        if (!EventNode)
        {
            return FString();
        }

        FString EventName = EventNode->EventReference.GetMemberName().ToString();
        if (EventName.IsEmpty())
        {
            EventName = EventNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
        }
        return DetectRepeatedLifecycleToken(EventName);
    }

    static FString GetCallFunctionName(const UK2Node_CallFunction* CallNode)
    {
        if (!CallNode)
        {
            return FString();
        }
        if (const UFunction* TargetFunction = CallNode->GetTargetFunction())
        {
            return TargetFunction->GetName();
        }
        return CallNode->FunctionReference.GetMemberName().ToString();
    }

    static FString GetCallOwnerClassName(const UK2Node_CallFunction* CallNode)
    {
        if (!CallNode)
        {
            return FString();
        }
        if (const UFunction* TargetFunction = CallNode->GetTargetFunction())
        {
            return TargetFunction->GetOwnerClass() ? TargetFunction->GetOwnerClass()->GetName() : FString();
        }
        if (const UClass* OwnerClass = CallNode->FunctionReference.GetMemberParentClass())
        {
            return OwnerClass->GetName();
        }
        return FString();
    }

    static bool IsDynamicMaterialCreationCall(const UK2Node_CallFunction* CallNode)
    {
        const FString FunctionName = GetCallFunctionName(CallNode);
        const FString OwnerClass = GetCallOwnerClassName(CallNode);
        if (FunctionName.IsEmpty())
        {
            return false;
        }

        if (FunctionName.Equals(TEXT("Create"), ESearchCase::IgnoreCase)
            && OwnerClass.Contains(TEXT("MaterialInstanceDynamic")))
        {
            return true;
        }

        static const TArray<FString> DynamicMaterialFunctionTokens = {
            TEXT("createdynamicmaterialinstance"),
            TEXT("createandsetmaterialinstancedynamic"),
            TEXT("createnameddynamicmaterialinstance"),
            TEXT("getdynamicmaterial"),
            TEXT("getdefaultdynamicmaterial"),
            TEXT("createdynamicmaterial")
        };
        return TextMatchesAnyToken(FunctionName, DynamicMaterialFunctionTokens);
    }

    static bool IsExecOutputPin(const UEdGraphPin* Pin)
    {
        return Pin
            && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    }

    static void MarkLifecycleReachableNodes(
        const UEdGraphNode* StartNode,
        const FString& Lifecycle,
        TMap<const UEdGraphNode*, FString>& OutReachable)
    {
        if (!StartNode || Lifecycle.IsEmpty())
        {
            return;
        }

        TArray<const UEdGraphNode*> Stack;
        TSet<const UEdGraphNode*> Visited;
        Stack.Add(StartNode);
        Visited.Add(StartNode);

        while (Stack.Num() > 0)
        {
            const UEdGraphNode* Node = Stack.Pop(EAllowShrinking::No);
            OutReachable.FindOrAdd(Node, Lifecycle);

            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!IsExecOutputPin(Pin))
                {
                    continue;
                }

                for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
                    if (LinkedNode && !Visited.Contains(LinkedNode))
                    {
                        Visited.Add(LinkedNode);
                        Stack.Add(LinkedNode);
                    }
                }
            }
        }
    }

    static FString GraphTypeForWidgetBlueprint(const UWidgetBlueprint* WBP, const UEdGraph* Graph)
    {
        if (!WBP || !Graph)
        {
            return TEXT("unknown");
        }
        if (WBP->UbergraphPages.Contains(Graph))
        {
            return TEXT("event_graph");
        }
        if (WBP->FunctionGraphs.Contains(Graph))
        {
            return TEXT("function");
        }
        if (WBP->MacroGraphs.Contains(Graph))
        {
            return TEXT("macro");
        }
        if (WBP->DelegateSignatureGraphs.Contains(Graph))
        {
            return TEXT("delegate_signature");
        }
        return TEXT("unknown");
    }

    static FMaterialLifecycleGraphContext BuildMaterialLifecycleGraphContext(
        const UWidgetBlueprint* WBP,
        const UEdGraph* Graph)
    {
        FMaterialLifecycleGraphContext Context;
        Context.GraphType = GraphTypeForWidgetBlueprint(WBP, Graph);
        if (!Graph)
        {
            return Context;
        }

        Context.GraphLifecycle = DetectRepeatedLifecycleToken(Graph->GetName());
        if (!Context.GraphLifecycle.IsEmpty())
        {
            Context.bGraphNameIsRepeatedLifecycle = true;
        }
        else
        {
            Context.GraphLifecycle = DetectStableLifecycleToken(Graph->GetName());
        }
        if (Context.GraphLifecycle.IsEmpty())
        {
            Context.GraphLifecycle = TEXT("unknown");
        }

        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
            const FString EventLifecycle = GetEventLifecycleToken(EventNode);
            if (!EventLifecycle.IsEmpty())
            {
                Context.bGraphContainsRepeatedLifecycleEvent = true;
                MarkLifecycleReachableNodes(Node, EventLifecycle, Context.RepeatedReachability);
            }
        }
        return Context;
    }

    static void AddMaterialLifecycleFinding(
        TArray<TSharedPtr<FJsonValue>>& Findings,
        int32& ErrorCount,
        int32& WarningCount,
        const TSet<FString>& SuppressedRuleIds,
        const FString& Severity,
        const FString& RuleId,
        const FString& AssetPath,
        const UEdGraph* Graph,
        const FString& GraphType,
        const FString& GraphLifecycle,
        const UK2Node_CallFunction* CallNode,
        const FString& FunctionName,
        const FString& OwnerClass,
        const FString& Message,
        const FString& SuggestedFix,
        bool bConnectedToLifecycle)
    {
        if (SuppressedRuleIds.Contains(RuleId))
        {
            return;
        }

        TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
        Finding->SetStringField(TEXT("severity"), Severity);
        Finding->SetStringField(TEXT("category"), RuleId);
        Finding->SetStringField(TEXT("rule_id"), RuleId);
        Finding->SetStringField(TEXT("asset_path"), AssetPath);
        Finding->SetStringField(TEXT("graph"), Graph ? Graph->GetName() : FString());
        Finding->SetStringField(TEXT("graph_type"), GraphType);
        Finding->SetStringField(TEXT("graph_lifecycle"), GraphLifecycle);
        Finding->SetStringField(TEXT("node_id"), CallNode ? CallNode->GetName() : FString());
        Finding->SetStringField(TEXT("node_guid"), CallNode ? CallNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
        Finding->SetStringField(TEXT("node_class"), CallNode && CallNode->GetClass() ? CallNode->GetClass()->GetName() : FString());
        Finding->SetStringField(TEXT("node_title"), CallNode ? CallNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : FString());
        Finding->SetStringField(TEXT("function_name"), FunctionName);
        Finding->SetStringField(TEXT("owner_class"), OwnerClass);
        Finding->SetBoolField(TEXT("connected_to_lifecycle_event"), bConnectedToLifecycle);
        Finding->SetStringField(TEXT("message"), Message);
        Finding->SetStringField(TEXT("suggested_fix"), SuggestedFix);

        if (Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
        {
            ++ErrorCount;
        }
        else if (Severity.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
        {
            ++WarningCount;
        }
        Findings.Add(MakeShared<FJsonValueObject>(Finding));
    }

    static FMonolithActionResult HandleAuditWidgetMaterialLifecycle(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        TArray<FString> AssetPaths;
        TArray<FString> SuppressedRuleIds;
        FString AssetPath;
        Params->TryGetStringField(TEXT("asset_path"), AssetPath);
        if (!AssetPath.IsEmpty())
        {
            AssetPaths.Add(AssetPath);
        }

        FString ParamError;
        if (!TryReadStringArrayParam(Params, TEXT("asset_paths"), AssetPaths, ParamError)
            || !TryReadStringArrayParam(Params, TEXT("suppress_rule_ids"), SuppressedRuleIds, ParamError))
        {
            return FMonolithActionResult::Error(ParamError, -32602);
        }

        bool bIncludeTests = false;
        bool bIncludeAdvisory = true;
        bool bTreatWarningsAsErrors = false;
        Params->TryGetBoolField(TEXT("include_tests"), bIncludeTests);
        Params->TryGetBoolField(TEXT("include_advisory"), bIncludeAdvisory);
        Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);

        FString PathPrefix = TEXT("/Game/UI");
        Params->TryGetStringField(TEXT("path_prefix"), PathPrefix);
        if (AssetPaths.Num() == 0)
        {
            CollectWidgetBlueprintAssetPaths(PathPrefix, bIncludeTests, AssetPaths);
        }
        TSet<FString> SuppressedRuleSet;
        for (const FString& RuleId : SuppressedRuleIds)
        {
            SuppressedRuleSet.Add(RuleId);
        }

        TArray<TSharedPtr<FJsonValue>> AssetReports;
        TArray<TSharedPtr<FJsonValue>> Findings;
        int32 TotalGraphs = 0;
        int32 TotalNodes = 0;
        int32 TotalDmiCreationCalls = 0;
        int32 TotalErrors = 0;
        int32 TotalWarnings = 0;
        int32 LoadErrors = 0;

        for (const FString& CurrentAssetPath : AssetPaths)
        {
            TSharedPtr<FJsonObject> AssetOut = MakeShared<FJsonObject>();
            AssetOut->SetStringField(TEXT("asset_path"), CurrentAssetPath);

            UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *CurrentAssetPath);
            if (!WBP)
            {
                ++LoadErrors;
                AssetOut->SetBoolField(TEXT("load_success"), false);
                AssetOut->SetStringField(TEXT("error"), TEXT("WidgetBlueprint asset could not be loaded."));
                AssetReports.Add(MakeShared<FJsonValueObject>(AssetOut));
                continue;
            }

            TArray<UEdGraph*> Graphs;
            WBP->GetAllGraphs(Graphs);

            int32 AssetNodeCount = 0;
            int32 AssetDmiCalls = 0;
            int32 AssetErrors = 0;
            int32 AssetWarnings = 0;
            TArray<TSharedPtr<FJsonValue>> GraphReports;

            for (UEdGraph* Graph : Graphs)
            {
                if (!Graph)
                {
                    continue;
                }

                const FMaterialLifecycleGraphContext GraphContext = BuildMaterialLifecycleGraphContext(WBP, Graph);
                ++TotalGraphs;
                AssetNodeCount += Graph->Nodes.Num();
                TotalNodes += Graph->Nodes.Num();

                int32 GraphDmiCalls = 0;
                int32 GraphErrors = 0;
                int32 GraphWarnings = 0;

                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
                    if (!IsDynamicMaterialCreationCall(CallNode))
                    {
                        continue;
                    }

                    ++GraphDmiCalls;
                    ++AssetDmiCalls;
                    ++TotalDmiCreationCalls;
                    const FString FunctionName = GetCallFunctionName(CallNode);
                    const FString OwnerClass = GetCallOwnerClassName(CallNode);

                    const FString* ReachableLifecycle = GraphContext.RepeatedReachability.Find(CallNode);
                    if (ReachableLifecycle)
                    {
                        AddMaterialLifecycleFinding(
                            Findings,
                            GraphErrors,
                            GraphWarnings,
                            SuppressedRuleSet,
                            TEXT("error"),
                            TEXT("DynamicMaterialCreatedInRepeatedLifecycle"),
                            CurrentAssetPath,
                            Graph,
                            GraphContext.GraphType,
                            *ReachableLifecycle,
                            CallNode,
                            FunctionName,
                            OwnerClass,
                            TEXT("A dynamic material instance is created on an execution path reachable from a repeated Widget Blueprint lifecycle event."),
                            TEXT("Create and cache the MID once in OnInitialized/Construct, store it in a member variable, and only update scalar/vector parameters in Tick/Paint/SynchronizeProperties."),
                            true);
                    }
                    else if (GraphContext.bGraphNameIsRepeatedLifecycle)
                    {
                        AddMaterialLifecycleFinding(
                            Findings,
                            GraphErrors,
                            GraphWarnings,
                            SuppressedRuleSet,
                            TEXT("error"),
                            TEXT("DynamicMaterialCreatedInRepeatedLifecycle"),
                            CurrentAssetPath,
                            Graph,
                            GraphContext.GraphType,
                            GraphContext.GraphLifecycle,
                            CallNode,
                            FunctionName,
                            OwnerClass,
                            TEXT("A dynamic material instance is created in a graph whose name matches a repeated Widget Blueprint lifecycle path."),
                            TEXT("Move MID creation into OnInitialized/Construct and reuse a cached MID during repeated lifecycle updates."),
                            true);
                    }
                    else if (GraphContext.bGraphContainsRepeatedLifecycleEvent)
                    {
                        AddMaterialLifecycleFinding(
                            Findings,
                            GraphErrors,
                            GraphWarnings,
                            SuppressedRuleSet,
                            TEXT("warning"),
                            TEXT("DynamicMaterialCreatedNearRepeatedLifecycle"),
                            CurrentAssetPath,
                            Graph,
                            GraphContext.GraphType,
                            GraphContext.GraphLifecycle,
                            CallNode,
                            FunctionName,
                            OwnerClass,
                            TEXT("A dynamic material instance is created in a graph that contains a repeated lifecycle event, but the static exec walk did not prove reachability."),
                            TEXT("Confirm the call is not reachable from Tick/Paint/SynchronizeProperties, or cache the MID in an initialization path."),
                            false);
                    }
                    else if (bIncludeAdvisory)
                    {
                        AddMaterialLifecycleFinding(
                            Findings,
                            GraphErrors,
                            GraphWarnings,
                            SuppressedRuleSet,
                            TEXT("warning"),
                            TEXT("DynamicMaterialCreationSiteReview"),
                            CurrentAssetPath,
                            Graph,
                            GraphContext.GraphType,
                            GraphContext.GraphLifecycle,
                            CallNode,
                            FunctionName,
                            OwnerClass,
                            TEXT("A dynamic material instance creation site exists and should be reviewed for caching/lifetime ownership."),
                            TEXT("Prefer a single cached MID owned by the widget instance; avoid recreating MIDs during user interaction or frame-driven updates."),
                            false);
                    }
                }

                AssetErrors += GraphErrors;
                AssetWarnings += GraphWarnings;
                TotalErrors += GraphErrors;
                TotalWarnings += GraphWarnings;

                TSharedPtr<FJsonObject> GraphOut = MakeShared<FJsonObject>();
                GraphOut->SetStringField(TEXT("graph"), Graph->GetName());
                GraphOut->SetStringField(TEXT("graph_type"), GraphContext.GraphType);
                GraphOut->SetStringField(TEXT("graph_lifecycle"), GraphContext.GraphLifecycle);
                GraphOut->SetBoolField(TEXT("graph_name_is_repeated_lifecycle"), GraphContext.bGraphNameIsRepeatedLifecycle);
                GraphOut->SetBoolField(TEXT("contains_repeated_lifecycle_event"), GraphContext.bGraphContainsRepeatedLifecycleEvent);
                GraphOut->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
                GraphOut->SetNumberField(TEXT("dynamic_material_creation_count"), GraphDmiCalls);
                GraphOut->SetNumberField(TEXT("error_count"), GraphErrors);
                GraphOut->SetNumberField(TEXT("warning_count"), GraphWarnings);
                GraphReports.Add(MakeShared<FJsonValueObject>(GraphOut));
            }

            AssetOut->SetBoolField(TEXT("load_success"), true);
            AssetOut->SetNumberField(TEXT("graphs_scanned"), Graphs.Num());
            AssetOut->SetNumberField(TEXT("nodes_scanned"), AssetNodeCount);
            AssetOut->SetNumberField(TEXT("dynamic_material_creation_count"), AssetDmiCalls);
            AssetOut->SetNumberField(TEXT("error_count"), AssetErrors);
            AssetOut->SetNumberField(TEXT("warning_count"), AssetWarnings);
            AssetOut->SetArrayField(TEXT("graphs"), GraphReports);
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetOut));
        }

        if (AssetPaths.Num() == 0)
        {
            ++TotalErrors;
            TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
            Finding->SetStringField(TEXT("severity"), TEXT("error"));
            Finding->SetStringField(TEXT("category"), TEXT("AssetInventory"));
            Finding->SetStringField(TEXT("rule_id"), TEXT("AssetInventory"));
            Finding->SetStringField(TEXT("asset_path"), PathPrefix);
            Finding->SetStringField(TEXT("message"), TEXT("No WidgetBlueprint assets were found for the requested path_prefix."));
            Finding->SetStringField(TEXT("suggested_fix"), TEXT("Pass explicit asset_path/asset_paths or verify the path_prefix points at WidgetBlueprint assets."));
            Findings.Add(MakeShared<FJsonValueObject>(Finding));
        }

        const bool bPassed = LoadErrors == 0
            && TotalErrors == 0
            && (!bTreatWarningsAsErrors || TotalWarnings == 0);

        TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("assets_scanned"), AssetPaths.Num());
        Summary->SetNumberField(TEXT("graphs_scanned"), TotalGraphs);
        Summary->SetNumberField(TEXT("nodes_scanned"), TotalNodes);
        Summary->SetNumberField(TEXT("dynamic_material_creation_count"), TotalDmiCreationCalls);
        Summary->SetNumberField(TEXT("error_count"), TotalErrors + LoadErrors);
        Summary->SetNumberField(TEXT("warning_count"), TotalWarnings);
        Summary->SetNumberField(TEXT("load_error_count"), LoadErrors);
        Summary->SetBoolField(TEXT("include_advisory"), bIncludeAdvisory);
        Summary->SetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);
        Summary->SetNumberField(TEXT("suppressed_rule_count"), SuppressedRuleSet.Num());

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("schema_version"), TEXT("ui_material_lifecycle_audit.v1"));
        Out->SetBoolField(TEXT("bSuccess"), bPassed);
        Out->SetBoolField(TEXT("ok"), bPassed);
        Out->SetBoolField(TEXT("mutates_assets"), false);
        Out->SetStringField(TEXT("status"), bPassed ? TEXT("ok") : TEXT("findings_failed"));
        Out->SetStringField(TEXT("path_prefix"), PathPrefix);
        Out->SetObjectField(TEXT("summary"), Summary);
        Out->SetArrayField(TEXT("assets"), AssetReports);
        Out->SetArrayField(TEXT("findings"), Findings);
        return FMonolithActionResult::Success(Out);
    }

    // ------------------------------------------------------------------
    // ui::measure_widget_layout handler
    //
    // Read-only layout evidence based on the canonical FUISpec dump. This
    // intentionally replaces weak external get_layout_data/check_widget_overlap
    // clones with a Monolith-owned result shape. v1 reports authored layout-model
    // bounds and marks render geometry as unavailable instead of pretending that
    // cached designer geometry is runtime proof.
    // ------------------------------------------------------------------

    struct FLayoutMeasureRect
    {
        double X = 0.0;
        double Y = 0.0;
        double W = 0.0;
        double H = 0.0;
        bool bValid = false;
    };

    struct FLayoutMeasureProfile
    {
        FString Name = TEXT("desktop");
        FVector2D Resolution = FVector2D(1920.0, 1080.0);
        double DpiScale = 1.0;
        bool bHasSafeZone = false;
        FMargin SafeZone;
        TSet<FString> VisibilityFilter;
    };

    struct FLayoutMeasuredWidget
    {
        FString WidgetName;
        FString WidgetPath;
        FString ParentPath;
        FString WidgetClass;
        FString Visibility;
        bool bOccupiesLayout = true;
        bool bHitTestVisible = true;
        bool bIncludedByVisibilityFilter = true;
        FLayoutMeasureRect LayoutBounds;
    };

    static TSharedPtr<FJsonObject> RectToJson(const FLayoutMeasureRect& Rect)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("x"), Rect.X);
        Out->SetNumberField(TEXT("y"), Rect.Y);
        Out->SetNumberField(TEXT("w"), Rect.W);
        Out->SetNumberField(TEXT("h"), Rect.H);
        Out->SetBoolField(TEXT("valid"), Rect.bValid);
        return Out;
    }

    static TSharedPtr<FJsonObject> MarginToJson(const FMargin& Margin)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("left"), Margin.Left);
        Out->SetNumberField(TEXT("top"), Margin.Top);
        Out->SetNumberField(TEXT("right"), Margin.Right);
        Out->SetNumberField(TEXT("bottom"), Margin.Bottom);
        return Out;
    }

    static double RectArea(const FLayoutMeasureRect& Rect)
    {
        return Rect.bValid ? FMath::Max(0.0, Rect.W) * FMath::Max(0.0, Rect.H) : 0.0;
    }

    static FLayoutMeasureRect IntersectRects(const FLayoutMeasureRect& A, const FLayoutMeasureRect& B)
    {
        FLayoutMeasureRect Out;
        if (!A.bValid || !B.bValid)
        {
            return Out;
        }

        const double X0 = FMath::Max(A.X, B.X);
        const double Y0 = FMath::Max(A.Y, B.Y);
        const double X1 = FMath::Min(A.X + A.W, B.X + B.W);
        const double Y1 = FMath::Min(A.Y + A.H, B.Y + B.H);
        if (X1 <= X0 || Y1 <= Y0)
        {
            return Out;
        }

        Out.X = X0;
        Out.Y = Y0;
        Out.W = X1 - X0;
        Out.H = Y1 - Y0;
        Out.bValid = true;
        return Out;
    }

    static FString VisibilityTokenForNode(const FUISpecNode& Node)
    {
        const FString Visibility = Node.Style.Visibility.ToString();
        return Visibility.IsEmpty() || Node.Style.Visibility.IsNone() ? FString(TEXT("Visible")) : Visibility;
    }

    static bool VisibilityOccupiesLayout(const FString& Visibility)
    {
        return !Visibility.Equals(TEXT("Collapsed"), ESearchCase::IgnoreCase);
    }

    static bool VisibilityIsHitTestVisible(const FString& Visibility)
    {
        return Visibility.Equals(TEXT("Visible"), ESearchCase::IgnoreCase);
    }

    static FVector2D DesiredSizeForNode(const FUISpecNode& Node, const FLayoutMeasureRect& ParentRect)
    {
        FVector2D Size(
            Node.Style.Width > 0.0f ? Node.Style.Width : Node.Slot.Size.X,
            Node.Style.Height > 0.0f ? Node.Style.Height : Node.Slot.Size.Y);

        if (Size.X <= 0.0)
        {
            Size.X = ParentRect.W > 0.0 ? ParentRect.W : 0.0;
        }
        if (Size.Y <= 0.0)
        {
            Size.Y = ParentRect.H > 0.0 ? ParentRect.H : 0.0;
        }

        if (Node.Style.bOverrideMinDesiredWidth)
        {
            Size.X = FMath::Max(Size.X, static_cast<double>(Node.Style.MinDesiredWidth));
        }
        if (Node.Style.bOverrideMinDesiredHeight)
        {
            Size.Y = FMath::Max(Size.Y, static_cast<double>(Node.Style.MinDesiredHeight));
        }
        if (Node.Style.bOverrideMaxDesiredWidth && Node.Style.MaxDesiredWidth > 0.0f)
        {
            Size.X = FMath::Min(Size.X, static_cast<double>(Node.Style.MaxDesiredWidth));
        }
        if (Node.Style.bOverrideMaxDesiredHeight && Node.Style.MaxDesiredHeight > 0.0f)
        {
            Size.Y = FMath::Min(Size.Y, static_cast<double>(Node.Style.MaxDesiredHeight));
        }
        return Size;
    }

    static FLayoutMeasureRect InsetRect(const FLayoutMeasureRect& Rect, const FMargin& Padding)
    {
        FLayoutMeasureRect Out = Rect;
        if (!Rect.bValid)
        {
            return Out;
        }

        Out.X += Padding.Left;
        Out.Y += Padding.Top;
        Out.W = FMath::Max(0.0, Out.W - Padding.Left - Padding.Right);
        Out.H = FMath::Max(0.0, Out.H - Padding.Top - Padding.Bottom);
        return Out;
    }

    static FLayoutMeasureRect ComputeCanvasChildRect(const FUISpecNode& Node, const FLayoutMeasureRect& ParentRect)
    {
        FLayoutMeasureRect Out;
        if (!ParentRect.bValid)
        {
            return Out;
        }

        const FString AnchorPreset = Node.Slot.AnchorPreset.IsNone()
            ? FString(TEXT("top_left"))
            : Node.Slot.AnchorPreset.ToString();
        const FAnchors Anchors = MonolithUI::GetAnchorPreset(AnchorPreset);
        const bool bStretchX = !FMath::IsNearlyEqual(Anchors.Minimum.X, Anchors.Maximum.X);
        const bool bStretchY = !FMath::IsNearlyEqual(Anchors.Minimum.Y, Anchors.Maximum.Y);
        const FVector2D DesiredSize = DesiredSizeForNode(Node, ParentRect);

        if (bStretchX)
        {
            Out.X = ParentRect.X + ParentRect.W * Anchors.Minimum.X + Node.Slot.Position.X;
            Out.W = ParentRect.W * (Anchors.Maximum.X - Anchors.Minimum.X) - Node.Slot.Position.X - Node.Slot.Size.X;
        }
        else
        {
            Out.W = DesiredSize.X;
            Out.X = ParentRect.X + ParentRect.W * Anchors.Minimum.X + Node.Slot.Position.X - Node.Slot.Alignment.X * Out.W;
        }

        if (bStretchY)
        {
            Out.Y = ParentRect.Y + ParentRect.H * Anchors.Minimum.Y + Node.Slot.Position.Y;
            Out.H = ParentRect.H * (Anchors.Maximum.Y - Anchors.Minimum.Y) - Node.Slot.Position.Y - Node.Slot.Size.Y;
        }
        else
        {
            Out.H = DesiredSize.Y;
            Out.Y = ParentRect.Y + ParentRect.H * Anchors.Minimum.Y + Node.Slot.Position.Y - Node.Slot.Alignment.Y * Out.H;
        }

        Out.W = FMath::Max(0.0, Out.W);
        Out.H = FMath::Max(0.0, Out.H);
        Out.bValid = true;
        return Out;
    }

    static FLayoutMeasureRect AlignRectWithinParent(
        const FUISpecNode& Node,
        const FLayoutMeasureRect& ParentRect,
        const FVector2D& DesiredSize)
    {
        FLayoutMeasureRect Out = InsetRect(ParentRect, Node.Slot.Padding);
        if (!Out.bValid)
        {
            return Out;
        }

        const FString HAlign = Node.Slot.HAlign.ToString();
        const FString VAlign = Node.Slot.VAlign.ToString();
        const bool bFillX = HAlign.IsEmpty() || HAlign.Equals(TEXT("Fill"), ESearchCase::IgnoreCase);
        const bool bFillY = VAlign.IsEmpty() || VAlign.Equals(TEXT("Fill"), ESearchCase::IgnoreCase);
        const double ChildW = bFillX ? Out.W : FMath::Min(Out.W, static_cast<double>(DesiredSize.X));
        const double ChildH = bFillY ? Out.H : FMath::Min(Out.H, static_cast<double>(DesiredSize.Y));

        if (HAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
        {
            Out.X += (Out.W - ChildW) * 0.5;
        }
        else if (HAlign.Equals(TEXT("Right"), ESearchCase::IgnoreCase))
        {
            Out.X += Out.W - ChildW;
        }

        if (VAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
        {
            Out.Y += (Out.H - ChildH) * 0.5;
        }
        else if (VAlign.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase))
        {
            Out.Y += Out.H - ChildH;
        }

        Out.W = ChildW;
        Out.H = ChildH;
        return Out;
    }

    static FLayoutMeasureRect ComputeChildRect(
        const FUISpecNode& Node,
        const FName& ParentType,
        const FLayoutMeasureRect& ParentRect,
        int32 ChildIndex,
        int32 ChildCount)
    {
        if (ParentType == FName(TEXT("CanvasPanel")))
        {
            return ComputeCanvasChildRect(Node, ParentRect);
        }

        const FVector2D DesiredSize = DesiredSizeForNode(Node, ParentRect);

        if (ParentType == FName(TEXT("VerticalBox")) || ParentType == FName(TEXT("ScrollBox")))
        {
            FLayoutMeasureRect Out = InsetRect(ParentRect, Node.Slot.Padding);
            const double Height = DesiredSize.Y > 0.0 && DesiredSize.Y < ParentRect.H
                ? DesiredSize.Y
                : (ChildCount > 0 ? ParentRect.H / static_cast<double>(ChildCount) : ParentRect.H);
            Out.Y = ParentRect.Y + Height * static_cast<double>(ChildIndex) + Node.Slot.Padding.Top;
            Out.H = FMath::Max(0.0, Height - Node.Slot.Padding.Top - Node.Slot.Padding.Bottom);
            return Out;
        }

        if (ParentType == FName(TEXT("HorizontalBox")))
        {
            FLayoutMeasureRect Out = InsetRect(ParentRect, Node.Slot.Padding);
            const double Width = DesiredSize.X > 0.0 && DesiredSize.X < ParentRect.W
                ? DesiredSize.X
                : (ChildCount > 0 ? ParentRect.W / static_cast<double>(ChildCount) : ParentRect.W);
            Out.X = ParentRect.X + Width * static_cast<double>(ChildIndex) + Node.Slot.Padding.Left;
            Out.W = FMath::Max(0.0, Width - Node.Slot.Padding.Left - Node.Slot.Padding.Right);
            return Out;
        }

        return AlignRectWithinParent(Node, ParentRect, DesiredSize);
    }

    static void MeasureWidgetNodeRecursive(
        const FUISpecNode& Node,
        const FString& WidgetPath,
        const FString& ParentPath,
        const FLayoutMeasureRect& LayoutRect,
        bool bParentCollapsed,
        const FLayoutMeasureProfile& Profile,
        TArray<FLayoutMeasuredWidget>& OutWidgets)
    {
        const FString Visibility = VisibilityTokenForNode(Node);
        const bool bCollapsed = bParentCollapsed || !VisibilityOccupiesLayout(Visibility);

        FLayoutMeasuredWidget Row;
        Row.WidgetName = Node.Id.ToString();
        Row.WidgetPath = WidgetPath;
        Row.ParentPath = ParentPath;
        Row.WidgetClass = Node.Type.ToString();
        Row.Visibility = Visibility;
        Row.bOccupiesLayout = !bCollapsed;
        Row.bHitTestVisible = !bCollapsed && VisibilityIsHitTestVisible(Visibility);
        Row.bIncludedByVisibilityFilter = Profile.VisibilityFilter.Num() == 0 || Profile.VisibilityFilter.Contains(Visibility);
        Row.LayoutBounds = LayoutRect;
        Row.LayoutBounds.bValid = LayoutRect.bValid && !bCollapsed;
        OutWidgets.Add(MoveTemp(Row));

        for (int32 Index = 0; Index < Node.Children.Num(); ++Index)
        {
            const TSharedPtr<FUISpecNode>& Child = Node.Children[Index];
            if (!Child.IsValid())
            {
                continue;
            }

            const FString ChildPath = WidgetPath.IsEmpty()
                ? Child->Id.ToString()
                : FString::Printf(TEXT("%s/%s"), *WidgetPath, *Child->Id.ToString());
            const FLayoutMeasureRect ChildRect = ComputeChildRect(*Child, Node.Type, LayoutRect, Index, Node.Children.Num());
            MeasureWidgetNodeRecursive(*Child, ChildPath, WidgetPath, ChildRect, bCollapsed, Profile, OutWidgets);
        }
    }

    static bool TryReadResolutionArray(
        const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* FieldName,
        FVector2D& OutResolution,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, Values) || !Values)
        {
            return true;
        }
        if (Values->Num() != 2)
        {
            OutError = FString::Printf(TEXT("`%s` must be [width, height]."), FieldName);
            return false;
        }

        double W = 0.0;
        double H = 0.0;
        if (!(*Values)[0].IsValid() || !(*Values)[0]->TryGetNumber(W)
            || !(*Values)[1].IsValid() || !(*Values)[1]->TryGetNumber(H)
            || W <= 0.0 || H <= 0.0)
        {
            OutError = FString::Printf(TEXT("`%s` must contain positive numeric width and height."), FieldName);
            return false;
        }

        OutResolution = FVector2D(W, H);
        return true;
    }

    static bool TryReadSafeZone(
        const TSharedPtr<FJsonObject>& Obj,
        FLayoutMeasureProfile& Profile,
        FString& OutError)
    {
        const TSharedPtr<FJsonObject>* SafeZoneObj = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetObjectField(TEXT("safe_zone"), SafeZoneObj) || !SafeZoneObj || !SafeZoneObj->IsValid())
        {
            return true;
        }

        double Left = 0.0;
        double Top = 0.0;
        double Right = 0.0;
        double Bottom = 0.0;
        (*SafeZoneObj)->TryGetNumberField(TEXT("left"), Left);
        (*SafeZoneObj)->TryGetNumberField(TEXT("top"), Top);
        (*SafeZoneObj)->TryGetNumberField(TEXT("right"), Right);
        (*SafeZoneObj)->TryGetNumberField(TEXT("bottom"), Bottom);
        if (Left < 0.0 || Top < 0.0 || Right < 0.0 || Bottom < 0.0)
        {
            OutError = TEXT("safe_zone margins must be non-negative.");
            return false;
        }

        Profile.SafeZone = FMargin(Left, Top, Right, Bottom);
        Profile.bHasSafeZone = true;
        return true;
    }

    static void SetDefaultVisibilityFilter(FLayoutMeasureProfile& Profile)
    {
        Profile.VisibilityFilter.Add(TEXT("Visible"));
        Profile.VisibilityFilter.Add(TEXT("HitTestInvisible"));
        Profile.VisibilityFilter.Add(TEXT("SelfHitTestInvisible"));
    }

    static bool TryReadVisibilityFilter(
        const TSharedPtr<FJsonObject>& Obj,
        FLayoutMeasureProfile& Profile,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetArrayField(TEXT("visibility_filter"), Values) || !Values)
        {
            SetDefaultVisibilityFilter(Profile);
            return true;
        }

        for (int32 Index = 0; Index < Values->Num(); ++Index)
        {
            FString Visibility;
            if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetString(Visibility) || Visibility.IsEmpty())
            {
                OutError = FString::Printf(TEXT("visibility_filter[%d] must be a non-empty string."), Index);
                return false;
            }
            Profile.VisibilityFilter.Add(Visibility);
        }
        return true;
    }

    static bool TryReadLayoutProfiles(const TSharedPtr<FJsonObject>& Params, TArray<FLayoutMeasureProfile>& OutProfiles, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("profiles"), Values) || !Values || Values->Num() == 0)
        {
            FLayoutMeasureProfile DefaultProfile;
            SetDefaultVisibilityFilter(DefaultProfile);
            OutProfiles.Add(DefaultProfile);
            return true;
        }

        for (int32 Index = 0; Index < Values->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject> ProfileObj = (*Values)[Index].IsValid() ? (*Values)[Index]->AsObject() : nullptr;
            if (!ProfileObj.IsValid())
            {
                OutError = FString::Printf(TEXT("profiles[%d] must be an object."), Index);
                return false;
            }

            FLayoutMeasureProfile Profile;
            Profile.Name = FString::Printf(TEXT("profile_%d"), Index);
            ProfileObj->TryGetStringField(TEXT("name"), Profile.Name);
            if (!TryReadResolutionArray(ProfileObj, TEXT("resolution"), Profile.Resolution, OutError)
                || !TryReadSafeZone(ProfileObj, Profile, OutError)
                || !TryReadVisibilityFilter(ProfileObj, Profile, OutError))
            {
                return false;
            }

            double DpiScale = 1.0;
            if (ProfileObj->TryGetNumberField(TEXT("dpi_scale"), DpiScale) && DpiScale > 0.0)
            {
                Profile.DpiScale = DpiScale;
            }
            OutProfiles.Add(MoveTemp(Profile));
        }

        return true;
    }

    static TSharedPtr<FJsonObject> MeasuredWidgetToJson(const FLayoutMeasuredWidget& Widget)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("widget_name"), Widget.WidgetName);
        Out->SetStringField(TEXT("widget_path"), Widget.WidgetPath);
        Out->SetStringField(TEXT("parent_path"), Widget.ParentPath);
        Out->SetStringField(TEXT("class"), Widget.WidgetClass);
        Out->SetStringField(TEXT("visibility"), Widget.Visibility);
        Out->SetBoolField(TEXT("occupies_layout"), Widget.bOccupiesLayout);
        Out->SetBoolField(TEXT("hit_test_visible"), Widget.bHitTestVisible);
        Out->SetBoolField(TEXT("included_by_visibility_filter"), Widget.bIncludedByVisibilityFilter);
        Out->SetObjectField(TEXT("layout_bounds"), RectToJson(Widget.LayoutBounds));
        Out->SetObjectField(TEXT("render_bounds"), RectToJson(Widget.LayoutBounds));
        Out->SetBoolField(TEXT("render_bounds_available"), false);
        Out->SetNumberField(TEXT("layout_render_divergence"), 0.0);
        return Out;
    }

    static void AddOverlapFindings(
        const TArray<FLayoutMeasuredWidget>& Widgets,
        double MaxAllowedOverlapRatio,
        TArray<TSharedPtr<FJsonValue>>& OutOverlaps)
    {
        for (int32 AIndex = 0; AIndex < Widgets.Num(); ++AIndex)
        {
            const FLayoutMeasuredWidget& A = Widgets[AIndex];
            if (!A.bOccupiesLayout || !A.bIncludedByVisibilityFilter || !A.LayoutBounds.bValid)
            {
                continue;
            }

            for (int32 BIndex = AIndex + 1; BIndex < Widgets.Num(); ++BIndex)
            {
                const FLayoutMeasuredWidget& B = Widgets[BIndex];
                if (!B.bOccupiesLayout || !B.bIncludedByVisibilityFilter || !B.LayoutBounds.bValid)
                {
                    continue;
                }
                if (A.ParentPath != B.ParentPath)
                {
                    continue;
                }

                const FLayoutMeasureRect Intersection = IntersectRects(A.LayoutBounds, B.LayoutBounds);
                const double IntersectionArea = RectArea(Intersection);
                if (IntersectionArea <= 0.0)
                {
                    continue;
                }

                const double SmallerArea = FMath::Min(RectArea(A.LayoutBounds), RectArea(B.LayoutBounds));
                const double Ratio = SmallerArea > 0.0 ? IntersectionArea / SmallerArea : 0.0;
                if (Ratio <= MaxAllowedOverlapRatio)
                {
                    continue;
                }

                TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
                Finding->SetStringField(TEXT("severity"), TEXT("error"));
                Finding->SetStringField(TEXT("category"), TEXT("WidgetOverlap"));
                Finding->SetStringField(TEXT("widget_a"), A.WidgetName);
                Finding->SetStringField(TEXT("widget_b"), B.WidgetName);
                Finding->SetStringField(TEXT("widget_path_a"), A.WidgetPath);
                Finding->SetStringField(TEXT("widget_path_b"), B.WidgetPath);
                Finding->SetObjectField(TEXT("intersection"), RectToJson(Intersection));
                Finding->SetNumberField(TEXT("overlap_area"), IntersectionArea);
                Finding->SetNumberField(TEXT("overlap_ratio_of_smaller"), Ratio);
                Finding->SetStringField(TEXT("message"), TEXT("Sibling widgets overlap in the authored layout model for this profile."));
                Finding->SetStringField(TEXT("suggested_fix"), TEXT("Adjust slot position/size/anchors or route the widgets through an intentional overlay container with visual proof."));
                OutOverlaps.Add(MakeShared<FJsonValueObject>(Finding));
            }
        }
    }

    static void AddSafeZoneFindings(
        const TArray<FLayoutMeasuredWidget>& Widgets,
        const FLayoutMeasureProfile& Profile,
        TArray<TSharedPtr<FJsonValue>>& OutViolations)
    {
        if (!Profile.bHasSafeZone)
        {
            return;
        }

        const FLayoutMeasureRect SafeRect{
            Profile.SafeZone.Left,
            Profile.SafeZone.Top,
            FMath::Max(0.0, static_cast<double>(Profile.Resolution.X) - Profile.SafeZone.Left - Profile.SafeZone.Right),
            FMath::Max(0.0, static_cast<double>(Profile.Resolution.Y) - Profile.SafeZone.Top - Profile.SafeZone.Bottom),
            true
        };

        for (const FLayoutMeasuredWidget& Widget : Widgets)
        {
            if (!Widget.bOccupiesLayout || !Widget.bIncludedByVisibilityFilter || !Widget.LayoutBounds.bValid)
            {
                continue;
            }
            if (Widget.ParentPath.IsEmpty())
            {
                continue;
            }

            const double LeftDelta = SafeRect.X - Widget.LayoutBounds.X;
            const double TopDelta = SafeRect.Y - Widget.LayoutBounds.Y;
            const double RightDelta = (Widget.LayoutBounds.X + Widget.LayoutBounds.W) - (SafeRect.X + SafeRect.W);
            const double BottomDelta = (Widget.LayoutBounds.Y + Widget.LayoutBounds.H) - (SafeRect.Y + SafeRect.H);
            if (LeftDelta <= 0.0 && TopDelta <= 0.0 && RightDelta <= 0.0 && BottomDelta <= 0.0)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
            Finding->SetStringField(TEXT("severity"), TEXT("error"));
            Finding->SetStringField(TEXT("category"), TEXT("SafeZoneViolation"));
            Finding->SetStringField(TEXT("widget_name"), Widget.WidgetName);
            Finding->SetStringField(TEXT("widget_path"), Widget.WidgetPath);
            Finding->SetObjectField(TEXT("layout_bounds"), RectToJson(Widget.LayoutBounds));
            Finding->SetObjectField(TEXT("safe_zone_rect"), RectToJson(SafeRect));
            Finding->SetNumberField(TEXT("left_violation"), FMath::Max(0.0, LeftDelta));
            Finding->SetNumberField(TEXT("top_violation"), FMath::Max(0.0, TopDelta));
            Finding->SetNumberField(TEXT("right_violation"), FMath::Max(0.0, RightDelta));
            Finding->SetNumberField(TEXT("bottom_violation"), FMath::Max(0.0, BottomDelta));
            Finding->SetStringField(TEXT("message"), TEXT("Widget bounds extend outside the explicit safe-zone rectangle for this profile."));
            Finding->SetStringField(TEXT("suggested_fix"), TEXT("Wrap the edge UI in SafeZone or move the anchored group inside the requested safe-zone margins."));
            OutViolations.Add(MakeShared<FJsonValueObject>(Finding));
        }
    }

    static FMonolithActionResult HandleMeasureWidgetLayout(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString AssetPath;
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
        }

        TArray<FLayoutMeasureProfile> Profiles;
        FString ParamError;
        if (!TryReadLayoutProfiles(Params, Profiles, ParamError))
        {
            return FMonolithActionResult::Error(ParamError, -32602);
        }

        bool bCheckOverlap = true;
        bool bCheckSafeZone = true;
        Params->TryGetBoolField(TEXT("check_overlap"), bCheckOverlap);
        Params->TryGetBoolField(TEXT("check_safe_zone"), bCheckSafeZone);
        double MaxAllowedOverlapRatio = 0.0;
        Params->TryGetNumberField(TEXT("max_allowed_overlap_ratio"), MaxAllowedOverlapRatio);
        MaxAllowedOverlapRatio = FMath::Clamp(MaxAllowedOverlapRatio, 0.0, 1.0);

        FUISpecSerializerInputs DumpInputs;
        DumpInputs.AssetPath = AssetPath;
        const FUISpecSerializerResult DumpResult = FUISpecSerializer::Dump(DumpInputs);

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("schema_version"), TEXT("ui_layout_measure.v1"));
        Out->SetStringField(TEXT("asset_path"), AssetPath);
        Out->SetBoolField(TEXT("mutates_assets"), false);
        Out->SetStringField(TEXT("measurement_model"), TEXT("authored_spec_layout_model"));
        Out->SetBoolField(TEXT("render_geometry_proof"), false);

        if (!DumpResult.bSuccess || !DumpResult.Document.Root.IsValid())
        {
            TArray<TSharedPtr<FJsonValue>> Errors;
            for (const FUISpecError& Error : DumpResult.Errors)
            {
                TSharedPtr<FJsonObject> ErrorOut = MakeShared<FJsonObject>();
                ErrorOut->SetStringField(TEXT("category"), Error.Category.ToString());
                ErrorOut->SetStringField(TEXT("message"), Error.Message);
                Errors.Add(MakeShared<FJsonValueObject>(ErrorOut));
            }
            Out->SetBoolField(TEXT("bSuccess"), false);
            Out->SetBoolField(TEXT("ok"), false);
            Out->SetStringField(TEXT("status"), TEXT("dump_failed"));
            Out->SetArrayField(TEXT("errors"), Errors);
            return FMonolithActionResult::Success(Out);
        }

        TArray<TSharedPtr<FJsonValue>> ProfileRows;
        int32 TotalOverlapCount = 0;
        int32 TotalSafeZoneViolationCount = 0;

        for (const FLayoutMeasureProfile& Profile : Profiles)
        {
            FLayoutMeasureRect RootRect;
            RootRect.X = 0.0;
            RootRect.Y = 0.0;
            RootRect.W = Profile.Resolution.X;
            RootRect.H = Profile.Resolution.Y;
            RootRect.bValid = true;

            TArray<FLayoutMeasuredWidget> Widgets;
            MeasureWidgetNodeRecursive(
                *DumpResult.Document.Root,
                DumpResult.Document.Root->Id.ToString(),
                FString(),
                RootRect,
                false,
                Profile,
                Widgets);

            TArray<TSharedPtr<FJsonValue>> WidgetRows;
            WidgetRows.Reserve(Widgets.Num());
            for (const FLayoutMeasuredWidget& Widget : Widgets)
            {
                WidgetRows.Add(MakeShared<FJsonValueObject>(MeasuredWidgetToJson(Widget)));
            }

            TArray<TSharedPtr<FJsonValue>> Overlaps;
            if (bCheckOverlap)
            {
                AddOverlapFindings(Widgets, MaxAllowedOverlapRatio, Overlaps);
            }

            TArray<TSharedPtr<FJsonValue>> SafeZoneViolations;
            if (bCheckSafeZone)
            {
                AddSafeZoneFindings(Widgets, Profile, SafeZoneViolations);
            }

            TotalOverlapCount += Overlaps.Num();
            TotalSafeZoneViolationCount += SafeZoneViolations.Num();

            TSharedPtr<FJsonObject> ProfileOut = MakeShared<FJsonObject>();
            ProfileOut->SetStringField(TEXT("name"), Profile.Name);
            ProfileOut->SetObjectField(TEXT("resolution"), Vec2ToJson(Profile.Resolution));
            ProfileOut->SetNumberField(TEXT("dpi_scale"), Profile.DpiScale);
            ProfileOut->SetStringField(TEXT("measurement_source"), TEXT("authored_spec_layout_model"));
            ProfileOut->SetBoolField(TEXT("render_bounds_available"), false);
            ProfileOut->SetStringField(TEXT("render_bounds_unavailable_reason"), TEXT("v1 does not own a virtual-window render/prepass geometry walk; compose editor.capture_scene_preview + ui.verify_widget_visual_artifacts for visual proof."));
            ProfileOut->SetBoolField(TEXT("check_overlap"), bCheckOverlap);
            ProfileOut->SetBoolField(TEXT("check_safe_zone"), bCheckSafeZone);
            ProfileOut->SetObjectField(TEXT("safe_zone"), Profile.bHasSafeZone ? MarginToJson(Profile.SafeZone) : MakeShared<FJsonObject>());
            ProfileOut->SetArrayField(TEXT("widgets"), WidgetRows);
            ProfileOut->SetArrayField(TEXT("overlaps"), Overlaps);
            ProfileOut->SetArrayField(TEXT("safe_zone_violations"), SafeZoneViolations);
            ProfileOut->SetStringField(TEXT("status"), (Overlaps.Num() == 0 && SafeZoneViolations.Num() == 0) ? TEXT("pass") : TEXT("findings_failed"));
            ProfileRows.Add(MakeShared<FJsonValueObject>(ProfileOut));
        }

        TArray<TSharedPtr<FJsonValue>> Checks;
        TSharedPtr<FJsonObject> ModelCheck = MakeShared<FJsonObject>();
        ModelCheck->SetStringField(TEXT("id"), TEXT("LayoutModelSource"));
        ModelCheck->SetStringField(TEXT("status"), TEXT("partial"));
        ModelCheck->SetStringField(TEXT("message"), TEXT("Bounds are derived from Monolith's canonical FUISpec slot/style serializer. This is deterministic authoring evidence, not cached Slate runtime geometry."));
        Checks.Add(MakeShared<FJsonValueObject>(ModelCheck));

        TSharedPtr<FJsonObject> RenderCheck = MakeShared<FJsonObject>();
        RenderCheck->SetStringField(TEXT("id"), TEXT("RenderGeometry"));
        RenderCheck->SetStringField(TEXT("status"), TEXT("unavailable"));
        RenderCheck->SetStringField(TEXT("message"), TEXT("Render bounds and render-transform divergence require a future owned virtual-window measurement pass. No cached designer geometry is used."));
        Checks.Add(MakeShared<FJsonValueObject>(RenderCheck));

        const bool bPassed = TotalOverlapCount == 0 && TotalSafeZoneViolationCount == 0;
        TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("profile_count"), Profiles.Num());
        Summary->SetNumberField(TEXT("widgets_measured_per_profile"), DumpResult.NodesVisited);
        Summary->SetNumberField(TEXT("overlap_count"), TotalOverlapCount);
        Summary->SetNumberField(TEXT("safe_zone_violation_count"), TotalSafeZoneViolationCount);

        Out->SetBoolField(TEXT("bSuccess"), bPassed);
        Out->SetBoolField(TEXT("ok"), bPassed);
        Out->SetStringField(TEXT("status"), bPassed ? TEXT("pass") : TEXT("findings_failed"));
        Out->SetObjectField(TEXT("summary"), Summary);
        Out->SetArrayField(TEXT("profiles"), ProfileRows);
        Out->SetArrayField(TEXT("checks"), Checks);
        return FMonolithActionResult::Success(Out);
    }

    // ------------------------------------------------------------------
    // Phase 3 Item #18 (2026-05-16 UI Gap Audit) — ui::build_menu_from_spec
    //
    // Conservative MVP scope: validator + per-screen embedded spec dispatch are
    // implemented, while cross-screen aggregation and kind-only scaffolder
    // dispatch return non-mutating status. The action accepts a menu-shape
    // document of:
    //
    //     {
    //       "layers":       [{ "id": "...", "screens": ["..."] }, ...],
    //       "screens":      [{ "id": "...", "asset_path": "/Game/UI/...",
    //                          "spec": <FUISpecDocument>?, "kind": "main_menu"|"settings"|... }, ...],
    //       "focus_table":  [{ "screen": "...", "target": "..." }, ...],
    //       "nav_overrides": [{ "screen": "...", "widget": "...",
    //                           "direction": "Up", "target": "..." }, ...]
    //     }
    //
    // For each screen that supplies an embedded `spec`, the MVP dispatches it
    // through the existing FUISpecBuilder pipeline (one call per screen). The
    // focus_table / nav_overrides / layer-aggregation surface is captured in
    // the response under explicit non-mutating status so callers cannot treat
    // partial coverage as a complete committed edit. Modes
    // (`dry_run`, `treat_warnings_as_errors`, `raw_mode`, `overwrite`) are
    // forwarded onto every per-screen build call so the menu-level mode flag
    // propagates uniformly.
    //
    // Full implementation (deferred to a follow-up issue): build pre-walker
    // that emits the activatable-stack layer hierarchy first, threading
    // focus_table writes into the post-compile CDO pass on each screen WBP,
    // and applying nav_overrides via SetNavigationRuleExplicit.

    static FMonolithActionResult HandleBuildMenuFromSpec(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString RequestId;
        Params->TryGetStringField(TEXT("request_id"), RequestId);

        bool bDryRun = false, bTreatWarningsAsErrors = false, bRawMode = false, bOverwrite = true;
        Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
        Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);
        Params->TryGetBoolField(TEXT("raw_mode"), bRawMode);
        Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

        // ---- Validator (Phase 3 Item #18 MVP clause) -------------------------
        // The full FUISpecValidator extension lives in UISpecValidator.cpp;
        // the MVP wires the menu-shape structural checks inline here so the
        // action surface is unblocked without dragging FUISpecValidator into
        // a partial refactor.
        TArray<TSharedPtr<FJsonValue>> StructuralErrors;
        TArray<TSharedPtr<FJsonValue>> StructuralWarnings;

        auto AddError = [&StructuralErrors](const FString& Category, const FString& JsonPath, const FString& Message)
        {
            TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("category"), Category);
            E->SetStringField(TEXT("json_path"), JsonPath);
            E->SetStringField(TEXT("message"), Message);
            StructuralErrors.Add(MakeShared<FJsonValueObject>(E));
        };

        auto AddWarning = [&StructuralWarnings](const FString& Category, const FString& JsonPath, const FString& Message)
        {
            TSharedPtr<FJsonObject> W = MakeShared<FJsonObject>();
            W->SetStringField(TEXT("category"), Category);
            W->SetStringField(TEXT("json_path"), JsonPath);
            W->SetStringField(TEXT("message"), Message);
            StructuralWarnings.Add(MakeShared<FJsonValueObject>(W));
        };

        // screens[] is the load-bearing array. layers[]/focus_table[]/nav_overrides[]
        // are partially-supported in the MVP — caller-supplied entries are echoed
        // back so downstream tooling can surface "expected vs delivered".
        const TArray<TSharedPtr<FJsonValue>>* Screens = nullptr;
        if (!Params->TryGetArrayField(TEXT("screens"), Screens) || !Screens || Screens->Num() == 0)
        {
            AddError(TEXT("MenuShape"), TEXT("screens"),
                TEXT("`screens` array is required and must contain at least one entry. "
                     "Each entry needs {id, asset_path} and either an embedded `spec` "
                     "(FUISpecDocument) or a `kind` token for scaffolder dispatch (kind dispatch "
                     "is not implemented in this action)."));
        }

        const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
        const bool bHasLayers = Params->TryGetArrayField(TEXT("layers"), Layers) && Layers && Layers->Num() > 0;

        const TArray<TSharedPtr<FJsonValue>>* FocusTable = nullptr;
        const bool bHasFocusTable = Params->TryGetArrayField(TEXT("focus_table"), FocusTable) && FocusTable;

        const TArray<TSharedPtr<FJsonValue>>* NavOverrides = nullptr;
        const bool bHasNavOverrides = Params->TryGetArrayField(TEXT("nav_overrides"), NavOverrides) && NavOverrides;

        if (bHasLayers || bHasFocusTable || bHasNavOverrides)
        {
            AddWarning(TEXT("MenuShape"), TEXT("layers|focus_table|nav_overrides"),
                TEXT("layers / focus_table / nav_overrides are accepted but not applied by this action. "
                     "Per-screen `spec` builds run FULL via FUISpecBuilder. The cross-screen "
                     "aggregation surface (activatable-stack layer hierarchy, focus-table CDO writes, "
                     "nav-override propagation) is deferred to issue #3-18b. Caller-supplied entries "
                     "echo back in the response under `deferred_aggregation`."));
        }

        // Hard-fail on structural errors. The result payload mirrors
        // PackResponse so consumers can dispatch on bSuccess uniformly.
        if (StructuralErrors.Num() > 0)
        {
            TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("bSuccess"), false);
            if (!RequestId.IsEmpty()) Out->SetStringField(TEXT("request_id"), RequestId);
            Out->SetArrayField(TEXT("errors"), StructuralErrors);
            Out->SetArrayField(TEXT("warnings"), StructuralWarnings);
            Out->SetStringField(TEXT("status"), TEXT("validation_failed"));
            return FMonolithActionResult::Success(Out);
        }

        // ---- Per-screen dispatch into FUISpecBuilder -------------------------
        TArray<TSharedPtr<FJsonValue>> ScreenResults;
        ScreenResults.Reserve(Screens->Num());
        int32 TotalCreated = 0, TotalModified = 0, TotalRemoved = 0;
        bool bAllSucceeded = true;

        for (int32 i = 0; i < Screens->Num(); ++i)
        {
            const TSharedPtr<FJsonValue>& V = (*Screens)[i];
            const TSharedPtr<FJsonObject>* ScreenObj = nullptr;
            if (!V.IsValid() || !V->TryGetObject(ScreenObj) || !ScreenObj)
            {
                AddError(TEXT("MenuShape"),
                    FString::Printf(TEXT("screens[%d]"), i),
                    TEXT("screen entry must be an object"));
                bAllSucceeded = false;
                continue;
            }

            FString ScreenId, ScreenAssetPath, ScreenKind;
            (*ScreenObj)->TryGetStringField(TEXT("id"), ScreenId);
            (*ScreenObj)->TryGetStringField(TEXT("asset_path"), ScreenAssetPath);
            (*ScreenObj)->TryGetStringField(TEXT("kind"), ScreenKind);

            if (ScreenAssetPath.IsEmpty())
            {
                AddError(TEXT("MenuShape"),
                    FString::Printf(TEXT("screens[%d].asset_path"), i),
                    TEXT("each screen entry requires `asset_path`"));
                bAllSucceeded = false;
                continue;
            }

            // Per-screen result block — populated below.
            TSharedPtr<FJsonObject> ScreenOut = MakeShared<FJsonObject>();
            ScreenOut->SetStringField(TEXT("id"), ScreenId);
            ScreenOut->SetStringField(TEXT("asset_path"), ScreenAssetPath);
            if (!ScreenKind.IsEmpty())
            {
                ScreenOut->SetStringField(TEXT("kind"), ScreenKind);
            }

            const TSharedPtr<FJsonObject>* EmbeddedSpec = nullptr;
            if (!(*ScreenObj)->TryGetObjectField(TEXT("spec"), EmbeddedSpec) || !EmbeddedSpec)
            {
                // Kind-only dispatch is not implemented here. Surface it loudly so
                // the caller knows the per-screen WBP is NOT being built.
                ScreenOut->SetBoolField(TEXT("bSuccess"), false);
                ScreenOut->SetBoolField(TEXT("bCommitted"), false);
                ScreenOut->SetStringField(TEXT("status"), TEXT("not_implemented"));
                ScreenOut->SetStringField(TEXT("reason"),
                    TEXT("screen has no embedded `spec` — kind-based scaffolder dispatch is not implemented in build_menu_from_spec. "
                         "Pass a full FUISpecDocument under screens[N].spec to build this screen now, or call "
                         "scaffold_main_menu / scaffold_settings_panel_with_tabs / scaffold_pause_menu directly."));
                ScreenResults.Add(MakeShared<FJsonValueObject>(ScreenOut));
                bAllSucceeded = false;
                continue;
            }

            FUISpecDocument Document;
            FUISpecValidationResult ParseValidation;
            if (!ParseDocument(*EmbeddedSpec, Document, ParseValidation))
            {
                ScreenOut->SetBoolField(TEXT("bSuccess"), false);
                ScreenOut->SetStringField(TEXT("status"), TEXT("parse_failed"));
                ScreenOut->SetStringField(TEXT("llm_report"), ParseValidation.ToLLMReport());
                ScreenResults.Add(MakeShared<FJsonValueObject>(ScreenOut));
                bAllSucceeded = false;
                continue;
            }

            FUISpecBuilderInputs In;
            In.Document  = &Document;
            In.AssetPath = ScreenAssetPath;
            In.bOverwrite             = bOverwrite;
            In.bDryRun                = bDryRun;
            In.bTreatWarningsAsErrors = bTreatWarningsAsErrors;
            In.bRawMode               = bRawMode;
            In.RequestId              = FString::Printf(TEXT("%s:%s"), *RequestId, *ScreenId);
            if (Document.bTreatWarningsAsErrors)
            {
                In.bTreatWarningsAsErrors = true;
            }

            const FUISpecBuilderResult R = FUISpecBuilder::Build(In);
            TotalCreated  += R.NodesCreated;
            TotalModified += R.NodesModified;
            TotalRemoved  += R.NodesRemoved;
            if (!R.bSuccess) bAllSucceeded = false;

            // Each screen reuses the shared PackResponse shape for symmetry
            // with build_ui_from_spec callers.
            TSharedPtr<FJsonObject> Packed = PackResponse(R, bDryRun);
            ScreenOut->SetObjectField(TEXT("build_result"), Packed);
            ScreenResults.Add(MakeShared<FJsonValueObject>(ScreenOut));
        }

        // ---- Deferred aggregation echo -------------------------------------
        // Capture caller-supplied layers / focus_table / nav_overrides so
        // downstream tooling can post-process them in user-space until the
        // full builder pipeline lands.
        TSharedPtr<FJsonObject> DeferredAgg = MakeShared<FJsonObject>();
        if (bHasLayers)        DeferredAgg->SetArrayField(TEXT("layers"),        *Layers);
        if (bHasFocusTable)    DeferredAgg->SetArrayField(TEXT("focus_table"),   *FocusTable);
        if (bHasNavOverrides)  DeferredAgg->SetArrayField(TEXT("nav_overrides"), *NavOverrides);

        // ---- Response ------------------------------------------------------
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        const bool bHasDeferredAggregation = bHasLayers || bHasFocusTable || bHasNavOverrides;
        const bool bCommittedAllRequestedWork = bAllSucceeded && StructuralErrors.Num() == 0 && !bHasDeferredAggregation;
        Out->SetBoolField(TEXT("bSuccess"), bCommittedAllRequestedWork);
        Out->SetBoolField(TEXT("bCommittedAllRequestedWork"), bCommittedAllRequestedWork);
        if (!RequestId.IsEmpty()) Out->SetStringField(TEXT("request_id"), RequestId);
        Out->SetStringField(TEXT("status"),
            bCommittedAllRequestedWork ? TEXT("ok")
            : (bHasDeferredAggregation ? TEXT("partial_non_mutating") : TEXT("incomplete_non_mutating")));
        Out->SetArrayField(TEXT("screens"), ScreenResults);

        TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
        Counts->SetNumberField(TEXT("created"),  TotalCreated);
        Counts->SetNumberField(TEXT("modified"), TotalModified);
        Counts->SetNumberField(TEXT("removed"),  TotalRemoved);
        Out->SetObjectField(TEXT("aggregate_node_counts"), Counts);

        if (StructuralErrors.Num() > 0)  Out->SetArrayField(TEXT("errors"),   StructuralErrors);
        if (StructuralWarnings.Num() > 0) Out->SetArrayField(TEXT("warnings"), StructuralWarnings);
        if (DeferredAgg->Values.Num() > 0)
        {
            Out->SetObjectField(TEXT("deferred_aggregation"), DeferredAgg);
        }
        return FMonolithActionResult::Success(Out);
    }

    // ------------------------------------------------------------------
    // ui::apply_common_menu_transform_spec
    //
    // Applies the menu-level aggregation work that build_menu_from_spec
    // intentionally left as deferred: layers, focus targets, navigation, plus
    // post-copy repair steps for copied Lyra/CommonUI menu assets. The handler
    // composes existing, narrower actions instead of reimplementing their asset
    // mutation logic here.

    enum class EMenuTransformDryRunMode : uint8
    {
        PlanOnly,
        ExecuteChildDryRun,
        ExecuteReadOnly
    };

    struct FMenuTransformStep
    {
        FString Type;
        FString Namespace;
        FString Action;
        int32 SourceIndex = INDEX_NONE;
        bool bMutating = true;
        EMenuTransformDryRunMode DryRunMode = EMenuTransformDryRunMode::PlanOnly;
        TSharedPtr<FJsonObject> Params;
    };

    static TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Source)
    {
        TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
        if (Source.IsValid())
        {
            Copy->Values = Source->Values;
        }
        return Copy;
    }

    static void NormalizeObjectFieldToSingletonArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        if (!Object.IsValid() || !Object->HasField(FieldName))
        {
            return;
        }

        const TSharedPtr<FJsonObject>* FieldObject = nullptr;
        if (!Object->TryGetObjectField(FieldName, FieldObject) || !FieldObject || !FieldObject->IsValid())
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Add(MakeShared<FJsonValueObject>(*FieldObject));
        Object->SetArrayField(FieldName, Values);
    }

    static void NormalizeCommonMenuTransformSpec(const TSharedPtr<FJsonObject>& Spec)
    {
        NormalizeObjectFieldToSingletonArray(Spec, TEXT("screens"));
    }

    static bool ResolveCommonMenuSpecObject(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutSpec, FString& OutError)
    {
        if (!Params.IsValid())
        {
            OutError = TEXT("apply_common_menu_transform_spec requires an object payload.");
            return false;
        }

        if (!Params->HasField(TEXT("spec")))
        {
            OutSpec = CloneJsonObject(Params);
            NormalizeCommonMenuTransformSpec(OutSpec);
            return true;
        }

        const TSharedPtr<FJsonObject>* SpecObject = nullptr;
        if (!Params->TryGetObjectField(TEXT("spec"), SpecObject) || !SpecObject || !SpecObject->IsValid())
        {
            OutError = TEXT("spec must be an object when provided.");
            return false;
        }
        OutSpec = CloneJsonObject(*SpecObject);
        NormalizeCommonMenuTransformSpec(OutSpec);
        return true;
    }

    static bool GetOptionalBoolFromRootOrSpec(
        const TSharedPtr<FJsonObject>& Params,
        const TSharedPtr<FJsonObject>& Spec,
        const FString& FieldName,
        bool& OutValue,
        FString& OutError,
        bool DefaultValue)
    {
        const TSharedPtr<FJsonObject>& Source = Params.IsValid() && Params->HasField(FieldName) ? Params : Spec;
        OutValue = DefaultValue;
        if (!Source.IsValid() || !Source->HasField(FieldName))
        {
            return true;
        }
        if (!Source->TryGetBoolField(FieldName, OutValue))
        {
            OutError = FString::Printf(TEXT("%s must be a boolean."), *FieldName);
            return false;
        }
        return true;
    }

    static FString GetFirstStringField(
        const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* FieldA,
        const TCHAR* FieldB = nullptr,
        const TCHAR* FieldC = nullptr,
        const TCHAR* FieldD = nullptr)
    {
        FString Value;
        if (Obj.IsValid() && FieldA && Obj->TryGetStringField(FieldA, Value) && !Value.IsEmpty())
        {
            return Value;
        }
        if (Obj.IsValid() && FieldB && Obj->TryGetStringField(FieldB, Value) && !Value.IsEmpty())
        {
            return Value;
        }
        if (Obj.IsValid() && FieldC && Obj->TryGetStringField(FieldC, Value) && !Value.IsEmpty())
        {
            return Value;
        }
        if (Obj.IsValid() && FieldD && Obj->TryGetStringField(FieldD, Value) && !Value.IsEmpty())
        {
            return Value;
        }
        return FString();
    }

    static bool TryGetStringArrayField(
        const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* FieldName,
        TArray<FString>& OutValues,
        FString& OutError)
    {
        OutValues.Reset();
        if (!Obj.IsValid() || !FieldName || !Obj->HasField(FieldName))
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Obj->TryGetArrayField(FieldName, Values) || !Values)
        {
            OutError = FString::Printf(TEXT("%s must be an array of strings."), FieldName);
            return false;
        }

        for (int32 Index = 0; Index < Values->Num(); ++Index)
        {
            FString Value;
            if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetString(Value) || Value.IsEmpty())
            {
                OutError = FString::Printf(TEXT("%s[%d] must be a non-empty string."), FieldName, Index);
                return false;
            }
            OutValues.Add(Value);
        }
        return true;
    }

    static bool TryGetFirstStringArrayField(
        const TSharedPtr<FJsonObject>& Obj,
        TArray<FString>& OutValues,
        FString& OutError,
        const TCHAR* FieldA,
        const TCHAR* FieldB = nullptr,
        const TCHAR* FieldC = nullptr)
    {
        OutValues.Reset();
        const TCHAR* Candidates[3] = { FieldA, FieldB, FieldC };
        for (const TCHAR* Candidate : Candidates)
        {
            if (!Obj.IsValid() || !Candidate || !Obj->HasField(Candidate))
            {
                continue;
            }
            return TryGetStringArrayField(Obj, Candidate, OutValues, OutError);
        }
        return true;
    }

    static bool TryGetObjectArray(
        const TSharedPtr<FJsonObject>& Spec,
        const FString& FieldName,
        TArray<TSharedPtr<FJsonObject>>& OutObjects,
        FString& OutError)
    {
        if (!Spec.IsValid() || !Spec->HasField(FieldName))
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Spec->TryGetArrayField(FieldName, Values) || !Values)
        {
            OutError = FString::Printf(TEXT("%s must be an array of objects."), *FieldName);
            return false;
        }

        for (int32 Index = 0; Index < Values->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetObject(Entry) || !Entry || !Entry->IsValid())
            {
                OutError = FString::Printf(TEXT("%s[%d] must be an object."), *FieldName, Index);
                return false;
            }
            OutObjects.Add(*Entry);
        }
        return true;
    }

    static void CopyFieldIfMissing(
        const TSharedPtr<FJsonObject>& Source,
        const TSharedPtr<FJsonObject>& Destination,
        const FString& FieldName)
    {
        if (!Source.IsValid() || !Destination.IsValid() || Destination->HasField(FieldName))
        {
            return;
        }

        TSharedPtr<FJsonValue> Value = Source->TryGetField(FieldName);
        if (Value.IsValid())
        {
            Destination->SetField(FieldName, Value);
        }
    }

    static void CopySharedRemapDefaults(
        const TSharedPtr<FJsonObject>& Spec,
        const TSharedPtr<FJsonObject>& StepParams)
    {
        CopyFieldIfMissing(Spec, StepParams, TEXT("class_remaps"));
        CopyFieldIfMissing(Spec, StepParams, TEXT("object_remaps"));
        CopyFieldIfMissing(Spec, StepParams, TEXT("root_remaps"));
        CopyFieldIfMissing(Spec, StepParams, TEXT("source_root"));
        CopyFieldIfMissing(Spec, StepParams, TEXT("dest_root"));
    }

    static void CopyFontRemapDefaults(
        const TSharedPtr<FJsonObject>& Spec,
        const TSharedPtr<FJsonObject>& StepParams)
    {
        CopyFieldIfMissing(Spec, StepParams, TEXT("root_remaps"));
        CopyFieldIfMissing(Spec, StepParams, TEXT("source_root"));
        CopyFieldIfMissing(Spec, StepParams, TEXT("dest_root"));
        CopyFieldIfMissing(Spec, StepParams, TEXT("font_asset_remaps"));
    }

    static void SetBoolIfMissing(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, bool Value)
    {
        if (Obj.IsValid() && !Obj->HasField(FieldName))
        {
            Obj->SetBoolField(FieldName, Value);
        }
    }

    static void SetStringIfMissing(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, const FString& Value)
    {
        if (Obj.IsValid() && !Obj->HasField(FieldName) && !Value.IsEmpty())
        {
            Obj->SetStringField(FieldName, Value);
        }
    }

    static FMenuTransformStep MakeStep(
        const FString& Type,
        const FString& Namespace,
        const FString& Action,
        int32 SourceIndex,
        const TSharedPtr<FJsonObject>& Params,
        EMenuTransformDryRunMode DryRunMode,
        bool bMutating = true)
    {
        FMenuTransformStep Step;
        Step.Type = Type;
        Step.Namespace = Namespace;
        Step.Action = Action;
        Step.SourceIndex = SourceIndex;
        Step.Params = Params;
        Step.DryRunMode = DryRunMode;
        Step.bMutating = bMutating;
        return Step;
    }

    static void IncrementCount(TMap<FString, int32>& Counts, const FString& Key)
    {
        Counts.FindOrAdd(Key) += 1;
    }

    static TSharedPtr<FJsonObject> MakeCountsObject(const TMap<FString, int32>& Counts)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        for (const TPair<FString, int32>& Pair : Counts)
        {
            Obj->SetNumberField(Pair.Key, Pair.Value);
        }
        return Obj;
    }

    static bool TryGetResultBool(const FMonolithActionResult& ChildResult, const FString& FieldName, bool& OutValue);
    static bool ChildPayloadReportsOk(const FMonolithActionResult& ChildResult);

    // ------------------------------------------------------------------
    // ui::diff_ui_spec + ui::apply_ui_spec_patch
    //
    // These actions intentionally use the canonical FUISpecDocument shape.
    // They do not add apply_json_to_umg/apply_layout compatibility actions;
    // external design data enters through convert_markup_to_ui_spec or caller-
    // supplied JSON, then flows through Monolith-owned diff/patch actions.

    struct FUISpecNodeRef
    {
        const FUISpecNode* Node = nullptr;
        FName ParentId;
        FString Path;
        int32 ChildIndex = INDEX_NONE;
    };

    static bool JsonValuesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B);

    static bool JsonObjectsEqual(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
    {
        if (A == B)
        {
            return true;
        }
        if (!A.IsValid() || !B.IsValid() || A->Values.Num() != B->Values.Num())
        {
            return false;
        }

        for (const auto& Pair : A->Values)
        {
            const TSharedPtr<FJsonValue> OtherValue = B->TryGetField(MakeStringView(Pair.Key));
            if (!OtherValue.IsValid() || !JsonValuesEqual(Pair.Value, OtherValue))
            {
                return false;
            }
        }
        return true;
    }

    static bool JsonArraysEqual(const TArray<TSharedPtr<FJsonValue>>& A, const TArray<TSharedPtr<FJsonValue>>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }
        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            if (!JsonValuesEqual(A[Index], B[Index]))
            {
                return false;
            }
        }
        return true;
    }

    static bool JsonValuesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
    {
        if (A == B)
        {
            return true;
        }
        if (!A.IsValid() || !B.IsValid() || A->Type != B->Type)
        {
            return false;
        }

        switch (A->Type)
        {
        case EJson::None:
        case EJson::Null:
            return true;
        case EJson::String:
            return A->AsString() == B->AsString();
        case EJson::Number:
            return FMath::IsNearlyEqual(A->AsNumber(), B->AsNumber(), UE_SMALL_NUMBER);
        case EJson::Boolean:
            return A->AsBool() == B->AsBool();
        case EJson::Array:
            return JsonArraysEqual(A->AsArray(), B->AsArray());
        case EJson::Object:
            return JsonObjectsEqual(A->AsObject(), B->AsObject());
        default:
            return false;
        }
    }

    static FString CompareModeOrDefault(const TSharedPtr<FJsonObject>& Params)
    {
        FString CompareMode = TEXT("structural");
        if (Params.IsValid())
        {
            Params->TryGetStringField(TEXT("compare_mode"), CompareMode);
        }
        if (!CompareMode.Equals(TEXT("structural"), ESearchCase::IgnoreCase) &&
            !CompareMode.Equals(TEXT("properties"), ESearchCase::IgnoreCase) &&
            !CompareMode.Equals(TEXT("full"), ESearchCase::IgnoreCase))
        {
            return FString();
        }
        return CompareMode.ToLower();
    }

    static void BuildNodeRefMap(
        const TSharedPtr<FUISpecNode>& Node,
        const FName& ParentId,
        const FString& Path,
        int32 ChildIndex,
        TMap<FName, FUISpecNodeRef>& OutNodes,
        TArray<TSharedPtr<FJsonValue>>& OutUnsupported)
    {
        if (!Node.IsValid())
        {
            return;
        }

        if (Node->Id.IsNone())
        {
            TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
            Finding->SetStringField(TEXT("path"), Path);
            Finding->SetStringField(TEXT("field"), TEXT("id"));
            Finding->SetStringField(TEXT("reason"), TEXT("UISpec diff/patch requires stable widget ids; tree-index-only addressing is rejected."));
            OutUnsupported.Add(MakeShared<FJsonValueObject>(Finding));
        }
        else
        {
            FUISpecNodeRef Ref;
            Ref.Node = Node.Get();
            Ref.ParentId = ParentId;
            Ref.Path = Path;
            Ref.ChildIndex = ChildIndex;
            OutNodes.Add(Node->Id, Ref);
        }

        for (int32 Index = 0; Index < Node->Children.Num(); ++Index)
        {
            const FString ChildPath = FString::Printf(TEXT("%s/children/%d"), *Path, Index);
            BuildNodeRefMap(Node->Children[Index], Node->Id, ChildPath, Index, OutNodes, OutUnsupported);
        }
    }

    static TArray<FString> GetDirectChildWidgetNames(const FUISpecNode& Node)
    {
        TArray<FString> Names;
        Names.Reserve(Node.Children.Num());
        for (const TSharedPtr<FUISpecNode>& Child : Node.Children)
        {
            if (Child.IsValid() && !Child->Id.IsNone())
            {
                Names.Add(Child->Id.ToString());
            }
        }
        return Names;
    }

    static bool DirectChildWidgetNamesEqual(const FUISpecNode& A, const FUISpecNode& B)
    {
        const TArray<FString> AChildren = GetDirectChildWidgetNames(A);
        const TArray<FString> BChildren = GetDirectChildWidgetNames(B);
        if (AChildren.Num() != BChildren.Num())
        {
            return false;
        }
        for (int32 Index = 0; Index < AChildren.Num(); ++Index)
        {
            if (AChildren[Index] != BChildren[Index])
            {
                return false;
            }
        }
        return true;
    }

    static void SetStringArrayField(
        const TSharedPtr<FJsonObject>& Obj,
        const FString& FieldName,
        const TArray<FString>& Strings)
    {
        if (!Obj.IsValid())
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Strings.Num());
        for (const FString& String : Strings)
        {
            Values.Add(MakeShared<FJsonValueString>(String));
        }
        Obj->SetArrayField(FieldName, Values);
    }

    static FString MakeReplacementTempWidgetName(const FString& WidgetName)
    {
        return WidgetName + TEXT("__ReplacementTmp");
    }

    static bool ReplacementClassCanHostPreservedChildren(
        const FString& WidgetClass,
        int32 ChildCount,
        FString& OutReason)
    {
        UClass* ResolvedClass = MonolithUI::WidgetClassFromName(WidgetClass);
        if (!ResolvedClass)
        {
            OutReason = FString::Printf(
                TEXT("replace_widget preserve_children could not resolve replacement widget_class '%s'."),
                *WidgetClass);
            return false;
        }

        UPanelWidget* PanelCDO = Cast<UPanelWidget>(ResolvedClass->GetDefaultObject());
        if (!PanelCDO)
        {
            OutReason = FString::Printf(
                TEXT("replace_widget preserve_children requires replacement widget_class '%s' to be a UPanelWidget subclass."),
                *WidgetClass);
            return false;
        }

        if (ChildCount > 1 && !PanelCDO->CanHaveMultipleChildren())
        {
            OutReason = FString::Printf(
                TEXT("replace_widget preserve_children cannot move %d children into single-child replacement widget_class '%s'."),
                ChildCount,
                *WidgetClass);
            return false;
        }

        return true;
    }

    static TSharedPtr<FJsonObject> MakePatchCandidate(
        const FString& Op,
        const FString& WidgetName,
        const FString& Reason)
    {
        TSharedPtr<FJsonObject> Patch = MakeShared<FJsonObject>();
        Patch->SetStringField(TEXT("op"), Op);
        if (!WidgetName.IsEmpty())
        {
            Patch->SetStringField(TEXT("widget_name"), WidgetName);
        }
        if (!Reason.IsEmpty())
        {
            Patch->SetStringField(TEXT("reason"), Reason);
        }
        return Patch;
    }

    static TSharedPtr<FJsonObject> MakeDiffRow(
        const FString& Kind,
        const FName& WidgetId,
        const FString& Path,
        const FString& Field,
        const FString& Message,
        const TSharedPtr<FJsonObject>& Patch = nullptr)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("kind"), Kind);
        Row->SetStringField(TEXT("widget_id"), WidgetId.ToString());
        Row->SetStringField(TEXT("path"), Path);
        if (!Field.IsEmpty())
        {
            Row->SetStringField(TEXT("field"), Field);
        }
        Row->SetStringField(TEXT("message"), Message);
        if (Patch.IsValid())
        {
            Row->SetObjectField(TEXT("patch_candidate"), Patch);
        }
        return Row;
    }

    static TSharedPtr<FJsonObject> MakeGraphBindingPreservationReport(
        UWidgetBlueprint* WBP,
        const TMap<FName, FUISpecNodeRef>& CurrentNodes,
        const TMap<FName, FUISpecNodeRef>& DesiredNodes)
    {
        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("schema_version"), TEXT("ui_graph_binding_preservation.v1"));
        Report->SetStringField(TEXT("status"), TEXT("reported"));
        Report->SetStringField(TEXT("patch_policy"), TEXT("UISpec patches do not synthesize or delete Blueprint graph/property bindings; bindings are preserved unless the target widget is explicitly removed."));
        Report->SetStringField(TEXT("write_owner"), TEXT("workflow.ui_bind_widget_event"));
        Report->SetBoolField(TEXT("represented_in_ui_spec"), false);

        TArray<TSharedPtr<FJsonValue>> Rows;
        int32 PreservedCount = 0;
        int32 AtRiskCount = 0;
        int32 MissingCurrentTargetCount = 0;

        if (WBP)
        {
            Rows.Reserve(WBP->Bindings.Num());
            for (const FDelegateEditorBinding& Binding : WBP->Bindings)
            {
                const FName WidgetId(*Binding.ObjectName);
                const bool bCurrentWidgetPresent = CurrentNodes.Contains(WidgetId);
                const bool bDesiredWidgetPresent = DesiredNodes.Contains(WidgetId);

                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("widget_name"), Binding.ObjectName);
                Row->SetStringField(TEXT("property_name"), Binding.PropertyName.ToString());
                Row->SetStringField(TEXT("function_name"), Binding.FunctionName.ToString());
                Row->SetStringField(TEXT("source_property"), Binding.SourceProperty.ToString());
                Row->SetStringField(TEXT("binding_kind"), StaticEnum<EBindingKind>() ? StaticEnum<EBindingKind>()->GetNameStringByValue(static_cast<int64>(Binding.Kind)) : TEXT("unknown"));
                Row->SetBoolField(TEXT("current_widget_present"), bCurrentWidgetPresent);
                Row->SetBoolField(TEXT("desired_widget_present"), bDesiredWidgetPresent);
                Row->SetBoolField(TEXT("represented_in_ui_spec"), false);

                if (!bCurrentWidgetPresent)
                {
                    Row->SetStringField(TEXT("status"), TEXT("binding_target_not_in_current_spec_dump"));
                    Row->SetStringField(TEXT("patch_behavior"), TEXT("reported_only; target widget was not found in the current UISpec dump."));
                    ++MissingCurrentTargetCount;
                    ++AtRiskCount;
                }
                else if (!bDesiredWidgetPresent)
                {
                    Row->SetStringField(TEXT("status"), TEXT("target_widget_absent_in_desired_spec"));
                    Row->SetStringField(TEXT("patch_behavior"), TEXT("at_risk_if_remove_widget_patch_is_applied; no graph binding rewrite is generated."));
                    ++AtRiskCount;
                }
                else
                {
                    Row->SetStringField(TEXT("status"), TEXT("preserved_by_default"));
                    Row->SetStringField(TEXT("patch_behavior"), TEXT("not_modified_by_ui_spec_patch"));
                    ++PreservedCount;
                }

                Rows.Add(MakeShared<FJsonValueObject>(Row));
            }
        }

        Report->SetNumberField(TEXT("binding_count"), WBP ? WBP->Bindings.Num() : 0);
        Report->SetNumberField(TEXT("preserved_by_default_count"), PreservedCount);
        Report->SetNumberField(TEXT("at_risk_binding_count"), AtRiskCount);
        Report->SetNumberField(TEXT("missing_current_target_count"), MissingCurrentTargetCount);
        Report->SetBoolField(TEXT("has_graph_bindings"), WBP && WBP->Bindings.Num() > 0);
        Report->SetArrayField(TEXT("bindings"), Rows);
        return Report;
    }

    static FString NormalizeStyleVisibilityToken(const FUISpecStyle& Style)
    {
        const FString Visibility = Style.Visibility.ToString();
        return Visibility.IsEmpty() || Style.Visibility.IsNone()
            ? FString(TEXT("Visible"))
            : Visibility;
    }

    static bool StyleHasCommonAddPatchIntent(const FUISpecStyle& Style)
    {
        const FString Visibility = NormalizeStyleVisibilityToken(Style);
        return !FMath::IsNearlyEqual(Style.Opacity, 1.0f, 0.001f)
            || (!Style.Visibility.IsNone() && !Visibility.Equals(TEXT("Visible"), ESearchCase::CaseSensitive));
    }

    static bool IsSpecSizeBoxNode(const FUISpecNode& Node)
    {
        return Node.Type.ToString().Equals(TEXT("SizeBox"), ESearchCase::IgnoreCase);
    }

    static bool IsSpecBorderNode(const FUISpecNode& Node)
    {
        const FString Type = Node.Type.ToString();
        return Type.Equals(TEXT("Border"), ESearchCase::IgnoreCase)
            || Type.Equals(TEXT("RoundedBorder"), ESearchCase::IgnoreCase);
    }

    static bool IsSpecProgressBarNode(const FUISpecNode& Node)
    {
        return Node.Type.ToString().Equals(TEXT("ProgressBar"), ESearchCase::IgnoreCase);
    }

    static bool StylePaddingHasIntent(const FMargin& Padding)
    {
        return Padding.GetTotalSpaceAlong<EOrientation::Orient_Horizontal>() != 0
            || Padding.GetTotalSpaceAlong<EOrientation::Orient_Vertical>() != 0;
    }

    static bool StyleColorHasIntent(const FLinearColor& Color)
    {
        return !Color.Equals(FLinearColor::Transparent);
    }

    static bool StyleHasSizeBoxAddPatchIntent(const FUISpecStyle& Style)
    {
        return Style.Width > 0.0f
            || Style.Height > 0.0f
            || Style.bOverrideMinDesiredWidth
            || Style.bOverrideMinDesiredHeight
            || Style.bOverrideMaxDesiredWidth
            || Style.bOverrideMaxDesiredHeight;
    }

    static bool StyleHasBorderAddPatchIntent(const FUISpecStyle& Style)
    {
        return StyleColorHasIntent(Style.Background)
            || StylePaddingHasIntent(Style.Padding);
    }

    static bool StyleHasProgressBarAddPatchIntent(const FUISpecStyle& Style)
    {
        return StyleColorHasIntent(Style.Background);
    }

    static TSharedPtr<FJsonObject> MarginToPatchObject(const FMargin& Margin)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("left"), Margin.Left);
        Out->SetNumberField(TEXT("top"), Margin.Top);
        Out->SetNumberField(TEXT("right"), Margin.Right);
        Out->SetNumberField(TEXT("bottom"), Margin.Bottom);
        return Out;
    }

    static void AddSizeBoxStyleFieldsToPatchObject(const FUISpecStyle& Style, const TSharedPtr<FJsonObject>& Out)
    {
        if (!Out.IsValid())
        {
            return;
        }
        if (Style.Width > 0.0f)
        {
            Out->SetNumberField(TEXT("width"), Style.Width);
        }
        if (Style.Height > 0.0f)
        {
            Out->SetNumberField(TEXT("height"), Style.Height);
        }
        if (Style.bOverrideMinDesiredWidth)
        {
            Out->SetNumberField(TEXT("minDesiredWidth"), Style.MinDesiredWidth);
        }
        if (Style.bOverrideMinDesiredHeight)
        {
            Out->SetNumberField(TEXT("minDesiredHeight"), Style.MinDesiredHeight);
        }
        if (Style.bOverrideMaxDesiredWidth)
        {
            Out->SetNumberField(TEXT("maxDesiredWidth"), Style.MaxDesiredWidth);
        }
        if (Style.bOverrideMaxDesiredHeight)
        {
            Out->SetNumberField(TEXT("maxDesiredHeight"), Style.MaxDesiredHeight);
        }
    }

    static TSharedPtr<FJsonObject> MakeStylePatchObjectForAdd(const FUISpecNode& Node)
    {
        const FUISpecStyle& Style = Node.Style;
        const bool bHasTypeSpecificIntent =
            (IsSpecSizeBoxNode(Node) && StyleHasSizeBoxAddPatchIntent(Style))
            || (IsSpecBorderNode(Node) && StyleHasBorderAddPatchIntent(Style))
            || (IsSpecProgressBarNode(Node) && StyleHasProgressBarAddPatchIntent(Style));

        if (!StyleHasCommonAddPatchIntent(Style) && !bHasTypeSpecificIntent)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!FMath::IsNearlyEqual(Style.Opacity, 1.0f, 0.001f))
        {
            Out->SetNumberField(TEXT("opacity"), Style.Opacity);
        }
        const FString Visibility = NormalizeStyleVisibilityToken(Style);
        if (!Style.Visibility.IsNone() && !Visibility.Equals(TEXT("Visible"), ESearchCase::CaseSensitive))
        {
            Out->SetStringField(TEXT("visibility"), Visibility);
        }
        if (IsSpecSizeBoxNode(Node))
        {
            AddSizeBoxStyleFieldsToPatchObject(Style, Out);
        }
        if (IsSpecBorderNode(Node))
        {
            if (StyleColorHasIntent(Style.Background))
            {
                Out->SetStringField(TEXT("background"), ColorToHexString(Style.Background));
            }
            if (StylePaddingHasIntent(Style.Padding))
            {
                Out->SetObjectField(TEXT("padding"), MarginToPatchObject(Style.Padding));
            }
        }
        if (IsSpecProgressBarNode(Node) && StyleColorHasIntent(Style.Background))
        {
            Out->SetStringField(TEXT("background"), ColorToHexString(Style.Background));
        }
        return Out;
    }

    static TSharedPtr<FJsonObject> MakeAddWidgetPatchCandidate(
        const FUISpecNodeRef& DesiredRef,
        const FName& DesiredParent)
    {
        const FUISpecNode& Node = *DesiredRef.Node;
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(TEXT("add_widget"), Node.Id.ToString(), TEXT("desired_spec contains a widget absent from the live WBP"));
        Patch->SetStringField(TEXT("widget_class"), Node.Type.ToString());
        Patch->SetStringField(TEXT("widget_name"), Node.Id.ToString());
        if (!DesiredParent.IsNone())
        {
            Patch->SetStringField(TEXT("parent_name"), DesiredParent.ToString());
        }
        Patch->SetObjectField(TEXT("slot"), SlotToJson(Node.Slot));
        if (!Node.Content.Text.IsEmpty()
            || Node.Content.FontSize > 0.0f
            || Node.Content.FontColor != FLinearColor::White
            || !Node.Content.BrushPath.IsEmpty())
        {
            Patch->SetObjectField(TEXT("content"), ContentToJson(Node.Content));
        }
        if (Node.bHasEffect)
        {
            Patch->SetObjectField(TEXT("effect"), EffectToJson(Node.Effect));
        }
        if (TSharedPtr<FJsonObject> StylePatch = MakeStylePatchObjectForAdd(Node))
        {
            Patch->SetObjectField(TEXT("style"), StylePatch);
        }
        return Patch;
    }

    static TSharedPtr<FJsonObject> MakeReplaceWidgetPatchCandidate(
        const FUISpecNodeRef& CurrentRef,
        const FUISpecNodeRef& DesiredRef,
        const FName& DesiredParent,
        const FName& CurrentType)
    {
        TSharedPtr<FJsonObject> Patch = MakeAddWidgetPatchCandidate(DesiredRef, DesiredParent);
        Patch->SetStringField(TEXT("op"), TEXT("replace_widget"));
        Patch->SetStringField(TEXT("reason"), TEXT("desired_spec widget type differs from the live WBP; explicit confirm-gated replacement is required"));
        Patch->SetStringField(TEXT("current_widget_class"), CurrentType.ToString());
        Patch->SetBoolField(TEXT("requires_confirm_replace"), true);

        const TArray<FString> PreservedChildNames = CurrentRef.Node ? GetDirectChildWidgetNames(*CurrentRef.Node) : TArray<FString>();
        FString PreservationReason;
        const bool bCanPreserveChildren =
            CurrentRef.Node
            && DesiredRef.Node
            && PreservedChildNames.Num() > 0
            && DirectChildWidgetNamesEqual(*CurrentRef.Node, *DesiredRef.Node)
            && ReplacementClassCanHostPreservedChildren(DesiredRef.Node->Type.ToString(), PreservedChildNames.Num(), PreservationReason);

        Patch->SetBoolField(TEXT("preserve_children"), bCanPreserveChildren);
        if (bCanPreserveChildren)
        {
            Patch->SetStringField(TEXT("replacement_strategy"), TEXT("add_temp_move_children_remove_rename_via_existing_owner_actions"));
            Patch->SetStringField(TEXT("temporary_widget_name"), MakeReplacementTempWidgetName(DesiredRef.Node->Id.ToString()));
            SetStringArrayField(Patch, TEXT("child_widget_names"), PreservedChildNames);
        }
        else
        {
            Patch->SetStringField(TEXT("replacement_strategy"), TEXT("remove_then_add_via_existing_owner_actions"));
            if (PreservedChildNames.Num() > 0)
            {
                if (PreservationReason.IsEmpty())
                {
                    PreservationReason = TEXT("direct child ids differ between current and desired specs; implicit child preservation would hide a structural delete/reparent.");
                }
                Patch->SetStringField(TEXT("preserve_children_unavailable_reason"), PreservationReason);
            }
        }
        return Patch;
    }

    static TSharedPtr<FJsonObject> MakeReplaceDecompositionEvidence()
    {
        TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
        Evidence->SetStringField(TEXT("schema_version"), TEXT("ui_replace_decomposition.v2"));
        Evidence->SetStringField(TEXT("op"), TEXT("replace_widget"));
        Evidence->SetBoolField(TEXT("requires_confirm_replace"), true);
        Evidence->SetBoolField(TEXT("preserve_children_supported"), true);
        Evidence->SetStringField(TEXT("strategy"), TEXT("Decompose explicit replacement through existing owner actions; preserve_children uses a temporary replacement widget, ui.move_widget for direct children, ui.remove_widget for the old node, and ui.rename_widget to restore the stable name."));

        TArray<TSharedPtr<FJsonValue>> OwnerActions;
        OwnerActions.Add(MakeShared<FJsonValueString>(TEXT("ui.add_widget")));
        OwnerActions.Add(MakeShared<FJsonValueString>(TEXT("ui.move_widget")));
        OwnerActions.Add(MakeShared<FJsonValueString>(TEXT("ui.remove_widget")));
        OwnerActions.Add(MakeShared<FJsonValueString>(TEXT("ui.rename_widget")));
        OwnerActions.Add(MakeShared<FJsonValueString>(TEXT("ui.set_text")));
        OwnerActions.Add(MakeShared<FJsonValueString>(TEXT("ui.set_image")));
        OwnerActions.Add(MakeShared<FJsonValueString>(TEXT("ui.set_widget_property")));
        OwnerActions.Add(MakeShared<FJsonValueString>(TEXT("ui.set_effect_surface_*")));
        Evidence->SetArrayField(TEXT("owner_actions"), OwnerActions);
        return Evidence;
    }

    static TSharedPtr<FJsonObject> MakeMoveWidgetPatchCandidate(
        const FName& WidgetId,
        const FName& DesiredParent)
    {
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(TEXT("move_widget"), WidgetId.ToString(), TEXT("desired_spec parent differs from live WBP"));
        Patch->SetStringField(TEXT("new_parent_name"), DesiredParent.ToString());
        return Patch;
    }

    static TSharedPtr<FJsonObject> MakeSlotPatchCandidate(const FUISpecNodeRef& DesiredRef)
    {
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(TEXT("set_slot_property"), DesiredRef.Node->Id.ToString(), TEXT("desired_spec slot differs from live WBP"));
        Patch->SetObjectField(TEXT("slot"), SlotToJson(DesiredRef.Node->Slot));
        return Patch;
    }

    static bool LinearColorsNearlyEqual(const FLinearColor& A, const FLinearColor& B, const float Tolerance = 0.001f)
    {
        return FMath::IsNearlyEqual(A.R, B.R, Tolerance)
            && FMath::IsNearlyEqual(A.G, B.G, Tolerance)
            && FMath::IsNearlyEqual(A.B, B.B, Tolerance)
            && FMath::IsNearlyEqual(A.A, B.A, Tolerance);
    }

    static bool Vector2DsNearlyEqual(const FVector2D& A, const FVector2D& B, const float Tolerance = 0.001f)
    {
        return FMath::IsNearlyEqual(A.X, B.X, Tolerance)
            && FMath::IsNearlyEqual(A.Y, B.Y, Tolerance);
    }

    static bool Vector4sNearlyEqual(const FVector4& A, const FVector4& B, const float Tolerance = 0.001f)
    {
        return FMath::IsNearlyEqual(A.X, B.X, Tolerance)
            && FMath::IsNearlyEqual(A.Y, B.Y, Tolerance)
            && FMath::IsNearlyEqual(A.Z, B.Z, Tolerance)
            && FMath::IsNearlyEqual(A.W, B.W, Tolerance);
    }

    static bool MarginsNearlyEqual(const FMargin& A, const FMargin& B, const float Tolerance = 0.001f)
    {
        return FMath::IsNearlyEqual(A.Left, B.Left, Tolerance)
            && FMath::IsNearlyEqual(A.Top, B.Top, Tolerance)
            && FMath::IsNearlyEqual(A.Right, B.Right, Tolerance)
            && FMath::IsNearlyEqual(A.Bottom, B.Bottom, Tolerance);
    }

    static bool IsSpecImageNode(const FUISpecNode& Node)
    {
        return Node.Type.ToString().Equals(TEXT("Image"), ESearchCase::IgnoreCase);
    }

    static bool IsSpecEffectSurfaceNode(const FUISpecNode& Node)
    {
        return Node.Type.ToString().Equals(TEXT("EffectSurface"), ESearchCase::IgnoreCase);
    }

    static bool IsSpecTextNode(const FUISpecNode& Node)
    {
        const FString Type = Node.Type.ToString();
        return Type.Equals(TEXT("TextBlock"), ESearchCase::IgnoreCase)
            || Type.Equals(TEXT("RichTextBlock"), ESearchCase::IgnoreCase);
    }

    static FString GetSpecBrushPropertyNameForTypeToken(const FString& WidgetClass)
    {
        if (WidgetClass.Equals(TEXT("Border"), ESearchCase::IgnoreCase))
        {
            return TEXT("Background");
        }
        return FString();
    }

    static FString GetSpecBrushPropertyNameForNode(const FUISpecNode& Node)
    {
        return GetSpecBrushPropertyNameForTypeToken(Node.Type.ToString());
    }

    static bool TryPopulateBrushPathAsImageParam(
        const FString& BrushPath,
        const TSharedPtr<FJsonObject>& Params,
        FString& OutError)
    {
        if (BrushPath.IsEmpty())
        {
            OutError = TEXT("brushPath is empty; clearing an Image brush is not supported by ui.set_image.");
            return false;
        }

        if (LoadObject<UTexture2D>(nullptr, *BrushPath))
        {
            SetStringIfMissing(Params, TEXT("texture_path"), BrushPath);
            return true;
        }

        if (LoadObject<UMaterialInterface>(nullptr, *BrushPath))
        {
            SetStringIfMissing(Params, TEXT("material_path"), BrushPath);
            return true;
        }

        OutError = FString::Printf(
            TEXT("brushPath '%s' could not be resolved as a UTexture2D or UMaterialInterface asset."),
            *BrushPath);
        return false;
    }

    static bool TryPopulateBrushPathAsBrushParam(
        const FString& BrushPath,
        const TSharedPtr<FJsonObject>& Params,
        FString& OutError)
    {
        if (BrushPath.IsEmpty())
        {
            OutError = TEXT("brushPath is empty; clearing a non-Image brush is not supported by ui.set_brush.");
            return false;
        }

        if (LoadObject<UTexture2D>(nullptr, *BrushPath))
        {
            SetStringIfMissing(Params, TEXT("texture_path"), BrushPath);
            return true;
        }

        if (LoadObject<UMaterialInterface>(nullptr, *BrushPath))
        {
            SetStringIfMissing(Params, TEXT("material_path"), BrushPath);
            return true;
        }

        OutError = FString::Printf(
            TEXT("brushPath '%s' could not be resolved as a UTexture2D or UMaterialInterface asset."),
            *BrushPath);
        return false;
    }

    static TSharedPtr<FJsonObject> MakeSetTextPatchCandidate(
        const FUISpecNodeRef& DesiredRef,
        const FUISpecNode* CurrentNode)
    {
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(TEXT("set_text"), DesiredRef.Node->Id.ToString(), TEXT("desired_spec text/font content differs from live WBP"));
        if (!CurrentNode || CurrentNode->Content.Text != DesiredRef.Node->Content.Text)
        {
            Patch->SetStringField(TEXT("text"), DesiredRef.Node->Content.Text);
        }
        if (DesiredRef.Node->Content.FontSize > 0.0f
            && (!CurrentNode || !FMath::IsNearlyEqual(CurrentNode->Content.FontSize, DesiredRef.Node->Content.FontSize, 0.1f)))
        {
            Patch->SetNumberField(TEXT("font_size"), DesiredRef.Node->Content.FontSize);
        }
        if (!CurrentNode || !LinearColorsNearlyEqual(CurrentNode->Content.FontColor, DesiredRef.Node->Content.FontColor))
        {
            Patch->SetStringField(TEXT("text_color"), ColorToHexString(DesiredRef.Node->Content.FontColor));
        }
        return Patch;
    }

    static bool ContentHasPatchableTextDelta(const FUISpecNode& Current, const FUISpecNode& Desired)
    {
        return IsSpecTextNode(Desired)
            && (Current.Content.Text != Desired.Content.Text
            || !FMath::IsNearlyEqual(Current.Content.FontSize, Desired.Content.FontSize, 0.1f)
            || !LinearColorsNearlyEqual(Current.Content.FontColor, Desired.Content.FontColor));
    }

    static bool ContentHasPatchableImageDelta(const FUISpecNode& Current, const FUISpecNode& Desired)
    {
        return IsSpecImageNode(Desired) && Current.Content.BrushPath != Desired.Content.BrushPath;
    }

    static bool ContentHasPatchableBrushDelta(const FUISpecNode& Current, const FUISpecNode& Desired)
    {
        return !IsSpecImageNode(Desired)
            && !GetSpecBrushPropertyNameForNode(Desired).IsEmpty()
            && Current.Content.BrushPath != Desired.Content.BrushPath;
    }

    static bool StyleHasCommonPatchableDelta(const FUISpecStyle& Current, const FUISpecStyle& Desired)
    {
        return !FMath::IsNearlyEqual(Current.Opacity, Desired.Opacity, 0.001f)
            || !NormalizeStyleVisibilityToken(Current).Equals(NormalizeStyleVisibilityToken(Desired), ESearchCase::CaseSensitive);
    }

    static bool StyleHasSizeBoxPatchableDelta(const FUISpecStyle& Current, const FUISpecStyle& Desired)
    {
        return (Desired.Width > 0.0f && !FMath::IsNearlyEqual(Current.Width, Desired.Width, 0.001f))
            || (Desired.Height > 0.0f && !FMath::IsNearlyEqual(Current.Height, Desired.Height, 0.001f))
            || (Desired.bOverrideMinDesiredWidth && (!Current.bOverrideMinDesiredWidth || !FMath::IsNearlyEqual(Current.MinDesiredWidth, Desired.MinDesiredWidth, 0.001f)))
            || (Desired.bOverrideMinDesiredHeight && (!Current.bOverrideMinDesiredHeight || !FMath::IsNearlyEqual(Current.MinDesiredHeight, Desired.MinDesiredHeight, 0.001f)))
            || (Desired.bOverrideMaxDesiredWidth && (!Current.bOverrideMaxDesiredWidth || !FMath::IsNearlyEqual(Current.MaxDesiredWidth, Desired.MaxDesiredWidth, 0.001f)))
            || (Desired.bOverrideMaxDesiredHeight && (!Current.bOverrideMaxDesiredHeight || !FMath::IsNearlyEqual(Current.MaxDesiredHeight, Desired.MaxDesiredHeight, 0.001f)));
    }

    static bool StyleHasBorderPatchableDelta(const FUISpecStyle& Current, const FUISpecStyle& Desired)
    {
        return !MarginsNearlyEqual(Current.Padding, Desired.Padding)
            || !LinearColorsNearlyEqual(Current.Background, Desired.Background);
    }

    static bool StyleHasProgressBarPatchableDelta(const FUISpecStyle& Current, const FUISpecStyle& Desired)
    {
        return !LinearColorsNearlyEqual(Current.Background, Desired.Background);
    }

    static bool StyleHasTypeSpecificPatchableDelta(const FUISpecNode& CurrentNode, const FUISpecNode& DesiredNode)
    {
        if (IsSpecSizeBoxNode(DesiredNode))
        {
            return StyleHasSizeBoxPatchableDelta(CurrentNode.Style, DesiredNode.Style);
        }
        if (IsSpecBorderNode(DesiredNode))
        {
            return StyleHasBorderPatchableDelta(CurrentNode.Style, DesiredNode.Style);
        }
        if (IsSpecProgressBarNode(DesiredNode))
        {
            return StyleHasProgressBarPatchableDelta(CurrentNode.Style, DesiredNode.Style);
        }
        return false;
    }

    static bool StyleHasUnsupportedPatchDelta(const FUISpecNode& CurrentNode, const FUISpecNode& DesiredNode)
    {
        const FUISpecStyle& Current = CurrentNode.Style;
        const FUISpecStyle& Desired = DesiredNode.Style;
        const bool bSizeBox = IsSpecSizeBoxNode(DesiredNode);
        const bool bBorder = IsSpecBorderNode(DesiredNode);
        const bool bProgressBar = IsSpecProgressBarNode(DesiredNode);

        auto DiffersUnlessSizeBoxSets = [bSizeBox](float CurrentValue, float DesiredValue, bool bDesiredSetsValue)
        {
            return !FMath::IsNearlyEqual(CurrentValue, DesiredValue, 0.001f)
                && !(bSizeBox && bDesiredSetsValue);
        };
        auto FlagDiffersUnlessSizeBoxSets = [bSizeBox](bool bCurrentFlag, bool bDesiredFlag)
        {
            return bCurrentFlag != bDesiredFlag
                && !(bSizeBox && bDesiredFlag);
        };

        return DiffersUnlessSizeBoxSets(Current.Width, Desired.Width, Desired.Width > 0.0f)
            || DiffersUnlessSizeBoxSets(Current.Height, Desired.Height, Desired.Height > 0.0f)
            || DiffersUnlessSizeBoxSets(Current.MinDesiredWidth, Desired.MinDesiredWidth, Desired.bOverrideMinDesiredWidth)
            || DiffersUnlessSizeBoxSets(Current.MinDesiredHeight, Desired.MinDesiredHeight, Desired.bOverrideMinDesiredHeight)
            || DiffersUnlessSizeBoxSets(Current.MaxDesiredWidth, Desired.MaxDesiredWidth, Desired.bOverrideMaxDesiredWidth)
            || DiffersUnlessSizeBoxSets(Current.MaxDesiredHeight, Desired.MaxDesiredHeight, Desired.bOverrideMaxDesiredHeight)
            || (!MarginsNearlyEqual(Current.Padding, Desired.Padding) && !bBorder)
            || (!LinearColorsNearlyEqual(Current.Background, Desired.Background) && !bBorder && !bProgressBar)
            || !LinearColorsNearlyEqual(Current.BorderColor, Desired.BorderColor)
            || !FMath::IsNearlyEqual(Current.BorderWidth, Desired.BorderWidth, 0.001f)
            || (Current.bUseCustomSize != Desired.bUseCustomSize && !(bSizeBox && (Desired.Width > 0.0f || Desired.Height > 0.0f)))
            || FlagDiffersUnlessSizeBoxSets(Current.bOverrideMinDesiredWidth, Desired.bOverrideMinDesiredWidth)
            || FlagDiffersUnlessSizeBoxSets(Current.bOverrideMinDesiredHeight, Desired.bOverrideMinDesiredHeight)
            || FlagDiffersUnlessSizeBoxSets(Current.bOverrideMaxDesiredWidth, Desired.bOverrideMaxDesiredWidth)
            || FlagDiffersUnlessSizeBoxSets(Current.bOverrideMaxDesiredHeight, Desired.bOverrideMaxDesiredHeight);
    }

    static bool StylesNearlyEqualForPatchDiff(const FUISpecNode& CurrentNode, const FUISpecNode& DesiredNode)
    {
        return !StyleHasCommonPatchableDelta(CurrentNode.Style, DesiredNode.Style)
            && !StyleHasTypeSpecificPatchableDelta(CurrentNode, DesiredNode)
            && !StyleHasUnsupportedPatchDelta(CurrentNode, DesiredNode);
    }

    static TSharedPtr<FJsonObject> MakeSetStylePatchCandidate(
        const FUISpecNodeRef& DesiredRef,
        const FUISpecNode& CurrentNode)
    {
        const FUISpecNode& DesiredNode = *DesiredRef.Node;
        if (!StyleHasCommonPatchableDelta(CurrentNode.Style, DesiredNode.Style)
            && !StyleHasTypeSpecificPatchableDelta(CurrentNode, DesiredNode))
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(
            TEXT("set_style"),
            DesiredRef.Node->Id.ToString(),
            TEXT("desired_spec UMG style differs from live WBP"));
        Patch->SetStringField(TEXT("widget_class"), DesiredNode.Type.ToString());

        if (!FMath::IsNearlyEqual(CurrentNode.Style.Opacity, DesiredNode.Style.Opacity, 0.001f))
        {
            Patch->SetNumberField(TEXT("opacity"), DesiredNode.Style.Opacity);
        }

        const FString CurrentVisibility = NormalizeStyleVisibilityToken(CurrentNode.Style);
        const FString DesiredVisibility = NormalizeStyleVisibilityToken(DesiredNode.Style);
        if (!CurrentVisibility.Equals(DesiredVisibility, ESearchCase::CaseSensitive))
        {
            Patch->SetStringField(TEXT("visibility"), DesiredVisibility);
        }
        if (IsSpecSizeBoxNode(DesiredNode))
        {
            AddSizeBoxStyleFieldsToPatchObject(DesiredNode.Style, Patch);
        }
        if (IsSpecBorderNode(DesiredNode))
        {
            if (!LinearColorsNearlyEqual(CurrentNode.Style.Background, DesiredNode.Style.Background))
            {
                Patch->SetStringField(TEXT("background"), ColorToHexString(DesiredNode.Style.Background));
            }
            if (!MarginsNearlyEqual(CurrentNode.Style.Padding, DesiredNode.Style.Padding))
            {
                Patch->SetObjectField(TEXT("padding"), MarginToPatchObject(DesiredNode.Style.Padding));
            }
        }
        if (IsSpecProgressBarNode(DesiredNode)
            && !LinearColorsNearlyEqual(CurrentNode.Style.Background, DesiredNode.Style.Background))
        {
            Patch->SetStringField(TEXT("background"), ColorToHexString(DesiredNode.Style.Background));
        }
        return Patch;
    }

    static bool CommonUIHasSingleStyleRefDelta(const FUISpecNode& CurrentNode, const FUISpecNode& DesiredNode)
    {
        if (!DesiredNode.bHasCommonUI || DesiredNode.CommonUI.StyleRefs.Num() != 1)
        {
            return false;
        }

        if (!CurrentNode.bHasCommonUI || CurrentNode.CommonUI.StyleRefs.Num() == 0)
        {
            return true;
        }

        return CurrentNode.CommonUI.StyleRefs[0] != DesiredNode.CommonUI.StyleRefs[0];
    }

    static bool CommonUIHasUnsupportedPatchDelta(const FUISpecNode& CurrentNode, const FUISpecNode& DesiredNode)
    {
        const bool bDiffers =
            CurrentNode.bHasCommonUI != DesiredNode.bHasCommonUI
            || !JsonObjectsEqual(CommonUIToJson(CurrentNode.CommonUI), CommonUIToJson(DesiredNode.CommonUI));
        if (!bDiffers)
        {
            return false;
        }

        const bool bOnlySingleStyleRefDelta =
            CommonUIHasSingleStyleRefDelta(CurrentNode, DesiredNode)
            && CurrentNode.CommonUI.InputLayer == DesiredNode.CommonUI.InputLayer
            && CurrentNode.CommonUI.InputMode == DesiredNode.CommonUI.InputMode
            && DesiredNode.CommonUI.StyleRefs.Num() == 1;
        return !bOnlySingleStyleRefDelta;
    }

    static TSharedPtr<FJsonObject> MakeApplyCommonStylePatchCandidate(const FUISpecNodeRef& DesiredRef)
    {
        if (!DesiredRef.Node || DesiredRef.Node->CommonUI.StyleRefs.Num() != 1)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(
            TEXT("apply_style_to_widget"),
            DesiredRef.Node->Id.ToString(),
            TEXT("desired_spec CommonUI styleRef differs from live WBP"));
        Patch->SetStringField(TEXT("style_asset"), DesiredRef.Node->CommonUI.StyleRefs[0].ToString());
        return Patch;
    }

    static TSharedPtr<FJsonObject> MakeSetImagePatchCandidate(
        const FUISpecNodeRef& DesiredRef,
        TArray<TSharedPtr<FJsonValue>>& Unsupported)
    {
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(TEXT("set_image"), DesiredRef.Node->Id.ToString(), TEXT("desired_spec Image brushPath differs from live WBP"));
        FString BrushError;
        if (!TryPopulateBrushPathAsImageParam(DesiredRef.Node->Content.BrushPath, Patch, BrushError))
        {
            TSharedPtr<FJsonObject> UnsupportedRow = MakeShared<FJsonObject>();
            UnsupportedRow->SetStringField(TEXT("widget_name"), DesiredRef.Node->Id.ToString());
            UnsupportedRow->SetStringField(TEXT("field"), TEXT("content.brushPath"));
            UnsupportedRow->SetStringField(TEXT("reason"), BrushError);
            Unsupported.Add(MakeShared<FJsonValueObject>(UnsupportedRow));
            return nullptr;
        }
        return Patch;
    }

    static TSharedPtr<FJsonObject> MakeSetBrushPatchCandidate(
        const FUISpecNodeRef& DesiredRef,
        TArray<TSharedPtr<FJsonValue>>& Unsupported)
    {
        const FString PropertyName = GetSpecBrushPropertyNameForNode(*DesiredRef.Node);
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(TEXT("set_brush"), DesiredRef.Node->Id.ToString(), TEXT("desired_spec non-Image brushPath differs from live WBP"));
        Patch->SetStringField(TEXT("property_name"), PropertyName);
        FString BrushError;
        if (!TryPopulateBrushPathAsBrushParam(DesiredRef.Node->Content.BrushPath, Patch, BrushError))
        {
            TSharedPtr<FJsonObject> UnsupportedRow = MakeShared<FJsonObject>();
            UnsupportedRow->SetStringField(TEXT("widget_name"), DesiredRef.Node->Id.ToString());
            UnsupportedRow->SetStringField(TEXT("field"), TEXT("content.brushPath"));
            UnsupportedRow->SetStringField(TEXT("reason"), BrushError);
            Unsupported.Add(MakeShared<FJsonValueObject>(UnsupportedRow));
            return nullptr;
        }
        return Patch;
    }

    static bool EffectShadowsNearlyEqual(
        const TArray<FUISpecEffectShadow>& A,
        const TArray<FUISpecEffectShadow>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }

        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            const FUISpecEffectShadow& Left = A[Index];
            const FUISpecEffectShadow& Right = B[Index];
            if (!Vector2DsNearlyEqual(Left.Offset, Right.Offset)
                || !FMath::IsNearlyEqual(Left.Blur, Right.Blur, 0.001f)
                || !FMath::IsNearlyEqual(Left.Spread, Right.Spread, 0.001f)
                || !LinearColorsNearlyEqual(Left.Color, Right.Color)
                || Left.bInset != Right.bInset)
            {
                return false;
            }
        }
        return true;
    }

    static bool EffectCornersHavePatchIntent(const FUISpecNode* CurrentNode, const FUISpecNode& Desired)
    {
        const bool bCurrentHasEffect = CurrentNode && CurrentNode->bHasEffect;
        if (bCurrentHasEffect)
        {
            return !Vector4sNearlyEqual(CurrentNode->Effect.CornerRadii, Desired.Effect.CornerRadii)
                || !FMath::IsNearlyEqual(CurrentNode->Effect.Smoothness, Desired.Effect.Smoothness, 0.001f);
        }

        return !Vector4sNearlyEqual(Desired.Effect.CornerRadii, FVector4(0.0, 0.0, 0.0, 0.0))
            || !FMath::IsNearlyEqual(Desired.Effect.Smoothness, 1.0f, 0.001f);
    }

    static bool EffectFillHasPatchIntent(const FUISpecNode* CurrentNode, const FUISpecNode& Desired)
    {
        const bool bCurrentHasEffect = CurrentNode && CurrentNode->bHasEffect;
        if (bCurrentHasEffect)
        {
            return !LinearColorsNearlyEqual(CurrentNode->Effect.SolidColor, Desired.Effect.SolidColor);
        }

        return !LinearColorsNearlyEqual(Desired.Effect.SolidColor, FLinearColor::White);
    }

    static bool EffectBackdropHasPatchIntent(const FUISpecNode* CurrentNode, const FUISpecNode& Desired)
    {
        const bool bCurrentHasEffect = CurrentNode && CurrentNode->bHasEffect;
        if (bCurrentHasEffect)
        {
            return !FMath::IsNearlyEqual(CurrentNode->Effect.BackdropBlurStrength, Desired.Effect.BackdropBlurStrength, 0.001f);
        }

        return !FMath::IsNearlyZero(Desired.Effect.BackdropBlurStrength, 0.001f);
    }

    static TArray<TSharedPtr<FJsonValue>> CornerRadiiToJsonArray(const FVector4& Radii)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Add(MakeShared<FJsonValueNumber>(Radii.X));
        Out.Add(MakeShared<FJsonValueNumber>(Radii.Y));
        Out.Add(MakeShared<FJsonValueNumber>(Radii.Z));
        Out.Add(MakeShared<FJsonValueNumber>(Radii.W));
        return Out;
    }

    static TArray<TSharedPtr<FJsonValue>> EffectShadowsToJsonArray(const TArray<FUISpecEffectShadow>& Shadows)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Shadows.Num());
        for (const FUISpecEffectShadow& Shadow : Shadows)
        {
            Out.Add(MakeShared<FJsonValueObject>(ShadowToJson(Shadow)));
        }
        return Out;
    }

    static TSharedPtr<FJsonObject> MakeEffectCornersPatchCandidate(const FUISpecNodeRef& DesiredRef)
    {
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(
            TEXT("set_effect_surface_corners"),
            DesiredRef.Node->Id.ToString(),
            TEXT("desired_spec EffectSurface corner radii or smoothness differs from live WBP"));
        Patch->SetArrayField(TEXT("corner_radii"), CornerRadiiToJsonArray(DesiredRef.Node->Effect.CornerRadii));
        Patch->SetNumberField(TEXT("smoothness"), DesiredRef.Node->Effect.Smoothness);
        return Patch;
    }

    static TSharedPtr<FJsonObject> MakeEffectFillPatchCandidate(const FUISpecNodeRef& DesiredRef)
    {
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(
            TEXT("set_effect_surface_fill"),
            DesiredRef.Node->Id.ToString(),
            TEXT("desired_spec EffectSurface solid fill differs from live WBP"));
        Patch->SetStringField(TEXT("mode"), TEXT("solid"));
        Patch->SetStringField(TEXT("color"), ColorToHexString(DesiredRef.Node->Effect.SolidColor));
        return Patch;
    }

    static TSharedPtr<FJsonObject> MakeEffectBackdropPatchCandidate(const FUISpecNodeRef& DesiredRef)
    {
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(
            TEXT("set_effect_surface_backdropBlur"),
            DesiredRef.Node->Id.ToString(),
            TEXT("desired_spec EffectSurface backdrop blur differs from live WBP"));
        Patch->SetNumberField(TEXT("strength"), DesiredRef.Node->Effect.BackdropBlurStrength);
        return Patch;
    }

    static TSharedPtr<FJsonObject> MakeEffectShadowPatchCandidate(
        const FUISpecNodeRef& DesiredRef,
        const FString& Op,
        const FString& Reason,
        const TArray<FUISpecEffectShadow>& Shadows)
    {
        TSharedPtr<FJsonObject> Patch = MakePatchCandidate(Op, DesiredRef.Node->Id.ToString(), Reason);
        Patch->SetArrayField(TEXT("layers"), EffectShadowsToJsonArray(Shadows));
        return Patch;
    }

    static void AddDiffUnsupportedField(
        TArray<TSharedPtr<FJsonValue>>& Unsupported,
        const FString& WidgetName,
        const FString& Field,
        const FString& Reason)
    {
        TSharedPtr<FJsonObject> UnsupportedRow = MakeShared<FJsonObject>();
        UnsupportedRow->SetStringField(TEXT("widget_name"), WidgetName);
        UnsupportedRow->SetStringField(TEXT("field"), Field);
        UnsupportedRow->SetStringField(TEXT("reason"), Reason);
        Unsupported.Add(MakeShared<FJsonValueObject>(UnsupportedRow));
    }

    static int32 AppendEffectPatchCandidates(
        const FUISpecNodeRef& DesiredRef,
        const FUISpecNode* CurrentNode,
        TArray<TSharedPtr<FJsonValue>>& PatchCandidates,
        TArray<TSharedPtr<FJsonValue>>& Unsupported,
        TSharedPtr<FJsonObject>& OutFirstPatch)
    {
        const FUISpecNode& Desired = *DesiredRef.Node;
        const FString WidgetName = Desired.Id.ToString();
        const bool bCurrentHasEffect = CurrentNode && CurrentNode->bHasEffect;
        if (!Desired.bHasEffect)
        {
            if (bCurrentHasEffect)
            {
                AddDiffUnsupportedField(
                    Unsupported,
                    WidgetName,
                    TEXT("effect"),
                    TEXT("Clearing an existing EffectSurface effect bag is not represented by FUISpec patch candidates; use explicit owner actions or a future confirm-gated clear op."));
            }
            return 0;
        }

        if (!IsSpecEffectSurfaceNode(Desired))
        {
            AddDiffUnsupportedField(
                Unsupported,
                WidgetName,
                TEXT("effect"),
                TEXT("Effect patch candidates are emitted only for EffectSurface widgets; other widget classes must use their existing owner actions explicitly."));
            return 0;
        }

        int32 Added = 0;
        auto AddPatch = [&](const TSharedPtr<FJsonObject>& Patch)
        {
            if (!Patch.IsValid())
            {
                return;
            }
            if (!OutFirstPatch.IsValid())
            {
                OutFirstPatch = Patch;
            }
            PatchCandidates.Add(MakeShared<FJsonValueObject>(Patch));
            ++Added;
        };

        if (EffectCornersHavePatchIntent(CurrentNode, Desired))
        {
            AddPatch(MakeEffectCornersPatchCandidate(DesiredRef));
        }
        if (EffectFillHasPatchIntent(CurrentNode, Desired))
        {
            AddPatch(MakeEffectFillPatchCandidate(DesiredRef));
        }
        if (EffectBackdropHasPatchIntent(CurrentNode, Desired))
        {
            AddPatch(MakeEffectBackdropPatchCandidate(DesiredRef));
        }
        if (!bCurrentHasEffect || !EffectShadowsNearlyEqual(CurrentNode->Effect.DropShadows, Desired.Effect.DropShadows))
        {
            if (Desired.Effect.DropShadows.Num() > 0 || (bCurrentHasEffect && CurrentNode->Effect.DropShadows.Num() > 0))
            {
                AddPatch(MakeEffectShadowPatchCandidate(
                    DesiredRef,
                    TEXT("set_effect_surface_dropShadow"),
                    TEXT("desired_spec EffectSurface drop-shadow stack differs from live WBP"),
                    Desired.Effect.DropShadows));
            }
        }
        if (!bCurrentHasEffect || !EffectShadowsNearlyEqual(CurrentNode->Effect.InnerShadows, Desired.Effect.InnerShadows))
        {
            if (Desired.Effect.InnerShadows.Num() > 0 || (bCurrentHasEffect && CurrentNode->Effect.InnerShadows.Num() > 0))
            {
                AddPatch(MakeEffectShadowPatchCandidate(
                    DesiredRef,
                    TEXT("set_effect_surface_innerShadow"),
                    TEXT("desired_spec EffectSurface inner-shadow stack differs from live WBP"),
                    Desired.Effect.InnerShadows));
            }
        }

        if (Added == 0)
        {
            AddDiffUnsupportedField(
                Unsupported,
                WidgetName,
                TEXT("effect"),
                TEXT("Effect bag differs only by default/no-op fields; no owner-action patch candidate was emitted to avoid enabling EffectSurface feature flags implicitly."));
        }
        return Added;
    }

    static FMonolithActionResult HandleDiffUISpec(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString AssetPath;
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
        }

        const FString CompareMode = CompareModeOrDefault(Params);
        if (CompareMode.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("compare_mode must be structural, properties, or full."), -32602);
        }

        const TSharedPtr<FJsonObject>* DesiredSpecObj = nullptr;
        if (!Params->TryGetObjectField(TEXT("desired_spec"), DesiredSpecObj) || !DesiredSpecObj || !DesiredSpecObj->IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing or invalid required param: desired_spec (must be a FUISpecDocument object)"), -32602);
        }

        FString RequestId;
        Params->TryGetStringField(TEXT("request_id"), RequestId);

        FUISpecDocument DesiredDoc;
        FUISpecValidationResult DesiredParseValidation;
        const bool bParsedDesired = ParseDocument(*DesiredSpecObj, DesiredDoc, DesiredParseValidation);
        FUISpecValidationResult DesiredValidation = bParsedDesired
            ? FUISpecValidator::Validate(DesiredDoc)
            : DesiredParseValidation;

        FUISpecSerializerInputs DumpInputs;
        DumpInputs.AssetPath = AssetPath;
        DumpInputs.RequestId = RequestId;
        const FUISpecSerializerResult DumpResult = FUISpecSerializer::Dump(DumpInputs);

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("bSuccess"), DumpResult.bSuccess && DesiredValidation.Errors.Num() == 0);
        Out->SetBoolField(TEXT("ok"), DumpResult.bSuccess && DesiredValidation.Errors.Num() == 0);
        Out->SetStringField(TEXT("schema_version"), TEXT("ui_spec_diff.v1"));
        Out->SetStringField(TEXT("asset_path"), AssetPath);
        Out->SetStringField(TEXT("compare_mode"), CompareMode);
        if (!RequestId.IsEmpty())
        {
            Out->SetStringField(TEXT("request_id"), RequestId);
        }

        TSharedPtr<FJsonObject> ValidationObj = MakeShared<FJsonObject>();
        ValidationObj->SetBoolField(TEXT("desired_spec_valid"), DesiredValidation.Errors.Num() == 0);
        ValidationObj->SetNumberField(TEXT("desired_error_count"), DesiredValidation.Errors.Num());
        ValidationObj->SetNumberField(TEXT("desired_warning_count"), DesiredValidation.Warnings.Num());
        ValidationObj->SetBoolField(TEXT("current_dump_success"), DumpResult.bSuccess);
        ValidationObj->SetStringField(TEXT("desired_llm_report"), DesiredValidation.ToLLMReport());
        Out->SetObjectField(TEXT("validation"), ValidationObj);
        SetValidationFindingArray(Out, TEXT("desired_errors"), DesiredValidation.Errors);
        SetValidationFindingArray(Out, TEXT("desired_warnings"), DesiredValidation.Warnings);

        if (!DumpResult.bSuccess || DesiredValidation.Errors.Num() > 0 || !DumpResult.Document.Root.IsValid() || !DesiredDoc.Root.IsValid())
        {
            if (DumpResult.Errors.Num() > 0)
            {
                SetValidationFindingArray(Out, TEXT("current_dump_errors"), DumpResult.Errors);
            }
            Out->SetStringField(TEXT("status"), TEXT("unavailable"));
            return FMonolithActionResult::Success(Out);
        }

        TArray<TSharedPtr<FJsonValue>> Unsupported;
        TMap<FName, FUISpecNodeRef> CurrentNodes;
        TMap<FName, FUISpecNodeRef> DesiredNodes;
        BuildNodeRefMap(DumpResult.Document.Root, NAME_None, TEXT("/current/rootWidget"), 0, CurrentNodes, Unsupported);
        BuildNodeRefMap(DesiredDoc.Root, NAME_None, TEXT("/desired/rootWidget"), 0, DesiredNodes, Unsupported);

        FMonolithActionResult BindingLoadError;
        if (UWidgetBlueprint* BindingWBP = MonolithUI::LoadWidgetBlueprint(AssetPath, BindingLoadError))
        {
            Out->SetObjectField(TEXT("graph_binding_preservation"), MakeGraphBindingPreservationReport(BindingWBP, CurrentNodes, DesiredNodes));
        }

        TArray<TSharedPtr<FJsonValue>> Changes;
        TArray<TSharedPtr<FJsonValue>> PatchCandidates;
        TMap<FString, int32> Counts;

        for (const TPair<FName, FUISpecNodeRef>& Pair : DesiredNodes)
        {
            const FName WidgetId = Pair.Key;
            const FUISpecNodeRef& DesiredRef = Pair.Value;
            const FUISpecNodeRef* CurrentRef = CurrentNodes.Find(WidgetId);
            if (!CurrentRef)
            {
                TSharedPtr<FJsonObject> Patch = MakeAddWidgetPatchCandidate(DesiredRef, DesiredRef.ParentId);
                Changes.Add(MakeShared<FJsonValueObject>(MakeDiffRow(
                    TEXT("add"), WidgetId, DesiredRef.Path, TEXT("node"),
                    TEXT("Widget exists in desired_spec but not in the current WBP."), Patch)));
                PatchCandidates.Add(MakeShared<FJsonValueObject>(Patch));
                IncrementCount(Counts, TEXT("add"));
                continue;
            }

            if (CurrentRef->Node->Type != DesiredRef.Node->Type)
            {
                TSharedPtr<FJsonObject> Patch = MakeReplaceWidgetPatchCandidate(*CurrentRef, DesiredRef, DesiredRef.ParentId, CurrentRef->Node->Type);
                TSharedPtr<FJsonObject> UnsupportedRow = MakeShared<FJsonObject>();
                UnsupportedRow->SetStringField(TEXT("widget_name"), WidgetId.ToString());
                UnsupportedRow->SetStringField(TEXT("field"), TEXT("type"));
                UnsupportedRow->SetStringField(TEXT("reason"), TEXT("Widget type replacement is not an implicit patch; use confirm-gated replace_widget if intended."));
                Unsupported.Add(MakeShared<FJsonValueObject>(UnsupportedRow));
                TSharedPtr<FJsonObject> Row = MakeDiffRow(
                    TEXT("replace_required"), WidgetId, DesiredRef.Path, TEXT("type"),
                    FString::Printf(TEXT("Widget type differs: current=%s desired=%s."),
                        *CurrentRef->Node->Type.ToString(), *DesiredRef.Node->Type.ToString()),
                    Patch);
                Row->SetObjectField(TEXT("replace_decomposition"), MakeReplaceDecompositionEvidence());
                Changes.Add(MakeShared<FJsonValueObject>(Row));
                PatchCandidates.Add(MakeShared<FJsonValueObject>(Patch));
                IncrementCount(Counts, TEXT("replace_required"));
            }

            if (CurrentRef->ParentId != DesiredRef.ParentId)
            {
                TSharedPtr<FJsonObject> Patch = MakeMoveWidgetPatchCandidate(WidgetId, DesiredRef.ParentId);
                Changes.Add(MakeShared<FJsonValueObject>(MakeDiffRow(
                    TEXT("move"), WidgetId, DesiredRef.Path, TEXT("parent"),
                    TEXT("Widget parent differs."), Patch)));
                PatchCandidates.Add(MakeShared<FJsonValueObject>(Patch));
                IncrementCount(Counts, TEXT("move"));
            }

            if (CompareMode != TEXT("structural"))
            {
                if (!JsonObjectsEqual(SlotToJson(CurrentRef->Node->Slot), SlotToJson(DesiredRef.Node->Slot)))
                {
                    TSharedPtr<FJsonObject> Patch = MakeSlotPatchCandidate(DesiredRef);
                    Changes.Add(MakeShared<FJsonValueObject>(MakeDiffRow(
                        TEXT("slot"), WidgetId, DesiredRef.Path, TEXT("slot"),
                        TEXT("Widget slot data differs."), Patch)));
                    PatchCandidates.Add(MakeShared<FJsonValueObject>(Patch));
                    IncrementCount(Counts, TEXT("slot"));
                }

                if (!JsonObjectsEqual(ContentToJson(CurrentRef->Node->Content), ContentToJson(DesiredRef.Node->Content)))
                {
                    TSharedPtr<FJsonObject> Patch;
                    if (ContentHasPatchableTextDelta(*CurrentRef->Node, *DesiredRef.Node))
                    {
                        Patch = MakeSetTextPatchCandidate(DesiredRef, CurrentRef->Node);
                        PatchCandidates.Add(MakeShared<FJsonValueObject>(Patch));
                    }
                    if (ContentHasPatchableImageDelta(*CurrentRef->Node, *DesiredRef.Node))
                    {
                        TSharedPtr<FJsonObject> ImagePatch = MakeSetImagePatchCandidate(DesiredRef, Unsupported);
                        if (ImagePatch.IsValid())
                        {
                            if (!Patch.IsValid())
                            {
                                Patch = ImagePatch;
                            }
                            PatchCandidates.Add(MakeShared<FJsonValueObject>(ImagePatch));
                        }
                    }
                    if (ContentHasPatchableBrushDelta(*CurrentRef->Node, *DesiredRef.Node))
                    {
                        TSharedPtr<FJsonObject> BrushPatch = MakeSetBrushPatchCandidate(DesiredRef, Unsupported);
                        if (BrushPatch.IsValid())
                        {
                            if (!Patch.IsValid())
                            {
                                Patch = BrushPatch;
                            }
                            PatchCandidates.Add(MakeShared<FJsonValueObject>(BrushPatch));
                        }
                    }
                    if (!Patch.IsValid())
                    {
                        TSharedPtr<FJsonObject> UnsupportedRow = MakeShared<FJsonObject>();
                        UnsupportedRow->SetStringField(TEXT("widget_name"), WidgetId.ToString());
                        UnsupportedRow->SetStringField(TEXT("field"), TEXT("content"));
                        UnsupportedRow->SetStringField(TEXT("reason"), TEXT("Only TextBlock text/font/color content, Image brushPath, and Border brushPath currently have automatic patch candidates; use explicit owner actions for placeholder changes."));
                        Unsupported.Add(MakeShared<FJsonValueObject>(UnsupportedRow));
                    }
                    Changes.Add(MakeShared<FJsonValueObject>(MakeDiffRow(
                        TEXT("content"), WidgetId, DesiredRef.Path, TEXT("content"),
                        TEXT("Widget content data differs."), Patch)));
                    IncrementCount(Counts, TEXT("content"));
                }

                if (!StylesNearlyEqualForPatchDiff(*CurrentRef->Node, *DesiredRef.Node))
                {
                    TSharedPtr<FJsonObject> Patch = MakeSetStylePatchCandidate(DesiredRef, *CurrentRef->Node);
                    if (Patch.IsValid())
                    {
                        PatchCandidates.Add(MakeShared<FJsonValueObject>(Patch));
                    }
                    if (StyleHasUnsupportedPatchDelta(*CurrentRef->Node, *DesiredRef.Node))
                    {
                        TSharedPtr<FJsonObject> UnsupportedRow = MakeShared<FJsonObject>();
                        UnsupportedRow->SetStringField(TEXT("widget_name"), WidgetId.ToString());
                        UnsupportedRow->SetStringField(TEXT("field"), TEXT("style"));
                        UnsupportedRow->SetStringField(TEXT("reason"), TEXT("Only common UWidget, SizeBox size constraints, Border background/padding, and ProgressBar fill-color style deltas are converted to automatic owner-action patch candidates; other style fields require explicit owner actions."));
                        Unsupported.Add(MakeShared<FJsonValueObject>(UnsupportedRow));
                    }
                    Changes.Add(MakeShared<FJsonValueObject>(MakeDiffRow(
                        TEXT("style"), WidgetId, DesiredRef.Path, TEXT("style"),
                        TEXT("Widget style data differs."), Patch)));
                    IncrementCount(Counts, TEXT("style"));
                }

                const bool bEffectDiffers =
                    CurrentRef->Node->bHasEffect != DesiredRef.Node->bHasEffect
                    || (CurrentRef->Node->bHasEffect && DesiredRef.Node->bHasEffect
                        && !JsonObjectsEqual(EffectToJson(CurrentRef->Node->Effect), EffectToJson(DesiredRef.Node->Effect)));
                if (bEffectDiffers)
                {
                    TSharedPtr<FJsonObject> Patch;
                    AppendEffectPatchCandidates(DesiredRef, CurrentRef->Node, PatchCandidates, Unsupported, Patch);
                    Changes.Add(MakeShared<FJsonValueObject>(MakeDiffRow(
                        TEXT("effect"), WidgetId, DesiredRef.Path, TEXT("effect"),
                        TEXT("Widget EffectSurface data differs."), Patch)));
                    IncrementCount(Counts, TEXT("effect"));
                }

                const bool bCommonUIDiffers =
                    CurrentRef->Node->bHasCommonUI != DesiredRef.Node->bHasCommonUI
                    || !JsonObjectsEqual(CommonUIToJson(CurrentRef->Node->CommonUI), CommonUIToJson(DesiredRef.Node->CommonUI));
                if (bCommonUIDiffers)
                {
                    TSharedPtr<FJsonObject> Patch;
                    if (CommonUIHasSingleStyleRefDelta(*CurrentRef->Node, *DesiredRef.Node))
                    {
                        Patch = MakeApplyCommonStylePatchCandidate(DesiredRef);
                        if (Patch.IsValid())
                        {
                            PatchCandidates.Add(MakeShared<FJsonValueObject>(Patch));
                        }
                    }
                    if (CommonUIHasUnsupportedPatchDelta(*CurrentRef->Node, *DesiredRef.Node))
                    {
                        TSharedPtr<FJsonObject> UnsupportedRow = MakeShared<FJsonObject>();
                        UnsupportedRow->SetStringField(TEXT("widget_name"), WidgetId.ToString());
                        UnsupportedRow->SetStringField(TEXT("field"), TEXT("commonUI"));
                        UnsupportedRow->SetStringField(TEXT("reason"), TEXT("Only a single CommonUI styleRef delta is converted to ui.apply_style_to_widget; input layer/mode changes, clearing styles, and multi-style refs require explicit owner workflows."));
                        Unsupported.Add(MakeShared<FJsonValueObject>(UnsupportedRow));
                    }
                    Changes.Add(MakeShared<FJsonValueObject>(MakeDiffRow(
                        TEXT("common_ui"), WidgetId, DesiredRef.Path, TEXT("commonUI"),
                        TEXT("Widget CommonUI metadata differs."), Patch)));
                    IncrementCount(Counts, TEXT("common_ui"));
                }
            }
        }

        for (const TPair<FName, FUISpecNodeRef>& Pair : CurrentNodes)
        {
            if (DesiredNodes.Contains(Pair.Key))
            {
                continue;
            }

            TSharedPtr<FJsonObject> Patch = MakePatchCandidate(TEXT("remove_widget"), Pair.Key.ToString(), TEXT("current WBP contains a widget absent from desired_spec"));
            Changes.Add(MakeShared<FJsonValueObject>(MakeDiffRow(
                TEXT("remove"), Pair.Key, Pair.Value.Path, TEXT("node"),
                TEXT("Widget exists in current WBP but not in desired_spec."), Patch)));
            PatchCandidates.Add(MakeShared<FJsonValueObject>(Patch));
            IncrementCount(Counts, TEXT("remove"));
        }

        const bool bChanged = Changes.Num() > 0;
        Out->SetStringField(TEXT("status"), bChanged ? TEXT("different") : TEXT("identical"));
        Out->SetBoolField(TEXT("changed"), bChanged);
        Out->SetNumberField(TEXT("change_count"), Changes.Num());
        Out->SetNumberField(TEXT("patch_candidate_count"), PatchCandidates.Num());
        Out->SetObjectField(TEXT("change_counts"), MakeCountsObject(Counts));
        Out->SetArrayField(TEXT("changes"), Changes);
        Out->SetArrayField(TEXT("patch_candidates"), PatchCandidates);
        Out->SetArrayField(TEXT("unsupported_fields"), Unsupported);
        return FMonolithActionResult::Success(Out);
    }

    struct FUISpecPatchStep
    {
        FString Type;
        FString Namespace;
        FString Action;
        int32 SourceIndex = INDEX_NONE;
        TSharedPtr<FJsonObject> Params;
        bool bMutating = true;
    };

    static void CopyPatchFieldAlias(
        const TSharedPtr<FJsonObject>& Source,
        const TSharedPtr<FJsonObject>& Destination,
        const FString& DestinationField,
        const TCHAR* A,
        const TCHAR* B = nullptr,
        const TCHAR* C = nullptr)
    {
        if (!Source.IsValid() || !Destination.IsValid() || Destination->HasField(DestinationField))
        {
            return;
        }

        const TCHAR* Candidates[3] = { A, B, C };
        for (const TCHAR* Candidate : Candidates)
        {
            if (!Candidate)
            {
                continue;
            }
            if (TSharedPtr<FJsonValue> Value = Source->TryGetField(Candidate))
            {
                Destination->SetField(DestinationField, Value);
                return;
            }
        }
    }

    static void CopySlotPatchFields(
        const TSharedPtr<FJsonObject>& Source,
        const TSharedPtr<FJsonObject>& Destination)
    {
        const TSharedPtr<FJsonObject>* Slot = nullptr;
        const TSharedPtr<FJsonObject>& SlotSource =
            Source.IsValid() && Source->TryGetObjectField(TEXT("slot"), Slot) && Slot && Slot->IsValid()
                ? *Slot
                : Source;

        CopyPatchFieldAlias(SlotSource, Destination, TEXT("anchor_preset"), TEXT("anchor_preset"), TEXT("anchorPreset"));
        CopyPatchFieldAlias(SlotSource, Destination, TEXT("position"), TEXT("position"));
        CopyPatchFieldAlias(SlotSource, Destination, TEXT("size"), TEXT("size"));
        CopyPatchFieldAlias(SlotSource, Destination, TEXT("alignment"), TEXT("alignment"));
        CopyPatchFieldAlias(SlotSource, Destination, TEXT("padding"), TEXT("padding"));
        CopyPatchFieldAlias(SlotSource, Destination, TEXT("auto_size"), TEXT("auto_size"), TEXT("autoSize"));
        CopyPatchFieldAlias(SlotSource, Destination, TEXT("z_order"), TEXT("z_order"), TEXT("zOrder"));
        CopyPatchFieldAlias(SlotSource, Destination, TEXT("h_align"), TEXT("h_align"), TEXT("hAlign"));
        CopyPatchFieldAlias(SlotSource, Destination, TEXT("v_align"), TEXT("v_align"), TEXT("vAlign"));
    }

    static TSharedPtr<FJsonObject> MakePatchStepParamsWithAsset(const TSharedPtr<FJsonObject>& Entry, const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Params = CloneJsonObject(Entry);
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->RemoveField(TEXT("op"));
        Params->RemoveField(TEXT("action"));
        Params->RemoveField(TEXT("reason"));
        return Params;
    }

    static void AddPatchStep(
        TArray<FUISpecPatchStep>& Steps,
        const FString& Type,
        const FString& Namespace,
        const FString& Action,
        int32 SourceIndex,
        const TSharedPtr<FJsonObject>& Params,
        bool bMutating = true)
    {
        FUISpecPatchStep Step;
        Step.Type = Type;
        Step.Namespace = Namespace;
        Step.Action = Action;
        Step.SourceIndex = SourceIndex;
        Step.Params = Params;
        Step.bMutating = bMutating;
        Steps.Add(MoveTemp(Step));
    }

    static void AddUnsupportedPatchField(
        TArray<TSharedPtr<FJsonValue>>& Unsupported,
        int32 Index,
        const FString& Op,
        const FString& Reason,
        const FString& Field = FString(),
        const FString& WidgetName = FString(),
        const FString& SlotType = FString(),
        const TArray<FString>* ValidOptions = nullptr)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("source_index"), Index);
        Row->SetStringField(TEXT("op"), Op);
        Row->SetStringField(TEXT("reason"), Reason);
        if (!Field.IsEmpty())
        {
            Row->SetStringField(TEXT("field"), Field);
        }
        if (!WidgetName.IsEmpty())
        {
            Row->SetStringField(TEXT("widget_name"), WidgetName);
        }
        if (!SlotType.IsEmpty())
        {
            Row->SetStringField(TEXT("slot_type"), SlotType);
        }
        if (ValidOptions)
        {
            TArray<TSharedPtr<FJsonValue>> Options;
            Options.Reserve(ValidOptions->Num());
            for (const FString& Option : *ValidOptions)
            {
                Options.Add(MakeShared<FJsonValueString>(Option));
            }
            Row->SetArrayField(TEXT("valid_options"), Options);
        }
        Unsupported.Add(MakeShared<FJsonValueObject>(Row));
    }

    static bool EnsurePatchWidgetName(
        const FString& WidgetName,
        int32 Index,
        const FString& Op,
        TArray<TSharedPtr<FJsonValue>>& OutUnsupported)
    {
        if (!WidgetName.IsEmpty())
        {
            return true;
        }

        AddUnsupportedPatchField(
            OutUnsupported,
            Index,
            Op,
            TEXT("Patch op requires widget_name/name/id."));
        return false;
    }

    static void AddSetWidgetPropertyPatchStep(
        const FString& AssetPath,
        const FString& WidgetName,
        const FString& PropertyName,
        const TSharedPtr<FJsonValue>& Value,
        int32 Index,
        bool bCompileEachMutation,
        TArray<FUISpecPatchStep>& OutSteps)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), WidgetName);
        Params->SetStringField(TEXT("property_name"), PropertyName);
        Params->SetField(TEXT("value"), Value);
        SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
        AddPatchStep(OutSteps, TEXT("set_widget_property"), TEXT("ui"), TEXT("set_widget_property"), Index, Params);
    }

    static bool IsSupportedStylePatchField(const FString& Field)
    {
        return Field.Equals(TEXT("opacity"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("render_opacity"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("renderOpacity"), ESearchCase::CaseSensitive)
            || Field.Equals(TEXT("visibility"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("width"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("widthOverride"), ESearchCase::CaseSensitive)
            || Field.Equals(TEXT("height"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("heightOverride"), ESearchCase::CaseSensitive)
            || Field.Equals(TEXT("minDesiredWidth"), ESearchCase::CaseSensitive)
            || Field.Equals(TEXT("min_desired_width"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("minDesiredHeight"), ESearchCase::CaseSensitive)
            || Field.Equals(TEXT("min_desired_height"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("maxDesiredWidth"), ESearchCase::CaseSensitive)
            || Field.Equals(TEXT("max_desired_width"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("maxDesiredHeight"), ESearchCase::CaseSensitive)
            || Field.Equals(TEXT("max_desired_height"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("padding"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("background"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("brushColor"), ESearchCase::CaseSensitive)
            || Field.Equals(TEXT("brush_color"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("fillColorAndOpacity"), ESearchCase::CaseSensitive)
            || Field.Equals(TEXT("fill_color_and_opacity"), ESearchCase::IgnoreCase);
    }

    static bool IsPatchEnvelopeField(const FString& Field)
    {
        return Field.Equals(TEXT("op"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("action"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("widget_name"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("widget"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("name"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("id"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("widget_class"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("widget_type"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("type"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("reason"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("asset_path"), ESearchCase::IgnoreCase)
            || Field.Equals(TEXT("style"), ESearchCase::IgnoreCase);
    }

    static void AddStylePatchStepsFromStyleObject(
        const FString& AssetPath,
        const FString& WidgetName,
        const FString& WidgetClassHint,
        const TSharedPtr<FJsonObject>& StyleObject,
        int32 Index,
        bool bCompileEachMutation,
        TArray<FUISpecPatchStep>& OutSteps,
        TArray<TSharedPtr<FJsonValue>>& OutUnsupported,
        const FString& OpForDiagnostics)
    {
        if (!StyleObject.IsValid())
        {
            return;
        }
        if (!EnsurePatchWidgetName(WidgetName, Index, OpForDiagnostics, OutUnsupported))
        {
            return;
        }

        bool bAddedSupportedStep = false;
        double Opacity = 0.0;
        if (StyleObject->TryGetNumberField(TEXT("opacity"), Opacity)
            || StyleObject->TryGetNumberField(TEXT("render_opacity"), Opacity)
            || StyleObject->TryGetNumberField(TEXT("renderOpacity"), Opacity))
        {
            AddSetWidgetPropertyPatchStep(
                AssetPath,
                WidgetName,
                TEXT("RenderOpacity"),
                MakeShared<FJsonValueNumber>(FMath::Clamp(Opacity, 0.0, 1.0)),
                Index,
                bCompileEachMutation,
                OutSteps);
            bAddedSupportedStep = true;
        }

        FString Visibility;
        if (StyleObject->TryGetStringField(TEXT("visibility"), Visibility) && !Visibility.IsEmpty())
        {
            AddSetWidgetPropertyPatchStep(
                AssetPath,
                WidgetName,
                TEXT("Visibility"),
                MakeShared<FJsonValueString>(Visibility),
                Index,
                bCompileEachMutation,
                OutSteps);
            bAddedSupportedStep = true;
        }

        auto TryAddNumberStyleProperty = [&](
            const TCHAR* CanonicalField,
            const TCHAR* AliasField,
            const TCHAR* PropertyName)
        {
            double Number = 0.0;
            if (StyleObject->TryGetNumberField(CanonicalField, Number)
                || (AliasField && StyleObject->TryGetNumberField(AliasField, Number)))
            {
                AddSetWidgetPropertyPatchStep(
                    AssetPath,
                    WidgetName,
                    PropertyName,
                    MakeShared<FJsonValueNumber>(Number),
                    Index,
                    bCompileEachMutation,
                    OutSteps);
                bAddedSupportedStep = true;
            }
        };

        TryAddNumberStyleProperty(TEXT("width"), TEXT("widthOverride"), TEXT("WidthOverride"));
        TryAddNumberStyleProperty(TEXT("height"), TEXT("heightOverride"), TEXT("HeightOverride"));
        TryAddNumberStyleProperty(TEXT("minDesiredWidth"), TEXT("min_desired_width"), TEXT("MinDesiredWidth"));
        TryAddNumberStyleProperty(TEXT("minDesiredHeight"), TEXT("min_desired_height"), TEXT("MinDesiredHeight"));
        TryAddNumberStyleProperty(TEXT("maxDesiredWidth"), TEXT("max_desired_width"), TEXT("MaxDesiredWidth"));
        TryAddNumberStyleProperty(TEXT("maxDesiredHeight"), TEXT("max_desired_height"), TEXT("MaxDesiredHeight"));

        TSharedPtr<FJsonValue> PaddingValue = StyleObject->TryGetField(TEXT("padding"));
        if (PaddingValue.IsValid())
        {
            AddSetWidgetPropertyPatchStep(
                AssetPath,
                WidgetName,
                TEXT("Padding"),
                PaddingValue,
                Index,
                bCompileEachMutation,
                OutSteps);
            bAddedSupportedStep = true;
        }

        auto TryGetStyleColorValue = [&](const TCHAR* CanonicalField, const TCHAR* AliasField) -> TSharedPtr<FJsonValue>
        {
            if (TSharedPtr<FJsonValue> Value = StyleObject->TryGetField(CanonicalField))
            {
                return Value;
            }
            if (AliasField)
            {
                return StyleObject->TryGetField(AliasField);
            }
            return nullptr;
        };

        if (TSharedPtr<FJsonValue> BrushColorValue = TryGetStyleColorValue(TEXT("brushColor"), TEXT("brush_color")))
        {
            AddSetWidgetPropertyPatchStep(
                AssetPath,
                WidgetName,
                TEXT("BrushColor"),
                BrushColorValue,
                Index,
                bCompileEachMutation,
                OutSteps);
            bAddedSupportedStep = true;
        }

        if (TSharedPtr<FJsonValue> FillColorValue = TryGetStyleColorValue(TEXT("fillColorAndOpacity"), TEXT("fill_color_and_opacity")))
        {
            AddSetWidgetPropertyPatchStep(
                AssetPath,
                WidgetName,
                TEXT("FillColorAndOpacity"),
                FillColorValue,
                Index,
                bCompileEachMutation,
                OutSteps);
            bAddedSupportedStep = true;
        }

        if (TSharedPtr<FJsonValue> BackgroundValue = StyleObject->TryGetField(TEXT("background")))
        {
            if (WidgetClassHint.Equals(TEXT("Border"), ESearchCase::IgnoreCase)
                || WidgetClassHint.Equals(TEXT("RoundedBorder"), ESearchCase::IgnoreCase))
            {
                AddSetWidgetPropertyPatchStep(
                    AssetPath,
                    WidgetName,
                    TEXT("BrushColor"),
                    BackgroundValue,
                    Index,
                    bCompileEachMutation,
                    OutSteps);
                bAddedSupportedStep = true;
            }
            else if (WidgetClassHint.Equals(TEXT("ProgressBar"), ESearchCase::IgnoreCase))
            {
                AddSetWidgetPropertyPatchStep(
                    AssetPath,
                    WidgetName,
                    TEXT("FillColorAndOpacity"),
                    BackgroundValue,
                    Index,
                    bCompileEachMutation,
                    OutSteps);
                bAddedSupportedStep = true;
            }
            else
            {
                AddUnsupportedPatchField(
                    OutUnsupported,
                    Index,
                    OpForDiagnostics,
                    TEXT("style.background requires widget_class Border/RoundedBorder/ProgressBar, or explicit brushColor/fillColorAndOpacity."),
                    TEXT("style.background"),
                    WidgetName);
            }
        }

        for (const auto& Pair : StyleObject->Values)
        {
            const FString StyleField(Pair.Key.Len(), *Pair.Key);
            if (!IsSupportedStylePatchField(StyleField) && !IsPatchEnvelopeField(StyleField))
            {
                AddUnsupportedPatchField(
                    OutUnsupported,
                    Index,
                    OpForDiagnostics,
                    TEXT("Only proof-gated common/SizeBox/Border/ProgressBar style fields are routed automatically by ui.apply_ui_spec_patch; use explicit owner actions for other style fields."),
                    FString::Printf(TEXT("style.%s"), *StyleField),
                    WidgetName);
            }
        }

        if (!bAddedSupportedStep)
        {
            AddUnsupportedPatchField(
                OutUnsupported,
                Index,
                OpForDiagnostics,
                TEXT("set_style patch op requires a supported common, SizeBox, Border, or ProgressBar style field."),
                TEXT("style"),
                WidgetName);
        }
    }

    static TSharedPtr<FJsonObject> MakeEffectStepParamsBase(
        const FString& AssetPath,
        const FString& WidgetName,
        bool bCompileEachMutation)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), WidgetName);
        SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
        return Params;
    }

    static bool EnsureEffectWidgetName(
        const FString& WidgetName,
        int32 Index,
        const FString& Op,
        TArray<TSharedPtr<FJsonValue>>& OutUnsupported)
    {
        if (!WidgetName.IsEmpty())
        {
            return true;
        }

        AddUnsupportedPatchField(
            OutUnsupported,
            Index,
            Op,
            TEXT("EffectSurface patch op requires widget_name/name/id."));
        return false;
    }

    static void AddEffectPatchStepsFromEffectObject(
        const FString& AssetPath,
        const FString& WidgetName,
        const TSharedPtr<FJsonObject>& EffectObject,
        int32 Index,
        bool bCompileEachMutation,
        TArray<FUISpecPatchStep>& OutSteps,
        TArray<TSharedPtr<FJsonValue>>& OutUnsupported)
    {
        if (!EffectObject.IsValid())
        {
            return;
        }
        if (!EnsureEffectWidgetName(WidgetName, Index, TEXT("effect"), OutUnsupported))
        {
            return;
        }

        const bool bHasCornerIntent =
            EffectObject->HasField(TEXT("cornerRadii"))
            || EffectObject->HasField(TEXT("corner_radii"))
            || EffectObject->HasField(TEXT("smoothness"));
        if (bHasCornerIntent)
        {
            TSharedPtr<FJsonObject> Params = MakeEffectStepParamsBase(AssetPath, WidgetName, bCompileEachMutation);
            CopyPatchFieldAlias(EffectObject, Params, TEXT("corner_radii"), TEXT("corner_radii"), TEXT("cornerRadii"));
            CopyPatchFieldAlias(EffectObject, Params, TEXT("smoothness"), TEXT("smoothness"));
            if (!Params->HasField(TEXT("corner_radii")))
            {
                AddUnsupportedPatchField(
                    OutUnsupported,
                    Index,
                    TEXT("effect.cornerRadii"),
                    TEXT("effect.smoothness cannot be routed alone because ui.set_effect_surface_corners requires corner_radii."));
            }
            else
            {
                AddPatchStep(OutSteps, TEXT("set_effect_surface_corners"), TEXT("ui"), TEXT("set_effect_surface_corners"), Index, Params);
            }
        }

        if (EffectObject->HasField(TEXT("solidColor")) || EffectObject->HasField(TEXT("color")))
        {
            TSharedPtr<FJsonObject> Params = MakeEffectStepParamsBase(AssetPath, WidgetName, bCompileEachMutation);
            Params->SetStringField(TEXT("mode"), TEXT("solid"));
            CopyPatchFieldAlias(EffectObject, Params, TEXT("color"), TEXT("color"), TEXT("solidColor"));
            AddPatchStep(OutSteps, TEXT("set_effect_surface_fill"), TEXT("ui"), TEXT("set_effect_surface_fill"), Index, Params);
        }

        if (EffectObject->HasField(TEXT("backdropBlurStrength")) || EffectObject->HasField(TEXT("strength")))
        {
            TSharedPtr<FJsonObject> Params = MakeEffectStepParamsBase(AssetPath, WidgetName, bCompileEachMutation);
            CopyPatchFieldAlias(EffectObject, Params, TEXT("strength"), TEXT("strength"), TEXT("backdropBlurStrength"));
            AddPatchStep(OutSteps, TEXT("set_effect_surface_backdropBlur"), TEXT("ui"), TEXT("set_effect_surface_backdropBlur"), Index, Params);
        }

        if (EffectObject->HasField(TEXT("dropShadows")) || EffectObject->HasField(TEXT("drop_shadows")))
        {
            TSharedPtr<FJsonObject> Params = MakeEffectStepParamsBase(AssetPath, WidgetName, bCompileEachMutation);
            CopyPatchFieldAlias(EffectObject, Params, TEXT("layers"), TEXT("dropShadows"), TEXT("drop_shadows"));
            AddPatchStep(OutSteps, TEXT("set_effect_surface_dropShadow"), TEXT("ui"), TEXT("set_effect_surface_dropShadow"), Index, Params);
        }

        if (EffectObject->HasField(TEXT("innerShadows")) || EffectObject->HasField(TEXT("inner_shadows")))
        {
            TSharedPtr<FJsonObject> Params = MakeEffectStepParamsBase(AssetPath, WidgetName, bCompileEachMutation);
            CopyPatchFieldAlias(EffectObject, Params, TEXT("layers"), TEXT("innerShadows"), TEXT("inner_shadows"));
            AddPatchStep(OutSteps, TEXT("set_effect_surface_innerShadow"), TEXT("ui"), TEXT("set_effect_surface_innerShadow"), Index, Params);
        }
    }

    static bool RouteDirectEffectPatchOp(
        const FString& AssetPath,
        const TSharedPtr<FJsonObject>& Entry,
        const FString& Op,
        int32 Index,
        bool bCompileEachMutation,
        TArray<FUISpecPatchStep>& OutSteps,
        TArray<TSharedPtr<FJsonValue>>& OutUnsupported)
    {
        auto MakeParams = [&]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
            SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name"), TEXT("id")));
            SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
            return Params;
        };

        if (Op == TEXT("set_effect_surface_corners"))
        {
            TSharedPtr<FJsonObject> Params = MakeParams();
            if (!EnsureEffectWidgetName(GetFirstStringField(Params, TEXT("widget_name")), Index, Op, OutUnsupported))
            {
                return true;
            }
            CopyPatchFieldAlias(Entry, Params, TEXT("corner_radii"), TEXT("corner_radii"), TEXT("cornerRadii"));
            if (!Params->HasField(TEXT("corner_radii")))
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("ui.set_effect_surface_corners requires corner_radii/cornerRadii."));
                return true;
            }
            AddPatchStep(OutSteps, TEXT("set_effect_surface_corners"), TEXT("ui"), TEXT("set_effect_surface_corners"), Index, Params);
            return true;
        }

        if (Op == TEXT("set_effect_surface_fill"))
        {
            TSharedPtr<FJsonObject> Params = MakeParams();
            if (!EnsureEffectWidgetName(GetFirstStringField(Params, TEXT("widget_name")), Index, Op, OutUnsupported))
            {
                return true;
            }
            CopyPatchFieldAlias(Entry, Params, TEXT("color"), TEXT("color"), TEXT("solidColor"));
            if (!Params->HasField(TEXT("mode")) && Params->HasField(TEXT("color")))
            {
                Params->SetStringField(TEXT("mode"), TEXT("solid"));
            }
            CopyPatchFieldAlias(Entry, Params, TEXT("radial_center"), TEXT("radial_center"), TEXT("radialCenter"));
            if (!Params->HasField(TEXT("mode")))
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("ui.set_effect_surface_fill requires mode, or color/solidColor for implicit solid mode."));
                return true;
            }
            AddPatchStep(OutSteps, TEXT("set_effect_surface_fill"), TEXT("ui"), TEXT("set_effect_surface_fill"), Index, Params);
            return true;
        }

        if (Op == TEXT("set_effect_surface_dropshadow"))
        {
            TSharedPtr<FJsonObject> Params = MakeParams();
            if (!EnsureEffectWidgetName(GetFirstStringField(Params, TEXT("widget_name")), Index, Op, OutUnsupported))
            {
                return true;
            }
            CopyPatchFieldAlias(Entry, Params, TEXT("layers"), TEXT("layers"), TEXT("dropShadows"), TEXT("drop_shadows"));
            if (!Params->HasField(TEXT("layers")))
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("ui.set_effect_surface_dropShadow requires layers/dropShadows."));
                return true;
            }
            AddPatchStep(OutSteps, TEXT("set_effect_surface_dropShadow"), TEXT("ui"), TEXT("set_effect_surface_dropShadow"), Index, Params);
            return true;
        }

        if (Op == TEXT("set_effect_surface_innershadow"))
        {
            TSharedPtr<FJsonObject> Params = MakeParams();
            if (!EnsureEffectWidgetName(GetFirstStringField(Params, TEXT("widget_name")), Index, Op, OutUnsupported))
            {
                return true;
            }
            CopyPatchFieldAlias(Entry, Params, TEXT("layers"), TEXT("layers"), TEXT("innerShadows"), TEXT("inner_shadows"));
            if (!Params->HasField(TEXT("layers")))
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("ui.set_effect_surface_innerShadow requires layers/innerShadows."));
                return true;
            }
            AddPatchStep(OutSteps, TEXT("set_effect_surface_innerShadow"), TEXT("ui"), TEXT("set_effect_surface_innerShadow"), Index, Params);
            return true;
        }

        if (Op == TEXT("set_effect_surface_backdropblur"))
        {
            TSharedPtr<FJsonObject> Params = MakeParams();
            if (!EnsureEffectWidgetName(GetFirstStringField(Params, TEXT("widget_name")), Index, Op, OutUnsupported))
            {
                return true;
            }
            CopyPatchFieldAlias(Entry, Params, TEXT("strength"), TEXT("strength"), TEXT("backdropBlurStrength"));
            if (!Params->HasField(TEXT("strength")))
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("ui.set_effect_surface_backdropBlur requires strength/backdropBlurStrength."));
                return true;
            }
            AddPatchStep(OutSteps, TEXT("set_effect_surface_backdropBlur"), TEXT("ui"), TEXT("set_effect_surface_backdropBlur"), Index, Params);
            return true;
        }

        if (Op == TEXT("set_effect_surface_border"))
        {
            TSharedPtr<FJsonObject> Params = MakeParams();
            if (!EnsureEffectWidgetName(GetFirstStringField(Params, TEXT("widget_name")), Index, Op, OutUnsupported))
            {
                return true;
            }
            CopyPatchFieldAlias(Entry, Params, TEXT("glow_color"), TEXT("glow_color"), TEXT("glowColor"));
            if (!Params->HasField(TEXT("width")))
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("ui.set_effect_surface_border requires width."));
                return true;
            }
            AddPatchStep(OutSteps, TEXT("set_effect_surface_border"), TEXT("ui"), TEXT("set_effect_surface_border"), Index, Params);
            return true;
        }

        if (Op == TEXT("set_effect_surface_glow"))
        {
            TSharedPtr<FJsonObject> Params = MakeParams();
            if (!EnsureEffectWidgetName(GetFirstStringField(Params, TEXT("widget_name")), Index, Op, OutUnsupported))
            {
                return true;
            }
            CopyPatchFieldAlias(Entry, Params, TEXT("inner_outer_mix"), TEXT("inner_outer_mix"), TEXT("innerOuterMix"));
            if (!Params->HasField(TEXT("radius")))
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("ui.set_effect_surface_glow requires radius."));
                return true;
            }
            AddPatchStep(OutSteps, TEXT("set_effect_surface_glow"), TEXT("ui"), TEXT("set_effect_surface_glow"), Index, Params);
            return true;
        }

        if (Op == TEXT("set_effect_surface_filter"))
        {
            TSharedPtr<FJsonObject> Params = MakeParams();
            if (!EnsureEffectWidgetName(GetFirstStringField(Params, TEXT("widget_name")), Index, Op, OutUnsupported))
            {
                return true;
            }
            if (!Params->HasField(TEXT("saturation")) && !Params->HasField(TEXT("brightness")) && !Params->HasField(TEXT("contrast")))
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("ui.set_effect_surface_filter requires at least one of saturation/brightness/contrast."));
                return true;
            }
            AddPatchStep(OutSteps, TEXT("set_effect_surface_filter"), TEXT("ui"), TEXT("set_effect_surface_filter"), Index, Params);
            return true;
        }

        if (Op == TEXT("set_effect_surface_insethighlight"))
        {
            TSharedPtr<FJsonObject> Params = MakeParams();
            if (!EnsureEffectWidgetName(GetFirstStringField(Params, TEXT("widget_name")), Index, Op, OutUnsupported))
            {
                return true;
            }
            CopyPatchFieldAlias(Entry, Params, TEXT("edge_mask"), TEXT("edge_mask"), TEXT("edgeMask"));
            if (!Params->HasField(TEXT("offset"))
                && !Params->HasField(TEXT("blur"))
                && !Params->HasField(TEXT("color"))
                && !Params->HasField(TEXT("intensity"))
                && !Params->HasField(TEXT("edge_mask")))
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("ui.set_effect_surface_insetHighlight requires at least one of offset/blur/color/intensity/edge_mask."));
                return true;
            }
            AddPatchStep(OutSteps, TEXT("set_effect_surface_insetHighlight"), TEXT("ui"), TEXT("set_effect_surface_insetHighlight"), Index, Params);
            return true;
        }

        return false;
    }

    static bool HasAnySlotPropertyField(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return false;
        }

        static const TCHAR* Fields[] = {
            TEXT("anchors"),
            TEXT("offsets"),
            TEXT("position"),
            TEXT("size"),
            TEXT("alignment"),
            TEXT("padding"),
            TEXT("auto_size"),
            TEXT("z_order"),
            TEXT("h_align"),
            TEXT("v_align")
        };
        for (const TCHAR* Field : Fields)
        {
            if (Params->HasField(Field))
            {
                return true;
            }
        }
        return false;
    }

    static void CollectPresentSlotFields(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutFields)
    {
        OutFields.Reset();
        if (!Params.IsValid())
        {
            return;
        }

        static const TCHAR* Fields[] = {
            TEXT("anchors"),
            TEXT("offsets"),
            TEXT("position"),
            TEXT("size"),
            TEXT("alignment"),
            TEXT("padding"),
            TEXT("auto_size"),
            TEXT("z_order"),
            TEXT("h_align"),
            TEXT("v_align")
        };
        for (const TCHAR* Field : Fields)
        {
            if (Params->HasField(Field))
            {
                OutFields.Add(Field);
            }
        }
    }

    static void GetSupportedSetSlotFieldsForSlot(UPanelSlot* Slot, TArray<FString>& OutFields, FString& OutSlotType)
    {
        OutFields.Reset();
        OutSlotType = Slot ? Slot->GetClass()->GetName() : TEXT("none");

        if (Cast<UCanvasPanelSlot>(Slot))
        {
            OutFields = {
                TEXT("anchors"),
                TEXT("offsets"),
                TEXT("position"),
                TEXT("size"),
                TEXT("alignment"),
                TEXT("auto_size"),
                TEXT("z_order")
            };
            return;
        }

        if (Cast<UVerticalBoxSlot>(Slot) || Cast<UHorizontalBoxSlot>(Slot) || Cast<UOverlaySlot>(Slot))
        {
            OutFields = {
                TEXT("padding"),
                TEXT("h_align"),
                TEXT("v_align")
            };
            return;
        }
    }

    static void PreflightSetSlotPatchStep(
        UWidgetBlueprint* WBP,
        const FUISpecPatchStep& Step,
        TArray<TSharedPtr<FJsonValue>>& Unsupported)
    {
        if (!WBP || !WBP->WidgetTree || !Step.Params.IsValid())
        {
            return;
        }

        const FString WidgetName = GetFirstStringField(Step.Params, TEXT("widget_name"), TEXT("widget"), TEXT("name"));
        if (WidgetName.IsEmpty())
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                TEXT("Slot patch step is missing widget_name."),
                TEXT("widget_name"));
            return;
        }

        UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
        if (!Widget)
        {
            // The patch may create this widget earlier in the same plan; defer
            // existence checks to the owner action instead of blocking dry-run.
            return;
        }

        UPanelSlot* Slot = Widget->Slot;
        if (!Slot)
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                FString::Printf(TEXT("Widget '%s' has no slot; root widgets cannot receive slot patch fields."), *WidgetName),
                TEXT("slot"),
                WidgetName,
                TEXT("none"));
            return;
        }

        if (Step.Action == TEXT("set_anchor_preset"))
        {
            if (!Cast<UCanvasPanelSlot>(Slot))
            {
                TArray<FString> ValidOptions = { TEXT("CanvasPanelSlot") };
                AddUnsupportedPatchField(
                    Unsupported,
                    Step.SourceIndex,
                    Step.Type,
                    TEXT("anchorPreset patches require a CanvasPanelSlot; route the widget under a CanvasPanel or remove the anchor preset from the patch."),
                    TEXT("anchor_preset"),
                    WidgetName,
                    Slot->GetClass()->GetName(),
                    &ValidOptions);
            }
            return;
        }

        if (Step.Action != TEXT("set_slot_property"))
        {
            return;
        }

        TArray<FString> PresentFields;
        CollectPresentSlotFields(Step.Params, PresentFields);
        if (PresentFields.Num() == 0)
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                TEXT("set_slot_property patch step has no slot fields after alias normalization."),
                TEXT("slot"),
                WidgetName,
                Slot->GetClass()->GetName());
            return;
        }

        TArray<FString> SupportedFields;
        FString SlotType;
        GetSupportedSetSlotFieldsForSlot(Slot, SupportedFields, SlotType);

        for (const FString& Field : PresentFields)
        {
            if (!SupportedFields.Contains(Field))
            {
                AddUnsupportedPatchField(
                    Unsupported,
                    Step.SourceIndex,
                    Step.Type,
                    FString::Printf(TEXT("Field '%s' is not supported by ui.set_slot_property for %s."), *Field, *SlotType),
                    Field,
                    WidgetName,
                    SlotType,
                    &SupportedFields);
            }
        }
    }

    static void PreflightAddReplacementWidgetStep(
        UWidgetBlueprint* WBP,
        const FUISpecPatchStep& Step,
        TArray<TSharedPtr<FJsonValue>>& Unsupported)
    {
        if (!WBP || !WBP->WidgetTree || !Step.Params.IsValid())
        {
            return;
        }

        const FString TempWidgetName = GetFirstStringField(Step.Params, TEXT("widget_name"), TEXT("name"), TEXT("id"));
        if (TempWidgetName.IsEmpty())
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                TEXT("preserve_children replacement add step is missing a temporary widget_name."),
                TEXT("temporary_widget_name"));
            return;
        }

        if (WBP->WidgetTree->FindWidget(FName(*TempWidgetName)))
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                FString::Printf(TEXT("Temporary replacement widget name '%s' already exists in the WBP."), *TempWidgetName),
                TEXT("temporary_widget_name"),
                TempWidgetName);
        }
    }

    static void PreflightMovePreservedChildStep(
        UWidgetBlueprint* WBP,
        const FUISpecPatchStep& Step,
        TArray<TSharedPtr<FJsonValue>>& Unsupported)
    {
        if (!WBP || !WBP->WidgetTree || !Step.Params.IsValid())
        {
            return;
        }

        const FString ChildName = GetFirstStringField(Step.Params, TEXT("widget_name"), TEXT("widget"), TEXT("name"));
        const FString ExpectedParentName = GetFirstStringField(Step.Params, TEXT("expected_parent_name"), TEXT("preserved_from_parent"));
        if (ChildName.IsEmpty() || ExpectedParentName.IsEmpty())
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                TEXT("preserve_children move step requires widget_name and expected_parent_name."),
                ChildName.IsEmpty() ? TEXT("widget_name") : TEXT("expected_parent_name"),
                ChildName);
            return;
        }

        UWidget* Child = WBP->WidgetTree->FindWidget(FName(*ChildName));
        if (!Child)
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                FString::Printf(TEXT("Preserved child widget '%s' does not exist in the WBP."), *ChildName),
                TEXT("child_widget_names"),
                ChildName);
            return;
        }

        int32 ChildIndex = INDEX_NONE;
        UPanelWidget* CurrentParent = UWidgetTree::FindWidgetParent(Child, ChildIndex);
        if (!CurrentParent || CurrentParent->GetFName() != FName(*ExpectedParentName))
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                FString::Printf(
                    TEXT("Preserved child widget '%s' is not a direct child of '%s'."),
                    *ChildName,
                    *ExpectedParentName),
                TEXT("child_widget_names"),
                ChildName,
                CurrentParent ? CurrentParent->GetClass()->GetName() : TEXT("none"));
        }
    }

    static void PreflightRenameReplacementWidgetStep(
        UWidgetBlueprint* WBP,
        const FUISpecPatchStep& Step,
        TArray<TSharedPtr<FJsonValue>>& Unsupported)
    {
        if (!WBP || !WBP->WidgetTree || !Step.Params.IsValid())
        {
            return;
        }

        const FString OldName = GetFirstStringField(Step.Params, TEXT("old_name"));
        const FString NewName = GetFirstStringField(Step.Params, TEXT("new_name"));
        if (OldName.IsEmpty() || NewName.IsEmpty())
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                TEXT("preserve_children replacement rename step requires old_name and new_name."),
                OldName.IsEmpty() ? TEXT("old_name") : TEXT("new_name"));
            return;
        }

        if (!WBP->WidgetTree->FindWidget(FName(*NewName)))
        {
            AddUnsupportedPatchField(
                Unsupported,
                Step.SourceIndex,
                Step.Type,
                FString::Printf(TEXT("Original widget '%s' does not exist before replacement."), *NewName),
                TEXT("widget_name"),
                NewName);
        }
    }

    static bool PreflightUISpecPatchSteps(
        const FString& AssetPath,
        const TArray<FUISpecPatchStep>& Steps,
        TArray<TSharedPtr<FJsonValue>>& Unsupported,
        FString& OutError)
    {
        bool bHasSlotSensitiveStep = false;
        bool bHasPreserveReplacementStep = false;
        for (const FUISpecPatchStep& Step : Steps)
        {
            if (Step.Namespace == TEXT("ui") &&
                (Step.Action == TEXT("set_slot_property") || Step.Action == TEXT("set_anchor_preset")))
            {
                bHasSlotSensitiveStep = true;
            }
            if (Step.Namespace == TEXT("ui") &&
                (Step.Type == TEXT("add_replacement_widget")
                    || Step.Type == TEXT("move_preserved_child")
                    || Step.Type == TEXT("rename_replacement_widget")))
            {
                bHasPreserveReplacementStep = true;
            }
        }

        if (!bHasSlotSensitiveStep && !bHasPreserveReplacementStep)
        {
            return true;
        }

        FMonolithActionResult LoadError;
        UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(AssetPath, LoadError);
        if (!WBP)
        {
            OutError = LoadError.ErrorMessage.IsEmpty()
                ? FString::Printf(TEXT("Unable to load Widget Blueprint '%s' for patch preflight."), *AssetPath)
                : LoadError.ErrorMessage;
            return false;
        }

        for (const FUISpecPatchStep& Step : Steps)
        {
            if (Step.Namespace == TEXT("ui") &&
                (Step.Action == TEXT("set_slot_property") || Step.Action == TEXT("set_anchor_preset")))
            {
                PreflightSetSlotPatchStep(WBP, Step, Unsupported);
            }
            if (Step.Namespace == TEXT("ui") && Step.Type == TEXT("add_replacement_widget"))
            {
                PreflightAddReplacementWidgetStep(WBP, Step, Unsupported);
            }
            else if (Step.Namespace == TEXT("ui") && Step.Type == TEXT("move_preserved_child"))
            {
                PreflightMovePreservedChildStep(WBP, Step, Unsupported);
            }
            else if (Step.Namespace == TEXT("ui") && Step.Type == TEXT("rename_replacement_widget"))
            {
                PreflightRenameReplacementWidgetStep(WBP, Step, Unsupported);
            }
        }
        return true;
    }

    static void AppendAddWidgetPatchSteps(
        const FString& AssetPath,
        const TSharedPtr<FJsonObject>& Entry,
        int32 Index,
        bool bCompileEachMutation,
        TArray<FUISpecPatchStep>& OutSteps,
        TArray<TSharedPtr<FJsonValue>>& OutUnsupported,
        const FString& OpForDiagnostics)
    {
        TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
        SetStringIfMissing(Params, TEXT("widget_class"), GetFirstStringField(Entry, TEXT("widget_class"), TEXT("widget_type"), TEXT("type")));
        SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("name"), TEXT("id")));
        SetStringIfMissing(Params, TEXT("parent_name"), GetFirstStringField(Entry, TEXT("parent_name"), TEXT("parent"), TEXT("parent_id"), TEXT("new_parent_name")));
        CopySlotPatchFields(Entry, Params);
        Params->RemoveField(TEXT("slot"));
        Params->RemoveField(TEXT("content"));
        Params->RemoveField(TEXT("style"));
        Params->RemoveField(TEXT("effect"));
        Params->RemoveField(TEXT("confirm_replace"));
        Params->RemoveField(TEXT("confirmReplace"));
        Params->RemoveField(TEXT("preserve_children"));
        Params->RemoveField(TEXT("preserveChildren"));
        Params->RemoveField(TEXT("child_widget_names"));
        Params->RemoveField(TEXT("preserve_child_names"));
        Params->RemoveField(TEXT("temporary_widget_name"));
        Params->RemoveField(TEXT("temporaryWidgetName"));
        Params->RemoveField(TEXT("temp_widget_name"));
        Params->RemoveField(TEXT("tempWidgetName"));
        Params->RemoveField(TEXT("requires_confirm_replace"));
        Params->RemoveField(TEXT("current_widget_class"));
        Params->RemoveField(TEXT("replacement_strategy"));
        Params->RemoveField(TEXT("preserve_children_unavailable_reason"));
        SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
        AddPatchStep(OutSteps, TEXT("add_widget"), TEXT("ui"), TEXT("add_widget"), Index, Params);

        const FString WidgetName = GetFirstStringField(Entry, TEXT("widget_name"), TEXT("name"), TEXT("id"));
        const FString WidgetClass = GetFirstStringField(Entry, TEXT("widget_class"), TEXT("widget_type"), TEXT("type"));

        const TSharedPtr<FJsonObject>* Content = nullptr;
        if (Entry->TryGetObjectField(TEXT("content"), Content) && Content && Content->IsValid())
        {
            TSharedPtr<FJsonObject> TextParams = MakeShared<FJsonObject>();
            TextParams->SetStringField(TEXT("asset_path"), AssetPath);
            TextParams->SetStringField(TEXT("widget_name"), WidgetName);
            CopyPatchFieldAlias(*Content, TextParams, TEXT("text"), TEXT("text"));
            CopyPatchFieldAlias(*Content, TextParams, TEXT("font_size"), TEXT("font_size"), TEXT("fontSize"));
            CopyPatchFieldAlias(*Content, TextParams, TEXT("text_color"), TEXT("text_color"), TEXT("fontColor"));
            SetBoolIfMissing(TextParams, TEXT("compile"), bCompileEachMutation);
            if (TextParams->HasField(TEXT("text")) || TextParams->HasField(TEXT("font_size")) || TextParams->HasField(TEXT("text_color")))
            {
                AddPatchStep(OutSteps, TEXT("set_text"), TEXT("ui"), TEXT("set_text"), Index, TextParams);
            }

            const FString BrushPath = GetFirstStringField(*Content, TEXT("brush_path"), TEXT("brushPath"));
            if (!BrushPath.IsEmpty())
            {
                if (!WidgetClass.Equals(TEXT("Image"), ESearchCase::IgnoreCase))
                {
                    const FString BrushPropertyName = GetSpecBrushPropertyNameForTypeToken(WidgetClass);
                    if (BrushPropertyName.IsEmpty())
                    {
                        AddUnsupportedPatchField(
                            OutUnsupported,
                            Index,
                            OpForDiagnostics + TEXT(".content.brushPath"),
                            TEXT("content.brushPath can be automatically patched for Image widgets via ui.set_image and Border widgets via ui.set_brush."));
                    }
                    else
                    {
                        TSharedPtr<FJsonObject> BrushParams = MakeShared<FJsonObject>();
                        BrushParams->SetStringField(TEXT("asset_path"), AssetPath);
                        BrushParams->SetStringField(TEXT("widget_name"), WidgetName);
                        BrushParams->SetStringField(TEXT("property_name"), BrushPropertyName);
                        FString BrushError;
                        if (!TryPopulateBrushPathAsBrushParam(BrushPath, BrushParams, BrushError))
                        {
                            AddUnsupportedPatchField(OutUnsupported, Index, OpForDiagnostics + TEXT(".content.brushPath"), BrushError);
                        }
                        else
                        {
                            SetBoolIfMissing(BrushParams, TEXT("compile"), bCompileEachMutation);
                            AddPatchStep(OutSteps, TEXT("set_brush"), TEXT("ui"), TEXT("set_brush"), Index, BrushParams);
                        }
                    }
                }
                else
                {
                    TSharedPtr<FJsonObject> ImageParams = MakeShared<FJsonObject>();
                    ImageParams->SetStringField(TEXT("asset_path"), AssetPath);
                    ImageParams->SetStringField(TEXT("widget_name"), WidgetName);
                    FString BrushError;
                    if (!TryPopulateBrushPathAsImageParam(BrushPath, ImageParams, BrushError))
                    {
                        AddUnsupportedPatchField(OutUnsupported, Index, OpForDiagnostics + TEXT(".content.brushPath"), BrushError);
                    }
                    else
                    {
                        SetBoolIfMissing(ImageParams, TEXT("compile"), bCompileEachMutation);
                        AddPatchStep(OutSteps, TEXT("set_image"), TEXT("ui"), TEXT("set_image"), Index, ImageParams);
                    }
                }
            }
        }

        const TSharedPtr<FJsonObject>* Style = nullptr;
        if (Entry->TryGetObjectField(TEXT("style"), Style) && Style && Style->IsValid())
        {
            AddStylePatchStepsFromStyleObject(
                AssetPath,
                WidgetName,
                WidgetClass,
                *Style,
                Index,
                bCompileEachMutation,
                OutSteps,
                OutUnsupported,
                OpForDiagnostics + TEXT(".style"));
        }

        const TSharedPtr<FJsonObject>* Effect = nullptr;
        if (Entry->TryGetObjectField(TEXT("effect"), Effect) && Effect && Effect->IsValid())
        {
            if (!WidgetClass.Equals(TEXT("EffectSurface"), ESearchCase::IgnoreCase))
            {
                AddUnsupportedPatchField(
                    OutUnsupported,
                    Index,
                    OpForDiagnostics + TEXT(".effect"),
                    TEXT("effect can be automatically patched only for EffectSurface widgets via ui.set_effect_surface_* owner actions."));
            }
            else
            {
                AddEffectPatchStepsFromEffectObject(
                    AssetPath,
                    WidgetName,
                    *Effect,
                    Index,
                    bCompileEachMutation,
                    OutSteps,
                    OutUnsupported);
            }
        }
    }

    static bool BuildUISpecPatchSteps(
        const FString& AssetPath,
        const TArray<TSharedPtr<FJsonValue>>& PatchValues,
        bool bDryRun,
        bool bCompileEachMutation,
        TArray<FUISpecPatchStep>& OutSteps,
        TArray<TSharedPtr<FJsonValue>>& OutUnsupported,
        FString& OutError)
    {
        for (int32 Index = 0; Index < PatchValues.Num(); ++Index)
        {
            const TSharedPtr<FJsonObject> Entry = PatchValues[Index].IsValid() ? PatchValues[Index]->AsObject() : nullptr;
            if (!Entry.IsValid())
            {
                OutError = FString::Printf(TEXT("patch[%d] must be an object."), Index);
                return false;
            }

            FString Op = GetFirstStringField(Entry, TEXT("op"), TEXT("action"));
            if (Op.IsEmpty())
            {
                OutError = FString::Printf(TEXT("patch[%d] requires op or action."), Index);
                return false;
            }
            Op = Op.ToLower();

            if (Op == TEXT("add") || Op == TEXT("add_widget"))
            {
                AppendAddWidgetPatchSteps(
                    AssetPath,
                    Entry,
                    Index,
                    bCompileEachMutation,
                    OutSteps,
                    OutUnsupported,
                    TEXT("add_widget"));
            }
            else if (Op == TEXT("replace") || Op == TEXT("replace_widget"))
            {
                const FString WidgetName = GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name"), TEXT("id"));
                const FString WidgetClass = GetFirstStringField(Entry, TEXT("widget_class"), TEXT("widget_type"), TEXT("type"));
                const FString ParentName = GetFirstStringField(Entry, TEXT("parent_name"), TEXT("parent"), TEXT("parent_id"), TEXT("new_parent_name"));

                bool bUnsupportedReplace = false;
                if (!EnsurePatchWidgetName(WidgetName, Index, Op, OutUnsupported))
                {
                    bUnsupportedReplace = true;
                }
                if (WidgetClass.IsEmpty())
                {
                    AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("replace_widget requires widget_class/widget_type/type for the replacement widget."));
                    bUnsupportedReplace = true;
                }
                if (ParentName.IsEmpty())
                {
                    AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("replace_widget requires parent_name/parent/parent_id because the existing parent is not inferred by the patch planner."));
                    bUnsupportedReplace = true;
                }

                bool bPreserveChildren = false;
                Entry->TryGetBoolField(TEXT("preserve_children"), bPreserveChildren);
                if (!bPreserveChildren)
                {
                    Entry->TryGetBoolField(TEXT("preserveChildren"), bPreserveChildren);
                }

                TArray<FString> ChildWidgetNames;
                FString ChildNamesError;
                if (bPreserveChildren
                    && !TryGetFirstStringArrayField(
                        Entry,
                        ChildWidgetNames,
                        ChildNamesError,
                        TEXT("child_widget_names"),
                        TEXT("preserve_child_names")))
                {
                    AddUnsupportedPatchField(OutUnsupported, Index, Op, ChildNamesError, TEXT("child_widget_names"), WidgetName);
                    bUnsupportedReplace = true;
                }
                if (bPreserveChildren && ChildWidgetNames.Num() == 0)
                {
                    AddUnsupportedPatchField(
                        OutUnsupported,
                        Index,
                        Op,
                        TEXT("replace_widget preserve_children=true requires child_widget_names[] so the plan can move only explicitly preserved direct children."),
                        TEXT("child_widget_names"),
                        WidgetName);
                    bUnsupportedReplace = true;
                }
                if (bPreserveChildren)
                {
                    FString PreserveReason;
                    if (!ReplacementClassCanHostPreservedChildren(WidgetClass, ChildWidgetNames.Num(), PreserveReason))
                    {
                        AddUnsupportedPatchField(OutUnsupported, Index, Op, PreserveReason, TEXT("widget_class"), WidgetName);
                        bUnsupportedReplace = true;
                    }
                }

                FString TemporaryWidgetName = GetFirstStringField(
                    Entry,
                    TEXT("temporary_widget_name"),
                    TEXT("temporaryWidgetName"),
                    TEXT("temp_widget_name"),
                    TEXT("tempWidgetName"));
                if (bPreserveChildren)
                {
                    if (TemporaryWidgetName.IsEmpty())
                    {
                        TemporaryWidgetName = MakeReplacementTempWidgetName(WidgetName);
                    }
                    if (TemporaryWidgetName == WidgetName)
                    {
                        AddUnsupportedPatchField(
                            OutUnsupported,
                            Index,
                            Op,
                            TEXT("replace_widget preserve_children requires a temporary widget name different from widget_name."),
                            TEXT("temporary_widget_name"),
                            WidgetName);
                        bUnsupportedReplace = true;
                    }
                }

                bool bConfirmReplace = false;
                Entry->TryGetBoolField(TEXT("confirm_replace"), bConfirmReplace);
                if (!bConfirmReplace)
                {
                    Entry->TryGetBoolField(TEXT("confirmReplace"), bConfirmReplace);
                }
                if (!bDryRun && !bConfirmReplace)
                {
                    AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("Mutating replace_widget requires confirm_replace=true on the patch op."));
                    bUnsupportedReplace = true;
                }

                if (bUnsupportedReplace)
                {
                    continue;
                }

                if (bPreserveChildren)
                {
                    TSharedPtr<FJsonObject> AddEntry = CloneJsonObject(Entry);
                    AddEntry->SetStringField(TEXT("widget_name"), TemporaryWidgetName);
                    AppendAddWidgetPatchSteps(
                        AssetPath,
                        AddEntry,
                        Index,
                        bCompileEachMutation,
                        OutSteps,
                        OutUnsupported,
                        TEXT("replace_widget"));

                    if (OutSteps.Num() > 0 && OutSteps.Last().SourceIndex == Index)
                    {
                        for (int32 StepIndex = OutSteps.Num() - 1; StepIndex >= 0; --StepIndex)
                        {
                            if (OutSteps[StepIndex].SourceIndex != Index || OutSteps[StepIndex].Type != TEXT("add_widget"))
                            {
                                continue;
                            }
                            OutSteps[StepIndex].Type = TEXT("add_replacement_widget");
                            break;
                        }
                    }

                    for (const FString& ChildWidgetName : ChildWidgetNames)
                    {
                        TSharedPtr<FJsonObject> MoveParams = MakeShared<FJsonObject>();
                        MoveParams->SetStringField(TEXT("asset_path"), AssetPath);
                        MoveParams->SetStringField(TEXT("widget_name"), ChildWidgetName);
                        MoveParams->SetStringField(TEXT("new_parent_name"), TemporaryWidgetName);
                        MoveParams->SetStringField(TEXT("expected_parent_name"), WidgetName);
                        MoveParams->SetBoolField(TEXT("compile"), bCompileEachMutation);
                        AddPatchStep(OutSteps, TEXT("move_preserved_child"), TEXT("ui"), TEXT("move_widget"), Index, MoveParams);
                    }

                    TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
                    RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
                    RemoveParams->SetStringField(TEXT("widget_name"), WidgetName);
                    RemoveParams->SetBoolField(TEXT("compile"), bCompileEachMutation);
                    AddPatchStep(OutSteps, TEXT("remove_replaced_widget"), TEXT("ui"), TEXT("remove_widget"), Index, RemoveParams);

                    TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
                    RenameParams->SetStringField(TEXT("asset_path"), AssetPath);
                    RenameParams->SetStringField(TEXT("old_name"), TemporaryWidgetName);
                    RenameParams->SetStringField(TEXT("new_name"), WidgetName);
                    AddPatchStep(OutSteps, TEXT("rename_replacement_widget"), TEXT("ui"), TEXT("rename_widget"), Index, RenameParams);
                    continue;
                }

                TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
                RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
                RemoveParams->SetStringField(TEXT("widget_name"), WidgetName);
                RemoveParams->SetBoolField(TEXT("compile"), bCompileEachMutation);
                AddPatchStep(OutSteps, TEXT("remove_widget"), TEXT("ui"), TEXT("remove_widget"), Index, RemoveParams);

                AppendAddWidgetPatchSteps(
                    AssetPath,
                    Entry,
                    Index,
                    bCompileEachMutation,
                    OutSteps,
                    OutUnsupported,
                    TEXT("replace_widget"));
            }
            else if (Op == TEXT("remove") || Op == TEXT("delete") || Op == TEXT("remove_widget"))
            {
                TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
                SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name")));
                SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
                AddPatchStep(OutSteps, TEXT("remove_widget"), TEXT("ui"), TEXT("remove_widget"), Index, Params);
            }
            else if (Op == TEXT("move") || Op == TEXT("reparent") || Op == TEXT("move_widget"))
            {
                TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
                SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name")));
                SetStringIfMissing(Params, TEXT("new_parent_name"), GetFirstStringField(Entry, TEXT("new_parent_name"), TEXT("new_parent"), TEXT("parent_name")));
                SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
                AddPatchStep(OutSteps, TEXT("move_widget"), TEXT("ui"), TEXT("move_widget"), Index, Params);
            }
            else if (Op == TEXT("set_slot") || Op == TEXT("slot") || Op == TEXT("set_slot_property"))
            {
                TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
                SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name")));
                CopySlotPatchFields(Entry, Params);
                Params->RemoveField(TEXT("slot"));
                SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);

                FString AnchorPreset;
                if (Params->TryGetStringField(TEXT("anchor_preset"), AnchorPreset) && !AnchorPreset.IsEmpty())
                {
                    TSharedPtr<FJsonObject> AnchorParams = MakeShared<FJsonObject>();
                    AnchorParams->SetStringField(TEXT("asset_path"), AssetPath);
                    AnchorParams->SetStringField(TEXT("widget_name"), GetFirstStringField(Params, TEXT("widget_name"), TEXT("widget"), TEXT("name")));
                    AnchorParams->SetStringField(TEXT("preset"), AnchorPreset);
                    SetBoolIfMissing(AnchorParams, TEXT("compile"), bCompileEachMutation);
                    AddPatchStep(OutSteps, TEXT("set_anchor_preset"), TEXT("ui"), TEXT("set_anchor_preset"), Index, AnchorParams);
                    Params->RemoveField(TEXT("anchor_preset"));
                }

                if (HasAnySlotPropertyField(Params))
                {
                    AddPatchStep(OutSteps, TEXT("set_slot_property"), TEXT("ui"), TEXT("set_slot_property"), Index, Params);
                }
            }
            else if (Op == TEXT("set_property") || Op == TEXT("update_property") || Op == TEXT("set_widget_property"))
            {
                TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
                SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name")));
                SetStringIfMissing(Params, TEXT("property_name"), GetFirstStringField(Entry, TEXT("property_name"), TEXT("property"), TEXT("path")));
                CopyPatchFieldAlias(Entry, Params, TEXT("value"), TEXT("value"), TEXT("property_value"));
                SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
                AddPatchStep(OutSteps, TEXT("set_widget_property"), TEXT("ui"), TEXT("set_widget_property"), Index, Params);
            }
            else if (Op == TEXT("set_text"))
            {
                TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
                SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name")));
                CopyPatchFieldAlias(Entry, Params, TEXT("font_size"), TEXT("font_size"), TEXT("fontSize"));
                CopyPatchFieldAlias(Entry, Params, TEXT("text_color"), TEXT("text_color"), TEXT("fontColor"));
                SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
                AddPatchStep(OutSteps, TEXT("set_text"), TEXT("ui"), TEXT("set_text"), Index, Params);
            }
            else if (Op == TEXT("set_brush"))
            {
                TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
                const FString WidgetClass = GetFirstStringField(Entry, TEXT("widget_class"), TEXT("widget_type"), TEXT("type"));
                SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name"), TEXT("id")));
                SetStringIfMissing(Params, TEXT("property_name"), GetFirstStringField(Entry, TEXT("property_name"), TEXT("property"), TEXT("path"), TEXT("brush_property")));
                if (!Params->HasField(TEXT("property_name")))
                {
                    SetStringIfMissing(Params, TEXT("property_name"), GetSpecBrushPropertyNameForTypeToken(WidgetClass));
                }
                if (GetFirstStringField(Params, TEXT("property_name")).IsEmpty())
                {
                    AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("set_brush requires property_name/property/path, or a supported widget_class such as Border to infer Background."));
                    continue;
                }

                const FString BrushPath = GetFirstStringField(Entry, TEXT("brush_path"), TEXT("brushPath"));
                if (!BrushPath.IsEmpty() && !Params->HasField(TEXT("texture_path")) && !Params->HasField(TEXT("material_path")))
                {
                    FString BrushError;
                    if (!TryPopulateBrushPathAsBrushParam(BrushPath, Params, BrushError))
                    {
                        AddUnsupportedPatchField(OutUnsupported, Index, Op, BrushError);
                        continue;
                    }
                }

                CopyPatchFieldAlias(Entry, Params, TEXT("texture_path"), TEXT("texture_path"), TEXT("texturePath"));
                CopyPatchFieldAlias(Entry, Params, TEXT("material_path"), TEXT("material_path"), TEXT("materialPath"));
                CopyPatchFieldAlias(Entry, Params, TEXT("tint_color"), TEXT("tint_color"), TEXT("tintColor"));
                CopyPatchFieldAlias(Entry, Params, TEXT("draw_type"), TEXT("draw_type"), TEXT("drawType"));
                SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
                AddPatchStep(OutSteps, TEXT("set_brush"), TEXT("ui"), TEXT("set_brush"), Index, Params);
            }
            else if (Op == TEXT("set_image"))
            {
                TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
                SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name")));

                const FString BrushPath = GetFirstStringField(Entry, TEXT("brush_path"), TEXT("brushPath"));
                if (!BrushPath.IsEmpty() && !Params->HasField(TEXT("texture_path")) && !Params->HasField(TEXT("material_path")))
                {
                    FString BrushError;
                    if (!TryPopulateBrushPathAsImageParam(BrushPath, Params, BrushError))
                    {
                        AddUnsupportedPatchField(OutUnsupported, Index, Op, BrushError);
                        continue;
                    }
                }

                CopyPatchFieldAlias(Entry, Params, TEXT("texture_path"), TEXT("texture_path"), TEXT("texturePath"));
                CopyPatchFieldAlias(Entry, Params, TEXT("material_path"), TEXT("material_path"), TEXT("materialPath"));
                CopyPatchFieldAlias(Entry, Params, TEXT("tint_color"), TEXT("tint_color"), TEXT("tintColor"));
                SetBoolIfMissing(Params, TEXT("compile"), bCompileEachMutation);
                AddPatchStep(OutSteps, TEXT("set_image"), TEXT("ui"), TEXT("set_image"), Index, Params);
            }
            else if (Op == TEXT("set_style"))
            {
                const TSharedPtr<FJsonObject>* NestedStyle = nullptr;
                const TSharedPtr<FJsonObject>& StyleSource =
                    Entry->TryGetObjectField(TEXT("style"), NestedStyle) && NestedStyle && NestedStyle->IsValid()
                        ? *NestedStyle
                        : Entry;
                AddStylePatchStepsFromStyleObject(
                    AssetPath,
                    GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name"), TEXT("id")),
                    GetFirstStringField(Entry, TEXT("widget_class"), TEXT("widget_type"), TEXT("type")),
                    StyleSource,
                    Index,
                    bCompileEachMutation,
                    OutSteps,
                        OutUnsupported,
                        Op);
            }
            else if (Op == TEXT("apply_style_to_widget") || Op == TEXT("set_common_style") || Op == TEXT("apply_common_style"))
            {
                TSharedPtr<FJsonObject> Params = MakePatchStepParamsWithAsset(Entry, AssetPath);
                SetStringIfMissing(Params, TEXT("widget_name"), GetFirstStringField(Entry, TEXT("widget_name"), TEXT("widget"), TEXT("name"), TEXT("id")));
                SetStringIfMissing(Params, TEXT("style_asset"), GetFirstStringField(Entry, TEXT("style_asset"), TEXT("styleAsset"), TEXT("style_ref"), TEXT("styleRef")));
                AddPatchStep(OutSteps, TEXT("apply_style_to_widget"), TEXT("ui"), TEXT("apply_style_to_widget"), Index, Params);
            }
            else if (RouteDirectEffectPatchOp(AssetPath, Entry, Op, Index, bCompileEachMutation, OutSteps, OutUnsupported))
            {
                continue;
            }
            else
            {
                AddUnsupportedPatchField(OutUnsupported, Index, Op, TEXT("Unsupported ui.apply_ui_spec_patch op. Supported: add_widget, replace_widget, remove_widget, move_widget, set_slot_property, set_widget_property, set_text, set_image, set_brush, set_style, apply_style_to_widget, and existing set_effect_surface_* owner actions. replace_widget preserve_children composes ui.add_widget, ui.move_widget, ui.remove_widget, and ui.rename_widget."));
            }
        }
        return true;
    }

    static TSharedPtr<FJsonObject> MakePatchPlannedStepRow(const FUISpecPatchStep& Step, int32 StepIndex)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("step_index"), StepIndex);
        Row->SetNumberField(TEXT("source_index"), Step.SourceIndex);
        Row->SetStringField(TEXT("type"), Step.Type);
        Row->SetStringField(TEXT("namespace"), Step.Namespace);
        Row->SetStringField(TEXT("action"), Step.Action);
        Row->SetStringField(TEXT("status"), TEXT("planned"));
        Row->SetBoolField(TEXT("ok"), true);
        Row->SetBoolField(TEXT("executed"), false);
        if (Step.Params.IsValid())
        {
            Row->SetObjectField(TEXT("params"), Step.Params);
        }
        return Row;
    }

    static TSharedPtr<FJsonObject> MakePatchExecutedStepRow(
        const FUISpecPatchStep& Step,
        int32 StepIndex,
        const FMonolithActionResult& ChildResult,
        bool& bOutStepOk)
    {
        bOutStepOk = ChildResult.bSuccess && ChildPayloadReportsOk(ChildResult);
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("step_index"), StepIndex);
        Row->SetNumberField(TEXT("source_index"), Step.SourceIndex);
        Row->SetStringField(TEXT("type"), Step.Type);
        Row->SetStringField(TEXT("namespace"), Step.Namespace);
        Row->SetStringField(TEXT("action"), Step.Action);
        Row->SetStringField(TEXT("status"), bOutStepOk ? TEXT("ok") : TEXT("failed"));
        Row->SetBoolField(TEXT("success"), ChildResult.bSuccess);
        Row->SetBoolField(TEXT("ok"), bOutStepOk);
        Row->SetBoolField(TEXT("executed"), true);
        if (ChildResult.Result.IsValid())
        {
            Row->SetObjectField(TEXT("result"), ChildResult.Result);
        }
        if (!ChildResult.bSuccess)
        {
            Row->SetStringField(TEXT("error"), ChildResult.ErrorMessage);
            Row->SetNumberField(TEXT("error_code"), ChildResult.ErrorCode);
        }
        return Row;
    }

    static void AddChangedWidgetFromParams(const TSharedPtr<FJsonObject>& Params, TSet<FString>& InOutNames)
    {
        const FString WidgetName = GetFirstStringField(Params, TEXT("widget_name"), TEXT("name"), TEXT("id"));
        if (!WidgetName.IsEmpty())
        {
            InOutNames.Add(WidgetName);
        }
    }

    static FMonolithActionResult HandleApplyUISpecPatch(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString AssetPath;
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
        }

        const TArray<TSharedPtr<FJsonValue>>* PatchValues = nullptr;
        if (!Params->TryGetArrayField(TEXT("patch"), PatchValues) || !PatchValues)
        {
            return FMonolithActionResult::Error(TEXT("Missing or invalid required param: patch (must be an array of patch op objects)"), -32602);
        }

        FString RequestId;
        Params->TryGetStringField(TEXT("request_id"), RequestId);

        bool bDryRun = true;
        bool bConfirm = false;
        bool bCompile = true;
        bool bSave = false;
        bool bReadBack = true;
        bool bContinueOnError = false;
        Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
        Params->TryGetBoolField(TEXT("confirm"), bConfirm);
        Params->TryGetBoolField(TEXT("compile"), bCompile);
        Params->TryGetBoolField(TEXT("save"), bSave);
        Params->TryGetBoolField(TEXT("read_back"), bReadBack);
        Params->TryGetBoolField(TEXT("continue_on_error"), bContinueOnError);

        if (!bDryRun && !bConfirm)
        {
            return FMonolithActionResult::Error(
                TEXT("ui.apply_ui_spec_patch is mutating; pass dry_run=true to inspect the plan or confirm=true with dry_run=false to apply."),
                -32602);
        }

        TArray<FUISpecPatchStep> Steps;
        TArray<TSharedPtr<FJsonValue>> Unsupported;
        FString ErrorMsg;
        if (!BuildUISpecPatchSteps(AssetPath, *PatchValues, bDryRun, /*bCompileEachMutation=*/false, Steps, Unsupported, ErrorMsg))
        {
            return FMonolithActionResult::Error(ErrorMsg, -32602);
        }
        if (!PreflightUISpecPatchSteps(AssetPath, Steps, Unsupported, ErrorMsg))
        {
            return FMonolithActionResult::Error(ErrorMsg, -32602);
        }

        if (!bDryRun && Unsupported.Num() > 0)
        {
            TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), false);
            Out->SetBoolField(TEXT("bSuccess"), false);
            Out->SetStringField(TEXT("schema_version"), TEXT("ui_spec_patch.v1"));
            Out->SetStringField(TEXT("asset_path"), AssetPath);
            Out->SetStringField(TEXT("status"), TEXT("unsupported_patch_ops"));
            Out->SetArrayField(TEXT("unsupported_fields"), Unsupported);
            return FMonolithActionResult::Success(Out);
        }

        if (!bDryRun && bCompile)
        {
            TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
            CompileParams->SetStringField(TEXT("asset_path"), AssetPath);
            AddPatchStep(Steps, TEXT("compile"), TEXT("ui"), TEXT("compile_widget"), INDEX_NONE, CompileParams);
        }
        if (!bDryRun && bSave)
        {
            TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
            SaveParams->SetStringField(TEXT("asset_path"), AssetPath);
            AddPatchStep(Steps, TEXT("save"), TEXT("asset"), TEXT("save_asset"), INDEX_NONE, SaveParams);
        }

        TArray<TSharedPtr<FJsonValue>> StepRows;
        TMap<FString, int32> PlannedCounts;
        TMap<FString, int32> AppliedCounts;
        TSet<FString> ChangedWidgets;
        bool bOverallOk = Unsupported.Num() == 0;
        bool bCompiled = false;
        bool bSaved = false;
        int32 ExecutedStepCount = 0;
        int32 FailedStepCount = 0;

        for (int32 StepIndex = 0; StepIndex < Steps.Num(); ++StepIndex)
        {
            const FUISpecPatchStep& Step = Steps[StepIndex];
            IncrementCount(PlannedCounts, Step.Type);
            AddChangedWidgetFromParams(Step.Params, ChangedWidgets);

            if (bDryRun)
            {
                StepRows.Add(MakeShared<FJsonValueObject>(MakePatchPlannedStepRow(Step, StepIndex)));
                continue;
            }

            const FMonolithActionResult ChildResult = FMonolithToolRegistry::Get().ExecuteAction(Step.Namespace, Step.Action, Step.Params);
            ++ExecutedStepCount;
            bool bStepOk = false;
            StepRows.Add(MakeShared<FJsonValueObject>(MakePatchExecutedStepRow(Step, StepIndex, ChildResult, bStepOk)));

            bool bChildBool = false;
            if (TryGetResultBool(ChildResult, TEXT("compiled"), bChildBool))
            {
                bCompiled = bCompiled || bChildBool;
            }
            if (TryGetResultBool(ChildResult, TEXT("saved"), bChildBool))
            {
                bSaved = bSaved || bChildBool;
            }

            if (bStepOk && Step.bMutating)
            {
                IncrementCount(AppliedCounts, Step.Type);
            }
            if (!bStepOk)
            {
                bOverallOk = false;
                ++FailedStepCount;
                if (!bContinueOnError)
                {
                    break;
                }
            }
        }

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("ok"), bOverallOk);
        Out->SetBoolField(TEXT("bSuccess"), bOverallOk);
        Out->SetStringField(TEXT("schema_version"), TEXT("ui_spec_patch.v1"));
        Out->SetStringField(TEXT("asset_path"), AssetPath);
        Out->SetStringField(TEXT("status"), bDryRun ? (Unsupported.Num() > 0 ? TEXT("planned_with_unsupported") : TEXT("planned")) : (bOverallOk ? TEXT("applied") : TEXT("partial_or_failed")));
        Out->SetBoolField(TEXT("dry_run"), bDryRun);
        Out->SetBoolField(TEXT("confirm"), bConfirm);
        Out->SetBoolField(TEXT("compile"), bCompile);
        Out->SetBoolField(TEXT("save"), bSave);
        Out->SetBoolField(TEXT("read_back"), bReadBack);
        Out->SetBoolField(TEXT("compiled"), bCompiled);
        Out->SetBoolField(TEXT("saved"), bSaved);
        Out->SetNumberField(TEXT("step_count"), Steps.Num());
        Out->SetNumberField(TEXT("executed_step_count"), ExecutedStepCount);
        Out->SetNumberField(TEXT("failed_step_count"), FailedStepCount);
        Out->SetObjectField(TEXT("planned_counts"), MakeCountsObject(PlannedCounts));
        Out->SetObjectField(TEXT("applied_counts"), MakeCountsObject(AppliedCounts));
        Out->SetArrayField(TEXT("steps"), StepRows);
        Out->SetArrayField(TEXT("unsupported_fields"), Unsupported);
        if (!RequestId.IsEmpty())
        {
            Out->SetStringField(TEXT("request_id"), RequestId);
        }

        TArray<TSharedPtr<FJsonValue>> ChangedWidgetRows;
        for (const FString& WidgetName : ChangedWidgets)
        {
            ChangedWidgetRows.Add(MakeShared<FJsonValueString>(WidgetName));
        }
        Out->SetArrayField(TEXT("changed_widgets"), ChangedWidgetRows);

        if (!bDryRun && bReadBack && bOverallOk)
        {
            FUISpecSerializerInputs DumpInputs;
            DumpInputs.AssetPath = AssetPath;
            DumpInputs.RequestId = RequestId;
            const FUISpecSerializerResult DumpResult = FUISpecSerializer::Dump(DumpInputs);
            TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
            Proof->SetBoolField(TEXT("dump_success"), DumpResult.bSuccess);
            Proof->SetNumberField(TEXT("nodes_visited"), DumpResult.NodesVisited);
            Proof->SetNumberField(TEXT("animations_captured"), DumpResult.AnimationsCaptured);
            if (DumpResult.bSuccess)
            {
                Proof->SetObjectField(TEXT("spec"), DocumentToJson(DumpResult.Document));
            }
            else
            {
                SetValidationFindingArray(Proof, TEXT("errors"), DumpResult.Errors);
            }
            Out->SetObjectField(TEXT("roundtrip_proof"), Proof);
        }

        return FMonolithActionResult::Success(Out);
    }

    static void BuildScreenAssetMap(const TSharedPtr<FJsonObject>& Spec, TMap<FString, FString>& OutScreenToAsset)
    {
        const TArray<TSharedPtr<FJsonValue>>* Screens = nullptr;
        if (!Spec.IsValid() || !Spec->TryGetArrayField(TEXT("screens"), Screens) || !Screens)
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Screens)
        {
            const TSharedPtr<FJsonObject>* ScreenObj = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(ScreenObj) || !ScreenObj || !ScreenObj->IsValid())
            {
                continue;
            }

            const FString ScreenId = GetFirstStringField(*ScreenObj, TEXT("id"), TEXT("screen"));
            const FString AssetPath = GetFirstStringField(*ScreenObj, TEXT("asset_path"), TEXT("wbp_path"));
            if (!ScreenId.IsEmpty() && !AssetPath.IsEmpty())
            {
                OutScreenToAsset.Add(ScreenId, AssetPath);
            }
        }
    }

    static FString ResolveScreenBoundAssetPath(const TSharedPtr<FJsonObject>& Entry, const TMap<FString, FString>& ScreenToAsset)
    {
        const FString ExplicitPath = GetFirstStringField(Entry, TEXT("asset_path"), TEXT("wbp_path"), TEXT("layout_asset_path"));
        if (!ExplicitPath.IsEmpty())
        {
            return ExplicitPath;
        }

        const FString ScreenId = GetFirstStringField(Entry, TEXT("screen"), TEXT("screen_id"));
        if (const FString* Found = ScreenToAsset.Find(ScreenId))
        {
            return *Found;
        }
        return FString();
    }

    static bool AddObjectArraySteps(
        const TSharedPtr<FJsonObject>& Spec,
        const FString& FieldName,
        const FString& Type,
        const FString& Namespace,
        const FString& Action,
        EMenuTransformDryRunMode DryRunMode,
        bool bDryRun,
        bool bConfirm,
        bool bCompile,
        bool bSave,
        bool bCopySharedRemaps,
        bool bCopyFontRemaps,
        TArray<FMenuTransformStep>& OutSteps,
        FString& OutError)
    {
        TArray<TSharedPtr<FJsonObject>> Entries;
        if (!TryGetObjectArray(Spec, FieldName, Entries, OutError))
        {
            return false;
        }

        OutSteps.Reserve(OutSteps.Num() + Entries.Num());
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> ChildParams = CloneJsonObject(Entries[Index]);
            if (bCopySharedRemaps)
            {
                CopySharedRemapDefaults(Spec, ChildParams);
            }
            if (bCopyFontRemaps)
            {
                CopyFontRemapDefaults(Spec, ChildParams);
            }
            if (DryRunMode == EMenuTransformDryRunMode::ExecuteChildDryRun)
            {
                ChildParams->SetBoolField(TEXT("dry_run"), bDryRun);
                ChildParams->SetBoolField(TEXT("confirm"), bConfirm);
            }
            SetBoolIfMissing(ChildParams, TEXT("compile"), bCompile);
            SetBoolIfMissing(ChildParams, TEXT("save"), bSave);
            OutSteps.Add(MakeStep(Type, Namespace, Action, Index, ChildParams, DryRunMode));
        }
        return true;
    }

    static bool AddLayoutLayerSteps(
        const TSharedPtr<FJsonObject>& Spec,
        bool bCompile,
        bool bSave,
        TArray<FMenuTransformStep>& OutSteps,
        FString& OutError)
    {
        TArray<TSharedPtr<FJsonObject>> Entries;
        if (!TryGetObjectArray(Spec, TEXT("layout_layers"), Entries, OutError))
        {
            return false;
        }

        TArray<TSharedPtr<FJsonObject>> MenuLayers;
        if (!TryGetObjectArray(Spec, TEXT("layers"), MenuLayers, OutError))
        {
            return false;
        }
        Entries.Append(MenuLayers);

        const FString DefaultLayoutPath = GetFirstStringField(Spec, TEXT("layout_asset_path"), TEXT("asset_path"));
        OutSteps.Reserve(OutSteps.Num() + Entries.Num());
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> ChildParams = CloneJsonObject(Entries[Index]);
            SetStringIfMissing(ChildParams, TEXT("asset_path"), DefaultLayoutPath);
            SetStringIfMissing(ChildParams, TEXT("layer_tag"), GetFirstStringField(Entries[Index], TEXT("tag"), TEXT("id")));
            SetBoolIfMissing(ChildParams, TEXT("compile"), bCompile);
            SetBoolIfMissing(ChildParams, TEXT("save"), bSave);
            OutSteps.Add(MakeStep(TEXT("layout_layer"), TEXT("ui"), TEXT("add_primary_game_layout_layer"), Index, ChildParams, EMenuTransformDryRunMode::PlanOnly));
        }
        return true;
    }

    static bool AddWidgetPropertySteps(
        const TSharedPtr<FJsonObject>& Spec,
        bool bCompile,
        TArray<FMenuTransformStep>& OutSteps,
        FString& OutError)
    {
        TArray<TSharedPtr<FJsonObject>> Entries;
        if (!TryGetObjectArray(Spec, TEXT("widget_properties"), Entries, OutError))
        {
            return false;
        }

        OutSteps.Reserve(OutSteps.Num() + Entries.Num());
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> ChildParams = CloneJsonObject(Entries[Index]);
            SetStringIfMissing(ChildParams, TEXT("widget_name"), GetFirstStringField(Entries[Index], TEXT("widget"), TEXT("name")));
            SetStringIfMissing(ChildParams, TEXT("property_name"), GetFirstStringField(Entries[Index], TEXT("property")));
            if (!ChildParams->HasField(TEXT("value")) && ChildParams->HasField(TEXT("property_value")))
            {
                TSharedPtr<FJsonValue> Value = ChildParams->TryGetField(TEXT("property_value"));
                if (Value.IsValid())
                {
                    ChildParams->SetField(TEXT("value"), Value);
                }
            }
            CopyFieldIfMissing(Spec, ChildParams, TEXT("raw_mode"));
            SetBoolIfMissing(ChildParams, TEXT("compile"), bCompile);
            OutSteps.Add(MakeStep(TEXT("widget_property"), TEXT("ui"), TEXT("set_widget_property"), Index, ChildParams, EMenuTransformDryRunMode::PlanOnly));
        }
        return true;
    }

    static bool AddRemoveWidgetSteps(
        const TSharedPtr<FJsonObject>& Spec,
        bool bCompile,
        TArray<FMenuTransformStep>& OutSteps,
        FString& OutError)
    {
        TArray<TSharedPtr<FJsonObject>> Entries;
        if (!TryGetObjectArray(Spec, TEXT("remove_widgets"), Entries, OutError))
        {
            return false;
        }

        OutSteps.Reserve(OutSteps.Num() + Entries.Num());
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            TArray<FString> WidgetNames;
            const FString SingleName = GetFirstStringField(Entries[Index], TEXT("widget_name"), TEXT("widget"), TEXT("name"));
            if (!SingleName.IsEmpty())
            {
                WidgetNames.Add(SingleName);
            }

            const TArray<TSharedPtr<FJsonValue>>* NameValues = nullptr;
            if (Entries[Index]->TryGetArrayField(TEXT("widget_names"), NameValues) ||
                Entries[Index]->TryGetArrayField(TEXT("names"), NameValues))
            {
                if (!NameValues)
                {
                    OutError = FString::Printf(TEXT("remove_widgets[%d].widget_names must be an array of strings."), Index);
                    return false;
                }
                for (const TSharedPtr<FJsonValue>& NameValue : *NameValues)
                {
                    FString Name;
                    if (!NameValue.IsValid() || !NameValue->TryGetString(Name) || Name.IsEmpty())
                    {
                        OutError = FString::Printf(TEXT("remove_widgets[%d].widget_names entries must be non-empty strings."), Index);
                        return false;
                    }
                    WidgetNames.Add(Name);
                }
            }

            for (const FString& WidgetName : WidgetNames)
            {
                TSharedPtr<FJsonObject> ChildParams = CloneJsonObject(Entries[Index]);
                ChildParams->RemoveField(TEXT("widget_names"));
                ChildParams->RemoveField(TEXT("names"));
                ChildParams->SetStringField(TEXT("widget_name"), WidgetName);
                SetBoolIfMissing(ChildParams, TEXT("compile"), bCompile);
                OutSteps.Add(MakeStep(TEXT("remove_widget"), TEXT("ui"), TEXT("remove_widget"), Index, ChildParams, EMenuTransformDryRunMode::PlanOnly));
            }
        }
        return true;
    }

    static bool AddVariableDefaultSteps(
        const TSharedPtr<FJsonObject>& Spec,
        TArray<FMenuTransformStep>& OutSteps,
        FString& OutError)
    {
        TArray<TSharedPtr<FJsonObject>> Entries;
        if (!TryGetObjectArray(Spec, TEXT("variable_defaults"), Entries, OutError))
        {
            return false;
        }

        OutSteps.Reserve(OutSteps.Num() + Entries.Num());
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> ChildParams = CloneJsonObject(Entries[Index]);
            SetStringIfMissing(ChildParams, TEXT("name"), GetFirstStringField(Entries[Index], TEXT("variable_name"), TEXT("variable")));
            if (!ChildParams->HasField(TEXT("default_value")) && ChildParams->HasField(TEXT("value")))
            {
                FString ValueString;
                TSharedPtr<FJsonValue> Value = ChildParams->TryGetField(TEXT("value"));
                if (Value.IsValid() && Value->TryGetString(ValueString))
                {
                    ChildParams->SetStringField(TEXT("default_value"), ValueString);
                }
            }
            OutSteps.Add(MakeStep(TEXT("variable_default"), TEXT("blueprint"), TEXT("set_variable_defaults"), Index, ChildParams, EMenuTransformDryRunMode::PlanOnly));
        }
        return true;
    }

    static bool AddFocusSteps(
        const TSharedPtr<FJsonObject>& Spec,
        const TMap<FString, FString>& ScreenToAsset,
        TArray<FMenuTransformStep>& OutSteps,
        FString& OutError)
    {
        TArray<TSharedPtr<FJsonObject>> Entries;
        if (!TryGetObjectArray(Spec, TEXT("focus_table"), Entries, OutError) ||
            !TryGetObjectArray(Spec, TEXT("initial_focus"), Entries, OutError) ||
            !TryGetObjectArray(Spec, TEXT("desired_focus"), Entries, OutError))
        {
            return false;
        }

        OutSteps.Reserve(OutSteps.Num() + Entries.Num());
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> ChildParams = MakeShared<FJsonObject>();
            SetStringIfMissing(ChildParams, TEXT("wbp_path"), ResolveScreenBoundAssetPath(Entries[Index], ScreenToAsset));
            SetStringIfMissing(ChildParams, TEXT("target_widget"), GetFirstStringField(Entries[Index], TEXT("target_widget"), TEXT("target"), TEXT("widget")));
            OutSteps.Add(MakeStep(TEXT("initial_focus"), TEXT("ui"), TEXT("set_initial_focus_target"), Index, ChildParams, EMenuTransformDryRunMode::PlanOnly));
        }
        return true;
    }

    static bool AddNavigationSteps(
        const TSharedPtr<FJsonObject>& Spec,
        const TMap<FString, FString>& ScreenToAsset,
        bool bSave,
        TArray<FMenuTransformStep>& OutSteps,
        FString& OutError)
    {
        TArray<TSharedPtr<FJsonObject>> DirectEntries;
        if (!TryGetObjectArray(Spec, TEXT("navigation_bulk"), DirectEntries, OutError))
        {
            return false;
        }

        OutSteps.Reserve(OutSteps.Num() + DirectEntries.Num());
        for (int32 Index = 0; Index < DirectEntries.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> ChildParams = CloneJsonObject(DirectEntries[Index]);
            SetBoolIfMissing(ChildParams, TEXT("save"), bSave);
            OutSteps.Add(MakeStep(TEXT("navigation_bulk"), TEXT("ui"), TEXT("set_widget_navigation_bulk"), Index, ChildParams, EMenuTransformDryRunMode::PlanOnly));
        }

        TArray<TSharedPtr<FJsonObject>> Overrides;
        if (!TryGetObjectArray(Spec, TEXT("nav_overrides"), Overrides, OutError))
        {
            return false;
        }

        TMap<FString, TArray<TSharedPtr<FJsonValue>>> EntriesByAsset;
        for (int32 Index = 0; Index < Overrides.Num(); ++Index)
        {
            const FString AssetPath = ResolveScreenBoundAssetPath(Overrides[Index], ScreenToAsset);
            if (AssetPath.IsEmpty())
            {
                OutError = FString::Printf(TEXT("nav_overrides[%d] requires asset_path/wbp_path or a screen that exists in screens[]."), Index);
                return false;
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            SetStringIfMissing(Entry, TEXT("widget_name"), GetFirstStringField(Overrides[Index], TEXT("widget_name"), TEXT("widget")));
            SetStringIfMissing(Entry, TEXT("direction"), GetFirstStringField(Overrides[Index], TEXT("direction")));
            SetStringIfMissing(Entry, TEXT("rule"), GetFirstStringField(Overrides[Index], TEXT("rule")));
            SetStringIfMissing(Entry, TEXT("explicit_target"), GetFirstStringField(Overrides[Index], TEXT("explicit_target"), TEXT("target"), TEXT("target_widget")));
            if (!Entry->HasField(TEXT("rule")) && Entry->HasField(TEXT("explicit_target")))
            {
                Entry->SetStringField(TEXT("rule"), TEXT("Explicit"));
            }
            EntriesByAsset.FindOrAdd(AssetPath).Add(MakeShared<FJsonValueObject>(Entry));
        }

        int32 GroupIndex = 0;
        OutSteps.Reserve(OutSteps.Num() + EntriesByAsset.Num());
        for (TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Pair : EntriesByAsset)
        {
            TSharedPtr<FJsonObject> ChildParams = MakeShared<FJsonObject>();
            ChildParams->SetStringField(TEXT("wbp_path"), Pair.Key);
            ChildParams->SetArrayField(TEXT("entries"), Pair.Value);
            SetBoolIfMissing(ChildParams, TEXT("save"), bSave);
            OutSteps.Add(MakeStep(TEXT("navigation_bulk"), TEXT("ui"), TEXT("set_widget_navigation_bulk"), GroupIndex++, ChildParams, EMenuTransformDryRunMode::PlanOnly));
        }
        return true;
    }

    static bool BuildCommonMenuTransformSteps(
        const TSharedPtr<FJsonObject>& Spec,
        bool bDryRun,
        bool bConfirm,
        bool bCompile,
        bool bSave,
        TArray<FMenuTransformStep>& OutSteps,
        FString& OutError)
    {
        if (!AddObjectArraySteps(Spec, TEXT("widget_subtrees"), TEXT("widget_subtree"), TEXT("ui"), TEXT("copy_widget_subtree_with_class_remap"), EMenuTransformDryRunMode::ExecuteChildDryRun, bDryRun, bConfirm, bCompile, bSave, true, false, OutSteps, OutError) ||
            !AddObjectArraySteps(Spec, TEXT("blueprint_graphs"), TEXT("blueprint_graph"), TEXT("blueprint"), TEXT("clone_graphs_with_reference_remap"), EMenuTransformDryRunMode::ExecuteChildDryRun, bDryRun, bConfirm, bCompile, bSave, true, false, OutSteps, OutError) ||
            !AddObjectArraySteps(Spec, TEXT("font_repairs"), TEXT("font_repair"), TEXT("ui"), TEXT("repair_slate_font_references"), EMenuTransformDryRunMode::ExecuteChildDryRun, bDryRun, bConfirm, bCompile, bSave, false, true, OutSteps, OutError) ||
            !AddObjectArraySteps(Spec, TEXT("extension_points"), TEXT("extension_point"), TEXT("ui"), TEXT("add_extension_point_widget"), EMenuTransformDryRunMode::PlanOnly, bDryRun, bConfirm, bCompile, bSave, false, false, OutSteps, OutError) ||
            !AddLayoutLayerSteps(Spec, bCompile, bSave, OutSteps, OutError) ||
            !AddWidgetPropertySteps(Spec, bCompile, OutSteps, OutError) ||
            !AddRemoveWidgetSteps(Spec, bCompile, OutSteps, OutError) ||
            !AddVariableDefaultSteps(Spec, OutSteps, OutError))
        {
            return false;
        }

        TMap<FString, FString> ScreenToAsset;
        BuildScreenAssetMap(Spec, ScreenToAsset);
        if (!AddFocusSteps(Spec, ScreenToAsset, OutSteps, OutError) ||
            !AddNavigationSteps(Spec, ScreenToAsset, bSave, OutSteps, OutError))
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* FrontendValidation = nullptr;
        if (Spec->TryGetObjectField(TEXT("frontend_validation"), FrontendValidation) && FrontendValidation && FrontendValidation->IsValid())
        {
            OutSteps.Add(MakeStep(TEXT("frontend_validation"), TEXT("ui"), TEXT("validate_frontend_menu_flow"), 0, CloneJsonObject(*FrontendValidation), EMenuTransformDryRunMode::ExecuteReadOnly, false));
        }
        else if (Spec->HasField(TEXT("frontend_validation")))
        {
            OutError = TEXT("frontend_validation must be an object when provided.");
            return false;
        }

        if (OutSteps.IsEmpty())
        {
            OutError = TEXT("apply_common_menu_transform_spec requires at least one transform field: widget_subtrees, blueprint_graphs, font_repairs, extension_points, layout_layers/layers, widget_properties, remove_widgets, variable_defaults, focus_table/initial_focus/desired_focus, nav_overrides/navigation_bulk, or frontend_validation.");
            return false;
        }
        return true;
    }

    static bool TryGetResultBool(const FMonolithActionResult& ChildResult, const FString& FieldName, bool& OutValue)
    {
        OutValue = false;
        return ChildResult.Result.IsValid() && ChildResult.Result->TryGetBoolField(FieldName, OutValue);
    }

    static bool ChildPayloadReportsOk(const FMonolithActionResult& ChildResult)
    {
        bool bPayloadOk = true;
        bool bField = true;
        if (ChildResult.Result.IsValid() && ChildResult.Result->TryGetBoolField(TEXT("ok"), bField))
        {
            bPayloadOk = bPayloadOk && bField;
        }
        if (ChildResult.Result.IsValid() && ChildResult.Result->TryGetBoolField(TEXT("bSuccess"), bField))
        {
            bPayloadOk = bPayloadOk && bField;
        }
        return bPayloadOk;
    }

    static TSharedPtr<FJsonObject> MakePlannedStepRow(const FMenuTransformStep& Step, int32 StepIndex)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("step_index"), StepIndex);
        Row->SetNumberField(TEXT("source_index"), Step.SourceIndex);
        Row->SetStringField(TEXT("type"), Step.Type);
        Row->SetStringField(TEXT("namespace"), Step.Namespace);
        Row->SetStringField(TEXT("action"), Step.Action);
        Row->SetStringField(TEXT("status"), TEXT("planned"));
        Row->SetBoolField(TEXT("success"), true);
        Row->SetBoolField(TEXT("ok"), true);
        Row->SetBoolField(TEXT("executed"), false);
        if (Step.Params.IsValid())
        {
            Row->SetObjectField(TEXT("params"), Step.Params);
        }
        return Row;
    }

    static TSharedPtr<FJsonObject> MakeExecutedStepRow(
        const FMenuTransformStep& Step,
        int32 StepIndex,
        const FMonolithActionResult& ChildResult,
        bool& bOutStepOk)
    {
        bOutStepOk = ChildResult.bSuccess && ChildPayloadReportsOk(ChildResult);

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("step_index"), StepIndex);
        Row->SetNumberField(TEXT("source_index"), Step.SourceIndex);
        Row->SetStringField(TEXT("type"), Step.Type);
        Row->SetStringField(TEXT("namespace"), Step.Namespace);
        Row->SetStringField(TEXT("action"), Step.Action);
        Row->SetStringField(TEXT("status"), bOutStepOk ? TEXT("ok") : TEXT("failed"));
        Row->SetBoolField(TEXT("success"), ChildResult.bSuccess);
        Row->SetBoolField(TEXT("ok"), bOutStepOk);
        Row->SetBoolField(TEXT("executed"), true);
        if (ChildResult.Result.IsValid())
        {
            Row->SetObjectField(TEXT("result"), ChildResult.Result);
        }
        if (!ChildResult.bSuccess)
        {
            Row->SetStringField(TEXT("error"), ChildResult.ErrorMessage);
            Row->SetNumberField(TEXT("error_code"), ChildResult.ErrorCode);
        }
        return Row;
    }

    static TSharedPtr<FJsonObject> MakeTransformErrorRow(const FMenuTransformStep& Step, int32 StepIndex, const FString& Message)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("step_index"), StepIndex);
        Row->SetStringField(TEXT("type"), Step.Type);
        Row->SetStringField(TEXT("namespace"), Step.Namespace);
        Row->SetStringField(TEXT("action"), Step.Action);
        Row->SetStringField(TEXT("message"), Message);
        return Row;
    }

    static FMonolithActionResult HandleApplyCommonMenuTransformSpec(const TSharedPtr<FJsonObject>& Params)
    {
        TSharedPtr<FJsonObject> Spec;
        FString ErrorMsg;
        if (!ResolveCommonMenuSpecObject(Params, Spec, ErrorMsg))
        {
            return FMonolithActionResult::Error(ErrorMsg, -32602);
        }

        FString RequestId;
        if (Params.IsValid())
        {
            Params->TryGetStringField(TEXT("request_id"), RequestId);
        }
        if (RequestId.IsEmpty() && Spec.IsValid())
        {
            Spec->TryGetStringField(TEXT("request_id"), RequestId);
        }

        bool bDryRun = true;
        bool bConfirm = false;
        bool bCompile = true;
        bool bSave = false;
        bool bContinueOnError = false;
        if (!GetOptionalBoolFromRootOrSpec(Params, Spec, TEXT("dry_run"), bDryRun, ErrorMsg, true) ||
            !GetOptionalBoolFromRootOrSpec(Params, Spec, TEXT("confirm"), bConfirm, ErrorMsg, false) ||
            !GetOptionalBoolFromRootOrSpec(Params, Spec, TEXT("compile"), bCompile, ErrorMsg, true) ||
            !GetOptionalBoolFromRootOrSpec(Params, Spec, TEXT("save"), bSave, ErrorMsg, false) ||
            !GetOptionalBoolFromRootOrSpec(Params, Spec, TEXT("continue_on_error"), bContinueOnError, ErrorMsg, false))
        {
            return FMonolithActionResult::Error(ErrorMsg, -32602);
        }

        if (!bDryRun && !bConfirm)
        {
            return FMonolithActionResult::Error(
                TEXT("apply_common_menu_transform_spec is mutating; pass dry_run=true to inspect the plan or confirm=true with dry_run=false to apply."),
                -32602);
        }

        TArray<FMenuTransformStep> Steps;
        if (!BuildCommonMenuTransformSteps(Spec, bDryRun, bConfirm, bCompile, bSave, Steps, ErrorMsg))
        {
            return FMonolithActionResult::Error(ErrorMsg, -32602);
        }

        TArray<TSharedPtr<FJsonValue>> StepRows;
        TArray<TSharedPtr<FJsonValue>> ErrorRows;
        TMap<FString, int32> PlannedCounts;
        TMap<FString, int32> AppliedCounts;
        bool bOverallOk = true;
        bool bReportedChanged = false;
        bool bChangedKnown = true;
        bool bCompiled = false;
        bool bSaved = false;
        int32 ExecutedStepCount = 0;
        int32 PlannedOnlyStepCount = 0;
        int32 FailedStepCount = 0;
        int32 ChangedUnknownStepCount = 0;

        for (int32 StepIndex = 0; StepIndex < Steps.Num(); ++StepIndex)
        {
            const FMenuTransformStep& Step = Steps[StepIndex];
            IncrementCount(PlannedCounts, Step.Type);

            const bool bExecute =
                !bDryRun ||
                Step.DryRunMode == EMenuTransformDryRunMode::ExecuteChildDryRun ||
                Step.DryRunMode == EMenuTransformDryRunMode::ExecuteReadOnly;

            if (!bExecute)
            {
                ++PlannedOnlyStepCount;
                StepRows.Add(MakeShared<FJsonValueObject>(MakePlannedStepRow(Step, StepIndex)));
                continue;
            }

            const FMonolithActionResult ChildResult = FMonolithToolRegistry::Get().ExecuteAction(Step.Namespace, Step.Action, Step.Params);
            ++ExecutedStepCount;

            bool bStepOk = false;
            StepRows.Add(MakeShared<FJsonValueObject>(MakeExecutedStepRow(Step, StepIndex, ChildResult, bStepOk)));

            bool bChildBool = false;
            if (TryGetResultBool(ChildResult, TEXT("changed"), bChildBool))
            {
                bReportedChanged = bReportedChanged || bChildBool;
            }
            else if (!bDryRun && Step.bMutating && bStepOk)
            {
                bChangedKnown = false;
                ++ChangedUnknownStepCount;
            }

            if (TryGetResultBool(ChildResult, TEXT("compiled"), bChildBool))
            {
                bCompiled = bCompiled || bChildBool;
            }
            if (TryGetResultBool(ChildResult, TEXT("compiled_once"), bChildBool))
            {
                bCompiled = bCompiled || bChildBool;
            }
            if (TryGetResultBool(ChildResult, TEXT("saved"), bChildBool))
            {
                bSaved = bSaved || bChildBool;
            }

            if (bStepOk && !bDryRun && Step.bMutating)
            {
                IncrementCount(AppliedCounts, Step.Type);
            }

            if (!bStepOk)
            {
                bOverallOk = false;
                ++FailedStepCount;
                const FString Message = ChildResult.bSuccess
                    ? FString::Printf(TEXT("%s.%s reported an unsuccessful payload."), *Step.Namespace, *Step.Action)
                    : ChildResult.ErrorMessage;
                ErrorRows.Add(MakeShared<FJsonValueObject>(MakeTransformErrorRow(Step, StepIndex, Message)));
                if (!bContinueOnError)
                {
                    break;
                }
            }
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("namespace"), TEXT("ui"));
        Result->SetStringField(TEXT("action"), TEXT("apply_common_menu_transform_spec"));
        if (!RequestId.IsEmpty())
        {
            Result->SetStringField(TEXT("request_id"), RequestId);
        }
        Result->SetBoolField(TEXT("ok"), bOverallOk);
        Result->SetBoolField(TEXT("dry_run"), bDryRun);
        Result->SetBoolField(TEXT("confirm"), bConfirm);
        Result->SetBoolField(TEXT("compile"), bCompile);
        Result->SetBoolField(TEXT("save"), bSave);
        Result->SetBoolField(TEXT("continue_on_error"), bContinueOnError);
        Result->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : (bOverallOk ? TEXT("applied") : TEXT("partial_or_failed")));
        Result->SetNumberField(TEXT("step_count"), Steps.Num());
        Result->SetNumberField(TEXT("executed_step_count"), ExecutedStepCount);
        Result->SetNumberField(TEXT("planned_only_step_count"), PlannedOnlyStepCount);
        Result->SetNumberField(TEXT("failed_step_count"), FailedStepCount);
        Result->SetBoolField(TEXT("changed"), bReportedChanged);
        Result->SetBoolField(TEXT("changed_known"), bChangedKnown);
        Result->SetNumberField(TEXT("changed_unknown_step_count"), ChangedUnknownStepCount);
        Result->SetBoolField(TEXT("compiled"), bCompiled);
        Result->SetBoolField(TEXT("saved"), bSaved);
        Result->SetObjectField(TEXT("planned_counts"), MakeCountsObject(PlannedCounts));
        Result->SetObjectField(TEXT("applied_counts"), MakeCountsObject(AppliedCounts));
        Result->SetArrayField(TEXT("steps"), StepRows);
        Result->SetArrayField(TEXT("errors"), ErrorRows);
        return FMonolithActionResult::Success(Result);
    }
} // namespace MonolithUI::SpecActionsInternal


// Action register entry-point — called once from FMonolithUIModule::StartupModule.
// Declaration lives in Actions/MonolithUISpecActions.h.
void MonolithUI::FSpecActions::Register(FMonolithToolRegistry& Registry)
{
    using namespace MonolithUI::SpecActionsInternal;

    Registry.RegisterAction(
        TEXT("ui"), TEXT("build_ui_from_spec"),
        TEXT("Phase H — transactional UISpec -> UWidgetBlueprint builder. Parses a FUISpecDocument JSON, "
             "validates it, dry-walks the spec tree, gets-or-creates the WBP at asset_path, pre-creates "
             "referenced styles, walks the tree, compiles, rebuilds the post-compile widget-id map, "
             "saves. Atomic: any failure between get-or-create and save rolls back the new asset (or "
             "cancels the in-place edit transaction). Supports dry_run, overwrite, treat_warnings_as_errors, "
             "raw_mode (per-write allowlist bypass), request_id (echoed back). Returns "
             "{ bSuccess, asset_path, request_id?, validation, node_counts, errors?, warnings?, diff? }."),
        FMonolithActionHandler::CreateStatic(&HandleBuildUIFromSpec),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Long-package asset path, e.g. /Game/UI/MyMenu"))
            .Required(TEXT("spec"),       TEXT("object"), TEXT("FUISpecDocument JSON. Use ui::dump_ui_spec_schema for the shape."))
            .Optional(TEXT("overwrite"),  TEXT("boolean"), TEXT("Replace an existing WBP at asset_path. Default true."), TEXT("true"))
            .Optional(TEXT("dry_run"),    TEXT("boolean"), TEXT("Validate + walk + report a diff but do not commit. Default false."), TEXT("false"))
            .Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"), TEXT("Promote validator warnings to errors. Default false."), TEXT("false"))
            .Optional(TEXT("raw_mode"),   TEXT("boolean"), TEXT("Bypass the per-write allowlist gate. Default false."), TEXT("false"))
            .Optional(TEXT("request_id"), TEXT("string"),  TEXT("Caller-supplied UUID echoed back in the response."))
            .Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_ui_spec_schema"),
        TEXT("Phase H — JSON-Schema-style description of FUISpecDocument + live allowlist projection. "
             "Returns { schema_version, document_type, document_fields, node_fields, allowlist_by_type }. "
             "LLMs use this to build valid spec inputs without crawling our headers."),
        FMonolithActionHandler::CreateStatic(&HandleDumpUISpecSchema),
        FParamSchemaBuilder().Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("convert_markup_to_ui_spec"),
        TEXT("Read-only UMG XML/HTML-like markup importer. Converts strict XML-style markup into Monolith's "
             "canonical FUISpecDocument JSON without creating, loading, saving, or mutating any Widget Blueprint. "
             "Use before ui.build_ui_from_spec or future spec diff/patch workflows; this intentionally replaces "
             "direct apply_layout/apply_json_to_umg-style mutation with validated owner-action composition."),
        FMonolithActionHandler::CreateStatic(&HandleConvertMarkupToUISpec),
        FParamSchemaBuilder()
            .Required(TEXT("markup"), TEXT("string"), TEXT("XML-like UMG markup. Tags are Monolith UI type tokens such as VerticalBox, Button, TextBlock."))
            .Optional(TEXT("dialect"), TEXT("string"), TEXT("Markup dialect. Supported: umg_xml_v1, umg_html_v1, html. Default umg_xml_v1."), TEXT("umg_xml_v1"))
            .Optional(TEXT("strict"), TEXT("boolean"), TEXT("When true, unknown tags/attributes are errors. When false, they are warnings. Default true."), TEXT("true"))
            .Optional(TEXT("root_save_path"), TEXT("string"), TEXT("Optional future WBP path used only to derive spec.name and next actions; no asset is created."))
            .Optional(TEXT("spec_name"), TEXT("string"), TEXT("Optional FUISpecDocument name override."))
            .Optional(TEXT("parent_class"), TEXT("string"), TEXT("Optional parent class token/path. Default UserWidget."))
            .Optional(TEXT("source_name"), TEXT("string"), TEXT("Optional source file/name recorded in spec.metadata.sourceFile."))
            .Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"), TEXT("Mark conversion invalid when warnings exist. Default false."), TEXT("false"))
            .Optional(TEXT("request_id"), TEXT("string"), TEXT("Caller-supplied UUID echoed back in the response."))
            .Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("diff_ui_spec"),
        TEXT("Read-only UISpec diff against a live Widget Blueprint. Dumps the current WBP through the canonical "
             "FUISpec serializer, validates desired_spec, compares structural/properties/full modes, and emits "
             "stable-name patch candidates plus graph-binding preservation evidence for safe owner-action operations. "
             "This replaces raw export/apply JSON round-trips with explicit, inspectable design-data deltas."),
        FMonolithActionHandler::CreateStatic(&HandleDiffUISpec),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint long-package path to diff."))
            .Required(TEXT("desired_spec"), TEXT("object"), TEXT("Desired FUISpecDocument JSON, often from ui.convert_markup_to_ui_spec or a design-data converter."))
            .Optional(TEXT("compare_mode"), TEXT("string"), TEXT("structural, properties, or full. Default structural."), TEXT("structural"))
            .Optional(TEXT("request_id"), TEXT("string"), TEXT("Caller-supplied UUID echoed back in the response."))
            .Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("apply_ui_spec_patch"),
        TEXT("Confirm-gated UISpec patch workflow for existing Widget Blueprints. Accepts explicit stable-widget-name "
             "patch ops and routes them through existing Monolith owner actions such as ui.add_widget, ui.remove_widget, "
             "ui.move_widget, confirm-gated replace_widget decomposition through ui.remove_widget + ui.add_widget or preserve_children temp-add + ui.move_widget + ui.remove_widget + ui.rename_widget, ui.set_slot_property, ui.set_widget_property, ui.set_text, ui.set_image, ui.set_brush for supported non-Image brush resources, common and typed style routing via ui.set_widget_property, ui.apply_style_to_widget, ui.set_effect_surface_* owner actions, "
             "ui.compile_widget, and asset.save_asset. "
             "Dry-run is the default; mutating calls require confirm=true. Unsupported ops are reported, not converted to raw writes."),
        FMonolithActionHandler::CreateStatic(&HandleApplyUISpecPatch),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint long-package path to patch."))
            .Required(TEXT("patch"), TEXT("array"), TEXT("Array of patch op objects. Supported op values: add_widget, replace_widget, remove_widget, move_widget, set_slot_property, set_widget_property, set_text, set_image, set_brush, set_style, apply_style_to_widget, and existing set_effect_surface_* owner actions. replace_widget accepts confirm_replace=true and optional preserve_children=true with child_widget_names[]."))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Plan patch steps without mutation. Default true."), TEXT("true"))
            .Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when dry_run=false."), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile once after applying patch steps. Default true."), TEXT("true"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after successful mutation. Default false."), TEXT("false"))
            .Optional(TEXT("read_back"), TEXT("boolean"), TEXT("Dump post-apply FUISpec round-trip proof. Default true."), TEXT("true"))
            .Optional(TEXT("continue_on_error"), TEXT("boolean"), TEXT("Continue executing patch steps after a child action failure. Default false."), TEXT("false"))
            .Optional(TEXT("request_id"), TEXT("string"), TEXT("Caller-supplied UUID echoed back in the response."))
            .Build());

    // Phase J: ui::dump_ui_spec — inverse of build_ui_from_spec. Read a live
    // UWidgetBlueprint and emit a FUISpecDocument JSON suitable for round-
    // tripping. Pure read; no asset mutation. Mirrors the build response shape
    // so action surfaces can compose dump + build uniformly.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_ui_spec"),
        TEXT("Phase J — UISpec roundtrip serializer. Reads the UWidgetBlueprint at asset_path "
             "and produces a FUISpecDocument JSON that, when fed back into ui::build_ui_from_spec, "
             "reconstructs the same widget tree (up to the documented lossy boundary -- style asset "
             "class refs serialise as paths; native graph bindings serialise by name; rich curve "
             "tangents serialise as Linear envelopes). Covers ALL stock UMG panel-slot types "
             "(Canvas / Vertical / Horizontal / Overlay / ScrollBox / Grid / UniformGrid / SizeBox / "
             "ScaleBox / WrapBox / WidgetSwitcher / Border) and EffectSurface drop/inner shadow "
             "arrays. Returns { bSuccess, asset_path, request_id?, nodes_visited, animations_captured, "
             "spec, errors?, warnings? } where `spec` is the FUISpecDocument JSON ready for build."),
        FMonolithActionHandler::CreateStatic(&HandleDumpUISpec),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Long-package asset path of the WBP to read, e.g. /Game/UI/MyMenu"))
            .Optional(TEXT("emit_defaults"), TEXT("boolean"), TEXT("Include fields that match engine defaults. Default false."), TEXT("false"))
            .Optional(TEXT("request_id"), TEXT("string"), TEXT("Caller-supplied UUID echoed back in the response."))
            .Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("audit_widget_layout"),
        TEXT("Read-only structural UMG layout audit layered on top of FUISpecSerializer. "
             "Batch-dumps WidgetBlueprint specs from asset_paths or a path_prefix (default /Game/UI), "
             "then reports CanvasPanelSlot concentration, unwhitelisted absolute placement, edge-anchor "
             "alignment/negative-offset mismatches, dynamic text widgets without serialized wrap or width containment, "
             "guide-derived Canvas overuse, one-child Canvas wrappers, decorative hit-test blockers, hidden interactive "
             "layout space, button-like controls without serialized interactive-state style evidence, edge UI without "
             "SafeZone ancestry, non-UI-domain brush materials, and large static lists that should use ListView/TileView. "
             "Returns { bSuccess, status, summary, assets[], findings[] }. Warnings can be promoted with "
             "treat_warnings_as_errors=true."),
        FMonolithActionHandler::CreateStatic(&HandleAuditWidgetLayout),
        FParamSchemaBuilder()
            .Optional(TEXT("asset_paths"), TEXT("array"),
                TEXT("Optional explicit WidgetBlueprint long-package paths. Omit to scan path_prefix."))
            .Optional(TEXT("path_prefix"), TEXT("string"),
                TEXT("Content path scanned when asset_paths is omitted. Default /Game/UI."), TEXT("/Game/UI"))
            .Optional(TEXT("allowed_canvas_slots"), TEXT("array"),
                TEXT("Canvas allowlist identifiers: '*', asset path, widget id, asset::widget id, or asset::widget/path."))
            .Optional(TEXT("include_tests"), TEXT("boolean"),
                TEXT("Include /Game/Tests assets during path_prefix scans. Default false."), TEXT("false"))
            .Optional(TEXT("rule_profile"), TEXT("string"),
                TEXT("Lint profile: advisory, shipping, or strict. Default shipping; strict promotes static SafeZone misses to errors."), TEXT("shipping"))
            .Optional(TEXT("suppress_rule_ids"), TEXT("array"),
                TEXT("Optional rule IDs to suppress for known intentional cases, e.g. OneChildCanvasWrapper or EdgeUiMissingSafeZone."))
            .Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"),
                TEXT("Fail bSuccess when warning findings exist. Default false."), TEXT("false"))
            .Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("audit_widget_material_lifecycle"),
        TEXT("Read-only Widget Blueprint graph audit for dynamic material instance lifecycle safety. "
             "Scans K2 graphs for UMaterialInstanceDynamic::Create, GetDynamicMaterial, and related MID creation calls, "
             "then flags calls reachable from repeated UI lifecycle execution paths such as Tick, Paint, "
             "SynchronizeProperties, and Prepass. This is the canonical Monolith UI owner lint for DMI lifetime; "
             "material graph authoring remains in the material namespace and no external hlsl/material aliases are registered."),
        FMonolithActionHandler::CreateStatic(&HandleAuditWidgetMaterialLifecycle),
        FParamSchemaBuilder()
            .Optional(TEXT("asset_path"), TEXT("string"),
                TEXT("Single Widget Blueprint long-package path to audit."))
            .Optional(TEXT("asset_paths"), TEXT("array"),
                TEXT("Optional explicit WidgetBlueprint long-package paths. Omit asset_path/asset_paths to scan path_prefix."))
            .Optional(TEXT("path_prefix"), TEXT("string"),
                TEXT("Content path scanned when asset_path/asset_paths are omitted. Default /Game/UI."), TEXT("/Game/UI"))
            .Optional(TEXT("include_tests"), TEXT("boolean"),
                TEXT("Include /Game/Tests assets during path_prefix scans. Default false."), TEXT("false"))
            .Optional(TEXT("include_advisory"), TEXT("boolean"),
                TEXT("Emit warning findings for MID creation sites outside proven repeated lifecycle paths. Default true."), TEXT("true"))
            .Optional(TEXT("suppress_rule_ids"), TEXT("array"),
                TEXT("Optional rule IDs to suppress for known intentional cases, e.g. DynamicMaterialCreationSiteReview."))
            .Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"),
                TEXT("Fail bSuccess when warning findings exist. Default false."), TEXT("false"))
            .Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("measure_widget_layout"),
        TEXT("Read-only UMG layout measurement evidence. Dumps the WidgetBlueprint through the canonical "
             "FUISpec serializer, evaluates authored slot/style bounds for one or more screen profiles, "
             "and reports deterministic overlap and explicit safe-zone findings. This intentionally replaces "
             "weak get_layout_data/check_widget_overlap clones: v1 marks render geometry unavailable instead "
             "of using cached designer geometry; compose editor.capture_scene_preview and ui.verify_widget_visual_artifacts "
             "for visual proof."),
        FMonolithActionHandler::CreateStatic(&HandleMeasureWidgetLayout),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint long-package path to measure."))
            .Optional(TEXT("profiles"), TEXT("array"),
                TEXT("Optional [{name, resolution:[w,h], dpi_scale, safe_zone:{left,top,right,bottom}, visibility_filter:[...]}]. Default desktop 1920x1080."))
            .Optional(TEXT("check_overlap"), TEXT("boolean"),
                TEXT("Report sibling authored-layout overlaps. Default true."), TEXT("true"))
            .Optional(TEXT("check_safe_zone"), TEXT("boolean"),
                TEXT("Report widgets outside explicit profile safe_zone rectangles. Default true."), TEXT("true"))
            .Optional(TEXT("max_allowed_overlap_ratio"), TEXT("number"),
                TEXT("Allowed intersection ratio relative to the smaller sibling bounds. Default 0.0."), TEXT("0.0"))
            .Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("apply_common_menu_transform_spec"),
        TEXT("Apply a high-level CommonUI/Lyra menu transform spec. Composes existing actions for PrimaryGameLayout layer widgets, UIExtension points, widget properties/removal, Blueprint variable defaults, initial focus, navigation bulk writes, WBP subtree copy repair, Blueprint function/macro graph clone, Slate font repair, and frontend menu validation. Dry-run is the default; mutating calls require confirm=true."),
        FMonolithActionHandler::CreateStatic(&HandleApplyCommonMenuTransformSpec),
        FParamSchemaBuilder()
            .Optional(TEXT("spec"), TEXT("object"), TEXT("Nested transform spec object; when omitted, the payload itself is the spec"))
            .Optional(TEXT("screens"), TEXT("array|object"), TEXT("Screen map entries [{id, asset_path}] used by focus_table and nav_overrides; a single object is normalized to one entry"))
            .Optional(TEXT("layout_asset_path"), TEXT("string"), TEXT("Default PrimaryGameLayout WBP path used by layout_layers/layers"))
            .Optional(TEXT("layout_layers"), TEXT("array"), TEXT("Array of ui.add_primary_game_layout_layer child specs"))
            .Optional(TEXT("layers"), TEXT("array"), TEXT("Menu layer entries from build_menu_from_spec deferred_aggregation; layer_tag falls back to tag/id and asset_path falls back to layout_asset_path"))
            .Optional(TEXT("extension_points"), TEXT("array"), TEXT("Array of ui.add_extension_point_widget child specs"))
            .Optional(TEXT("widget_properties"), TEXT("array"), TEXT("Array of ui.set_widget_property child specs; widget/property aliases normalize to widget_name/property_name"))
            .Optional(TEXT("remove_widgets"), TEXT("array"), TEXT("Array of ui.remove_widget child specs; each entry accepts widget_name or widget_names[]"))
            .Optional(TEXT("variable_defaults"), TEXT("array"), TEXT("Array of blueprint.set_variable_defaults child specs; variable_name/value aliases normalize to name/default_value"))
            .Optional(TEXT("focus_table"), TEXT("array"), TEXT("[{screen|asset_path, target}] entries routed to ui.set_initial_focus_target"))
            .Optional(TEXT("initial_focus"), TEXT("array"), TEXT("Array of ui.set_initial_focus_target child specs; accepts asset_path/wbp_path plus target_widget/target"))
            .Optional(TEXT("desired_focus"), TEXT("array"), TEXT("Alias array for initial_focus"))
            .Optional(TEXT("nav_overrides"), TEXT("array"), TEXT("[{screen|asset_path, widget, direction, rule?, target?}] grouped into ui.set_widget_navigation_bulk"))
            .Optional(TEXT("navigation_bulk"), TEXT("array"), TEXT("Array of direct ui.set_widget_navigation_bulk child specs"))
            .Optional(TEXT("widget_subtrees"), TEXT("array"), TEXT("Array of ui.copy_widget_subtree_with_class_remap child specs"))
            .Optional(TEXT("blueprint_graphs"), TEXT("array"), TEXT("Array of blueprint.clone_graphs_with_reference_remap child specs"))
            .Optional(TEXT("font_repairs"), TEXT("array"), TEXT("Array of ui.repair_slate_font_references child specs"))
            .Optional(TEXT("frontend_validation"), TEXT("object"), TEXT("ui.validate_frontend_menu_flow validation spec run after transform steps"))
            .Optional(TEXT("class_remaps"), TEXT("object"), TEXT("Shared class remaps merged into widget_subtrees and blueprint_graphs when a child omits them"))
            .Optional(TEXT("object_remaps"), TEXT("object"), TEXT("Shared exact object remaps merged into widget_subtrees and blueprint_graphs when a child omits them"))
            .Optional(TEXT("root_remaps"), TEXT("object"), TEXT("Shared root remaps merged into child repair steps when a child omits them"))
            .Optional(TEXT("source_root"), TEXT("string"), TEXT("Shared source root shorthand; must be supplied with dest_root"))
            .Optional(TEXT("dest_root"), TEXT("string"), TEXT("Shared destination root shorthand; must be supplied with source_root"))
            .Optional(TEXT("raw_mode"), TEXT("boolean"), TEXT("Default raw_mode forwarded to widget_properties children when omitted"), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Default compile flag for child writers that expose compile"), TEXT("true"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("Default save flag for child writers that expose save"), TEXT("false"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Plan mutating child steps; child actions with native dry-run are executed in dry-run mode"), TEXT("true"))
            .Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when dry_run=false"), TEXT("false"))
            .Optional(TEXT("continue_on_error"), TEXT("boolean"), TEXT("Continue running remaining child steps after a child error"), TEXT("false"))
            .Optional(TEXT("request_id"), TEXT("string"), TEXT("Caller-supplied identifier echoed in the response"))
            .Build(),
        TEXT("Spec Builder"));

    FMonolithToolRegistry::Get().SetActionSearchMetadata(
        TEXT("ui"),
        TEXT("apply_common_menu_transform_spec"),
        { TEXT("CommonUI menu transform"), TEXT("Lyra frontend copy repair"), TEXT("build_menu deferred aggregation"), TEXT("PrimaryGameLayout layer focus navigation"), TEXT("post-copy UI repair orchestration") },
        { TEXT("apply_common_menu_spec"), TEXT("repair_copied_frontend_menu"), TEXT("apply_menu_deferred_aggregation"), TEXT("orchestrate_ui_copy_repair") },
        { TEXT("dry-run layers, focus_table, and nav_overrides emitted by build_menu_from_spec"), TEXT("apply a copied Lyra frontend menu transform with widget subtree repair, focus, navigation, and frontend validation") });

    // Phase 3 Item #18 (2026-05-16 UI Gap Audit) — build_menu_from_spec.
    // Always-on (not WITH_COMMONUI-gated): the spec system is the source
    // of the shared menu document grammar; per-screen WBPs may use CommonUI
    // types but the dispatch surface itself is engine-side.
    // per-screen `spec` builds run FULL via FUISpecBuilder; cross-screen
    // aggregation (layers / focus_table / nav_overrides) is deferred to
    // issue #3-18b. Same modes as build_ui_from_spec.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("build_menu_from_spec"),
        TEXT("Phase 3 Tier-3 — multi-screen menu document builder. Accepts {layers[], screens[], "
             "focus_table[], nav_overrides[]}. For each screens[N] entry that includes an embedded "
             "`spec` (FUISpecDocument), dispatches through the existing FUISpecBuilder pipeline "
             "(same atomicity + dry-run + strict-mode semantics as build_ui_from_spec). screens[N] "
             "entries without an embedded `spec` echo back as status='not_implemented' and bSuccess=false "
             "(kind-based scaffolder dispatch is not implemented in this action). layers / focus_table / nav_overrides are accepted, "
             "validated structurally, and echoed under `deferred_aggregation` so user-space tooling "
             "can post-process; the action returns status='partial_non_mutating' and bSuccess=false when "
             "those fields are present because the cross-screen activatable-stack hierarchy, focus-table CDO writes, "
             "and nav-override propagation are not applied. Modes (`dry_run`, `treat_warnings_as_errors`, "
             "`raw_mode`, `overwrite`) propagate to every per-screen build call. Returns "
             "{ bSuccess, status, screens[], aggregate_node_counts, errors?, warnings?, "
             "deferred_aggregation?, request_id? } where each screens[] entry includes a full "
             "build_result object (same shape as build_ui_from_spec)."),
        FMonolithActionHandler::CreateStatic(&HandleBuildMenuFromSpec),
        FParamSchemaBuilder()
            .Required(TEXT("screens"), TEXT("array"),
                TEXT("[{ id, asset_path, spec?, kind? }, ...] — each entry triggers a per-screen FUISpecBuilder "
                     "dispatch when `spec` is set. Without `spec`, the entry echoes status='not_implemented' and bSuccess=false."))
            .Optional(TEXT("layers"), TEXT("array"),
                TEXT("[{ id, screens[] }, ...] — activatable-stack layer hierarchy. Echoed back and not applied; response is partial_non_mutating."))
            .Optional(TEXT("focus_table"), TEXT("array"),
                TEXT("[{ screen, target }, ...] — per-screen DesiredFocusTargetName CDO writes. Echoed back and not applied; response is partial_non_mutating."))
            .Optional(TEXT("nav_overrides"), TEXT("array"),
                TEXT("[{ screen, widget, direction, target }, ...] — per-widget nav overrides. Echoed back and not applied; response is partial_non_mutating."))
            .Optional(TEXT("overwrite"), TEXT("boolean"),
                TEXT("Replace existing WBPs at each screen's asset_path. Default true."), TEXT("true"))
            .Optional(TEXT("dry_run"), TEXT("boolean"),
                TEXT("Validate + walk each per-screen spec; do not commit. Default false."), TEXT("false"))
            .Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"),
                TEXT("Promote validator warnings to errors. Default false."), TEXT("false"))
            .Optional(TEXT("raw_mode"), TEXT("boolean"),
                TEXT("Bypass the per-write allowlist gate on every per-screen build. Default false."), TEXT("false"))
            .Optional(TEXT("request_id"), TEXT("string"),
                TEXT("Caller-supplied UUID echoed back; per-screen builds receive '<request_id>:<screen.id>'."))
            .Build());
}
