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
#include "Containers/Map.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "WidgetBlueprint.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Sub)->Values)
            {
                const TSharedPtr<FJsonObject>* StyleObj = nullptr;
                if (Pair.Value.IsValid() && Pair.Value->TryGetObject(StyleObj) && StyleObj)
                {
                    FUISpecStyle Style;
                    ParseStyle(*StyleObj, Style);
                    OutDoc.Styles.Add(FName(*Pair.Key), Style);
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
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetNumberField(TEXT("version"), R.Document.Version);
        Spec->SetStringField(TEXT("name"), R.Document.Name);
        Spec->SetStringField(TEXT("parentClass"), R.Document.ParentClass);
        TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
        Meta->SetStringField(TEXT("authoringTool"), R.Document.Metadata.AuthoringTool);
        Meta->SetStringField(TEXT("sourceFile"),    R.Document.Metadata.SourceFile);
        Meta->SetStringField(TEXT("author"),        R.Document.Metadata.Author);
        Meta->SetStringField(TEXT("description"),   R.Document.Metadata.Description);
        Spec->SetObjectField(TEXT("metadata"), Meta);

        if (R.Document.Animations.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            Arr.Reserve(R.Document.Animations.Num());
            for (const FUISpecAnimation& A : R.Document.Animations)
            {
                TSharedPtr<FJsonObject> AObj = MakeShared<FJsonObject>();
                AObj->SetStringField(TEXT("name"),           A.Name.ToString());
                AObj->SetStringField(TEXT("targetWidgetId"), A.TargetWidgetId.ToString());
                AObj->SetStringField(TEXT("targetProperty"), A.TargetProperty.ToString());
                AObj->SetNumberField(TEXT("duration"),       A.Duration);
                AObj->SetNumberField(TEXT("delay"),          A.Delay);
                if (!A.Easing.IsNone())   AObj->SetStringField(TEXT("easing"),   A.Easing.ToString());
                if (!A.LoopMode.IsNone()) AObj->SetStringField(TEXT("loopMode"), A.LoopMode.ToString());
                AObj->SetBoolField(TEXT("autoPlay"), A.bAutoPlay);
                if (A.Keyframes.Num() > 0)
                {
                    TArray<TSharedPtr<FJsonValue>> Kfs;
                    Kfs.Reserve(A.Keyframes.Num());
                    for (const FUISpecKeyframe& K : A.Keyframes)
                    {
                        TSharedPtr<FJsonObject> KO = MakeShared<FJsonObject>();
                        KO->SetNumberField(TEXT("time"),         K.Time);
                        KO->SetNumberField(TEXT("scalarValue"),  K.ScalarValue);
                        if (!K.Easing.IsNone()) KO->SetStringField(TEXT("easing"), K.Easing.ToString());
                        Kfs.Add(MakeShared<FJsonValueObject>(KO));
                    }
                    AObj->SetArrayField(TEXT("keyframes"), Kfs);
                }
                Arr.Add(MakeShared<FJsonValueObject>(AObj));
            }
            Spec->SetArrayField(TEXT("animations"), Arr);
        }

        if (R.Document.Root.IsValid())
        {
            Spec->SetObjectField(TEXT("rootWidget"), NodeToJson(*R.Document.Root));
        }

        Out->SetObjectField(TEXT("spec"), Spec);

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
        TArray<TSharedPtr<FJsonValue>>& Findings;
        TMap<FString, int32> SlotCounts;
        int32 NodeCount = 0;
        int32 CanvasSlotCount = 0;
        int32 ErrorCount = 0;
        int32 WarningCount = 0;

        FWidgetLayoutAuditAccumulator(
            const FString& InAssetPath,
            const TSet<FString>& InAllowedCanvasSlots,
            TArray<TSharedPtr<FJsonValue>>& InFindings)
            : AssetPath(InAssetPath)
            , AllowedCanvasSlots(InAllowedCanvasSlots)
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
        TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
        Finding->SetStringField(TEXT("severity"), Severity);
        Finding->SetStringField(TEXT("category"), Category);
        Finding->SetStringField(TEXT("asset_path"), Acc.AssetPath);
        Finding->SetStringField(TEXT("widget_id"), WidgetId.ToString());
        Finding->SetStringField(TEXT("widget_path"), WidgetPath);
        Finding->SetStringField(TEXT("message"), Message);
        Finding->SetStringField(TEXT("suggested_fix"), SuggestedFix);
        if (Details.IsValid())
        {
            Finding->SetObjectField(TEXT("details"), Details);
        }

        if (FCString::Stricmp(Severity, TEXT("error")) == 0)
        {
            ++Acc.ErrorCount;
        }
        else if (FCString::Stricmp(Severity, TEXT("warning")) == 0)
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

    static bool NodeAddsWidthBound(const FUISpecNode& Node, bool bCanvasSlot)
    {
        return Node.Style.Width > 0.f
            || (Node.Style.bOverrideMaxDesiredWidth && Node.Style.MaxDesiredWidth > 0.f)
            || (bCanvasSlot && Node.Slot.Size.X > 0.f && !Node.Slot.bAutoSize);
    }

    static void AuditWidgetNodeRecursive(
        const FUISpecNode& Node,
        const FName& ParentType,
        const FString& WidgetPath,
        bool bInheritedWidthBound,
        FWidgetLayoutAuditAccumulator& Acc)
    {
        ++Acc.NodeCount;

        const bool bCanvasSlot = ParentType == FName(TEXT("CanvasPanel"));
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

        for (const TSharedPtr<FUISpecNode>& Child : Node.Children)
        {
            if (!Child.IsValid())
            {
                continue;
            }

            const FString ChildPath = WidgetPath.IsEmpty()
                ? Child->Id.ToString()
                : FString::Printf(TEXT("%s/%s"), *WidgetPath, *Child->Id.ToString());
            AuditWidgetNodeRecursive(*Child, Node.Type, ChildPath, bHasWidthBound, Acc);
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
        FString ParamError;
        if (!TryReadStringArrayParam(Params, TEXT("asset_paths"), AssetPaths, ParamError)
            || !TryReadStringArrayParam(Params, TEXT("allowed_canvas_slots"), AllowedCanvasSlots, ParamError))
        {
            return FMonolithActionResult::Error(ParamError, -32602);
        }

        bool bIncludeTests = false;
        bool bTreatWarningsAsErrors = false;
        Params->TryGetBoolField(TEXT("include_tests"), bIncludeTests);
        Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);

        FString PathPrefix = TEXT("/Game/UI");
        Params->TryGetStringField(TEXT("path_prefix"), PathPrefix);
        if (AssetPaths.Num() == 0)
        {
            CollectWidgetBlueprintAssetPaths(PathPrefix, bIncludeTests, AssetPaths);
        }

        TSet<FString> AllowedSet;
        for (const FString& Identifier : AllowedCanvasSlots)
        {
            AllowedSet.Add(Identifier);
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

            FWidgetLayoutAuditAccumulator Acc(AssetPath, AllowedSet, Findings);
            AuditWidgetNodeRecursive(
                *DumpResult.Document.Root,
                NAME_None,
                DumpResult.Document.Root->Id.ToString(),
                /*bInheritedWidthBound=*/false,
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

    static bool ResolveCommonMenuSpecObject(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutSpec, FString& OutError)
    {
        if (!Params.IsValid())
        {
            OutError = TEXT("apply_common_menu_transform_spec requires an object payload.");
            return false;
        }

        if (!Params->HasField(TEXT("spec")))
        {
            OutSpec = Params;
            return true;
        }

        const TSharedPtr<FJsonObject>* SpecObject = nullptr;
        if (!Params->TryGetObjectField(TEXT("spec"), SpecObject) || !SpecObject || !SpecObject->IsValid())
        {
            OutError = TEXT("spec must be an object when provided.");
            return false;
        }
        OutSpec = *SpecObject;
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
        const TCHAR* FieldC = nullptr)
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
        return FString();
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
             "alignment/negative-offset mismatches, and dynamic text widgets without serialized wrap or width containment. "
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
            .Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"),
                TEXT("Fail bSuccess when warning findings exist. Default false."), TEXT("false"))
            .Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("apply_common_menu_transform_spec"),
        TEXT("Apply a high-level CommonUI/Lyra menu transform spec. Composes existing actions for PrimaryGameLayout layer widgets, UIExtension points, widget properties/removal, Blueprint variable defaults, initial focus, navigation bulk writes, WBP subtree copy repair, Blueprint function/macro graph clone, Slate font repair, and frontend menu validation. Dry-run is the default; mutating calls require confirm=true."),
        FMonolithActionHandler::CreateStatic(&HandleApplyCommonMenuTransformSpec),
        FParamSchemaBuilder()
            .Optional(TEXT("spec"), TEXT("object"), TEXT("Nested transform spec object; when omitted, the payload itself is the spec"))
            .Optional(TEXT("screens"), TEXT("array"), TEXT("Screen map entries [{id, asset_path}] used by focus_table and nav_overrides"))
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
