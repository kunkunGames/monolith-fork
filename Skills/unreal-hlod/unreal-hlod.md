---
name: unreal-hlod
description: "Use for Hierarchical LOD (HLOD) and World Partition HLOD layers via Monolith MCP: configure HLOD layers, build/clear HLOD, and proxy/merged mesh setup. Triggers on HLOD, hierarchical LOD, proxy mesh, merged mesh, HLOD layer, world partition HLOD, build HLOD, instancing HLOD."
---

# unreal-hlod

**12 actions** via `hlod_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "hlod" })                      # all actions in this namespace
monolith_discover({ namespace: "hlod", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (12)

| Action | Purpose |
|--------|---------|
| `build_hlod` | Report HLOD build orchestration status. Does not launch long-running builds yet. |
| `check_hlod_hash` | Compute a lightweight HLOD readiness hash from layer and actor identity rows. |
| `clear_legacy_hlod` | Report legacy HLOD clear orchestration status. Does not clear generated actors yet. |
| `configure_hlod_layer` | Configure a HLOD layer through mesh.setup_hlod. |
| `create_hlod_layer` | Create/configure a HLOD layer through mesh.setup_hlod. |
| `export_hlod` | Export a bounded HLOD inspection report JSON file under Saved/Monolith/HLOD. |
| `get_hlod_layer` | Inspect a HLODLayer asset using AssetRegistry and reflection. |
| `get_hlod_stats` | Return HLOD layer and loaded actor counts for the current editor world. |
| `legacy_hlod_needs_build` | Return a conservative legacy-HLOD needs-build signal based on loaded HLOD actor presence. |
| `list_hlod_actors` | List loaded HLOD-like actors in the editor world. |
| `list_hlod_layers` | List HLODLayer assets under a package path. |
| `list_hlod_source_actors` | List loaded static mesh actors that are eligible source candidates for HLOD reports. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "hlod" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
