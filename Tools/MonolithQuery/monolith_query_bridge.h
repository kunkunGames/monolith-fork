#pragma once

// Bridge asset/source symbol helpers and actions.

// ============================================================
// Bridge utilities
// ============================================================

static bool iequals(const std::string& a, const std::string& b) {
    return lower_copy(a) == lower_copy(b);
}

static bool bridge_starts_with_any_prefix(const std::string& value,
                                          const std::vector<std::string>& prefixes,
                                          std::string& out_trimmed) {
    std::string lower = lower_copy(value);
    for (const auto& prefix : prefixes) {
        std::string lower_prefix = lower_copy(prefix);
        if (lower.rfind(lower_prefix, 0) == 0 && value.size() > prefix.size()) {
            out_trimmed = value.substr(prefix.size());
            return true;
        }
    }
    return false;
}

static std::string bridge_clean_token(std::string value) {
    value = trim_copy(value);
    std::replace(value.begin(), value.end(), '\\', '/');
    for (char sep : {'/', '.', ':'}) {
        size_t idx = value.find_last_of(sep);
        if (idx != std::string::npos && idx + 1 < value.size())
            value = value.substr(idx + 1);
    }
    if (value.size() > 2 && iequals(value.substr(value.size() - 2), "_C"))
        value.resize(value.size() - 2);
    return value;
}

static bool bridge_is_generic_asset_class(const std::string& value) {
    static const std::set<std::string> generic = {
        "blueprint", "widgetblueprint", "animblueprint", "dataasset",
        "primarydataasset", "material", "materialinstanceconstant",
        "texture2d", "staticmesh", "skeletalmesh"
    };
    return generic.count(lower_copy(value)) > 0;
}

static std::string bridge_normalize_name(const std::string& value) {
    std::string normalized = bridge_clean_token(value);
    static const std::vector<std::string> prefixes = {
        "BP_", "B_", "WBP_", "ABP_", "DA_", "PDA_", "GA_", "GE_", "GC_", "GCN_"
    };
    bool trimmed = true;
    while (trimmed) {
        std::string next;
        trimmed = bridge_starts_with_any_prefix(normalized, prefixes, next);
        if (trimmed) normalized = next;
    }
    if (normalized.size() > 2) {
        char first = normalized[0];
        char second = normalized[1];
        if ((first == 'U' || first == 'A' || first == 'F' || first == 'I') &&
            std::isupper(static_cast<unsigned char>(second))) {
            normalized = normalized.substr(1);
        }
    }
    return normalized;
}

static void bridge_add_unique(std::vector<std::string>& out,
                              std::set<std::string>& seen,
                              const std::string& candidate) {
    std::string cleaned = trim_copy(bridge_clean_token(candidate));
    if (cleaned.empty()) return;
    std::string key = lower_copy(cleaned);
    if (seen.insert(key).second) out.push_back(cleaned);
}

static std::vector<std::string> bridge_asset_symbol_candidates(const std::string& asset_path,
                                                               const std::string& asset_name,
                                                               const std::string& asset_class) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;
    std::string clean_asset_name = bridge_clean_token(asset_name.empty() ? asset_path : asset_name);
    std::string clean_path_name = bridge_clean_token(asset_path);
    bridge_add_unique(candidates, seen, clean_asset_name);
    bridge_add_unique(candidates, seen, clean_path_name);

    std::string normalized = bridge_normalize_name(clean_asset_name);
    bridge_add_unique(candidates, seen, normalized);
    if (!normalized.empty()) {
        bridge_add_unique(candidates, seen, "U" + normalized);
        bridge_add_unique(candidates, seen, "A" + normalized);
        bridge_add_unique(candidates, seen, "F" + normalized);
    }

    std::string clean_class = bridge_clean_token(asset_class);
    if (!clean_class.empty() && !bridge_is_generic_asset_class(clean_class)) {
        bridge_add_unique(candidates, seen, clean_class);
        bridge_add_unique(candidates, seen, bridge_normalize_name(clean_class));
    }
    return candidates;
}

static std::vector<std::string> bridge_symbol_asset_candidates(const std::string& symbol_name,
                                                               const std::string& qualified_name) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;
    std::string clean_symbol = bridge_clean_token(symbol_name.empty() ? qualified_name : symbol_name);
    std::string clean_qualified = bridge_clean_token(qualified_name);
    bridge_add_unique(candidates, seen, clean_symbol);
    bridge_add_unique(candidates, seen, clean_qualified);

    std::string normalized = bridge_normalize_name(clean_symbol);
    bridge_add_unique(candidates, seen, normalized);
    if (!normalized.empty()) {
        bridge_add_unique(candidates, seen, "BP_" + normalized);
        bridge_add_unique(candidates, seen, "WBP_" + normalized);
        bridge_add_unique(candidates, seen, "DA_" + normalized);
        bridge_add_unique(candidates, seen, "GA_" + normalized);
        bridge_add_unique(candidates, seen, "GE_" + normalized);
    }
    return candidates;
}

