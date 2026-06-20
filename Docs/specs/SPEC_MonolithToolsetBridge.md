# Monolith — MonolithToolsetBridge Module

Status: Scaffold
Date: 2026-06-21
Module: `MonolithToolsetBridge` (Editor)

---

## 1. Purpose

Optional, opt-in bridge to UE 5.8's Experimental `ToolsetRegistry` plugin (UnrealMCP gap spec M2). It exists so a source/dev build on an engine that ships `ToolsetRegistry` can later expose registry-registered toolsets to Monolith discovery — without ever making `ToolsetRegistry` a dependency of public Monolith builds.

This is a **scaffold**: it establishes the gated module boundary only. Toolset enumeration and tool import into Monolith discovery (routed through `FMonolithToolProfileManager` and host/allowlist controls) are follow-up slices.

## 2. Build Boundary

| Gate | Owner | Default | Effect |
|------|-------|---------|--------|
| `MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE` | `MonolithToolsetBridge.Build.cs` | `0` | Compile guard. Set to `1` only when env `MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1` is set AND the `ToolsetRegistry` plugin is found. Otherwise the module compiles as an inert empty shell (deps: `Core`/`CoreUObject`/`Engine`/`MonolithCore`). |
| `bEnableToolsetRegistryBridge` | `UMonolithSettings` | `false` | Runtime opt-in, checked at `StartupModule`. |
| `MONOLITH_RELEASE_BUILD=1` | env | — | Forces the bridge off so binary releases never link `ToolsetRegistry`. |

UE 5.8 `ToolsetRegistry` is Experimental / NoRedist and is absent from the project's UE 5.7 build, so the default build always compiles the empty shell.

## 3. Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithToolsetBridgeModule` | `IModuleInterface`. When compiled in (`MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1`) and `bEnableToolsetRegistryBridge=true`, will host the ToolsetRegistry adapter (follow-up). Currently logs status only; inert otherwise. |

## 4. Verification

- Default build: module compiles as the empty shell and loads inert (`MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=0`).
- `monolith_status` / `monolith_discover`: action surface unchanged (the scaffold adds no MCP actions, so no action-count change).
- Source/dev build with `ToolsetRegistry` present + `MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1`: module links `ToolsetRegistry`.

## 5. Non-Goals

- No `ToolsetRegistry` dependency in public builds.
- No toolset import / external tool exposure until active profile, host allowlist, and redaction controls are applied (follow-up).
