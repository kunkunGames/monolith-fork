# MCP SSE Streaming Feasibility / Deferral Record (P1d Stage-2)

**Date:** 2026-06-21
**Scope:** MonolithCore MCP Streamable HTTP transport — feasibility of real server-push SSE streaming (progress notifications, `notifications/*` push, incremental tool-result frames) on top of the UE 5.8 patched `HTTPServer` module.
**Type:** Feasibility / deferral record. **DOCS ONLY — no transport code lands in this changelist.**
**Result:** Engine support is now present and verified; the slice is **deferred** because three Monolith-side blockers remain after the P1a/P1c slices. This record names each blocker against current source so the P1d Stage-2 implementation has a grounded starting point.
**Changelist:** 874
**Related specs:** [specs/SPEC_MonolithMcpSessionMode.md](../specs/SPEC_MonolithMcpSessionMode.md), [specs/SPEC_MonolithMcpSessionModeGate.md](../specs/SPEC_MonolithMcpSessionModeGate.md)

---

## 1. Why This Record Exists

The MCP Streamable HTTP spec allows a server to answer a `POST /mcp` request with a `text/event-stream` body and push multiple frames (progress notifications, partial results, then the final JSON-RPC response) before closing. Monolith does **not** do this today: every `POST /mcp` request is answered with exactly one buffered JSON response, and `GET /mcp` returns a single SSE `endpoint` event then closes (`FMonolithHttpServer::HandleGetMcp`, `MonolithHttpServer.cpp:532`).

Two prior slices set up the surface but stopped short of transport:

- **P1a — session observation** ([SPEC_MonolithMcpSessionMode.md](../specs/SPEC_MonolithMcpSessionMode.md)): added `FMonolithMcpSessionTracker` as a bounded, redacted, in-memory **observer** of session headers. It explicitly lists "Request-level progress records", "`notifications/progress` emission", and "In-flight cancellation" as follow-up slices.
- **P1c — session-mode spec-correctness gate** ([SPEC_MonolithMcpSessionModeGate.md](../specs/SPEC_MonolithMcpSessionModeGate.md)): promoted the tracker toward the MCP lifecycle (`Observed`/`Initializing`/`Initialized`, redacted client-capability booleans, server-side session gate at `MonolithHttpServer.cpp:431-457`) and tracks a `tools/list` revision. The existing TODO item **"`notifications/tools/list_changed` delivery (awaits SSE)"** records that server push is blocked on a real SSE transport.

This record answers the open question from those slices: **is the transport now buildable, and what exactly is missing?**

---

## 2. Engine Support — Now Present and Verified (UE 5.8)

`GO.uproject` `EngineAssociation` is `5.8`, so the project builds against the patched `HTTPServer` module at `D:\Engine\UE_5.8\Engine\Source\Runtime\Online\HTTPServer`. The streaming primitives the transport needs are real in this build, not reference-only:

| Engine primitive | Location | What it provides |
|---|---|---|
| `EHttpServerResponseFlags::MultipleWriteStream` (`1 << 0`) | `Public/HttpServerResponse.h:24` | Marks a response whose body is written as multiple chunks before another read/close — "possibly an SSE stream" per the engine comment. |
| `EHttpServerResponseFlags::HasAdditionalWrites` (`1 << 1`) | `Public/HttpServerResponse.h:27` | Signals more stream writes are expected; used together with `MultipleWriteStream`. |
| `EHttpServerResponseFlags::SkipHeaderWrite` (`1 << 2`) | `Public/HttpServerResponse.h:30` | Lets follow-up frames skip re-emitting headers. |
| `FHttpServerResponse::StreamingBodyQueue` | `Public/HttpServerResponse.h:75` | `TSharedPtr<TQueue<TArray<uint8>, EQueueMode::Spsc>>` — a single-producer/single-consumer queue. One producer thread enqueues chunks; the game thread (`WriteStream`) dequeues. |
| `FHttpServerResponse::StreamingBodyComplete` | `Public/HttpServerResponse.h:76` | `TSharedPtr<TAtomic<bool>>` — set true after the last chunk is enqueued so the writer can finish and close. |
| Streaming drain in the write context | `Private/HttpConnectionResponseWriteContext.cpp:30-145` | Honors the flags, drains `StreamingBodyQueue` chunk-by-chunk after the initial `Body`, and finalizes on `StreamingBodyComplete`. |

Engine contract that constrains the design — the result callback reentry rule (`Public/HttpResultCallback.h:7-15`):

> A streaming handler using `MultipleWriteStream | HasAdditionalWrites` may invoke this callback **up to twice on the same call stack** (e.g. SSE open + a single follow-up frame). The second invocation is captured into the connection's single-slot pending response and drained once the in-flight write finishes. **A third synchronous invocation asserts via `checkf`.** Cross-tick invocations are not bounded.

`FHttpConnection::CompleteRead` (`Private/HttpConnection.cpp:157-183`) implements the teardown side of that contract: a streaming handler may re-enter the callback after the connection was already torn down mid-stream by `HandleWriteError`, and the only safe detection is the connection state, so a late response is dropped silently.