static bool bridge_names_match_normalized(const std::string& left, const std::string& right) {
    return iequals(bridge_normalize_name(left), bridge_normalize_name(right));
}

static std::string bridge_source_file_path(Database& db, int file_id) {
    auto rows = query(db, "SELECT path FROM files WHERE id = ?", {std::to_string(file_id)});
    return rows.empty() ? "" : rows[0].get("path");
}

static json bridge_asset_object(const Row& asset,
                                const std::string& fallback_path = "",
                                const std::string& fallback_name = "",
                                const std::string& fallback_class = "") {
    return {
        {"asset_path", asset.cols.empty() ? fallback_path : asset.get("package_path")},
        {"asset_name", asset.cols.empty() ? fallback_name : asset.get("asset_name")},
        {"asset_class", asset.cols.empty() ? fallback_class : asset.get("asset_class")},
        {"module_name", asset.cols.empty() ? "" : asset.get("module_name")},
    };
}

static json bridge_symbol_object(Database& db, const Row& symbol, bool standard) {
    json result = {
        {"id", symbol.get_int64("id")},
        {"name", symbol.get("name")},
        {"qualified_name", symbol.get("qualified_name")},
        {"kind", symbol.get("kind")},
        {"line_start", symbol.get_int("line_start")},
        {"line_end", symbol.get_int("line_end")},
        {"path", short_path(bridge_source_file_path(db, symbol.get_int("file_id")))},
    };
    if (standard) {
        result["signature"] = symbol.get("signature");
        result["docstring"] = symbol.get("docstring");
        result["access"] = symbol.get("access");
        result["is_ue_macro"] = symbol.get_int("is_ue_macro") != 0;
    }
    return result;
}

static std::string bridge_confidence(double score) {
    if (score >= 0.80) return "high";
    if (score >= 0.58) return "medium";
    return "low";
}

struct BridgeLinkCandidate {
    json object;
    double score = 0.0;
    std::string key;
};

static void bridge_add_link(std::vector<BridgeLinkCandidate>& links,
                            std::set<std::string>& seen,
                            const json& asset,
                            const json& symbol,
                            const std::string& direction,
                            double score,
                            const std::vector<std::string>& reasons) {
    std::string asset_path = json_string_field(asset, "asset_path", "");
    std::string symbol_name = json_string_field(symbol, "qualified_name",
        json_string_field(symbol, "name", ""));
    std::string key = lower_copy(direction + "|" + asset_path + "|" + symbol_name);

    json link = {
        {"direction", direction},
        {"confidence", bridge_confidence(score)},
        {"score", score},
        {"reasons", reasons},
        {"asset", asset},
        {"symbol", symbol},
    };
    if (!seen.insert(key).second) {
        auto existing = std::find_if(links.begin(), links.end(),
            [&](const BridgeLinkCandidate& candidate) { return candidate.key == key; });
        if (existing != links.end() && score > existing->score) {
            existing->object = link;
            existing->score = score;
        }
        return;
    }
    links.push_back({link, score, key});
}

static Rows bridge_source_symbols_exact(Database& db, const std::string& name, int limit) {
    return query(db,
        "SELECT id, name, qualified_name, kind, file_id, line_start, line_end, "
        "parent_symbol_id, access, signature, docstring, is_ue_macro "
        "FROM symbols WHERE lower(name) = lower(?) "
        "ORDER BY (line_end > line_start) DESC LIMIT " + std::to_string(limit),
        {name});
}

static Rows bridge_source_symbols_fts(Database& db, const std::string& name, int limit) {
    return query(db,
        "SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, "
        "s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro "
        "FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
        "WHERE symbols_fts MATCH ? ORDER BY bm25(symbols_fts) LIMIT " + std::to_string(limit),
        {escape_fts(name)});
}

static Row bridge_asset_by_path(Database& db, const std::string& path) {
    auto rows = query(db,
        "SELECT id, package_path, asset_name, asset_class, module_name "
        "FROM assets WHERE package_path = ? LIMIT 1",
        {path});
    return rows.empty() ? Row{} : rows[0];
}

