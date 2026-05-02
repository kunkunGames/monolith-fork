// MonolithUIInternal.h
#pragma once

#include "CoreMinimal.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Components/ProgressBar.h"
#include "Components/CheckBox.h"
#include "Components/Slider.h"
#include "Components/ScrollBox.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/WrapBox.h"
#include "Components/UniformGridPanel.h"
#include "Components/GridPanel.h"
#include "Components/BackgroundBlur.h"
#include "Components/RichTextBlock.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/ComboBoxString.h"
#include "Components/InputKeySelector.h"
#include "Components/WidgetSwitcher.h"
#include "Components/ListView.h"
#include "Components/TileView.h"
#include "Components/NamedSlot.h"
#include "Dom/JsonObject.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprintEditorUtils.h"
#include "MonolithToolRegistry.h"
#include "WidgetBlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateBrush.h"
#include "MonolithAssetUtils.h"

namespace MonolithUIInternal
{
    inline bool TryGetRequiredString(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName,
        FString& OutValue,
        FMonolithActionResult& OutError)
    {
        if (!Object.IsValid() || !Object->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("Missing required param: %s"), FieldName));
            return false;
        }

        return true;
    }

    inline FString GetOptionalString(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const FString& DefaultValue = FString())
    {
        if (!Object.IsValid())
        {
            return DefaultValue;
        }

        FString Value;
        return Object->TryGetStringField(FieldName, Value) ? Value : DefaultValue;
    }

    inline bool GetOptionalBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, bool DefaultValue)
    {
        if (!Object.IsValid())
        {
            return DefaultValue;
        }

        bool Value = DefaultValue;
        return Object->TryGetBoolField(FieldName, Value) ? Value : DefaultValue;
    }

    // Load a widget blueprint by asset path, returning error result if not found
    inline UWidgetBlueprint* LoadWidgetBlueprint(const FString& AssetPath, FMonolithActionResult& OutError)
    {
        UWidgetBlueprint* WBP = FMonolithAssetUtils::LoadAssetByPath<UWidgetBlueprint>(AssetPath);
        if (!WBP)
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget Blueprint not found: %s"), *AssetPath));
            return nullptr;
        }
        return WBP;
    }

    // Map of widget class short names to UClass pointers
    inline UClass* WidgetClassFromName(const FString& ClassName)
    {
        static TMap<FString, UClass*> ClassMap;
        if (ClassMap.Num() == 0)
        {
            ClassMap.Add(TEXT("CanvasPanel"),       UCanvasPanel::StaticClass());
            ClassMap.Add(TEXT("VerticalBox"),        UVerticalBox::StaticClass());
            ClassMap.Add(TEXT("HorizontalBox"),      UHorizontalBox::StaticClass());
            ClassMap.Add(TEXT("Overlay"),             UOverlay::StaticClass());
            ClassMap.Add(TEXT("ScrollBox"),           UScrollBox::StaticClass());
            ClassMap.Add(TEXT("SizeBox"),             USizeBox::StaticClass());
            ClassMap.Add(TEXT("ScaleBox"),            UScaleBox::StaticClass());
            ClassMap.Add(TEXT("Border"),              UBorder::StaticClass());
            ClassMap.Add(TEXT("WrapBox"),             UWrapBox::StaticClass());
            ClassMap.Add(TEXT("UniformGridPanel"),    UUniformGridPanel::StaticClass());
            ClassMap.Add(TEXT("GridPanel"),           UGridPanel::StaticClass());
            ClassMap.Add(TEXT("WidgetSwitcher"),      UWidgetSwitcher::StaticClass());
            ClassMap.Add(TEXT("BackgroundBlur"),      UBackgroundBlur::StaticClass());
            ClassMap.Add(TEXT("NamedSlot"),           UNamedSlot::StaticClass());
            ClassMap.Add(TEXT("TextBlock"),           UTextBlock::StaticClass());
            ClassMap.Add(TEXT("RichTextBlock"),       URichTextBlock::StaticClass());
            ClassMap.Add(TEXT("Image"),               UImage::StaticClass());
            ClassMap.Add(TEXT("Button"),              UButton::StaticClass());
            ClassMap.Add(TEXT("CheckBox"),            UCheckBox::StaticClass());
            ClassMap.Add(TEXT("ProgressBar"),         UProgressBar::StaticClass());
            ClassMap.Add(TEXT("Slider"),              USlider::StaticClass());
            ClassMap.Add(TEXT("Spacer"),              USpacer::StaticClass());
            ClassMap.Add(TEXT("EditableText"),        UEditableText::StaticClass());
            ClassMap.Add(TEXT("EditableTextBox"),     UEditableTextBox::StaticClass());
            ClassMap.Add(TEXT("ComboBoxString"),      UComboBoxString::StaticClass());
            ClassMap.Add(TEXT("InputKeySelector"),    UInputKeySelector::StaticClass());
            ClassMap.Add(TEXT("ListView"),            UListView::StaticClass());
            ClassMap.Add(TEXT("TileView"),            UTileView::StaticClass());
        }

        if (UClass** Found = ClassMap.Find(ClassName))
        {
            return *Found;
        }

        // Fall back to FindFirstObject for full class paths
        UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
        return FoundClass;
    }

    // Serialize a single widget's slot properties to JSON
    inline TSharedPtr<FJsonObject> SerializeSlotProperties(UPanelSlot* Slot)
    {
        TSharedPtr<FJsonObject> SlotJson = MakeShared<FJsonObject>();
        if (!Slot) return SlotJson;

        SlotJson->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());

        if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Slot))
        {
            const FAnchorData& Layout = CS->GetLayout();
            TSharedPtr<FJsonObject> AnchorsJson = MakeShared<FJsonObject>();
            AnchorsJson->SetNumberField(TEXT("min_x"), Layout.Anchors.Minimum.X);
            AnchorsJson->SetNumberField(TEXT("min_y"), Layout.Anchors.Minimum.Y);
            AnchorsJson->SetNumberField(TEXT("max_x"), Layout.Anchors.Maximum.X);
            AnchorsJson->SetNumberField(TEXT("max_y"), Layout.Anchors.Maximum.Y);
            SlotJson->SetObjectField(TEXT("anchors"), AnchorsJson);

            TSharedPtr<FJsonObject> OffsetsJson = MakeShared<FJsonObject>();
            OffsetsJson->SetNumberField(TEXT("left"), Layout.Offsets.Left);
            OffsetsJson->SetNumberField(TEXT("top"), Layout.Offsets.Top);
            OffsetsJson->SetNumberField(TEXT("right"), Layout.Offsets.Right);
            OffsetsJson->SetNumberField(TEXT("bottom"), Layout.Offsets.Bottom);
            SlotJson->SetObjectField(TEXT("offsets"), OffsetsJson);

            TSharedPtr<FJsonObject> AlignJson = MakeShared<FJsonObject>();
            AlignJson->SetNumberField(TEXT("x"), Layout.Alignment.X);
            AlignJson->SetNumberField(TEXT("y"), Layout.Alignment.Y);
            SlotJson->SetObjectField(TEXT("alignment"), AlignJson);

            SlotJson->SetNumberField(TEXT("z_order"), CS->GetZOrder());
            SlotJson->SetBoolField(TEXT("auto_size"), CS->GetAutoSize());
        }
        else if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
        {
            SlotJson->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(VS->GetHorizontalAlignment()));
            SlotJson->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(VS->GetVerticalAlignment()));
        }
        else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
        {
            SlotJson->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(HS->GetHorizontalAlignment()));
            SlotJson->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(HS->GetVerticalAlignment()));
        }
        else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
        {
            SlotJson->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(OS->GetHorizontalAlignment()));
            SlotJson->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(OS->GetVerticalAlignment()));
        }

        return SlotJson;
    }

    // Recursively serialize widget tree to JSON
    inline TSharedPtr<FJsonObject> SerializeWidget(UWidget* Widget)
    {
        if (!Widget) return nullptr;

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Widget->GetName());
        Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
        Obj->SetStringField(TEXT("visibility"), UEnum::GetValueAsString(Widget->GetVisibility()));
        Obj->SetNumberField(TEXT("render_opacity"), Widget->GetRenderOpacity());
        Obj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

        // Slot info (how this widget sits in its parent)
        if (UPanelSlot* Slot = Widget->Slot)
        {
            Obj->SetObjectField(TEXT("slot"), SerializeSlotProperties(Slot));
        }

        // Children (if panel)
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            TArray<TSharedPtr<FJsonValue>> ChildArray;
            for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
            {
                if (UWidget* Child = Panel->GetChildAt(i))
                {
                    TSharedPtr<FJsonObject> ChildObj = SerializeWidget(Child);
                    if (ChildObj.IsValid())
                    {
                        ChildArray.Add(MakeShared<FJsonValueObject>(ChildObj));
                    }
                }
            }
            if (ChildArray.Num() > 0)
            {
                Obj->SetArrayField(TEXT("children"), ChildArray);
            }
        }

        return Obj;
    }

    // Anchor preset map
    inline FAnchors GetAnchorPreset(const FString& PresetName)
    {
        static TMap<FString, FAnchors> Presets;
        if (Presets.Num() == 0)
        {
            Presets.Add(TEXT("top_left"),            FAnchors(0.f, 0.f, 0.f, 0.f));
            Presets.Add(TEXT("top_center"),          FAnchors(0.5f, 0.f, 0.5f, 0.f));
            Presets.Add(TEXT("top_right"),           FAnchors(1.f, 0.f, 1.f, 0.f));
            Presets.Add(TEXT("center_left"),         FAnchors(0.f, 0.5f, 0.f, 0.5f));
            Presets.Add(TEXT("center"),              FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
            Presets.Add(TEXT("center_right"),        FAnchors(1.f, 0.5f, 1.f, 0.5f));
            Presets.Add(TEXT("bottom_left"),         FAnchors(0.f, 1.f, 0.f, 1.f));
            Presets.Add(TEXT("bottom_center"),       FAnchors(0.5f, 1.f, 0.5f, 1.f));
            Presets.Add(TEXT("bottom_right"),        FAnchors(1.f, 1.f, 1.f, 1.f));
            Presets.Add(TEXT("stretch_top"),         FAnchors(0.f, 0.f, 1.f, 0.f));
            Presets.Add(TEXT("stretch_bottom"),      FAnchors(0.f, 1.f, 1.f, 1.f));
            Presets.Add(TEXT("stretch_left"),        FAnchors(0.f, 0.f, 0.f, 1.f));
            Presets.Add(TEXT("stretch_right"),       FAnchors(1.f, 0.f, 1.f, 1.f));
            Presets.Add(TEXT("stretch_horizontal"),  FAnchors(0.f, 0.5f, 1.f, 0.5f));
            Presets.Add(TEXT("stretch_vertical"),    FAnchors(0.5f, 0.f, 0.5f, 1.f));
            Presets.Add(TEXT("stretch_fill"),        FAnchors(0.f, 0.f, 1.f, 1.f));
        }

        if (const FAnchors* Found = Presets.Find(PresetName))
        {
            return *Found;
        }
        return FAnchors(0.f, 0.f, 0.f, 0.f); // Default: top-left
    }

    // Parse hex color string to FLinearColor
    // Supports: "#RRGGBB", "#RRGGBBAA", "R,G,B,A" (0-1 floats)
    inline FLinearColor ParseColor(const FString& ColorStr)
    {
        FString S = ColorStr.TrimStartAndEnd();

        // Hex format
        if (S.StartsWith(TEXT("#")))
        {
            FColor C = FColor::FromHex(S);
            return FLinearColor(C);
        }

        // r,g,b,a float format
        TArray<FString> Parts;
        S.ParseIntoArray(Parts, TEXT(","));
        if (Parts.Num() >= 3)
        {
            float R = FCString::Atof(*Parts[0].TrimStartAndEnd());
            float G = FCString::Atof(*Parts[1].TrimStartAndEnd());
            float B = FCString::Atof(*Parts[2].TrimStartAndEnd());
            float A = Parts.Num() >= 4 ? FCString::Atof(*Parts[3].TrimStartAndEnd()) : 1.f;
            return FLinearColor(R, G, B, A);
        }

        return FLinearColor::White;
    }

    // Configure a canvas panel slot with anchor preset + position/size
    inline void ConfigureCanvasSlot(UPanelSlot* Slot, const FString& AnchorPreset,
        FVector2D Position = FVector2D::ZeroVector, FVector2D Size = FVector2D::ZeroVector,
        bool bAutoSize = false, FVector2D Alignment = FVector2D(0.f, 0.f))
    {
        if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Slot))
        {
            if (!AnchorPreset.IsEmpty())
            {
                CS->SetAnchors(GetAnchorPreset(AnchorPreset));
            }
            if (!Position.IsNearlyZero())
            {
                CS->SetPosition(Position);
            }
            if (!Size.IsNearlyZero())
            {
                CS->SetSize(Size);
            }
            if (bAutoSize)
            {
                CS->SetAutoSize(true);
            }
            if (!Alignment.IsNearlyZero())
            {
                CS->SetAlignment(Alignment);
            }
        }
    }

    // Create a new Widget Blueprint, returning it (or nullptr + error). Mirrors HandleCreateWidgetBlueprint logic.
    inline void RegisterVariableName(UWidgetBlueprint* WBP, const FName& VariableName)
    {
        if (!WBP || VariableName.IsNone())
        {
            return;
        }

        if (!WBP->WidgetVariableNameToGuidMap.Contains(VariableName))
        {
            WBP->OnVariableAdded(VariableName);
        }
    }

    inline void RegisterCreatedWidget(UWidgetBlueprint* WBP, UWidget* Widget)
    {
        if (!Widget)
        {
            return;
        }

        RegisterVariableName(WBP, Widget->GetFName());
    }

    inline void ReconcileWidgetVariableGuids(UWidgetBlueprint* WBP)
    {
        if (!WBP)
        {
            return;
        }

        WBP->ForEachSourceWidget([WBP](UWidget* Widget)
        {
            RegisterCreatedWidget(WBP, Widget);
        });
    }

    inline UWidgetBlueprint* CreateNewWidgetBlueprint(const FString& SavePath, FMonolithActionResult& OutError)
    {
        FString PackagePath, AssetName;
        SavePath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (AssetName.IsEmpty())
        {
            OutError = FMonolithActionResult::Error(TEXT("Invalid save_path — must contain at least one / separator"));
            return nullptr;
        }

        UPackage* Package = CreatePackage(*SavePath);
        if (!Package)
        {
            OutError = FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *SavePath));
            return nullptr;
        }

        UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
        Factory->BlueprintType = BPTYPE_Normal;
        Factory->ParentClass = UUserWidget::StaticClass();

        UObject* CreatedObj = Factory->FactoryCreateNew(
            UWidgetBlueprint::StaticClass(), Package,
            FName(*AssetName), RF_Public | RF_Standalone,
            nullptr, GWarn);

        UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(CreatedObj);
        if (!WBP)
        {
            OutError = FMonolithActionResult::Error(TEXT("UWidgetBlueprintFactory::FactoryCreateNew returned null"));
            return nullptr;
        }

        // Set CanvasPanel as root
        if (WBP->WidgetTree && !WBP->WidgetTree->RootWidget)
        {
            UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
            WBP->WidgetTree->RootWidget = Root;
            RegisterCreatedWidget(WBP, Root);
        }

        return WBP;
    }

    // Save and compile a widget blueprint
    inline void SaveAndCompileWidgetBlueprint(UWidgetBlueprint* WBP, const FString& SavePath)
    {
        ReconcileWidgetVariableGuids(WBP);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        FAssetRegistryModule::AssetCreated(WBP);
        WBP->GetPackage()->MarkPackageDirty();
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        UPackage::SavePackage(WBP->GetPackage(), WBP,
            *FPackageName::LongPackageNameToFilename(SavePath, FPackageName::GetAssetPackageExtension()),
            SaveArgs);
    }

    // Helper: add a child widget to a panel, returning the slot
    inline UWidget* AddChildWidget(UWidgetBlueprint* WBP, UPanelWidget* Parent,
        UClass* WidgetClass, const FString& Name)
    {
        UWidget* W = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*Name));
        if (W)
        {
            Parent->AddChild(W);
            RegisterCreatedWidget(WBP, W);
        }
        return W;
    }

    // Parse horizontal alignment from string
    inline EHorizontalAlignment ParseHAlign(const FString& S)
    {
        if (S == TEXT("Left")) return HAlign_Left;
        if (S == TEXT("Center")) return HAlign_Center;
        if (S == TEXT("Right")) return HAlign_Right;
        return HAlign_Fill;
    }

    // Parse vertical alignment from string
    inline EVerticalAlignment ParseVAlign(const FString& S)
    {
        if (S == TEXT("Top")) return VAlign_Top;
        if (S == TEXT("Center")) return VAlign_Center;
        if (S == TEXT("Bottom")) return VAlign_Bottom;
        return VAlign_Fill;
    }
}
