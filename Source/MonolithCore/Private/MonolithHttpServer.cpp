#include "MonolithHttpServer.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithCancellationRegistry.h"
#include "MonolithCoreModule.h"
#include "MonolithExecutionContext.h"
#include "MonolithJsonUtils.h"
#include "MonolithMcpSessionTracker.h"
#include "MonolithProgressRegistry.h"
#include "MonolithResourceRegistry.h"
#include "MonolithToolRegistry.h"
#include "MonolithToolInvocationLogger.h"
#include "MonolithToolProfileManager.h"
#include "MonolithToolResultUtils.h"
#include "MonolithSettings.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "Internationalization/Regex.h"
#include "Misc/SecureHash.h"
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

	FString RedactedSessionKey(const FString& RawSessionId)
	{
		if (RawSessionId.IsEmpty())
		{
			return TEXT("stateless");
		}
		if (RawSessionId == TEXT("stateless") || RawSessionId.StartsWith(TEXT("md5:")))
		{
			return RawSessionId;
		}
		return TEXT("md5:") + FMD5::HashAnsiString(*RawSessionId);
	}

	// Returns true when the client's initialize params advertise the named
	// capability group (e.g. "roots", "sampling", "elicitation"). Only the
	// boolean presence is read; the capability object itself is never stored.
	bool ClientAdvertisesCapability(const TSharedPtr<FJsonObject>& Params, const TCHAR* CapabilityKey)
	{
		if (!Params.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Capabilities = nullptr;
		if (!Params->TryGetObjectField(TEXT("capabilities"), Capabilities) || !Capabilities || !(*Capabilities).IsValid())
		{
			return false;
		}
		return (*Capabilities)->HasField(CapabilityKey);
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
		const FString ProtocolVersion = GetJsonRpcProtocolVersion(Request, HeaderProtocolVersion);
		FMonolithMcpSessionTracker& Tracker = FMonolithMcpSessionTracker::Get();

		// Lifecycle routing (P1c): initialize / notifications/initialized advance
		// the session lifecycle additively; everything else stays a plain observe.
		if (Method == TEXT("initialize"))
		{
			const TSharedPtr<FJsonObject> Params = GetJsonRpcParams(Request);
			Tracker.MarkInitialize(
				HeaderSessionId,
				ProtocolVersion,
				ClientAdvertisesCapability(Params, TEXT("roots")),
				ClientAdvertisesCapability(Params, TEXT("sampling")),
				ClientAdvertisesCapability(Params, TEXT("elicitation")));
			return;
		}
		if (Method == TEXT("notifications/initialized"))
		{
			Tracker.MarkInitialized(HeaderSessionId);
			return;
		}

		Tracker.ObserveRequest(
			HeaderSessionId,
			ProtocolVersion,
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
			nullptr, FMonolithJsonUtils::ErrParseError, TEXT("Empty request body — send a JSON-RPC 2.0 request, e.g. {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}."));
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
				nullptr, FMonolithJsonUtils::ErrParseError, TEXT("Invalid JSON — body must be a valid JSON-RPC 2.0 request object or an array of them for batch."));
			auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Err), EHttpServerResponseCodes::BadRequest);
			AddCorsHeaders(*Response, Request);
			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	// Process each request
	const FString HeaderSessionId = FirstHeaderValue(Request, TEXT("MCP-Session-Id"));
	const FString HeaderProtocolVersion = FirstHeaderValue(Request, TEXT("MCP-Protocol-Version"));
	const FString HeaderTraceId = FirstHeaderValue(Request, TEXT("X-Monolith-Trace-Id"));

	// P1c session gate — only active when bEnableMcpSessionMode is on; a pure
	// pass-through to legacy behavior otherwise (EvaluateSessionGate returns
	// bReject=false when the flag is off, leaving the per-request loop untouched).
	{
		const UMonolithSettings* GateSettings = UMonolithSettings::Get();
		const bool bSessionModeEnabled = GateSettings && GateSettings->bEnableMcpSessionMode;
		if (bSessionModeEnabled)
		{
			TArray<FString> Methods;
			Methods.Reserve(Requests.Num());
			for (const TSharedPtr<FJsonObject>& Req : Requests)
			{
				Methods.Add(GetJsonRpcMethod(Req));
			}
			const bool bSessionKnown = !HeaderSessionId.IsEmpty()
				&& FMonolithMcpSessionTracker::Get().IsKnownSession(HeaderSessionId);

			const FSessionGateResult Gate = EvaluateSessionGate(
				Methods, HeaderSessionId, HeaderProtocolVersion, bSessionKnown, bSessionModeEnabled);
			if (Gate.bReject)
			{
				TSharedPtr<FJsonObject> GateError = FMonolithJsonUtils::ErrorResponse(
					nullptr, Gate.RpcCode, Gate.Message);
				auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(GateError), Gate.HttpCode);
				AddCorsHeaders(*Response, Request);
				OnComplete(MoveTemp(Response));
				return true;
			}
		}
	}

	Responses.Reserve(Requests.Num());
	for (const TSharedPtr<FJsonObject>& Req : Requests)
	{
		FString TraceId = HeaderTraceId;
		FString ParentSpanId;
		FString SessionKey = RedactedSessionKey(HeaderSessionId);
		TSharedPtr<FJsonObject> RoutingContext;
		if (Req.IsValid())
		{
			FString BodyTraceId;
			if (Req->TryGetStringField(TEXT("_monolith_trace_id"), BodyTraceId) && !BodyTraceId.IsEmpty())
			{
				TraceId = BodyTraceId;
			}
			Req->TryGetStringField(TEXT("_monolith_parent_span_id"), ParentSpanId);
			FString BodySessionKey;
			if (Req->TryGetStringField(TEXT("_monolith_session_key"), BodySessionKey) && !BodySessionKey.IsEmpty())
			{
				SessionKey = RedactedSessionKey(BodySessionKey);
			}
			const TSharedPtr<FJsonObject>* RoutingObj = nullptr;
			if (Req->TryGetObjectField(TEXT("_monolith_routing_context"), RoutingObj) && RoutingObj)
			{
				RoutingContext = *RoutingObj;
			}
		}
		FMonolithToolInvocationLogger::FScopedTrace TraceScope(TraceId, ParentSpanId, FString(), SessionKey, RoutingContext);
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
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const bool bBrowserLoopbackCors = !Settings || Settings->bEnableBrowserLoopbackCors;
	Transport->SetStringField(TEXT("cors_mode"), bBrowserLoopbackCors ? TEXT("loopback_origin_allowlist") : TEXT("browser_cors_disabled"));
	Transport->SetStringField(TEXT("browser_access"), bBrowserLoopbackCors ? TEXT("loopback_only") : TEXT("disabled"));
	Transport->SetBoolField(TEXT("allow_origin_header_enabled"), bBrowserLoopbackCors);
	Transport->SetNumberField(TEXT("max_request_body_bytes"), MaxMcpRequestBodyBytes);

	TArray<TSharedPtr<FJsonValue>> Protocols;
	for (const FString& ProtocolVersion : GetSupportedProtocolVersions())
	{
		Protocols.Add(MakeShared<FJsonValueString>(ProtocolVersion));
	}
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
// MCP protocol version negotiation
// ============================================================================

const TArray<FString>& FMonolithHttpServer::GetSupportedProtocolVersions()
{
	// Oldest first; the last entry is the server-preferred version. Single source
	// of truth for the initialize negotiation and the /health advertisement.
	static const TArray<FString> Versions = {
		TEXT("2024-11-05"),
		TEXT("2025-03-26"),
		TEXT("2025-06-18"),
		TEXT("2025-11-25"),
	};
	return Versions;
}

FString FMonolithHttpServer::NegotiateProtocolVersion(const FString& RequestedVersion)
{
	const TArray<FString>& Versions = GetSupportedProtocolVersions();
	return Versions.Contains(RequestedVersion) ? RequestedVersion : Versions.Last();
}

// ============================================================================
// MCP session gate (P1c)
// ============================================================================

FMonolithHttpServer::FSessionGateResult FMonolithHttpServer::EvaluateSessionGate(
	const TArray<FString>& Methods,
	const FString& HeaderSessionId,
	const FString& HeaderProtocolVersion,
	bool bSessionKnown,
	bool bSessionModeEnabled)
{
	FSessionGateResult Result;

	// Off => byte-identical legacy behavior: never reject.
	if (!bSessionModeEnabled)
	{
		return Result;
	}

	// 1. An explicit but unsupported MCP-Protocol-Version is a malformed request
	//    regardless of method (transport-level contract): InvalidRequest + 400.
	if (!HeaderProtocolVersion.IsEmpty()
		&& !GetSupportedProtocolVersions().Contains(HeaderProtocolVersion))
	{
		Result.bReject = true;
		Result.HttpCode = EHttpServerResponseCodes::BadRequest;
		Result.RpcCode = FMonolithJsonUtils::ErrInvalidRequest;
		Result.Message = FString::Printf(
			TEXT("Unsupported MCP-Protocol-Version: %s — server supports %s."),
			*HeaderProtocolVersion,
			*FString::Join(GetSupportedProtocolVersions(), TEXT(", ")));
		return Result;
	}

	// Handshake methods establish or probe a session before the server knows its id. initialize is
	// where the client's session id is first observed (MarkInitialize); notifications/initialized
	// and ping are the follow-up/liveness steps. They are exempt from the session-id checks below.
	auto IsHandshakeMethod = [](const FString& Method)
	{
		return Method == TEXT("initialize")
			|| Method == TEXT("notifications/initialized")
			|| Method == TEXT("ping");
	};
	bool bAllHandshake = Methods.Num() > 0;
	for (const FString& Method : Methods)
	{
		if (!IsHandshakeMethod(Method))
		{
			bAllHandshake = false;
			break;
		}
	}

	// 2. A supplied session id that matches no observed row is unknown/expired: 404 — EXCEPT on a
	//    pure handshake request. initialize carries the client-chosen id the server is about to
	//    observe; rejecting it as "unknown" here (the gate runs before MarkInitialize) would make
	//    establishing a session impossible.
	if (!HeaderSessionId.IsEmpty() && !bSessionKnown && !bAllHandshake)
	{
		Result.bReject = true;
		Result.HttpCode = EHttpServerResponseCodes::NotFound;
		Result.RpcCode = FMonolithJsonUtils::ErrInvalidRequest;
		Result.Message = TEXT("Unknown or expired MCP session — re-run initialize to establish a new session.");
		return Result;
	}

	// 3. Post-initialize methods require a session id header; the handshake methods are exempt
	//    because they are how a session id is first established.
	if (HeaderSessionId.IsEmpty())
	{
		for (const FString& Method : Methods)
		{
			if (IsHandshakeMethod(Method))
			{
				continue;
			}

			Result.bReject = true;
			Result.HttpCode = EHttpServerResponseCodes::BadRequest;
			Result.RpcCode = FMonolithJsonUtils::ErrInvalidRequest;
			Result.Message = FString::Printf(
				TEXT("Missing MCP-Session-Id header for method '%s' — send the session id returned by initialize."),
				*Method);
			return Result;
		}
	}

	return Result;
}

// ============================================================================
// JSON-RPC 2.0 Processing
// ============================================================================

TSharedPtr<FJsonObject> FMonolithHttpServer::ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request)
{
	if (!Request.IsValid())
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Invalid request object — must be a JSON object with jsonrpc, method, and id fields."));
	}

	// Validate jsonrpc version
	FString Version;
	if (!Request->TryGetStringField(TEXT("jsonrpc"), Version) || Version != TEXT("2.0"))
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Missing or invalid jsonrpc version — set \"jsonrpc\" to the string \"2.0\"."));
	}

	// Get method
	FString Method;
	if (!Request->TryGetStringField(TEXT("method"), Method))
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Missing method field — set \"method\" to one of: initialize, tools/list, tools/call, ping."));
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
	else if (Method == TEXT("notifications/cancelled"))
	{
		// MCP cancellation notification. Signal the in-flight request (if it is
		// still running) through the request-id-keyed registry, then return no
		// response (it is a notification). Already-finished / synchronous requests
		// are a no-op; cooperative long-running actions observe the flag and abort.
		const TSharedPtr<FJsonValue> CancelIdField = Params->TryGetField(TEXT("requestId"));
		const FString CancelRequestId = FMonolithExecutionContext::JsonRpcIdToString(CancelIdField);
		FString CancelReason;
		Params->TryGetStringField(TEXT("reason"), CancelReason);
		if (CancelRequestId != TEXT("notification") && CancelRequestId != TEXT("unknown"))
		{
			const bool bFound = FMonolithCancellationRegistry::Get().RequestCancellation(CancelRequestId, CancelReason);
			UE_LOG(LogMonolith, Verbose, TEXT("notifications/cancelled requestId=%s found=%d"), *CancelRequestId, bFound ? 1 : 0);
		}
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
			FString::Printf(TEXT("Unknown method: %s — use tools/list to enumerate available tools, then tools/call."), *Method));
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
	// support it, otherwise fall back to the latest version we support (per the
	// MCP spec, the server returns its preferred version on a version mismatch).
	FString ClientVersion;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("protocolVersion"), ClientVersion);
	}
	Result->SetStringField(TEXT("protocolVersion"), NegotiateProtocolVersion(ClientVersion));

	// Server info
	TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), TEXT("monolith"));
	ServerInfo->SetStringField(TEXT("version"), MONOLITH_VERSION);
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);

	// Capabilities
	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();

	const UMonolithSettings* Settings = UMonolithSettings::Get();

	// We support tools. The tools.listChanged capability is advertised true only
	// when session mode is on (P1c): the active tool profile can change which
	// actions are visible, so the revision counter is meaningful and the server
	// may later emit notifications/tools/list_changed. With the flag off this
	// stays false — byte-identical legacy capabilities.
	//
	// NOTE: this advertises the capability and tracks the revision only. Actual
	// notifications/tools/list_changed delivery is NOT implemented here because
	// GET /mcp is a single-shot SSE response with no long-lived push channel;
	// delivery awaits a real SSE transport. See SPEC_MonolithMcpSessionModeGate.
	const bool bSessionModeOn = Settings && Settings->bEnableMcpSessionMode;
	TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
	ToolsCap->SetBoolField(TEXT("listChanged"), bSessionModeOn);
	if (bSessionModeOn)
	{
		// Additive, non-standard hint so clients/operators can poll tools/list and
		// detect a changed advertised surface without a server push. Lives under
		// the tools capability so it never collides with a top-level MCP field.
		ToolsCap->SetNumberField(TEXT("_monolith_tool_list_revision"),
			static_cast<double>(FMonolithToolProfileManager::Get().GetToolListRevision()));
	}
	Capabilities->SetObjectField(TEXT("tools"), ToolsCap);

	if (Settings
		&& Settings->bEnableMcpResources
		&& FMonolithResourceRegistry::Get().HasDefaultResourcesRegistered())
	{
		TSharedPtr<FJsonObject> ResourcesCap = MakeShared<FJsonObject>();
		ResourcesCap->SetBoolField(TEXT("listChanged"), false);
		Capabilities->SetObjectField(TEXT("resources"), ResourcesCap);
	}

	Result->SetObjectField(TEXT("capabilities"), Capabilities);

	// Onboarding hint so agents discover schemas instead of guessing parameter names.
	Result->SetStringField(TEXT("instructions"),
		TEXT("Monolith MCP server for Unreal Engine. ")
		TEXT("Before calling a domain action, check its schema instead of guessing: ")
		TEXT("monolith_discover() lists namespaces, monolith_discover('<namespace>') lists a ")
		TEXT("namespace's action names + descriptions (terse by default — pass detail=true to ")
		TEXT("inline param schemas), and describe_query('action_schema', ...) returns one action's ")
		TEXT("exact parameter schema. monolith_guide(section='recipes') gives cross-namespace ")
		TEXT("workflows, decision matrices, and gotchas."));

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleToolsList(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	// Each namespace becomes a tool
	TArray<FString> Namespaces = Registry.GetNamespaces();

	// Namespace tool visibility: when bExposeNamespaceTools is false (default), suppress
	// all per-namespace {ns}_query dispatcher tools. Agents use the proxy-level
	// monolith_query({namespace, action, params}) dispatcher instead and consult
	// skills or monolith_discover for the available action list.
	// The "monolith" namespace is always kept — it provides the core routing tools.
	const UMonolithSettings* MonolithSettings = GetDefault<UMonolithSettings>();
	if (!MonolithSettings || !MonolithSettings->bExposeNamespaceTools)
	{
		Namespaces = Namespaces.FilterByPredicate([](const FString& NS)
		{
			return NS == TEXT("monolith");
		});
	}

	TArray<FMonolithActionInfo> CoreActions = Registry.GetActions(TEXT("monolith"));
	ToolsArray.Reserve(Namespaces.Num() + CoreActions.Num());
	for (const FString& Namespace : Namespaces)
	{
		if (Namespace == TEXT("monolith"))
		{
			if (CoreActions.Num() == 0) continue;

			// Tool-list minimization: when management tools are hidden, advertise only the
			// core routing/discovery actions. The management/control-plane actions stay
			// registered and callable (by name via tools/call or monolith_query) — only
			// their tools/list advertisement is suppressed. See UMonolithSettings::bExposeManagementTools.
			// Core routing/discovery actions kept when management tools are hidden.
			// Paired with the synthetic monolith_query dispatcher (appended below when
			// bExposeNamespaceTools is false), this yields the minimal 4-tool surface:
			// monolith_find, monolith_discover, monolith_status, monolith_query.
			static const TSet<FString> CoreRoutingActions = {
				TEXT("find"), TEXT("discover"), TEXT("status"),
			};
			const bool bHideManagementTools = (MonolithSettings && !MonolithSettings->bExposeManagementTools);

			// Core tools are individual: monolith_discover, monolith_status
			for (const FMonolithActionInfo& ActionInfo : CoreActions)
			{
				if (bHideManagementTools && !CoreRoutingActions.Contains(ActionInfo.Action))
				{
					continue;
				}

				TSharedPtr<FJsonObject> CoreTool = MakeShared<FJsonObject>();

				FString ToolName;
				ToolName.Reserve(9 + ActionInfo.Action.Len()); // "monolith_" = 9 chars
				ToolName += TEXT("monolith_");
				ToolName += ActionInfo.Action;
				CoreTool->SetStringField(TEXT("name"), ToolName);
				CoreTool->SetStringField(TEXT("description"), ActionInfo.Description);

				// Input schema — build a JSON-Schema-compliant inputSchema.
				// ActionInfo.ParamSchema is a flat map where each entry carries
				// Monolith-internal fields (required:bool, aliases, kind) that must
				// not be forwarded to MCP clients.  Copy only standard JSON Schema
				// keywords and promote required:bool entries to the top-level
				// "required" array so clients can validate tool calls correctly.
				TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
				InputSchema->SetStringField(TEXT("type"), TEXT("object"));

				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> RequiredArray;

				if (ActionInfo.ParamSchema.IsValid())
				{
					RequiredArray.Reserve(ActionInfo.ParamSchema->Values.Num());
					static const TCHAR* const kForwardFields[] = {
						TEXT("type"), TEXT("description"), TEXT("default"),
						TEXT("enum"), TEXT("minimum"), TEXT("maximum"),
					};
					for (const auto& SchemaEntry : FMonolithJsonUtils::GetFields(ActionInfo.ParamSchema))
					{
						// Root-level internal markers (keys prefixed with '_', e.g.
						// _validate_types) are not parameters — skip them regardless
						// of whether the JSON value is a bool or an object.
						if (SchemaEntry.Key.StartsWith(TEXT("_"))) continue;
						const TSharedPtr<FJsonObject> ParamObj = SchemaEntry.Value->AsObject();
						if (!ParamObj.IsValid()) continue;

						TSharedPtr<FJsonObject> CleanProp = MakeShared<FJsonObject>();
						for (const TCHAR* Field : kForwardFields)
						{
							TSharedPtr<FJsonValue> Val = ParamObj->TryGetField(FString(Field));
							if (Val.IsValid())
							{
								CleanProp->SetField(FString(Field), Val);
							}
						}
						Properties->SetObjectField(SchemaEntry.Key, CleanProp);

						bool bParamRequired = false;
						if (ParamObj->TryGetBoolField(TEXT("required"), bParamRequired) && bParamRequired)
						{
							RequiredArray.Add(MakeShared<FJsonValueString>(SchemaEntry.Key));
						}
					}
				}

				InputSchema->SetObjectField(TEXT("properties"), Properties);
				InputSchema->SetArrayField(TEXT("required"), RequiredArray);
				CoreTool->SetObjectField(TEXT("inputSchema"), InputSchema);

				// Survivor A (plan §3.A) — MCP-spec tool annotations. Only emit
				// the `annotations` block when at least one hint is non-default;
				// avoids bloating the wire with default-false annotations on every
				// individually-registered top-level tool. Spec ref:
				// modelcontextprotocol.io/specification/2025-06-18/server/tools
				const bool bAnyHint = ActionInfo.bReadOnlyHint
					|| ActionInfo.bDestructiveHint
					|| ActionInfo.bIdempotentHint
					|| !ActionInfo.Title.IsEmpty();
				if (bAnyHint)
				{
					TSharedPtr<FJsonObject> Ann = MakeShared<FJsonObject>();
					Ann->SetBoolField(TEXT("readOnlyHint"), ActionInfo.bReadOnlyHint);
					Ann->SetBoolField(TEXT("destructiveHint"), ActionInfo.bDestructiveHint);
					Ann->SetBoolField(TEXT("idempotentHint"), ActionInfo.bIdempotentHint);
					if (!ActionInfo.Title.IsEmpty())
					{
						Ann->SetStringField(TEXT("title"), ActionInfo.Title);
					}
					CoreTool->SetObjectField(TEXT("annotations"), Ann);
				}

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

			// "params" property — keep lightweight; per-action schemas come from
			// describe_query action_schema (or monolith_discover detail=true).
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

			// Survivor A (plan §3.A) — MCP-spec dispatcher annotations. Pulled
			// from the registry's per-namespace dispatcher map (set via
			// FMonolithToolRegistry::SetDispatcherAnnotations at module init).
			// Untagged dispatchers leave IsAnyNonDefault()==false → no
			// `annotations` block on the wire.
			const FMonolithDispatcherAnnotations DispatcherAnn = Registry.GetDispatcherAnnotations(Namespace);
			if (DispatcherAnn.IsAnyNonDefault())
			{
				TSharedPtr<FJsonObject> Ann = MakeShared<FJsonObject>();
				Ann->SetBoolField(TEXT("readOnlyHint"), DispatcherAnn.bReadOnlyHint);
				Ann->SetBoolField(TEXT("destructiveHint"), DispatcherAnn.bDestructiveHint);
				Ann->SetBoolField(TEXT("idempotentHint"), DispatcherAnn.bIdempotentHint);
				if (!DispatcherAnn.Title.IsEmpty())
				{
					Ann->SetStringField(TEXT("title"), DispatcherAnn.Title);
				}
				Tool->SetObjectField(TEXT("annotations"), Ann);
			}

			ToolsArray.Add(MakeShared<FJsonValueObject>(Tool));
		}
	}

	// Single cross-namespace dispatcher. When per-namespace {ns}_query tools are
	// suppressed (bExposeNamespaceTools=false), advertise one monolith_query tool so
	// domain actions stay reachable over a direct MCP connection without the external
	// proxy. Dispatched natively in HandleToolsCall (name == "monolith_query").
	if (!MonolithSettings || !MonolithSettings->bExposeNamespaceTools)
	{
		TSharedPtr<FJsonObject> QueryTool = MakeShared<FJsonObject>();
		QueryTool->SetStringField(TEXT("name"), TEXT("monolith_query"));
		QueryTool->SetStringField(TEXT("description"),
			TEXT("Single dispatcher for every Monolith domain namespace. Set 'namespace' and 'action' (and optional 'params'). ")
			TEXT("Use monolith_discover() to list namespaces, monolith_discover(\"<namespace>\") to list a namespace's actions, ")
			TEXT("and monolith_discover(\"<namespace>\", \"<action>\") for an action's parameter schema."));

		TSharedPtr<FJsonObject> QSchema = MakeShared<FJsonObject>();
		QSchema->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> QProps = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> NsProp = MakeShared<FJsonObject>();
		NsProp->SetStringField(TEXT("type"), TEXT("string"));
		NsProp->SetStringField(TEXT("description"), TEXT("Target namespace, e.g. blueprint, material, gas, ai (see monolith_discover())."));
		QProps->SetObjectField(TEXT("namespace"), NsProp);

		TSharedPtr<FJsonObject> ActProp = MakeShared<FJsonObject>();
		ActProp->SetStringField(TEXT("type"), TEXT("string"));
		ActProp->SetStringField(TEXT("description"), TEXT("Action within the namespace (see monolith_discover(\"<namespace>\"))."));
		QProps->SetObjectField(TEXT("action"), ActProp);

		TSharedPtr<FJsonObject> PProp = MakeShared<FJsonObject>();
		PProp->SetStringField(TEXT("type"), TEXT("object"));
		PProp->SetStringField(TEXT("description"), TEXT("Action parameters; call monolith_discover(\"<namespace>\", \"<action>\") for the schema."));
		QProps->SetObjectField(TEXT("params"), PProp);

		QSchema->SetObjectField(TEXT("properties"), QProps);
		TArray<TSharedPtr<FJsonValue>> QRequired;
		QRequired.Add(MakeShared<FJsonValueString>(TEXT("namespace")));
		QRequired.Add(MakeShared<FJsonValueString>(TEXT("action")));
		QSchema->SetArrayField(TEXT("required"), QRequired);
		QueryTool->SetObjectField(TEXT("inputSchema"), QSchema);

		ToolsArray.Add(MakeShared<FJsonValueObject>(QueryTool));
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
		TSharedPtr<FJsonValue> LimitField = Params->TryGetField(TEXT("limit"));
		if (LimitField.IsValid() && !LimitField->TryGetNumber(LimitValue))
		{
			return FMonolithJsonUtils::ErrorResponse(
				Id,
				FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Parameter 'limit' must be a number"));
		}
		TSharedPtr<FJsonValue> CursorField = Params->TryGetField(TEXT("cursor"));
		if (CursorField.IsValid() && !CursorField->TryGetString(Cursor))
		{
			return FMonolithJsonUtils::ErrorResponse(
				Id,
				FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Parameter 'cursor' must be a string"));
		}
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
	if (!Params.IsValid())
	{
		return FMonolithJsonUtils::ErrorResponse(
			Id,
			FMonolithJsonUtils::ErrInvalidParams,
			TEXT("Missing params"));
	}

	FString Uri;
	if (const TSharedPtr<FJsonValue> UriField = Params->TryGetField(TEXT("uri")))
	{
		if (!UriField->TryGetString(Uri))
		{
			return FMonolithJsonUtils::ErrorResponse(
				Id,
				FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Parameter 'uri' must be a string"));
		}
	}
	if (Uri.IsEmpty())
	{
		return FMonolithJsonUtils::ErrorResponse(
			Id,
			FMonolithJsonUtils::ErrInvalidParams,
			TEXT("Missing resource uri"));
	}

	FMonolithResourceRegistry& Registry = FMonolithResourceRegistry::Get();

	// Resolve the resource exactly once: resources/read returns a JSON-RPC not-found error when the
	// resource is missing, and reuses that same resolved result to build the success body. The
	// text/blob content shape stays in one place (FMonolithResourceRegistry::ResultToContentsJson),
	// and a provider/file-backed resource is read only once per request.
	const FMonolithResourceReadResult Read = Registry.ReadResource(Uri);
	if (!Read.bFound)
	{
		return FMonolithJsonUtils::ErrorResponse(
			Id,
			FMonolithJsonUtils::ErrResourceNotFound,
			Read.Error);
	}

	TSharedPtr<FJsonObject> Result = FMonolithResourceRegistry::ResultToContentsJson(Read);
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
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing params — tools/call params must include \"name\" and optionally \"arguments\"."));
	}

	FString ToolName;
	if (const TSharedPtr<FJsonValue> NameField = Params->TryGetField(TEXT("name")))
	{
		if (!NameField->TryGetString(ToolName))
		{
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				TEXT(""), TEXT(""), TEXT(""), TEXT("malformed_dispatch"),
				FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'name' must be a string"));
			return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'name' must be a string."));
		}
	}
	if (ToolName.IsEmpty())
	{
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			TEXT(""),
			TEXT(""),
			TEXT(""),
			TEXT("malformed_dispatch"),
			FMonolithJsonUtils::ErrInvalidParams,
			TEXT("Missing tool name"));
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing tool name — set params.name to a tool like monolith_discover or <namespace>_query."));
	}

	// Get arguments
	TSharedPtr<FJsonObject> Arguments;
	const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
	if (const TSharedPtr<FJsonValue> ArgsField = Params->TryGetField(TEXT("arguments")))
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!ArgsField->TryGetObject(ObjectPtr) || !ObjectPtr || !(*ObjectPtr).IsValid())
		{
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				ToolName, TEXT(""), TEXT(""), TEXT("malformed_dispatch"),
				FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'arguments' must be an object"));
			return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'arguments' must be an object."));
		}
		ArgsObj = ObjectPtr;
	}
	if (ArgsObj)
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
	if (ToolName == TEXT("monolith_query"))
	{
		// Single cross-namespace dispatcher: arguments carry {namespace, action, params}.
		// Checked before the generic monolith_ branch because "monolith_query" also
		// starts with "monolith_" but must route to the namespace named in arguments.
		if (const TSharedPtr<FJsonValue> NamespaceField = Arguments->TryGetField(TEXT("namespace")))
		{
			if (!NamespaceField->TryGetString(Namespace))
			{
				FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
					ToolName, TEXT(""), TEXT(""), TEXT("malformed_dispatch"),
					FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'namespace' must be a string"));
				return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'namespace' must be a string."));
			}
		}
			if (Namespace.IsEmpty())
		{
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				ToolName, TEXT(""), TEXT(""), TEXT("malformed_dispatch"),
				FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing 'namespace'"));
			return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Missing 'namespace' — monolith_query requires arguments.namespace; call monolith_discover() to enumerate namespaces."));
		}
		if (const TSharedPtr<FJsonValue> ActionField = Arguments->TryGetField(TEXT("action")))
		{
			if (!ActionField->TryGetString(Action))
			{
				FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
					ToolName, Namespace, TEXT(""), TEXT("malformed_dispatch"),
					FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'action' must be a string"));
				return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'action' must be a string."));
			}
		}
			if (Action.IsEmpty())
		{
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				ToolName, Namespace, TEXT(""), TEXT("malformed_dispatch"),
				FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing 'action'"));
			return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Missing 'action' — monolith_query requires arguments.action; call monolith_discover(\"<namespace>\") to enumerate actions."));
		}

		// Normalise the params shape: top-level extras (excluding namespace/action/params)
		// merged with a nested "params" object or a JSON-encoded "params" string. Mirrors
		// the *_query branch so the dispatched handler sees a single flat params object.
		TSharedPtr<FJsonObject> TopLevelExtras = MakeShared<FJsonObject>();
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Arguments))
		{
			if (Pair.Key != TEXT("namespace") && Pair.Key != TEXT("action") && Pair.Key != TEXT("params"))
			{
				TopLevelExtras->SetField(Pair.Key, Pair.Value);
			}
		}

		const TSharedPtr<FJsonObject>* NestedParams = nullptr;
		TSharedPtr<FJsonObject> ParsedParamsObj;
		bool bHasNestedParams = false;
		if (Arguments->TryGetObjectField(TEXT("params"), NestedParams) && NestedParams)
		{
			bHasNestedParams = true;
		}
		else
		{
			FString ParamsStr;
			if (const TSharedPtr<FJsonValue> ParamsStrField = Arguments->TryGetField(TEXT("params")))
			{
				if (!ParamsStrField->TryGetString(ParamsStr))
				{
					FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
						ToolName, Namespace, Action, TEXT("malformed_dispatch"),
						FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'params' must be a JSON object or string if present"));
					return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'params' must be a JSON object or string if present."));
				}
			}
			else if (!ParamsStr.IsEmpty())
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
			for (const auto& Pair : FMonolithJsonUtils::GetFields(TopLevelExtras)) { Arguments->SetField(Pair.Key, Pair.Value); }
			for (const auto& Pair : FMonolithJsonUtils::GetFields(*NestedParams)) { Arguments->SetField(Pair.Key, Pair.Value); }
		}
		else
		{
			Arguments = TopLevelExtras;
		}
	}
	else if (ToolName.StartsWith(TEXT("monolith_")))
	{
		// Core tool: monolith_discover -> namespace="monolith", action="discover"
		Namespace = TEXT("monolith");
		Action = ToolName.Mid(9);

		// Symmetric string-unwrap + top-level-extras merge (mirrors the *_query
		// branch below). Some MCP clients (Claude Code) serialize the "params"
		// object as a JSON-encoded string; others nest a real object; others
		// scatter optional shaping flags (_fields/_omit/_compact_json) at the
		// top level alongside the tool-specific args. Normalise all shapes
		// so the dispatched action handler sees a single flat params object.
		TSharedPtr<FJsonObject> TopLevelExtras = MakeShared<FJsonObject>();
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Arguments))
		{
			if (Pair.Key != TEXT("params"))
			{
				TopLevelExtras->SetField(Pair.Key, Pair.Value);
			}
		}

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
			if (const TSharedPtr<FJsonValue> ParamsStrField = Arguments->TryGetField(TEXT("params")))
			{
				if (!ParamsStrField->TryGetString(ParamsStr))
				{
					FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
						ToolName, Namespace, Action, TEXT("malformed_dispatch"),
						FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'params' must be a JSON object or string if present"));
					return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'params' must be a JSON object or string if present."));
				}
			}
			else if (!ParamsStr.IsEmpty())
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
			for (const auto& Pair : FMonolithJsonUtils::GetFields(TopLevelExtras))
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
			// Overlay nested params (higher priority)
			for (const auto& Pair : FMonolithJsonUtils::GetFields(*NestedParams))
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
	else if (ToolName.EndsWith(TEXT("_query")) || ToolName.EndsWith(TEXT(".query")))
	{
		// Domain tool: blueprint_query (or legacy blueprint.query) -> namespace="blueprint"
		Namespace = ToolName.Left(ToolName.Len() - 6); // strip "_query" or ".query"

		if (const TSharedPtr<FJsonValue> ActionField = Arguments->TryGetField(TEXT("action")))
		{
			if (!ActionField->TryGetString(Action))
			{
				FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
					ToolName, Namespace, TEXT(""), TEXT("malformed_dispatch"),
					FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'action' must be a string"));
				return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'action' must be a string."));
			}
		}
			if (Action.IsEmpty())
		{
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				ToolName,
				Namespace,
				TEXT(""),
				TEXT("malformed_dispatch"),
				FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Missing 'action' in arguments"));
			return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Missing 'action' in arguments — for *_query tools, set arguments.action; call monolith_discover(\"<namespace>\") to enumerate."));
		}

		// Collect top-level fields (excluding reserved keys) — MCP clients may
		// place optional params like members_only alongside "action" rather than
		// nesting them inside "params".
		TSharedPtr<FJsonObject> TopLevelExtras = MakeShared<FJsonObject>();
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Arguments))
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
			if (Arguments->HasField(TEXT("params")) && !Arguments->TryGetStringField(TEXT("params"), ParamsStr))
			{
				FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
					ToolName, Namespace, Action, TEXT("malformed_dispatch"),
					FMonolithJsonUtils::ErrInvalidParams, TEXT("Parameter 'params' must be a JSON object or string if present"));
				return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams,
					TEXT("Parameter 'params' must be a JSON object or string if present."));
			}
			else if (!ParamsStr.IsEmpty())
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
			for (const auto& Pair : FMonolithJsonUtils::GetFields(TopLevelExtras))
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
			// Overlay nested params (higher priority)
			for (const auto& Pair : FMonolithJsonUtils::GetFields(*NestedParams))
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
			FString::Printf(TEXT("Unknown tool: %s — tool must start with monolith_ or end with _query; call tools/list to enumerate."), *ToolName));
	}

	// Record start time for duration measurement without shadowing the server start timestamp member.
	double ActionStartTimeSeconds = FPlatformTime::Seconds();

	// Publish a per-request execution context for the duration of dispatch so the
	// ToolCall-record guard (and future cancellation/progress wiring) can read the
	// request metadata via FMonolithExecutionContext::GetCurrent(). Additive: any
	// handler that does not consult the context is unaffected.
	FMonolithExecutionContext::FParams ExecutionContextParams;
	ExecutionContextParams.JsonRpcId = FMonolithExecutionContext::JsonRpcIdToString(Id);
	ExecutionContextParams.SourceToolName = ToolName;
	ExecutionContextParams.Namespace = Namespace;
	ExecutionContextParams.Action = Action;
	ExecutionContextParams.ProgressToken = FMonolithExecutionContext::ExtractProgressToken(Params);
	FMonolithExecutionContext ExecutionContext(ExecutionContextParams);
	FScopedMonolithExecutionContext ScopedExecutionContext(ExecutionContext);

	// Register this request as cancellable for the duration of dispatch so a
	// concurrent notifications/cancelled can signal it cross-thread (via the
	// request-id-keyed registry, not the thread-local context). Opt-in long-running
	// actions poll FMonolithCancellationRegistry::IsCancellationRequested(json_rpc_id).
	FScopedMonolithCancellationRegistration CancellationRegistration(ExecutionContextParams.JsonRpcId);

	// Track in-flight progress for this request's progressToken (if any) so an
	// opt-in long-running action can report progress visible via the
	// monolith://progress/active resource. Empty token (the common case) is inert.
	FScopedMonolithProgressRegistration ProgressRegistration(ExecutionContextParams.ProgressToken);

	// Execute via registry
	FMonolithActionResult ActionResult = FMonolithToolRegistry::Get().ExecuteAction(Namespace, Action, Arguments);

	// Calculate duration
	double DurationMs = (FPlatformTime::Seconds() - ActionStartTimeSeconds) * 1000.0;
	UE_LOG(LogMonolith, Verbose, TEXT("Monolith action %s.%s completed in %.2f ms"), *Namespace, *Action, DurationMs);

	// Build MCP tool result
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(
		ActionResult,
		Settings && Settings->bEnableStructuredToolResults,
		Settings && Settings->bEnableTypedMediaResults,
		!Settings || Settings->bCompactErrorEnvelope);

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
}

bool FMonolithHttpServer::IsAllowedOrigin(const FString& Origin)
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

void FMonolithHttpServer::AddCorsHeaders(FHttpServerResponse& Response, const FHttpServerRequest& Request)
{
	// Always advertise the methods/headers we support — these are not
	// origin-sensitive. The allow-origin echo is the gated piece.
	Response.Headers.Add(TEXT("Access-Control-Allow-Methods"), {TEXT("GET, POST, DELETE, OPTIONS")});
	Response.Headers.Add(TEXT("Access-Control-Allow-Headers"), {TEXT("Content-Type, Accept, MCP-Session-Id, MCP-Protocol-Version")});
	Response.Headers.Add(TEXT("Vary"), {TEXT("Origin")});

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->bEnableBrowserLoopbackCors)
	{
		return;
	}

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
