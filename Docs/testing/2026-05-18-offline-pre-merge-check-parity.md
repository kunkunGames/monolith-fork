# Offline Pre-Merge Check Parity Verification

| Field | Value |
| --- | --- |
| Date | 2026-05-18 |
| Scope | `monolith_query.exe source pre_merge_check`, `monolith_query.exe project pre_merge_check` |
| Branch | `feat/offline-pre-merge-check-parity` |

---

## 1. Build

| Command | Result |
| --- | --- |
| `cmd /c build.bat` from `Tools/MonolithQuery` | PASS. Built and copied `Plugins/Monolith/Binaries/monolith_query.exe`; MSVC emitted existing CP949 source-encoding warnings only. |

---

## 2. Offline Smoke

| Command | Expected Contract | Result |
| --- | --- | --- |
| `.\Binaries\monolith_query.exe source pre_merge_check Actor.cpp --max-results=5 --unused-limit=2 --detail-level=minimal` | Compact gate with `status`, `decision`, `checks[]`, `findings[]`, counts, and `next_actions[]` | PASS. `status=warning`, `decision=warn`, `changed_entity_count=5`, `test_gap_count=5`, `unused_count=2`, checks=`health,detect_changes,find_unused`. |
| `.\Binaries\monolith_query.exe project pre_merge_check Content/Maps/Interactable/BP_Wave.uasset --max-results=5 --unused-limit=2 --detail-level=minimal` | Compact project gate with changed asset mapping and advisory findings | PASS. `status=warning`, `decision=warn`, `changed_entity_count=1`, `impacted_count=1`, `unused_count=2`, checks=`health,detect_changes,find_unused`. |
| `.\Binaries\monolith_query.exe source pre_merge_check --include-unused=false --detail-level=minimal` | Missing path fails through `detect_changes` without running `find_unused` | PASS. `status=error`, `decision=fail`, `checks=health,detect_changes`, required fields present. |
| `.\Binaries\monolith_query.exe project pre_merge_check Content/Maps/Interactable/BP_Wave.uasset --max-results=5 --unused-limit=2 --detail-level=standard --include-unused=false` | Standard mode embeds `health` and `change_analysis`, omits `unused` when disabled | PASS. `status=warning`, `decision=warn`, `health` and `change_analysis` present, `unused` absent, next actions returned. |

---

## 3. Notes

- The action is read-only: it reuses offline `health`, `detect_changes`, and `find_unused`; no P4/git shell-out and no DB mutation path were added.
- Warning results are expected for the sampled live DBs because health/change/test-gap/unused advisories are surfaced exactly like the editor gate.
