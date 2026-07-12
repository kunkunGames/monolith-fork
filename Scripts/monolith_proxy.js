#!/usr/bin/env node
"use strict";

const http = require("http");
const https = require("https");
const crypto = require("crypto");
const fs = require("fs");
const os = require("os");
const path = require("path");
const readline = require("readline");

const MONOLITH_URL = process.env.MONOLITH_URL || "http://localhost:9316/mcp";
const MONOLITH_HEALTH = MONOLITH_URL.replace(/\/mcp$/, "/health");
const PROXY_NAME = "monolith-proxy";
const PROXY_VERSION = "1.1.1-node";
const TIMEOUT_MS = 30000;
const POLL_INTERVAL_MS = 5000;
const POLL_START_DELAY_MS = 3000;
// Transient-connection retry: the editor MCP endpoint at 9316 flickers (a request fails to
// connect, the next succeeds) during busy/GC/build windows. Retry ONLY send-side connection
// failures — the request never reached the server, so retrying cannot double-execute a
// mutation. Read timeouts are deliberately NOT retried (the request may already be applied).
const CONNECT_RETRIES = Math.max(0, parseInt(process.env.MONOLITH_CONNECT_RETRIES || "3", 10) || 0);
const CONNECT_RETRY_BACKOFF_MS = 250;
// Total wall-clock budget for retries (see the Python proxy note): bails after ~1 slow attempt
// when the endpoint is fully down/blackholed, but allows several fast retries for real flicker.
const CONNECT_RETRY_BUDGET_MS = Math.max(0, parseInt(process.env.MONOLITH_CONNECT_RETRY_BUDGET_MS || "1500", 10) || 0);
const RETRYABLE_CONNECT_CODES = new Set([
  "ECONNREFUSED", "ECONNRESET", "ECONNABORTED", "EHOSTUNREACH", "ENETUNREACH", "EPIPE",
]);

let monolithWasUp = null;
let pendingRequests = 0;
let stdinClosed = false;
let toolLogSequence = 0;
let processInstanceId = null;
let lastRecordId = null;
let lastRecordStartMs = null;
let lastErrorRecordId = null;
let recentFind = null;
let recentDiscover = null;
const recentToolLogSignatures = new Map();

const SENSITIVE_KEY_FRAGMENTS = [
  "authorization",
  "bearer",
  "token",
  "api_key",
  "apikey",
  "password",
  "passwd",
  "secret",
  "cookie",
  "private_key",
  "session_id",
];
const DEFAULT_MAX_LOG_FIELD_BYTES = 256 * 1024;
const REPEAT_LOG_WINDOW_MS = 15000;

// Per-namespace *_query seed tools are no longer used — agents dispatch via
// the single monolith_query tool. Kept empty for structural compatibility.
const CORE_QUERY_TOOLS = [];

function log(message) {
  process.stderr.write(`[monolith-proxy] ${message}\n`);
}

function toolLogEnabled() {
  return process.env.MONOLITH_TOOL_LOG_ENABLED !== "0";
}

function maxLogFieldBytes() {
  const raw = process.env.MONOLITH_TOOL_LOG_MAX_FIELD_BYTES;
  if (!raw) return DEFAULT_MAX_LOG_FIELD_BYTES;
  const parsed = Number.parseInt(raw, 10);
  if (!Number.isFinite(parsed)) return DEFAULT_MAX_LOG_FIELD_BYTES;
  return Math.max(1024, Math.min(parsed, 16 * 1024 * 1024));
}

function findLogRoot() {
  if (process.env.MONOLITH_TOOL_LOG_DIR) return process.env.MONOLITH_TOOL_LOG_DIR;

  let current = __dirname;
  for (;;) {
    if (fs.existsSync(path.join(current, "Monolith.uplugin"))) {
      return path.join(current, "Logs");
    }
    const parent = path.dirname(current);
    if (parent === current) break;
    current = parent;
  }
  return path.join(process.cwd(), "Logs");
}

function dailyLogPath() {
  const now = new Date();
  const day = `${now.getFullYear()}${String(now.getMonth() + 1).padStart(2, "0")}${String(now.getDate()).padStart(2, "0")}`;
  return path.join(findLogRoot(), day, "proxy.jsonl");
}

function localIsoNow() {
  const now = new Date();
  const offsetMinutes = -now.getTimezoneOffset();
  const sign = offsetMinutes >= 0 ? "+" : "-";
  const absMinutes = Math.abs(offsetMinutes);
  const offset = `${sign}${String(Math.floor(absMinutes / 60)).padStart(2, "0")}:${String(absMinutes % 60).padStart(2, "0")}`;
  return `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, "0")}-${String(now.getDate()).padStart(2, "0")}T${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}:${String(now.getSeconds()).padStart(2, "0")}.${String(now.getMilliseconds()).padStart(3, "0")}${offset}`;
}

function redact(value) {
  if (Array.isArray(value)) return value.map((item) => redact(item));
  if (value && typeof value === "object") {
    const out = {};
    Object.entries(value).forEach(([key, item]) => {
      const lower = key.toLowerCase();
      out[key] = SENSITIVE_KEY_FRAGMENTS.some((fragment) => lower.includes(fragment))
        ? "[REDACTED]"
        : redact(item);
    });
    return out;
  }
  if (typeof value === "string") {
    const trimmed = value.trim();
    if ((trimmed.startsWith("{") && trimmed.endsWith("}")) || (trimmed.startsWith("[") && trimmed.endsWith("]"))) {
      try {
        return redact(JSON.parse(trimmed));
      } catch (_) {
        return value;
      }
    }
  }
  return value;
}