static Rows bridge_project_assets_search(Database& db, const std::string& candidate, int limit) {
    Rows exact = query(db,
        "SELECT id, package_path, asset_name, asset_class, module_name "
        "FROM assets WHERE lower(asset_name) = lower(?) LIMIT " + std::to_string(limit),
        {candidate});
    Rows fts = query(db,
        "SELECT a.id, a.package_path, a.asset_name, a.asset_class, a.module_name "
        "FROM fts_assets f JOIN assets a ON a.id = f.rowid "
        "WHERE fts_assets MATCH ? ORDER BY rank LIMIT " + std::to_string(limit),
        {escape_fts(candidate)});

    Rows rows;
    std::set<std::string> seen;
    auto add_unique = [&](const Row& row) {
        if ((int)rows.size() >= limit) return;
        std::string key = row.get("id");
        if (key.empty()) key = lower_copy(row.get("package_path"));
        if (key.empty() || !seen.insert(key).second) return;
        rows.push_back(row);
    };
    for (const auto& row : exact) add_unique(row);
    for (const auto& row : fts) add_unique(row);
    return rows;
}

static void bridge_add_source_matches_for_candidate(Database& source_db,
                                                    const json& asset,
                                                    const std::string& asset_name,
                                                    const std::string& candidate,
                                                    int limit,
                                                    bool standard,
                                                    std::vector<BridgeLinkCandidate>& links,
                                                    std::set<std::string>& seen) {
    if (candidate.empty()) return;
    for (const auto& symbol : bridge_source_symbols_exact(source_db, candidate, limit)) {
        std::vector<std::string> reasons = {"exact source symbol name match: " + candidate};
        double score = 0.90;
        if (bridge_names_match_normalized(asset_name, symbol.get("name"))) {
            reasons.push_back("normalized UE asset/symbol names match");
            score = 0.95;
        }
        bridge_add_link(links, seen, asset, bridge_symbol_object(source_db, symbol, standard),
                        "asset_to_symbol", score, reasons);
    }
    for (const auto& symbol : bridge_source_symbols_fts(source_db, candidate, limit)) {
        std::vector<std::string> reasons;
        double score = 0.48;
        if (iequals(symbol.get("name"), candidate)) {
            reasons.push_back("FTS result has exact symbol name: " + candidate);
            score = 0.82;
        } else if (bridge_names_match_normalized(asset_name, symbol.get("name"))) {
            reasons.push_back("FTS result normalized name matches the asset");
            score = 0.72;
        } else {
            reasons.push_back("source FTS fallback matched candidate: " + candidate);
        }
        bridge_add_link(links, seen, asset, bridge_symbol_object(source_db, symbol, standard),
                        "asset_to_symbol", score, reasons);
    }
}

static void bridge_add_asset_matches_for_symbol(Database& project_db,
                                                Database& source_db,
                                                const Row& symbol,
                                                const std::string& candidate,
                                                int limit,
                                                bool standard,
                                                std::vector<BridgeLinkCandidate>& links,
                                                std::set<std::string>& seen) {
    if (candidate.empty()) return;
    for (const auto& asset : bridge_project_assets_search(project_db, candidate, limit)) {
        std::vector<std::string> reasons;
        double score = 0.45;
        if (iequals(asset.get("asset_name"), candidate)) {
            reasons.push_back("exact project asset name match: " + candidate);
            score = 0.88;
        } else if (bridge_names_match_normalized(asset.get("asset_name"), symbol.get("name"))) {
            reasons.push_back("normalized UE symbol/asset names match");
            score = 0.74;
        } else {
            reasons.push_back("project index FTS fallback matched candidate: " + candidate);
        }
        bridge_add_link(links, seen, bridge_asset_object(asset),
                        bridge_symbol_object(source_db, symbol, standard),
                        "symbol_to_asset", score, reasons);
    }
}

// ============================================================
// Source actions
// ============================================================

// ============================================================
// Bridge actions
// ============================================================

class BridgeActions {
    Database project_db;
    Database source_db;
    bool project_open = false;
    bool source_open = false;
    json open_warnings = json::array();

public:
    void open(const std::string& project_path, const std::string& source_path) {
        if (fs::exists(project_path)) {
            project_db.open(project_path, true);
            project_open = true;
        } else {
            open_warnings.push_back("project index database unavailable: " + project_path);
        }
        if (fs::exists(source_path)) {
            source_db.open(source_path, true);
            source_open = true;
        } else {
            open_warnings.push_back("source database unavailable: " + source_path);
        }
    }

