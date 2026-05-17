#include "MonolithHttpServer.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithCoreModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithMcpSessionTracker.h"
#include "MonolithResourceRegistry.h"
#include "MonolithToolRegistry.h"
#include "MonolithToolResultUtils.h"
#include "MonolithSettings.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "Internationalization/Regex.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

namespace
{
	constexpr int32 MaxMcpRequestBodyBytes = 16 * 1024 * 1024;

	FString FirstHeaderValue(const FHttpServerRequest& Request, const TCHAR* HeaderName)
	{
		for (const TPair<FString, TArray<FString>>& Pair : Request.Headers)
		{
			if (Pair.Key.Equals(HeaderName, ESearchCase::IgnoreCase) && Pair.Value.Num() > 0)
			{
				return Pair.Value[0];
			}
		}
		return FString();
	}

	TSharedPtr<FJsonObject> GetJsonRpcParams(const TSharedPtr<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		if (Request.IsValid() && Request->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj)
		{
			return *ParamsObj;
		}
		return nullptr;
	}

	FString GetJsonRpcMethod(const TSharedPtr<FJsonObject>& Request)
	{
		FString Method;
		if (Request.IsValid())
		{
			Request->TryGetStringField(TEXT("method"), Method);
		}
		return Method;
	}

	FString GetJsonRpcToolName(const TSharedPtr<FJsonObject>& Request, const FString& Method)
	{
		if (Method != TEXT("tools/call"))
		{
			return FString();
		}

		FString ToolName;
		const TSharedPtr<FJsonObject> Params = GetJsonRpcParams(Request);
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("name"), ToolName);
		}
		return ToolName;
	}

	FString GetJsonRpcProtocolVersion(const TSharedPtr<FJsonObject>& Request, const FString& HeaderProtocolVersion)
	{
		if (!HeaderProtocolVersion.IsEmpty())
		{
			return HeaderProtocolVersion;
		}

		const TSharedPtr<FJsonObject> Params = GetJsonRpcParams(Request);
		FString ProtocolVersion;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("protocolVersion"), ProtocolVersion);
		}
		return ProtocolVersion;
	}

	void ObserveMcpSessionIfEnabled(
		const TSharedPtr<FJsonObject>& Request,
		const FString& HeaderSessionId,
		const FString& HeaderProtocolVersion)
	{
		const UMonolithSettings* Settings = UMonolithSettings::Get();
		if (!Settings || !Settings->bEnableMcpSessionMode || !Request.IsValid())
		{
			return;
		}

		const FString Method = GetJsonRpcMethod(Request);
		FMonolithMcpSessionTracker::Get().ObserveRequest(
			HeaderSessionId,
			GetJsonRpcProtocolVersion(Request, HeaderProtocolVersion),
			Method,
			GetJsonRpcToolName(Request, Method));
	}
}

FMonolithHttpServer::FMonolithHttpServer()
{
}

FMonolithHttpServer::~FMonolithHttpServer()
{
	Stop();
}