function stableJson(value) {
  return JSON.stringify(value, (_key, val) => {
    if (val && typeof val === "object" && !Array.isArray(val)) {
      return Object.keys(val).sort().reduce((acc, key) => {
        acc[key] = val[key];
        return acc;
      }, {});
    }
    return val;
  });
}

function sha256Text(text) {
  return `sha256:${crypto.createHash("sha256").update(text).digest("hex")}`;
}

function bounded(value, maxBytes = null) {
  if (maxBytes === null) maxBytes = maxLogFieldBytes();
  const text = stableJson(value);
  const bytes = Buffer.byteLength(text);
  if (bytes <= maxBytes) return { value, truncated: false, bytes, hash: null };
  return {
    value: {
      truncated: true,
      original_bytes: bytes,
      sha256: sha256Text(text),
      preview: Buffer.from(text).subarray(0, maxBytes).toString("utf8"),
    },
    truncated: true,
    bytes,
    hash: sha256Text(text),
  };
}

function retrySignature(toolName, args) {
  return sha256Text(stableJson({ tool: toolName, arguments: redact(args) }));
}

function makeLogId(prefix, payload) {
  return `${prefix}-${crypto.createHash("sha256").update(payload).digest("hex").slice(0, 32)}`;
}

function getProcessInstanceId() {
  if (!processInstanceId) processInstanceId = makeLogId("proc", `proxy-node:${process.pid}:${localIsoNow()}`);
  return processInstanceId;
}

function toolNamespaceAction(toolName, args) {
  const payload = args && typeof args === "object" ? args : {};
  if (toolName.startsWith("monolith_")) return ["monolith", toolName.slice("monolith_".length)];
  if (toolName.endsWith("_query")) return [toolName.slice(0, -"_query".length), String(payload.action || "")];
  return ["", ""];
}

function namespaceSource(toolName) {
  if (toolName.startsWith("monolith_")) return "core_tool";
  if (toolName.endsWith("_query")) return "domain_query";
  return "unknown";
}

function inferIntent(namespace, action, outcome) {
  const ns = namespace.toLowerCase();
  const act = action.toLowerCase();
  if (ns === "monolith" && ["find", "discover"].includes(act)) return ["schema_discovery", "high"];
  if (["health", "status", "check", "validate", "test"].some((token) => act.includes(token))) return ["verification", "medium"];
  if (ns === "source" || ["source", "symbol", "reference", "caller", "callee"].some((token) => act.includes(token))) return ["source_lookup", "medium"];
  if (ns === "project" || ns === "asset" || act.includes("asset")) return ["asset_search", "medium"];
  if (ns === "editor" && ["build", "compile", "log", "crash"].some((token) => act.includes(token))) return ["build_diagnostics", "medium"];
  if (["repair", "reindex", "rebuild", "snapshot"].some((token) => act.includes(token))) return ["maintenance", "medium"];
  if (outcome !== "success") return ["error_recovery", "low"];
  if (["create", "set", "add", "remove", "delete", "import", "build"].some((prefix) => act.startsWith(prefix))) return ["mutation", "medium"];
  return ["unknown", "low"];
}

function workflowStep(intent, outcome) {
  if (outcome !== "success") return "recover";
  if (intent === "schema_discovery") return "discover";
  if (intent === "verification") return "verify";
  if (intent === "maintenance") return "maintenance";
  if (intent === "mutation") return "execute";
  if (["source_lookup", "asset_search", "build_diagnostics"].includes(intent)) return "inspect";
  return "unknown";
}

function buildRoutingContext(toolName, args, signature, repeated, outcome, namespace, action, intent, confidence) {
  let decisionSource = "direct";
  let discoveryRootRecordId = null;
  let matchedDiscoveredAction = false;
  if (repeated) {
    const previous = recentToolLogSignatures.get(signature);
    decisionSource = previous && previous.failed ? "retry_after_error" : "fallback";
  } else if (recentDiscover) {
    if (recentDiscover.namespace && recentDiscover.namespace === namespace && (!recentDiscover.action || recentDiscover.action === action)) {
      decisionSource = "after_discover";
      matchedDiscoveredAction = true;
      discoveryRootRecordId = recentDiscover.record_id;
    }
  } else if (recentFind && toolName !== "monolith_find" && toolName !== "monolith_discover") {
    decisionSource = "after_find";
    discoveryRootRecordId = recentFind.record_id;
  }
  return dropEmpty({
    decision_source: decisionSource,
    namespace_source: namespaceSource(toolName),
    recent_find_trace_id: recentFind ? recentFind.trace_id : null,
    recent_discover_trace_id: recentDiscover ? recentDiscover.trace_id : null,
    matched_discovered_action: matchedDiscoveredAction,
    inferred_intent: intent,
    intent_confidence: confidence,
    discovery_root_record_id: discoveryRootRecordId,
  });
}

