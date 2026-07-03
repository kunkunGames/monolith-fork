// Copyright tumourlove. All Rights Reserved.
// MonolithUIRegistryActions.cpp

#include "MonolithUIRegistryActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithParamSchema.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UITypeRegistry.h"

// Phase 2 (2026-05-16 UI gap audit) — Item #8 (add_widget_variable) +
// Item #11 (list_widget_property_enums) bring reflection-walking surface
#include "MonolithUIInternal.h"
// into this file. Item #8 wraps FBlueprintEditorUtils::AddMemberVariable,
// Item #11 walks FEnumProperty for the curated property allowlist.
#include "WidgetBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "MonolithUICommon.h"  // MonolithUI::LoadWidgetBlueprint
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Blueprint/WidgetTree.h"

namespace MonolithUIRegistryPhase2
{
    static FString ContainerKindToString(EUIContainerKind Kind)
    {
        switch (Kind)
        {
            case EUIContainerKind::Panel: return TEXT("Panel");
            case EUIContainerKind::Content: return TEXT("Content");
            case EUIContainerKind::Leaf:
            default: return TEXT("Leaf");
        }
    }

    static void AddStringArray(TSharedPtr<FJsonObject> Obj, const TCHAR* FieldName, const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> JsonValues;
        JsonValues.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            JsonValues.Add(MakeShared<FJsonValueString>(Value));
        }
        Obj->SetArrayField(FieldName, JsonValues);
    }

    static TArray<TSharedPtr<FJsonValue>> MakeEnumValues(UEnum* EnumPtr)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        if (!EnumPtr)
        {
            return Values;
        }

        const int32 NumEntries = EnumPtr->NumEnums();
        Values.Reserve(NumEntries);
        for (int32 Index = 0; Index < NumEntries; ++Index)
        {
            const FName NameByIndex = EnumPtr->GetNameByIndex(Index);
            if (NameByIndex.ToString().EndsWith(TEXT("_MAX")) && Index == NumEntries - 1)
            {
                continue;
            }

            TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
            ValueObj->SetStringField(TEXT("name"), EnumPtr->GetNameStringByIndex(Index));
            ValueObj->SetStringField(TEXT("display_name"), EnumPtr->GetDisplayNameTextByIndex(Index).ToString());
            ValueObj->SetNumberField(TEXT("value"), static_cast<double>(EnumPtr->GetValueByIndex(Index)));
            Values.Add(MakeShared<FJsonValueObject>(ValueObj));
        }
        return Values;
    }

    static UClass* ResolveWidgetClassFromToken(const FString& WidgetClassToken)
    {
        if (WidgetClassToken.IsEmpty())
        {
            return nullptr;
        }

        if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
        {
            const FUITypeRegistry& TypeRegistry = Sub->GetTypeRegistry();
            if (const FUITypeRegistryEntry* Entry = TypeRegistry.FindByToken(FName(*WidgetClassToken)))
            {
                if (Entry->WidgetClass.IsValid())
                {
                    return Entry->WidgetClass.Get();
                }
            }
        }

        if (UClass* Loaded = StaticLoadClass(UWidget::StaticClass(), nullptr, *WidgetClassToken))
        {
            return Loaded;
        }
        if (UClass* Found = FindFirstObject<UClass>(*WidgetClassToken, EFindFirstObjectOptions::NativeFirst))
        {
            return Found;
        }
        if (!WidgetClassToken.StartsWith(TEXT("U")))
        {
            return FindFirstObject<UClass>(*(TEXT("U") + WidgetClassToken), EFindFirstObjectOptions::NativeFirst);
        }
        return nullptr;
    }

    static FProperty* ResolvePropertyPath(UStruct* RootStruct, const FString& PropertyPath)
    {
        if (!RootStruct || PropertyPath.IsEmpty())
        {
            return nullptr;
        }

        TArray<FString> Segments;
        PropertyPath.ParseIntoArray(Segments, TEXT("."), /*bCullEmpty=*/true);
        if (Segments.Num() == 0)
        {
            return nullptr;
        }

        UStruct* CurrentStruct = RootStruct;
        FProperty* CurrentProperty = nullptr;
        for (int32 Index = 0; Index < Segments.Num(); ++Index)
        {
            CurrentProperty = CurrentStruct ? CurrentStruct->FindPropertyByName(FName(*Segments[Index])) : nullptr;
            if (!CurrentProperty)
            {
                return nullptr;
            }

            if (Index == Segments.Num() - 1)
            {
                return CurrentProperty;
            }

            if (FStructProperty* StructProperty = CastField<FStructProperty>(CurrentProperty))
            {
                CurrentStruct = StructProperty->Struct;
            }
            else if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(CurrentProperty))
            {
                CurrentStruct = ObjectProperty->PropertyClass;
            }
            else
            {
                return nullptr;
            }
        }

        return CurrentProperty;
    }

    static FString GetPropertyCppType(const FProperty* Property)
    {
        return Property ? Property->GetCPPType() : FString();
    }

    static UEnum* GetPropertyEnum(const FProperty* Property)
    {
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            return EnumProperty->GetEnum();
        }
        if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            return ByteProperty->Enum;
        }
        return nullptr;
    }

    static FString GetSetterName(const UClass* OwnerClass, const FString& LeafPropertyName)
    {
        if (!OwnerClass || LeafPropertyName.IsEmpty())
        {
            return FString();
        }

        const FName SetterName(*FString::Printf(TEXT("Set%s"), *LeafPropertyName));
        if (OwnerClass->FindFunctionByName(SetterName))
        {
            return SetterName.ToString();
        }
        return FString();
    }

    static FString GetFirstPathSegment(const FString& PropertyPath)
    {
        FString Segment = PropertyPath;
        if (Segment.IsEmpty())
        {
            return FString();
        }
        int32 DotIndex = INDEX_NONE;
        if (Segment.FindChar(TEXT('.'), DotIndex))
        {
            Segment.LeftInline(DotIndex);
        }
        return Segment;
    }

    static TSharedPtr<FJsonObject> MakePropertySchemaEntry(
        const FUIPropertyMapping& Mapping,
        const UClass* WidgetClass,
        const UClass* LiveSlotClass,
        const FString& AllowlistStatus,
        bool bSettable)
    {
        const bool bSlotPath = Mapping.JsonPath.StartsWith(TEXT("Slot."));
        FString ResolutionPath = Mapping.EnginePath.IsEmpty() ? Mapping.JsonPath : Mapping.EnginePath;
        UClass* ResolutionClass = const_cast<UClass*>(WidgetClass);
        if (bSlotPath && LiveSlotClass)
        {
            ResolutionClass = const_cast<UClass*>(LiveSlotClass);
            if (ResolutionPath.StartsWith(TEXT("Slot.")))
            {
                ResolutionPath.RightChopInline(5);
            }
        }

        FProperty* Property = ResolvePropertyPath(ResolutionClass, ResolutionPath);
        const FString LeafName = Property ? Property->GetName() : FPaths::GetCleanFilename(ResolutionPath);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"), Mapping.JsonPath);
        Entry->SetStringField(TEXT("json_path"), Mapping.JsonPath);
        Entry->SetStringField(TEXT("engine_path"), Mapping.EnginePath);
        Entry->SetStringField(TEXT("cpp_type"), GetPropertyCppType(Property));
        const bool bEffectiveSettable = bSettable && (!bSlotPath || Property != nullptr);
        Entry->SetBoolField(TEXT("settable"), bEffectiveSettable);
        Entry->SetStringField(TEXT("allowlist_status"), AllowlistStatus);
        Entry->SetStringField(TEXT("tooltip"), Mapping.Description);
        Entry->SetStringField(TEXT("description"), Mapping.Description);
        AddStringArray(Entry, TEXT("aliases"), TArray<FString>());

        if (Property)
        {
            Entry->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
            Entry->SetBoolField(TEXT("deprecated_direct_access"),
                Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->HasMetaData(TEXT("DeprecatedProperty")));
            if (UEnum* EnumPtr = GetPropertyEnum(Property))
            {
                Entry->SetStringField(TEXT("enum_name"), EnumPtr->GetName());
                Entry->SetArrayField(TEXT("enum_values"), MakeEnumValues(EnumPtr));
            }
            else
            {
                Entry->SetArrayField(TEXT("enum_values"), TArray<TSharedPtr<FJsonValue>>());
            }

            const UClass* SetterOwner = (bSlotPath && LiveSlotClass) ? LiveSlotClass : WidgetClass;
            Entry->SetStringField(TEXT("setter"), GetSetterName(SetterOwner, LeafName));
        }
        else
        {
            Entry->SetBoolField(TEXT("deprecated_direct_access"), false);
            Entry->SetArrayField(TEXT("enum_values"), TArray<TSharedPtr<FJsonValue>>());
            Entry->SetStringField(TEXT("setter"), FString());
        }

        if (bSlotPath)
        {
            Entry->SetStringField(TEXT("slot_class"), LiveSlotClass ? LiveSlotClass->GetPathName() : FString());
            if (!LiveSlotClass)
            {
                Entry->SetStringField(TEXT("slot_context"),
                    TEXT("contextual; settable only when the live parent creates a compatible slot class"));
            }
            else if (!Property)
            {
                Entry->SetStringField(TEXT("slot_context"),
                    TEXT("blocked for the supplied live widget slot class; this Slot.* path belongs to a different parent slot shape"));
                Entry->SetStringField(TEXT("blocked_reason"), FString::Printf(
                    TEXT("Live slot class '%s' does not expose engine path '%s'."),
                    *LiveSlotClass->GetPathName(),
                    *ResolutionPath));
            }
            else
            {
                Entry->SetStringField(TEXT("slot_context"),
                    TEXT("settable for the supplied live widget slot class"));
            }
        }

        return Entry;
    }

    static TSharedPtr<FJsonObject> MakeLiveChildCapacityEntry(UWidget* LiveWidget, const FUITypeRegistryEntry* TypeEntry)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        UPanelWidget* Panel = Cast<UPanelWidget>(LiveWidget);
        const int32 MaxChildren = Panel
            ? (Panel->CanHaveMultipleChildren() ? -1 : 1)
            : (TypeEntry ? TypeEntry->MaxChildren : 0);

        Entry->SetNumberField(TEXT("max_children"), MaxChildren);
        Entry->SetBoolField(TEXT("is_panel"), Panel != nullptr);
        Entry->SetBoolField(TEXT("can_add_child"), false);
        Entry->SetNumberField(TEXT("child_count"), 0);

        if (!LiveWidget)
        {
            Entry->SetStringField(TEXT("capacity_context"), TEXT("no live widget supplied"));
            return Entry;
        }

        Entry->SetStringField(TEXT("widget_class"), LiveWidget->GetClass()->GetPathName());
        if (!Panel)
        {
            Entry->SetStringField(TEXT("capacity_context"), TEXT("leaf widget cannot parent children"));
            return Entry;
        }

        const int32 ChildCount = Panel->GetChildrenCount();
        const bool bCanAddChild = Panel->CanAddMoreChildren();
        Entry->SetStringField(TEXT("panel_class"), Panel->GetClass()->GetPathName());
        Entry->SetNumberField(TEXT("child_count"), ChildCount);
        Entry->SetBoolField(TEXT("can_add_child"), bCanAddChild);

        if (MaxChildren < 0)
        {
            Entry->SetStringField(TEXT("capacity_context"), TEXT("multi-child panel can accept additional children"));
        }
        else if (MaxChildren == 1)
        {
            if (bCanAddChild)
            {
                Entry->SetStringField(TEXT("capacity_context"), TEXT("single-child container is empty"));
            }
            else
            {
                Entry->SetStringField(TEXT("capacity_context"), TEXT("single-child container is full"));
                if (const UWidget* ExistingChild = Panel->GetChildAt(0))
                {
                    Entry->SetStringField(TEXT("blocking_child"), ExistingChild->GetName());
                }
            }
        }
        else
        {
            Entry->SetStringField(TEXT("capacity_context"), TEXT("widget cannot parent children"));
        }

        return Entry;
    }

    static TSharedPtr<FJsonObject> MakeUnsafeReflectedPropertyEntry(const FProperty* Property)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        const FString PropertyName = Property ? Property->GetName() : FString();
        Entry->SetStringField(TEXT("path"), PropertyName);
        Entry->SetStringField(TEXT("json_path"), PropertyName);
        Entry->SetStringField(TEXT("engine_path"), PropertyName);
        Entry->SetStringField(TEXT("cpp_type"), GetPropertyCppType(Property));
        Entry->SetBoolField(TEXT("settable"), false);
        Entry->SetStringField(TEXT("allowlist_status"), TEXT("requires_raw_mode"));
        Entry->SetStringField(TEXT("tooltip"), TEXT("Reflected property is not in the curated Monolith UI allowlist."));
        Entry->SetStringField(TEXT("description"), TEXT("Reflected property is not in the curated Monolith UI allowlist."));
        AddStringArray(Entry, TEXT("aliases"), TArray<FString>());
        Entry->SetBoolField(TEXT("deprecated_direct_access"),
            Property && (Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->HasMetaData(TEXT("DeprecatedProperty"))));
        if (UEnum* EnumPtr = GetPropertyEnum(Property))
        {
            Entry->SetStringField(TEXT("enum_name"), EnumPtr->GetName());
            Entry->SetArrayField(TEXT("enum_values"), MakeEnumValues(EnumPtr));
        }
        else
        {
            Entry->SetArrayField(TEXT("enum_values"), TArray<TSharedPtr<FJsonValue>>());
        }
        return Entry;
    }

    static FMonolithActionResult HandleDescribeWidgetTypeSchema(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
        }

        UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
        if (!Sub)
        {
            return FMonolithActionResult::Error(
                TEXT("UMonolithUIRegistrySubsystem not available — editor not initialised?"), -32603);
        }

        FString WidgetClassToken;
        FString AssetPath;
        FString WidgetName;
        Params->TryGetStringField(TEXT("widget_class"), WidgetClassToken);
        Params->TryGetStringField(TEXT("asset_path"), AssetPath);
        Params->TryGetStringField(TEXT("widget_name"), WidgetName);

        bool bIncludeUnsafe = false;
        Params->TryGetBoolField(TEXT("include_unsafe"), bIncludeUnsafe);
        bool bIncludeInherited = false;
        Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

        UClass* WidgetClass = nullptr;
        UClass* LiveSlotClass = nullptr;
        UWidget* LiveWidget = nullptr;
        FString ResolvedFrom;

        if (!AssetPath.IsEmpty() && !WidgetName.IsEmpty())
        {
            FMonolithActionResult LoadErr;
            UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(AssetPath, LoadErr);
            if (!WBP)
            {
                return LoadErr;
            }
            if (!WBP->WidgetTree)
            {
                return FMonolithActionResult::Error(TEXT("WidgetTree is null."), -32603);
            }
            LiveWidget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
            if (!LiveWidget)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("Widget '%s' not found in '%s'."), *WidgetName, *AssetPath),
                    -32602);
            }
            WidgetClass = LiveWidget->GetClass();
            if (LiveWidget->Slot)
            {
                LiveSlotClass = LiveWidget->Slot->GetClass();
            }
            ResolvedFrom = TEXT("live_widget");
        }

        if (!WidgetClass && !WidgetClassToken.IsEmpty())
        {
            WidgetClass = ResolveWidgetClassFromToken(WidgetClassToken);
            ResolvedFrom = TEXT("widget_class");
        }

        if (!WidgetClass)
        {
            return FMonolithActionResult::Error(
                TEXT("Provide widget_class, or provide asset_path + widget_name for a live widget instance."),
                -32602);
        }

        const FUITypeRegistry& TypeRegistry = Sub->GetTypeRegistry();
        const FUITypeRegistryEntry* Entry = TypeRegistry.FindByClass(WidgetClass);
        const FName WidgetToken = Entry ? Entry->Token : MonolithUI::MakeTokenFromClassName(WidgetClass);

        TArray<TSharedPtr<FJsonValue>> Properties;
        TArray<TSharedPtr<FJsonValue>> SlotProperties;
        TSet<FString> MappedJsonPaths;
        TSet<FString> MappedReflectedPropertyRoots;
        int32 IncompatibleLiveSlotPathCount = 0;

        if (Entry)
        {
            for (const FUIPropertyMapping& Mapping : Entry->PropertyMappings)
            {
                MappedJsonPaths.Add(Mapping.JsonPath);
                MappedReflectedPropertyRoots.Add(GetFirstPathSegment(Mapping.JsonPath));
                MappedReflectedPropertyRoots.Add(GetFirstPathSegment(Mapping.EnginePath));
                const bool bSlotPath = Mapping.JsonPath.StartsWith(TEXT("Slot."));
                const bool bSlotSettable = !bSlotPath || LiveSlotClass != nullptr;
                const FString Status = bSlotPath ? TEXT("slot_only") : TEXT("allowed");
                TSharedPtr<FJsonObject> PropertyEntry = MakePropertySchemaEntry(
                    Mapping,
                    WidgetClass,
                    LiveSlotClass,
                    Status,
                    bSlotSettable);
                if (bSlotPath && LiveSlotClass && !PropertyEntry->GetBoolField(TEXT("settable")))
                {
                    ++IncompatibleLiveSlotPathCount;
                }
                Properties.Add(MakeShared<FJsonValueObject>(PropertyEntry));
                if (bSlotPath)
                {
                    SlotProperties.Add(MakeShared<FJsonValueObject>(PropertyEntry));
                }
            }
        }

        if (bIncludeUnsafe)
        {
            const EFieldIteratorFlags::SuperClassFlags SuperClassFlags =
                bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
            for (TFieldIterator<FProperty> It(WidgetClass, SuperClassFlags); It; ++It)
            {
                FProperty* Property = *It;
                if (!Property
                    || MappedJsonPaths.Contains(Property->GetName())
                    || MappedReflectedPropertyRoots.Contains(Property->GetName()))
                {
                    continue;
                }
                if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
                {
                    continue;
                }
                Properties.Add(MakeShared<FJsonValueObject>(MakeUnsafeReflectedPropertyEntry(Property)));
            }
        }

        TArray<TSharedPtr<FJsonValue>> Warnings;
        if (!Entry)
        {
            Warnings.Add(MakeShared<FJsonValueString>(
                TEXT("Widget class is not registered in Monolith's UI type registry; safe property mappings are unavailable.")));
        }
        else if (Entry->PropertyMappings.Num() == 0)
        {
            Warnings.Add(MakeShared<FJsonValueString>(
                TEXT("Widget type is registered but has no curated safe property mappings; use existing owner actions or add allowlist mappings before broad writes.")));
        }
        if (!AssetPath.IsEmpty() && !WidgetName.IsEmpty() && !LiveSlotClass)
        {
            Warnings.Add(MakeShared<FJsonValueString>(
                TEXT("Live widget has no parent slot, so Slot.* paths are not settable in this context.")));
        }
        if (IncompatibleLiveSlotPathCount > 0)
        {
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("%d Slot.* path(s) are not settable for the live slot class; inspect each slot_context before writing."),
                IncompatibleLiveSlotPathCount)));
        }

        TArray<TSharedPtr<FJsonValue>> NextActions;
        auto AddNextAction = [&NextActions](const FString& ToolName, bool bAvailable)
        {
            TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
            Action->SetStringField(TEXT("tool"), ToolName);
            Action->SetBoolField(TEXT("available"), bAvailable);
            NextActions.Add(MakeShared<FJsonValueObject>(Action));
        };
        AddNextAction(TEXT("ui.set_widget_property"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("set_widget_property")));
        AddNextAction(TEXT("ui.set_slot_property"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("set_slot_property")));
        AddNextAction(TEXT("ui.dump_property_allowlist"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("dump_property_allowlist")));
        AddNextAction(TEXT("ui.list_widget_property_enums"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("list_widget_property_enums")));

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("ok"), true);
        Result->SetStringField(TEXT("schema_version"), TEXT("ui_widget_type_schema.v1"));
        Result->SetStringField(TEXT("widget_class"), WidgetClass->GetPathName());
        Result->SetStringField(TEXT("widget_token"), WidgetToken.ToString());
        Result->SetStringField(TEXT("resolved_from"), ResolvedFrom);
        Result->SetStringField(TEXT("engine_path"), WidgetClass->GetPathName());
        Result->SetBoolField(TEXT("registered"), Entry != nullptr);
        Result->SetBoolField(TEXT("include_inherited"), bIncludeInherited);
        Result->SetBoolField(TEXT("include_unsafe"), bIncludeUnsafe);
        if (Entry)
        {
            Result->SetStringField(TEXT("container_kind"), ContainerKindToString(Entry->ContainerKind));
            Result->SetNumberField(TEXT("max_children"), Entry->MaxChildren);
            if (Entry->SlotClass.IsValid())
            {
                Result->SetStringField(TEXT("default_slot_class"), Entry->SlotClass->GetPathName());
            }
        }
        if (!AssetPath.IsEmpty())
        {
            Result->SetStringField(TEXT("asset_path"), AssetPath);
        }
        if (!WidgetName.IsEmpty())
        {
            Result->SetStringField(TEXT("widget_name"), WidgetName);
        }
        Result->SetStringField(TEXT("live_slot_class"), LiveSlotClass ? LiveSlotClass->GetPathName() : FString());
        if (LiveWidget)
        {
            const TSharedPtr<FJsonObject> ChildCapacity = MakeLiveChildCapacityEntry(LiveWidget, Entry);
            Result->SetObjectField(TEXT("live_child_capacity"), ChildCapacity);
            Result->SetNumberField(TEXT("live_child_count"), ChildCapacity->GetNumberField(TEXT("child_count")));
            Result->SetBoolField(TEXT("live_can_add_child"), ChildCapacity->GetBoolField(TEXT("can_add_child")));
        }
        Result->SetArrayField(TEXT("properties"), Properties);
        Result->SetArrayField(TEXT("slot_properties"), SlotProperties);
        Result->SetNumberField(TEXT("property_count"), Properties.Num());
        Result->SetNumberField(TEXT("slot_property_count"), SlotProperties.Num());
        Result->SetArrayField(TEXT("warnings"), Warnings);
        Result->SetArrayField(TEXT("next_actions"), NextActions);
        return FMonolithActionResult::Success(Result);
    }

    // ---- Phase 2 Item #8 helpers ---------------------------------------------
    //
    // ParsePinTypeFromString mirrors the canonical MCP-friendly token grammar
    // already shipped in `Source/MonolithBlueprint/Private/MonolithBlueprintInternal.h:782`.
    // Replicated locally rather than cross-module-included so MonolithUI keeps
    // its dependency boundary clean (MonolithBlueprint is NOT listed in
    // MonolithUI.Build.cs PrivateDependencyModuleNames). The grammar is the
    // SAME tokens (bool/int/int64/float/double/string/name/text/byte/
    // object:Class/class:Class/struct:Name/enum:Name/softobject:Class/
    // softclass:Class/exec/wildcard, with container prefixes array:/set:/map:).
    // Cross-reference: MonolithBlueprintInternal.h:782-915.

    static FEdGraphPinType ParsePinTypeFromString(const FString& TypeStr)
    {
        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;  // safe default

        FString BaseType = TypeStr;
        EPinContainerType ContainerType = EPinContainerType::None;

        if (TypeStr.StartsWith(TEXT("array:")))
        {
            ContainerType = EPinContainerType::Array;
            BaseType = TypeStr.Mid(6);
        }
        else if (TypeStr.StartsWith(TEXT("set:")))
        {
            ContainerType = EPinContainerType::Set;
            BaseType = TypeStr.Mid(4);
        }
        else if (TypeStr.StartsWith(TEXT("map:")))
        {
            ContainerType = EPinContainerType::Map;
            int32 SecondColon;
            if (BaseType.Mid(4).FindChar(TEXT(':'), SecondColon))
            {
                BaseType = TypeStr.Mid(4, SecondColon);
                const FString ValueType = TypeStr.Mid(4 + SecondColon + 1);
                PinType.PinValueType = FEdGraphTerminalType();
                const FEdGraphPinType ValPinType = ParsePinTypeFromString(ValueType);
                PinType.PinValueType.TerminalCategory = ValPinType.PinCategory;
                PinType.PinValueType.TerminalSubCategory = ValPinType.PinSubCategory;
                PinType.PinValueType.TerminalSubCategoryObject = ValPinType.PinSubCategoryObject;
            }
            else
            {
                BaseType = TypeStr.Mid(4);
            }
        }
        PinType.ContainerType = ContainerType;

        if (BaseType == TEXT("bool"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        }
        else if (BaseType == TEXT("int"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
        }
        else if (BaseType == TEXT("int64"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
        }
        else if (BaseType == TEXT("float"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            PinType.PinSubCategory = TEXT("float");
        }
        else if (BaseType == TEXT("double"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            PinType.PinSubCategory = TEXT("double");
        }
        else if (BaseType == TEXT("string"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_String;
        }
        else if (BaseType == TEXT("name"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
        }
        else if (BaseType == TEXT("text"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
        }
        else if (BaseType == TEXT("byte"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
        }
        else if (BaseType.StartsWith(TEXT("object:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            const FString ClassName = BaseType.Mid(7);
            if (UClass* C = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = C;
        }
        else if (BaseType.StartsWith(TEXT("class:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
            const FString ClassName = BaseType.Mid(6);
            if (UClass* C = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = C;
        }
        else if (BaseType.StartsWith(TEXT("struct:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            const FString StructName = BaseType.Mid(7);
            if (UScriptStruct* S = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = S;
        }
        else if (BaseType.StartsWith(TEXT("enum:")))
        {
            // PC_Byte + sub-category-object-as-enum is the canonical Kismet pattern
            // for TEnumAsByte<EFoo> style variables. UE 5.7 still uses PC_Byte for
            // editor-visible enums; PC_Enum is reserved for C++-only enum class.
            // FBlueprintEditorUtils::AddMemberVariable accepts either.
            PinType.PinCategory = UEdGraphSchema_K2::PC_Enum;
            const FString EnumName = BaseType.Mid(5);
            if (UEnum* E = FindFirstObject<UEnum>(*EnumName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = E;
        }
        else if (BaseType.StartsWith(TEXT("softobject:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
            const FString ClassName = BaseType.Mid(11);
            if (UClass* C = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = C;
        }
        else if (BaseType.StartsWith(TEXT("softclass:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
            const FString ClassName = BaseType.Mid(10);
            if (UClass* C = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = C;
        }
        else if (BaseType == TEXT("exec"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        }
        else if (BaseType == TEXT("wildcard"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        }

        return PinType;
    }

    // ---- Phase 2 Item #8 — add_widget_variable -------------------------------
    //
    // Wraps FBlueprintEditorUtils::AddMemberVariable so a caller can stamp a
    // user-variable on the WBP without having to drive the BlueprintEditor UI.
    // The variable becomes editable in the WBP's Details panel and shows up in
    // get_widget_tree -> NewVariables. AddMemberVariable handles default flags
    // (CPF_Edit | CPF_BlueprintVisible | CPF_DisableEditOnInstance) so the new
    // variable matches engine-canonical "user variable" semantics.

    static FMonolithActionResult HandleAddWidgetVariable(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
        }

        FMonolithActionResult ParamError;
        FString WbpPath;
        if (!Params->TryGetStringField(TEXT("wbp_path"), WbpPath))
        {
            if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), WbpPath, ParamError))
            {
                ParamError.ErrorCode = -32602;
                return ParamError;
            }
        }
        if (WbpPath.IsEmpty())
            return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"), -32602);

        FString VarName, VarType;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("var_name"), VarName, ParamError))
        {
            ParamError.ErrorCode = -32602;
            return ParamError;
        }
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("var_type"), VarType, ParamError))
            return FMonolithActionResult::Error(
                TEXT("Required parameter 'var_type' missing or empty. Use bool/int/int64/float/double/string/"
                     "name/text/byte/object:Class/class:Class/struct:Name/enum:Name/softobject:Class/softclass:Class. "
                     "Container prefixes: array:/set:/map:Key:Value."),
                -32602);

        FString DefaultValue;
        Params->TryGetStringField(TEXT("default_value"), DefaultValue);

        FString VarCategory;
        Params->TryGetStringField(TEXT("var_category"), VarCategory);

        FMonolithActionResult LoadErr;
        UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(WbpPath, LoadErr);
        if (!WBP) return LoadErr;

        const FEdGraphPinType PinType = ParsePinTypeFromString(VarType);

        // Quick sanity check — if the caller passed an unknown enum:/struct:/
        // object: token, ParsePinTypeFromString silently dropped the resolve
        // and the PinSubCategoryObject is null. AddMemberVariable would still
        // create the variable but with an invalid type — refuse loudly here.
        const bool bWantsTypeObject =
            PinType.PinCategory == UEdGraphSchema_K2::PC_Object   ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_Class    ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_Struct   ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_Enum     ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass;
        if (bWantsTypeObject && !PinType.PinSubCategoryObject.IsValid())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("var_type '%s' references a class/struct/enum that could not be resolved via FindFirstObject. Use a fully-qualified name (e.g. 'object:UMG.TextBlock' or pass the engine class short name like 'TextBlock')."),
                    *VarType),
                -32602);
        }

        const FName NewVarFName(*VarName);
        const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(WBP, NewVarFName, PinType, DefaultValue);
        if (!bAdded)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("AddMemberVariable returned false for '%s' (likely already exists on '%s' or a parent class)."),
                    *VarName, *WbpPath),
                -32602);
        }

        // Apply optional category — keeps the WBP's Details panel grouped under
        // a user-supplied label. AddMemberVariable defaults to VR_DefaultCategory
        // (BlueprintEditorUtils.cpp:4681) so we only override when caller asked.
        if (!VarCategory.IsEmpty())
        {
            // Pass bDontRecompile=true — we drive the compile ourselves below
            // so the category set + variable add commit in a single compile pass
            // rather than thrashing the compile manager twice. UE signature:
            // (Blueprint, VarName, InLocalVarScope, NewCategory, bDontRecompile)
            // — BlueprintEditorUtils.cpp:4058.
            FBlueprintEditorUtils::SetBlueprintVariableCategory(
                WBP, NewVarFName, /*InLocalVarScope=*/nullptr,
                FText::FromString(VarCategory), /*bDontRecompile=*/true);
        }

        // Compile so the BP regenerates its Skeleton class with the new
        // variable — downstream get_widget_tree / set_widget_property reads
        // see the variable on the Skeleton (next-tick) instead of waiting
        // for the next compile pass.
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("wbp_path"), WbpPath);
        Result->SetStringField(TEXT("var_name"), VarName);
        Result->SetStringField(TEXT("var_type"), VarType);
        Result->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());
        if (PinType.PinSubCategoryObject.IsValid())
        {
            Result->SetStringField(TEXT("pin_sub_category_object"), PinType.PinSubCategoryObject->GetName());
        }
        if (!VarCategory.IsEmpty())
        {
            Result->SetStringField(TEXT("var_category"), VarCategory);
        }
        if (!DefaultValue.IsEmpty())
        {
            Result->SetStringField(TEXT("default_value"), DefaultValue);
        }
        return FMonolithActionResult::Success(Result);
    }

    // ---- Phase 4 Item #2 — set_widget_is_variable ----------------------------
    //
    // First-class flip of UWidget::bIsVariable. When true, the widget is exposed
    // as a named BindWidget-style variable on the WBP's generated class (visible
    // to get_variables and accessible from the graph); when false, it becomes an
    // anonymous tree-only widget. bIsVariable is a public uint8:1 member on the
    // base UWidget — the engine's own SWidgetDetailsView sets it via direct
    // member write (UMGEditor SWidgetDetailsView.cpp), which is the path used
    // here since MonolithUI links UMG. MonolithBlueprint's get_variables reads
    // the same flag through reflection only because that module has no UMG dep.
    // After the flip the WBP must be marked structurally modified + compiled so
    // the generated class regenerates with (or without) the variable binding.

    static FMonolithActionResult HandleSetWidgetIsVariable(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
        }

        FMonolithActionResult ParamError;
        FString WbpPath;
        if (!Params->TryGetStringField(TEXT("wbp_path"), WbpPath))
        {
            if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), WbpPath, ParamError))
            {
                ParamError.ErrorCode = -32602;
                return ParamError;
            }
        }
        if (WbpPath.IsEmpty())
            return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"), -32602);

        FString WidgetName;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError))
        {
            ParamError.ErrorCode = -32602;
            return ParamError;
        }

        bool bIsVariable = false;
        if (!Params->TryGetBoolField(TEXT("is_variable"), bIsVariable))
        {
            return FMonolithActionResult::Error(TEXT("Required parameter 'is_variable' (bool) missing."), -32602);
        }

        FMonolithActionResult LoadErr;
        UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(WbpPath, LoadErr);
        if (!WBP) return LoadErr;

        if (!WBP->WidgetTree)
        {
            return FMonolithActionResult::Error(
                TEXT("WidgetTree is null (editor-only data not available)."), -32603);
        }

        UWidget* Target = WBP->WidgetTree->FindWidget(FName(*WidgetName));
        if (!Target)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget '%s' not found in '%s'."), *WidgetName, *WbpPath),
                -32602);
        }

        const bool bWasVariable = Target->bIsVariable;

        // Mark the WBP modified BEFORE the write so the transaction captures the
        // pre-edit state, matching the engine's Modify()-then-mutate ordering.
        Target->Modify();
        Target->bIsVariable = bIsVariable;

        // Structural modification + compile so the generated class adds/removes
        // the variable binding; without this the flip stays inert until the next
        // editor-driven compile.
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget_name"), WidgetName);
        Result->SetBoolField(TEXT("is_variable"), bIsVariable);
        Result->SetBoolField(TEXT("changed"), bWasVariable != bIsVariable);
        return FMonolithActionResult::Success(Result);
    }

    // ---- Phase 2 Item #11 — list_widget_property_enums -----------------------
    //
    // Walks the curated allowlist for a given widget type (or resolved WBP)
    // and returns every enum-typed property with its enumerator names. LLMs
    // use this to know "what valid values can I pass to set_widget_property
    // when the property is an enum" — the allowlist gates the writes, but it
    // does NOT advertise the legal value set.
    //
    // Resolution order: prefer wbp_path if supplied (so the caller resolves the
    // OWN class with custom variables / inherited UPROPERTIES); fall back to
    // widget_class token via FUITypeRegistry::FindByToken. Optional
    // property_name filter narrows to one entry.

    static FMonolithActionResult HandleListWidgetPropertyEnums(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
        }

        FString WbpPath, WidgetClassToken, PropertyNameFilter;
        Params->TryGetStringField(TEXT("wbp_path"), WbpPath);
        Params->TryGetStringField(TEXT("widget_class"), WidgetClassToken);
        Params->TryGetStringField(TEXT("property_name"), PropertyNameFilter);

        // At least one of wbp_path / widget_class is required so we have a
        // concrete UClass to walk. The plan calls this "at least one of
        // wbp_path / widget_class / property_name required" — property_name
        // alone is insufficient because we need a class context to find it.
        if (WbpPath.IsEmpty() && WidgetClassToken.IsEmpty())
        {
            return FMonolithActionResult::Error(
                TEXT("At least one of 'wbp_path' or 'widget_class' is required."),
                -32602);
        }

        UClass* WidgetClass = nullptr;
        if (!WbpPath.IsEmpty())
        {
            FMonolithActionResult LoadErr;
            UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(WbpPath, LoadErr);
            if (!WBP) return LoadErr;
            // For WBP-rooted introspection we walk the widget tree's root
            // class — that's the most common "what enums exist on the widgets
            // I instantiated in this WBP" question. Caller can narrow further
            // via property_name filter.
            WidgetClass = WBP->GeneratedClass;
        }

        if (!WidgetClass && !WidgetClassToken.IsEmpty())
        {
            // Token-form lookup via the type registry (canonical surface).
            // Fall back to FindFirstObject in case the caller passed a raw
            // class name that the registry has not registered (e.g. a marketplace
            // widget that hot-loaded after the registry scan).
            if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
            {
                const FUITypeRegistry& TypeRegistry = Sub->GetTypeRegistry();
                if (const FUITypeRegistryEntry* Entry = TypeRegistry.FindByToken(FName(*WidgetClassToken)))
                {
                    WidgetClass = Entry->WidgetClass.Get();
                }
            }
            if (!WidgetClass)
            {
                WidgetClass = FindFirstObject<UClass>(*WidgetClassToken, EFindFirstObjectOptions::NativeFirst);
                if (!WidgetClass)
                {
                    // Try with leading 'U' prefix (engine convention)
                    WidgetClass = FindFirstObject<UClass>(*(TEXT("U") + WidgetClassToken), EFindFirstObjectOptions::NativeFirst);
                }
            }
        }

        if (!WidgetClass)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Could not resolve a UClass for widget_class='%s' / wbp_path='%s'."),
                    *WidgetClassToken, *WbpPath),
                -32602);
        }

        // Walk the UClass property surface looking for enum types. Two flavours
        // surface in UE 5.7:
        //   * FEnumProperty       — modern enum class declared via UENUM().
        //   * FByteProperty + Enum — legacy TEnumAsByte<EFoo>. The byte property
        //                            has an Enum() accessor returning UEnum*.
        // We surface BOTH so callers can pass enum values from either flavour.

        TArray<TSharedPtr<FJsonValue>> Enums;
        for (TFieldIterator<FProperty> It(WidgetClass); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop) continue;

            const FName PropName = Prop->GetFName();
            if (!PropertyNameFilter.IsEmpty()
                && !PropName.ToString().Equals(PropertyNameFilter, ESearchCase::IgnoreCase))
            {
                continue;
            }

            UEnum* EnumPtr = nullptr;
            FString EnumKind;
            if (FEnumProperty* EProp = CastField<FEnumProperty>(Prop))
            {
                EnumPtr = EProp->GetEnum();
                EnumKind = TEXT("EnumProperty");
            }
            else if (FByteProperty* BProp = CastField<FByteProperty>(Prop))
            {
                if (BProp->Enum)  // TEnumAsByte<E> only — plain bytes have null Enum
                {
                    EnumPtr = BProp->Enum;
                    EnumKind = TEXT("ByteProperty");
                }
            }

            if (!EnumPtr) continue;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("property_name"), PropName.ToString());
            Entry->SetStringField(TEXT("property_kind"), EnumKind);
            Entry->SetStringField(TEXT("enum_name"), EnumPtr->GetName());

            // Honour the engine's "MAX" sentinel suppression — UE-generated
            // enums historically include a synthetic <Name>_MAX terminator
            // that is not a real value. NumEnums() includes it; we filter on
            // the standard `IsValidEnumValue` test.
            TArray<TSharedPtr<FJsonValue>> Values;
            const int32 NumEntries = EnumPtr->NumEnums();
            Values.Reserve(NumEntries);
            for (int32 i = 0; i < NumEntries; ++i)
            {
                // Skip _MAX sentinel (last entry on UENUM() with no explicit values)
                const int64 RawVal = EnumPtr->GetValueByIndex(i);
                const FName NameByIdx = EnumPtr->GetNameByIndex(i);
                if (NameByIdx.ToString().EndsWith(TEXT("_MAX")) && i == NumEntries - 1)
                    continue;

                TSharedPtr<FJsonObject> ValObj = MakeShared<FJsonObject>();
                ValObj->SetStringField(TEXT("name"), EnumPtr->GetNameStringByIndex(i));
                ValObj->SetStringField(TEXT("display_name"), EnumPtr->GetDisplayNameTextByIndex(i).ToString());
                ValObj->SetNumberField(TEXT("value"), static_cast<double>(RawVal));
                Values.Add(MakeShared<FJsonValueObject>(ValObj));
            }
            Entry->SetArrayField(TEXT("valid_values"), Values);
            Entry->SetNumberField(TEXT("valid_value_count"), Values.Num());

            Enums.Add(MakeShared<FJsonValueObject>(Entry));
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget_class"), WidgetClass->GetName());
        if (!WbpPath.IsEmpty()) Result->SetStringField(TEXT("wbp_path"), WbpPath);
        if (!PropertyNameFilter.IsEmpty()) Result->SetStringField(TEXT("property_name_filter"), PropertyNameFilter);
        Result->SetArrayField(TEXT("enum_properties"), Enums);
        Result->SetNumberField(TEXT("enum_property_count"), Enums.Num());
        return FMonolithActionResult::Success(Result);
    }
}

void FMonolithUIRegistryActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_property_allowlist"),
        TEXT("Dump the property allowlist for a widget type. Returns {type, allowed_paths:[...]}."),
        FMonolithActionHandler::CreateStatic(&HandleDumpPropertyAllowlist),
        FParamSchemaBuilder()
            .Required(TEXT("widget_type"), TEXT("string"),
                TEXT("Widget token (e.g. \"VerticalBox\", \"TextBlock\", \"RoundedBorder\")."))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("describe_widget_type_schema"),
        TEXT("Describe a UMG widget type or live widget instance using Monolith's existing type registry, "
             "property allowlist, reflected cpp types, enum values, slot context, and next owner actions. "
             "Use before set_widget_property/set_slot_property to avoid raw-mode guesses."),
        FMonolithActionHandler::CreateStatic(&MonolithUIRegistryPhase2::HandleDescribeWidgetTypeSchema),
        FParamSchemaBuilder()
            .Optional(TEXT("widget_class"), TEXT("string"), TEXT("Widget token or class path, e.g. TextBlock, Button, /Script/UMG.Button"))
            .OptionalAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint path. Combine with widget_name to describe a live widget instance."))
            .Optional(TEXT("widget_name"), TEXT("string"), TEXT("Widget name inside asset_path. When supplied, live slot context is included."))
            .Optional(TEXT("include_inherited"), TEXT("bool"), TEXT("When include_unsafe=true, include inherited reflected fields as raw-mode-only entries."), TEXT("false"))
            .Optional(TEXT("include_unsafe"), TEXT("bool"), TEXT("Also include reflected editable properties that require raw_mode and are not in the curated allowlist."), TEXT("false"))
            .Optional(TEXT("include_examples"), TEXT("bool"), TEXT("Reserved for future examples; current result returns next_actions and schema metadata."), TEXT("true"))
            .Build(),
        TEXT("Registry")
    );

    // Phase 2 Item #8 (2026-05-16 UI gap audit): add_widget_variable.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_widget_variable"),
        TEXT("Add a member variable to a Widget Blueprint via FBlueprintEditorUtils::AddMemberVariable. "
             "var_type accepts MCP-token grammar: bool|int|int64|float|double|string|name|text|byte|"
             "object:Class|class:Class|struct:Name|enum:Name|softobject:Class|softclass:Class|exec|wildcard. "
             "Container prefixes (array:|set:|map:Key:Value) compose. "
             "AddMemberVariable defaults flags CPF_Edit|CPF_BlueprintVisible|CPF_DisableEditOnInstance — matches "
             "the editor's 'add variable' affordance."),
        FMonolithActionHandler::CreateStatic(&MonolithUIRegistryPhase2::HandleAddWidgetVariable),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("wbp_path"), TEXT("Widget Blueprint path (alias: asset_path)"))
            .Required(TEXT("var_name"), TEXT("string"), TEXT("New variable FName (uniqueness enforced by AddMemberVariable)"))
            .Required(TEXT("var_type"), TEXT("string"), TEXT("Type token. See action description for grammar."))
            .Optional(TEXT("default_value"), TEXT("string"), TEXT("Default value as UE text format (engine ImportText grammar)"))
            .Optional(TEXT("var_category"), TEXT("string"), TEXT("Details-panel grouping label"))
            .Build(),
        TEXT("Registry")
    );

    // Phase 4 Item #2 (2026-05-23 UI gap closure): set_widget_is_variable.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_widget_is_variable"),
        TEXT("Set a UWidget's bIsVariable flag. When true the widget is exposed as a named "
             "variable on the WBP's generated class (visible to get_variables, accessible from "
             "the graph); when false it becomes an anonymous tree-only widget. Marks the WBP "
             "structurally modified + compiles so the binding materializes. "
             "Returns {widget_name, is_variable, changed}."),
        FMonolithActionHandler::CreateStatic(&MonolithUIRegistryPhase2::HandleSetWidgetIsVariable),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("wbp_path"), TEXT("Widget Blueprint path"), {TEXT("asset_path")})
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the widget in the WBP's WidgetTree"))
            .Required(TEXT("is_variable"), TEXT("bool"), TEXT("New bIsVariable value (true = expose as named variable)"))
            .Build(),
        TEXT("Registry")
    );

    // Phase 2 Item #11 (2026-05-16 UI gap audit): list_widget_property_enums.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("list_widget_property_enums"),
        TEXT("List enum-typed properties on a widget class (or WBP's generated class) and their valid values. "
             "Surfaces both FEnumProperty (modern enum class) AND FByteProperty-with-Enum (legacy TEnumAsByte). "
             "Use this to discover the legal value set for set_widget_property writes that target enum fields."),
        FMonolithActionHandler::CreateStatic(&MonolithUIRegistryPhase2::HandleListWidgetPropertyEnums),
        FParamSchemaBuilder()
            .OptionalAssetPath(TEXT("wbp_path"), TEXT("Resolve via this WBP's generated class (highest priority)"))
            .Optional(TEXT("widget_class"), TEXT("string"), TEXT("Token form (e.g. 'TextBlock', 'CommonButtonBase')"))
            .Optional(TEXT("property_name"), TEXT("string"), TEXT("Optional case-insensitive filter; returns only the named property"))
            .Build(),
        TEXT("Registry")
    );
}

