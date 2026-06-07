#pragma once

// ============================================================
// CLI help catalog
// ============================================================

enum class CliOptionKind {
    Flag,
    Value,
    Bool,
};

struct CliActionHelp {
    std::string name;
    std::string group;
    std::string summary;
    std::string syntax;
    std::vector<std::string> positionals;
    std::vector<std::string> options;
    std::string defaults;
    std::string access;
    std::vector<std::string> output_keys;
    std::vector<std::string> examples;
    std::vector<std::string> related;
};

struct CliNamespaceHelp {
    std::string name;
    std::string summary;
    std::string description;
    std::string db_scope;
    std::vector<std::string> start_examples;
    std::vector<CliActionHelp> actions;
};

static std::string normalize_option_key(std::string key) {
    if (key.rfind("--", 0) == 0) key = key.substr(2);
    std::replace(key.begin(), key.end(), '-', '_');
    return lower_copy(key);
}

static bool is_help_token(const std::string& value) {
    return value == "--help" || value == "-h" || lower_copy(value) == "help";
}

static bool is_bool_literal(const std::string& value) {
    std::string lower = lower_copy(value);
    return lower == "true" || lower == "false" ||
           lower == "1" || lower == "0" ||
           lower == "yes" || lower == "no";
}

static bool parse_bool_literal(const std::string& value, bool def) {
    std::string lower = lower_copy(value);
    if (lower == "true" || lower == "1" || lower == "yes") return true;
    if (lower == "false" || lower == "0" || lower == "no") return false;
    return def;
}

static const std::map<std::string, CliOptionKind>& cli_option_kinds() {
    static const std::map<std::string, CliOptionKind> kinds = {
        {"after", CliOptionKind::Value},
        {"asset_path", CliOptionKind::Value},
        {"before", CliOptionKind::Value},
        {"blueprint_callable_only", CliOptionKind::Bool},
        {"blueprint_visible_only", CliOptionKind::Bool},
        {"changed_paths", CliOptionKind::Value},
        {"class_name", CliOptionKind::Value},
        {"context_lines", CliOptionKind::Value},
        {"cursor", CliOptionKind::Value},
        {"db", CliOptionKind::Value},
        {"decision_id", CliOptionKind::Value},
        {"dependency_type", CliOptionKind::Value},
        {"depth", CliOptionKind::Value},
        {"detail_level", CliOptionKind::Value},
        {"diff_file", CliOptionKind::Value},
        {"diff_stdin", CliOptionKind::Bool},
        {"direction", CliOptionKind::Value},
        {"edge_kinds", CliOptionKind::Value},
        {"end", CliOptionKind::Value},
        {"execute", CliOptionKind::Bool},
        {"file_path", CliOptionKind::Value},
        {"force", CliOptionKind::Bool},
        {"graph_db", CliOptionKind::Value},
        {"include_content", CliOptionKind::Bool},
        {"include_counts", CliOptionKind::Bool},
        {"include_deep_checks", CliOptionKind::Bool},
        {"include_questions", CliOptionKind::Bool},
        {"include_unused", CliOptionKind::Bool},
        {"interface_name", CliOptionKind::Value},
        {"kind", CliOptionKind::Value},
        {"label", CliOptionKind::Value},
        {"limit", CliOptionKind::Value},
        {"macro_filter", CliOptionKind::Value},
        {"max_age_days", CliOptionKind::Value},
        {"max_depth", CliOptionKind::Value},
        {"max_lines", CliOptionKind::Value},
        {"max_results", CliOptionKind::Value},
        {"members_only", CliOptionKind::Bool},
        {"min_confidence", CliOptionKind::Value},
        {"min_lines", CliOptionKind::Value},
        {"min_tier", CliOptionKind::Value},
        {"module", CliOptionKind::Value},
        {"module_name", CliOptionKind::Value},
        {"no_header", CliOptionKind::Bool},
        {"offset", CliOptionKind::Value},
        {"path", CliOptionKind::Value},
        {"path_filter", CliOptionKind::Value},
        {"project_db", CliOptionKind::Value},
        {"ranges", CliOptionKind::Value},
        {"ref_kind", CliOptionKind::Value},
        {"repo_tag", CliOptionKind::Value},
        {"rpc_kind", CliOptionKind::Value},
        {"scope", CliOptionKind::Value},
        {"section", CliOptionKind::Value},
        {"seed", CliOptionKind::Value},
        {"since_unix", CliOptionKind::Value},
        {"source_db", CliOptionKind::Value},
        {"specifier_name", CliOptionKind::Value},
        {"start", CliOptionKind::Value},
        {"start_line", CliOptionKind::Value},
        {"end_line", CliOptionKind::Value},
        {"line_count", CliOptionKind::Value},
        {"status", CliOptionKind::Value},
        {"symbol", CliOptionKind::Value},
        {"target", CliOptionKind::Value},
        {"unused_limit", CliOptionKind::Value},
    };
    return kinds;
}

static CliOptionKind cli_option_kind_for(const std::string& key) {
    auto it = cli_option_kinds().find(normalize_option_key(key));
    return it == cli_option_kinds().end() ? CliOptionKind::Flag : it->second;
}

static const std::map<std::string, std::vector<std::string>>& offline_dispatch_action_names() {
    static const std::map<std::string, std::vector<std::string>> names = {
        {"source", {
            "search_source", "read_source", "find_references", "find_callers",
            "find_callees", "get_class_hierarchy", "get_module_info",
            "get_symbol_context", "read_file", "impact_radius", "find_overrides",
            "health", "trigger_reindex", "trigger_project_reindex",
            "repair_fts", "repair_crg_cache", "build_crg_graph",
            "rebuild_crg_graph", "repair_crg_graph", "search_crg_graph",
            "crg_graph_health", "risk_score", "review_hotspots", "review_context",
            "detect_changes", "find_unused", "pre_merge_check", "snapshot",
            "diff_snapshots",
        }},
        {"project", {
            "search", "find_by_type", "find_references", "get_stats",
            "get_asset_details", "impact_radius", "health", "repair_fts",
            "repair_crg_cache", "risk_score", "review_hotspots", "review_context",
            "detect_changes", "find_unused", "pre_merge_check", "snapshot",
            "diff_snapshots",
        }},
        {"bridge", {"search_asset_symbols"}},
        {"monolith", {"guide"}},
        {"cppreflect", {
            "get_uclass", "list_uproperties", "list_ufunctions",
            "find_interface_impls", "find_class_specifier", "list_class_specifiers",
        }},
        {"network", {
            "list_replicated_classes", "list_rpc_functions",
            "list_onrep_handlers", "audit_unbalanced_onreps",
        }},
        {"decision", {
            "list_decisions", "get_decision", "list_stale",
            "find_supersession_chain", "find_referent_decisions",
        }},
        {"risk", {
            "get_hotspot_score", "get_cochange_pairs", "get_file_churn",
            "get_release_window_hotspots", "list_conditional_gates",
        }},
    };
    return names;
}

