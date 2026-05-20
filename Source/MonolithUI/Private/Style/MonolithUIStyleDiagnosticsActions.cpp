// Copyright tumourlove. All Rights Reserved.
#include "Style/MonolithUIStyleDiagnosticsActions.h"

#if WITH_COMMONUI

#include "MonolithParamSchema.h"
#include "Style/MonolithUIStyleService.h"

namespace MonolithUI
{
	void FStyleDiagnosticsActions::Register(FMonolithToolRegistry& Registry)
	{
		Registry.RegisterAction(
			TEXT("ui"), TEXT("dump_style_cache_stats"),
			TEXT("Return live FMonolithUIStyleService cache stats: cache_size, hits, misses, evictions, "
				"and per-type counts (Button/Text/Border). Diagnostic for the Phase G dedup work."),
			FMonolithActionHandler::CreateStatic(&FStyleDiagnosticsActions::HandleDumpStyleCacheStats),
			FParamSchemaBuilder().Build(),
			TEXT("Diagnostics"));
	}

	FMonolithActionResult FStyleDiagnosticsActions::HandleDumpStyleCacheStats(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		const FUIStyleCacheStats Stats = FMonolithUIStyleService::Get().GetStats();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("cache_size"), Stats.CacheSize);
		Result->SetNumberField(TEXT("hits"), static_cast<double>(Stats.Hits));
		Result->SetNumberField(TEXT("misses"), static_cast<double>(Stats.Misses));
		Result->SetNumberField(TEXT("evictions"), static_cast<double>(Stats.Evictions));

		TSharedPtr<FJsonObject> ByType = MakeShared<FJsonObject>();
		ByType->SetNumberField(TEXT("Button"), Stats.ButtonCount);
		ByType->SetNumberField(TEXT("Text"), Stats.TextCount);
		ByType->SetNumberField(TEXT("Border"), Stats.BorderCount);
		Result->SetObjectField(TEXT("by_type"), ByType);

		return FMonolithActionResult::Success(Result);
	}
}

#endif // WITH_COMMONUI
