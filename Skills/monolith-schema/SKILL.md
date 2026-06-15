---
name: monolith-schema
description: Use for schema-first Monolith MCP WRITE workflows once you already know the target action - describe a writable target's shape, run validated dry-run bulk_fill mass property updates, and use the reflection walker for strict reflected writes. To find WHICH namespace/action to call or check server health, use monolith-mcp. Triggers on describe, bulk fill, schema, dry run, reflection walker, strict write, set many properties, batch edit properties, mass property update, writable fields, reflected write, describe action shape, write many fields.
---

# Monolith Schema Skill

Drives the `describe` and `bulk_fill` namespaces. Use this skill when you already know the target action and need to learn its exact writable shape, then apply many reflected property updates safely. This is the schema-first WRITE path, not a router.

## Discover first (avoid stale action names)

```text
monolith_discover({ "namespace": "describe" })
monolith_discover({ "namespace": "bulk_fill" })
monolith_discover({ "namespace": "<ns>", "action": "<action>", "mode": "schema" })
```

The live catalog is authoritative. The tables below are a snapshot — confirm names and params with `monolith_discover` before relying on them.

## When to use / Use a different skill for

- Use this skill when you already know the namespace/action and want its writable field tree or a validated bulk property update.
- Use **monolith-mcp** when you do NOT yet know which namespace/action to call, or need server status, health, version, action count, or reindex.
- Use **unreal-reflection-intel** when you need reflection-metadata intelligence (UCLASS/UPROPERTY lookup, replication audit, decision records, risk hotspots) rather than bulk property writes.

## Namespaces

- `describe`: read-only schema and field-shape discovery (the reflection walker for writable target shapes).
- `bulk_fill`: reflected batch property writes, strict by default.
- Related discovery: `monolith_find`, `monolith_discover` (routing lives in monolith-mcp).

## Action reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode` `schema` (or `describe.action_schema`).

### describe

| Action | Params (req* opt? =default) |
| --- | --- |
| `schema` | `target_namespace?` (alias `namespace`/`domain`; omit for registered-namespace guidance), `target?` (asset path or action name; omit/empty for namespace-level writable-shape summary) |
| `list_targets` | `target_namespace?` (alias `namespace`/`domain`; if `inventory_supported=false`, use `schema` instead) |
| `action_schema` | `target_namespace*` (alias `namespace`/`domain`), `target_action*` (alias `action`) |

### bulk_fill

| Action | Params (req* opt? =default) |
| --- | --- |
| `apply` [w] | `target_namespace*` (alias `namespace`; blueprint/gas/inventory/ui/ai/niagara/material/audio/mesh/animation/logicdriver/combograph), `target*` (asset path or adapter target id), `tree*` (nested JSON object), `dry_run=false`, `strict=false` |
| `list_namespaces` | (no params) |

## Workflow

1. Find the target asset or object with `project_query("search", ...)` or the owning domain query.
2. Read the action contract with `monolith_discover({ "namespace": "<ns>", "action": "<action>", "mode": "schema" })`.
3. For reflected property writes, read the writable field tree with `describe_query("schema", { "target_namespace": "<ns>", ... })`.
4. Prepare a minimal `bulk_fill_query("apply", ...)` payload with `dry_run=true`.
5. Review validation output, field paths, enum values, clamped ranges, and warnings.
6. Re-run with execute enabled only after the dry-run matches intent.
7. Verify with the owning namespace read action.

## Rules

- `describe` teaches the shape; `bulk_fill` writes the shape. Do not skip the read step.
- Do not infer writable field paths from `ProjectIndex.db` search rows. Search provenance is not a write schema.
- Prefer strict mode and small payloads for first writes.
- For authored graph/structure targets, prefer a domain builder over `bulk_fill`: `build_material_graph` is documented in **unreal-materials**. `build_ui_from_spec` is mentioned in **unreal-ui** prose but does NOT appear in that skill's action table, so confirm it with live `monolith_discover({ "namespace": "ui" })` before relying on it.
