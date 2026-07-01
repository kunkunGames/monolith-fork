// MonolithUIStylingActions.cpp
#include "MonolithUIStylingActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithUIInternal.h"
#include "MonolithParamSchema.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"
#include "Components/RetainerBox.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"


namespace
{
    TSharedPtr<FJsonObject> MakeStylingResponse(const FString& WidgetName, int32 PropsSet, bool bCompile, const FString& PropertyName = TEXT(""))
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget"), WidgetName);
        if (!PropertyName.IsEmpty())
        {
            Result->SetStringField(TEXT("property"), PropertyName);
        }
        Result->SetNumberField(TEXT("properties_set"), PropsSet);
        Result->SetBoolField(TEXT("compiled"), bCompile);
        return Result;
    }

    TArray<TSharedPtr<FJsonValue>> CollectTextureParameterNames(UMaterialInterface* Material)
    {
        TArray<TSharedPtr<FJsonValue>> TextureParameters;
        if (!Material)
        {
            return TextureParameters;
        }

        TArray<FMaterialParameterInfo> TextureInfos;
        TArray<FGuid> TextureGuids;
        Material->GetAllTextureParameterInfo(TextureInfos, TextureGuids);
        TextureParameters.Reserve(TextureInfos.Num());
        for (const FMaterialParameterInfo& Info : TextureInfos)
        {
            TextureParameters.Add(MakeShared<FJsonValueString>(Info.Name.ToString()));
        }
        return TextureParameters;
    }

    bool HasExactTextureParameter(UMaterialInterface* Material, const FName& TextureParameter)
    {
        if (!Material || TextureParameter.IsNone())
        {
            return false;
        }

        TArray<FMaterialParameterInfo> TextureInfos;
        TArray<FGuid> TextureGuids;
        Material->GetAllTextureParameterInfo(TextureInfos, TextureGuids);
        for (const FMaterialParameterInfo& Info : TextureInfos)
        {
            if (Info.Name == TextureParameter)
            {
                return true;
            }
        }
        return false;
    }

    FString GetMaterialDomainString(UMaterialInterface* Material)
    {
        const UMaterial* BaseMaterial = Material ? Material->GetMaterial() : nullptr;
        if (!BaseMaterial)
        {
            return TEXT("unknown");
        }

        const UEnum* DomainEnum = StaticEnum<EMaterialDomain>();
        return DomainEnum ? DomainEnum->GetNameStringByValue(static_cast<int64>(BaseMaterial->MaterialDomain)) : TEXT("unknown");
    }

    bool IsUiMaterial(UMaterialInterface* Material)
    {
        const UMaterial* BaseMaterial = Material ? Material->GetMaterial() : nullptr;
        return BaseMaterial && BaseMaterial->MaterialDomain == MD_UI;
    }
}

void FMonolithUIStylingActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_brush"),
        TEXT("Configure a slate brush on any widget property (background, fill image, etc.)"),
        FMonolithActionHandler::CreateStatic(&HandleSetBrush),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget name"))
            .Required(TEXT("property_name"), TEXT("string"), TEXT("Brush property: Background, BarFillStyle.FillImage, etc."))
            .Optional(TEXT("draw_type"), TEXT("string"), TEXT("Draw type: Image, Box, Border, RoundedBox, NoDrawType"), TEXT("Image"))
            .Optional(TEXT("tint_color"), TEXT("string"), TEXT("Tint color as hex (#RRGGBB) or r,g,b,a floats"))
            .Optional(TEXT("image_size"), TEXT("object"), TEXT("Image size: {\"x\": 64, \"y\": 64}"))
            .Optional(TEXT("margin"), TEXT("object"), TEXT("9-slice margin: {\"left\":0, \"top\":0, \"right\":0, \"bottom\":0}"))
            .Optional(TEXT("corner_radius"), TEXT("object"), TEXT("Corner radius: {\"top_left\":0, \"top_right\":0, \"bottom_right\":0, \"bottom_left\":0}"))
            .Optional(TEXT("outline_color"), TEXT("string"), TEXT("Outline color as hex or r,g,b,a"))
            .Optional(TEXT("outline_width"), TEXT("number"), TEXT("Outline width in pixels"))
            .OptionalAssetPath(TEXT("texture_path"), TEXT("Texture asset path to set on the brush"))
            .OptionalAssetPath(TEXT("material_path"), TEXT("Material asset path to set on the brush"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_font"),
        TEXT("Set font properties on a text widget (TextBlock, RichTextBlock, EditableText, etc.)"),
        FMonolithActionHandler::CreateStatic(&HandleSetFont),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget name (must be a text widget)"))
            .Optional(TEXT("font_size"), TEXT("integer"), TEXT("Font size in points"))
            .Optional(TEXT("font_family"), TEXT("string"), TEXT("Font asset path"))
            .Optional(TEXT("typeface"), TEXT("string"), TEXT("Typeface: Regular, Bold, Italic, Light"), TEXT("Regular"))
            .Optional(TEXT("letter_spacing"), TEXT("integer"), TEXT("Letter spacing in design units"))
            .Optional(TEXT("outline_size"), TEXT("integer"), TEXT("Font outline size"))
            .Optional(TEXT("outline_color"), TEXT("string"), TEXT("Font outline color as hex or r,g,b,a"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_color_scheme"),
        TEXT("Set EStyleColor User1-16 slots for widget theming"),
        FMonolithActionHandler::CreateStatic(&HandleSetColorScheme),
        FParamSchemaBuilder()
            .Required(TEXT("colors"), TEXT("object"), TEXT("Color slot map: {\"User1\": \"#0A0A14\", \"User2\": \"#E6B822\", ...}"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("batch_style"),
        TEXT("Apply a property value to all widgets of a given class in a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleBatchStyle),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_class"), TEXT("string"), TEXT("Widget class to target: TextBlock, Button, Image, etc."))
            .Required(TEXT("property_name"), TEXT("string"), TEXT("Property name to set"))
            .Required(TEXT("value"), TEXT("string"), TEXT("Property value as string"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_text"),
        TEXT("Convenience: set text, color, size, justification on a TextBlock or RichTextBlock"),
        FMonolithActionHandler::CreateStatic(&HandleSetText),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target TextBlock or RichTextBlock name"))
            .Optional(TEXT("text"), TEXT("string"), TEXT("Text content to set"))
            .Optional(TEXT("text_color"), TEXT("string"), TEXT("Text color as hex (#RRGGBB) or r,g,b,a"))
            .Optional(TEXT("font_size"), TEXT("integer"), TEXT("Font size in points"))
            .Optional(TEXT("justification"), TEXT("string"), TEXT("Text justification: Left, Center, Right"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_image"),
        TEXT("Convenience: set texture or material on an Image widget"),
        FMonolithActionHandler::CreateStatic(&HandleSetImage),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target Image widget name"))
            .OptionalAssetPath(TEXT("texture_path"), TEXT("Texture asset path"))
            .OptionalAssetPath(TEXT("material_path"), TEXT("Material asset path"))
            .Optional(TEXT("tint_color"), TEXT("string"), TEXT("Tint color as hex or r,g,b,a"))
            .Optional(TEXT("size"), TEXT("object"), TEXT("Desired size: {\"x\": 64, \"y\": 64}"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_retainer_effect_material"),
        TEXT("Set a RetainerBox effect material and exact render-target texture parameter using the RetainerBox owner API"),
        FMonolithActionHandler::CreateStatic(&HandleSetRetainerEffectMaterial),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target RetainerBox widget name"))
            .RequiredAssetPath(TEXT("material_path"), TEXT("UI-domain effect material asset path"))
            .Optional(TEXT("texture_parameter"), TEXT("string"), TEXT("Texture parameter receiving the Retainer render target. Defaults to Texture."), TEXT("Texture"))
            .Optional(TEXT("require_ui_material"), TEXT("boolean"), TEXT("Fail when the material domain is not UI. Default true."), TEXT("true"))
            .Optional(TEXT("request_render"), TEXT("boolean"), TEXT("Call RequestRender after applying the effect material."), TEXT("false"))
            .Optional(TEXT("retain_rendering"), TEXT("boolean"), TEXT("Optional SetRetainRendering value. Omit to preserve existing value."))
            .Optional(TEXT("render_phase"), TEXT("integer"), TEXT("Optional render phase. Requires total_render_phases."))
            .Optional(TEXT("total_render_phases"), TEXT("integer"), TEXT("Optional total rendering phases. Requires render_phase."))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Build()
    );
}

// --- set_brush ---
FMonolithActionResult FMonolithUIStylingActions::HandleSetBrush(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FMonolithActionResult ParamError;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)) return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError)) return ParamError;
    FString PropertyName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("property_name"), PropertyName, ParamError)) return ParamError;

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

    // Find the FSlateBrush property by name (supports nested like "BarFillStyle.FillImage")
    // Split on '.' for nested access
    void* ContainerPtr = Widget;
    UStruct* ContainerStruct = Widget->GetClass();
    FProperty* BrushProp = nullptr;

    TArray<FString> PathParts;
    PropertyName.ParseIntoArray(PathParts, TEXT("."));
    for (int32 i = 0; i < PathParts.Num(); ++i)
    {
        FProperty* Prop = ContainerStruct->FindPropertyByName(FName(*PathParts[i]));
        if (!Prop)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Property '%s' not found on %s"), *PathParts[i], *ContainerStruct->GetName()));
        }

        if (i < PathParts.Num() - 1)
        {
            // Navigate into struct
            FStructProperty* StructProp = CastField<FStructProperty>(Prop);
            if (!StructProp)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("'%s' is not a struct property — cannot navigate deeper"), *PathParts[i]));
            }
            ContainerPtr = Prop->ContainerPtrToValuePtr<void>(ContainerPtr);
            ContainerStruct = StructProp->Struct;
        }
        else
        {
            BrushProp = Prop;
        }
    }

    // Verify it's an FSlateBrush
    FStructProperty* BrushStructProp = CastField<FStructProperty>(BrushProp);
    if (!BrushStructProp || BrushStructProp->Struct->GetFName() != TEXT("SlateBrush"))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Property '%s' is not an FSlateBrush"), *PropertyName));
    }

    FSlateBrush* Brush = BrushProp->ContainerPtrToValuePtr<FSlateBrush>(ContainerPtr);
    if (!Brush)
    {
        return FMonolithActionResult::Error(TEXT("Failed to get brush pointer"));
    }

    int32 PropsSet = 0;

    // Draw type
    FString DrawType;
    Params->TryGetStringField(TEXT("draw_type"), DrawType);
    if (!DrawType.IsEmpty())
    {
        // Phase K — pre-K behaviour silently ignored unknown draw_type tokens
        // (no PropsSet++ + no error). Now: explicit FUISpecError with the full
        // 5-token list so the LLM can self-correct.
        bool bMatched = true;
        if (DrawType == TEXT("Image"))           Brush->DrawAs = ESlateBrushDrawType::Image;
        else if (DrawType == TEXT("Box"))        Brush->DrawAs = ESlateBrushDrawType::Box;
        else if (DrawType == TEXT("Border"))     Brush->DrawAs = ESlateBrushDrawType::Border;
        else if (DrawType == TEXT("RoundedBox")) Brush->DrawAs = ESlateBrushDrawType::RoundedBox;
        else if (DrawType == TEXT("NoDrawType")) Brush->DrawAs = ESlateBrushDrawType::NoDrawType;
        else                                     bMatched = false;

        if (!bMatched)
        {
            FUISpecError E = MonolithUIInternal::MakeSpecError(
                TEXT("Enum"),
                TEXT("/draw_type"),
                FString::Printf(TEXT("Unknown draw_type token '%s'."), *DrawType),
                TEXT("Pick one of the ESlateBrushDrawType tokens. RoundedBox requires a corner_radius alongside."),
                { TEXT("Image"), TEXT("Box"), TEXT("Border"), TEXT("RoundedBox"), TEXT("NoDrawType") });
            E.WidgetId = FName(*WidgetName);
            return MonolithUIInternal::MakeErrorFromSpecError(E);
        }
        PropsSet++;
    }

    // Tint color
    FString TintColor;
    Params->TryGetStringField(TEXT("tint_color"), TintColor);
    if (!TintColor.IsEmpty())
    {
        Brush->TintColor = FSlateColor(MonolithUIInternal::ParseColor(TintColor));
        PropsSet++;
    }

    // Image size
    const TSharedPtr<FJsonObject>* ImageSizeObj = nullptr;
    if (Params->TryGetObjectField(TEXT("image_size"), ImageSizeObj))
    {
        double X = 0.0, Y = 0.0;
        (*ImageSizeObj)->TryGetNumberField(TEXT("x"), X);
        (*ImageSizeObj)->TryGetNumberField(TEXT("y"), Y);
        Brush->ImageSize = FVector2D(X, Y);
        PropsSet++;
    }

    // 9-slice margin
    const TSharedPtr<FJsonObject>* MarginObj = nullptr;
    if (Params->TryGetObjectField(TEXT("margin"), MarginObj))
    {
        FMargin Pad;
        if (MonolithUIInternal::TryParseMargin(MarginObj, Pad))
        {
            Brush->Margin = Pad;
            PropsSet++;
        }
    }

    // Corner radius (RoundedBox)
    const TSharedPtr<FJsonObject>* CornerObj = nullptr;
    if (Params->TryGetObjectField(TEXT("corner_radius"), CornerObj))
    {
        double TL = 0.0, TR = 0.0, BR = 0.0, BL = 0.0;
        (*CornerObj)->TryGetNumberField(TEXT("top_left"), TL);
        (*CornerObj)->TryGetNumberField(TEXT("top_right"), TR);
        (*CornerObj)->TryGetNumberField(TEXT("bottom_right"), BR);
        (*CornerObj)->TryGetNumberField(TEXT("bottom_left"), BL);
        Brush->OutlineSettings.CornerRadii = FVector4(TL, TR, BR, BL);
        Brush->OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
        PropsSet++;
    }

    // Outline
    FString OutlineColor;
    Params->TryGetStringField(TEXT("outline_color"), OutlineColor);
    if (!OutlineColor.IsEmpty())
    {
        Brush->OutlineSettings.Color = FSlateColor(MonolithUIInternal::ParseColor(OutlineColor));
        PropsSet++;
    }
    double OutlineWidthVal;
    if (Params->TryGetNumberField(TEXT("outline_width"), OutlineWidthVal))
    {
        Brush->OutlineSettings.Width = static_cast<float>(OutlineWidthVal);
        PropsSet++;
    }

    // Texture
    FString TexturePath;
    Params->TryGetStringField(TEXT("texture_path"), TexturePath);
    if (!TexturePath.IsEmpty())
    {
        UTexture2D* Tex = FMonolithAssetUtils::LoadAssetByPath<UTexture2D>(TexturePath);
        if (Tex)
        {
            Brush->SetResourceObject(Tex);
            PropsSet++;
        }
        else
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
        }
    }

    // Material
    FString MaterialPath;
    Params->TryGetStringField(TEXT("material_path"), MaterialPath);
    if (!MaterialPath.IsEmpty())
    {
        UMaterialInterface* Mat = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(MaterialPath);
        if (Mat)
        {
            Brush->SetResourceObject(Mat);
            PropsSet++;
        }
        else
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
        }
    }

    if (PropsSet == 0)
    {
        // Phase K — list the legal property keys in valid_options.
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("MissingInput"),
            TEXT("/"),
            TEXT("No brush properties specified."),
            TEXT("Pass one or more of the listed parameter keys to mutate the SlateBrush."),
            { TEXT("draw_type"), TEXT("tint_color"), TEXT("image_size"), TEXT("margin"),
              TEXT("corner_radius"), TEXT("outline_color"), TEXT("outline_width"),
              TEXT("texture_path"), TEXT("material_path") }));
    }

    Widget->SynchronizeProperties();
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);
    return FMonolithActionResult::Success(MakeStylingResponse(WidgetName, PropsSet, bCompile, PropertyName));
}

