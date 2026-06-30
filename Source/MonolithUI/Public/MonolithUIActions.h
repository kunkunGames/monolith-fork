// MonolithUIActions.h
#pragma once

#include "MonolithToolRegistry.h"

class FMonolithUIActions
{
public:
    static void RegisterActions(FMonolithToolRegistry& Registry);

    static FMonolithActionResult HandleCreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleGetWidgetTree(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleAddWidget(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleAddExtensionPointWidget(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleAddPrimaryGameLayoutLayer(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleGetCommonFrameworkStatus(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleDescribeCommonWidgetBlueprint(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleDescribeCommonMessagingFlow(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleValidateCommonDialogContract(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleValidateCommonLayerPushContract(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleValidateFrontendMenuFlow(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleRemoveWidget(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleSetWidgetProperty(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleCompileWidget(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleListWidgetTypes(const TSharedPtr<FJsonObject>& Params);
};