    void search_asset_symbols(const Args& args) {
        std::string asset_path = args.opt("asset_path");
        std::string symbol_seed = args.opt("symbol");
        if (asset_path.empty() && args.positional.size() == 1 && symbol_seed.empty())
            asset_path = args.positional[0];
        asset_path = trim_copy(asset_path);
        symbol_seed = trim_copy(symbol_seed);

        bool has_asset = !asset_path.empty();
        bool has_symbol = !symbol_seed.empty();
        if (has_asset == has_symbol)
            die("search_asset_symbols requires exactly one of --asset-path or --symbol");

        int limit = clamp_int(args.opt_int("limit", 20), 1, 100);
        std::string detail_level = args.opt("detail_level", "minimal");
        if (detail_level != "minimal" && detail_level != "standard")
            die("detail_level must be 'minimal' or 'standard'");
        bool standard = detail_level == "standard";

        json warnings = open_warnings;
        std::vector<BridgeLinkCandidate> candidates;
        std::set<std::string> seen_links;
        std::vector<std::string> candidate_names;
        std::set<std::string> seen_candidate_names;

        auto remember_candidate = [&](const std::string& value) {
            std::string cleaned = trim_copy(value);
            if (cleaned.empty()) return;
            std::string key = lower_copy(cleaned);
            if (seen_candidate_names.insert(key).second) candidate_names.push_back(cleaned);
        };

        if (has_asset) {
            Row asset;
            if (project_open) {
                asset = bridge_asset_by_path(project_db, asset_path);
                if (asset.cols.empty())
                    warnings.push_back("asset '" + asset_path + "' was not found in the project index");
            }
            std::string asset_name = asset.cols.empty() ? bridge_clean_token(asset_path) : asset.get("asset_name");
            std::string asset_class = asset.cols.empty() ? "" : asset.get("asset_class");
            json asset_object = bridge_asset_object(asset, asset_path, asset_name, asset_class);
            for (const auto& candidate : bridge_asset_symbol_candidates(asset_path, asset_name, asset_class)) {
                remember_candidate(candidate);
                if (source_open)
                    bridge_add_source_matches_for_candidate(source_db, asset_object, asset_name, candidate,
                                                            limit, standard, candidates, seen_links);
            }
            if (!source_open)
                warnings.push_back("source database unavailable. Provide --source-db or run source.trigger_project_reindex first.");
        } else {
            std::vector<Row> symbols;
            if (source_open) {
                symbols = bridge_source_symbols_exact(source_db, symbol_seed, limit);
                if (symbols.empty())
                    symbols = bridge_source_symbols_fts(source_db, symbol_seed, limit);
                if (symbols.empty())
                    warnings.push_back("symbol '" + symbol_seed + "' was not found in the source index");
            } else {
                warnings.push_back("source database unavailable. Provide --source-db or run source.trigger_project_reindex first.");
            }

            if (!project_open)
                warnings.push_back("project index database unavailable. Provide --project-db or run project indexing first.");

            for (const auto& symbol : symbols) {
                for (const auto& candidate : bridge_symbol_asset_candidates(symbol.get("name"), symbol.get("qualified_name"))) {
                    remember_candidate(candidate);
                    if (project_open)
                        bridge_add_asset_matches_for_symbol(project_db, source_db, symbol, candidate,
                                                            limit, standard, candidates, seen_links);
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const BridgeLinkCandidate& a, const BridgeLinkCandidate& b) {
                      return a.score > b.score;
                  });
        bool truncated = (int)candidates.size() > limit;
        if (truncated) candidates.resize(limit);

        json links = json::array();
        for (const auto& candidate : candidates)
            links.push_back(candidate.object);

        json input = {
            {"mode", has_asset ? "asset" : "symbol"},
            {"candidate_names", candidate_names},
        };
        if (has_asset) input["asset_path"] = asset_path;
        else input["symbol"] = symbol_seed;

        json next_actions = json::array({
            "Use bridge.build_attachment on matching asset/source_symbol items for prompt materialization.",
            "Use source.review_context or project.review_context on high-confidence matches before code review.",
        });
        if (!warnings.empty())
            next_actions.push_back("Run bridge.get_index_status or bridge.start_indexing if an index is unavailable or stale.");

        json root = {
            {"status", warnings.empty() ? "ok" : "warning"},
            {"success", true},
            {"read_only", true},
            {"lexical_only", true},
            {"input", input},
            {"limits", {{"limit", limit}, {"detail_level", detail_level}}},
            {"links", links},
            {"warnings", warnings},
            {"count", links.size()},
            {"truncated", truncated},
            {"next_actions", next_actions},
        };
        print_json(root);
    }
};
