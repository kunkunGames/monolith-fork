// Copyright tumourlove. All Rights Reserved.
// MonolithUIRegistryActions.cpp

#include "MonolithUIRegistryActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithParamSchema.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UITypeRegistry.h"

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
}

FMonolithActionResult FMonolithUIRegistryActions::HandleDumpPropertyAllowlist(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
    }

    FString WidgetTypeStr;
    if (!Params->TryGetStringField(TEXT("widget_type"), WidgetTypeStr) || WidgetTypeStr.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Required parameter 'widget_type' missing or empty."), -32602);
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
