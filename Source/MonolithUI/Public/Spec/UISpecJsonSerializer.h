// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FUISpecDocument;

/**
 * Canonical JSON boundary for UISpec documents.
 *
 * `ui.dump_ui_spec` and native identity/proof consumers must use this class so
 * they cannot drift into independent widget-tree projections. `DocumentToJson`
 * is the exact JSON shape emitted by the action. `TryWriteCanonicalJson` only
 * normalizes the JSON encoding (recursively sorted object keys, compact engine
 * number/string encoding); array order and values are preserved exactly.
 */
class MONOLITHUI_API FUISpecJsonSerializer
{
public:
    /** Project one typed UISpec document into the exact `dump_ui_spec.spec` object. */
    static TSharedPtr<FJsonObject> DocumentToJson(const FUISpecDocument& Document);

    /**
     * Serialize an arbitrary JSON object deterministically for hashing.
     * Returns false, leaves OutCanonicalJson empty, and supplies OutError when
     * the input is invalid or contains an unsupported/non-finite value.
     */
    static bool TryWriteCanonicalJson(
        const TSharedPtr<FJsonObject>& JsonObject,
        FString& OutCanonicalJson,
        FString& OutError);

    /** Compose DocumentToJson + TryWriteCanonicalJson without a second tree serializer. */
    static bool TryWriteCanonicalDocument(
        const FUISpecDocument& Document,
        FString& OutCanonicalJson,
        FString& OutError);
};