static const std::vector<CliNamespaceHelp>& cli_help_catalog() {
    static const std::vector<CliNamespaceHelp> catalog = {
        {"source",
         "C++ source, CRG, risk, and code-review queries.",
         "Read and maintain Saved/EngineSource.db plus source CRG graph data. Help is DB-free.",
         "Default DB: Saved/EngineSource.db. CRG graph actions use Saved/graph.db unless --graph-db is supplied.",
         {
             "monolith_query.exe source health --include-counts=false",
             "monolith_query.exe source search_source UObject --limit=5",
             "monolith_query.exe source review_context UObject --detail-level=minimal",
             "monolith_query.exe source impact_radius UObject --max-depth=1 --max-results=20",
             "monolith_query.exe source find_overrides UActorComponent::BeginPlay --direction=in",
         },
         {
             {"search_source", "Discovery", "Search symbol names and indexed source text.",
              "source search_source <query> [--scope=all|cpp|shaders] [--limit=N] [--module=M] [--kind=K]",
              {"query: FTS search text"}, {"--limit N", "--module M", "--kind K", "--scope all|cpp|shaders"},
              "limit=20; scope is accepted for compatibility.", "read-only",
              {"text sections: Symbol Matches, Source Line Matches"},
              {"monolith_query.exe source search_source UObject --limit=5"}, {"source.read_source", "source.review_context"}},
             {"read_source", "Discovery", "Print source for the best matching symbol.",
              "source read_source <symbol> [--max-lines=N] [--no-header] [--members-only] OR source read_source --path=Source/Foo.cpp [--start-line=N] [--line-count=N]",
              {"symbol: class, function, qualified name, or source file path"}, {"--max-lines N", "--no-header", "--members-only", "--path PATH", "--start-line N", "--end-line N", "--line-count N"},
              "max-lines=0 means full indexed range; path input delegates to read_file.", "read-only",
              {"source text"}, {"monolith_query.exe source read_source UObject --max-lines=80"}, {"source.find_references"}},
             {"find_references", "Discovery", "Find indexed references to a source symbol.",
              "source find_references <symbol> [--ref-kind=K] [--limit=N]",
              {"symbol"}, {"--ref-kind K", "--limit N"}, "limit=50.", "read-only",
              {"references"}, {"monolith_query.exe source find_references UObject --limit=20"}, {"source.find_callers", "source.find_callees"}},
             {"find_callers", "Discovery", "Find callers of a function or method.",
              "source find_callers <symbol> [--limit=N]", {"symbol"}, {"--limit N"}, "limit=50.", "read-only",
              {"callers"}, {"monolith_query.exe source find_callers UActorComponent::BeginPlay --limit=20"}, {"source.review_context"}},
             {"find_callees", "Discovery", "Find callees used by a function or method.",
              "source find_callees <symbol> [--limit=N]", {"symbol"}, {"--limit N"}, "limit=50.", "read-only",
              {"callees"}, {"monolith_query.exe source find_callees TickComponent --limit=20"}, {"source.review_context"}},
             {"get_class_hierarchy", "Discovery", "Traverse inheritance around a class or struct.",
              "source get_class_hierarchy <symbol> [--direction=up|down|both] [--depth=N]",
              {"symbol"}, {"--direction up|down|both", "--depth N"}, "direction=both; depth=5.", "read-only",
              {"hierarchy"}, {"monolith_query.exe source get_class_hierarchy UObject --direction=down --depth=2"}, {"source.find_overrides"}},
             {"get_module_info", "Discovery", "Summarize one source module.",
              "source get_module_info <module_name>", {"module_name"}, {}, "no options.", "read-only",
              {"module", "files", "symbols"}, {"monolith_query.exe source get_module_info CoreUObject"}, {"source.search_source"}},
             {"get_symbol_context", "Discovery", "Print nearby context lines for a symbol.",
              "source get_symbol_context <symbol> [--context-lines=N]", {"symbol"}, {"--context-lines N"},
              "context-lines=10.", "read-only", {"context"}, {"monolith_query.exe source get_symbol_context UObject --context-lines=6"}, {"source.read_source"}},
             {"read_file", "Discovery", "Print indexed file lines.",
              "source read_file <file_path> [--start=N] [--end=N] [--line-count=N]", {"file_path"}, {"--start N", "--end N", "--start-line N", "--end-line N", "--line-count N", "--max-lines N", "--path PATH"},
              "start=1; end=0 means 200 lines unless line-count/max-lines is supplied.", "read-only", {"file text"}, {"monolith_query.exe source read_file Source/Foo.cpp --start=1 --end=80"}, {"source.search_source"}},
             {"impact_radius", "Review", "Estimate source blast radius through CRG edges.",
              "source impact_radius <symbol> [--edge-kinds=call|type|inheritance[|override]] [--direction=in|out|both] [--max-depth=N] [--max-results=N]",
              {"symbol"}, {"--edge-kinds K", "--direction in|out|both", "--max-depth N", "--max-results N"},
              "edge-kinds=call|type|inheritance; direction=both; max-depth=2; max-results=200.", "read-only",
              {"success", "input", "nodes", "edges", "count", "truncated"}, {"monolith_query.exe source impact_radius UObject --max-depth 1 --max-results 20"}, {"source.review_context", "source.risk_score"}},
             {"find_overrides", "Review", "Find overrides or overridden functions for a virtual contract.",
              "source find_overrides <symbol> [--direction=in|out|both] [--max-depth=N] [--max-results=N] [--detail-level=minimal|standard]",
              {"symbol, preferably qualified"}, {"--direction in|out|both", "--max-depth N", "--max-results N", "--detail-level minimal|standard"},
              "direction=in; max-depth=2; max-results=200; detail-level=minimal samples edge arrays. Use standard for full edges.", "read-only",
              {"success", "input", "overrides", "edge_count", "truncated"}, {"monolith_query.exe source find_overrides UActorComponent::BeginPlay --direction=in"}, {"source.impact_radius", "source.review_hotspots"}},
             {"health", "Health", "Report EngineSource.db health with optional deep parity checks and counts.",
             "source health [--include-counts=true|false] [--include-deep-checks=true|false]", {}, {"--include-counts bool", "--include-deep-checks bool"}, "Default is a fast shallow check; include-counts=true also enables deep checks.", "read-only",
              {"status", "success", "checks", "counts"}, {"monolith_query.exe source health --include-counts=false"}, {"source.repair_fts"}},
             {"trigger_reindex", "Live-only", "Explain how to run the live full source reindex action.",
              "source trigger_reindex", {}, {}, "Offline guidance only; requires live MCP/editor to execute.", "read-only guidance",
              {"status", "offline_supported", "next_actions"}, {"monolith_query.exe source trigger_reindex"}, {"source.health"}},
             {"trigger_project_reindex", "Live-only", "Explain how to run the live incremental project source reindex action.",
              "source trigger_project_reindex", {}, {}, "Offline guidance only; requires live MCP/editor to execute.", "read-only guidance",
              {"status", "offline_supported", "next_actions"}, {"monolith_query.exe source trigger_project_reindex"}, {"source.health"}},
             {"repair_fts", "Maintenance", "Inspect or repair source FTS tables.",
              "source repair_fts [--target=all|symbols|source] [--execute]", {}, {"--target NAME", "--execute bool"},
              "target=all; dry-run unless --execute=true.", "execute-gated write",
              {"status", "success", "target", "execute", "actions"}, {"monolith_query.exe source repair_fts --target=all", "monolith_query.exe source repair_fts --target=all --execute"}, {"source.health"}},
             {"repair_crg_cache", "Maintenance", "Inspect or repair source CRG projection/cache parity.",
              "source repair_crg_cache [--scope=all|override_edges] [--execute]", {}, {"--scope NAME", "--execute bool"},
              "scope=all; dry-run unless --execute=true.", "execute-gated write",
              {"status", "success", "scope", "execute", "actions"}, {"monolith_query.exe source repair_crg_cache --scope=all"}, {"source.health"}},
             {"build_crg_graph", "Maintenance", "Build Saved/graph.db for CRG graph search.",
              "source build_crg_graph [--execute] [--force] [--graph-db=PATH]", {}, {"--execute bool", "--force bool", "--graph-db PATH"},
              "dry-run unless --execute=true; graph-db defaults to Saved/graph.db.", "execute-gated write",
              {"status", "success", "graph_db", "job_id"}, {"monolith_query.exe source build_crg_graph --execute"}, {"source.crg_graph_health", "source.search_crg_graph"}},
             {"rebuild_crg_graph", "Maintenance", "Alias-compatible CRG graph rebuild action.",
              "source rebuild_crg_graph [--execute] [--force] [--graph-db=PATH]", {}, {"--execute bool", "--force bool", "--graph-db PATH"},
              "dry-run unless --execute=true.", "execute-gated write",
              {"status", "success", "graph_db"}, {"monolith_query.exe source rebuild_crg_graph --execute --force"}, {"source.crg_graph_health"}},
             {"repair_crg_graph", "Maintenance", "Compatibility alias for rebuild_crg_graph.",
              "source repair_crg_graph [--execute] [--force] [--graph-db=PATH]", {}, {"--execute bool", "--force bool", "--graph-db PATH"},
              "alias: dispatches to rebuild_crg_graph.", "execute-gated write",
              {"status", "success", "graph_db"}, {"monolith_query.exe source repair_crg_graph --execute"}, {"source.rebuild_crg_graph"}},
             {"search_crg_graph", "CRG", "Search graph nodes in Saved/graph.db.",
              "source search_crg_graph <query> [--kind=K] [--limit=N] [--graph-db=PATH]", {"query"}, {"--kind K", "--limit N", "--graph-db PATH"},
              "limit=20.", "read-only", {"status", "success", "nodes", "count"}, {"monolith_query.exe source search_crg_graph UObject --limit=10"}, {"source.build_crg_graph"}},
             {"crg_graph_health", "CRG", "Report graph.db freshness and row counts.",
              "source crg_graph_health [--graph-db=PATH]", {}, {"--graph-db PATH"}, "graph-db defaults to Saved/graph.db.", "read-only",
              {"status", "success", "graph_db", "counts"}, {"monolith_query.exe source crg_graph_health"}, {"source.build_crg_graph"}},
             {"risk_score", "Review", "Score source symbols by fan-in, fan-out, size, and risk cues.",
              "source risk_score <symbol> [--limit=N] [--min-tier=low|medium|high]", {"symbol"}, {"--limit N", "--min-tier low|medium|high"},
              "limit=10; min-tier=low.", "read-only", {"success", "symbol", "items", "count"}, {"monolith_query.exe source risk_score UObject --limit=5"}, {"source.review_context"}},
             {"review_hotspots", "Review", "Find high-value review targets.",
              "source review_hotspots [--kind=fan_in|fan_out|risk|large|override|all] [--limit=N] [--min-lines=N] [--include-questions=false]",
              {}, {"--kind K", "--limit N", "--min-lines N", "--include-questions bool"},
              "kind=all; limit=50; min-lines=100; include-questions=true.", "read-only",
              {"success", "items", "count", "truncated"}, {"monolith_query.exe source review_hotspots --kind=override --limit=10"}, {"source.find_overrides"}},
             {"review_context", "Review", "Collect compact source review context for a symbol.",
              "source review_context <symbol> [--direction=in|out|both] [--detail-level=minimal|standard] [--max-depth=N] [--max-results=N]",
              {"symbol"}, {"--direction in|out|both", "--detail-level minimal|standard", "--max-depth N", "--max-results N"},
              "direction=both; detail-level=minimal; max-depth=2; max-results=200.", "read-only",
              {"success", "input", "symbol", "risk", "impact", "next_actions"}, {"monolith_query.exe source review_context UObject --detail-level=minimal"}, {"source.impact_radius", "source.read_source"}},
             {"detect_changes", "Review", "Map changed source paths to review context.",
              "source detect_changes <path...> [--changed-paths=a,b] [--ranges=path:s-e,...] [--diff-file=PATH] [--diff-stdin] [--max-results=N] [--detail-level=minimal|standard]",
              {"path..."}, {"--changed-paths CSV", "--ranges SPEC", "--diff-file PATH", "--diff-stdin bool", "--max-results N", "--detail-level minimal|standard"},
              "max-results=200; detail-level=minimal.", "read-only",
              {"success", "changes", "items", "next_actions"}, {"monolith_query.exe source detect_changes Source/Foo.cpp --max-results 20"}, {"source.pre_merge_check"}},
             {"find_unused", "Review", "Find potentially unused source symbols.",
              "source find_unused [--kind=function|class|struct|all] [--limit=N] [--min-confidence=low|medium|high]",
              {}, {"--kind K", "--limit N", "--min-confidence low|medium|high"},
              "kind=all; limit=100; min-confidence=low.", "read-only",
              {"success", "items", "count"}, {"monolith_query.exe source find_unused --kind=function --limit=20"}, {"source.pre_merge_check"}},
             {"pre_merge_check", "Review", "Run a compact source pre-merge review bundle.",
              "source pre_merge_check <path...> [--changed-paths=a,b] [--max-results=N] [--unused-limit=N] [--detail-level=minimal|standard] [--include-unused=false]",
              {"path..."}, {"--changed-paths CSV", "--max-results N", "--unused-limit N", "--detail-level minimal|standard", "--include-unused bool"},
              "max-results=200; unused-limit=20; detail-level=minimal; include-unused=true.", "read-only",
              {"success", "changes", "unused", "next_actions"}, {"monolith_query.exe source pre_merge_check Source/Foo.cpp --include-unused false"}, {"source.detect_changes"}},
             {"snapshot", "Maintenance", "Create or preview a source CRG snapshot.",
              "source snapshot [label] [--label=name] [--execute]", {"label optional"}, {"--label NAME", "--execute bool"},
              "dry-run unless --execute=true.", "execute-gated write",
              {"status", "success", "snapshot"}, {"monolith_query.exe source snapshot pre-change --execute"}, {"source.diff_snapshots"}},
             {"diff_snapshots", "Review", "Diff two source CRG snapshots.",
              "source diff_snapshots <before> [after] [--before=label-or-id] [--after=label-or-id|current] [--limit=N]",
              {"before", "after optional"}, {"--before REF", "--after REF", "--limit N"}, "limit=100; after=current when omitted.", "read-only",
              {"status", "success", "changes", "count"}, {"monolith_query.exe source diff_snapshots pre-change current --limit=50"}, {"source.snapshot"}},
         }},
        {"project",
         "Asset, Blueprint, actor, DataTable, and project-index queries.",
         "Read and maintain Saved/ProjectIndex.db. Help is DB-free.",
         "Default DB: Saved/ProjectIndex.db.",
         {
             "monolith_query.exe project health --include-counts=false",
             "monolith_query.exe project search Health --limit=10 --include-content=true",
             "monolith_query.exe project review_context /Game/UI/WBP_HUD --detail-level=minimal",
             "monolith_query.exe project pre_merge_check /Game/UI/WBP_HUD --include-unused=false",
         },
         {
             {"search", "Discovery", "Search project asset identity and indexed content.",
              "project search <query> [--limit=N] [--include-content=true|false]", {"query"}, {"--limit N", "--include-content bool"},
              "limit=50; include-content=true.", "read-only",
              {"success", "results", "match_source", "match_table", "match_field"}, {"monolith_query.exe project search Health --limit=10 --include-content=true"}, {"project.get_asset_details"}},
             {"find_by_type", "Discovery", "Find assets by asset_class.",
              "project find_by_type <asset_class> [--limit=N] [--offset=N]", {"asset_class"}, {"--limit N", "--offset N"},
              "limit=50; offset=0.", "read-only", {"success", "results", "count"}, {"monolith_query.exe project find_by_type WidgetBlueprint --limit=10"}, {"project.search"}},
             {"find_references", "Discovery", "Find project references for an asset path.",
              "project find_references <asset_path>", {"asset_path"}, {}, "no options.", "read-only",
              {"success", "references", "count"}, {"monolith_query.exe project find_references /Game/UI/WBP_HUD"}, {"project.impact_radius"}},
             {"get_stats", "Health", "Summarize project index tables.",
              "project get_stats", {}, {}, "no options.", "read-only",
              {"success", "stats"}, {"monolith_query.exe project get_stats"}, {"project.health"}},
             {"get_asset_details", "Discovery", "Read one asset detail bundle.",
              "project get_asset_details <asset_path>", {"asset_path"}, {}, "no options.", "read-only",
              {"success", "asset", "nodes", "variables"}, {"monolith_query.exe project get_asset_details /Game/UI/WBP_HUD"}, {"bridge.search_asset_symbols"}},
             {"impact_radius", "Review", "Trace project dependency radius around an asset.",
              "project impact_radius <asset_path> [--direction=in|out|both] [--max-depth=N] [--max-results=N] [--dependency-type=Hard|Soft]",
              {"asset_path"}, {"--direction in|out|both", "--max-depth N", "--max-results N", "--dependency-type Hard|Soft"},
              "direction=both; max-depth=2; max-results=200.", "read-only",
              {"success", "input", "assets", "edges", "count"}, {"monolith_query.exe project impact_radius /Game/UI/WBP_HUD --max-depth 1"}, {"project.review_context"}},
             {"health", "Health", "Report ProjectIndex.db health and optional counts.",
              "project health [--include-counts=true|false]", {}, {"--include-counts bool"}, "include-counts=true.", "read-only",
              {"status", "success", "checks", "counts"}, {"monolith_query.exe project health --include-counts=false"}, {"project.repair_fts"}},
             {"repair_fts", "Maintenance", "Inspect or repair project FTS tables.",
              "project repair_fts [--target=all|assets|nodes|variables|parameters|datatable_rows|actors|asset_search_values] [--execute]",
              {}, {"--target NAME", "--execute bool"}, "target=all; dry-run unless --execute=true.", "execute-gated write",
              {"status", "success", "target", "execute", "actions"}, {"monolith_query.exe project repair_fts --target=all"}, {"project.health"}},
             {"repair_crg_cache", "Maintenance", "Inspect or repair project CRG cache parity.",
              "project repair_crg_cache [--scope=all] [--execute]", {}, {"--scope NAME", "--execute bool"},
              "scope=all; dry-run unless --execute=true.", "execute-gated write",
              {"status", "success", "scope", "execute"}, {"monolith_query.exe project repair_crg_cache --scope=all"}, {"project.health"}},
             {"risk_score", "Review", "Score project assets by dependency and size cues.",
              "project risk_score [asset_path] [--limit=N] [--min-tier=low|medium|high]", {"asset_path optional"}, {"--asset-path PATH", "--seed PATH", "--limit N", "--min-tier low|medium|high"},
              "limit=20; min-tier=low.", "read-only", {"success", "items", "count"}, {"monolith_query.exe project risk_score /Game/UI/WBP_HUD --limit=5"}, {"project.review_context"}},
             {"review_hotspots", "Review", "Find high-value project review targets.",
              "project review_hotspots [--kind=fan_in|fan_out|risk|large|all] [--limit=N] [--min-lines=N] [--include-questions=false]",
              {}, {"--kind K", "--limit N", "--min-lines N", "--include-questions bool"},
              "kind=all; limit=50; min-lines=100; include-questions=true.", "read-only",
              {"success", "items", "count"}, {"monolith_query.exe project review_hotspots --kind=risk --limit=10"}, {"project.review_context"}},
             {"review_context", "Review", "Collect compact review context for one asset.",
              "project review_context <asset_path> [--direction=in|out|both] [--detail-level=minimal|standard] [--max-depth=N] [--max-results=N]",
              {"asset_path"}, {"--direction in|out|both", "--detail-level minimal|standard", "--max-depth N", "--max-results N"},
              "direction=both; detail-level=minimal; max-depth=2; max-results=200.", "read-only",
              {"success", "asset", "risk", "impact", "next_actions"}, {"monolith_query.exe project review_context /Game/UI/WBP_HUD --detail-level=minimal"}, {"project.impact_radius", "bridge.search_asset_symbols"}},
             {"detect_changes", "Review", "Map changed project asset paths to review context.",
              "project detect_changes <path...> [--changed-paths=a,b] [--ranges=path:s-e,...] [--diff-file=PATH] [--diff-stdin] [--max-results=N] [--detail-level=minimal|standard]",
              {"path..."}, {"--changed-paths CSV", "--ranges SPEC", "--diff-file PATH", "--diff-stdin bool", "--max-results N", "--detail-level minimal|standard"},
              "max-results=200; detail-level=minimal.", "read-only", {"success", "changes", "items"}, {"monolith_query.exe project detect_changes /Game/UI/WBP_HUD"}, {"project.pre_merge_check"}},
             {"find_unused", "Review", "Find potentially unused project assets.",
              "project find_unused [--kind=<asset_class>] [--limit=N] [--min-confidence=low|medium|high]",
              {}, {"--kind CLASS", "--limit N", "--min-confidence low|medium|high"},
              "kind=all; limit=100; min-confidence=low.", "read-only", {"success", "items", "count"}, {"monolith_query.exe project find_unused --kind=WidgetBlueprint --limit=20"}, {"project.pre_merge_check"}},
             {"pre_merge_check", "Review", "Run a compact project pre-merge review bundle.",
              "project pre_merge_check <path...> [--changed-paths=a,b] [--max-results=N] [--unused-limit=N] [--detail-level=minimal|standard] [--include-unused=false]",
              {"path..."}, {"--changed-paths CSV", "--max-results N", "--unused-limit N", "--detail-level minimal|standard", "--include-unused bool"},
              "max-results=200; unused-limit=20; detail-level=minimal; include-unused=true.", "read-only",
              {"success", "changes", "unused", "next_actions"}, {"monolith_query.exe project pre_merge_check /Game/UI/WBP_HUD --include-unused false"}, {"project.detect_changes"}},
             {"snapshot", "Maintenance", "Create or preview a project snapshot.",
              "project snapshot [label] [--label=name] [--execute]", {"label optional"}, {"--label NAME", "--execute bool"},
              "dry-run unless --execute=true.", "execute-gated write", {"status", "success", "snapshot"}, {"monolith_query.exe project snapshot pre-change --execute"}, {"project.diff_snapshots"}},
             {"diff_snapshots", "Review", "Diff two project snapshots.",
              "project diff_snapshots <before> [after] [--before=label-or-id] [--after=label-or-id|current] [--limit=N]",
              {"before", "after optional"}, {"--before REF", "--after REF", "--limit N"}, "limit=100; after=current when omitted.", "read-only",
              {"status", "success", "changes", "count"}, {"monolith_query.exe project diff_snapshots pre-change current --limit=50"}, {"project.snapshot"}},
         }},
        {"bridge",
         "Heuristic links between indexed assets and C++ source symbols.",
         "Reads Saved/ProjectIndex.db and Saved/EngineSource.db. Help is DB-free.",
         "Default DBs: Saved/ProjectIndex.db and Saved/EngineSource.db.",
         {
             "monolith_query.exe bridge search_asset_symbols --asset-path=/Game/Maps/Interactable/BP_Wave --limit=5",
             "monolith_query.exe bridge search_asset_symbols --symbol=UObject --limit=5",
         },
         {
             {"search_asset_symbols", "Bridge", "Search asset-to-symbol or symbol-to-asset links.",
              "bridge search_asset_symbols [--asset-path=/Game/...] [--symbol=Name] [--limit=N] [--detail-level=minimal|standard]",
              {"one of --asset-path or --symbol"}, {"--asset-path PATH", "--symbol NAME", "--limit N", "--detail-level minimal|standard"},
              "limit=20; detail-level=minimal.", "read-only",
              {"status", "success", "links", "warnings", "count", "truncated", "lexical_only"}, {"monolith_query.exe bridge search_asset_symbols --symbol=UObject --limit=5"}, {"source.review_context", "project.review_context"}},
         }},
        {"monolith",
         "Offline guide material bundled with the plugin.",
         "Prints Docs/MONOLITH_GUIDE.md sections without editor access.",
         "No SQLite DB required.",
         {
             "monolith_query.exe monolith guide",
             "monolith_query.exe monolith guide --section=recipes",
         },
         {
             {"guide", "Guide", "Print the bundled Monolith guide or a named section.",
              "monolith guide [--section=onboarding|recipes|decisions|errors|skills_map|gotchas]",
              {}, {"--section NAME"}, "prints all sections when section is omitted.", "read-only",
              {"markdown text"}, {"monolith_query.exe monolith guide --section=recipes"}, {}},
         }},
        {"cppreflect",
         "Reflection-intelligence C++ UCLASS/UPROPERTY/UFUNCTION queries.",
         "Reads reflect_* tables from Saved/EngineSource.db. Help is DB-free.",
         "Default DB: Saved/EngineSource.db.",
         {
             "monolith_query.exe cppreflect get_uclass UObject",
             "monolith_query.exe cppreflect list_ufunctions UObject --blueprint-callable-only --limit=20",
         },
         {
             {"get_uclass", "Reflection", "Read one UCLASS reflection bundle.", "cppreflect get_uclass <class_name> [--module_name=M]", {"class_name"}, {"--class-name C", "--module M", "--module-name M"}, "not paginated.", "read-only", {"success", "uclass"}, {"monolith_query.exe cppreflect get_uclass UObject"}, {"source.review_context"}},
             {"list_uproperties", "Reflection", "List UPROPERTY rows.", "cppreflect list_uproperties [class_name] [--class_name=C] [--blueprint_visible_only] [--limit=N] [--cursor=B64]", {"class_name optional"}, {"--class-name C", "--blueprint-visible-only bool", "--limit N", "--cursor B64"}, "limit=50.", "read-only", {"success", "uproperties", "total_estimate", "next_cursor"}, {"monolith_query.exe cppreflect list_uproperties UObject --limit=20"}, {"cppreflect.get_uclass"}},
             {"list_ufunctions", "Reflection", "List UFUNCTION rows.", "cppreflect list_ufunctions [class_name] [--class_name=C] [--blueprint_callable_only] [--limit=N] [--cursor=B64]", {"class_name optional"}, {"--class-name C", "--blueprint-callable-only bool", "--limit N", "--cursor B64"}, "limit=50.", "read-only", {"success", "ufunctions", "total_estimate", "next_cursor"}, {"monolith_query.exe cppreflect list_ufunctions UObject --blueprint-callable-only --limit=20"}, {"cppreflect.get_uclass"}},
             {"find_interface_impls", "Reflection", "Find classes implementing an interface.", "cppreflect find_interface_impls <interface_name>", {"interface_name"}, {"--interface-name NAME"}, "not paginated.", "read-only", {"success", "implementations"}, {"monolith_query.exe cppreflect find_interface_impls UInterface"}, {"source.search_source"}},
             {"find_class_specifier", "Reflection", "Find classes with a specifier.", "cppreflect find_class_specifier <specifier_name> [--limit=N] [--cursor=B64]", {"specifier_name"}, {"--specifier-name NAME", "--limit N", "--cursor B64"}, "limit=50.", "read-only", {"success", "classes", "next_cursor"}, {"monolith_query.exe cppreflect find_class_specifier Blueprintable --limit=20"}, {"cppreflect.list_class_specifiers"}},
             {"list_class_specifiers", "Reflection", "List known UCLASS specifiers.", "cppreflect list_class_specifiers", {}, {}, "no options.", "read-only", {"success", "specifiers"}, {"monolith_query.exe cppreflect list_class_specifiers"}, {"cppreflect.find_class_specifier"}},
         }},
        {"network",
         "Reflection-intelligence replication and RPC queries.",
         "Reads reflect network tables from Saved/EngineSource.db. Help is DB-free.",
         "Default DB: Saved/EngineSource.db.",
         {
             "monolith_query.exe network list_replicated_classes --limit=20",
             "monolith_query.exe network list_rpc_functions --rpc-kind=Server --limit=20",
         },
         {
             {"list_replicated_classes", "Network", "List replicated classes.", "network list_replicated_classes [--limit=N] [--cursor=B64]", {}, {"--limit N", "--cursor B64"}, "limit=50.", "read-only", {"success", "classes", "next_cursor"}, {"monolith_query.exe network list_replicated_classes --limit=20"}, {"network.list_onrep_handlers"}},
             {"list_rpc_functions", "Network", "List RPC UFUNCTIONs.", "network list_rpc_functions [--class_name=C] [--rpc_kind=Server|Client|Multicast] [--limit=N] [--cursor=B64]", {}, {"--class-name C", "--rpc-kind K", "--limit N", "--cursor B64"}, "limit=50.", "read-only", {"success", "functions", "next_cursor"}, {"monolith_query.exe network list_rpc_functions --rpc-kind=Server --limit=20"}, {"source.review_context"}},
             {"list_onrep_handlers", "Network", "List OnRep handlers.", "network list_onrep_handlers [--class_name=C] [--limit=N] [--cursor=B64]", {}, {"--class-name C", "--limit N", "--cursor B64"}, "limit=50.", "read-only", {"success", "handlers", "next_cursor"}, {"monolith_query.exe network list_onrep_handlers --class-name AMyActor"}, {"network.audit_unbalanced_onreps"}},
             {"audit_unbalanced_onreps", "Network", "Find replicated fields without balanced OnRep coverage.", "network audit_unbalanced_onreps [--limit=N] [--cursor=B64]", {}, {"--limit N", "--cursor B64"}, "limit=50.", "read-only", {"success", "findings", "next_cursor"}, {"monolith_query.exe network audit_unbalanced_onreps --limit=20"}, {"source.review_context"}},
         }},
        {"decision",
         "Reflection-intelligence architecture decision queries.",
         "Reads decision_* tables from Saved/EngineSource.db. Help is DB-free.",
         "Default DB: Saved/EngineSource.db.",
         {
             "monolith_query.exe decision list_decisions --limit=20",
             "monolith_query.exe decision list_stale 30 --path-filter=Source",
         },
         {
             {"list_decisions", "Decision", "List extracted decisions.", "decision list_decisions [--path_filter=P] [--min_confidence=N] [--status=S] [--limit=N] [--cursor=B64]", {}, {"--path-filter P", "--min-confidence N", "--status S", "--limit N", "--cursor B64"}, "min-confidence=0.6; limit=50.", "read-only", {"success", "decisions", "total_estimate", "next_cursor"}, {"monolith_query.exe decision list_decisions --limit=20"}, {"decision.get_decision"}},
             {"get_decision", "Decision", "Read one decision by id.", "decision get_decision <decision_id>", {"decision_id"}, {"--decision-id ID"}, "not paginated.", "read-only", {"success", "decision"}, {"monolith_query.exe decision get_decision DEC-001"}, {"decision.find_supersession_chain"}},
             {"list_stale", "Decision", "List stale decisions older than a day window.", "decision list_stale <max_age_days> [--path_filter=P] [--limit=N] [--cursor=B64]", {"max_age_days"}, {"--max-age-days N", "--path-filter P", "--limit N", "--cursor B64"}, "max_age_days must be positive; limit=50.", "read-only", {"success", "stale_decisions", "cutoff_unix", "next_cursor"}, {"monolith_query.exe decision list_stale 30 --limit=20"}, {"decision.get_decision"}},
             {"find_supersession_chain", "Decision", "Traverse decision supersession chain.", "decision find_supersession_chain <decision_id> [--depth=N]", {"decision_id"}, {"--decision-id ID", "--depth N"}, "depth=10, clamped 1..50.", "read-only", {"success", "start", "chain", "truncated"}, {"monolith_query.exe decision find_supersession_chain DEC-001 --depth=5"}, {"decision.find_referent_decisions"}},
             {"find_referent_decisions", "Decision", "Find decisions superseded by a decision id.", "decision find_referent_decisions <decision_id>", {"decision_id"}, {"--decision-id ID"}, "not paginated.", "read-only", {"success", "decision_id", "referent_decisions"}, {"monolith_query.exe decision find_referent_decisions DEC-001"}, {"decision.get_decision"}},
         }},
        {"risk",
         "Reflection-intelligence file risk and co-change queries.",
         "Reads risk_* tables from Saved/EngineSource.db. Help is DB-free.",
         "Default DB: Saved/EngineSource.db.",
         {
             "monolith_query.exe risk get_hotspot_score Source/Foo.cpp",
             "monolith_query.exe risk list_conditional_gates --macro-filter=WITH_EDITOR --limit=20",
         },
         {
             {"get_hotspot_score", "Risk", "Read one file hotspot score.", "risk get_hotspot_score <file_path>", {"file_path"}, {"--file-path PATH"}, "not paginated.", "read-only", {"success", "hotspot"}, {"monolith_query.exe risk get_hotspot_score Source/Foo.cpp"}, {"source.review_context"}},
             {"get_cochange_pairs", "Risk", "List files that co-change with a file.", "risk get_cochange_pairs <file_path> [--limit=N] [--cursor=B64]", {"file_path"}, {"--file-path PATH", "--limit N", "--cursor B64"}, "limit=50.", "read-only", {"success", "pairs", "next_cursor"}, {"monolith_query.exe risk get_cochange_pairs Source/Foo.cpp --limit=20"}, {"risk.get_hotspot_score"}},
             {"get_file_churn", "Risk", "Read churn data for a file.", "risk get_file_churn <file_path> [--repo_tag=R]", {"file_path"}, {"--file-path PATH", "--repo-tag R"}, "repo-tag optional.", "read-only", {"success", "churn"}, {"monolith_query.exe risk get_file_churn Source/Foo.cpp"}, {"risk.get_hotspot_score"}},
             {"get_release_window_hotspots", "Risk", "List recent release-window hotspots.", "risk get_release_window_hotspots [--since_unix=N] [--limit=N] [--cursor=B64]", {}, {"--since-unix N", "--limit N", "--cursor B64"}, "since_unix defaults to recent window; limit=50.", "read-only", {"success", "hotspots", "since_unix", "next_cursor"}, {"monolith_query.exe risk get_release_window_hotspots --limit=20"}, {"source.review_hotspots"}},
             {"list_conditional_gates", "Risk", "List macro/conditional gates.", "risk list_conditional_gates [--macro_filter=M] [--path_filter=P] [--limit=N] [--cursor=B64]", {}, {"--macro-filter M", "--path-filter P", "--limit N", "--cursor B64"}, "limit=50.", "read-only", {"success", "gates", "next_cursor"}, {"monolith_query.exe risk list_conditional_gates --macro-filter=WITH_EDITOR --limit=20"}, {"source.review_context"}},
         }},
    };
    return catalog;
}