bool FMonolithHttpServer::Start(int32 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogMonolith, Warning, TEXT("HTTP server already running on port %d"), BoundPort);
		return true;
	}

	// On a fresh editor launch, the OS keeps the port in TIME_WAIT for up to
	// 2*MSL (~30s on macOS/Linux) after the previous editor shut down. UE's
	// HttpServerModule also caches a broken listener internally and won't
	// rebind until StopAllListeners() is called. Budget ~40s total so a
	// rapid close+reopen cycle doesn't drop the MCP server on the floor.
	constexpr int32 MaxAttempts = 20;
	constexpr float BackoffSeconds = 2.0f;

	for (int32 Attempt = 1; Attempt <= MaxAttempts; ++Attempt)
	{
		if (Attempt > 1)
		{
			UE_LOG(LogMonolith, Warning, TEXT("HTTP bind attempt %d/%d on port %d — waiting %.1fs"),
				Attempt, MaxAttempts, Port, BackoffSeconds);
			FPlatformProcess::Sleep(BackoffSeconds);

			// Drop our router handle + routes so GetHttpRouter can evict failed listener.
			if (HttpRouter.IsValid())
			{
				for (const FHttpRouteHandle& Handle : RouteHandles)
				{
					HttpRouter->UnbindRoute(Handle);
				}
			}
			RouteHandles.Empty();
			HttpRouter.Reset();

			// Full module reset — the HttpServerModule caches a failed listener
			// and refuses to re-bind the same port until we explicitly stop it.
			FHttpServerModule::Get().StopAllListeners();
		}

		HttpRouter = FHttpServerModule::Get().GetHttpRouter(Port, true);
		if (!HttpRouter.IsValid())
		{
			UE_LOG(LogMonolith, Warning, TEXT("GetHttpRouter failed on port %d (attempt %d)"), Port, Attempt);
			continue;
		}

		BindRoutes();
		FHttpServerModule::Get().StartAllListeners();

		// Brief wait for OS to complete bind before probing
		FPlatformProcess::Sleep(0.1f);

		if (ProbePort(Port))
		{
			bIsRunning = true;
			BoundPort = Port;
			StartTime = FDateTime::UtcNow();
			UE_LOG(LogMonolith, Log, TEXT("Monolith MCP server listening on port %d (attempt %d)"), Port, Attempt);
			return true;
		}

		UE_LOG(LogMonolith, Warning, TEXT("Port %d not listening after StartAllListeners (attempt %d)"), Port, Attempt);
	}

	UE_LOG(LogMonolith, Error, TEXT("Failed to bind Monolith MCP server on port %d after %d attempts (~%ds total)"),
		Port, MaxAttempts, static_cast<int32>(MaxAttempts * BackoffSeconds));
	// Clean up
	if (HttpRouter.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
	}
	RouteHandles.Empty();
	HttpRouter.Reset();
	return false;
}

void FMonolithHttpServer::Stop()
{
	if (!bIsRunning)
	{
		return;
	}

	if (HttpRouter.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
		RouteHandles.Empty();
	}

	FHttpServerModule::Get().StopAllListeners();
	HttpRouter.Reset();

	bIsRunning = false;
	UE_LOG(LogMonolith, Log, TEXT("Monolith MCP server stopped"));
}

void FMonolithHttpServer::BindRoutes()
{
	if (!HttpRouter.IsValid()) return;

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandlePostMcp)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleGetMcp)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleDeleteMcp)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleOptions)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/health")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleHealthCheck)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/health")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleOptions)));
}

bool FMonolithHttpServer::ProbePort(int32 Port)
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem) return false;

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bValid = false;
	Addr->SetIp(TEXT("127.0.0.1"), bValid);
	if (!bValid) return false;
	Addr->SetPort(Port);

	FSocket* Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MonolithProbe"), false);
	if (!Socket) return false;

	Socket->SetNonBlocking(false);
	const bool bConnected = Socket->Connect(*Addr);
	Socket->Close();
	SocketSubsystem->DestroySocket(Socket);
	return bConnected;
}

bool FMonolithHttpServer::Restart(int32 Port)
{
	// Unbind our routes
	if (HttpRouter.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
	}
	RouteHandles.Empty();
	HttpRouter.Reset();

	// Full stop — safe here because we own the listener
	FHttpServerModule::Get().StopAllListeners();

	bIsRunning = false;
	BoundPort = 0;
	return Start(Port);
}

// ============================================================================
// Route Handlers
// ============================================================================

