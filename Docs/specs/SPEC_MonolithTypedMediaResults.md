# Monolith Typed Media Result Blocks

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.8+
**Status:** Implemented first slice (utils + first adopter; emission gated dark by default)
**Owner module:** MonolithCore (with first adopter in MonolithImageGen)
**Scope:** Let MCP tool results carry typed image/audio content blocks alongside the always-present text block, so binary-producing actions (image generation, audio synthesis) can hand the bytes directly to MCP clients instead of only a JSON asset path. Emission is gated by `UMonolithSettings::bEnableTypedMediaResults` and is byte-identical to the prior contract when unused.
**Non-goals:** Resource-link content blocks (TODO), `resource` embedded blocks, audio production wiring beyond the type allow-list, changing any existing action's input/output JSON schema, emitting media when the gate flag is off, retrofitting every binary action (only `imagegen.generate_image` adopts in this slice).

---

## 1. ROI Queue Position

This slice sits in the MCP results stack next to structured tool results. It reuses the proven "empty-default slot → byte-identical when unused" pattern from the CC-05 hint slots (`RelatedActions` / `Hints` / `ErrorData`).

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| Structured tool results (`bEnableStructuredToolResults`) | Live (`=True` in `DefaultMonolith.ini`). | High: compact content + `structuredContent`. | Done earlier. |
| Typed media result blocks | This slice: `MediaBlocks` slot + emission in `BuildMcpToolResult`, first adopter `imagegen.generate_image`. | High: binary bytes reach the client without a second fetch. | This slice. |
| Resource-link / embedded-resource blocks | Deferred (TODO in emission loop). | Medium: large-asset referencing. | Follow-up. |
| Additional adopters (audio synthesis, SVG raster) | Deferred. | Medium: per-domain opt-in. | Follow-up. |

The flag is kept **separate** from `bEnableStructuredToolResults` on purpose: structured results are on by default, but media must stay dark until clients opt in.

---

## 2. Problem

Binary-producing actions today return only a JSON payload (asset path, dimensions, provenance). An MCP client that wants the actual image must do a second round trip. MCP defines typed `image`/`audio` content blocks for exactly this, but Monolith had no slot to carry them and no gated emission path.

| Question | Current state | Needed first slice |
|----------|---------------|--------------------|
| Where do media bytes live on a result? | Nowhere; only JSON. | `TArray<FMonolithToolContentBlock> MediaBlocks` on `FMonolithActionResult`. |
| Are existing responses affected? | N/A. | Empty slot → byte-identical content array. |
| What controls emission? | N/A. | `bEnableTypedMediaResults`, read at the HTTP call site only. |
| Which block types are allowed? | N/A. | `image` and `audio` only; everything else skipped. |
| Who adopts first? | N/A. | `imagegen.generate_image` under an opt-in `attach_image_block` param. |

---

## 3. First Slice Contract

- `FMonolithToolContentBlock { FString Type; FString MimeType; FString Base64Data; FString Audience; }` declared in `Source/MonolithCore/Public/MonolithToolRegistry.h` ahead of `FMonolithActionResult`.
- `FMonolithActionResult::MediaBlocks` is a `TArray<FMonolithToolContentBlock>`, empty by default, placed beside the CC-05 hint slots with the same "empty by default → byte-identical" rationale.
- `FMonolithToolResultUtils::BuildMcpToolResult` gains a third defaulted param `bool bEnableTypedMedia = false`.
- Media blocks are appended to `content[]` **after** the text block, **only** on success, **only** when `bEnableTypedMedia` is true and `MediaBlocks` is non-empty. The text block is always present.
- Emission restricts `Type` to `image` and `audio`; any other type (including a future `resource_link`) is skipped — the resource-link path is a documented TODO.
- The HTTP dispatcher (`FMonolithHttpServer`, `tools/call` path) passes `Settings && Settings->bEnableTypedMediaResults` as the third argument.