function rememberToolOutcome(toolName, args, traceId, recordId, signature, now, failed) {
  if (!recordId) return;
  const [namespace, action] = toolNamespaceAction(toolName, args);
  recentToolLogSignatures.set(signature, { at: now, failed, record_id: recordId });
  if (failed) lastErrorRecordId = recordId;
  else if (lastErrorRecordId) lastErrorRecordId = null;

  if (toolName === "monolith_find") {
    recentFind = { trace_id: traceId, record_id: recordId };
  } else if (toolName === "monolith_discover") {
    recentDiscover = {
      trace_id: traceId,
      record_id: recordId,
      namespace: String((args && args.namespace) || ""),
      action: String((args && args.action) || ""),
    };
  } else if (namespace && action && recentDiscover) {
    if (recentDiscover.namespace === namespace && (!recentDiscover.action || recentDiscover.action === action)) {
      recentDiscover = null;
    }
  }
}

function withTrace(message, traceId, parentSpanId, routingContext, sessionKey) {
  return {
    ...message,
    _monolith_trace_id: traceId,
    _monolith_parent_span_id: parentSpanId,
    _monolith_session_key: sessionKey,
    _monolith_routing_context: routingContext,
  };
}

function dropEmpty(value) {
  if (Array.isArray(value)) {
    return value
      .map((item) => dropEmpty(item))
      .filter((item) => item !== null && item !== "" && !(Array.isArray(item) && item.length === 0) && !(item && typeof item === "object" && !Array.isArray(item) && Object.keys(item).length === 0));
  }
  if (value && typeof value === "object") {
    return Object.fromEntries(Object.entries(value)
      .map(([key, item]) => [key, dropEmpty(item)])
      .filter(([_key, item]) => item !== null && item !== "" && !(Array.isArray(item) && item.length === 0) && !(item && typeof item === "object" && !Array.isArray(item) && Object.keys(item).length === 0)));
  }
  return value;
}

function parseResponse(response) {
  try {
    return JSON.parse(response);
  } catch (_) {
    return response;
  }
}

function classifyResponse(responseObj, repeated, durationMs, argBytes, resultBytes) {
  let outcome = "unknown";
  let errorClass = "";
  let errorCode = null;
  const tags = [];

  if (responseObj && typeof responseObj === "object" && Object.prototype.hasOwnProperty.call(responseObj, "error")) {
    outcome = "jsonrpc_error";
    const error = responseObj.error || {};
    errorCode = typeof error.code === "number" ? error.code : null;
    const message = String(error.message || error);
    const lower = message.toLowerCase();
    if (lower.includes("unknown action") || lower.includes("method not found")) {
      errorClass = "unknown_action";
      tags.push("missing_action");
    } else if (lower.includes("missing required") || lower.includes("invalid param")) {
      errorClass = "missing_param";
      tags.push("schema_confusing");
    } else {
      errorClass = "jsonrpc_error";
    }
  } else if (responseObj && typeof responseObj === "object" && responseObj.result && responseObj.result.isError) {
    outcome = "tool_error";
    const message = JSON.stringify(responseObj.result.content || []);
    const lower = message.toLowerCase();
    if (lower.includes("not available") || lower.includes("not running") || lower.includes("unreachable")) {
      outcome = "editor_unavailable";
      errorClass = "editor_unavailable";
      tags.push("editor_unavailable");
    } else if (lower.includes("blocked")) {
      errorClass = "profile_blocked";
      tags.push("profile_blocked");
    } else {
      errorClass = "tool_error";
    }
  } else {
    outcome = "success";
  }

  if (repeated) tags.push("repeated_call");
  if (durationMs > 5000) tags.push("slow_action");
  const maxFieldBytes = maxLogFieldBytes();
  if (argBytes > maxFieldBytes || resultBytes > maxFieldBytes) tags.push("large_result");

  return { outcome, errorClass, errorCode, tags: [...new Set(tags)].sort() };
}

function summarizeResponse(responseObj, resultBytes, truncated) {
  const summary = {
    result_bytes: resultBytes,
    truncated,
  };
  if (responseObj && typeof responseObj === "object") {
    if (Object.prototype.hasOwnProperty.call(responseObj, "error")) summary.result_shape = "error";
    else if (responseObj.result && typeof responseObj.result === "object" && Array.isArray(responseObj.result.content)) summary.result_shape = "mcp_content";
    else if (responseObj.result && typeof responseObj.result === "object") summary.result_shape = "object";
    else if (Array.isArray(responseObj.result)) summary.result_shape = "list";
    else if (responseObj.result === undefined || responseObj.result === null) summary.result_shape = "empty";
    else summary.result_shape = "text";
    summary.response_top_keys = Object.keys(responseObj).sort().slice(0, 20);
    if (Object.prototype.hasOwnProperty.call(responseObj, "error")) {
      const error = responseObj.error || {};
      if (error && typeof error === "object") {
        summary.jsonrpc_error_code = error.code;
        summary.jsonrpc_error_message = String(error.message || "").slice(0, 240);
      } else {
        summary.jsonrpc_error_message = String(error).slice(0, 240);
      }
    }
    const result = responseObj.result;
    if (result && typeof result === "object") {
      summary.result_top_keys = Object.keys(result).sort().slice(0, 20);
      if (Object.prototype.hasOwnProperty.call(result, "isError")) summary.is_error = Boolean(result.isError);
      if (Array.isArray(result.content)) summary.content_count = result.content.length;
      if (Array.isArray(result.tools)) summary.tools_count = result.tools.length;
      if (Array.isArray(result.resources)) summary.resources_count = result.resources.length;
    } else if (result !== undefined) {
      summary.result_type = typeof result;
    }
  } else {
    summary.response_type = typeof responseObj;
  }
  return dropEmpty(summary);
}