static const CliNamespaceHelp* find_namespace_help(const std::string& ns) {
    for (const auto& item : cli_help_catalog()) {
        if (item.name == ns) return &item;
    }
    return nullptr;
}

static const CliActionHelp* find_action_help(const CliNamespaceHelp& ns, const std::string& action) {
    for (const auto& item : ns.actions) {
        if (item.name == action) return &item;
    }
    return nullptr;
}

static std::vector<std::string> catalog_namespace_names() {
    std::vector<std::string> names;
    for (const auto& item : cli_help_catalog()) names.push_back(item.name);
    return names;
}

static std::vector<std::string> catalog_action_names(const CliNamespaceHelp& ns) {
    std::vector<std::string> names;
    for (const auto& item : ns.actions) names.push_back(item.name);
    return names;
}

static std::string join_strings(const std::vector<std::string>& values, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += sep;
        out += values[i];
    }
    return out;
}

static void print_string_list(std::ostream& out, const std::vector<std::string>& values, const std::string& prefix = "  - ") {
    if (values.empty()) {
        out << prefix << "(none)\n";
        return;
    }
    for (const auto& value : values) out << prefix << value << "\n";
}

static std::map<std::string, std::vector<std::string>> validate_help_catalog_errors() {
    std::map<std::string, std::vector<std::string>> errors;
    std::set<std::string> seen_ns;
    for (const auto& ns : cli_help_catalog()) {
        if (!seen_ns.insert(ns.name).second)
            errors[ns.name].push_back("duplicate namespace descriptor");

        std::set<std::string> catalog_actions;
        for (const auto& action : ns.actions) {
            if (!catalog_actions.insert(action.name).second)
                errors[ns.name].push_back("duplicate action descriptor: " + action.name);
        }

        auto it = offline_dispatch_action_names().find(ns.name);
        if (it == offline_dispatch_action_names().end()) {
            errors[ns.name].push_back("namespace missing from offline dispatch names");
            continue;
        }
        std::set<std::string> dispatch_actions(it->second.begin(), it->second.end());
        for (const auto& action : catalog_actions) {
            if (!dispatch_actions.count(action))
                errors[ns.name].push_back("catalog-only action: " + action);
        }
        for (const auto& action : dispatch_actions) {
            if (!catalog_actions.count(action))
                errors[ns.name].push_back("dispatch-only action: " + action);
        }
    }

    std::set<std::string> catalog_names;
    for (const auto& ns : cli_help_catalog()) catalog_names.insert(ns.name);
    for (const auto& pair : offline_dispatch_action_names()) {
        if (!catalog_names.count(pair.first))
            errors[pair.first].push_back("dispatch namespace missing from catalog");
    }
    return errors;
}