// --- set_font ---
FMonolithActionResult FMonolithUIStylingActions::HandleSetFont(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FMonolithActionResult ParamError;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)) return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError)) return ParamError;

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

    // Get font via getter/setter pattern (safer than raw reflection — respects property accessors)
    UTextBlock* TextBlock = Cast<UTextBlock>(Widget);
    URichTextBlock* RichText = Cast<URichTextBlock>(Widget);
    if (!TextBlock && !RichText)
    {
        // Phase K — class-mismatch with valid_options enumerating the
        // accepted text-widget tokens.
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("WidgetClass"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' (class %s) is not a text widget."),
                *WidgetName, *Widget->GetClass()->GetName()),
            TEXT("set_font targets text widgets only. For non-text widgets, set the relevant FSlateFontInfo struct field via set_widget_property."),
            { TEXT("TextBlock"), TEXT("RichTextBlock") });
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    FSlateFontInfo FontInfoCopy = TextBlock ? TextBlock->GetFont() :
                                  FSlateFontInfo(); // RichTextBlock uses style sets, apply via reflection fallback
    FSlateFontInfo* FontInfo = &FontInfoCopy;

    int32 PropsSet = 0;

    // Font size
    double FontSizeVal;
    if (Params->TryGetNumberField(TEXT("font_size"), FontSizeVal))
    {
        FontInfo->Size = static_cast<float>(FontSizeVal);
        PropsSet++;
    }

    // Font family (asset path)
    FString FontFamily;
    Params->TryGetStringField(TEXT("font_family"), FontFamily);
    if (!FontFamily.IsEmpty())
    {
        UFont* FontObj = FMonolithAssetUtils::LoadAssetByPath<UFont>(FontFamily);
        if (FontObj)
        {
            FontInfo->FontObject = FontObj;
            PropsSet++;
        }
        else
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("Font asset not found: %s"), *FontFamily));
        }
    }

    // Typeface
    FString Typeface;
    Params->TryGetStringField(TEXT("typeface"), Typeface);
    if (!Typeface.IsEmpty())
    {
        FontInfo->TypefaceFontName = FName(*Typeface);
        PropsSet++;
    }

    // Letter spacing
    double LetterSpacingVal;
    if (Params->TryGetNumberField(TEXT("letter_spacing"), LetterSpacingVal))
    {
        FontInfo->LetterSpacing = static_cast<int32>(LetterSpacingVal);
        PropsSet++;
    }

    // Outline size
    double OutlineSizeVal;
    if (Params->TryGetNumberField(TEXT("outline_size"), OutlineSizeVal))
    {
        FontInfo->OutlineSettings.OutlineSize = static_cast<int32>(OutlineSizeVal);
        PropsSet++;
    }

    // Outline color
    FString OutlineColor;
    Params->TryGetStringField(TEXT("outline_color"), OutlineColor);
    if (!OutlineColor.IsEmpty())
    {
        FontInfo->OutlineSettings.OutlineColor = MonolithUIInternal::ParseColor(OutlineColor);
        PropsSet++;
    }

    if (PropsSet == 0)
    {
        // Phase K
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("MissingInput"),
            TEXT("/"),
            TEXT("No font properties specified."),
            TEXT("Pass one or more of the listed parameter keys to mutate FSlateFontInfo."),
            { TEXT("font_size"), TEXT("font_family"), TEXT("typeface"),
              TEXT("letter_spacing"), TEXT("outline_size"), TEXT("outline_color") }));
    }

    // Write back via setter (not raw reflection) to ensure Slate update
    if (TextBlock) TextBlock->SetFont(FontInfoCopy);
    Widget->SynchronizeProperties();
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);
    return FMonolithActionResult::Success(MakeStylingResponse(WidgetName, PropsSet, bCompile));
}