function appendToolLog(record) {
  if (!toolLogEnabled()) return;
  try {
    const file = dailyLogPath();
    fs.mkdirSync(path.dirname(file), { recursive: true });
    const lockFile = `${file}.lock`;
    let lockFd = null;
    const start = Date.now();
    for (;;) {
      try {
        lockFd = fs.openSync(lockFile, "wx");
        break;
      } catch (error) {
        if (error.code !== "EEXIST") throw error;
        try {
          const stat = fs.statSync(lockFile);
          if (Date.now() - stat.mtimeMs > 30000) fs.unlinkSync(lockFile);
        } catch (_) {
          // Race with another process removing the stale lock; retry.
        }
        if (Date.now() - start > 5000) throw new Error("timed out acquiring tool log lock");
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 25);
      }
    }
    try {
      fs.appendFileSync(file, `${JSON.stringify(dropEmpty(record))}\n`, { encoding: "utf8" });
    } finally {
      if (lockFd !== null) fs.closeSync(lockFd);
      try {
        fs.unlinkSync(lockFile);
      } catch (_) {
        // Best effort cleanup only; a later writer can retire stale locks.
      }
    }
  } catch (error) {
    log(`Tool daily log failed: ${error.message}`);
  }
}

function logToolsCall(message, startTime, startHr, response, repeated, signature, traceId, spanId, phaseInput = {}) {
  if (!toolLogEnabled()) return null;

  const logPrepareStart = process.hrtime.bigint();
  const diff = process.hrtime.bigint() - startHr;
  const durationMs = Number(diff) / 1e6;
  const params = message.params && typeof message.params === "object" ? message.params : {};
  const toolName = params.name || "unknown";
  const args = params.arguments || {};
  const responseObj = parseResponse(response);
  const boundedArgs = bounded(redact(args));
  const boundedResponse = bounded(redact(responseObj));
  const classification = classifyResponse(responseObj, repeated, durationMs, boundedArgs.bytes, boundedResponse.bytes);
  const [namespace, action] = toolNamespaceAction(toolName, args);
  const [intent, confidence] = inferIntent(namespace, action, classification.outcome);
  const returnSummary = summarizeResponse(responseObj, boundedResponse.bytes, boundedArgs.truncated || boundedResponse.truncated);

  const redaction = {
    argument_bytes: boundedArgs.bytes,
    result_bytes: boundedResponse.bytes,
  };
  if (boundedArgs.truncated || boundedResponse.truncated) redaction.truncated = true;
  if (boundedArgs.hash) redaction.argument_sha256 = boundedArgs.hash;
  if (boundedResponse.hash) redaction.result_sha256 = boundedResponse.hash;

  const agentSignal = {
    outcome: classification.outcome,
    hints_returned: 0,
  };
  if (classification.errorCode !== null && classification.errorCode !== undefined) agentSignal.error_code = classification.errorCode;
  if (classification.errorClass) agentSignal.error_class = classification.errorClass;
  if (repeated) agentSignal.repeat_within_window = true;
  if (classification.tags.length) agentSignal.improvement_tags = classification.tags;

  const routingContext = buildRoutingContext(toolName, args, signature, repeated, classification.outcome, namespace, action, intent, confidence);
  const workflow = {
    step: workflowStep(intent, classification.outcome),
  };
  const previousSignature = recentToolLogSignatures.get(signature);
  if (previousSignature && previousSignature.record_id && repeated) workflow.retry_of_record_id = previousSignature.record_id;
  if (lastErrorRecordId && classification.outcome === "success") workflow.recovery_from_record_id = lastErrorRecordId;
  if (routingContext.discovery_root_record_id) workflow.discovery_root_record_id = routingContext.discovery_root_record_id;

  const returnRecord = {
    response: boundedResponse.value,
  };
  const responseId = responseObj && typeof responseObj === "object" ? responseObj.id : null;
  if (responseId !== message.id) returnRecord.jsonrpc_id = responseId;

  toolLogSequence += 1;
  const procId = getProcessInstanceId();
  const recordId = makeLogId("rec", `${procId}:proxy:${toolLogSequence}:${traceId}:${spanId}:${startTime}`);
  const previousRecordId = lastRecordId;
  const timeSincePreviousMs = lastRecordStartMs !== null ? Number(startHr - lastRecordStartMs) / 1e6 : null;
  lastRecordId = recordId;
  lastRecordStartMs = startHr;
  const phaseTiming = dropEmpty({
    parse_ms: phaseInput.parseMs,
    rewrite_ms: phaseInput.rewriteMs,
    dedup_ms: phaseInput.dedupMs,
    http_roundtrip_ms: phaseInput.httpRoundtripMs,
    fallback_ms: phaseInput.fallbackMs,
    log_prepare_ms: Number(process.hrtime.bigint() - logPrepareStart) / 1e6,
  });
  appendToolLog({
    format_version: 3,
    surface: "proxy",
    record_id: recordId,
    sequence: toolLogSequence,
    trace_id: traceId,
    span_id: spanId,
    session_key: "stateless",
    process_instance_id: procId,
    call_index: toolLogSequence,
    previous_record_id: previousRecordId,
    time_since_previous_ms: timeSincePreviousMs,
    start_time: startTime,
    end_time: localIsoNow(),
    duration_ms: Math.round(durationMs * 1000) / 1000,
    pid: process.pid,
    thread_id: "main",
    status: classification.outcome === "success" ? "success" : "error",
    client: {
      proxy_runtime: "node",
      proxy_version: PROXY_VERSION,
    },
    routing_context: routingContext,
    workflow,
    phase_timing: phaseTiming,
    call: {
      jsonrpc_id: message.id,
      tool_name_original: toolName,
      tool_name_forwarded: toolName,
      arguments: boundedArgs.value,
      retry_signature: signature,
    },
    return: returnRecord,
    return_summary: returnSummary,
    redaction,
    agent_signal: agentSignal,
  });
  return recordId;
}