static bool help_catalog_is_valid() {
    return validate_help_catalog_errors().empty();
}

static void print_catalog_self_check(std::ostream& out) {
    auto errors = validate_help_catalog_errors();
    json root = {
        {"success", errors.empty()},
        {"namespaces", catalog_namespace_names()},
        {"errors", json::object()},
    };
    for (const auto& pair : errors) {
        root["errors"][pair.first] = pair.second;
    }
    out << root.dump(2) << std::endl;
}

static void print_top_level_help(std::ostream& out) {
    out << "Usage:\n"
        << "  monolith_query.exe --help\n"
        << "  monolith_query.exe <namespace> --help\n"
        << "  monolith_query.exe <namespace> <action> --help\n"
        << "  monolith_query.exe <namespace> <action> [params...] [--options]\n\n";

    out << "Offline namespaces:\n";
    for (const auto& ns : cli_help_catalog())
        out << "  " << ns.name << " - " << ns.summary << "\n";

    out << "\nDB rules:\n"
        << "  Default DBs resolve from the executable location.\n"
        << "  source and RI namespaces read Saved/EngineSource.db.\n"
        << "  project reads Saved/ProjectIndex.db.\n"
        << "  bridge reads both ProjectIndex.db and EngineSource.db.\n"
        << "  source CRG graph actions use Saved/graph.db.\n"
        << "  Overrides: --db PATH, --source-db PATH, --project-db PATH, --graph-db PATH.\n\n";

    out << "Query logging and trace environment:\n"
        << "  MONOLITH_TOOL_LOG_ENABLED=0 disables query logs.\n"
        << "  MONOLITH_TOOL_LOG_DIR redirects Logs/yyyyMMdd/query.jsonl.\n"
        << "  MONOLITH_TOOL_LOG_MAX_FIELD_BYTES bounds captured stdout/stderr fields.\n"
        << "  MONOLITH_TRACE_ID and MONOLITH_PARENT_SPAN_ID attach trace context.\n"
        << "  MONOLITH_VERSION is reported in log environment metadata when set.\n\n";

    out << "High-ROI examples:\n"
        << "  monolith_query.exe source health --include-counts=false\n"
        << "  monolith_query.exe source search_source UObject --limit=5\n"
        << "  monolith_query.exe source review_context UObject --detail-level=minimal\n"
        << "  monolith_query.exe project search Health --limit=10 --include-content=true\n"
        << "  monolith_query.exe bridge search_asset_symbols --symbol=UObject --limit=5\n"
        << "  monolith_query.exe source crg_graph_health\n\n";

    out << "Help forms:\n"
        << "  monolith_query.exe help source\n"
        << "  monolith_query.exe source help impact_radius\n"
        << "  monolith_query.exe help bridge search_asset_symbols\n\n";

    out << "Only offline SQLite-backed namespaces are listed here. Live editor-only namespaces require MCP/editor access.\n";
}

