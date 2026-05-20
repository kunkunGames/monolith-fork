#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * JSON-driven room templates for rapid blockout.
 */
class MONOLITHWORLDGEN_API FMonolithMeshTemplateActions
{
public:
	/** Register room template actions with the tool registry. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// --- Template actions ---
	static FMonolithActionResult ListRoomTemplates(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetRoomTemplate(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ApplyRoomTemplate(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateRoomTemplate(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---

	/** Get the templates directory path (creates if missing) */
	static FString GetTemplatesDirectory();

	/** Load a template JSON file by name, returns nullptr and sets OutError on failure */
	static TSharedPtr<FJsonObject> LoadTemplate(const FString& TemplateName, FString& OutError);

	/** Save a JSON object as a template file */
	static bool SaveTemplate(const FString& TemplateName, const TSharedPtr<FJsonObject>& TemplateJson, FString& OutError);

	/** Parse a 3-element JSON array to FVector */
	static bool ParseJsonArrayToVector(const TArray<TSharedPtr<FJsonValue>>& Arr, FVector& Out);
};