// --- set_color_scheme ---
FMonolithActionResult FMonolithUIStylingActions::HandleSetColorScheme(const TSharedPtr<FJsonObject>& Params)
{
    const TSharedPtr<FJsonObject>* ColorsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("colors"), ColorsObj))
    {
        return FMonolithActionResult::Error(TEXT("Missing required param: colors (object with slot names as keys)"));
    }

    // Map slot names to EStyleColor values
    static TMap<FString, EStyleColor> SlotMap;
    if (SlotMap.Num() == 0)
    {
        SlotMap.Add(TEXT("User1"),  EStyleColor::User1);
        SlotMap.Add(TEXT("User2"),  EStyleColor::User2);
        SlotMap.Add(TEXT("User3"),  EStyleColor::User3);
        SlotMap.Add(TEXT("User4"),  EStyleColor::User4);
        SlotMap.Add(TEXT("User5"),  EStyleColor::User5);
        SlotMap.Add(TEXT("User6"),  EStyleColor::User6);
        SlotMap.Add(TEXT("User7"),  EStyleColor::User7);
        SlotMap.Add(TEXT("User8"),  EStyleColor::User8);
        SlotMap.Add(TEXT("User9"),  EStyleColor::User9);
        SlotMap.Add(TEXT("User10"), EStyleColor::User10);
        SlotMap.Add(TEXT("User11"), EStyleColor::User11);
        SlotMap.Add(TEXT("User12"), EStyleColor::User12);
        SlotMap.Add(TEXT("User13"), EStyleColor::User13);
        SlotMap.Add(TEXT("User14"), EStyleColor::User14);
        SlotMap.Add(TEXT("User15"), EStyleColor::User15);
        SlotMap.Add(TEXT("User16"), EStyleColor::User16);
    }

    int32 SlotsSet = 0;
    TArray<FString> SetNames;
    SetNames.Reserve((*ColorsObj)->Values.Num());

    for (const auto& Pair : FMonolithJsonUtils::GetFields(*ColorsObj))
    {
        const FString PairKeyStr = MonolithKeyToString(Pair.Key);
        const EStyleColor* SlotColor = SlotMap.Find(PairKeyStr);
        if (!SlotColor)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Unknown color slot: '%s'. Valid: User1-User16"), *PairKeyStr));
        }

        FString ColorStr = Pair.Value->AsString();
        FLinearColor Color = MonolithUIInternal::ParseColor(ColorStr);
        USlateThemeManager::Get().SetDefaultColor(*SlotColor, Color);
        SlotsSet++;
        SetNames.Add(PairKeyStr);
    }

    if (SlotsSet == 0)
    {
        return FMonolithActionResult::Error(TEXT("No colors specified in the colors object"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("slots_set"), SlotsSet);

    TArray<TSharedPtr<FJsonValue>> NamesArray;
    NamesArray.Reserve(SetNames.Num());
    for (const FString& N : SetNames)
    {
        NamesArray.Add(MakeShared<FJsonValueString>(N));
    }
    Result->SetArrayField(TEXT("slot_names"), NamesArray);
    return FMonolithActionResult::Success(Result);
}

// --- batch_style ---
FMonolithActionResult FMonolithUIStylingActions::HandleBatchStyle(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FMonolithActionResult ParamError;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)) return ParamError;
    FString WidgetClassName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_class"), WidgetClassName, ParamError)) return ParamError;
    FString PropertyName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("property_name"), PropertyName, ParamError)) return ParamError;
    FString Value;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("value"), Value, ParamError)) return ParamError;

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UClass* TargetClass = MonolithUIInternal::WidgetClassFromName(WidgetClassName);
    if (!TargetClass)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Unknown widget class: %s"), *WidgetClassName));
    }

    TArray<UWidget*> AllWidgets;
    WBP->WidgetTree->GetAllWidgets(AllWidgets);

    int32 Modified = 0;
    TArray<FString> ModifiedNames;

    for (UWidget* Widget : AllWidgets)
    {
        if (!Widget || !Widget->IsA(TargetClass)) continue;

        FProperty* Prop = Widget->GetClass()->FindPropertyByName(FName(*PropertyName));
        if (!Prop) continue;

        void* PropAddr = Prop->ContainerPtrToValuePtr<void>(Widget);
        if (Prop->ImportText_Direct(*Value, PropAddr, Widget, PPF_None))
        {
            Widget->SynchronizeProperties();
            Modified++;
            ModifiedNames.Add(Widget->GetName());
        }
    }

    if (Modified == 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("No %s widgets found with settable property '%s'"), *WidgetClassName, *PropertyName));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("widgets_modified"), Modified);
    Result->SetStringField(TEXT("widget_class"), WidgetClassName);
    Result->SetStringField(TEXT("property"), PropertyName);
    Result->SetStringField(TEXT("value"), Value);
    Result->SetBoolField(TEXT("compiled"), bCompile);

    TArray<TSharedPtr<FJsonValue>> NamesArray;
    NamesArray.Reserve(ModifiedNames.Num());
    for (const FString& N : ModifiedNames)
    {
        NamesArray.Add(MakeShared<FJsonValueString>(N));
    }
    Result->SetArrayField(TEXT("modified_widgets"), NamesArray);
    return FMonolithActionResult::Success(Result);
}