static void print_namespace_help(std::ostream& out, const CliNamespaceHelp& ns) {
    out << "Usage:\n"
        << "  monolith_query.exe " << ns.name << " --help\n"
        << "  monolith_query.exe " << ns.name << " <action> --help\n"
        << "  monolith_query.exe " << ns.name << " <action> [params...] [--options]\n\n";
    out << ns.name << " - " << ns.summary << "\n"
        << ns.description << "\n"
        << "DB scope: " << ns.db_scope << "\n\n";

    out << "Start here:\n";
    print_string_list(out, ns.start_examples, "  ");

    out << "\nActions:\n";
    std::string current_group;
    for (const auto& action : ns.actions) {
        if (action.group != current_group) {
            current_group = action.group;
            out << "\n" << current_group << ":\n";
        }
        out << "  " << action.name << " - " << action.summary << "\n";
    }

    out << "\nMaintenance warning:\n"
        << "  repair_fts, repair_crg_cache, build_crg_graph, snapshot, and aliases are dry-run or read-only unless --execute=true is supplied.\n";
}

static void print_action_help(std::ostream& out, const CliNamespaceHelp& ns, const CliActionHelp& action) {
    out << "Usage:\n"
        << "  monolith_query.exe " << action.syntax << "\n\n";
    out << ns.name << "." << action.name << " - " << action.summary << "\n"
        << "Workflow: " << action.group << "\n"
        << "DB scope: " << ns.db_scope << "\n"
        << "Access: " << action.access << "\n\n";

    out << "Positionals:\n";
    print_string_list(out, action.positionals);

    out << "\nOptions:\n";
    print_string_list(out, action.options);

    out << "\nDefaults:\n"
        << "  " << action.defaults << "\n\n";

    out << "Primary output keys:\n";
    print_string_list(out, action.output_keys);

    out << "\nExamples:\n";
    print_string_list(out, action.examples, "  ");

    if (!action.related.empty()) {
        out << "\nRelated follow-up commands:\n";
        print_string_list(out, action.related);
    }
}