bool FMonolithHttpServer::HandlePostMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (Request.Body.Num() > MaxMcpRequestBodyBytes)
	{
		TSharedPtr<FJsonObject> Err = FMonolithJsonUtils::ErrorResponse(
			nullptr,
			FMonolithJsonUtils::ErrInvalidRequest,
			FString::Printf(TEXT("Request body exceeds Monolith MCP limit of %d bytes"), MaxMcpRequestBodyBytes));
		auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Err), EHttpServerResponseCodes::RequestTooLarge);
		AddCorsHeaders(*Response, Request);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Parse body as UTF-8 JSON (Body is NOT null-terminated — must add terminator)
	TArray<uint8> NullTermBody(Request.Body);
	NullTermBody.Add(0);
	FString BodyString = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(NullTermBody.GetData())));
	if (BodyString.IsEmpty())
	{
		TSharedPtr<FJsonObject> Err = FMonolithJsonUtils::ErrorResponse(
			nullptr, FMonolithJsonUtils::ErrParseError, TEXT("Empty request body"));
		auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Err), EHttpServerResponseCodes::BadRequest);
		AddCorsHeaders(*Response, Request);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Try parse as JSON
	TSharedPtr<FJsonObject> JsonRequest = FMonolithJsonUtils::Parse(BodyString);

	// Could be a single request or a batch (array)
	TArray<TSharedPtr<FJsonObject>> Requests;
	TArray<TSharedPtr<FJsonObject>> Responses;

	if (JsonRequest.IsValid())
	{
		// Single request
		Requests.Add(JsonRequest);
	}
	else
	{
		// Try parsing as array (batch)
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyString);
		if (FJsonSerializer::Deserialize(Reader, JsonArray) && JsonArray.Num() > 0)
		{
			Requests.Reserve(JsonArray.Num());
			for (const TSharedPtr<FJsonValue>& Value : JsonArray)
			{
				if (Value.IsValid() && Value->Type == EJson::Object)
				{
					Requests.Add(Value->AsObject());
				}
			}
		}
		else
		{
			TSharedPtr<FJsonObject> Err = FMonolithJsonUtils::ErrorResponse(
				nullptr, FMonolithJsonUtils::ErrParseError, TEXT("Invalid JSON"));
			auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Err), EHttpServerResponseCodes::BadRequest);
			AddCorsHeaders(*Response, Request);
			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	// Process each request
	const FString HeaderSessionId = FirstHeaderValue(Request, TEXT("MCP-Session-Id"));
	const FString HeaderProtocolVersion = FirstHeaderValue(Request, TEXT("MCP-Protocol-Version"));
	Responses.Reserve(Requests.Num());
	for (const TSharedPtr<FJsonObject>& Req : Requests)
	{
		ObserveMcpSessionIfEnabled(Req, HeaderSessionId, HeaderProtocolVersion);
		TSharedPtr<FJsonObject> Resp = ProcessJsonRpcRequest(Req);
		if (Resp.IsValid())
		{
			// Only add response if it's not a notification (notifications have no id)
			Responses.Add(Resp);
		}
	}

	// Build response
	FString ResponseBody;
	if (Responses.Num() == 0)
	{
		// All notifications — 202 Accepted with no body
		auto Response = FHttpServerResponse::Ok();
		Response->Code = EHttpServerResponseCodes::Accepted;
		AddCorsHeaders(*Response, Request);
		OnComplete(MoveTemp(Response));
		return true;
	}
	else if (Responses.Num() == 1)
	{
		ResponseBody = FMonolithJsonUtils::Serialize(Responses[0]);
	}
	else
	{
		// Batch response — serialize as array
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Responses.Num());
		for (const TSharedPtr<FJsonObject>& Resp : Responses)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(Resp));
		}
		FString ArrayStr;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ArrayStr);
		FJsonSerializer::Serialize(JsonArray, Writer);
		ResponseBody = ArrayStr;
	}

	auto Response = MakeJsonResponse(ResponseBody);
	AddCorsHeaders(*Response, Request);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleGetMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// SSE endpoint — return a single SSE event with an endpoint message.
	// UE's HTTP server doesn't natively support long-lived SSE connections,
	// so we return a single SSE event and close.
	FString SseBody = TEXT("event: endpoint\ndata: \"/mcp\"\n\n");
	auto Response = FHttpServerResponse::Create(SseBody, TEXT("text/event-stream"));
	Response->Code = EHttpServerResponseCodes::Ok;
	AddCorsHeaders(*Response, Request);
	Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-cache")});
	Response->Headers.Add(TEXT("Connection"), {TEXT("keep-alive")});
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleDeleteMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	auto Response = FHttpServerResponse::Ok();
	AddCorsHeaders(*Response, Request);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	auto Response = FHttpServerResponse::Ok();
	AddCorsHeaders(*Response, Request);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleHealthCheck(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Health = MakeShared<FJsonObject>();
	Health->SetStringField(TEXT("status"), TEXT("ok"));
	Health->SetNumberField(TEXT("port"), BoundPort);
	Health->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Health->SetStringField(TEXT("version"), MONOLITH_VERSION);

	const FTimespan Uptime = FDateTime::UtcNow() - StartTime;
	Health->SetNumberField(TEXT("uptime_seconds"), static_cast<double>(Uptime.GetTotalSeconds()));

	Health->SetNumberField(TEXT("tools_registered"), FMonolithToolRegistry::Get().GetActionCount());

	TSharedPtr<FJsonObject> Transport = MakeShared<FJsonObject>();
	Transport->SetStringField(TEXT("primary_route"), TEXT("/mcp"));
	Transport->SetBoolField(TEXT("streamable_http_enabled"), true);
	Transport->SetBoolField(TEXT("mcp_get_sse_endpoint_enabled"), true);
	Transport->SetBoolField(TEXT("legacy_sse_route_enabled"), false);
	Transport->SetBoolField(TEXT("legacy_message_route_enabled"), false);
	Transport->SetStringField(TEXT("cors_mode"), TEXT("loopback_origin_allowlist"));
	Transport->SetNumberField(TEXT("max_request_body_bytes"), MaxMcpRequestBodyBytes);

	TArray<TSharedPtr<FJsonValue>> Protocols;
	Protocols.Add(MakeShared<FJsonValueString>(TEXT("2024-11-05")));
	Protocols.Add(MakeShared<FJsonValueString>(TEXT("2025-03-26")));
	Transport->SetArrayField(TEXT("supported_protocol_versions"), Protocols);
	Health->SetObjectField(TEXT("mcp_transport"), Transport);

	FString Body;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	FJsonSerializer::Serialize(Health.ToSharedRef(), Writer);

	auto Response = MakeJsonResponse(Body);
	AddCorsHeaders(*Response, Request);
	OnComplete(MoveTemp(Response));
	return true;
}