// --- set_text ---
FMonolithActionResult FMonolithUIStylingActions::HandleSetText(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FMonolithActionResult ParamError;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)) return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError)) return ParamError;

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

    int32 PropsSet = 0;

    // TextBlock
    if (UTextBlock* TB = Cast<UTextBlock>(Widget))
    {
        FString Text;
        if (Params->TryGetStringField(TEXT("text"), Text))
        {
            TB->SetText(FText::FromString(Text));
            PropsSet++;
        }

        FString TextColor;
        Params->TryGetStringField(TEXT("text_color"), TextColor);
        if (!TextColor.IsEmpty())
        {
            TB->SetColorAndOpacity(FSlateColor(MonolithUIInternal::ParseColor(TextColor)));
            PropsSet++;
        }

        double FontSizeVal;
        if (Params->TryGetNumberField(TEXT("font_size"), FontSizeVal))
        {
            FSlateFontInfo FontInfo = TB->GetFont();
            FontInfo.Size = static_cast<float>(FontSizeVal);
            TB->SetFont(FontInfo);
            PropsSet++;
        }

        FString Justification;
        Params->TryGetStringField(TEXT("justification"), Justification);
        if (!Justification.IsEmpty())
        {
            // Phase K — pre-K silently ignored unknown tokens. Now: explicit
            // FUISpecError with the 3-token list.
            bool bMatched = true;
            if (Justification == TEXT("Left"))        TB->SetJustification(ETextJustify::Left);
            else if (Justification == TEXT("Center")) TB->SetJustification(ETextJustify::Center);
            else if (Justification == TEXT("Right"))  TB->SetJustification(ETextJustify::Right);
            else                                      bMatched = false;

            if (!bMatched)
            {
                FUISpecError E = MonolithUIInternal::MakeSpecError(
                    TEXT("Enum"),
                    TEXT("/justification"),
                    FString::Printf(TEXT("Unknown justification token '%s'."), *Justification),
                    TEXT("Pick one of the ETextJustify tokens."),
                    { TEXT("Left"), TEXT("Center"), TEXT("Right") });
                E.WidgetId = FName(*WidgetName);
                return MonolithUIInternal::MakeErrorFromSpecError(E);
            }
            PropsSet++;
        }
    }
    // RichTextBlock
    else if (URichTextBlock* RTB = Cast<URichTextBlock>(Widget))
    {
        FString Text;
        if (Params->TryGetStringField(TEXT("text"), Text))
        {
            RTB->SetText(FText::FromString(Text));
            PropsSet++;
        }
    }
    else
    {
        // Phase K
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("WidgetClass"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' (class %s) is not a TextBlock or RichTextBlock."),
                *WidgetName, *Widget->GetClass()->GetName()),
            TEXT("set_text targets text widgets only. Use set_widget_property for direct property writes on other classes."),
            { TEXT("TextBlock"), TEXT("RichTextBlock") });
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    if (PropsSet == 0)
    {
        // Phase K
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("MissingInput"),
            TEXT("/"),
            TEXT("No text properties specified."),
            TEXT("Pass one or more of the listed parameter keys to mutate the text widget."),
            { TEXT("text"), TEXT("text_color"), TEXT("font_size"), TEXT("justification") }));
    }

    Widget->SynchronizeProperties();
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);
    return FMonolithActionResult::Success(MakeStylingResponse(WidgetName, PropsSet, bCompile));
}

