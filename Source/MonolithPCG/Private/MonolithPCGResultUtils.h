#pragma once

#include "MonolithToolRegistry.h"

namespace MonolithPCGResultUtils
{
inline TSharedPtr<FJsonObject> GetErrorDataObject(const FMonolithActionResult& Result)
{
	if (!Result.ErrorData.IsValid())
	{
		return nullptr;
	}

	return Result.ErrorData;
}

inline TSharedPtr<FJsonObject> EnsureErrorDataObject(FMonolithActionResult& Result)
{
	if (TSharedPtr<FJsonObject> Existing = GetErrorDataObject(Result))
	{
		return Existing;
	}

	TSharedPtr<FJsonObject> Created = MakeShared<FJsonObject>();
	Result.WithErrorData(Created);
	return Created;
}
} // namespace MonolithPCGResultUtils