function write(message) {
  process.stdout.write(`${message}\n`);
}

function result(id, payload) {
  return JSON.stringify({ jsonrpc: "2.0", id, result: payload });
}

function toolError(id, message) {
  return JSON.stringify({
    jsonrpc: "2.0",
    id,
    result: {
      content: [{ type: "text", text: message }],
      isError: true,
    },
  });
}

function jsonrpcError(id, code, message) {
  return JSON.stringify({
    jsonrpc: "2.0",
    id,
    error: { code, message },
  });
}

function requestText(urlText, options, body, errOut) {
  return new Promise((resolve) => {
    let url;
    try {
      url = new URL(urlText);
    } catch (error) {
      if (errOut) errOut.code = "EBADURL";
      log(`Bad URL ${urlText}: ${error.message}`);
      resolve(null);
      return;
    }

    const transport = url.protocol === "https:" ? https : http;
    const req = transport.request(
      {
        protocol: url.protocol,
        hostname: url.hostname,
        port: url.port || (url.protocol === "https:" ? 443 : 80),
        path: `${url.pathname}${url.search}`,
        method: options.method || "GET",
        headers: options.headers || {},
        timeout: options.timeout || TIMEOUT_MS,
      },
      (res) => {
        const chunks = [];
        res.setEncoding("utf8");
        res.on("data", (chunk) => chunks.push(chunk));
        res.on("end", () => {
          if (res.statusCode && res.statusCode >= 200 && res.statusCode < 300) {
            resolve(chunks.join(""));
          } else {
            if (errOut) errOut.code = "EHTTPSTATUS";
            log(`HTTP ${res.statusCode} from ${urlText}`);
            resolve(null);
          }
        });
      },
    );

    let timedOut = false;
    req.on("timeout", () => {
      timedOut = true;
      req.destroy(new Error("timeout"));
    });
    req.on("error", (error) => {
      if (errOut) {
        errOut.code = error.code;
        errOut.timedOut = timedOut;
      }
      log(`Monolith unreachable: ${error.message}`);
      resolve(null);
    });
    if (body) req.write(body);
    req.end();
  });
}

// retry is enabled only for tools/call (real action forwarding where a transient drop loses
// work). tools/list and unknown-method forwards stay fast-fail because they have a cached/seed
// fallback — retrying there would only delay the offline path.
async function postMonolith(body, retry = false) {
  const deadline = Date.now() + CONNECT_RETRY_BUDGET_MS;
  for (let attempt = 0; ; attempt++) {
    const errOut = {};
    const resp = await requestText(
      MONOLITH_URL,
      {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "Content-Length": Buffer.byteLength(body),
        },
        timeout: TIMEOUT_MS,
      },
      body,
      errOut,
    );
    if (resp !== null) return resp;
    const retryable = !errOut.timedOut && RETRYABLE_CONNECT_CODES.has(errOut.code);
    if (retry && attempt < CONNECT_RETRIES && Date.now() < deadline && retryable) {
      log(`Monolith transient connection failure (attempt ${attempt + 1}/${CONNECT_RETRIES}), retrying`);
      await new Promise((r) => setTimeout(r, CONNECT_RETRY_BACKOFF_MS * (attempt + 1)));
      continue;
    }
    return null;
  }
}

async function checkMonolithUp() {
  const response = await requestText(MONOLITH_HEALTH, { method: "GET", timeout: 3000 });
  return response !== null;
}

function makeTool(name, description, schema) {
  return { name, description, inputSchema: schema };
}

function sanitizeCachePart(value) {
  return value.replace(/[^a-zA-Z0-9-_]/g, "_");
}