**Conclusion:** the engine can carry a real SSE stream. The producer is expected to enqueue chunks onto the SPSC `StreamingBodyQueue` from a worker thread while the game thread drains it; synchronous re-invocation of the result callback is hard-capped at two per call stack.

---

## 3. Remaining Blockers After P1a / P1c (Monolith side)

Engine readiness does not make the slice droppable. Three Monolith-side blockers remain, each grounded in current source.

### 3.1 No progress sink in the synchronous game-thread `ExecuteAction`

`FMonolithHttpServer::HandleToolCall` runs the action synchronously on the request/game thread and only obtains a single, fully-buffered result:

```
// MonolithHttpServer.cpp:1486
FMonolithActionResult ActionResult = FMonolithToolRegistry::Get().ExecuteAction(Namespace, Action, Arguments);
```

`ExecuteAction` takes `(Namespace, Action, Arguments)` and returns one `FMonolithActionResult`. There is **no progress callback, cancellation token, or streaming sink** threaded into the call, and every domain handler behind it is written to compute its whole result and return. A long action (reindex, CRG rebuild, town-gen, batch asset edits) is therefore opaque until it finishes — there is no interior point at which it could emit a `notifications/progress` frame even if the transport existed.

This is the load-bearing blocker: without a progress-sink parameter on `ExecuteAction` (or a thread-local/ambient sink scoped per request) that long handlers can poll/push to, an SSE transport would only ever be able to send "open" then "final result" — i.e., it would add framing overhead with zero incremental value. Stage-2 must introduce the sink contract first.

### 3.2 The session tracker is not an authoritative per-request store

`FMonolithMcpSessionTracker` (`Public/MonolithMcpSessionTracker.h`) is deliberately a **redacted observer**, not a request registry. Its `FSessionRow` holds `SessionKey`, `SessionIdRedacted`, `ProtocolVersion`, `LastMethod`, `LastToolName`, timestamps, a `RequestCount`, an `EMonolithMcpSessionStatus`, and three client-capability booleans — and nothing else. It is keyed per **session**, bounded to `SessionCapacity = 128`, and evicts oldest rows.

A streaming transport needs an authoritative **per-request** store that the tracker does not provide:

- **`progressToken`** — MCP carries the progress token in `params._meta.progressToken` of the originating request; `notifications/progress` must echo it. The tracker stores no per-request token (and, by contract, stores no raw params), so there is currently nowhere to record or look it up.
- **`StreamingBodyQueue` handle + `StreamingBodyComplete`** — the producer needs the live SPSC queue/atomic for the specific in-flight request to push frames; the tracker holds neither.
- **`AliveGuard`** — per §2 the connection can be torn down mid-stream (`CompleteRead`/`HandleWriteError`), so the producer must hold a weak/alive guard and stop enqueuing once the request is dead. The tracker has no such handle.

Stage-2 therefore needs a new per-request structure (e.g. an `FMonolithMcpRequestStream` keyed by request/connection) that owns `{ progressToken, StreamingBodyQueue, StreamingBodyComplete, AliveGuard }`. The session tracker stays the redacted session observer; it must not be overloaded into a raw-token request store (that would regress its redaction contract).

### 3.3 Reentrancy hazard at the single-shot completion site (`MonolithHttpServer.cpp:462`)

The per-request loop currently treats completion as single-shot. Each request is processed and the handler calls `OnComplete(MoveTemp(Response))` exactly once (the `CompleteRead` path), e.g. at `MonolithHttpServer.cpp:418`, `:453`, `:503`, `:528`. The loop that owns this — and the trace/observe scope that wraps each request — begins at:

```
// MonolithHttpServer.cpp:459-493
Responses.Reserve(Requests.Num());
for (const TSharedPtr<FJsonObject>& Req : Requests)
{
    FString TraceId = HeaderTraceId;          // line 462
    ...
    FMonolithToolInvocationLogger::FScopedTrace TraceScope(...);
    ObserveMcpSessionIfEnabled(Req, HeaderSessionId, HeaderProtocolVersion);
    TSharedPtr<FJsonObject> Resp = ProcessJsonRpcRequest(Req);
    if (Resp.IsValid()) { Responses.Add(Resp); }
}
... // one OnComplete(...) after the loop builds the buffered response
```

The hazard: the engine's result callback may be re-invoked at most **twice synchronously** per call stack and **asserts via `checkf` on a third** (`HttpResultCallback.h:11-14`). A naive streaming retrofit that pushed an "open" frame plus per-step progress frames synchronously from inside this loop — across a **batch** request, where the loop iterates `Requests.Num()` times — would blow that cap and trip the engine `checkf`. It would also break the current invariant that batch responses are coalesced into a single array body after the loop (`MonolithHttpServer.cpp:506-524`). Any streaming work must move progress frames onto the SPSC `StreamingBodyQueue` (drained on later ticks, which are unbounded) rather than re-invoking the result callback inline, and must define batch-vs-stream behavior explicitly rather than inheriting the single-shot coalescing path.