// ============================================================================
// JSON-RPC 2.0 Processing
// ============================================================================

TSharedPtr<FJsonObject> FMonolithHttpServer::ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request)
{
	if (!Request.IsValid())
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Invalid request object"));
	}

	// Validate jsonrpc version
	FString Version;
	if (!Request->TryGetStringField(TEXT("jsonrpc"), Version) || Version != TEXT("2.0"))
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Missing or invalid jsonrpc version"));
	}

	// Get method
	FString Method;
	if (!Request->TryGetStringField(TEXT("method"), Method))
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Missing method field"));
	}

	// Get id (null for notifications)
	TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
	bool bIsNotification = !Id.IsValid() || Id->IsNull();

	// Get params
	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (Request->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj)
	{
		Params = *ParamsObj;
	}
	if (!Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	UE_LOG(LogMonolith, Verbose, TEXT("JSON-RPC: %s (id=%s)"), *Method, Id.IsValid() ? *Id->AsString() : TEXT("notification"));

	// Dispatch by method
	TSharedPtr<FJsonObject> Response;
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const bool bResourcesEnabled = Settings
		&& Settings->bEnableMcpResources
		&& FMonolithResourceRegistry::Get().HasDefaultResourcesRegistered();

	if (Method == TEXT("initialize"))
	{
		Response = HandleInitialize(Id, Params);
	}
	else if (Method == TEXT("notifications/initialized"))
	{
		// Notification — no response
		return nullptr;
	}
	else if (Method == TEXT("tools/list"))
	{
		Response = HandleToolsList(Id, Params);
	}
	else if (Method == TEXT("tools/call"))
	{
		Response = HandleToolsCall(Id, Params);
	}
	else if (bResourcesEnabled && Method == TEXT("resources/list"))
	{
		Response = HandleResourcesList(Id, Params);
	}
	else if (bResourcesEnabled && Method == TEXT("resources/read"))
	{
		Response = HandleResourcesRead(Id, Params);
	}
	else if (Method == TEXT("ping"))
	{
		Response = HandlePing(Id);
	}
	else
	{
		Response = FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrMethodNotFound,
			FString::Printf(TEXT("Unknown method: %s"), *Method));
	}

	// Notifications don't get responses
	if (bIsNotification)
	{
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleInitialize(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Protocol version negotiation: echo the client's requested version if we
	// support it, otherwise fall back to the latest we support.
	FString ClientVersion;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("protocolVersion"), ClientVersion)
		&& (ClientVersion == TEXT("2024-11-05") || ClientVersion == TEXT("2025-03-26")))
	{
		Result->SetStringField(TEXT("protocolVersion"), ClientVersion);
	}
	else
	{
		Result->SetStringField(TEXT("protocolVersion"), TEXT("2025-03-26"));
	}

	// Server info
	TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), TEXT("monolith"));
	ServerInfo->SetStringField(TEXT("version"), MONOLITH_VERSION);
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);

	// Capabilities
	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();

	// We support tools
	TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
	ToolsCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("tools"), ToolsCap);

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings
		&& Settings->bEnableMcpResources
		&& FMonolithResourceRegistry::Get().HasDefaultResourcesRegistered())
	{
		TSharedPtr<FJsonObject> ResourcesCap = MakeShared<FJsonObject>();
		ResourcesCap->SetBoolField(TEXT("listChanged"), false);
		Capabilities->SetObjectField(TEXT("resources"), ResourcesCap);
	}

	Result->SetObjectField(TEXT("capabilities"), Capabilities);

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleToolsList(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	// Each namespace becomes a tool
	TArray<FString> Namespaces = Registry.GetNamespaces();
	TArray<FMonolithActionInfo> CoreActions = Registry.GetActions(TEXT("monolith"));
	ToolsArray.Reserve(Namespaces.Num() + CoreActions.Num());
	for (const FString& Namespace : Namespaces)
	{
		if (Namespace == TEXT("monolith"))
		{
			if (CoreActions.Num() == 0) continue;

			// Core tools are individual: monolith_discover, monolith_status
			for (const FMonolithActionInfo& ActionInfo : CoreActions)
			{
				TSharedPtr<FJsonObject> CoreTool = MakeShared<FJsonObject>();

				FString ToolName;
				ToolName.Reserve(9 + ActionInfo.Action.Len()); // "monolith_" = 9 chars
				ToolName += TEXT("monolith_");
				ToolName += ActionInfo.Action;
				CoreTool->SetStringField(TEXT("name"), ToolName);
				CoreTool->SetStringField(TEXT("description"), ActionInfo.Description);

				// Input schema
				TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
				InputSchema->SetStringField(TEXT("type"), TEXT("object"));
				if (ActionInfo.ParamSchema.IsValid())
				{
					InputSchema->SetObjectField(TEXT("properties"), ActionInfo.ParamSchema);
				}
				else
				{
					InputSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
				}
				CoreTool->SetObjectField(TEXT("inputSchema"), InputSchema);

				ToolsArray.Add(MakeShared<FJsonValueObject>(CoreTool));
			}
		}
		else
		{
			TArray<FString> ActionNames = Registry.GetActionNames(Namespace);
			if (ActionNames.Num() == 0) continue;

			// Build the tool entry for this namespace
			// Format: "namespace_query" with action as a parameter
			TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();

			// Domain tools use the dispatch pattern: namespace_query (underscore, not dot)
			// Dots in tool names break Claude Code's mcp__server__tool mapping.
			FString ToolName = Namespace;
			ToolName += TEXT("_query");
			Tool->SetStringField(TEXT("name"), ToolName);

			// Build description with action list
			int32 TotalActionLen = 0;
			for (const FString& Name : ActionNames)
			{
				TotalActionLen += Name.Len();
			}
			if (ActionNames.Num() > 0)
			{
				TotalActionLen += (ActionNames.Num() - 1) * 2; // ", "
			}
			FString Description;
			Description.Reserve(38 + Namespace.Len() + TotalActionLen); // "Query the  domain. Available actions: " = 38 chars
			Description += TEXT("Query the ");
			Description += Namespace;
			Description += TEXT(" domain. Available actions: ");
			for (int32 i = 0; i < ActionNames.Num(); ++i)
			{
				if (i > 0)
				{
					Description += TEXT(", ");
				}
				Description += ActionNames[i];
			}
			Tool->SetStringField(TEXT("description"), Description);

			// Build input schema
			TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));

			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

			// "action" property (required)
			TSharedPtr<FJsonObject> ActionProp = MakeShared<FJsonObject>();
			ActionProp->SetStringField(TEXT("type"), TEXT("string"));
			ActionProp->SetStringField(TEXT("description"), TEXT("The action to execute"));
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			EnumValues.Reserve(ActionNames.Num());
			for (const FString& Name : ActionNames)
			{
				EnumValues.Add(MakeShared<FJsonValueString>(Name));
			}
			ActionProp->SetArrayField(TEXT("enum"), EnumValues);
			Properties->SetObjectField(TEXT("action"), ActionProp);

			// "params" property — keep lightweight; full per-action schemas available via monolith_discover
			TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
			ParamsProp->SetStringField(TEXT("type"), TEXT("object"));

			FString ParamsDesc;
			ParamsDesc.Reserve(81 + Namespace.Len());
			ParamsDesc += TEXT("Parameters for the action. Call monolith_discover(\"");
			ParamsDesc += Namespace;
			ParamsDesc += TEXT("\") for full parameter schemas.");
			ParamsProp->SetStringField(TEXT("description"), ParamsDesc);
			Properties->SetObjectField(TEXT("params"), ParamsProp);

			InputSchema->SetObjectField(TEXT("properties"), Properties);
			InputSchema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("action"))});

			Tool->SetObjectField(TEXT("inputSchema"), InputSchema);
			ToolsArray.Add(MakeShared<FJsonValueObject>(Tool));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("tools"), ToolsArray);

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleResourcesList(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	double LimitValue = 100.0;
	FString Cursor;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("limit"), LimitValue);
		Params->TryGetStringField(TEXT("cursor"), Cursor);
	}

	if (!FMath::IsFinite(LimitValue) ||
		LimitValue < static_cast<double>(TNumericLimits<int32>::Min()) ||
		LimitValue > static_cast<double>(TNumericLimits<int32>::Max()))
	{
		return FMonolithJsonUtils::ErrorResponse(
			Id,
			FMonolithJsonUtils::ErrInvalidParams,
			TEXT("Invalid limit: must be a finite number within int32 range"));
	}

	TSharedPtr<FJsonObject> Result = FMonolithResourceRegistry::Get().ListResourcesJson(
		static_cast<int32>(LimitValue),
		Cursor);
	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleResourcesRead(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString Uri;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("uri"), Uri) || Uri.IsEmpty())
	{
		return FMonolithJsonUtils::ErrorResponse(
			Id,
			FMonolithJsonUtils::ErrInvalidParams,
			TEXT("Missing resource uri"));
	}

	FMonolithResourceReadResult Read = FMonolithResourceRegistry::Get().ReadResource(Uri);
	if (!Read.bFound)
	{
		return FMonolithJsonUtils::ErrorResponse(
			Id,
			FMonolithJsonUtils::ErrResourceNotFound,
			Read.Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Contents;
	TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
	Content->SetStringField(TEXT("uri"), Read.Uri);
	Content->SetStringField(TEXT("mimeType"), Read.MimeType);
	Content->SetStringField(TEXT("text"), Read.Text);
	Content->SetBoolField(TEXT("truncated"), Read.bTruncated);
	Contents.Add(MakeShared<FJsonValueObject>(Content));
	Result->SetArrayField(TEXT("contents"), Contents);
	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleToolsCall(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			TEXT(""),
			TEXT(""),
			TEXT(""),
			TEXT("malformed_dispatch"),
			FMonolithJsonUtils::ErrInvalidParams,
			TEXT("Missing params"));
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing params"));
	}

	FString ToolName;
	if (!Params->TryGetStringField(TEXT("name"), ToolName))
	{
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			TEXT(""),
			TEXT(""),
			TEXT(""),
			TEXT("malformed_dispatch"),
			FMonolithJsonUtils::ErrInvalidParams,
			TEXT("Missing tool name"));
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing tool name"));
	}

	// Get arguments
	TSharedPtr<FJsonObject> Arguments;
	const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj)
	{
		Arguments = *ArgsObj;
	}
	if (!Arguments.IsValid())
	{
		Arguments = MakeShared<FJsonObject>();
	}

	FString Namespace;
	FString Action;

	// Determine dispatch pattern
	if (ToolName.StartsWith(TEXT("monolith_")))
	{
		// Core tool: monolith_discover -> namespace="monolith", action="discover"
		Namespace = TEXT("monolith");
		Action = ToolName.Mid(9);
	}
	else if (ToolName.EndsWith(TEXT("_query")) || ToolName.EndsWith(TEXT(".query")))
	{
		// Domain tool: blueprint_query (or legacy blueprint.query) -> namespace="blueprint"
		Namespace = ToolName.Left(ToolName.Len() - 6); // strip "_query" or ".query"

		if (!Arguments->TryGetStringField(TEXT("action"), Action))
		{
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				ToolName,
				Namespace,
				TEXT(""),
				TEXT("malformed_dispatch"),
				FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Missing 'action' in arguments"));
			return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Missing 'action' in arguments"));
		}

		// Collect top-level fields (excluding reserved keys) — MCP clients may
		// place optional params like members_only alongside "action" rather than
		// nesting them inside "params".
		TSharedPtr<FJsonObject> TopLevelExtras = MakeShared<FJsonObject>();
		for (const auto& Pair : Arguments->Values)
		{
			if (Pair.Key != TEXT("action") && Pair.Key != TEXT("params"))
			{
				TopLevelExtras->SetField(Pair.Key, Pair.Value);
			}
		}

		// Extract nested params if present, then merge in any top-level extras
		// NOTE: Claude Code sends "params" as a JSON-encoded string, not a nested object.
		// We must handle both cases.
		const TSharedPtr<FJsonObject>* NestedParams = nullptr;
		TSharedPtr<FJsonObject> ParsedParamsObj; // lifetime holder for string-parsed params
		bool bHasNestedParams = false;

		if (Arguments->TryGetObjectField(TEXT("params"), NestedParams) && NestedParams)
		{
			bHasNestedParams = true;
		}
		else
		{
			// Try parsing "params" as a JSON string (Claude Code serializes objects to strings)
			FString ParamsStr;
			if (Arguments->TryGetStringField(TEXT("params"), ParamsStr) && !ParamsStr.IsEmpty())
			{
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsStr);
				if (FJsonSerializer::Deserialize(Reader, ParsedParamsObj) && ParsedParamsObj.IsValid())
				{
					NestedParams = &ParsedParamsObj;
					bHasNestedParams = true;
				}
			}
		}

		if (bHasNestedParams && NestedParams)
		{
			Arguments = MakeShared<FJsonObject>();
			// Start with top-level extras (lower priority)
			for (const auto& Pair : TopLevelExtras->Values)
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
			// Overlay nested params (higher priority)
			for (const auto& Pair : (*NestedParams)->Values)
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
		}
		else
		{
			// No nested "params" — use top-level fields as params directly
			Arguments = TopLevelExtras;
		}
	}
	else
	{
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			ToolName,
			TEXT(""),
			TEXT(""),
			TEXT("malformed_dispatch"),
			FMonolithJsonUtils::ErrMethodNotFound,
			FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrMethodNotFound,
			FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
	}

	// Execute via registry
	FMonolithActionResult ActionResult = FMonolithToolRegistry::Get().ExecuteAction(Namespace, Action, Arguments);

	// Build MCP tool result
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(
		ActionResult,
		Settings && Settings->bEnableStructuredToolResults);

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandlePing(const TSharedPtr<FJsonValue>& Id)
{
	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
}

