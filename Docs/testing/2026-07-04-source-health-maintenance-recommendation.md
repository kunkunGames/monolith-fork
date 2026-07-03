# Source Health Maintenance Recommendation

---

## Metadata

| Field | Value |
|---|---|
| Date | 2026-07-04 |
| Area | Monolith Source |
| Change | `source.health` separates required maintenance `next_actions` from optional deep diagnostics |

---

## 1. Purpose

Prevent healthy shallow `source.health` results from driving routine expensive deep-health loops. Both live MCP and offline CLI results expose `maintenance_recommendation` with concrete maintenance booleans and reason codes.

---

## 2. Verification

| Step | Command | Expected |
|---|---|---|
| Offline build | `Plugins\Monolith\Tools\MonolithQuery\build.bat` | Succeeds and refreshes `Binaries\monolith_query.exe` |
| Freshness | `python Plugins\Monolith\Scripts\check_offline_exe_fresh.py` | Reports the offline executable matches source |
| Offline shallow health | `Plugins\Monolith\Binaries\monolith_query.exe source health --no-log` | `status=ok`, `maintenance_recommendation.maintenance_required=false`, and `next_actions` does not include `source.health --include-deep-checks=true` |
| Automation | `Monolith.IndexGuard.Source.HealthHealthy` | Verifies healthy shallow health omits deep health from required next actions and exposes the recommendation object |
| UBT | `UnrealBuildTool.exe SpeedEditor Win64 Development "-Project=Speed.uproject" -WaitMutex -NoHotReloadFromIDE` | Succeeds |

---

## 3. Result

Passed.

| Check | Result |
|---|---|
| Offline build | Passed; source hash `eb2727c5f9929717` |
| Offline freshness | Passed; `RESULT: FRESH` |
| Offline shallow health | Passed; `next_actions=source.search_source,source.review_context,source.risk_score`; optional diagnostics contain `source.health --include-deep-checks=true` |
| Live shallow health | Passed; `structuredContent.maintenance_recommendation.maintenance_required=false`; `next_actions` excludes deep health |
| Automation | Passed; `Saved\Automation\SourceHealthMaintenanceRecommendation_20260704-003457\index.json` reports `succeeded=1`, `failed=0`, `Monolith.IndexGuard.Source.HealthHealthy` |
| UBT | Passed; `Result: Succeeded` |
