#pragma once

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"

namespace MonolithAsset
{
	/**
	 * The fork's FMonolithActionResult stores structured error data as a
	 * FJsonValue. Asset workflow composition requires an object and must fail
	 * closed when a child returns another JSON kind.
	 */
	inline TSharedPtr<FJsonObject> GetErrorDataObject(const FMonolithActionResult& Result)
	{
		if (!Result.ErrorData.IsValid() || Result.ErrorData->Type != EJson::Object)
		{
			return nullptr;
		}
		return Result.ErrorData->AsObject();
	}
}
