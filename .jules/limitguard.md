## 2025-02-18 - Bound MonolithAudioQueryActions find_unattenuated_sounds results array
**Boundary:** `limit` param and `ResultsArray.Num()` on `find_unattenuated_sounds` action
**Learning:** `FindUnattenuatedSounds` traverses potentially tens of thousands of assets via the asset registry. An unbounded list risks returning a huge JSON array, which could consume significant memory and crash the VM/MCP if a project has huge numbers of missing attenuations.
**Prevention:** Bound queries iterating `AssetDataList` and returning large result sets with an optional `limit` parameter, defaulting to a conservative max like 100.
[blocked: UE 5.7 editor unavailable in Jules VM]
