// MonolithUISlotActions.cpp
#include "MonolithUISlotActions.h"
#include "MonolithUIInternal.h"
#include "MonolithUIMoveWidgetTransaction.h"
#include "MonolithParamUtils.h"
#include "MonolithParamSchema.h"
#include "Components/UniformGridSlot.h"
#include "Components/GridSlot.h"

#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/VerticalBoxSlot.h"

namespace
{
    static const TArray<FString>& GetBoxSlotSizeRuleTokens()
    {
        static const TArray<FString> Tokens = {
            TEXT("Automatic"),
            TEXT("Fill")
        };
        return Tokens;
    }

    static bool TryParseBoxSlotSizeRule(const FString& Token, ESlateSizeRule::Type& OutRule)
    {
        if (Token == TEXT("Automatic"))
        {
            OutRule = ESlateSizeRule::Automatic;
            return true;
        }
        if (Token == TEXT("Fill"))
        {
            OutRule = ESlateSizeRule::Fill;
            return true;
        }
        return false;
    }

    enum class EMonolithUISlotSnapshotKind
    {
        None,
        Canvas,
        VerticalBox,
        HorizontalBox,
        Overlay
    };

    struct FMonolithUISlotSnapshot
    {
        EMonolithUISlotSnapshotKind Kind = EMonolithUISlotSnapshotKind::None;
        FString SlotType;
        FAnchorData CanvasLayout;
        bool bCanvasAutoSize = false;
        int32 CanvasZOrder = 0;
        FSlateChildSize BoxSize;
        FMargin Padding;
        EHorizontalAlignment HorizontalAlignment = HAlign_Fill;
        EVerticalAlignment VerticalAlignment = VAlign_Fill;

        bool IsValid() const
        {
            return Kind != EMonolithUISlotSnapshotKind::None;
        }
    };

    static void AddCopiedField(TArray<TSharedPtr<FJsonValue>>& Fields, const TCHAR* FieldName)
    {
        Fields.Add(MakeShared<FJsonValueString>(FieldName));
    }

    static FMonolithUISlotSnapshot CaptureCompatibleSlotProperties(UPanelSlot* Slot)
    {
        FMonolithUISlotSnapshot Snapshot;
        if (!Slot)
        {
            return Snapshot;
        }

        Snapshot.SlotType = Slot->GetClass()->GetName();

        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
        {
            Snapshot.Kind = EMonolithUISlotSnapshotKind::Canvas;
            Snapshot.CanvasLayout = CanvasSlot->GetLayout();
            Snapshot.bCanvasAutoSize = CanvasSlot->GetAutoSize();
            Snapshot.CanvasZOrder = CanvasSlot->GetZOrder();
            return Snapshot;
        }

        if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(Slot))
        {
            Snapshot.Kind = EMonolithUISlotSnapshotKind::VerticalBox;
            Snapshot.BoxSize = VerticalSlot->GetSize();
            Snapshot.Padding = VerticalSlot->GetPadding();
            Snapshot.HorizontalAlignment = VerticalSlot->GetHorizontalAlignment();
            Snapshot.VerticalAlignment = VerticalSlot->GetVerticalAlignment();
            return Snapshot;
        }

