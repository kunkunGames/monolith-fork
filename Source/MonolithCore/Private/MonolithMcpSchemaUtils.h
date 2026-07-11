#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace MonolithMcpSchemaUtils
{
	TSharedPtr<FJsonObject> BuildJsonSchemaProperty(const TSharedPtr<FJsonObject>& ParamDef);
	TSharedPtr<FJsonObject> BuildInputSchema(const TSharedPtr<FJsonObject>& ParamSchema);
}
