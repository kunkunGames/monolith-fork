---
name: monolith-schema
description: "Use for schema-first Monolith workflows: discover action schemas, inspect writable target shapes with describe, and apply validated dry-run bulk_fill updates. Triggers on describe, bulk fill, schema, dry run, reflection walker, strict write, set many properties."
---

# Monolith Schema Skill

Use this skill when the task is about learning the exact shape of an action or applying many reflected property updates safely.

## Namespaces

- `describe`: read-only schema and field-shape discovery.
- `bulk_fill`: reflected batch property writes.
- Related discovery: `monolith_find`, `monolith_discover`.

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
- Use domain builders such as `build_material_graph` or `build_ui_from_spec` when the target is a graph or authored structure; use `bulk_fill` for reflected properties.