---

## 4. Content-Block Wire Shape

Each emitted media block mirrors the MCP content-block shape:

```json
{ "type": "image", "mimeType": "image/png", "data": "<base64>" }
```

Rules:

1. `type` is `image` or `audio` (the allow-list); other types are not emitted.
2. `mimeType` carries the concrete media type (e.g. `image/png`, `audio/wav`).
3. `data` is the base64 payload from `Base64Data`.
4. The text block remains `content[0]`; media blocks follow in `MediaBlocks` order.
5. `Audience` is reserved for MCP annotation metadata; this slice stores it on the struct but does not yet serialize it (no client consumes it). Wiring `annotations.audience` is a follow-up.

When `bEnableTypedMedia` is false or `MediaBlocks` is empty, `content[]` is exactly the prior single text block (success) — byte-identical.

---

## 5. First Adopter: `imagegen.generate_image`

`MonolithImageGen/Private/MonolithImageGenActions.cpp` `HandleGenerateImage` gains an **optional** `attach_image_block` boolean param (default `false`):

- When `false` (default), the result has no `MediaBlocks` → existing contract unchanged.
- When `true` and the import succeeded, the generated PNG (already base64 in `BytesB64`) is attached as one `image` block with `mimeType="image/png"`.
- Whether the block actually reaches the wire is still gated by `bEnableTypedMediaResults` at the HTTP layer. With the param on but the server flag off, the result carries the block but `BuildMcpToolResult` does not emit it. This two-key gate keeps media dark by default even if a caller opts in.

The param is documented in the `generate_image` schema as optional with default `false`.

---

## 6. Two-Key Gating Model

| `attach_image_block` (param) | `bEnableTypedMediaResults` (server) | Result content |
|------------------------------|--------------------------------------|----------------|
| false (default) | any | text only (byte-identical) |
| true | false (default) | text only; block present on struct but not emitted |
| true | true | text + image block |

The handler decides *whether to populate* the slot; the server decides *whether to serialize* it. Either key off → no media on the wire.

---

## 7. No-Mask / No-Fake and Contract Preservation

- Per §9 contract preservation: no existing action's input/output schema changes. `MediaBlocks` and `attach_image_block` are pure **additions** with empty/false defaults, so unused paths stay byte-identical (mirrors the CC-05 slot discipline at `MonolithToolRegistry.h`).
- The emission loop never fabricates a block: it copies exactly what the handler populated and silently skips disallowed types rather than coercing them. Resource-link is a TODO, not a fake `image`.

---

## 8. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Empty media byte-identical | `BuildMcpToolResult` with empty `MediaBlocks` produces one text content block whether `bEnableTypedMedia` is true or false. |
| Media gating | A populated image `MediaBlock` is suppressed when `bEnableTypedMedia` is false, and appended (after the text block) when true. |
| Allow-list | Only `image`/`audio` types emit; other types are skipped. |
| Adopter default | `imagegen.generate_image` without `attach_image_block` carries no `MediaBlocks`. |

Automation tests live at `Source/MonolithCore/Private/Tests/MonolithToolResultUtilsTests.cpp`
(`Monolith.Core.ToolResults.EmptyMediaByteIdentical`, `Monolith.Core.ToolResults.MediaBlockGating`).

---

## 9. Follow-up Slices

| Follow-up | Reason to defer | Gate |
|-----------|-----------------|------|
| Resource-link content blocks | Needs large-asset referencing design; currently a TODO in the emission loop. | `bEnableTypedMediaResults` |
| `annotations.audience` serialization | No client consumes it yet; struct field reserved. | `bEnableTypedMediaResults` |
| Audio-producing adopters | No audio-synthesis action emits bytes yet. | `bEnableTypedMediaResults` |
| Additional image adopters (SVG raster, ima2 path) | Per-domain opt-in; keep blast radius small. | `bEnableTypedMediaResults` |