static void print_unknown_namespace_error(std::ostream& err, const std::string& ns) {
    const auto names = catalog_namespace_names();
    err << "ERROR: Unknown namespace: " << ns << "\n";
    auto sugg = fuzzy_top(ns, names, 3);
    if (!sugg.empty())
        err << "Did you mean: " << join_strings(sugg, ", ") << "?\n";
    err << "Offline namespaces: " << join_strings(names, ", ") << "\n";
    err << "Run: monolith_query.exe --help\n";
}

static void print_unknown_action_error(std::ostream& err, const CliNamespaceHelp& ns, const std::string& action) {
    const auto names = catalog_action_names(ns);
    err << "ERROR: Unknown action: " << ns.name << "." << action << "\n";
    auto sugg = fuzzy_top(action, names, 3);
    if (!sugg.empty())
        err << "Did you mean: " << join_strings(sugg, ", ") << "?\n";
    err << "Run: monolith_query.exe " << ns.name << " --help\n";
}

static int print_namespace_or_action_help(const std::string& ns_name, const std::string& action_name) {
    const CliNamespaceHelp* ns = find_namespace_help(ns_name);
    if (!ns) {
        print_unknown_namespace_error(std::cerr, ns_name);
        return 2;
    }
    if (action_name.empty()) {
        print_namespace_help(std::cout, *ns);
        return 0;
    }
    const CliActionHelp* action = find_action_help(*ns, action_name);
    if (!action) {
        print_unknown_action_error(std::cerr, *ns, action_name);
        return 2;
    }
    print_action_help(std::cout, *ns, *action);
    return 0;
}

