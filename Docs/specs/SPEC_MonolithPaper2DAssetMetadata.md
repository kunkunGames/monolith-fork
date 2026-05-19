# Monolith - Paper2D Asset Metadata Spec

**Parent:** [SPEC_MonolithMaterial.md](SPEC_MonolithMaterial.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented in `MonolithMaterial`

---

## 1. Goal

Expose a low-risk, high-signal Paper2D inspection action without introducing a hard Paper2D dependency. The action lets clients inspect one Paper2D asset by `/Game` package or object path using `AssetRegistry` metadata only.

---

## 2. API Contract

| Action | Namespace | Mode | Description |
|--------|-----------|------|-------------|
| `get_paper2d_asset` | `material` | read-only | Return bounded metadata for one PaperSprite, PaperFlipbook, PaperTileSet, or PaperTileMap asset under `/Game`. |

| Parameter | Type | Required | Default | Contract |
|-----------|------|----------|---------|----------|
| `asset_path` | string | yes | - | `/Game` package path or object path. Non-project paths are rejected before registry lookup. |
| `include_tags` | boolean | no | `true` | Include bounded AssetRegistry tags when true. |
| `tag_limit` | integer | no | `50` | Maximum returned tag rows, clamped to `0..200`. |

---

## 3. Response Shape

| Field | Type | Description |
|-------|------|-------------|
| `namespace` | string | Always `material`. |
| `domain` | string | Always `paper2d_discovery`. |
| `asset` | object | AssetRegistry row for the resolved Paper2D asset. |
| `asset.object_path` | string | Full object path from AssetRegistry. |
| `asset.package_name` | string | Package name, for example `/Game/Sprites/HeroSprite`. |
| `asset.package_path` | string | Package folder. |
| `asset.asset_name` | string | Asset name. |
| `asset.asset_class` | string | One of `PaperSprite`, `PaperFlipbook`, `PaperTileSet`, `PaperTileMap`. |
| `asset.asset_class_path` | string | Full class path reported by AssetRegistry. |
| `asset.loaded` | boolean | Whether the asset is already loaded. The action does not load it. |
| `tags` | array | Optional bounded tag rows sorted by tag name. |
| `tag_count` | integer | Total available registry tag count. |
| `returned_tag_count` | integer | Number of returned tag rows. |
| `tags_truncated` | boolean | True when more tags existed than returned. |

---

## 4. Safety

| Gate | Requirement |
|------|-------------|
| Project path guard | `asset_path` must be `/Game` or below `/Game/`. |
| Optional dependency | The implementation must not include Paper2D headers or load `Paper2D`/`Paper2DEditor`. |
| Read-only behavior | The action must not call `GetAsset()`, mutate packages, create transactions, or save assets. |
| Bounded output | Tag values are truncated for JSON readability and tag rows are capped. |
| Class guard | Non-Paper2D assets and unresolved paths return structured errors. |

---

## 5. Verification

| Gate | Expected Result |
|------|-----------------|
| Param guard automation | Non-`/Game` paths are rejected before lookup. |
| UE 5.7 compile | `MonolithMaterial` builds against the resolved project engine root. |
| Docs sync | `SPEC_MonolithMaterial.md`, `SPEC_CORE.md`, and `API_REFERENCE.md` reflect the new action count and contract. |