---

## 4. Why Streaming Is Unsafe Beside the Default `/mcp` Path the Headless Harness Uses

The headless MCP harness (`BatchFiles\RunHeadlessEditor.bat`, MCP client → `http://localhost:9316/mcp`, recovery via `Scripts/recover_mcp.ps1`) and the offline proxy path drive the **default `POST /mcp`** route and expect exactly one buffered JSON (or JSON array) response per request. Turning that same route into a multi-frame `text/event-stream` would be unsafe for three reasons grounded above:

1. **Single-shot consumers break on multi-frame bodies.** The default path's callers parse one JSON body; an SSE-framed body (`event: message\ndata: ...`) on the route they already use would be a silent content-type/shape change for the harness, the proxy, and `monolith_query.exe` flows. The existing `GET /mcp` SSE is tolerated only because it returns a single event and closes (`MonolithHttpServer.cpp:532-544`); a long-lived multi-frame stream on `POST /mcp` is a different contract.
2. **The reentrancy cap is per call stack (§3.3).** Retrofitting streaming inline on the default route risks tripping the engine `checkf` exactly on the high-traffic path the harness exercises most, converting a feature gap into an editor assert.
3. **No alive guard / mid-stream teardown handling on the default path (§3.2, §2).** `CompleteRead`/`HandleWriteError` can tear the connection down mid-stream; without a per-request `AliveGuard`, a producer would keep enqueuing onto a dead connection. The default path never needs this today because it completes synchronously in one write.

Therefore Stage-2 streaming must be **opt-in and isolated** — gated behind a dark-by-default setting and negotiated per request (client opts in via `Accept: text/event-stream` plus a `progressToken`), so the default `POST /mcp` path the headless harness depends on keeps its single-buffered-response contract unchanged. Streaming is an additive capability on top of the existing route, never a silent behavior change to it.

---

## 5. Stage-2 Implementation Order (for the future code slice — not this CL)

This record fixes the order so the eventual P1d Stage-2 code slice is grounded:

1. **Progress-sink contract** — thread an optional per-request progress/stream sink into `FMonolithToolRegistry::ExecuteAction` (or an ambient request-scoped sink), and teach at least the known long handlers (reindex, CRG rebuild, town-gen, batch edits) to push progress. Without this, streaming has nothing to stream (§3.1).
2. **Per-request stream store** — add `FMonolithMcpRequestStream` owning `{ progressToken, StreamingBodyQueue, StreamingBodyComplete, AliveGuard }`, keyed per in-flight request/connection; keep `FMonolithMcpSessionTracker` as the redacted session observer (§3.2).
3. **Opt-in SSE transport on `POST /mcp`** — negotiated by `Accept: text/event-stream` + `progressToken`, gated dark-by-default; produce frames onto `StreamingBodyQueue` (drained cross-tick), never by re-invoking the result callback inline; define explicit batch-vs-stream behavior; honor `AliveGuard` on mid-stream teardown (§3.3, §4).
4. **`notifications/*` push** — once 1–3 land, deliver `notifications/progress` and the already-tracked `notifications/tools/list_changed` (the existing "awaits SSE" TODO item) over the stream.

---

## 6. Verification Performed for This Record

| Claim | Evidence |
|---|---|
| Engine has `MultipleWriteStream` / `HasAdditionalWrites` flags | `D:\Engine\UE_5.8\...\HTTPServer\Public\HttpServerResponse.h:24,27` |
| Engine has SPSC `StreamingBodyQueue` + `StreamingBodyComplete` | `HttpServerResponse.h:75-76`; drain logic `Private\HttpConnectionResponseWriteContext.cpp:30-145` |
| Result-callback reentry capped at 2/stack, 3rd `checkf` | `Public\HttpResultCallback.h:7-15` |
| Mid-stream teardown / silent-drop contract | `Private\HttpConnection.cpp:157-183` (`CompleteRead`) |
| Project builds against UE 5.8 | `GO.uproject` `"EngineAssociation": "5.8"` |
| `ExecuteAction` is single synchronous buffered call, no sink | `MonolithHttpServer.cpp:1486` |
| Tracker is redacted session observer, not per-request token store | `Public\MonolithMcpSessionTracker.h` (`FSessionRow`, `SessionCapacity=128`) |
| Single-shot completion loop / batch coalescing | `MonolithHttpServer.cpp:459-528` (loop opens at `:462`) |
| `GET /mcp` returns single SSE event then closes | `MonolithHttpServer.cpp:532-544` |
| P1a/P1c follow-ups already name progress/notifications as deferred | [SPEC_MonolithMcpSessionMode.md](../specs/SPEC_MonolithMcpSessionMode.md) §8; TODO "`notifications/tools/list_changed` delivery (awaits SSE)" |

No transport code, settings flags, or action contracts were added or changed in this changelist; this is a docs-only feasibility/deferral record.