static int maybe_handle_help_or_catalog_check(int argc, char* argv[], bool& handled) {
    handled = false;
    if (argc == 1) {
        handled = true;
        print_top_level_help(std::cout);
        return 0;
    }

    if (argc >= 2 && std::string(argv[1]) == "--catalog-self-check") {
        handled = true;
        print_catalog_self_check(std::cout);
        return help_catalog_is_valid() ? 0 : 3;
    }

    if (argc == 2 && is_help_token(argv[1])) {
        handled = true;
        print_top_level_help(std::cout);
        return 0;
    }

    if (argc >= 2 && lower_copy(std::string(argv[1])) == "help") {
        handled = true;
        if (argc == 2) {
            print_top_level_help(std::cout);
            return 0;
        }
        if (argc == 3) return print_namespace_or_action_help(argv[2], "");
        return print_namespace_or_action_help(argv[2], argv[3]);
    }

    if (argc >= 3) {
        std::string ns = argv[1];
        std::string second = argv[2];
        if (is_help_token(second)) {
            handled = true;
            return print_namespace_or_action_help(ns, "");
        }
        if (lower_copy(second) == "help") {
            handled = true;
            if (argc >= 4) return print_namespace_or_action_help(ns, argv[3]);
            return print_namespace_or_action_help(ns, "");
        }
        for (int i = 3; i < argc; ++i) {
            if (is_help_token(argv[i])) {
                handled = true;
                return print_namespace_or_action_help(ns, second);
            }
        }
    }

    handled = false;
    return 0;
}

static void validate_help_catalog_or_die() {
    auto errors = validate_help_catalog_errors();
    if (errors.empty()) return;

    std::ostringstream msg;
    msg << "Help catalog/dispatch drift detected:";
    for (const auto& pair : errors) {
        for (const auto& item : pair.second)
            msg << " [" << pair.first << "] " << item << ";";
    }
    die(msg.str());
}

static void validate_known_command_or_die(const std::string& ns_name, const std::string& action_name) {
    const CliNamespaceHelp* ns = find_namespace_help(ns_name);
    if (!ns) {
        print_unknown_namespace_error(std::cerr, ns_name);
        throw QueryFatal("", 2, true);
    }
    if (!find_action_help(*ns, action_name)) {
        print_unknown_action_error(std::cerr, *ns, action_name);
        throw QueryFatal("", 2, true);
    }
}
