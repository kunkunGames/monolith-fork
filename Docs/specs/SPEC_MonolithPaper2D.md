# Monolith - MonolithPaper2D Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented

---

## 1. Purpose

`MonolithPaper2D` owns the `paper2d` namespace for optional Paper2D asset discovery. The module is intentionally dependency-light: it uses AssetRegistry metadata and module-status probes only, does not include Paper2D headers, does not load Paper2D assets, and does not require the Paper2D plugin at compile time.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithPaper2DModule` | Registers and unregisters the `paper2d` namespace. |
| `FMonolithPaper2DActions` | Implements read-only Paper2D status, asset listing, and single-asset metadata handlers. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, and parameter schema contracts. |
| `AssetRegistry` | Read-only asset discovery and bounded metadata rows. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `paper2d.get_status` | none | Reports Paper2D/Paper2DEditor module availability and supported Paper2D asset classes. |
| `paper2d.list_assets` | `package_path`?, `limit`? | Lists PaperSprite, PaperFlipbook, PaperTileSet, and PaperTileMap registry rows under `/Game`. |
| `paper2d.get_asset` | `asset_path`, `include_tags`?, `tag_limit`? | Returns one bounded Paper2D registry row and optional bounded tags under `/Game`. |

The single-asset response contract is documented in [SPEC_MonolithPaper2DAssetMetadata.md](SPEC_MonolithPaper2DAssetMetadata.md).

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Optional dependency | No Paper2D or Paper2DEditor include paths, module dependencies, class loads, or asset loads. |
| Path boundary | `package_path` and `asset_path` must be `/Game` or below `/Game/`. |
| Read-only behavior | Actions must not call `GetAsset()`, mutate packages, create transactions, save assets, or execute Paper2D editor code. |
| Output bounds | `limit` clamps to `1..500`; `tag_limit` clamps to `0..200`; long tag values are truncated. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithPaper2DModule::StartupModule` registers `paper2d.get_status`, `paper2d.list_assets`, and `paper2d.get_asset`. |
| Parameter guard | `FMonolithPaper2DAssetRejectsUnsafePathTest` rejects filesystem paths before AssetRegistry lookup. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from `GO.uproject`. |