// ============================================================================
// Helpers
// ============================================================================

TUniquePtr<FHttpServerResponse> FMonolithHttpServer::MakeJsonResponse(const FString& JsonBody, EHttpServerResponseCodes Code)
{
	auto Response = FHttpServerResponse::Create(JsonBody, TEXT("application/json"));
	Response->Code = Code;
	return Response;
}

TUniquePtr<FHttpServerResponse> FMonolithHttpServer::MakeSseResponse(const TArray<TSharedPtr<FJsonObject>>& Messages)
{
	FString SseBody;
	for (const TSharedPtr<FJsonObject>& Msg : Messages)
	{
		SseBody += TEXT("event: message\ndata: ");
		SseBody += FMonolithJsonUtils::Serialize(Msg);
		SseBody += TEXT("\n\n");
	}

	auto Response = FHttpServerResponse::Create(SseBody, TEXT("text/event-stream"));
	Response->Code = EHttpServerResponseCodes::Ok;
	return Response;
}

namespace
{
	// Allowlisted origins for browser CORS. Loopback only — the MCP server
	// is a developer tool and should never be exposed cross-origin to the
	// public web. Replaces the previous wildcard `*` that allowed any
	// website to read project data via a tab pinging localhost (Issue #38).
	//
	// Includes IPv6 loopback `[::1]` because some browsers prefer it over
	// 127.0.0.1 when resolving `localhost`. Anchored with ^ and $ so
	// subdomain attacks like `http://localhost.evil.com` are rejected.
	bool IsAllowedOrigin(const FString& Origin)
	{
		if (Origin.IsEmpty()) return false;

		// Reject the literal string "null" (sandboxed iframes / file:// origins).
		if (Origin.Equals(TEXT("null"), ESearchCase::IgnoreCase)) return false;

		// Match: http(s)://localhost[:NNNN], http(s)://127.0.0.1[:NNNN],
		// http(s)://[::1][:NNNN]. Reject anything else.
		static const FRegexPattern Pattern(
			TEXT("^https?://(localhost|127\\.0\\.0\\.1|\\[::1\\])(:\\d+)?$"));
		FRegexMatcher Matcher(Pattern, Origin);
		return Matcher.FindNext();
	}
}