function getToolsCachePath() {
  const base = process.platform === "win32"
    ? (process.env.LOCALAPPDATA || os.tmpdir())
    : (process.env.XDG_CACHE_HOME || path.join(os.homedir(), ".cache"));
  const cacheDir = path.join(base, "Monolith");
  fs.mkdirSync(cacheDir, { recursive: true });

  let hostPort = MONOLITH_HEALTH.replace(/^https?:\/\//, "");
  hostPort = hostPort.split("/")[0];
  return path.join(cacheDir, `monolith_proxy_tools_${sanitizeCachePart(hostPort)}.json`);
}

function writeToolsCache(respStr) {
  try {
    const payload = JSON.parse(respStr);
    const tools = payload?.result?.tools;
    if (Array.isArray(tools) && tools.length > 0) {
      fs.writeFileSync(getToolsCachePath(), JSON.stringify(tools), "utf8");
    }
  } catch (e) {
    log(`Failed to write tools/list cache: ${e}`);
  }
}

function readToolsCache() {
  try {
    const p = getToolsCachePath();
    if (!fs.existsSync(p)) return null;
    const tools = JSON.parse(fs.readFileSync(p, "utf8"));
    if (Array.isArray(tools) && tools.length > 0) {
      return tools;
    }
  } catch (e) {
    log(`Failed to read tools/list cache: ${e}`);
  }
  return null;
}

function queryToolSchema() {
  return {
    type: "object",
    properties: {
      action: {
        type: "string",
        description: "The action to execute. Use monolith_discover first when the editor is available.",
      },
      params: {
        type: "object",
        description: "Parameters for the selected action.",
      },
      _fields: {
        type: "array",
        items: { type: "string" },
        description: "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
      },
      _omit: {
        type: "array",
        items: { type: "string" },
        description: "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
      },
      _compact_json: {
        type: "boolean",
        description: "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
      },
    },
    required: ["action"],
  };
}

function emptyObjectSchema() {
  return {
    type: "object",
    properties: {
      _fields: {
        type: "array",
        items: { type: "string" },
        description: "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
      },
      _omit: {
        type: "array",
        items: { type: "string" },
        description: "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
      },
      _compact_json: {
        type: "boolean",
        description: "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
      },
    },
  };
}

function injectMonolithQuery(responseStr) {
  try {
    const payload = JSON.parse(responseStr);
    const tools = payload && payload.result && Array.isArray(payload.result.tools)
      ? payload.result.tools
      : null;
    if (!tools) return responseStr;
    if (tools.some((t) => t.name === "monolith_query")) return responseStr;
    tools.push(makeTool("monolith_query",
      "Execute any Monolith action. Use monolith_find(query) to locate the right action, " +
      "monolith_discover(namespace) to inspect its schema, then call monolith_query with " +
      "the resolved namespace, action, and params.",
      {
        type: "object",
        properties: {
          namespace: {
            type: "string",
            description: "Target namespace, e.g. 'blueprint', 'source', 'gas'. " +
              "Call monolith_discover() with no args to list all available namespaces.",
          },
          action: {
            type: "string",
            description: "Action to execute. Call monolith_discover(namespace) for the " +
              "full action list and monolith_discover(namespace, action, mode='schema') " +
              "for exact parameter schemas.",
          },
          params: {
            type: "object",
            description: "Parameters for the action.",
          },
        },
        required: ["namespace", "action"],
      }));
    return JSON.stringify(payload);
  } catch (e) {
    log(`injectMonolithQuery parse error: ${e}`);
    return responseStr;
  }
}

function seedTools() {
  const tools = CORE_QUERY_TOOLS.map((name) => {
    const domain = name.endsWith("_query") ? name.slice(0, -6) : name;
    return makeTool(
      name,
      `Query the ${domain} domain. The editor may be offline at session start; retry after Monolith is healthy.`,
      queryToolSchema(),
    );
  });

  tools.push(makeTool("monolith_discover", "List available tool namespaces and their actions. Pass namespace and optional category to filter.", {
    type: "object",
    properties: {
      namespace: { type: "string", description: "Optional: filter to a specific namespace" },
      category: { type: "string", description: "Optional: filter actions within the namespace by category" },
      _fields: {
        type: "array",
        items: { type: "string" },
        description: "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
      },
      _omit: {
        type: "array",
        items: { type: "string" },
        description: "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
      },
      _compact_json: {
        type: "boolean",
        description: "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
      },
    },
  }));
  tools.push(makeTool("monolith_status", "Get Monolith server health: version, uptime, port, registered action count, and module status.", emptyObjectSchema()));
  tools.push(makeTool("monolith_update", "Check for or install Monolith updates from GitHub Releases.", {
    type: "object",
    properties: {
      action: {
        type: "string",
        description: "'check' to compare versions, 'install' to download and stage update",
        default: "check",
      },
      _fields: {
        type: "array",
        items: { type: "string" },
        description: "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
      },
      _omit: {
        type: "array",
        items: { type: "string" },
        description: "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
      },
      _compact_json: {
        type: "boolean",
        description: "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
      },
    },
  }));
  tools.push(makeTool("monolith_reindex", "Re-index the Monolith project database. Requires the editor-side Monolith server.", emptyObjectSchema()));
  tools.push(makeTool("monolith_query",
    "Execute any Monolith action. Use monolith_find(query) to locate the right action, " +
    "monolith_discover(namespace) to inspect its schema, then call monolith_query with " +
    "the resolved namespace, action, and params.",
    {
      type: "object",
      properties: {
        namespace: {
          type: "string",
          description: "Target namespace, e.g. 'blueprint', 'source', 'gas'. " +
            "Call monolith_discover() with no args to list all available namespaces.",
        },
        action: {
          type: "string",
          description: "Action to execute. Call monolith_discover(namespace) for the " +
            "full action list and monolith_discover(namespace, action, mode='schema') " +
            "for exact parameter schemas.",
        },
        params: {
          type: "object",
          description: "Parameters for the action.",
        },
      },
      required: ["namespace", "action"],
    }));
  return tools;
}

async function checkMonolithStateChange() {
  const isUp = await checkMonolithUp();
  if (monolithWasUp !== null && isUp !== monolithWasUp) {
    const direction = isUp ? "online" : "offline";
    log(`Monolith went ${direction} - sending tools/list_changed`);
    write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/tools/list_changed" }));
  }
  monolithWasUp = isUp;
}

function startHealthPoll() {
  const startTimer = setTimeout(() => {
    log(`Health poll started (interval=${POLL_INTERVAL_MS / 1000}s)`);
    const interval = setInterval(() => {
      checkMonolithStateChange().catch((error) => log(`Health poll error: ${error.message}`));
    }, POLL_INTERVAL_MS);
    interval.unref();
  }, POLL_START_DELAY_MS);
  startTimer.unref();
}

function handleInitialize(message) {
  const clientVersion = message.params && message.params.protocolVersion
    ? message.params.protocolVersion
    : "2025-11-25";
  const supported = new Set(["2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"]);
  const protocolVersion = supported.has(clientVersion) ? clientVersion : "2025-11-25";
  return result(message.id, {
    protocolVersion,
    capabilities: { tools: { listChanged: true } },
    serverInfo: { name: PROXY_NAME, version: PROXY_VERSION },
    instructions:
      "Monolith MCP proxy for Unreal Engine. Tools forward to the Unreal Editor.\n" +
      "\n" +
      "ROUTING:\n" +
      "  monolith_find(query)                            — find the right action\n" +
      "  monolith_discover()                             — list all namespaces\n" +
      "  monolith_discover(namespace)                    — list actions in a namespace\n" +
      "  monolith_discover(namespace, action, 'schema')  — fetch exact param schema\n" +
      "  monolith_query({namespace, action, params})     — execute any action\n" +
      "\n" +
      "SKILL LOADING: domain skills live in Skills/<namespace>/SKILL.md and document\n" +
      "available actions and params for that namespace.\n" +
      "\n" +
      "EDITOR OFFLINE: offline fallback is disabled for this proxy process, so editor transport failures remain unavailable errors.\n" +
      "Before calling a domain action, check its schema instead of guessing. " +
      "monolith_discover() lists namespaces and monolith_discover(namespace='<namespace>', " +
      "mode='actions') lists actions; monolith_discover(namespace='<namespace>', action='<action>', mode='schema') fetches the exact live schema. " +
      "Offline schema mode returns explicitly degraded catalog guidance rather than fabricating a live JSON schema. " +
      "If an editor-only tool returns a transport-unavailable error, run Scripts/recover_mcp.ps1, wait for the configured endpoint, and retry.",
  });
}

function fallbackToolsList(message) {
  const cached = readToolsCache();
  if (cached) {
    log("Monolith down during tools/list - returning cached tools");
    return result(message.id, { tools: cached });
  }
  log("Monolith down during tools/list - returning seed tools");
  return result(message.id, { tools: seedTools() });
}

async function handleToolsList(message) {
  const response = await postMonolith(JSON.stringify(message));
  if (response) {
    writeToolsCache(response);
    return injectMonolithQuery(response);
  }
  return fallbackToolsList(message);
}

async function handleToolsCall(message) {
  const startTime = localIsoNow();
  const startHr = process.hrtime.bigint();
  const parseStart = process.hrtime.bigint();
  const params = message.params && typeof message.params === "object" ? message.params : {};
  const toolName = params.name || "unknown";
  const args = params.arguments || {};
  const parseMs = Number(process.hrtime.bigint() - parseStart) / 1e6;
  const dedupStart = process.hrtime.bigint();
  const signature = retrySignature(toolName, args);
  const now = Date.now();
  const previous = recentToolLogSignatures.get(signature);
  const repeated = Boolean(previous && now - previous.at <= REPEAT_LOG_WINDOW_MS);
  const dedupMs = Number(process.hrtime.bigint() - dedupStart) / 1e6;
  const rewriteStart = process.hrtime.bigint();
  const traceId = makeLogId("trace", `${startTime}:${process.pid}:main:${signature}`);
  const spanId = makeLogId("span", `${traceId}:proxy:${message.id}:${startTime}`);

  // monolith_query unified dispatcher: rewrite to the appropriate editor tool.
  // Domain namespaces use {ns}_query({action, params}) envelope.
  // The "monolith" namespace exposes tools directly as monolith_{action}, not via a query envelope.
  let msgToForward = message;
  if (toolName === "monolith_query") {
    const qNs = (typeof args.namespace === "string" ? args.namespace : "").trim();
    const qAction = (typeof args.action === "string" ? args.action : "").trim();
    if (qNs && qAction) {
      let forwardName, forwardArgs;
      if (qNs === "monolith") {
        // monolith_* tools are individual named tools — pass params directly as their arguments.
        forwardName = `monolith_${qAction}`;
        forwardArgs = args.params || {};
      } else {
        // All other namespaces use the {ns}_query(action, params) envelope.
        forwardName = `${qNs}_query`;
        forwardArgs = { action: qAction, params: args.params || {} };
      }
      msgToForward = Object.assign({}, message, {
        params: Object.assign({}, message.params, {
          name: forwardName,
          arguments: forwardArgs,
        }),
      });
      log(`monolith_query → ${forwardName}`);
    }
  }

  const [namespace, action] = toolNamespaceAction(toolName, args);
  const [intent, confidence] = inferIntent(namespace, action, "unknown");
  const routingContext = buildRoutingContext(toolName, args, signature, repeated, "unknown", namespace, action, intent, confidence);
  const forwardedMessage = withTrace(msgToForward, traceId, spanId, routingContext, "stateless");
  const rewriteMs = Number(process.hrtime.bigint() - rewriteStart) / 1e6;

  const httpStart = process.hrtime.bigint();
  const response = await postMonolith(JSON.stringify(forwardedMessage), true);
  const httpRoundtripMs = Number(process.hrtime.bigint() - httpStart) / 1e6;
  const phaseInput = { parseMs, dedupMs, rewriteMs, httpRoundtripMs };
  if (response) {
    const responseObj = parseResponse(response);
    const failed = Boolean(responseObj && typeof responseObj === "object" && (
      Object.prototype.hasOwnProperty.call(responseObj, "error") ||
      (responseObj.result && responseObj.result.isError)
    ));

    const recordId = logToolsCall(message, startTime, startHr, response, repeated, signature, traceId, spanId, phaseInput);
    rememberToolOutcome(toolName, args, traceId, recordId, signature, now, failed);
    return response;
  }

  const fallbackStart = process.hrtime.bigint();
  const fallback = toolError(
    message.id,
    `Monolith MCP is not available (Unreal Editor not running). Tool '${toolName}' cannot execute. Start the editor and try again.`,
  );
  phaseInput.fallbackMs = Number(process.hrtime.bigint() - fallbackStart) / 1e6;
  const recordId = logToolsCall(message, startTime, startHr, fallback, repeated, signature, traceId, spanId, phaseInput);
  rememberToolOutcome(toolName, args, traceId, recordId, signature, now, true);
  return fallback;
}

async function handleMessage(message) {
  const method = message.method || "";
  const hasId = Object.prototype.hasOwnProperty.call(message, "id") && message.id !== null;

  if (method === "initialize") {
    log("Initialized");
    return handleInitialize(message);
  }
  if (method === "notifications/initialized" || method === "initialized") {
    await checkMonolithStateChange();
    return null;
  }
  if (method === "ping") {
    return result(message.id, {});
  }
  if (method === "tools/list") {
    await checkMonolithStateChange();
    return handleToolsList(message);
  }
  if (method === "tools/call") {
    return handleToolsCall(message);
  }

  const forwarded = await postMonolith(JSON.stringify(message));
  if (forwarded) return forwarded;
  return hasId ? jsonrpcError(message.id, -32601, `Method not found: ${method}`) : null;
}

function printHelp() {
  process.stdout.write(`Usage:
  node monolith_proxy.js --help
  node monolith_proxy.js --version
  node monolith_proxy.js

Role:
  Stdio-to-HTTP MCP bridge for the editor-hosted Monolith server.
  Default target: MONOLITH_URL=http://localhost:9316/mcp

Environment:
  MONOLITH_URL                         Editor MCP endpoint.
  MONOLITH_TOOL_LOG_ENABLED            Set 0 to disable daily proxy logs.
  MONOLITH_TOOL_LOG_DIR                Redirect Logs/yyyyMMdd/proxy.jsonl.
  MONOLITH_TOOL_LOG_MAX_FIELD_BYTES    Bound captured log fields.
  LOCALAPPDATA / XDG_CACHE_HOME        Used for script-proxy tool cache fallback paths.

Runtime support notes:
  MONOLITH_SPLIT_EDITOR_QUERY, MONOLITH_EDITOR_ACTION_ALLOWLIST, and
  MONOLITH_EDITOR_ACTION_DENYLIST are native C++ proxy controls.
  MONOLITH_CALL_LOG and MONOLITH_PROJECT_ROOT control the native C++ proxy call log.

MCP config example:
  {"mcpServers":{"monolith":{"command":"<project-root>/Plugins/Monolith/Scripts/monolith_proxy.sh"}}} (or .bat on Windows)

Offline fallback:
  Use Binaries/monolith_query (or .exe on Windows) for read-only source/project/bridge/console queries when the editor or MCP server is unavailable.
`);
}

function printVersion() {
  process.stdout.write(`${JSON.stringify({ tool: PROXY_NAME, version: PROXY_VERSION, runtime: "node" }, null, 2)}\n`);
}

function handleHelpOrVersion() {
  for (const arg of process.argv.slice(2)) {
    if (arg === "--help" || arg === "-h" || arg === "help") {
      printHelp();
      return true;
    }
    if (arg === "--version" || arg === "-v" || arg === "version") {
      printVersion();
      return true;
    }
  }
  return false;
}

function main() {
  if (handleHelpOrVersion()) return;

  log(`Started. Forwarding to ${MONOLITH_URL}`);
  startHealthPoll();

  const rl = readline.createInterface({
    input: process.stdin,
    crlfDelay: Infinity,
    terminal: false,
  });

  rl.on("line", (line) => {
    const trimmed = line.trim();
    if (!trimmed) return;
    let message;
    try {
      message = JSON.parse(trimmed);
    } catch (error) {
      log(`Bad JSON: ${error.message}`);
      return;
    }
    pendingRequests += 1;
    handleMessage(message)
      .then((response) => {
        if (response) write(response);
      })
      .catch((error) => {
        log(`Request error: ${error.message}`);
        if (Object.prototype.hasOwnProperty.call(message, "id")) {
          write(jsonrpcError(message.id, -32603, error.message));
        }
      })
      .finally(() => {
        pendingRequests -= 1;
        maybeExit();
      });
  });

  rl.on("close", () => {
    stdinClosed = true;
    maybeExit();
  });
}

function maybeExit() {
  if (stdinClosed && pendingRequests === 0) {
    process.exit(0);
  }
}

main();