// --- set_image ---
FMonolithActionResult FMonolithUIStylingActions::HandleSetImage(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FMonolithActionResult ParamError;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)) return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError)) return ParamError;

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

    UImage* ImageWidget = Cast<UImage>(Widget);
    if (!ImageWidget)
    {
        // Phase K
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("WidgetClass"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' (class %s) is not an Image widget."),
                *WidgetName, *Widget->GetClass()->GetName()),
            TEXT("set_image targets the UImage class only. For Border/Button backgrounds, use set_brush."),
            { TEXT("Image") });
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    int32 PropsSet = 0;

    // Texture
    FString TexturePath;
    Params->TryGetStringField(TEXT("texture_path"), TexturePath);
    if (!TexturePath.IsEmpty())
    {
        UTexture2D* Tex = FMonolithAssetUtils::LoadAssetByPath<UTexture2D>(TexturePath);
        if (Tex)
        {
            ImageWidget->SetBrushFromTexture(Tex);
            PropsSet++;
        }
        else
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
        }
    }

    // Material
    FString MaterialPath;
    Params->TryGetStringField(TEXT("material_path"), MaterialPath);
    if (!MaterialPath.IsEmpty())
    {
        UMaterialInterface* Mat = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(MaterialPath);
        if (Mat)
        {
            ImageWidget->SetBrushFromMaterial(Mat);
            PropsSet++;
        }
        else
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
        }
    }

    // Tint color
    FString TintColor;
    Params->TryGetStringField(TEXT("tint_color"), TintColor);
    if (!TintColor.IsEmpty())
    {
        ImageWidget->SetColorAndOpacity(MonolithUIInternal::ParseColor(TintColor));
        PropsSet++;
    }

    // Desired size
    const TSharedPtr<FJsonObject>* SizeObj = nullptr;
    if (Params->TryGetObjectField(TEXT("size"), SizeObj))
    {
        double X = 0.0, Y = 0.0;
        (*SizeObj)->TryGetNumberField(TEXT("x"), X);
        (*SizeObj)->TryGetNumberField(TEXT("y"), Y);
        FVector2D DesiredSize(X, Y);
        ImageWidget->SetDesiredSizeOverride(DesiredSize);
        PropsSet++;
    }

    if (PropsSet == 0)
    {
        // Phase K
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("MissingInput"),
            TEXT("/"),
            TEXT("No image properties specified."),
            TEXT("Pass one or more of the listed parameter keys to mutate the UImage."),
            { TEXT("texture_path"), TEXT("material_path"), TEXT("tint_color"), TEXT("size") }));
    }

    ImageWidget->SynchronizeProperties();
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);
    return FMonolithActionResult::Success(MakeStylingResponse(WidgetName, PropsSet, bCompile));
}

