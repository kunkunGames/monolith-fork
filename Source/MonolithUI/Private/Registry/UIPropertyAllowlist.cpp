// Copyright tumourlove. All Rights Reserved.
// UIPropertyAllowlist.cpp
//
// Implementation: lazy projection over `FUITypeRegistry::PropertyMappings`.
// First call for a given token populates two parallel mutable maps:
//   * `AllowedPaths` — the hot-path TSet used by `IsAllowed`.
//   * `AllowedPathsList` — a stable-ordered TArray for diagnostic dumps.
//
// Both maps share the same key (FName widget token). Keeping them parallel
// (rather than just rebuilding a TArray from the TSet on every call) avoids
// repeated allocations in the dump diagnostic path.

#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UITypeRegistry.h"

FUIPropertyAllowlist::FUIPropertyAllowlist(const FUITypeRegistry& InRegistry)
    : Registry(InRegistry)
{
}

bool FUIPropertyAllowlist::IsAllowed(const FName& WidgetToken, const FString& JsonPath) const
{
    if (WidgetToken.IsNone() || JsonPath.IsEmpty())
    {
        return false;
    }

    if (!AllowedPaths.Contains(WidgetToken))
    {
        BuildCacheFor(WidgetToken);
    }

    const TSet<FString>* PathSet = AllowedPaths.Find(WidgetToken);
    return PathSet && PathSet->Contains(JsonPath);
}

const TArray<FString>& FUIPropertyAllowlist::GetAllowedPaths(const FName& WidgetToken) const
{
    if (!AllowedPathsList.Contains(WidgetToken))
    {
        BuildCacheFor(WidgetToken);
    }

    if (const TArray<FString>* List = AllowedPathsList.Find(WidgetToken))
    {
        return *List;
    }

    static const TArray<FString> Empty;
    return Empty;
}

void FUIPropertyAllowlist::Invalidate()
{
    AllowedPaths.Reset();
    AllowedPathsList.Reset();
}

void FUIPropertyAllowlist::BuildCacheFor(const FName& WidgetToken) const
{
    TSet<FString>& PathSet = AllowedPaths.FindOrAdd(WidgetToken);
    TArray<FString>& PathList = AllowedPathsList.FindOrAdd(WidgetToken);

    PathSet.Reset();
    PathList.Reset();

    // De-dup helper: keeps the two parallel containers consistent.
    auto AddPath = [&PathSet, &PathList](const FString& Path)
    {
        bool bAlreadyInSet = false;
        PathSet.Add(Path, &bAlreadyInSet);
        if (!bAlreadyInSet)
        {
            PathList.Add(Path);
        }
    };

    // Common UWidget base-class property paths — allowlisted for EVERY widget
    // token, registered or not. They live on UWidget itself, so any subclass
    // carries them; the per-type registry below only maps the type-specific
    // surface. Injected BEFORE the unregistered-token early-return so tokens
    // with no FUITypeRegistryEntry still accept the base props.
    static const TCHAR* const CommonWidgetPaths[] = {
        TEXT("Visibility"),
        TEXT("RenderOpacity"),
        TEXT("ToolTipText"),
        TEXT("bIsEnabled"),
        TEXT("RenderTransform.Angle"),
        TEXT("RenderTransform.Scale"),
        TEXT("RenderTransform.Translation"),
    };
    for (const TCHAR* CommonPath : CommonWidgetPaths)
    {
        AddPath(CommonPath);
    }

    const FUITypeRegistryEntry* Entry = Registry.FindByToken(WidgetToken);
    if (!Entry)
    {
        // Base props are cached above; no type-specific surface to add. The
        // populated set short-circuits repeated calls.
        return;
    }

    PathList.Reserve(PathList.Num() + Entry->PropertyMappings.Num());
    for (const FUIPropertyMapping& Mapping : Entry->PropertyMappings)
    {
        AddPath(Mapping.JsonPath);
    }
}