FMonolithActionResult FMonolithUIRegistryActions::HandleDumpPropertyAllowlist(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
    }

    FMonolithActionResult ParamError;
    FString WidgetTypeStr;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_type"), WidgetTypeStr, ParamError))
        {
            ParamError.ErrorCode = -32602;
            return ParamError;
        }

    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    if (!Sub)
    {
        return FMonolithActionResult::Error(
            TEXT("UMonolithUIRegistrySubsystem not available — editor not initialised?"), -32603);
    }

    const FName WidgetToken(*WidgetTypeStr);
    const FUITypeRegistry& TypeRegistry = Sub->GetTypeRegistry();
    const FUITypeRegistryEntry* Entry = TypeRegistry.FindByToken(WidgetToken);

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("type"), WidgetTypeStr);

    if (!Entry)
    {
        // Unknown type — return empty allowed_paths + a hint that the type is
        // not registered. Distinct from a registered-but-no-mappings response.
        Result->SetBoolField(TEXT("registered"), false);
        Result->SetArrayField(TEXT("allowed_paths"), TArray<TSharedPtr<FJsonValue>>());
        Result->SetStringField(TEXT("note"),
            TEXT("Widget type not in registry. Check spelling or confirm the providing plugin is loaded."));
        return FMonolithActionResult::Success(Result);
    }

    Result->SetBoolField(TEXT("registered"), true);

    // Container kind / max-children for context — useful for LLM consumers.
    const TCHAR* KindToken = TEXT("Leaf");
    switch (Entry->ContainerKind)
    {
        case EUIContainerKind::Panel:   KindToken = TEXT("Panel");   break;
        case EUIContainerKind::Content: KindToken = TEXT("Content"); break;
        case EUIContainerKind::Leaf:    KindToken = TEXT("Leaf");    break;
    }
    Result->SetStringField(TEXT("container_kind"), KindToken);
    Result->SetNumberField(TEXT("max_children"), Entry->MaxChildren);

    if (Entry->WidgetClass.IsValid())
    {
        Result->SetStringField(TEXT("widget_class"), Entry->WidgetClass->GetPathName());
    }

    const FUIPropertyAllowlist& Allowlist = Sub->GetAllowlist();
    const TArray<FString>& AllowedPaths = Allowlist.GetAllowedPaths(WidgetToken);

    TArray<TSharedPtr<FJsonValue>> PathValues;
    PathValues.Reserve(AllowedPaths.Num());
    for (const FString& Path : AllowedPaths)
    {
        PathValues.Add(MakeShared<FJsonValueString>(Path));
    }
    Result->SetArrayField(TEXT("allowed_paths"), PathValues);
    Result->SetNumberField(TEXT("allowed_path_count"), AllowedPaths.Num());

    return FMonolithActionResult::Success(Result);
}
