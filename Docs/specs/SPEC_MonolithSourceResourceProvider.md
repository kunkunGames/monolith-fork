# Monolith Source Resource Provider (P3b)

**Parent:** [SPEC_MonolithMcpResources.md](SPEC_MonolithMcpResources.md)
**Engine:** Unreal Engine 5.7+
**Status:** Implemented slice
**Owner modules:** MonolithCore (provider seam), MonolithSource (concrete provider)
**Scope:** Add a per-namespace resource provider seam to `FMonolithResourceRegistry`, and one concrete provider that exposes indexed C++ source files as the read-only resource family `monolith://source/file/{path}`.
**Non-goals:** Arbitrary filesystem access, writable resources, per-row enumeration of the indexed file table, new settings flags, absolute-path exposure.

---

## 1. Motivation

The P3a resource registry serves only fixed, eagerly-registered descriptors (static doc text and eager blobs). Some resource families are dynamic and per-namespace: a stable URI scheme whose concrete instances are resolved on demand (for example, "read indexed source file X"). Enumerating every such instance up front would be unbounded and expensive. P3b adds a provider seam so a namespace module can advertise a bounded URI family and resolve concrete reads lazily, without bloating the static descriptor map.

---

## 2. Provider Seam Contract (MonolithCore)

`IMonolithResourceProvider` (`Source/MonolithCore/Public/IMonolithResourceProvider.h`):

| Method | Contract |
|--------|----------|
| `ListResources(TArray<FMonolithResourceDescriptor>&)` | Append ONE (or a few) stable TEMPLATE descriptors. MUST NOT perform an unbounded scan. |
| `ReadResource(const FString& Uri, FMonolithResourceReadResult&)` | Return `true` only when the provider owns the URI scheme. Populate `bFound`, `Uri`, `MimeType`, and either `Text` or `BlobBytes`+`bBinary`. Return `false` for a URI the provider does not own so the registry tries the next provider. |

`FMonolithResourceRegistry` additions:

| Method | Contract |
|--------|----------|
| `RegisterProvider(const TSharedRef<IMonolithResourceProvider>&)` | Register a provider (idempotent via `AddUnique`). Code-only; no caller-driven path. |
| `UnregisterProvider(const TSharedRef<IMonolithResourceProvider>&)` | Remove a registered provider; no-op if absent. |
| `GetProviderCount() const` | Number of registered providers. |

Ordering and safety:

1. `ReadResource` consults providers ONLY AFTER the static-map and eager-blob branches miss, so a provider never shadows an explicitly registered resource.
2. `ListResourcesJson` appends provider template descriptors into the same sorted URI space as the static descriptors; a static URI wins on collision. Pagination stays deterministic.
3. The registry copies the provider `TSharedRef` array under `ResourceLock`, then invokes `ListResources` / `ReadResource` OUTSIDE the lock, so a provider read (DB or filesystem) cannot stall concurrent registry access or deadlock.
4. `ResetForTests()` clears providers as well as static resources.

---

## 3. Source Provider (MonolithSource)

`FMonolithSourceResourceProvider` (`Source/MonolithSource/Private/MonolithSourceResourceProvider.{h,cpp}`):

| Aspect | Value |
|--------|-------|
| URI scheme prefix | `monolith://source/file/` |
| Template URI | `monolith://source/file/{path}` |
| MIME type | `text/plain` |
| `ListResources` | Exactly ONE template descriptor; never enumerates the indexed file table. |
| `ReadResource` | Disowns (`false`) any URI not under the prefix. For an owned URI, resolves `{path}` through the shared hardened read and returns a line-numbered slice with the same `--- <ShortPath> (lines X-Y) ---` header the `source.read_file` action emits. |

Registration: `FMonolithSourceModule::StartupModule` creates and registers the provider only when `UMonolithSettings::bEnableMcpResources` is true (the P3a gate is REUSED; no new flag). `ShutdownModule` unregisters it.

---

## 4. Shared Hardened Read

`FMonolithSourceActions::ResolveAndReadFile(DB, RequestedPath, StartLine, EndLine, DefaultWindow)` is the single resolve+read path shared by the `source.read_file` action and the source resource provider. It mirrors the previous `HandleReadFile` resolution order:

1. Absolute on-disk path (`FPaths::FileExists`).
2. DB exact-path match (`FindFileByPath`, separators normalized to backslash).
3. DB suffix match (`FindFileBySuffix`).

On a miss it returns `bResolved=false` with `ErrorClass="coverage_miss"`. The result struct (`FResolveReadResult`) carries ONLY a `ShortPath()`-form path and the line-numbered `Text`; it never carries an absolute path, so the provider cannot leak local filesystem layout to MCP clients. `HandleReadFile` now calls this helper and preserves its exact output JSON and `coverage_miss` error contract byte-for-byte.

---

## 5. Security And Privacy

1. Providers are explicit, code-registered services — not arbitrary caller-driven filesystem access.
2. The source provider resolves only against the engine source DB and on-disk existence, exactly as the existing `source.read_file` action does.
3. Reads return `ShortPath()`-relative text and headers; no absolute local path, environment value, or secret is exposed.
4. Content is bounded by the read window (`DefaultWindow` lines by default) and the registry's per-resource caps still apply to static resources.

---

## 6. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Template only | `FMonolithSourceResourceProvider::ListResources` returns exactly one descriptor with URI `monolith://source/file/{path}`. |
| URI ownership | A non-source URI returns `false`; the bare scheme and an unresolved owned path return `true` with `bFound=false`. |
| Registry fall-through | A registered provider's template appears in `ListResourcesJson`, and an owned URI read falls through the static-map miss to the provider. |
| Idempotent register | Registering the same provider twice keeps `GetProviderCount()==1`; unregister returns it to 0. |
| No absolute-path leak | Provider error/text for an unresolved owned read contains no absolute path. |
| Disabled by default | With `bEnableMcpResources=false`, the source module registers no provider and the registry is untouched. |
| Action parity | `source.read_file` output JSON and the `coverage_miss` error are unchanged after routing through `ResolveAndReadFile`. |

Tests: `Source/MonolithSource/Private/Tests/MonolithSourceResourceProviderTests.cpp` covers the template, ownership, registry fall-through, idempotent register/unregister, and absolute-path-leak gates.

---

## 7. Follow-up Slices

| Follow-up | Reason to defer |
|-----------|-----------------|
| Percent-decoding of `{path}` segments | Needs a shared URL-decode utility; current paths are passed verbatim and normalized by the hardened read. |
| Project-asset / log resource providers | Separate namespaces; land each with its own provider and tests. |
| Resource templates surface | Depends on MCP URI-template validation, deferred in SPEC_MonolithMcpResources.md §9. |
