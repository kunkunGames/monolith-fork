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

let monolithWasUp = null;
let pendingRequests = 0;
let stdinClosed = false;
let toolLogSequence = 0;
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
const MAX_LOG_FIELD_BYTES = 256 * 1024;
const REPEAT_LOG_WINDOW_MS = 15000;

const CORE_QUERY_TOOLS = [
  "blueprint_query",
  "material_query",
  "animation_query",
  "niagara_query",
  "editor_query",
  "config_query",
  "project_query",
  "source_query",
  "ui_query",
  "mesh_query",
  "gas_query",
  "combograph_query",
  "ai_query",
  "logicdriver_query",
  "audio_query",
  "level_sequence_query",
  "movie_render_query",
];

function log(message) {
  process.stderr.write(`[monolith-proxy] ${message}\n`);
}

function toolLogEnabled() {
  return process.env.MONOLITH_TOOL_LOG_ENABLED !== "0";
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
  return path.join(findLogRoot(), `${day}_proxy.log`);
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

function bounded(value, maxBytes = MAX_LOG_FIELD_BYTES) {
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
  if (argBytes > MAX_LOG_FIELD_BYTES || resultBytes > MAX_LOG_FIELD_BYTES) tags.push("large_result");

  return { outcome, errorClass, errorCode, tags: [...new Set(tags)].sort() };
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
      fs.appendFileSync(file, `${JSON.stringify(record)}\n`, { encoding: "utf8" });
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

function logToolsCall(message, startTime, startHr, response, repeated, signature) {
  if (!toolLogEnabled()) return;

  const diff = process.hrtime.bigint() - startHr;
  const durationMs = Number(diff) / 1e6;
  const params = message.params && typeof message.params === "object" ? message.params : {};
  const toolName = params.name || "unknown";
  const args = params.arguments || {};
  const responseObj = parseResponse(response);
  const boundedArgs = bounded(redact(args));
  const boundedResponse = bounded(redact(responseObj));
  const classification = classifyResponse(responseObj, repeated, durationMs, boundedArgs.bytes, boundedResponse.bytes);

  toolLogSequence += 1;
  appendToolLog({
    format_version: 1,
    surface: "proxy",
    sequence: toolLogSequence,
    start_time: startTime,
    end_time: localIsoNow(),
    duration_ms: Math.round(durationMs * 1000) / 1000,
    pid: process.pid,
    thread_id: "main",
    status: classification.outcome === "success" ? "success" : "error",
    client: {
      name: "unknown",
      version: "",
      protocol_version: "",
      proxy_runtime: "node",
      proxy_version: PROXY_VERSION,
    },
    call: {
      jsonrpc_id: message.id,
      tool_name_original: toolName,
      tool_name_forwarded: toolName,
      arguments: boundedArgs.value,
      retry_signature: signature,
    },
    return: {
      jsonrpc_id: responseObj && typeof responseObj === "object" ? responseObj.id : null,
      response: boundedResponse.value,
      result_bytes: boundedResponse.bytes,
    },
    redaction: {
      applied: true,
      truncated: boundedArgs.truncated || boundedResponse.truncated,
      argument_bytes: boundedArgs.bytes,
      result_bytes: boundedResponse.bytes,
      argument_sha256: boundedArgs.hash,
      result_sha256: boundedResponse.hash,
    },
    agent_signal: {
      outcome: classification.outcome,
      error_code: classification.errorCode,
      error_class: classification.errorClass,
      hints_returned: 0,
      discovery_context: "unknown",
      retry_signature: signature,
      repeat_within_window: repeated,
      argument_bytes: boundedArgs.bytes,
      result_bytes: boundedResponse.bytes,
      improvement_tags: classification.tags,
    },
  });
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

function requestText(urlText, options, body) {
  return new Promise((resolve) => {
    let url;
    try {
      url = new URL(urlText);
    } catch (error) {
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
            log(`HTTP ${res.statusCode} from ${urlText}`);
            resolve(null);
          }
        });
      },
    );

    req.on("timeout", () => {
      req.destroy(new Error("timeout"));
    });
    req.on("error", (error) => {
      log(`Monolith unreachable: ${error.message}`);
      resolve(null);
    });
    if (body) req.write(body);
    req.end();
  });
}

async function postMonolith(body) {
  return requestText(
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
  );
}

async function checkMonolithUp() {
  const response = await requestText(MONOLITH_HEALTH, { method: "GET", timeout: 3000 });
  return response !== null;
}

function makeTool(name, description, schema) {
  return { name, description, inputSchema: schema };
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
    },
    required: ["action"],
  };
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

  tools.push(makeTool("monolith_discover", "List available tool namespaces and their actions.", {
    type: "object",
    properties: {
      namespace: { type: "string", description: "Optional: filter to a specific namespace" },
      category: { type: "string", description: "Optional: filter actions within the namespace by category" },
    },
  }));
  tools.push(makeTool("monolith_status", "Get Monolith server health.", {
    type: "object",
    properties: {},
  }));
  tools.push(makeTool("monolith_update", "Check for or install Monolith updates from GitHub Releases.", {
    type: "object",
    properties: {
      action: {
        type: "string",
        description: "'check' to compare versions, 'install' to download and stage update",
        default: "check",
      },
    },
  }));
  tools.push(makeTool("monolith_reindex", "Re-index the Monolith project database.", {
    type: "object",
    properties: {},
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
    instructions: "Monolith MCP proxy. Tools are forwarded to the Unreal Editor. If tools return errors about the editor not running, wait and retry.",
  });
}

async function handleToolsList(message) {
  const response = await postMonolith(JSON.stringify(message));
  if (response) return response;
  log("Monolith down during tools/list - returning seed tools");
  return result(message.id, { tools: seedTools() });
}

async function handleToolsCall(message) {
  const startTime = localIsoNow();
  const startHr = process.hrtime.bigint();
  const params = message.params && typeof message.params === "object" ? message.params : {};
  const toolName = params.name || "unknown";
  const args = params.arguments || {};
  const signature = retrySignature(toolName, args);
  const now = Date.now();
  const previous = recentToolLogSignatures.get(signature);
  const repeated = Boolean(previous && now - previous.at <= REPEAT_LOG_WINDOW_MS);

  const response = await postMonolith(JSON.stringify(message));
  if (response) {
    const responseObj = parseResponse(response);
    const failed = Boolean(responseObj && typeof responseObj === "object" && (
      Object.prototype.hasOwnProperty.call(responseObj, "error") ||
      (responseObj.result && responseObj.result.isError)
    ));
    recentToolLogSignatures.set(signature, { at: now, failed });
    logToolsCall(message, startTime, startHr, response, repeated, signature);
    return response;
  }

  const fallback = toolError(
    message.id,
    `Monolith MCP is not available (Unreal Editor not running). Tool '${toolName}' cannot execute. Start the editor and try again.`,
  );
  recentToolLogSignatures.set(signature, { at: now, failed: true });
  logToolsCall(message, startTime, startHr, fallback, repeated, signature);
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

function main() {
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
