#pragma once

#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

namespace MonolithAsset
{
	/**
	 * Keep composed asset workflows on the shared FMonolithActionResult
	 * structured-error contract instead of duplicating field access at each
	 * call site.
	 */
	inline TSharedPtr<FJsonObject> GetErrorDataObject(const FMonolithActionResult& Result)
	{
		return Result.ErrorData;
	}
}
