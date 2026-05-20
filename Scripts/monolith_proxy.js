#!/usr/bin/env node
"use strict";

const http = require("http");
const https = require("https");
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
  const response = await postMonolith(JSON.stringify(message));
  if (response) return response;
  const toolName = message.params && message.params.name ? message.params.name : "unknown";
  return toolError(
    message.id,
    `Monolith MCP is not available (Unreal Editor not running). Tool '${toolName}' cannot execute. Start the editor and try again.`,
  );
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
