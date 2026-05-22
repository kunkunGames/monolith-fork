#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithSourceControlActions
{
public:
	static void RegisterActions();

private:
	static FMonolithActionResult HandleGetCapabilities(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCheckout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAdd(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCheckoutOrAdd(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDelete(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleMarkForDelete(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRevert(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRevertUnchanged(const TSharedPtr<FJsonObject>& Params);
};