        if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(Slot))
        {
            Snapshot.Kind = EMonolithUISlotSnapshotKind::HorizontalBox;
            Snapshot.BoxSize = HorizontalSlot->GetSize();
            Snapshot.Padding = HorizontalSlot->GetPadding();
            Snapshot.HorizontalAlignment = HorizontalSlot->GetHorizontalAlignment();
            Snapshot.VerticalAlignment = HorizontalSlot->GetVerticalAlignment();
            return Snapshot;
        }

        if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
        {
            Snapshot.Kind = EMonolithUISlotSnapshotKind::Overlay;
            Snapshot.Padding = OverlaySlot->GetPadding();
            Snapshot.HorizontalAlignment = OverlaySlot->GetHorizontalAlignment();
            Snapshot.VerticalAlignment = OverlaySlot->GetVerticalAlignment();
            return Snapshot;
        }

        return Snapshot;
    }

    static TSharedPtr<FJsonObject> RestoreCompatibleSlotProperties(
        const FMonolithUISlotSnapshot& Snapshot,
        UPanelSlot* NewSlot,
        int32 OldParentIndex,
        int32 NewParentIndex)
    {
        TSharedPtr<FJsonObject> Preservation = MakeShared<FJsonObject>();
        Preservation->SetBoolField(TEXT("attempted"), Snapshot.IsValid());
        Preservation->SetStringField(TEXT("old_slot_type"), Snapshot.SlotType.IsEmpty() ? TEXT("none") : Snapshot.SlotType);
        Preservation->SetStringField(TEXT("new_slot_type"), NewSlot ? NewSlot->GetClass()->GetName() : TEXT("none"));
        Preservation->SetNumberField(TEXT("old_parent_index"), OldParentIndex);
        Preservation->SetNumberField(TEXT("new_parent_index"), NewParentIndex);

        TArray<TSharedPtr<FJsonValue>> CopiedFields;

        if (!Snapshot.IsValid())
        {
            Preservation->SetStringField(TEXT("status"), TEXT("unsupported_old_slot"));
            Preservation->SetArrayField(TEXT("copied_fields"), CopiedFields);
            return Preservation;
        }

        if (!NewSlot)
        {
            Preservation->SetStringField(TEXT("status"), TEXT("failed_no_new_slot"));
            Preservation->SetArrayField(TEXT("copied_fields"), CopiedFields);
            return Preservation;
        }

        if (Snapshot.Kind == EMonolithUISlotSnapshotKind::Canvas)
        {
            if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(NewSlot))
            {
                CanvasSlot->SetLayout(Snapshot.CanvasLayout);
                CanvasSlot->SetAutoSize(Snapshot.bCanvasAutoSize);
                CanvasSlot->SetZOrder(Snapshot.CanvasZOrder);
                AddCopiedField(CopiedFields, TEXT("layout"));
                AddCopiedField(CopiedFields, TEXT("auto_size"));
                AddCopiedField(CopiedFields, TEXT("z_order"));
                Preservation->SetStringField(TEXT("status"), TEXT("preserved"));
                Preservation->SetArrayField(TEXT("copied_fields"), CopiedFields);
                return Preservation;
            }
        }
        else if (Snapshot.Kind == EMonolithUISlotSnapshotKind::VerticalBox)
        {
            if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(NewSlot))
            {
                VerticalSlot->SetSize(Snapshot.BoxSize);
                VerticalSlot->SetPadding(Snapshot.Padding);
                VerticalSlot->SetHorizontalAlignment(Snapshot.HorizontalAlignment);
                VerticalSlot->SetVerticalAlignment(Snapshot.VerticalAlignment);
                AddCopiedField(CopiedFields, TEXT("size"));
                AddCopiedField(CopiedFields, TEXT("padding"));
                AddCopiedField(CopiedFields, TEXT("h_align"));
                AddCopiedField(CopiedFields, TEXT("v_align"));
                Preservation->SetStringField(TEXT("status"), TEXT("preserved"));
                Preservation->SetArrayField(TEXT("copied_fields"), CopiedFields);
                return Preservation;
            }
        }
        else if (Snapshot.Kind == EMonolithUISlotSnapshotKind::HorizontalBox)
        {
            if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(NewSlot))
            {
                HorizontalSlot->SetSize(Snapshot.BoxSize);
                HorizontalSlot->SetPadding(Snapshot.Padding);
                HorizontalSlot->SetHorizontalAlignment(Snapshot.HorizontalAlignment);
                HorizontalSlot->SetVerticalAlignment(Snapshot.VerticalAlignment);
                AddCopiedField(CopiedFields, TEXT("size"));
                AddCopiedField(CopiedFields, TEXT("padding"));
                AddCopiedField(CopiedFields, TEXT("h_align"));
                AddCopiedField(CopiedFields, TEXT("v_align"));
                Preservation->SetStringField(TEXT("status"), TEXT("preserved"));
                Preservation->SetArrayField(TEXT("copied_fields"), CopiedFields);
                return Preservation;
            }
        }
        else if (Snapshot.Kind == EMonolithUISlotSnapshotKind::Overlay)
        {
            if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(NewSlot))
            {
                OverlaySlot->SetPadding(Snapshot.Padding);
                OverlaySlot->SetHorizontalAlignment(Snapshot.HorizontalAlignment);
                OverlaySlot->SetVerticalAlignment(Snapshot.VerticalAlignment);
                AddCopiedField(CopiedFields, TEXT("padding"));
                AddCopiedField(CopiedFields, TEXT("h_align"));
                AddCopiedField(CopiedFields, TEXT("v_align"));
                Preservation->SetStringField(TEXT("status"), TEXT("preserved"));
                Preservation->SetArrayField(TEXT("copied_fields"), CopiedFields);
                return Preservation;
            }
        }

        Preservation->SetStringField(TEXT("status"), TEXT("slot_class_changed"));
        Preservation->SetArrayField(TEXT("copied_fields"), CopiedFields);
        return Preservation;
    }
}

void FMonolithUISlotActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_slot_property"),
        TEXT("Set a slot property on a widget (anchors, offsets, padding, alignment, box size rule, z-order)"),
        FMonolithActionHandler::CreateStatic(&HandleSetSlotProperty),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget name"))
            .Optional(TEXT("anchors"), TEXT("object"), TEXT("Canvas anchors: {\"min_x\":0, \"min_y\":0, \"max_x\":1, \"max_y\":1}"))
            .Optional(TEXT("offsets"), TEXT("object"), TEXT("Canvas offsets: {\"left\":0, \"top\":0, \"right\":0, \"bottom\":0}"))
            .Optional(TEXT("position"), TEXT("object"), TEXT("Canvas position: {\"x\":0, \"y\":0}"))
            .Optional(TEXT("size"), TEXT("object"), TEXT("Canvas size: {\"x\":200, \"y\":50}"))
            .Optional(TEXT("alignment"), TEXT("object"), TEXT("Canvas alignment: {\"x\":0.5, \"y\":0.5}"))
            .Optional(TEXT("z_order"), TEXT("integer"), TEXT("Canvas z-order"))
            .Optional(TEXT("auto_size"), TEXT("boolean"), TEXT("Canvas auto-size"))
            .Optional(TEXT("h_align"), TEXT("string"), TEXT("Horizontal alignment: Left, Center, Right, Fill"))
            .Optional(TEXT("v_align"), TEXT("string"), TEXT("Vertical alignment: Top, Center, Bottom, Fill"))
            .Optional(TEXT("size_rule"), TEXT("string"), TEXT("VerticalBoxSlot/HorizontalBoxSlot size rule: Automatic or Fill")).Enum(TEXT("size_rule"), { TEXT("Automatic"), TEXT("Fill") })
            .Optional(TEXT("fill_weight"), TEXT("number"), TEXT("VerticalBoxSlot/HorizontalBoxSlot fill weight; must be finite and >= 0")).Minimum(TEXT("fill_weight"), 0.0)
            .Optional(TEXT("padding"), TEXT("object"), TEXT("Slot padding: {\"left\":0, \"top\":0, \"right\":0, \"bottom\":0}"))
            .Optional(TEXT("row"), TEXT("integer"), TEXT("Grid slot row index (UniformGrid/Grid slots)"))
            .Optional(TEXT("column"), TEXT("integer"), TEXT("Grid slot column index (UniformGrid/Grid slots)"))
            .Optional(TEXT("row_span"), TEXT("integer"), TEXT("Grid slot row span (Grid slots only)"))
            .Optional(TEXT("column_span"), TEXT("integer"), TEXT("Grid slot column span (Grid slots only)"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_anchor_preset"),
        TEXT("Set anchor to a named preset (center, top_left, stretch_fill, etc.)"),
        FMonolithActionHandler::CreateStatic(&HandleSetAnchorPreset),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget name"))
            .Required(TEXT("preset"), TEXT("string"), TEXT("Preset name: top_left, top_center, top_right, center_left, center, center_right, bottom_left, bottom_center, bottom_right, stretch_horizontal, stretch_vertical, stretch_fill, stretch_top, stretch_bottom, stretch_left, stretch_right"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("move_widget"),
        TEXT("Move a widget to a parent panel or reorder it within its current panel, preserving compatible slot layout data"),
        FMonolithActionHandler::CreateStatic(&HandleMoveWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to move"))
            .Required(TEXT("new_parent_name"), TEXT("string"), TEXT("New parent panel name"))
            .Optional(TEXT("sibling_index"), TEXT("integer"), TEXT("Zero-based final child index in the target panel; same-parent reorders preserve the exact slot object")).Minimum(TEXT("sibling_index"), 0.0)
            .Optional(TEXT("expected_parent_name"), TEXT("string"), TEXT("Optional fail-closed assertion for the widget's current direct parent"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after moving"), TEXT("true"))
            .Build()
    );
}

// --- set_slot_property ---
FMonolithActionResult FMonolithUISlotActions::HandleSetSlotProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FMonolithActionResult ParamError;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)) return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError)) return ParamError;

    // UE's JSON string values implement TryGetBool via FString::ToBool, so a
    // direct TryGetBoolField call accepts inputs such as "yes". Validate the
    // exact wire type before loading or mutating the Widget Blueprint.
    FString BoolParamError;
    const bool bHasAutoSize = Params->HasField(TEXT("auto_size"));
    bool bAutoSize = false;
    if (bHasAutoSize
        && !MonolithParamUtils::GetOptionalBoolParam(
            Params,
            TEXT("auto_size"),
            bAutoSize,
            BoolParamError,
            false))
    {
        return FMonolithActionResult::Error(BoolParamError, FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bCompile = false;
    if (!MonolithParamUtils::GetOptionalBoolParam(
        Params,
        TEXT("compile"),
        bCompile,
        BoolParamError,
        false))
    {
        return FMonolithActionResult::Error(BoolParamError, FMonolithJsonUtils::ErrInvalidParams);
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        // Phase K
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Lookup"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' not found in WBP '%s'."), *WidgetName, *AssetPath),
            TEXT("Call ui::get_widget_tree to enumerate live widget names."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    UPanelSlot* Slot = Widget->Slot;
    if (!Slot)
    {
        // Phase K — root widget has no slot. Common LLM mistake; suggested_fix
        // calls out the typical cause.
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Slot"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' has no slot — likely the WidgetTree root."), *WidgetName),
            TEXT("Slot properties only apply to non-root widgets. Use a parent panel and address its child."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    const bool bHasSizeRule = Params->HasField(TEXT("size_rule"));
    FString SizeRuleToken;
    ESlateSizeRule::Type ParsedSizeRule = ESlateSizeRule::Automatic;
    if (bHasSizeRule)
    {
        if (!Params->TryGetStringField(TEXT("size_rule"), SizeRuleToken)
            || !TryParseBoxSlotSizeRule(SizeRuleToken, ParsedSizeRule))
        {
            FUISpecError E = MonolithUIInternal::MakeSpecError(
                TEXT("Enum"),
                TEXT("/size_rule"),
                FString::Printf(TEXT("Unknown box slot size rule '%s'."), *SizeRuleToken),
                TEXT("Use one of the listed canonical size-rule tokens."),
                GetBoxSlotSizeRuleTokens());
            E.WidgetId = FName(*WidgetName);
            return MonolithUIInternal::MakeErrorFromSpecError(E);
        }
    }

    const bool bHasFillWeight = Params->HasField(TEXT("fill_weight"));
    double FillWeight = 0.0;
    if (bHasFillWeight
        && (!Params->TryGetNumberField(TEXT("fill_weight"), FillWeight)
            || !FMath::IsFinite(FillWeight)
            || FillWeight < 0.0))
    {
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Range"),
            TEXT("/fill_weight"),
            TEXT("fill_weight must be a finite number greater than or equal to zero."),
            TEXT("Pass a finite non-negative box-slot fill coefficient."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    const bool bHasBoxSizeField = bHasSizeRule || bHasFillWeight;
    const bool bIsBoxSlot = Cast<UVerticalBoxSlot>(Slot) || Cast<UHorizontalBoxSlot>(Slot);
    if (bHasBoxSizeField && !bIsBoxSlot)
    {
        const TCHAR* BoxSizeJsonPath = bHasSizeRule ? TEXT("/size_rule") : TEXT("/fill_weight");
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("SlotClass"),
            BoxSizeJsonPath,
            FString::Printf(
                TEXT("Widget '%s' uses %s; size_rule and fill_weight require a VerticalBoxSlot or HorizontalBoxSlot."),
                *WidgetName,
                *Slot->GetClass()->GetName()),
            TEXT("Re-parent the widget under a VerticalBox or HorizontalBox, or remove the box-only fields."),
            { TEXT("VerticalBoxSlot"), TEXT("HorizontalBoxSlot") });
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    int32 PropsSet = 0;

    if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Slot))
    {
        const TSharedPtr<FJsonObject>* AnchorObj = nullptr;
        if (Params->TryGetObjectField(TEXT("anchors"), AnchorObj))
        {
            double MinX = 0.0, MinY = 0.0, MaxX = 0.0, MaxY = 0.0;
            const TSharedPtr<FJsonValue> MinXField = (*AnchorObj)->TryGetField(TEXT("min_x"));
            if (MinXField.IsValid() && !MinXField->TryGetNumber(MinX)) return FMonolithActionResult::Error(TEXT("Invalid param: anchors.min_x must be a number"));
            const TSharedPtr<FJsonValue> MinYField = (*AnchorObj)->TryGetField(TEXT("min_y"));
            if (MinYField.IsValid() && !MinYField->TryGetNumber(MinY)) return FMonolithActionResult::Error(TEXT("Invalid param: anchors.min_y must be a number"));
            const TSharedPtr<FJsonValue> MaxXField = (*AnchorObj)->TryGetField(TEXT("max_x"));
            if (MaxXField.IsValid() && !MaxXField->TryGetNumber(MaxX)) return FMonolithActionResult::Error(TEXT("Invalid param: anchors.max_x must be a number"));
            const TSharedPtr<FJsonValue> MaxYField = (*AnchorObj)->TryGetField(TEXT("max_y"));
            if (MaxYField.IsValid() && !MaxYField->TryGetNumber(MaxY)) return FMonolithActionResult::Error(TEXT("Invalid param: anchors.max_y must be a number"));

            FAnchors A(MinX, MinY, MaxX, MaxY);
            CS->SetAnchors(A);
            PropsSet++;
        }

        const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
        if (Params->TryGetObjectField(TEXT("offsets"), OffsetObj))
        {
            double Left = 0.0, Top = 0.0, Right = 0.0, Bottom = 0.0;
            if ((*OffsetObj)->HasField(TEXT("left")) && !(*OffsetObj)->TryGetNumberField(TEXT("left"), Left)) return FMonolithActionResult::Error(TEXT("Invalid param: offsets.left must be a number"));
            if ((*OffsetObj)->HasField(TEXT("top")) && !(*OffsetObj)->TryGetNumberField(TEXT("top"), Top)) return FMonolithActionResult::Error(TEXT("Invalid param: offsets.top must be a number"));
            if ((*OffsetObj)->HasField(TEXT("right")) && !(*OffsetObj)->TryGetNumberField(TEXT("right"), Right)) return FMonolithActionResult::Error(TEXT("Invalid param: offsets.right must be a number"));
            if ((*OffsetObj)->HasField(TEXT("bottom")) && !(*OffsetObj)->TryGetNumberField(TEXT("bottom"), Bottom)) return FMonolithActionResult::Error(TEXT("Invalid param: offsets.bottom must be a number"));
            FMargin Offsets(Left, Top, Right, Bottom);
            CS->SetOffsets(Offsets);
            PropsSet++;
        }

        const TSharedPtr<FJsonObject>* PosObj = nullptr;
        if (Params->TryGetObjectField(TEXT("position"), PosObj))
        {
            double X = 0.0, Y = 0.0;
            if ((*PosObj)->HasField(TEXT("x")) && !(*PosObj)->TryGetNumberField(TEXT("x"), X)) return FMonolithActionResult::Error(TEXT("Invalid param: position.x must be a number"));
            if ((*PosObj)->HasField(TEXT("y")) && !(*PosObj)->TryGetNumberField(TEXT("y"), Y)) return FMonolithActionResult::Error(TEXT("Invalid param: position.y must be a number"));
            CS->SetPosition(FVector2D(X, Y));
            PropsSet++;
        }

        const TSharedPtr<FJsonObject>* SizeObj = nullptr;
        if (Params->TryGetObjectField(TEXT("size"), SizeObj))
        {
            double X = 0.0, Y = 0.0;
            if ((*SizeObj)->HasField(TEXT("x")) && !(*SizeObj)->TryGetNumberField(TEXT("x"), X)) return FMonolithActionResult::Error(TEXT("Invalid param: size.x must be a number"));
            if ((*SizeObj)->HasField(TEXT("y")) && !(*SizeObj)->TryGetNumberField(TEXT("y"), Y)) return FMonolithActionResult::Error(TEXT("Invalid param: size.y must be a number"));
            CS->SetSize(FVector2D(X, Y));
            PropsSet++;
        }

        const TSharedPtr<FJsonObject>* AlignObj = nullptr;
        if (Params->TryGetObjectField(TEXT("alignment"), AlignObj))
        {
            double X = 0.0, Y = 0.0;
            if ((*AlignObj)->HasField(TEXT("x")) && !(*AlignObj)->TryGetNumberField(TEXT("x"), X)) return FMonolithActionResult::Error(TEXT("Invalid param: alignment.x must be a number"));
            if ((*AlignObj)->HasField(TEXT("y")) && !(*AlignObj)->TryGetNumberField(TEXT("y"), Y)) return FMonolithActionResult::Error(TEXT("Invalid param: alignment.y must be a number"));
            CS->SetAlignment(FVector2D(X, Y));
            PropsSet++;
        }

        double ZOrderVal;
        if (Params->HasField(TEXT("z_order")))
        {
            if (!Params->TryGetNumberField(TEXT("z_order"), ZOrderVal)) return FMonolithActionResult::Error(TEXT("Invalid param: z_order must be a number"));
            CS->SetZOrder(static_cast<int32>(ZOrderVal));
            PropsSet++;
        }

        if (bHasAutoSize)
        {
            CS->SetAutoSize(bAutoSize);
            PropsSet++;
        }
    }

    // Alignment for box/overlay slots
    FString HAlign;
    Params->TryGetStringField(TEXT("h_align"), HAlign);
    FString VAlign;
    Params->TryGetStringField(TEXT("v_align"), VAlign);
    auto ParseHAlign = [](const FString& S) -> EHorizontalAlignment {
        if (S == TEXT("Left")) return HAlign_Left;
        if (S == TEXT("Center")) return HAlign_Center;
        if (S == TEXT("Right")) return HAlign_Right;
        return HAlign_Fill;
    };
    auto ParseVAlign = [](const FString& S) -> EVerticalAlignment {
        if (S == TEXT("Top")) return VAlign_Top;
        if (S == TEXT("Center")) return VAlign_Center;
        if (S == TEXT("Bottom")) return VAlign_Bottom;
        return VAlign_Fill;
    };

    if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty()) { VS->SetHorizontalAlignment(ParseHAlign(HAlign)); PropsSet++; }
        if (!VAlign.IsEmpty()) { VS->SetVerticalAlignment(ParseVAlign(VAlign)); PropsSet++; }
        if (bHasBoxSizeField)
        {
            FSlateChildSize BoxSize = VS->GetSize();
            if (bHasSizeRule) BoxSize.SizeRule = ParsedSizeRule;
            if (bHasFillWeight) BoxSize.Value = static_cast<float>(FillWeight);
            VS->SetSize(BoxSize);
            PropsSet += static_cast<int32>(bHasSizeRule) + static_cast<int32>(bHasFillWeight);
        }
    }
    else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty()) { HS->SetHorizontalAlignment(ParseHAlign(HAlign)); PropsSet++; }
        if (!VAlign.IsEmpty()) { HS->SetVerticalAlignment(ParseVAlign(VAlign)); PropsSet++; }
        if (bHasBoxSizeField)
        {
            FSlateChildSize BoxSize = HS->GetSize();
            if (bHasSizeRule) BoxSize.SizeRule = ParsedSizeRule;
            if (bHasFillWeight) BoxSize.Value = static_cast<float>(FillWeight);
            HS->SetSize(BoxSize);
            PropsSet += static_cast<int32>(bHasSizeRule) + static_cast<int32>(bHasFillWeight);
        }
    }
    else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
    {
        if (!HAlign.IsEmpty()) { OS->SetHorizontalAlignment(ParseHAlign(HAlign)); PropsSet++; }
        if (!VAlign.IsEmpty()) { OS->SetVerticalAlignment(ParseVAlign(VAlign)); PropsSet++; }
    }

    // Padding
    const TSharedPtr<FJsonObject>* PadObj = nullptr;
    if (Params->TryGetObjectField(TEXT("padding"), PadObj))
    {
        FMargin Pad;
        if (MonolithUIInternal::TryParseMargin(PadObj, Pad))
        {
            if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot)) { VS->SetPadding(Pad); PropsSet++; }
            else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot)) { HS->SetPadding(Pad); PropsSet++; }
            else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot)) { OS->SetPadding(Pad); PropsSet++; }
        }
    }

    // Grid slots (UniformGridPanel / GridPanel). Mirrors the Canvas branch: one
    // Cast per slot type, HasField-gated writes, PropsSet++ per applied property.
    // UniformGrid shares one cell size and has NO spans; Grid supports row/column
    // spans plus per-slot padding. h_align/v_align reuse the parsers above.
    if (UUniformGridSlot* UGS = Cast<UUniformGridSlot>(Slot))
    {
        double UGSRow = 0; if (Params->HasField(TEXT("row")))    { if (!Params->TryGetNumberField(TEXT("row"), UGSRow)) return FMonolithActionResult::Error(TEXT("Parameter 'row' must be a number"), FMonolithJsonUtils::ErrInvalidParams); UGS->SetRow(static_cast<int32>(UGSRow)); PropsSet++; }
        double UGSCol = 0; if (Params->HasField(TEXT("column"))) { if (!Params->TryGetNumberField(TEXT("column"), UGSCol)) return FMonolithActionResult::Error(TEXT("Parameter 'column' must be a number"), FMonolithJsonUtils::ErrInvalidParams); UGS->SetColumn(static_cast<int32>(UGSCol)); PropsSet++; }
        if (!HAlign.IsEmpty()) { UGS->SetHorizontalAlignment(ParseHAlign(HAlign)); PropsSet++; }
        if (!VAlign.IsEmpty()) { UGS->SetVerticalAlignment(ParseVAlign(VAlign)); PropsSet++; }
    }
    else if (UGridSlot* GS = Cast<UGridSlot>(Slot))
    {
        double GSRow = 0; if (Params->HasField(TEXT("row")))         { if (!Params->TryGetNumberField(TEXT("row"), GSRow)) return FMonolithActionResult::Error(TEXT("Parameter 'row' must be a number"), FMonolithJsonUtils::ErrInvalidParams); GS->SetRow(static_cast<int32>(GSRow)); PropsSet++; }
        double GSCol = 0; if (Params->HasField(TEXT("column")))      { if (!Params->TryGetNumberField(TEXT("column"), GSCol)) return FMonolithActionResult::Error(TEXT("Parameter 'column' must be a number"), FMonolithJsonUtils::ErrInvalidParams); GS->SetColumn(static_cast<int32>(GSCol)); PropsSet++; }
        double GSRowSpan = 0; if (Params->HasField(TEXT("row_span")))    { if (!Params->TryGetNumberField(TEXT("row_span"), GSRowSpan)) return FMonolithActionResult::Error(TEXT("Parameter 'row_span' must be a number"), FMonolithJsonUtils::ErrInvalidParams); GS->SetRowSpan(static_cast<int32>(GSRowSpan)); PropsSet++; }
        double GSColSpan = 0; if (Params->HasField(TEXT("column_span"))) { if (!Params->TryGetNumberField(TEXT("column_span"), GSColSpan)) return FMonolithActionResult::Error(TEXT("Parameter 'column_span' must be a number"), FMonolithJsonUtils::ErrInvalidParams); GS->SetColumnSpan(static_cast<int32>(GSColSpan)); PropsSet++; }
        if (!HAlign.IsEmpty()) { GS->SetHorizontalAlignment(ParseHAlign(HAlign)); PropsSet++; }
        if (!VAlign.IsEmpty()) { GS->SetVerticalAlignment(ParseVAlign(VAlign)); PropsSet++; }

        const TSharedPtr<FJsonObject>* GridPadObj = nullptr;
        if (Params->TryGetObjectField(TEXT("padding"), GridPadObj))
        {
            double Left = 0.0, Top = 0.0, Right = 0.0, Bottom = 0.0;
            if ((*GridPadObj)->HasField(TEXT("left")) && !(*GridPadObj)->TryGetNumberField(TEXT("left"), Left)) return FMonolithActionResult::Error(TEXT("Padding 'left' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
            if ((*GridPadObj)->HasField(TEXT("top")) && !(*GridPadObj)->TryGetNumberField(TEXT("top"), Top)) return FMonolithActionResult::Error(TEXT("Padding 'top' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
            if ((*GridPadObj)->HasField(TEXT("right")) && !(*GridPadObj)->TryGetNumberField(TEXT("right"), Right)) return FMonolithActionResult::Error(TEXT("Padding 'right' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
            if ((*GridPadObj)->HasField(TEXT("bottom")) && !(*GridPadObj)->TryGetNumberField(TEXT("bottom"), Bottom)) return FMonolithActionResult::Error(TEXT("Padding 'bottom' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
            GS->SetPadding(FMargin(Left, Top, Right, Bottom));
            PropsSet++;
        }
    }

    if (PropsSet == 0)
    {
        // Phase K — surface the legal property keys in valid_options so the
        // LLM doesn't have to retry guess-by-guess.
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("MissingInput"),
            TEXT("/"),
            TEXT("No slot properties were set. Provide at least one slot property parameter."),
            TEXT("Pass one or more of the listed parameter keys with the appropriate JSON shape."),
            { TEXT("anchors"), TEXT("offsets"), TEXT("position"), TEXT("size"), TEXT("alignment"),
              TEXT("z_order"), TEXT("auto_size"), TEXT("h_align"), TEXT("v_align"), TEXT("size_rule"),
              TEXT("fill_weight"), TEXT("padding"),
              TEXT("row"), TEXT("column"), TEXT("row_span"), TEXT("column_span") }));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("widget"), WidgetName);
    Result->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());
    Result->SetNumberField(TEXT("properties_set"), PropsSet);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    if (const UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
    {
        const FSlateChildSize BoxSize = VS->GetSize();
        Result->SetStringField(TEXT("size_rule"), BoxSize.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Automatic"));
        Result->SetNumberField(TEXT("fill_weight"), BoxSize.Value);
    }
    else if (const UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
    {
        const FSlateChildSize BoxSize = HS->GetSize();
        Result->SetStringField(TEXT("size_rule"), BoxSize.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Automatic"));
        Result->SetNumberField(TEXT("fill_weight"), BoxSize.Value);
    }
    return FMonolithActionResult::Success(Result);
}

// --- set_anchor_preset ---
FMonolithActionResult FMonolithUISlotActions::HandleSetAnchorPreset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FMonolithActionResult ParamError;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)) return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError)) return ParamError;
    FString Preset;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("preset"), Preset, ParamError)) return ParamError;

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        // Phase K
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Lookup"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' not found in WBP '%s'."), *WidgetName, *AssetPath),
            TEXT("Call ui::get_widget_tree to enumerate live widget names."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Widget->Slot);
    if (!CS)
    {
        // Phase K — slot-class mismatch is a structural issue. valid_options is
        // empty; the suggested_fix names the only path forward (re-parent).
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("SlotClass"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' is not in a CanvasPanel — anchor presets only apply to CanvasPanel slots."), *WidgetName),
            TEXT("Re-parent the widget into a CanvasPanel, or use ui::set_slot_property with the appropriate slot-class fields (h_align/v_align/padding for box slots)."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    // Phase K — preset name validation. The pre-K code silently fell through
    // to FAnchors(0,0,0,0) on a miss, which left the LLM thinking the call
    // succeeded. Now: explicit FUISpecError with the full 16-entry valid_options
    // list so the LLM can self-correct on the next attempt.
    const TArray<FString>& KnownPresets = MonolithUIInternal::GetAnchorPresetNames();
    if (!KnownPresets.Contains(Preset))
    {
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Enum"),
            TEXT("/preset"),
            FString::Printf(TEXT("Unknown anchor preset '%s'."), *Preset),
            TEXT("Pick one of the listed preset tokens — names are lowercase with underscore separators."),
            KnownPresets);
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    FAnchors Anchors = MonolithUIInternal::GetAnchorPreset(Preset);
    CS->SetAnchors(Anchors);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("widget"), WidgetName);
    Result->SetStringField(TEXT("preset"), Preset);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    return FMonolithActionResult::Success(Result);
}

// --- move_widget ---
FMonolithActionResult FMonolithUISlotActions::HandleMoveWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FMonolithActionResult ParamError;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)) return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError)) return ParamError;
    FString NewParentName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("new_parent_name"), NewParentName, ParamError)) return ParamError;

    FString ExpectedParentName;
    Params->TryGetStringField(TEXT("expected_parent_name"), ExpectedParentName);

    const bool bHasSiblingIndex = Params->HasField(TEXT("sibling_index"));
    int32 RequestedSiblingIndex = INDEX_NONE;
    if (bHasSiblingIndex)
    {
        double RawSiblingIndex = 0.0;
        if (!Params->TryGetNumberField(TEXT("sibling_index"), RawSiblingIndex)
            || !FMath::IsFinite(RawSiblingIndex)
            || RawSiblingIndex < 0.0
            || RawSiblingIndex > static_cast<double>(MAX_int32)
            || RawSiblingIndex != FMath::FloorToDouble(RawSiblingIndex))
        {
            FUISpecError E = MonolithUIInternal::MakeSpecError(
                TEXT("Range"),
                TEXT("/sibling_index"),
                TEXT("sibling_index must be a non-negative integer."),
                TEXT("Pass the zero-based final child index in the target panel."));
            E.WidgetId = FName(*WidgetName);
            return MonolithUIInternal::MakeErrorFromSpecError(E);
        }
        RequestedSiblingIndex = static_cast<int32>(RawSiblingIndex);
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Lookup"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' not found in WBP '%s'."), *WidgetName, *AssetPath),
            TEXT("Call ui::get_widget_tree to enumerate live widget names before moving."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    UWidget* NewParentWidget = WBP->WidgetTree->FindWidget(FName(*NewParentName));
    UPanelWidget* NewParent = Cast<UPanelWidget>(NewParentWidget);
    if (!NewParent)
    {
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Slot"),
            TEXT("/new_parent_name"),
            FString::Printf(TEXT("New parent '%s' was not found or is not a UPanelWidget."), *NewParentName),
            TEXT("Choose a parent panel from ui::get_widget_tree, or create a compatible panel first."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    int32 OldIndex = -1;
    UPanelWidget* OldParent = UWidgetTree::FindWidgetParent(Widget, OldIndex);
    if (!OldParent || !Widget->Slot)
    {
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Slot"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' has no parent slot and cannot be moved with ui::move_widget."), *WidgetName),
            TEXT("Use ui::reparent_widget_root for root-widget replacement, or move a non-root child widget."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    if (!ExpectedParentName.IsEmpty() && OldParent->GetFName() != FName(*ExpectedParentName))
    {
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Conflict"),
            TEXT("/expected_parent_name"),
            FString::Printf(
                TEXT("Widget '%s' is a direct child of '%s', not the asserted parent '%s'."),
                *WidgetName,
                *OldParent->GetName(),
                *ExpectedParentName),
            TEXT("Refresh ui::get_widget_tree or ui::diff_ui_spec and retry with the current direct parent."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    for (UWidget* Ancestor = NewParent; Ancestor; )
    {
        if (Ancestor == Widget)
        {
            FUISpecError E = MonolithUIInternal::MakeSpecError(
                TEXT("Hierarchy"),
                TEXT("/new_parent_name"),
                FString::Printf(
                    TEXT("Widget '%s' cannot be moved under itself or its descendant '%s'."),
                    *WidgetName,
                    *NewParentName),
                TEXT("Choose a target panel outside the widget's own subtree."));
            E.WidgetId = FName(*WidgetName);
            return MonolithUIInternal::MakeErrorFromSpecError(E);
        }

        int32 AncestorIndex = INDEX_NONE;
        Ancestor = UWidgetTree::FindWidgetParent(Ancestor, AncestorIndex);
    }

    if (OldParent != NewParent && !NewParent->CanAddMoreChildren())
    {
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Slot"),
            TEXT("/new_parent_name"),
            FString::Printf(
                TEXT("New parent '%s' cannot accept another child; it currently has %d child widget(s)."),
                *NewParentName,
                NewParent->GetChildrenCount()),
            TEXT("Pick an empty single-child panel, remove its existing child first, or move the widget into a multi-child panel."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    if (bHasSiblingIndex)
    {
        // A same-parent reorder addresses an existing final slot [0, N-1]. A
        // cross-parent move inserts into the target's final child list [0, N].
        const int32 MaximumSiblingIndex = OldParent == NewParent
            ? OldParent->GetChildrenCount() - 1
            : NewParent->GetChildrenCount();
        if (RequestedSiblingIndex > MaximumSiblingIndex)
        {
            FUISpecError E = MonolithUIInternal::MakeSpecError(
                TEXT("Range"),
                TEXT("/sibling_index"),
                FString::Printf(
                    TEXT("sibling_index %d is outside the valid range [0, %d] for target panel '%s'."),
                    RequestedSiblingIndex,
                    MaximumSiblingIndex,
                    *NewParentName),
                TEXT("Use the desired zero-based final child index from ui::diff_ui_spec."));
            E.WidgetId = FName(*WidgetName);
            return MonolithUIInternal::MakeErrorFromSpecError(E);
        }
    }

    const int32 TargetSiblingIndex = bHasSiblingIndex
        ? RequestedSiblingIndex
        : NewParent->GetChildrenCount() - (OldParent == NewParent ? 1 : 0);

    if (OldParent == NewParent)
    {
        UPanelSlot* const OriginalSlot = Widget->Slot;
        NewParent->ShiftChild(TargetSiblingIndex, Widget);
        const int32 NewIndex = NewParent->GetChildIndex(Widget);
        const bool bChanged = OldIndex != NewIndex;

        if (bChanged)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        }

        bool bCompile = true;
        Params->TryGetBoolField(TEXT("compile"), bCompile);
        if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

        TSharedPtr<FJsonObject> SlotPreservation = MakeShared<FJsonObject>();
        SlotPreservation->SetBoolField(TEXT("attempted"), true);
        SlotPreservation->SetStringField(TEXT("old_slot_type"), OriginalSlot ? OriginalSlot->GetClass()->GetName() : TEXT("none"));
        SlotPreservation->SetStringField(TEXT("new_slot_type"), Widget->Slot ? Widget->Slot->GetClass()->GetName() : TEXT("none"));
        SlotPreservation->SetNumberField(TEXT("old_parent_index"), OldIndex);
        SlotPreservation->SetNumberField(TEXT("new_parent_index"), NewIndex);
        SlotPreservation->SetStringField(TEXT("status"), TEXT("preserved_same_slot_instance"));
        SlotPreservation->SetBoolField(TEXT("same_slot_instance"), Widget->Slot == OriginalSlot);
        SlotPreservation->SetArrayField(TEXT("copied_fields"), TArray<TSharedPtr<FJsonValue>>());

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget"), WidgetName);
        Result->SetStringField(TEXT("operation_source"), TEXT("monolith_equivalent"));
        Result->SetStringField(TEXT("old_parent"), OldParent->GetName());
        Result->SetStringField(TEXT("new_parent"), NewParentName);
        Result->SetNumberField(TEXT("old_sibling_index"), OldIndex);
        Result->SetNumberField(TEXT("new_sibling_index"), NewIndex);
        Result->SetStringField(TEXT("new_slot_type"), Widget->Slot ? Widget->Slot->GetClass()->GetName() : TEXT("none"));
        Result->SetObjectField(TEXT("slot_preservation"), SlotPreservation);
        Result->SetBoolField(TEXT("changed"), bChanged);
        Result->SetBoolField(TEXT("compiled"), bCompile);
        return FMonolithActionResult::Success(Result);
    }

    const FMonolithUISlotSnapshot SlotSnapshot = CaptureCompatibleSlotProperties(Widget->Slot);
    TSharedPtr<FJsonObject> RollbackSlotPreservation;
    const MonolithUI::MoveWidgetTransaction::FResult MoveResult =
        MonolithUI::MoveWidgetTransaction::MoveCrossParent(
            *OldParent,
            *NewParent,
            *Widget,
            OldIndex,
            bHasSiblingIndex,
            RequestedSiblingIndex,
            [](UPanelWidget& TargetParent, UWidget& Child) -> UPanelSlot*
            {
                return TargetParent.AddChild(&Child);
            },
            [&SlotSnapshot, OldIndex, &RollbackSlotPreservation](UPanelSlot& RestoredSlot, int32 RestoredIndex)
            {
                RollbackSlotPreservation = RestoreCompatibleSlotProperties(
                    SlotSnapshot,
                    &RestoredSlot,
                    OldIndex,
                    RestoredIndex);
                FString PreservationStatus;
                return !SlotSnapshot.IsValid()
                    || (RollbackSlotPreservation.IsValid()
                        && RollbackSlotPreservation->TryGetStringField(TEXT("status"), PreservationStatus)
                        && PreservationStatus == TEXT("preserved"));
            });

    if (!MoveResult.bSucceeded)
    {
        const FString RecoveryMessage = MoveResult.bRollbackSucceeded
            ? TEXT("The original parent, sibling index, and compatible slot properties remain intact.")
            : TEXT("Rollback did not fully restore the original hierarchy and slot state; reload the Widget Blueprint before retrying.");
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Mutation"),
            TEXT("/new_parent_name"),
            FString::Printf(
                TEXT("Moving widget '%s' to '%s' failed: %s. %s"),
                *WidgetName,
                *NewParentName,
                *MoveResult.FailureReason,
                *RecoveryMessage),
            MoveResult.bRollbackSucceeded
                ? TEXT("Refresh ui::get_widget_tree, verify the target panel still accepts the widget, and retry.")
                : TEXT("Reload the Widget Blueprint to recover a canonical tree before issuing another mutation."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    UPanelSlot* const NewSlot = MoveResult.NewSlot;
    const int32 NewIndex = MoveResult.NewIndex;
    TSharedPtr<FJsonObject> SlotPreservation = RestoreCompatibleSlotProperties(SlotSnapshot, NewSlot, OldIndex, NewIndex);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("widget"), WidgetName);
    Result->SetStringField(TEXT("operation_source"), TEXT("monolith_equivalent"));
    Result->SetStringField(TEXT("old_parent"), OldParent ? OldParent->GetName() : TEXT("none"));
    Result->SetStringField(TEXT("new_parent"), NewParentName);
    Result->SetNumberField(TEXT("old_sibling_index"), OldIndex);
    Result->SetNumberField(TEXT("new_sibling_index"), NewIndex);
    Result->SetStringField(TEXT("new_slot_type"), NewSlot ? NewSlot->GetClass()->GetName() : TEXT("none"));
    Result->SetObjectField(TEXT("slot_preservation"), SlotPreservation);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    return FMonolithActionResult::Success(Result);
}
