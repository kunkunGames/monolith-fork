// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 2 — v0.17.0).
//
// FRiskQueryAdapter — registers the `risk_query` namespace against the central
// FMonolithToolRegistry. Pure read-only handlers; no indexing happens here
// (that's FGitCoChangeIndexer / FHotspotScorer / FConditionalGateIndexer).
//
// Action surface (6 actions):
//   risk_query("get_hotspot_score",          {file_path})
//   risk_query("get_cochange_pairs",         {file_path, limit?, cursor?})
//   risk_query("get_file_churn",             {file_path, repo_tag?})
//   risk_query("get_release_window_hotspots",{since_unix?, limit?})
//   risk_query("list_conditional_gates",     {macro_filter?, path_filter?, limit?, cursor?})
//   risk_query("get_mining_status",          {})
//
// The four git-substrate handlers attach a `diagnostics` block when they return
// NOTHING (issue #119): which repositories were mined, which candidates were
// rejected and why, and — only when nothing was mined and something was
// rejected — an actionable hint. `list_conditional_gates` does not, because its
// substrate is the source sweep and a repository hint there would be wrong.
//
// DEVIATION (vs plan §7 handler enumeration): Plan §7 lists 4 handlers
// (HotspotScore / CoChangePairs / FileChurn / ReleaseWindowHotspots). The
// task spec calls for 5 risk_query actions AND a Phase 2 deliverable is the
// `reflect_conditional_gates` table. A 5th action `list_conditional_gates`
// is the natural surface for it — without it, gates are unreachable through
// MCP until Phase 3. The handler is read-only and idempotent like its
// siblings; same dispatcher annotation applies.
//
// v0.17.0 ergonomics adoption (same as decision_query Phase 1):
//   - `file_path` / `path_filter` params tagged EMonolithParamKind::DiskPath.
//   - Dispatcher annotated readOnlyHint=true via SetDispatcherAnnotations.
//   - `get_cochange_pairs` and `list_conditional_gates` adopt cursor pagination
//     (plan §16 mandates pairs paging; pairs scale O(n^2) so the cap matters).

#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class MONOLITHREFLECTIONINTEL_API FRiskQueryAdapter
{
public:
	/** Register all 6 risk_query actions + dispatcher annotations. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// Handlers
	static FMonolithActionResult HandleGetHotspotScore(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetCoChangePairs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetFileChurn(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetReleaseWindowHotspots(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListConditionalGates(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetMiningStatus(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Attach the `diagnostics` object to an otherwise-empty response, so a
	 * caller who gets no rows is told whether any repository was mined at all
	 * instead of having to read source to find out.
	 */
	static void AttachEmptyResultDiagnostics(const TSharedPtr<FJsonObject>& Out);

	/** Shared DB accessor — routes through the module's cached query DB. */
	static class FSQLiteDatabase* GetRawDB();
};
