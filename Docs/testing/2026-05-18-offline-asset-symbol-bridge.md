# Offline Asset/Symbol Bridge Verification

| Field | Value |
| --- | --- |
| Date | 2026-05-18 |
| Scope | `monolith_query.exe context bridge_asset_symbols` |
| Branch | `feat/offline-asset-symbol-bridge` |

---

## 1. Build

| Command | Result |
| --- | --- |
| `cmd /c build.bat` from `Tools/MonolithQuery` | PASS. Built and copied `Plugins/Monolith/Binaries/monolith_query.exe`; MSVC emitted existing CP949 source-encoding warnings only. |

---

## 2. Offline Smoke

Smoke opened `Saved/ProjectIndex.db` and `Saved/EngineSource.db` read-only through the new `context` namespace.

| Command | Expected Contract | Result |
| --- | --- | --- |
| `.\Binaries\monolith_query.exe context bridge_asset_symbols --asset-path=/Game/Maps/Interactable/BP_Wave --db=<Saved> --limit=8` | Asset-seeded bridge returns editor-compatible `links[]` with confidence/reasons | PASS. `status=ok`, `count=8`, `warnings=0`, `truncated=true`; first link `confidence=high`, reason `exact source symbol name match: Wave`, asset `BP_Wave`, symbol `Wave`. |
| `.\Binaries\monolith_query.exe context bridge_asset_symbols --symbol=Wave --db=<Saved> --limit=8` | Symbol-seeded bridge returns project assets with confidence/reasons | PASS. `status=ok`, `count=8`, `warnings=0`, `truncated=true`; first link `confidence=medium`, reason `normalized UE symbol/asset names match`, asset `BP_Wave`, symbol `Wave`. |
| `.\Binaries\monolith_query.exe context bridge_asset_symbols --asset-path=/Game/Maps/Interactable/BP_Wave --db=<Saved> --limit=3 --detail-level=standard` | Standard mode includes extended symbol fields while keeping the same shape | PASS. `status=ok`, `count=3`, `warnings=0`, first link `confidence=high`. |

---

## 3. Notes

- The offline action opens both databases read-only and adds no write path.
- The output shape matches the editor RX-6 contract: `status`, `success`, `read_only`, `lexical_only`, `input`, `limits`, `links[]`, `warnings[]`, `count`, `truncated`, and `next_actions`.
