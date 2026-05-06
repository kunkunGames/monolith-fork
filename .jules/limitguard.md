## 2025-02-18 - Bound MonolithAudioQueryActions find_unattenuated_sounds results array
**Boundary:** `limit` param and `ResultsArray.Num()` on `find_unattenuated_sounds` action
**Learning:** `FindUnattenuatedSounds` traverses potentially tens of thousands of assets via the asset registry. An unbounded list risks returning a huge JSON array, which could consume significant memory and crash the VM/MCP if a project has huge numbers of missing attenuations.
**Prevention:** Bound queries iterating `AssetDataList` and returning large result sets with an optional `limit` parameter, defaulting to a conservative max like 100.
[blocked: UE 5.7 editor unavailable in Jules VM]
## 2026-05-05 - 🧱 LimitGuard: Bound audio/find_sounds_without_class limit
**Boundary:** `limit` query parameter upper bound and JSON type.
**Learning:** Raw JSON number access combined with unbounded array allocation for results allows trivial out-of-memory or timeout exploits for large queries.
**Prevention:** Always use `TryGetNumberField` and immediately `FMath::Clamp` user-provided limits against a sane local maximum (e.g., 1000) before pre-allocating result arrays or initiating resource-heavy loops.