void FMonolithHttpServer::AddCorsHeaders(FHttpServerResponse& Response, const FHttpServerRequest& Request)
{
	// Always advertise the methods/headers we support — these are not
	// origin-sensitive. The allow-origin echo is the gated piece.
	Response.Headers.Add(TEXT("Access-Control-Allow-Methods"), {TEXT("GET, POST, DELETE, OPTIONS")});
	Response.Headers.Add(TEXT("Access-Control-Allow-Headers"), {TEXT("Content-Type, Accept, MCP-Session-Id, MCP-Protocol-Version")});
	Response.Headers.Add(TEXT("Vary"), {TEXT("Origin")});

	// Pull the Origin header (HTTP header names are case-insensitive per RFC 7230,
	// but the underlying TMap keys may preserve case — try both common spellings).
	FString Origin;
	if (const TArray<FString>* Hdr = Request.Headers.Find(TEXT("Origin")))
	{
		if (Hdr->Num() > 0) Origin = (*Hdr)[0];
	}
	else if (const TArray<FString>* HdrLower = Request.Headers.Find(TEXT("origin")))
	{
		if (HdrLower->Num() > 0) Origin = (*HdrLower)[0];
	}

	if (IsAllowedOrigin(Origin))
	{
		Response.Headers.Add(TEXT("Access-Control-Allow-Origin"), {Origin});
	}
	// else: omit ACAO entirely — browsers will block the response from being
	// read by the requesting page. Same-origin and non-browser callers
	// (Claude Code via the proxy) are unaffected.
}