// --- set_retainer_effect_material ---
FMonolithActionResult FMonolithUIStylingActions::HandleSetRetainerEffectMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FMonolithActionResult ParamError;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)) return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError)) return ParamError;
    FString MaterialPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("material_path"), MaterialPath, ParamError)) return ParamError;

    FString TextureParameterName = TEXT("Texture");
    Params->TryGetStringField(TEXT("texture_parameter"), TextureParameterName);
    if (TextureParameterName.IsEmpty())
    {
        TextureParameterName = TEXT("Texture");
    }
    const FName TextureParameter(*TextureParameterName);

    bool bRequireUiMaterial = true;
    Params->TryGetBoolField(TEXT("require_ui_material"), bRequireUiMaterial);

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
            TEXT("Call ui::get_widget_tree to enumerate live widget names."));
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    URetainerBox* RetainerBox = Cast<URetainerBox>(Widget);
    if (!RetainerBox)
    {
        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("WidgetClass"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' (class %s) is not a RetainerBox widget."),
                *WidgetName, *Widget->GetClass()->GetName()),
            TEXT("set_retainer_effect_material targets URetainerBox. Use ui::get_widget_tree to find the RetainerBox name."),
            { TEXT("RetainerBox") });
        E.WidgetId = FName(*WidgetName);
        return MonolithUIInternal::MakeErrorFromSpecError(E);
    }

    UMaterialInterface* Material = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(MaterialPath);
    if (!Material)
    {
        return FMonolithActionResult::Error(FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
    }

    const bool bIsUiMaterial = IsUiMaterial(Material);
    if (bRequireUiMaterial && !bIsUiMaterial)
    {
        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("rule_id"), TEXT("MaterialDomainMismatch"));
        ErrorData->SetStringField(TEXT("material_path"), MaterialPath);
        ErrorData->SetStringField(TEXT("material_domain"), GetMaterialDomainString(Material));
        ErrorData->SetStringField(TEXT("expected_domain"), TEXT("MD_UI"));
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Retainer effect material '%s' is not UI-domain."), *MaterialPath),
            FMonolithJsonUtils::ErrInvalidParams).WithErrorData(ErrorData);
    }

    const bool bTextureParameterExists = HasExactTextureParameter(Material, TextureParameter);
    if (!bTextureParameterExists)
    {
        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("rule_id"), TEXT("RetainerEffectParameterMismatch"));
        ErrorData->SetStringField(TEXT("material_path"), MaterialPath);
        ErrorData->SetStringField(TEXT("texture_parameter"), TextureParameterName);
        ErrorData->SetArrayField(TEXT("available_texture_parameters"), CollectTextureParameterNames(Material));
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Retainer effect material '%s' does not expose exact texture parameter '%s'."),
                *MaterialPath, *TextureParameterName),
            FMonolithJsonUtils::ErrInvalidParams).WithErrorData(ErrorData);
    }

    RetainerBox->Modify();
    WBP->Modify();

    int32 PropsSet = 0;
    RetainerBox->SetEffectMaterial(Material);
    ++PropsSet;
    RetainerBox->SetTextureParameter(TextureParameter);
    ++PropsSet;

    bool bRetainRendering = false;
    if (Params->TryGetBoolField(TEXT("retain_rendering"), bRetainRendering))
    {
        RetainerBox->SetRetainRendering(bRetainRendering);
        ++PropsSet;
    }

    double RenderPhase = 0.0;
    double TotalRenderPhases = 0.0;
    const bool bHasRenderPhase = Params->TryGetNumberField(TEXT("render_phase"), RenderPhase);
    const bool bHasTotalRenderPhases = Params->TryGetNumberField(TEXT("total_render_phases"), TotalRenderPhases);
    if (bHasRenderPhase != bHasTotalRenderPhases)
    {
        return FMonolithActionResult::Error(TEXT("render_phase and total_render_phases must be supplied together."), FMonolithJsonUtils::ErrInvalidParams);
    }
    if (bHasRenderPhase && bHasTotalRenderPhases)
    {
        if (TotalRenderPhases < 1.0 || RenderPhase < 0.0)
        {
            return FMonolithActionResult::Error(TEXT("render_phase must be >= 0 and total_render_phases must be >= 1."), FMonolithJsonUtils::ErrInvalidParams);
        }
        RetainerBox->SetRenderingPhase(static_cast<int32>(RenderPhase), static_cast<int32>(TotalRenderPhases));
        ++PropsSet;
    }

    bool bRequestRender = false;
    Params->TryGetBoolField(TEXT("request_render"), bRequestRender);
    if (bRequestRender)
    {
        RetainerBox->RequestRender();
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeStylingResponse(WidgetName, PropsSet, bCompile);
    Result->SetStringField(TEXT("widget_class"), Widget->GetClass()->GetName());
    Result->SetStringField(TEXT("material_path"), MaterialPath);
    Result->SetStringField(TEXT("material_domain"), GetMaterialDomainString(Material));
    Result->SetBoolField(TEXT("is_ui_material"), bIsUiMaterial);
    Result->SetStringField(TEXT("texture_parameter"), TextureParameterName);
    Result->SetBoolField(TEXT("texture_parameter_exists"), bTextureParameterExists);
    Result->SetArrayField(TEXT("available_texture_parameters"), CollectTextureParameterNames(Material));
    Result->SetBoolField(TEXT("requested_render"), bRequestRender);
    Result->SetBoolField(TEXT("retain_rendering"), RetainerBox->IsRetainRendering());
    Result->SetBoolField(TEXT("render_on_phase"), RetainerBox->IsRenderOnPhase());
    Result->SetBoolField(TEXT("render_on_invalidation"), RetainerBox->IsRenderOnInvalidation());
    Result->SetNumberField(TEXT("phase"), RetainerBox->GetPhase());
    Result->SetNumberField(TEXT("phase_count"), RetainerBox->GetPhaseCount());
    Result->SetStringField(TEXT("proof_schema"), TEXT("ui_retainer_effect_binding.v1"));
    return FMonolithActionResult::Success(Result);
}
