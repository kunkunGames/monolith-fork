#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithConsoleActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult ListLiveObjects(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RefreshSnapshot(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SearchObjects(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetObject(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Health(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ResolveCommand(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetLogCursor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SearchLogsSince(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult WaitForLog(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExecuteAndExpect(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RunSequence(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExecuteAndCapture(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PollCapture(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DiagnoseFailure(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetCvarScoped(const TSharedPtr<FJsonObject>& Params);
};
