// Copyright Monolith. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Shared parameter parsing utilities for Monolith action handlers.
 * Extracted from MonolithMeshUtils to avoid duplication across modules.
 */
namespace MonolithParamUtils
{
	/** Read a required, non-empty string param. OutError is suitable for ErrInvalidParams. */
	MONOLITHCORE_API bool GetRequiredStringParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, FString& OutValue, FString& OutError, bool bTrim = true);

	/** Read an optional string param. Wrong-type values fail; missing values return the provided default. */
	MONOLITHCORE_API bool GetOptionalStringParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, FString& OutValue, FString& OutError, const FString& DefaultValue = FString(), bool bTrim = true);

	/** Read an optional integral param and clamp it to [MinValue, MaxValue]. Non-integral numbers fail. */
	MONOLITHCORE_API bool GetOptionalClampedIntParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, int32& OutValue, FString& OutError, int32 DefaultValue, int32 MinValue, int32 MaxValue);

	/** Read an optional numeric param and clamp it to [MinValue, MaxValue]. */
	MONOLITHCORE_API bool GetOptionalClampedDoubleParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, double& OutValue, FString& OutError, double DefaultValue, double MinValue, double MaxValue);

	/** Read an optional bool param. Wrong-type values fail; missing values return the provided default. */
	MONOLITHCORE_API bool GetOptionalBoolParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, bool& OutValue, FString& OutError, bool DefaultValue = false);

	/** Read an optional string-array param. Wrong-type array elements fail; missing values return an empty/default array. */
	MONOLITHCORE_API bool GetOptionalStringArrayParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, TArray<FString>& OutValues, FString& OutError, const TArray<FString>& DefaultValues = TArray<FString>());

	/** Strict integer parse for action DSLs; rejects empty or malformed text instead of silently producing zero. */
	MONOLITHCORE_API bool TryParseStrictInt(const FString& Text, int32& OutValue, FString& OutError, const FString& Context = TEXT("value"));

	/** Parse a vector from JSON params. Accepts [x,y,z] array or {"x":..,"y":..,"z":..} object. Returns false if key not found. */
	MONOLITHCORE_API bool ParseVector(const TSharedPtr<FJsonObject>& Params, const FString& Key, FVector& Out);

	/** Parse a rotator from JSON params. Accepts [pitch,yaw,roll] array or {"pitch":..,"yaw":..,"roll":..} object. Returns false if key not found. */
	MONOLITHCORE_API bool ParseRotator(const TSharedPtr<FJsonObject>& Params, const FString& Key, FRotator& Out);

	/** Get the current editor world from GEditor. Returns nullptr if unavailable. */
	MONOLITHCORE_API UWorld* GetEditorWorld();

	/** Convert FVector to a JSON array [x, y, z]. */
	MONOLITHCORE_API TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V);

	/**
	 * Normalize a Blueprint asset path to a class path suitable for StaticLoadClass.
	 * "/Game/Foo/BP_Bar" -> "/Game/Foo/BP_Bar.BP_Bar_C"
	 * Paths already containing "." get "_C" appended if missing.
	 */
	MONOLITHCORE_API FString NormalizeBlueprintClassPath(const FString& BlueprintPath);

	/** Parse a mobility string ("static", "stationary", "movable") into EComponentMobility. Returns false if unrecognized. */
	MONOLITHCORE_API bool ParseMobility(const FString& MobilityStr, EComponentMobility::Type& OutMobility);
}
