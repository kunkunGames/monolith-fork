#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "HttpServerConstants.h"
#include "IHttpRouter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

class FJsonObject;
class FJsonValue;
class FMonolithToolRegistry;

/**
 * Embedded MCP HTTP server.
 * Implements Streamable HTTP transport with JSON-RPC 2.0 dispatch.
 */
class MONOLITHCORE_API FMonolithHttpServer
{
public:
	FMonolithHttpServer();
	~FMonolithHttpServer();

	/** Start the HTTP server on the configured port */
	bool Start(int32 Port);

	/**
	 * Stop serving Monolith by unbinding its routes. UE owns HTTP listeners
	 * process-wide, so the shared transport remains available for other plugins
	 * and can be reused by a later Start on the same port.
	 */
	void Stop();

	/** Stop then Start — useful after a silent bind failure */
	bool Restart(int32 Port);

	/** Is the server currently running? */
	bool IsRunning() const { return bIsRunning; }

	/** Get the port on which Monolith routes are currently active. */
	int32 GetPort() const { return BoundPort; }

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Inject a deterministic port probe and bounded retry policy for startup
	 * failure tests. Production instances retain the normal 20-attempt policy.
	 */
	void ConfigureStartForTests(
		TFunction<bool(int32)> InPortProbe,
		int32 InMaxAttempts = 1,
		float InRetryBackoffSeconds = 0.0f,
		float InPostBindProbeDelaySeconds = 0.0f);
#endif

	// Validates origin against localhost/loopback allowlist. Exposed for testing.
	static bool IsAllowedOrigin(const FString& Origin);

	// MCP protocol versions this server supports, oldest first; the last entry is
	// the server-preferred version. Exposed for testing and the /health endpoint.
	static const TArray<FString>& GetSupportedProtocolVersions();

	// Negotiate the response protocolVersion for an initialize request: echoes
	// RequestedVersion when supported, otherwise returns the latest supported
	// version (server-preferred, per the MCP version-mismatch rule). For testing.
	static FString NegotiateProtocolVersion(const FString& RequestedVersion);

	// P1c MCP session gate result. bReject=false means the request passes (or the
	// session-mode flag is off — the legacy pass-through). When bReject=true,
	// HttpCode/RpcCode/Message carry the rejection; the caller serializes a
	// JSON-RPC error body with HttpCode and returns it as the whole HTTP response.
	struct FSessionGateResult
	{
		bool bReject = false;
		EHttpServerResponseCodes HttpCode = EHttpServerResponseCodes::Ok;
		int32 RpcCode = 0;
		FString Message;
	};

	// Evaluate the MCP Streamable HTTP session contract for one POST /mcp body.
	// Pure (no server state) and static so it is unit-testable like IsAllowedOrigin.
	// Returns bReject=false unconditionally when bSessionModeEnabled is false.
	// Methods is the ordered list of JSON-RPC method names in the request body;
	// bSessionKnown is whether the supplied session id matches an observed row.
	static FSessionGateResult EvaluateSessionGate(
		const TArray<FString>& Methods,
		const FString& HeaderSessionId,
		const FString& HeaderProtocolVersion,
		bool bSessionKnown,
		bool bSessionModeEnabled);

private:
	// --- Route Handlers ---
	bool HandlePostMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleGetMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDeleteMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleHealthCheck(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// --- JSON-RPC Processing ---
	TSharedPtr<FJsonObject> ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request);
	TSharedPtr<FJsonObject> HandleInitialize(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleToolsList(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleToolsCall(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleResourcesList(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleResourcesRead(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandlePing(const TSharedPtr<FJsonValue>& Id);

	// --- Helpers ---
	TUniquePtr<FHttpServerResponse> MakeJsonResponse(const FString& JsonBody, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok);
	TUniquePtr<FHttpServerResponse> MakeSseResponse(const TArray<TSharedPtr<FJsonObject>>& Messages);

	// Echo Origin only when it matches the localhost allowlist. Browsers block
	// cross-origin reads when ACAO is missing, so omitting the header for
	// non-allowlisted origins is the defence — see Issue #38.
	void AddCorsHeaders(FHttpServerResponse& Response, const FHttpServerRequest& Request);

	/** Register all HTTP routes on the current HttpRouter. */
	void BindRoutes();

	/** Unbind only Monolith routes; never stop process-wide UE HTTP listeners. */
	void DeactivateRoutes();

	/** Use the production TCP probe unless a dev automation test injected one. */
	bool IsPortListening(int32 Port) const;

	/** Probe 127.0.0.1:Port via a TCP connect to verify the listener is actually bound. */
	static bool ProbePort(int32 Port);

	// --- State ---
	TSharedPtr<IHttpRouter> HttpRouter;
	TArray<FHttpRouteHandle> RouteHandles;
	/** Port of the UE-owned router retained for safe same-process reuse. */
	int32 ListenerPort = 0;
	int32 BoundPort = 0;
	bool bIsRunning = false;
	FDateTime StartTime;
	int32 MaxStartAttempts = 20;
	float RetryBackoffSeconds = 2.0f;
	float PostBindProbeDelaySeconds = 0.1f;

#if WITH_DEV_AUTOMATION_TESTS
	TFunction<bool(int32)> PortProbeForTests;
#endif
};
