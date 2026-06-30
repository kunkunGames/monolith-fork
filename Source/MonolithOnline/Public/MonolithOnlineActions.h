#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithOnlineActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateEOSOSSv2Config(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeCommonSessionFlow(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateCommonSessionSchema(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateUserFacingSession(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateCommonUserInitializationContract(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateCommonUserPrivilegeMatrix(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DiagnoseEOSAccountPortalLogs(const TSharedPtr<FJsonObject>& Params);
};
